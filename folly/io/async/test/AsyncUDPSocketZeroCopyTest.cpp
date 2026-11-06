/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <chrono>
#include <limits>
#include <vector>

#include <folly/ScopeGuard.h>
#include <folly/SocketAddress.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/AsyncUDPSocket.h>
#include <folly/io/async/EventBase.h>
#include <folly/portability/GTest.h>
#include <folly/portability/Sockets.h>

#if defined(FOLLY_HAVE_MSG_ERRQUEUE)
#include <linux/errqueue.h>
#endif

using folly::AsyncUDPSocket;
using folly::EventBase;
using folly::IOBuf;
using folly::SocketAddress;

namespace {

#if defined(FOLLY_HAVE_MSG_ERRQUEUE)

// Drains one batch of cmsgs from the socket's error queue (non-blocking)
// and returns true iff at least one cmsg is a SO_EE_ORIGIN_ZEROCOPY
// completion. The kernel only emits these in response to sendmsg calls
// that were issued with the MSG_ZEROCOPY flag, so this is an end-to-end
// check that MSG_ZEROCOPY actually reached the kernel.
bool tryReadZeroCopyCmsg(int fd) {
  uint8_t ctrl[1024];
  unsigned char data;
  struct iovec iov{};
  iov.iov_base = &data;
  iov.iov_len = sizeof(data);
  struct msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = ctrl;
  msg.msg_controllen = sizeof(ctrl);

  ssize_t ret = ::recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
  if (ret < 0) {
    return false;
  }
  for (auto* cm = CMSG_FIRSTHDR(&msg); cm != nullptr;
       cm = CMSG_NXTHDR(&msg, cm)) {
    const bool isErr =
        (cm->cmsg_level == SOL_IPV6 && cm->cmsg_type == IPV6_RECVERR) ||
        (cm->cmsg_level == SOL_IP && cm->cmsg_type == IP_RECVERR);
    if (!isErr) {
      continue;
    }
    auto* serr =
        reinterpret_cast<const struct sock_extended_err*>(CMSG_DATA(cm));
    if (serr->ee_origin == SO_EE_ORIGIN_ZEROCOPY) {
      return true;
    }
  }
  return false;
}

// Waits up to `timeout` for a ZC completion cmsg. Uses poll(POLLERR) so
// we block in the kernel until the err queue actually has data, rather
// than busy-sleeping. MSG_ZEROCOPY completions arrive asynchronously
// after the kernel has finished with the buffer; on loopback this is
// typically sub-millisecond but we allow generous slack for slow CI
// hosts.
bool waitForZeroCopyCmsg(int fd, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (tryReadZeroCopyCmsg(fd)) {
      return true;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      break;
    }
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLERR;
    ::poll(&pfd, 1, static_cast<int>(remaining.count()));
  }
  return tryReadZeroCopyCmsg(fd);
}

// Test fixture: a sender bound to loopback with SO_ZEROCOPY enabled, plus
// a receiver socket so the kernel has somewhere to deliver the packet.
// SetUp returns early (sets `supported_` = false) if the running kernel
// or socket family rejects SO_ZEROCOPY, so the tests gracefully skip on
// platforms without ZC support.
class AsyncUDPSocketZeroCopyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    sender_ = std::make_unique<AsyncUDPSocket>(&evb_);
    receiver_ = std::make_unique<AsyncUDPSocket>(&evb_);
    sender_->bind(SocketAddress("127.0.0.1", 0));
    receiver_->bind(SocketAddress("127.0.0.1", 0));
    receiverAddr_ = receiver_->address();
    supported_ = sender_->setZeroCopy(true);
  }

  EventBase evb_;
  std::unique_ptr<AsyncUDPSocket> sender_;
  std::unique_ptr<AsyncUDPSocket> receiver_;
  SocketAddress receiverAddr_;
  bool supported_{false};
};

// Verifies that writev (which routes through writevImpl) actually passes
// MSG_ZEROCOPY to the kernel when WriteOptions::zerocopy=true. Regression
// test for a bug where writevImpl silently dropped the flag, so callers
// using writeGSO/writev with zerocopy=true ended up doing plain sends.
//
// NOTE: this test runs over loopback, where the kernel cannot perform
// real zerocopy (no DMA to defer the page-pinning to) and ALWAYS falls
// back to a synchronous copy, returning SO_EE_CODE_ZEROCOPY_COPIED set
// in the completion cmsg. That fallback is fine for this test's purpose:
// the kernel still emits the SO_EE_ORIGIN_ZEROCOPY cmsg only in response
// to a sendmsg that included MSG_ZEROCOPY, so the cmsg's presence (not
// its `copied` bit) is what proves the flag reached the kernel. We
// intentionally don't assert on `copied` so a future kernel that ever
// supports real loopback ZC doesn't break this test.
TEST_F(AsyncUDPSocketZeroCopyTest, Writev_ZerocopyTrue_ProducesZeroCopyCmsg) {
  if (!supported_) {
    GTEST_SKIP() << "SO_ZEROCOPY not supported on this kernel";
  }
  std::vector<uint8_t> payload(16 * 1024, 'Z');
  struct iovec vec{};
  vec.iov_base = payload.data();
  vec.iov_len = payload.size();

  const auto ret = sender_->writev(
      receiverAddr_,
      &vec,
      1,
      AsyncUDPSocket::WriteOptions(0 /*gso*/, true /*zerocopy*/));
  ASSERT_GT(ret, 0) << "writev failed: errno=" << errno;

  // Read the datagram back out of the receiver before waiting for the
  // completion. On kernels that perform real (non-copy) loopback zerocopy,
  // the sender-side completion is generated only once the skb is released,
  // which never happens while the packet sits in an unread receiver queue;
  // the copy-fallback path completes synchronously, so this is a no-op
  // there.
  const int rfd = receiver_->getNetworkSocket().toFd();
  struct pollfd rpfd{};
  rpfd.fd = rfd;
  rpfd.events = POLLIN;
  ASSERT_GT(::poll(&rpfd, 1, 5000), 0)
      << "packet never arrived: errno=" << errno;
  char rbuf[65536];
  ASSERT_GT(::recv(rfd, rbuf, sizeof(rbuf), MSG_DONTWAIT), 0)
      << "receiver read failed: errno=" << errno;

  EXPECT_TRUE(waitForZeroCopyCmsg(
      sender_->getNetworkSocket().toFd(), std::chrono::milliseconds(5000)));
}

// Negative control: with zerocopy=false the kernel must NOT emit a ZC
// completion cmsg. Catches the inverse failure where writevImpl might
// over-eagerly set MSG_ZEROCOPY regardless of the WriteOptions bit.
TEST_F(AsyncUDPSocketZeroCopyTest, Writev_ZerocopyFalse_ProducesNoCmsg) {
  if (!supported_) {
    GTEST_SKIP() << "SO_ZEROCOPY not supported on this kernel";
  }
  std::vector<uint8_t> payload(16 * 1024, 'P');
  struct iovec vec{};
  vec.iov_base = payload.data();
  vec.iov_len = payload.size();

  const auto ret = sender_->writev(
      receiverAddr_,
      &vec,
      1,
      AsyncUDPSocket::WriteOptions(0 /*gso*/, false /*zerocopy*/));
  ASSERT_GT(ret, 0) << "writev failed: errno=" << errno;

  // Wait the same amount of time and confirm nothing arrives. If we ever
  // start seeing flakes here it would indicate the kernel coalesced a
  // late completion from a previous test or that MSG_ZEROCOPY leaked
  // into the flags.
  EXPECT_FALSE(waitForZeroCopyCmsg(
      sender_->getNetworkSocket().toFd(), std::chrono::milliseconds(100)));
}

// Records every IOBuf it receives so the test can assert which bufs the
// bookkeeping handed back, in what order, and how many times. If an
// EventBase is supplied, terminates the loop on the first release so
// shared-fd tests can wait via evb.loopForever() rather than polling.
class RecordingReleaseCb : public AsyncUDPSocket::ReleaseIOBufCallback {
 public:
  RecordingReleaseCb() = default;
  explicit RecordingReleaseCb(EventBase* terminateOnFire)
      : terminateOnFire_(terminateOnFire) {}
  void releaseIOBuf(std::unique_ptr<folly::IOBuf> buf) noexcept override {
    bufs.push_back(std::move(buf));
    if (terminateOnFire_) {
      terminateOnFire_->terminateLoopSoon();
    }
  }
  std::vector<std::unique_ptr<folly::IOBuf>> bufs;

 private:
  EventBase* terminateOnFire_{nullptr};
};

// Stashes the IOBuf the test wants the writer to retain a reference to,
// so a single IOBuf identity round-trips from `writeChain` to
// `releaseIOBuf`. The other AsyncUDPSocket::WriteCallback methods aren't
// invoked on the UDP zerocopy path so they're left unimplemented.
class RecordingWriteCb : public AsyncUDPSocket::WriteCallback {
 public:
  explicit RecordingWriteCb(AsyncUDPSocket::ReleaseIOBufCallback* cb)
      : releaseCb_(cb) {}
  AsyncUDPSocket::ReleaseIOBufCallback*
  getReleaseIOBufCallback() noexcept override {
    return releaseCb_;
  }

 private:
  AsyncUDPSocket::ReleaseIOBufCallback* releaseCb_;
};

// The shared-fd test needs a ReadCallback on the listener so resumeRead
// is satisfied; the actual recv path is not exercised here, only the
// POLLERR drain that piggybacks on the same EventHandler registration.
class NoopReadCallback : public AsyncUDPSocket::ReadCallback {
 public:
  void getReadBuffer(void**, size_t*) noexcept override {}
  void onDataAvailable(
      const folly::SocketAddress&, size_t, bool, OnDataAvailableParams) noexcept
      override {}
  void onReadError(const folly::AsyncSocketException&) noexcept override {}
  void onReadClosed() noexcept override {}
};

// End-to-end check that createPeerOnSameFd correctly wires up shared
// bookkeeping: a writer created from the listener via the factory
// registers an IOBuf for a MSG_ZEROCOPY send, and when the kernel posts
// the completion, the listener's POLLERR drain invokes the writer's
// release callback with the original IOBuf. This is the canonical
// SO_REUSEPORT-worker pattern (e.g. mvfst's QuicServerWorker).
TEST_F(
    AsyncUDPSocketZeroCopyTest,
    CreatePeerOnSameFd_ReleaseCallbackFiresViaListener) {
  if (!supported_) {
    GTEST_SKIP() << "SO_ZEROCOPY not supported on this kernel";
  }
  auto writer = sender_->createPeerOnSameFd();
  if (writer == nullptr) {
    FAIL() << "createPeerOnSameFd returned null";
  }
  auto& writerRef = *writer;

  // Listener owns the read side and the POLLERR drain. SCOPE_EXIT
  // guarantees the stack-local readCb is unregistered on every exit
  // path, including ASSERT failures — otherwise sender_ would outlive
  // readCb and hold a dangling pointer until fixture teardown.
  NoopReadCallback readCb;
  sender_->resumeRead(&readCb);
  SCOPE_EXIT {
    sender_->pauseRead();
  };

  RecordingReleaseCb releaseCb(&evb_);
  RecordingWriteCb writeCb(&releaseCb);

  const std::vector<uint8_t> payloadData(16 * 1024, 'A');
  auto payload = IOBuf::copyBuffer(payloadData.data(), payloadData.size());
  auto* rawBuf = payload.get();

  const auto ret = writerRef.writeChain(
      &writeCb,
      receiverAddr_,
      std::move(payload),
      AsyncUDPSocket::WriteOptions(0 /*gso*/, true /*zerocopy*/));
  ASSERT_GT(ret, 0) << "writeChain failed: errno=" << errno;

  // Safety deadline: if the release never fires, this terminates the
  // loop so the test fails fast rather than hanging.
  evb_.tryRunAfterDelay([this]() { evb_.terminateLoopSoon(); }, 500 /*ms*/);
  evb_.loopForever();

  ASSERT_EQ(releaseCb.bufs.size(), 1)
      << "release callback did not fire via listener drain";
  EXPECT_EQ(releaseCb.bufs[0].get(), rawBuf);
}

// Exercises the MSG_ZEROCOPY completion counters and kill-switch state.
// On loopback the kernel cannot do real ZC and always responds with
// SO_EE_CODE_ZEROCOPY_COPIED set, so a single writeChain produces a
// single cmsg over range [N, N] that lands in the "Copied" buckets and
// latches zeroCopyEnabled to false. Without these counters, that latch
// is invisible to callers and silent ZC fallbacks look like successful
// ZC sends.
TEST_F(
    AsyncUDPSocketZeroCopyTest,
    CountersAndKillSwitchOnLoopbackCopiedCompletion) {
  if (!supported_) {
    GTEST_SKIP() << "SO_ZEROCOPY not supported on this kernel";
  }
  // Pre-state: nothing has happened yet.
  EXPECT_EQ(sender_->getZeroCopyCompletionsZc(), 0u);
  EXPECT_EQ(sender_->getZeroCopyCompletionsCopied(), 0u);
  EXPECT_EQ(sender_->getZeroCopySendsAckedZc(), 0u);
  EXPECT_EQ(sender_->getZeroCopySendsAckedMaybeCopied(), 0u);
  EXPECT_TRUE(sender_->getZeroCopy());

  // POLLERR drain is what fires processZeroCopyMsg, so we need a read
  // registration to wake the EventBase on err-queue activity.
  NoopReadCallback readCb;
  sender_->resumeRead(&readCb);
  SCOPE_EXIT {
    sender_->pauseRead();
  };

  RecordingReleaseCb releaseCb(&evb_);
  RecordingWriteCb writeCb(&releaseCb);

  const std::vector<uint8_t> payloadData(16 * 1024, 'C');
  auto payload = IOBuf::copyBuffer(payloadData.data(), payloadData.size());
  const auto ret = sender_->writeChain(
      &writeCb,
      receiverAddr_,
      std::move(payload),
      AsyncUDPSocket::WriteOptions(0 /*gso*/, true /*zerocopy*/));
  ASSERT_GT(ret, 0) << "writeChain failed: errno=" << errno;

  // Safety deadline so the loop terminates even if no completion fires.
  evb_.tryRunAfterDelay([this]() { evb_.terminateLoopSoon(); }, 500);
  evb_.loopForever();
  ASSERT_EQ(releaseCb.bufs.size(), 1u)
      << "ZC completion did not drain via POLLERR";

  // One send → one cmsg → range == 1, COPIED bit set on loopback.
  EXPECT_EQ(sender_->getZeroCopyCompletionsZc(), 0u);
  EXPECT_EQ(sender_->getZeroCopyCompletionsCopied(), 1u);
  EXPECT_EQ(sender_->getZeroCopySendsAckedZc(), 0u);
  EXPECT_EQ(sender_->getZeroCopySendsAckedMaybeCopied(), 1u);
  // First COPIED-marked completion latches the kill-switch.
  EXPECT_FALSE(sender_->getZeroCopy());
}

// Regression test for counter placement: when MSG_ZEROCOPY counters live
// on ZeroCopyFdBookkeeping (the per-fd shared object), a writer created
// via createPeerOnSameFd observes the same counter values as the listener
// — because both AsyncUDPSocket instances hold a shared_ptr to the same
// bookkeeping. If counters had been put on AsyncUDPSocket itself, this
// test would fail with writer counters at 0 while the listener's moved,
// reproducing the silent footgun for callers (e.g. mvfst QuicServerWorker)
// that hold a worker rather than the listener.
TEST_F(AsyncUDPSocketZeroCopyTest, CountersAreSharedAcrossCreatePeerOnSameFd) {
  if (!supported_) {
    GTEST_SKIP() << "SO_ZEROCOPY not supported on this kernel";
  }
  auto writer = sender_->createPeerOnSameFd();
  ASSERT_NE(writer, nullptr) << "createPeerOnSameFd returned null";
  auto& writerRef = *writer;

  // Listener owns the read side and the POLLERR drain.
  NoopReadCallback readCb;
  sender_->resumeRead(&readCb);
  SCOPE_EXIT {
    sender_->pauseRead();
  };

  RecordingReleaseCb releaseCb(&evb_);
  RecordingWriteCb writeCb(&releaseCb);

  const std::vector<uint8_t> payloadData(16 * 1024, 'S');
  auto payload = IOBuf::copyBuffer(payloadData.data(), payloadData.size());
  const auto ret = writerRef.writeChain(
      &writeCb,
      receiverAddr_,
      std::move(payload),
      AsyncUDPSocket::WriteOptions(0 /*gso*/, true /*zerocopy*/));
  ASSERT_GT(ret, 0) << "writeChain failed: errno=" << errno;

  evb_.tryRunAfterDelay([this]() { evb_.terminateLoopSoon(); }, 500);
  evb_.loopForever();
  ASSERT_EQ(releaseCb.bufs.size(), 1u)
      << "ZC completion did not drain via listener";

  // The same per-fd kernel completion must be visible from both handles.
  EXPECT_EQ(
      writerRef.getZeroCopyCompletionsCopied(),
      sender_->getZeroCopyCompletionsCopied());
  EXPECT_EQ(
      writerRef.getZeroCopySendsAckedMaybeCopied(),
      sender_->getZeroCopySendsAckedMaybeCopied());
  EXPECT_EQ(sender_->getZeroCopyCompletionsCopied(), 1u);
  EXPECT_EQ(sender_->getZeroCopySendsAckedMaybeCopied(), 1u);
}

// Unit test of the bookkeeping in isolation: a freshly constructed
// bookkeeping assigns monotonic ids starting at 0, and onCompletion
// hands back exactly the buf that was registered for each id.
TEST(AsyncUDPSocketZeroCopyBookkeepingTest, RegisterAndComplete) {
  AsyncUDPSocket::ZeroCopyFdBookkeeping bk;
  RecordingReleaseCb cb;

  auto buf0 = IOBuf::create(8);
  auto buf1 = IOBuf::create(8);
  auto* raw0 = buf0.get();
  auto* raw1 = buf1.get();

  bk.registerBuf(std::move(buf0), &cb);
  bk.registerBuf(std::move(buf1), &cb);
  EXPECT_TRUE(bk.hasPending());

  bk.onCompletion(0, 1);
  EXPECT_FALSE(bk.hasPending());
  ASSERT_EQ(cb.bufs.size(), 2);
  EXPECT_EQ(cb.bufs[0].get(), raw0);
  EXPECT_EQ(cb.bufs[1].get(), raw1);
}

// onCompletion must silently ignore ids that aren't registered; this is
// what makes the shared-fd case work — a listener processes the whole
// kernel-emitted [lo, hi] range but only some ids belong to a given
// bookkeeping. Without this tolerance, a separate-bookkeeping-on-same-fd
// scenario would CHECK-fail.
TEST(AsyncUDPSocketZeroCopyBookkeepingTest, ToleratesUnknownIdsInRange) {
  AsyncUDPSocket::ZeroCopyFdBookkeeping bk;
  RecordingReleaseCb cb;

  auto buf = IOBuf::create(8);
  auto* raw = buf.get();
  bk.registerBuf(std::move(buf), &cb);

  bk.onCompletion(0, 5); // ids 1..5 are unknown
  EXPECT_FALSE(bk.hasPending());
  ASSERT_EQ(cb.bufs.size(), 1);
  EXPECT_EQ(cb.bufs[0].get(), raw);
}

// A null ReleaseIOBufCallback means "the buf has no owner that needs to
// know about completion — just hold it alive until the kernel is done."
// onCompletion drops the buf without calling anything.
TEST(AsyncUDPSocketZeroCopyBookkeepingTest, NullCallbackDropsBufOnCompletion) {
  AsyncUDPSocket::ZeroCopyFdBookkeeping bk;

  auto buf = IOBuf::create(8);
  bk.registerBuf(std::move(buf), nullptr);
  EXPECT_TRUE(bk.hasPending());

  bk.onCompletion(0, 0);
  EXPECT_FALSE(bk.hasPending());
}

// Regression: onCompletion's loop walks [lo, hi] inclusive and must not
// overflow uint32_t when hi == UINT32_MAX, even on iterations that don't
// match a registered id. Reaching this test at all means we didn't hang.
TEST(AsyncUDPSocketZeroCopyBookkeepingTest, OnCompletionAtUint32MaxBoundary) {
  AsyncUDPSocket::ZeroCopyFdBookkeeping bk;
  RecordingReleaseCb cb;

  // Single unknown id at the max — no registered entries at all.
  bk.onCompletion(
      std::numeric_limits<uint32_t>::max(),
      std::numeric_limits<uint32_t>::max());
  EXPECT_FALSE(bk.hasPending());
  EXPECT_TRUE(cb.bufs.empty());

  // Two-id range ending at UINT32_MAX with neither id registered.
  bk.onCompletion(
      std::numeric_limits<uint32_t>::max() - 1,
      std::numeric_limits<uint32_t>::max());
  EXPECT_FALSE(bk.hasPending());
  EXPECT_TRUE(cb.bufs.empty());
}

// Regression: recordCompletion must compute the range correctly when a
// single cmsg straddles the sk_zckey wrap boundary (hi < lo). Without
// u32-wrap-aware subtraction the SendsAcked* counters jump by ~2^32 per
// cmsg, silently poisoning the lower-bound ZC-fraction estimate.
TEST(AsyncUDPSocketZeroCopyBookkeepingTest, RecordCompletionAcrossWrap) {
  AsyncUDPSocket::ZeroCopyFdBookkeeping bk;

  // Single-id at UINT32_MAX (no wrap, sanity).
  bk.recordCompletion(
      std::numeric_limits<uint32_t>::max(),
      std::numeric_limits<uint32_t>::max(),
      /*copied=*/false);
  EXPECT_EQ(bk.getSendsAckedZc(), 1u);

  // Straddling cmsg: lo=UINT32_MAX-1, hi=1 → covers 4 sends.
  bk.recordCompletion(
      std::numeric_limits<uint32_t>::max() - 1,
      1,
      /*copied=*/false);
  EXPECT_EQ(bk.getSendsAckedZc(), 1u + 4u);
}

#endif // FOLLY_HAVE_MSG_ERRQUEUE

} // namespace
