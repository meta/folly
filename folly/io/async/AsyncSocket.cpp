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

#include <fmt/format.h>
#include <folly/io/async/AsyncSocket.h>

#include <sys/types.h>

#include <cerrno>
#include <sstream>

#include <folly/Exception.h>
#include <folly/Overload.h>
#include <folly/Portability.h>
#include <folly/SocketAddress.h>
#include <folly/String.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <folly/io/SocketOptionMap.h>
#include <folly/io/async/IoUringBackend.h>
#include <folly/lang/CheckedMath.h>
#include <folly/portability/Fcntl.h>
#include <folly/portability/Sockets.h>
#include <folly/portability/SysMman.h>
#include <folly/portability/SysUio.h>
#include <folly/portability/Unistd.h>

#if defined(__linux__)
#include <linux/if_packet.h>
#include <linux/sockios.h>
#include <sys/ioctl.h>
#endif

using ZeroCopyMemStore = folly::AsyncReader::ReadCallback::ZeroCopyMemStore;

namespace {
class ZeroCopyMMapMemStoreFallback : public ZeroCopyMemStore {
 public:
  ZeroCopyMMapMemStoreFallback(size_t /*entries*/, size_t /*size*/) {}
  ~ZeroCopyMMapMemStoreFallback() override = default;
  ZeroCopyMemStore::EntryPtr get() override { return nullptr; }

  void put(ZeroCopyMemStore::Entry* /*entry*/) override {}
};

#if defined(TCP_ZEROCOPY_RECEIVE)
std::unique_ptr<folly::IOBuf> getRXZeroCopyIOBuf(
    ZeroCopyMemStore::EntryPtr&& ptr) {
  auto* entry = ptr.release();
  return folly::IOBuf::takeOwnership(
      entry->data,
      entry->len,
      entry->len,
      [](void* /*buf*/, void* userData) {
        reinterpret_cast<ZeroCopyMemStore::Entry*>(userData)->put();
      },
      entry);
}

class ZeroCopyMMapMemStoreReal : public ZeroCopyMemStore {
 public:
  ZeroCopyMMapMemStoreReal(size_t entries, size_t size) {
    // we just need a socket so the kernel
    // will set the vma->vm_ops = &tcp_vm_ops
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      void* addr =
          ::mmap(nullptr, entries * size, PROT_READ, MAP_SHARED, fd, 0);
      folly::fileops::close(fd);
      if (addr != MAP_FAILED) {
        addr_ = addr;
        numEntries_ = entries;
        entrySize_ = size;
        entries_.resize(numEntries_);
        for (size_t i = 0; i < numEntries_; i++) {
          entries_[i].data =
              reinterpret_cast<uint8_t*>(addr_) + (i * entrySize_);
          entries_[i].capacity = entrySize_;
          entries_[i].store = this;
          avail_.push_back(&entries_[i]);
        }
      }
    }
  }
  ~ZeroCopyMMapMemStoreReal() override {
    CHECK_EQ(avail_.size(), numEntries_);
    if (addr_) {
      ::munmap(addr_, numEntries_ * entrySize_);
    }
  }

  ZeroCopyMemStore::EntryPtr get() override {
    std::unique_lock lk(availMutex_);
    if (!avail_.empty()) {
      auto* entry = avail_.front();
      avail_.pop_front();
      DCHECK(entry->len == 0);
      DCHECK(entry->capacity == entrySize_);

      ZeroCopyMemStore::EntryPtr ret(entry);

      return ret;
    }

    return nullptr;
  }

  void put(ZeroCopyMemStore::Entry* entry) override {
    if (entry) {
      DCHECK(entry->store == this);
      if (entry->len) {
        auto ret = ::madvise(entry->data, entry->len, MADV_DONTNEED);
        entry->len = 0;
        DCHECK(!ret);
      }

      std::unique_lock lk(availMutex_);
      avail_.push_back(entry);
    }
  }

 private:
  std::vector<ZeroCopyMemStore::Entry> entries_;
  std::mutex availMutex_;
  std::deque<ZeroCopyMemStore::Entry*> avail_;
  void* addr_{nullptr};
  size_t numEntries_{0};
  size_t entrySize_{0};
};

using ZeroCopyMMapMemStore = ZeroCopyMMapMemStoreReal;
#else
using ZeroCopyMMapMemStore = ZeroCopyMMapMemStoreFallback;
#endif

#if FOLLY_HAS_LIBURING
bool checkIoUringBackend(folly::EventBase* evb) {
  return dynamic_cast<folly::IoUringBackend*>(evb->getBackend()) != nullptr;
}
#else
bool checkIoUringBackend(folly::EventBase*) {
  return false;
}
#endif
} // namespace

#if FOLLY_HAVE_VLA
#define FOLLY_DETAIL_VLA_SIZE(size, maxSize) size
#else
#define FOLLY_DETAIL_VLA_SIZE(size, maxSize) maxSize
#endif

using std::string;
using std::unique_ptr;

namespace fsp = folly::portability::sockets;

namespace folly {

static constexpr bool msgErrQueueSupported =
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
    true;
#else
    false;
#endif // FOLLY_HAVE_MSG_ERRQUEUE

std::unique_ptr<ZeroCopyMemStore> AsyncSocket::createDefaultZeroCopyMemStore(
    size_t entries, size_t size) {
  return std::make_unique<ZeroCopyMMapMemStore>(entries, size);
}

static AsyncSocketException const& getSocketClosedLocallyEx() {
  static auto& ex = *new AsyncSocketException(
      AsyncSocketException::END_OF_FILE, "socket closed locally");
  return ex;
}

static AsyncSocketException const& getSocketShutdownForWritesEx() {
  static auto& ex = *new AsyncSocketException(
      AsyncSocketException::END_OF_FILE, "socket shutdown for writes");
  return ex;
}

namespace {
#if FOLLY_HAVE_SO_TIMESTAMPING
const sock_extended_err* FOLLY_NULLABLE
cmsgToSockExtendedErr(const cmsghdr& cmsg) {
  if ((cmsg.cmsg_level == SOL_IP && cmsg.cmsg_type == IP_RECVERR) ||
      (cmsg.cmsg_level == SOL_IPV6 && cmsg.cmsg_type == IPV6_RECVERR) ||
      (cmsg.cmsg_level == SOL_PACKET &&
       cmsg.cmsg_type == PACKET_TX_TIMESTAMP)) {
    return reinterpret_cast<const sock_extended_err*>(CMSG_DATA(&cmsg));
  }
  (void)cmsg;
  return nullptr;
}

const sock_extended_err* FOLLY_NULLABLE
cmsgToSockExtendedErrTimestamping(const cmsghdr& cmsg) {
  const auto serr = cmsgToSockExtendedErr(cmsg);
  if (serr && serr->ee_errno == ENOMSG &&
      serr->ee_origin == SO_EE_ORIGIN_TIMESTAMPING) {
    return serr;
  }
  (void)cmsg;
  return nullptr;
}

const scm_timestamping* FOLLY_NULLABLE
cmsgToScmTimestamping(const cmsghdr& cmsg) {
  if (cmsg.cmsg_level == SOL_SOCKET && cmsg.cmsg_type == SCM_TIMESTAMPING) {
    return reinterpret_cast<const struct scm_timestamping*>(CMSG_DATA(&cmsg));
  }
  (void)cmsg;
  return nullptr;
}

#endif // FOLLY_HAVE_SO_TIMESTAMPING
} // namespace

// TODO: It might help performance to provide a version of BytesWriteRequest
// that users could derive from, so we can avoid the extra allocation for each
// call to write()/writev().
//
// We would need the version for external users where they provide the iovec
// storage space, and only our internal version would allocate it at the end of
// the WriteRequest.

/* The default WriteRequest implementation, used for write(), writev() and
 * writeChain()
 *
 * A new BytesWriteRequest operation is allocated on the heap for all write
 * operations that cannot be completed immediately.
 */
class AsyncSocket::BytesWriteRequest : public AsyncSocket::WriteRequest {
 public:
  static BytesWriteRequest* newRequest(
      AsyncSocket* socket,
      WriteCallbackWithState callbackWithState,
      const iovec* ops,
      uint32_t opCount,
      uint32_t partialWritten,
      uint32_t bytesWritten,
      unique_ptr<IOBuf>&& ioBuf,
      WriteFlags flags) {
    assert(opCount > 0);
    // Since we put a variable size iovec array at the end
    // of each BytesWriteRequest, we have to manually allocate the memory.
    uint32_t bufSize = 0;
    if (!checked_muladd<uint32_t>(
            &bufSize,
            opCount,
            sizeof(struct iovec),
            sizeof(BytesWriteRequest))) {
      throw std::bad_alloc();
    }
    void* buf = malloc(bufSize);
    if (buf == nullptr) {
      throw std::bad_alloc();
    }

    return new (buf) BytesWriteRequest(
        socket,
        callbackWithState,
        ops,
        opCount,
        partialWritten,
        bytesWritten,
        std::move(ioBuf),
        flags);
  }

  void destroy() override {
    auto* FOLLY_NONNULL socket = CHECK_NOTNULL(socket_);
    if (ioBuf_ && socket->containsZeroCopyBuf(ioBuf_.get())) {
      socket->setZeroCopyBuf(std::move(ioBuf_), releaseIOBufCallback_);
    } else {
      socket->releaseIOBuf(std::move(ioBuf_), releaseIOBufCallback_);
    }
    this->~BytesWriteRequest();
    free(this);
  }

  WriteResult performWrite() override {
    WriteFlags writeFlags = flags_;
    if (getNext() != nullptr) {
      writeFlags |= WriteFlags::CORK;
    }

    socket_->adjustZeroCopyFlags(writeFlags);

    auto writeResult = socket_->performWrite(
        getOps(),
        getOpCount(),
        writeFlags,
        &opsWritten_,
        &partialBytes_,
        WriteRequestTag{ioBuf_.get()});
    bytesWritten_ = writeResult.writeReturn > 0 ? writeResult.writeReturn : 0;
    if (bytesWritten_) {
      if (socket_->isZeroCopyRequest(writeFlags)) {
        if (isComplete()) {
          socket_->addZeroCopyBuf(std::move(ioBuf_), releaseIOBufCallback_);
        } else {
          socket_->addZeroCopyBuf(ioBuf_.get());
        }
      } else {
        // this happens if at least one of the prev requests were sent
        // with zero copy but not the last one
        if (isComplete() && zeroCopyRequest_ &&
            socket_->containsZeroCopyBuf(ioBuf_.get())) {
          socket_->setZeroCopyBuf(std::move(ioBuf_), releaseIOBufCallback_);
        }
      }
    }
    return writeResult;
  }

  bool isComplete() override { return opsWritten_ == getOpCount(); }

  void consume() override {
    // Advance opIndex_ forward by opsWritten_
    opIndex_ += opsWritten_;
    assert(opIndex_ < opCount_);

    bool zeroCopyReq = socket_->isZeroCopyRequest(flags_);
    if (zeroCopyReq) {
      zeroCopyRequest_ = true;
    }

    if (!zeroCopyRequest_) {
      // If we've finished writing any IOBufs, release them
      // but only if we did not send any of them via zerocopy
      if (ioBuf_) {
        for (uint32_t i = opsWritten_; i != 0; --i) {
          assert(ioBuf_);
          auto next = ioBuf_->pop();
          socket_->releaseIOBuf(std::move(ioBuf_), releaseIOBufCallback_);
          ioBuf_ = std::move(next);
        }
      }
    }

    // Move partialBytes_ forward into the current iovec buffer
    struct iovec* currentOp = writeOps_ + opIndex_;
    assert((partialBytes_ < currentOp->iov_len) || (currentOp->iov_len == 0));
    currentOp->iov_base =
        reinterpret_cast<uint8_t*>(currentOp->iov_base) + partialBytes_;
    currentOp->iov_len -= partialBytes_;

    // Increment the totalBytesWritten_ count by bytesWritten_;
    assert(bytesWritten_ >= 0);
    totalBytesWritten_ += uint32_t(bytesWritten_);
  }

 private:
  BytesWriteRequest(
      AsyncSocket* socket,
      WriteCallbackWithState callbackWithState,
      const struct iovec* ops,
      uint32_t opCount,
      uint32_t partialBytes,
      uint32_t bytesWritten,
      unique_ptr<IOBuf>&& ioBuf,
      WriteFlags flags)
      : AsyncSocket::WriteRequest(socket, callbackWithState),
        opCount_(opCount),
        opIndex_(0),
        flags_(flags),
        ioBuf_(std::move(ioBuf)),
        opsWritten_(0),
        partialBytes_(partialBytes),
        bytesWritten_(bytesWritten) {
    memcpy(writeOps_, ops, sizeof(*ops) * opCount_);
    zeroCopyRequest_ = socket_->isZeroCopyRequest(flags_);
  }

  // private destructor, to ensure callers use destroy()
  ~BytesWriteRequest() override = default;

  const struct iovec* getOps() const {
    assert(opCount_ > opIndex_);
    return writeOps_ + opIndex_;
  }

  uint32_t getOpCount() const {
    assert(opCount_ > opIndex_);
    return opCount_ - opIndex_;
  }

  uint32_t opCount_; ///< number of entries in writeOps_
  uint32_t opIndex_; ///< current index into writeOps_
  WriteFlags flags_; ///< set for WriteFlags
  bool zeroCopyRequest_{
      false}; ///< if we sent any part of the ioBuf_ with zerocopy
  unique_ptr<IOBuf> ioBuf_; ///< underlying IOBuf, or nullptr if N/A

  // for consume(), how much we wrote on the last write
  uint32_t opsWritten_; ///< complete ops written
  uint32_t partialBytes_; ///< partial bytes of incomplete op written
  ssize_t bytesWritten_; ///< bytes written altogether

  struct iovec writeOps_[]; ///< write operation(s) list
};

int AsyncSocket::SendMsgParamsCallback::getDefaultFlags(
    folly::WriteFlags flags, bool zeroCopyEnabled) noexcept {
  int msg_flags = MSG_DONTWAIT;

#ifdef MSG_NOSIGNAL // Linux-only
  msg_flags |= MSG_NOSIGNAL;
#ifdef MSG_MORE
  if (isSet(flags, WriteFlags::CORK)) {
    // MSG_MORE tells the kernel we have more data to send, so wait for us to
    // give it the rest of the data rather than immediately sending a partial
    // frame, even when TCP_NODELAY is enabled.
    msg_flags |= MSG_MORE;
  }
#endif // MSG_MORE
#endif // MSG_NOSIGNAL
  if (isSet(flags, WriteFlags::EOR)) {
    // marks that this is the last byte of a record (response)
    msg_flags |= MSG_EOR;
  }

  if (zeroCopyEnabled && isSet(flags, WriteFlags::WRITE_MSG_ZEROCOPY)) {
    msg_flags |= MSG_ZEROCOPY;
  }

  return msg_flags;
}

void AsyncSocket::SendMsgParamsCallback::getAncillaryData(
    folly::WriteFlags flags,
    void* data,
    const WriteRequestTag& writeTag,
    const bool byteEventsEnabled) noexcept {
  auto ancillaryDataSize =
      getAncillaryDataSize(flags, writeTag, byteEventsEnabled);
  if (!ancillaryDataSize) {
    return;
  }
#if FOLLY_HAVE_SO_TIMESTAMPING
  CHECK_NOTNULL(data);
  // this function only handles ancillary data for timestamping
  //
  // if getAncillaryDataSize() is overridden and returning a size different
  // than what we expect, then this function needs to be overridden too, in
  // order to avoid conflict with how cmsg / msg are written
  CHECK_EQ(CMSG_SPACE(sizeof(uint32_t)), ancillaryDataSize);

  uint32_t sofFlags = 0;
  if (byteEventsEnabled && isSet(flags, WriteFlags::TIMESTAMP_TX)) {
    sofFlags = sofFlags | folly::netops::SOF_TIMESTAMPING_TX_SOFTWARE;
  }
  if (byteEventsEnabled && isSet(flags, WriteFlags::TIMESTAMP_ACK)) {
    sofFlags = sofFlags | folly::netops::SOF_TIMESTAMPING_TX_ACK;
  }
  if (byteEventsEnabled && isSet(flags, WriteFlags::TIMESTAMP_SCHED)) {
    sofFlags = sofFlags | folly::netops::SOF_TIMESTAMPING_TX_SCHED;
  }

  msghdr msg{};
  msg.msg_control = data;
  msg.msg_controllen = ancillaryDataSize;
  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  CHECK_NOTNULL(cmsg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SO_TIMESTAMPING;
  cmsg->cmsg_len = CMSG_LEN(sizeof(uint32_t));
  memcpy(CMSG_DATA(cmsg), &sofFlags, sizeof(sofFlags));
#else
  (void)data;
#endif // FOLLY_HAVE_SO_TIMESTAMPING
  return;
}

uint32_t AsyncSocket::SendMsgParamsCallback::getAncillaryDataSize(
    folly::WriteFlags flags,
    const WriteRequestTag&,
    const bool byteEventsEnabled) noexcept {
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  if (WriteFlags::NONE != (flags & kWriteFlagsForTimestamping) &&
      byteEventsEnabled) {
    return CMSG_SPACE(sizeof(uint32_t));
  }
#else
  (void)flags;
  (void)byteEventsEnabled;
#endif
  return 0;
}

folly::Optional<AsyncSocket::ByteEvent>
AsyncSocket::ByteEventHelper::processCmsg(
    const cmsghdr& cmsg, const size_t rawBytesWritten) {
#if FOLLY_HAVE_SO_TIMESTAMPING
  if (!byteEventsEnabled || maybeEx.has_value()) {
    return folly::none;
  }
  if (!maybeTsState_.has_value()) {
    maybeTsState_ = TimestampState();
  }
  auto& state = maybeTsState_.value();
  if (auto serrTs = cmsgToSockExtendedErrTimestamping(cmsg)) {
    if (state.serrReceived) {
      // already have this part of the message pending
      throw Exception("already have serr event");
    }
    state.serrReceived = true;
    state.typeRaw = serrTs->ee_info;
    state.byteOffsetKernel = serrTs->ee_data;
  } else if (auto scmTs = cmsgToScmTimestamping(cmsg)) {
    if (state.scmTsReceived) {
      throw Exception("already have scmTs event");
    }
    state.scmTsReceived = true;

    auto timespecToDuration =
        [](const timespec& ts) -> folly::Optional<std::chrono::nanoseconds> {
      std::chrono::nanoseconds duration = std::chrono::seconds(ts.tv_sec) +
          std::chrono::nanoseconds(ts.tv_nsec);
      if (duration == duration.zero()) {
        return folly::none;
      }
      return duration;
    };
    // ts[0] -> software timestamp
    // ts[1] -> hardware timestamp transformed to userspace time (deprecated)
    // ts[2] -> hardware timestamp
    state.maybeSoftwareTs = timespecToDuration(scmTs->ts[0]);
    state.maybeHardwareTs = timespecToDuration(scmTs->ts[2]);
  }

  // if we have both components needed for a complete timestamp, build it
  if (state.serrReceived && state.scmTsReceived) {
    // cleanup state so that we're ready for next timestamp
    TimestampState completeState = state;
    maybeTsState_ = folly::none;

    // map the type
    folly::Optional<ByteEvent::Type> tsType;
    switch (completeState.typeRaw) {
      case folly::netops::SCM_TSTAMP_SND: {
        tsType = ByteEvent::Type::TX;
        break;
      }
      case folly::netops::SCM_TSTAMP_ACK: {
        tsType = ByteEvent::Type::ACK;
        break;
      }
      case folly::netops::SCM_TSTAMP_SCHED: {
        tsType = ByteEvent::Type::SCHED;
        break;
      }
      default:
        break; // unknown, maybe something new
    }
    if (!tsType) {
      // it's a timestamp, but not one that we're set up to handle
      // we've cleared our state, loop back around
      return folly::none;
    }

    // Calculate the byte offset.
    //
    // See documentation for SOF_TIMESTAMPING_OPT_ID for details.
    //
    // In summary, two things we have to consider:
    //
    //   (1) The byte stream offset is relative:
    //       Socket timestamps include the byte stream offset for which the
    //       timestamp applies. There may have been bytes transferred before the
    //       fd was controlled by AsyncSocket. As a result, we don't know the
    //       socket byte stream offset when we enable timestamping.
    //
    //       To get around this, we set SOF_TIMESTAMPING_OPT_ID when we enable
    //       timestamping via setsockopt. This flag causes the kernel to reset
    //       the offset it uses for timestamps to 0. This allows us to determine
    //       an offset relative to the number of bytes that had been written to
    //       the socket since timestamps were enabled.
    //
    //       Note that offsets begin at zero; if only a single byte is written
    //       after timestamping is enabled, the offset included in the kernel
    //       cmsg will be 0.
    //
    //   (2) The byte stream offset is a uint32_t:
    //       Because the kernel uses a uint32_t to store and communicate the
    //       byte stream offset, the offset will wrap every ~4GB. When we get a
    //       timestamp, we need to figure out which byte it is for. We assume
    //       that there will never be more than ~4GB of bytes sent between us
    //       requesting timestamping for a byte and receiving the timestamp;
    //       this is a realistic assumption given CWND and TCP buffer sizes. We
    //       then calculate assuming that the counter has not wrapped since we
    //       sent the byte that we are getting the timestamp for. If the counter
    //       has wrapped, we detect it, and go back one position.
    const uint64_t bytesPerOffsetWrap =
        static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1;

    // Normalize the aligned kernel offset before placing it in the epoch
    // containing `rawBytesWritten`.
    const auto signedBytesPerOffsetWrap =
        static_cast<int64_t>(bytesPerOffsetWrap);
    const auto offsetWithinWrap = static_cast<uint64_t>(
        (rawBytesWrittenWhenByteEventsEnabled % signedBytesPerOffsetWrap +
         static_cast<int64_t>(completeState.byteOffsetKernel) +
         signedBytesPerOffsetWrap) %
        signedBytesPerOffsetWrap);
    size_t byteOffset = rawBytesWritten -
        (rawBytesWritten % bytesPerOffsetWrap) + offsetWithinWrap;
    // Kernel 7.1 can rarely give a corrupt offset for retransmitted packets.
    // Reject offsets that cannot be placed in a valid epoch.
    if (byteOffset > rawBytesWritten) {
      if (static_cast<uint64_t>(rawBytesWritten) < bytesPerOffsetWrap) {
        return folly::none;
      }
      byteOffset -= bytesPerOffsetWrap;
    }
    // Kernel offset zero corresponds to this baseline, so no valid timestamp
    // can resolve to an earlier nonnegative offset.
    if (rawBytesWrittenWhenByteEventsEnabled > 0 &&
        byteOffset <
            static_cast<size_t>(rawBytesWrittenWhenByteEventsEnabled)) {
      return folly::none;
    }

    ByteEvent event = {};
    event.type = tsType.value();
    event.offset = byteOffset;
    event.maybeSoftwareTs = state.maybeSoftwareTs;
    event.maybeHardwareTs = state.maybeHardwareTs;
    return event;
  }
#else
  (void)cmsg;
  (void)rawBytesWritten;
#endif // FOLLY_HAVE_SO_TIMESTAMPING
  return folly::none;
}

namespace {
AsyncSocket::SendMsgParamsCallback defaultSendMsgParamsCallback;

// Based on flags, signal the transparent handler to disable certain functions
void disableTransparentFunctions(
    NetworkSocket fd, bool noTransparentTls, bool noTSocks) {
  (void)fd;
  (void)noTransparentTls;
  (void)noTSocks;
#if defined(__linux__)
  if (noTransparentTls) {
    // Ignore return value, errors are ok
    VLOG(5) << "Disabling TTLS for fd " << fd;
    __u8 optval = FOLLY_SO_TTLS_TRUSTED_VAL_ENCRYPTED;
    netops::setsockopt(
        fd, SOL_SOCKET, FOLLY_SO_TTLS_TRUSTED, &optval, sizeof(optval));
  }
  if (noTSocks) {
    VLOG(5) << "Disabling TSOCKS for fd " << fd;
    // Ignore return value, errors are ok
    netops::setsockopt(fd, SOL_SOCKET, SO_NO_TSOCKS, nullptr, 0);
  }
#endif
}

constexpr size_t kSmallIoVecSize = 64;

} // namespace

AsyncSocket::AsyncSocket()
    : eventBase_(nullptr),
      writeTimeout_(this, nullptr),
      ioHandler_(this, nullptr),
      immediateReadHandler_(this),
      observerContainer_(this) {
  VLOG(5) << "new AsyncSocket()";
  init();
}

AsyncSocket::AsyncSocket(EventBase* evb)
    : eventBase_(evb),
      writeTimeout_(this, evb),
      ioHandler_(this, evb),
      immediateReadHandler_(this),
      observerContainer_(this) {
  VLOG(5) << "new AsyncSocket(" << this << ", evb=" << evb << ")";
  init();
}

AsyncSocket::AsyncSocket(
    EventBase* evb,
    const folly::SocketAddress& address,
    uint32_t connectTimeout,
    bool useZeroCopy)
    : AsyncSocket(evb) {
  setZeroCopy(useZeroCopy);
  connect(nullptr, address, connectTimeout);
}

AsyncSocket::AsyncSocket(
    EventBase* evb,
    const std::string& ip,
    uint16_t port,
    uint32_t connectTimeout,
    bool useZeroCopy)
    : AsyncSocket(evb) {
  setZeroCopy(useZeroCopy);
  connect(nullptr, ip, port, connectTimeout);
}

AsyncSocket::AsyncSocket(
    EventBase* evb,
    NetworkSocket fd,
    uint32_t zeroCopyBufId,
    const SocketAddress* peerAddress,
    folly::Optional<std::chrono::steady_clock::time_point>
        maybeConnectionEstablishTime)
    : zeroCopyBufId_(zeroCopyBufId),
      state_(StateEnum::ESTABLISHED),
      fd_(fd),
      addr_(peerAddress ? *peerAddress : folly::SocketAddress()),
      eventBase_(evb),
      writeTimeout_(this, evb),
      ioHandler_(this, evb, fd),
      immediateReadHandler_(this),
      maybeConnectionEstablishTime_(std::move(maybeConnectionEstablishTime)),
      observerContainer_(this) {
  VLOG(5) << "new AsyncSocket(" << this << ", evb=" << evb << ", fd=" << fd
          << ", zeroCopyBufId=" << zeroCopyBufId << ")";
  init();
  disableTransparentFunctions(fd_, noTransparentTls_, noTSocks_);
  setCloseOnExec();
}

AsyncSocket::AsyncSocket(AsyncSocket* oldAsyncSocket)
    : zeroCopyBufId_(oldAsyncSocket->getZeroCopyBufId()),
      state_(oldAsyncSocket->state_),
      fd_(oldAsyncSocket->getNetworkSocket()),
      addr_(oldAsyncSocket->addr_),
      eventBase_(oldAsyncSocket->getEventBase()),
      writeTimeout_(this, eventBase_),
      ioHandler_(this, eventBase_, fd_),
      immediateReadHandler_(this),
      appBytesWritten_(oldAsyncSocket->appBytesWritten_),
      rawBytesWritten_(oldAsyncSocket->rawBytesWritten_),
      preReceivedData_(std::move(oldAsyncSocket->preReceivedData_)),
      connectStartTime_(oldAsyncSocket->connectStartTime_),
      connectEndTime_(oldAsyncSocket->connectEndTime_),
      maybeConnectionEstablishTime_(
          oldAsyncSocket->maybeConnectionEstablishTime_),
      tfoInfo_(std::move(oldAsyncSocket->tfoInfo_)),
      byteEventHelper_(std::move(oldAsyncSocket->byteEventHelper_)),
      observerContainer_(this, std::move(oldAsyncSocket->observerContainer_)) {
  // delay detaching network socket until observers moved to prevent spurious
  // detachFd and close notifications
  if (oldAsyncSocket->useIoUring_ && oldAsyncSocket->iouRecvHandle_) {
    // io_uring recv operations are async — detach the handle so in-flight
    // recvs complete into a DetachedReadCallback. The detached handle is
    // transferred to the new socket and passed to IoUringRecvHandle::clone()
    // in setReadCB(), which inherits any queued data and pending reads.
    if (!oldAsyncSocket->iouRecvHandleDetached_) {
      oldAsyncSocket->iouRecvHandle_->detachEventBase();
    }
    iouRecvHandle_ = std::move(oldAsyncSocket->iouRecvHandle_);
    iouRecvHandleDetached_ = true;
  }
  oldAsyncSocket->iouSendHandle_.reset();
  oldAsyncSocket->detachNetworkSocket();
  if (oldAsyncSocket->isZeroCopyWriteInProgress()) {
    moveZeroCopyStateFrom(*oldAsyncSocket);
  }

  VLOG(5) << "move AsyncSocket(" << oldAsyncSocket << "->" << this
          << ", evb=" << eventBase_ << ", fd=" << fd_
          << ", zeroCopyBufId=" << zeroCopyBufId_ << ")";
  init();
  disableTransparentFunctions(fd_, noTransparentTls_, noTSocks_);
  setCloseOnExec();

  // inform lifecycle observers to give them an opportunity to unsubscribe from
  // events for the old socket and subscribe to the new socket; we do not move
  // the subscription ourselves

  // legacy observer support
  for (const auto& cb : oldAsyncSocket->lifecycleObservers_) {
    cb->move(oldAsyncSocket, this);
  }
}

AsyncSocket::AsyncSocket(AsyncSocket::UniquePtr oldAsyncSocket)
    : AsyncSocket(oldAsyncSocket.get()) {}

// init() method, since constructor forwarding isn't supported in most
// compilers yet.
void AsyncSocket::init() {
  if (eventBase_) {
    eventBase_->dcheckIsInEventBaseThread();
    useIoUring_ = checkIoUringBackend(eventBase_);
  }
  eventFlags_ = EventHandler::NONE;
  sendTimeout_ = 0;
  maxReadsPerEvent_ = 16;
  connectCallback_ = nullptr;
  errMessageCallback_ = nullptr;
  readAncillaryDataCallback_ = nullptr;
  readCallback_ = nullptr;
  writeReqHead_ = nullptr;
  writeReqTail_ = nullptr;
  wShutdownSocketSet_.reset();
  appBytesReceived_ = 0;
  totalAppBytesScheduledForWrite_ = 0;
  sendMsgParamCallback_ = &defaultSendMsgParamsCallback;
  if (useIoUring_ && state_ == StateEnum::ESTABLISHED) {
    assert(iouSendHandle_ == nullptr);
    iouSendHandle_ = IoUringSendHandle::create(eventBase_, fd_, addr_, this);
  }
}

AsyncSocket::~AsyncSocket() {
  VLOG(7) << "actual destruction of AsyncSocket(this=" << this << ", evb="
          << eventBase_ << ", fd=" << fd_ << ", state=" << state_ << ")";
  for (const auto& cb : lifecycleObservers_) {
    cb->destroy(this);
  }
  DCHECK_EQ(allocatedBytesBuffered_, 0);
}

void AsyncSocket::destroy() {
  VLOG(5) << "AsyncSocket::destroy(this=" << this << ", evb=" << eventBase_
          << ", fd=" << fd_ << ", state=" << state_;
  // When destroy is called, close the socket immediately
  closeNow();

  // Then call DelayedDestruction::destroy() to take care of
  // whether or not we need immediate or delayed destruction
  DelayedDestruction::destroy();
}

NetworkSocket AsyncSocket::detachNetworkSocket() {
  VLOG(6) << "AsyncSocket::detachFd(this=" << this << ", fd=" << fd_
          << ", evb=" << eventBase_ << ", state=" << state_
          << ", events=" << std::hex << eventFlags_ << ")";
  // legacy observer support
  for (const auto& cb : lifecycleObservers_) {
    cb->fdDetach(this);
  }

  // folly::ObserverContainer observer support
  if (auto list = getAsyncSocketObserverContainer()) {
    list->invokeInterfaceMethodAllObservers([](auto observer, auto observed) {
      observer->fdDetach(observed);
    });
  }

  // Extract the fd, and set fd_ to -1 first, so closeNow() won't
  // actually close the descriptor.
  if (const auto socketSet = wShutdownSocketSet_.lock()) {
    socketSet->remove(fd_);
  }
  auto fd = fd_;
  fd_ = NetworkSocket();
  // Call closeNow() to invoke all pending callbacks with an error.
  closeNow();
  // Update the EventHandler to stop using this fd.
  // This can only be done after closeNow() unregisters the handler.
  ioHandler_.changeHandlerFD(NetworkSocket());
  return fd;
}

void AsyncSocket::asyncDetachFd(AsyncDetachFdCallback* callback) {
  CHECK(callback);
  eventBase_->dcheckIsInEventBaseThread();
  DestructorGuard guard(this);

  if (state_ != StateEnum::ESTABLISHED) {
    callback->fdDetachFail(AsyncSocketException(
        AsyncSocketException::INVALID_STATE,
        withAddr("cannot detach an unconnected socket")));
    return;
  }

  setReadCB(nullptr);
  auto unread = takePreReceivedData();

  if (!useIoUring_ || !iouRecvHandle_) {
    auto fd = detachNetworkSocket();
    callback->fdDetached(fd, std::move(unread));
    return;
  }

  if (!iouRecvHandleDetached_) {
    iouRecvHandle_->detachEventBase();
  }
  iouRecvHandleDetached_ = false;
  auto recvHandle = std::move(iouRecvHandle_);
  auto pendingData = recvHandle->takeDetachedData();
  auto fd = detachNetworkSocket();

  folly::futures::detachOn(
      eventBase_,
      std::move(pendingData)
          .deferValue(
              [callback,
               fd,
               unread = std::move(unread),
               recvHandle = std::move(recvHandle)](
                  std::unique_ptr<IOBuf> received) mutable {
                static_cast<void>(recvHandle);
                if (!unread) {
                  unread = std::move(received);
                } else if (received) {
                  unread->appendToChain(std::move(received));
                }
                callback->fdDetached(fd, std::move(unread));
              }));
}

SemiFuture<std::unique_ptr<IOBuf>>
AsyncSocket::asyncSwitchToEventHandlerMode() {
  eventBase_->dcheckIsInEventBaseThread();

  using PendingData = std::unique_ptr<IOBuf>;
  if (!useIoUring_) {
    return makeSemiFuture(PendingData{});
  }

  const bool isEstablished = state_ == StateEnum::ESTABLISHED;
  const bool hasPendingConnect = iouConnectHandle_ != nullptr;
  const bool pendingWrites = hasPendingWrites();
  if (!isEstablished || hasPendingConnect || pendingWrites) {
    return makeSemiFuture<PendingData>(AsyncSocketException(
        AsyncSocketException::INVALID_STATE,
        withAddr(
            fmt::format(
                "cannot switch I/O mode: established={}, pending connect={}, "
                "pending writes={}",
                isEstablished,
                hasPendingConnect,
                pendingWrites))));
  }

  setReadCB(nullptr);
  useIoUring_ = false;
  iouSendHandle_.reset();

  if (!iouRecvHandle_) {
    return makeSemiFuture(PendingData{});
  }

  if (!iouRecvHandleDetached_) {
    iouRecvHandle_->detachEventBase();
  }
  iouRecvHandleDetached_ = false;
  auto recvHandle = std::move(iouRecvHandle_);
  auto pendingData = recvHandle->takeDetachedData();
  return std::move(pendingData)
      .deferValue([recvHandle = std::move(recvHandle)](PendingData data) {
        static_cast<void>(recvHandle);
        return data;
      });
}

void AsyncSocket::setShutdownSocketSet(
    const std::weak_ptr<ShutdownSocketSet>& wNewSS) {
  const auto newSS = wNewSS.lock();
  const auto shutdownSocketSet = wShutdownSocketSet_.lock();

  if (newSS == shutdownSocketSet) {
    return;
  }

  if (shutdownSocketSet && fd_ != NetworkSocket()) {
    shutdownSocketSet->remove(fd_);
  }

  if (newSS && fd_ != NetworkSocket()) {
    newSS->add(fd_);
  }

  wShutdownSocketSet_ = wNewSS;
}

void AsyncSocket::setCloseOnExec() {
  int rv = netops_->set_socket_close_on_exec(fd_);
  if (rv != 0) {
    auto errnoCopy = errno;
    throw AsyncSocketException(
        AsyncSocketException::INTERNAL_ERROR,
        withAddr("failed to set close-on-exec flag"),
        errnoCopy);
  }
}

void AsyncSocket::connect(
    ConnectCallback* callback,
    const folly::SocketAddress& address,
    int timeout,
    const SocketOptionMap& options,
    const BindOptions& bindOptions,
    const std::string& ifName) noexcept {
  DestructorGuard dg(this);
  eventBase_->dcheckIsInEventBaseThread();

  addr_ = address;

  // Make sure we're in the uninitialized state
  if (state_ != StateEnum::UNINIT) {
    if (auto* fd = std::get_if<NetworkSocket>(&bindOptions)) {
      netops_->close(*fd);
    }
    return invalidState(callback);
  }

  connectTimeout_ = std::chrono::milliseconds(timeout);
  connectStartTime_ = std::chrono::steady_clock::now();
  // Make connect end time at least >= connectStartTime.
  connectEndTime_ = connectStartTime_;

  assert(fd_ == NetworkSocket());
  state_ = StateEnum::CONNECTING;
  connectCallback_ = callback;
  invokeConnectAttempt();

  sockaddr_storage addrStorage{};
  auto saddr = reinterpret_cast<sockaddr*>(&addrStorage);

  try {
    if (auto* boundFd = std::get_if<NetworkSocket>(&bindOptions)) {
      struct sockaddr_storage peerAddr{};
      socklen_t peerLen = sizeof(peerAddr);
      if (netops_->getpeername(
              *boundFd,
              reinterpret_cast<struct sockaddr*>(&peerAddr),
              &peerLen) == 0) {
        netops_->close(*boundFd);
        throw AsyncSocketException(
            AsyncSocketException::INVALID_STATE,
            withAddr("boundFd is already connected"));
      }
      fd_ = *boundFd;
    } else {
      // Create the socket
      // Technically the first parameter should actually be a protocol family
      // constant (PF_xxx) rather than an address family (AF_xxx), but the
      // distinction is mainly just historical.  In pretty much all
      // implementations the PF_foo and AF_foo constants are identical.
      fd_ = netops_->socket(address.getFamily(), SOCK_STREAM, 0);
      if (fd_ == NetworkSocket()) {
        auto errnoCopy = errno;
        throw AsyncSocketException(
            AsyncSocketException::INTERNAL_ERROR,
            withAddr("failed to create socket"),
            errnoCopy);
      }
    }

    disableTransparentFunctions(fd_, noTransparentTls_, noTSocks_);
    handleNetworkSocketAttached();
    setCloseOnExec();

    // Put the socket in non-blocking mode
    int rv = netops_->set_socket_non_blocking(fd_);
    if (rv == -1) {
      auto errnoCopy = errno;
      throw AsyncSocketException(
          AsyncSocketException::INTERNAL_ERROR,
          withAddr("failed to put socket in non-blocking mode"),
          errnoCopy);
    }
    if (tosOrTrafficClass_) {
      setTosOrTrafficClass(*tosOrTrafficClass_);
    }

#if !defined(MSG_NOSIGNAL) && defined(F_SETNOSIGPIPE)
    // iOS and OS X don't support MSG_NOSIGNAL; set F_SETNOSIGPIPE instead
    rv = fcntl(fd_.toFd(), F_SETNOSIGPIPE, 1);
    if (rv == -1) {
      auto errnoCopy = errno;
      throw AsyncSocketException(
          AsyncSocketException::INTERNAL_ERROR,
          "failed to enable F_SETNOSIGPIPE on socket",
          errnoCopy);
    }
#endif

    // By default, turn on TCP_NODELAY
    // If setNoDelay() fails, we continue anyway; this isn't a fatal error.
    // setNoDelay() will log an error message if it fails.
    // Also set the cached zeroCopyVal_ since it cannot be set earlier if the fd
    // is not created
    if (address.getFamily() != AF_UNIX) {
      (void)setNoDelay(true);
      setZeroCopy(zeroCopyVal_);
    }

    // Apply the additional PRE_BIND options if any.
    applyOptions(options, SocketOptionKey::ApplyPos::PRE_BIND);

    VLOG(5) << "AsyncSocket::connect(this=" << this << ", evb=" << eventBase_
            << ", fd=" << fd_ << ", host=" << address.describe().c_str();

    // bind the socket to the interface
#if defined(__linux__)
    if (!ifName.empty() &&
        netops_->setsockopt(
            fd_,
            SOL_SOCKET,
            SO_BINDTODEVICE,
            ifName.c_str(),
            ifName.length())) {
      auto errnoCopy = errno;
      doClose();
      throw AsyncSocketException(
          AsyncSocketException::NOT_OPEN,
          "failed to bind to device: " + ifName,
          errnoCopy);
    }
#else
    (void)ifName;
#endif

    // bind the socket
    if (auto* bindAddr = std::get_if<folly::SocketAddress>(&bindOptions);
        bindAddr && *bindAddr != anyAddress()) {
      int one = 1;
#if defined(IP_BIND_ADDRESS_NO_PORT) && !FOLLY_MOBILE && !defined(_WIN32) && \
    !defined(__APPLE__)
      // If the any port is specified with a non-any address this is typically
      // a client socket. However, calling bind before connect without
      // IP_BIND_ADDRESS_NO_PORT forces the OS to find a unique port relying
      // on only the local tuple. This limits the range of available ephemeral
      // ports.  Using the IP_BIND_ADDRESS_NO_PORT delays assigning a port until
      // connect expanding the available port range, unless
      // setBindAddressNoPort() is called.
      if (bindAddr->getPort() == 0) {
        if (bindAddressNoPort_ &&
            netops_->setsockopt(
                fd_, IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &one, sizeof(one))) {
          auto errnoCopy = errno;
          doClose();
          throw AsyncSocketException(
              AsyncSocketException::NOT_OPEN,
              "failed to setsockopt IP_BIND_ADDRESS_NO_PORT prior to bind on " +
                  bindAddr->describe(),
              errnoCopy);
        }
      } else {
#else
      {
#endif
        if (netops_->setsockopt(
                fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one))) {
          auto errnoCopy = errno;
          doClose();
          throw AsyncSocketException(
              AsyncSocketException::NOT_OPEN,
              "failed to setsockopt SO_REUSEADDR prior to bind on " +
                  bindAddr->describe(),
              errnoCopy);
        }
      }

      bindAddr->getAddress(&addrStorage);

      if (netops_->bind(fd_, saddr, bindAddr->getActualSize()) != 0) {
        auto errnoCopy = errno;
        doClose();
        throw AsyncSocketException(
            AsyncSocketException::NOT_OPEN,
            "failed to bind to async socket: " + bindAddr->describe(),
            errnoCopy);
      }
    }

    // Apply the additional POST_BIND options if any.
    applyOptions(options, SocketOptionKey::ApplyPos::POST_BIND);

    // Call preConnect hook if any.
    if (connectCallback_) {
      connectCallback_->preConnect(fd_);
    }

    // Perform the connect()
    address.getAddress(&addrStorage);

    assert(eventBase_ != nullptr);
    if (useIoUring_ && !iouSendHandle_) {
      iouSendHandle_ =
          IoUringSendHandle::create(eventBase_, fd_, address, this);
    }

    if (tfoInfo_.enabled) {
      state_ = StateEnum::FAST_OPEN;
      tfoInfo_.attempted = true;
    } else {
      if (socketConnect(saddr, addr_.getActualSize()) < 0) {
        return;
      }
    }

    // If we're still here the connect() succeeded immediately.
    // Fall through to call the callback outside of this try...catch block
  } catch (const AsyncSocketException& ex) {
    return failConnect(__func__, ex);
  } catch (const std::exception& ex) {
    // shouldn't happen, but handle it just in case
    VLOG(4) << "AsyncSocket::connect(this=" << this << ", fd=" << fd_
            << "): unexpected " << typeid(ex).name()
            << " exception: " << ex.what();
    AsyncSocketException tex(
        AsyncSocketException::INTERNAL_ERROR,
        withAddr(string("unexpected exception: ") + ex.what()));
    return failConnect(__func__, tex);
  }

  // The connection succeeded immediately
  // The read callback may not have been set yet, and no writes may be pending
  // yet, so we don't have to register for any events at the moment.
  VLOG(8) << "AsyncSocket::connect succeeded immediately; this=" << this;
  assert(errMessageCallback_ == nullptr);
  assert(readCallback_ == nullptr);
  assert(!hasPendingWrites());
  assert(iouConnectHandle_ == nullptr);
  if (state_ != StateEnum::FAST_OPEN) {
    state_ = StateEnum::ESTABLISHED;
    if (useIoUring_ && netops_->set_socket_blocking(fd_)) {
      auto errnoCopy = errno;
      AsyncSocketException tex(
          AsyncSocketException::INTERNAL_ERROR,
          withAddr("failed to restore socket in blocking mode"),
          errnoCopy);
      return failConnect(__func__, tex);
    }
  }
  invokeConnectSuccess();
}

int AsyncSocket::socketConnect(const struct sockaddr* saddr, socklen_t len) {
  int rv = netops_->connect(fd_, saddr, len);
  if (rv < 0) {
    auto errnoCopy = errno;
    if (errnoCopy == EINPROGRESS) {
      if (!useIoUring_) {
        scheduleConnectTimeout();
        registerForConnectEvents();
      } else {
        iouConnectHandle_ = IoUringConnectHandle::create(
            eventBase_, fd_, this, connectTimeout_);
      }
    } else {
      throw AsyncSocketException(
          AsyncSocketException::NOT_OPEN,
          "connect failed (immediately)",
          errnoCopy);
    }
  }
  return rv;
}

void AsyncSocket::scheduleConnectTimeout() {
  // Connection in progress.
  auto timeout = connectTimeout_.count();
  if (timeout > 0) {
    // Start a timer in case the connection takes too long.
    if (!writeTimeout_.scheduleTimeout(uint32_t(timeout))) {
      throw AsyncSocketException(
          AsyncSocketException::INTERNAL_ERROR,
          withAddr("failed to schedule AsyncSocket connect timeout"));
    }
  }
}

void AsyncSocket::registerForConnectEvents() {
  // Register for write events, so we'll
  // be notified when the connection finishes/fails.
  // Note that we don't register for a persistent event here.
  assert(eventFlags_ == EventHandler::NONE);
  eventFlags_ = EventHandler::WRITE;
  if (!ioHandler_.registerHandler(eventFlags_)) {
    throw AsyncSocketException(
        AsyncSocketException::INTERNAL_ERROR,
        withAddr("failed to register AsyncSocket connect handler"));
  }
}

void AsyncSocket::connect(
    ConnectCallback* callback,
    const string& ip,
    uint16_t port,
    int timeout,
    const SocketOptionMap& options) noexcept {
  DestructorGuard dg(this);
  try {
    connectCallback_ = callback;
    connect(callback, folly::SocketAddress(ip, port), timeout, options);
  } catch (const std::exception& ex) {
    AsyncSocketException tex(AsyncSocketException::INTERNAL_ERROR, ex.what());
    return failConnect(__func__, tex);
  }
}

void AsyncSocket::cancelConnect() {
  connectCallback_ = nullptr;
  if (state_ == StateEnum::CONNECTING || state_ == StateEnum::FAST_OPEN) {
    closeNow();
  }
}

void AsyncSocket::setSendTimeout(uint32_t milliseconds) {
  sendTimeout_ = milliseconds;
  if (eventBase_) {
    eventBase_->dcheckIsInEventBaseThread();
  }

  // If we are currently pending on write requests, immediately update
  // writeTimeout_ with the new value.
  if ((eventFlags_ & EventHandler::WRITE) &&
      (state_ != StateEnum::CONNECTING && state_ != StateEnum::FAST_OPEN)) {
    assert(state_ == StateEnum::ESTABLISHED);
    assert((shutdownFlags_ & SHUT_WRITE) == 0);
    if (sendTimeout_ > 0) {
      if (!writeTimeout_.scheduleTimeout(sendTimeout_)) {
        AsyncSocketException ex(
            AsyncSocketException::INTERNAL_ERROR,
            withAddr("failed to reschedule send timeout in setSendTimeout"));
        return failWrite(__func__, ex);
      }
    } else {
      writeTimeout_.cancelTimeout();
    }
  }
}

void AsyncSocket::setErrMessageCB(ErrMessageCallback* callback) {
  CHECK(!useIoUring_);
  VLOG(6) << "AsyncSocket::setErrMessageCB() this=" << this << ", fd=" << fd_
          << ", callback=" << callback << ", state=" << state_;

  // Linux Unix domain sockets do not have an error queue. MSG_ERRQUEUE is
  // ignored by AF_UNIX recvmsg(), which can consume normal application data.
  if (callback != nullptr) {
    cacheLocalAddress();
    if (localAddr_.getFamily() == AF_UNIX) {
      LOG(ERROR) << "Failed to set ErrMessageCallback=" << callback
                 << " for Unix Domain Socket where MSG_ERRQUEUE is unsupported,"
                 << " fd=" << fd_;
      return;
    }
  }

  // Short circuit if callback is the same as the existing errMessageCallback_.
  if (callback == errMessageCallback_) {
    return;
  }

  if (!msgErrQueueSupported) {
    // Per-socket error message queue is not supported on this platform.
    return invalidState(callback);
  }

  DestructorGuard dg(this);
  eventBase_->dcheckIsInEventBaseThread();

  if (callback == nullptr) {
    // We should be able to reset the callback regardless of the
    // socket state. It's important to have a reliable callback
    // cancellation mechanism.
    errMessageCallback_ = callback;
    return;
  }

  switch ((StateEnum)state_) {
    case StateEnum::CONNECTING:
    case StateEnum::FAST_OPEN:
    case StateEnum::ESTABLISHED: {
      errMessageCallback_ = callback;
      return;
    }
    case StateEnum::CLOSED:
    case StateEnum::ERROR:
      // We should never reach here.  SHUT_READ should always be set
      // if we are in STATE_CLOSED or STATE_ERROR.
      assert(false);
      return invalidState(callback);
    case StateEnum::UNINIT:
      // We do not allow setReadCallback() to be called before we start
      // connecting.
      return invalidState(callback);
  }

  // We don't put a default case in the switch statement, so that the compiler
  // will warn us to update the switch statement if a new state is added.
  return invalidState(callback);
}

AsyncSocket::ErrMessageCallback* AsyncSocket::getErrMessageCallback() const {
  return errMessageCallback_;
}

void AsyncSocket::setReadAncillaryDataCB(ReadAncillaryDataCallback* callback) {
  CHECK(!useIoUring_);
  VLOG(6) << "AsyncSocket::setReadAncillaryDataCB() this=" << this << ", fd="
          << fd_ << ", callback=" << callback << ", state=" << state_;

  readAncillaryDataCallback_ = callback;
}

AsyncSocket::ReadAncillaryDataCallback*
AsyncSocket::getReadAncillaryDataCallback() const {
  return readAncillaryDataCallback_;
}

void AsyncSocket::setSendMsgParamCB(SendMsgParamsCallback* callback) {
  sendMsgParamCallback_ = callback;
}

AsyncSocket::SendMsgParamsCallback* AsyncSocket::getSendMsgParamsCB() const {
  return sendMsgParamCallback_;
}

void AsyncSocket::setReadCB(ReadCallback* callback) {
  VLOG(6) << "AsyncSocket::setReadCallback() this=" << this << ", fd=" << fd_
          << ", callback=" << callback << ", state=" << state_;

  // Short circuit if callback is the same as the existing readCallback_.
  //
  // Note that this is needed for proper functioning during some cleanup cases.
  // During cleanup we allow setReadCallback(nullptr) to be called even if the
  // read callback is already unset and we have been detached from an event
  // base.  This check prevents us from asserting
  // eventBase_->isInEventBaseThread() when eventBase_ is nullptr.
  if (callback == readCallback_) {
    return;
  }

  /* We are removing a read callback */
  if (callback == nullptr && immediateReadHandler_.isLoopCallbackScheduled()) {
    immediateReadHandler_.cancelLoopCallback();
  }

  DestructorGuard dg(this);

  if (shutdownFlags_ & SHUT_READ) {
    // Reads have already been shut down on this socket.
    //
    // Allow setReadCallback(nullptr) to be called in this case, but don't
    // allow a new callback to be set.
    //
    // For example, setReadCallback(nullptr) can happen after an error if we
    // invoke some other error callback before invoking readError().  The other
    // error callback that is invoked first may go ahead and clear the read
    // callback before we get a chance to invoke readError().
    if (callback != nullptr) {
      return invalidState(callback);
    }
    assert((eventFlags_ & EventHandler::READ) == 0);
    readCallback_ = nullptr;
    return;
  }

  eventBase_->dcheckIsInEventBaseThread();
  // This new callback might support zero copy reads, so reset the
  // zerocopyReadDisabled_ flag to its default value so we will
  // check for support again on the next read attempt.
  zerocopyReadDisabled_ = false;

  switch ((StateEnum)state_) {
    case StateEnum::CONNECTING:
    case StateEnum::FAST_OPEN:
      // For convenience, we allow the read callback to be set while we are
      // still connecting.  We just store the callback for now.  Once the
      // connection completes we'll register for read events.
      readCallback_ = callback;
      return;
    case StateEnum::ESTABLISHED: {
      readCallback_ = callback;
      uint16_t oldFlags = eventFlags_;
      if (readCallback_) {
        eventFlags_ |= EventHandler::READ;
        if (useIoUring_ && !iouRecvHandle_) {
          iouRecvHandle_ =
              IoUringRecvHandle::create(eventBase_, fd_, addr_, this);
          CHECK(iouRecvHandle_);
        } else if (useIoUring_ && iouRecvHandleDetached_) {
          iouRecvHandle_ = IoUringRecvHandle::clone(
              eventBase_, fd_, addr_, this, std::move(iouRecvHandle_));
          CHECK(iouRecvHandle_);
          iouRecvHandleDetached_ = false;
        }
      } else {
        eventFlags_ &= ~EventHandler::READ;
      }

      // Update our registration if our flags have changed
      if (eventFlags_ != oldFlags) {
        // We intentionally ignore the return value here.
        // updateEventRegistration() will move us into the error state if it
        // fails, and we don't need to do anything else here afterwards.
        (void)updateEventRegistration();
      }

      if (readCallback_) {
        checkForImmediateRead();
      }
      return;
    }
    case StateEnum::CLOSED:
    case StateEnum::ERROR:
      // We should never reach here.  SHUT_READ should always be set
      // if we are in STATE_CLOSED or STATE_ERROR.
      assert(false);
      return invalidState(callback);
    case StateEnum::UNINIT:
      // We do not allow setReadCallback() to be called before we start
      // connecting.
      return invalidState(callback);
  }

  // We don't put a default case in the switch statement, so that the compiler
  // will warn us to update the switch statement if a new state is added.
  return invalidState(callback);
}

AsyncSocket::ReadCallback* AsyncSocket::getReadCallback() const {
  return readCallback_;
}

bool AsyncSocket::setZeroCopy(bool enable) {
  if (useIoUring_) {
    zeroCopyVal_ = enable;
    zeroCopyEnabled_ = enable;
    return true;
  }
  if (msgErrQueueSupported) {
    zeroCopyVal_ = enable;

    if (fd_ == NetworkSocket()) {
      return false;
    }

    // No-op, bail out early
    if (enable == zeroCopyEnabled_) {
      return true;
    }

    int val = enable ? 1 : 0;
    int ret =
        netops_->setsockopt(fd_, SOL_SOCKET, SO_ZEROCOPY, &val, sizeof(val));

    // if enable == false, set zeroCopyEnabled_ = false regardless
    // if SO_ZEROCOPY is set or not
    if (!enable) {
      zeroCopyEnabled_ = enable;
      return true;
    }

    /* if the setsockopt failed, try to see if the socket inherited the flag
     * since we cannot set SO_ZEROCOPY on a socket s = accept
     */
    if (ret) {
      val = 0;
      socklen_t optlen = sizeof(val);
      ret = netops_->getsockopt(fd_, SOL_SOCKET, SO_ZEROCOPY, &val, &optlen);

      if (!ret) {
        enable = val != 0;
      }
    }

    if (!ret) {
      zeroCopyEnabled_ = enable;

      return true;
    }
  }

  return false;
}

void AsyncSocket::setZeroCopyEnableFunc(AsyncWriter::ZeroCopyEnableFunc func) {
  zeroCopyEnablePolicy_ = std::move(func);
}

void AsyncSocket::setZeroCopyEnableThreshold(size_t threshold) {
  zeroCopyEnablePolicy_ = threshold;
}

void AsyncSocket::setZeroCopyReenableThreshold(size_t threshold) {
  zeroCopyReenableThreshold_ = threshold;
}

void AsyncSocket::setZeroCopyDrainConfig(const ZeroCopyDrainConfig& config) {
  zeroCopyDrainConfig_ = config;
}

bool AsyncSocket::isZeroCopyRequest(WriteFlags flags) {
  return (zeroCopyEnabled_ && isSet(flags, WriteFlags::WRITE_MSG_ZEROCOPY));
}

void AsyncSocket::adjustZeroCopyFlags(folly::WriteFlags& flags) {
  if (!zeroCopyEnabled_) {
    // if the zeroCopyReenableCounter_ is > 0
    // we try to dec and if we reach 0
    // we set zeroCopyEnabled_ to true
    if (zeroCopyReenableCounter_) {
      if (0 == --zeroCopyReenableCounter_) {
        zeroCopyEnabled_ = true;
        return;
      }
    }
    flags = unSet(flags, folly::WriteFlags::WRITE_MSG_ZEROCOPY);
  }
}

void AsyncSocket::addZeroCopyBuf(
    std::unique_ptr<folly::IOBuf>&& buf, ReleaseIOBufCallback* cb) {
  uint32_t id = getNextZeroCopyBufId();
  folly::IOBuf* ptr = buf.get();

  idZeroCopyBufPtrMap_[id] = ptr;
  auto& p = idZeroCopyBufInfoMap_[ptr];
  p.count_++;
  CHECK(p.buf_.get() == nullptr);
  p.buf_ = std::move(buf);
  p.cb_ = cb;
}

void AsyncSocket::addZeroCopyBuf(folly::IOBuf* ptr) {
  uint32_t id = getNextZeroCopyBufId();
  idZeroCopyBufPtrMap_[id] = ptr;

  idZeroCopyBufInfoMap_[ptr].count_++;
}

void AsyncSocket::releaseZeroCopyBuf(uint32_t id) {
  auto iter = idZeroCopyBufPtrMap_.find(id);
  CHECK(iter != idZeroCopyBufPtrMap_.end());
  auto ptr = iter->second;
  auto iter1 = idZeroCopyBufInfoMap_.find(ptr);
  CHECK(iter1 != idZeroCopyBufInfoMap_.end());
  if (0 == --iter1->second.count_) {
    releaseIOBuf(std::move(iter1->second.buf_), iter1->second.cb_);
    idZeroCopyBufInfoMap_.erase(iter1);
  }

  idZeroCopyBufPtrMap_.erase(iter);
  if (idZeroCopyBufPtrMap_.empty()) {
    if ((state_ == StateEnum::CLOSED || state_ == StateEnum::ERROR) &&
        fd_ != NetworkSocket()) {
      closeNetworkSocket();
    }
  }
}

void AsyncSocket::moveZeroCopyStateFrom(AsyncSocket& other) {
  if (&other == this) {
    return;
  }
  // This transfers only the zero-copy bookkeeping, not the fd itself: the
  // caller performs the fd handoff (e.g. toFDSocket() detaches the fd from the
  // old socket and constructs this one around it) and then calls this to move
  // the pending completion maps. Require that `other`'s fd is already detached
  // so the fd and the zero-copy completion state it belongs to move together,
  // never independently.
  CHECK(other.fd_ == NetworkSocket())
      << "moveZeroCopyStateFrom: source socket still owns its fd";
  // `this` must not already own zero-copy state, or we would drop our own
  // outstanding completions on the move-assign below.
  DCHECK(idZeroCopyBufPtrMap_.empty());
  DCHECK(idZeroCopyBufInfoMap_.empty());

  // The retained IOBufs were counted in `other`'s buffered-bytes accounting at
  // write time; move that accounting along with ownership so both sockets stay
  // consistent (mirrors detachIOBuf, which runs when a completion is reaped).
  for (const auto& [ptr, info] : other.idZeroCopyBufInfoMap_) {
    if (info.buf_) {
      const size_t bytes = info.buf_->computeChainCapacity();
      DCHECK_GE(other.allocatedBytesBuffered_, bytes);
      other.allocatedBytesBuffered_ -= bytes;
      allocatedBytesBuffered_ += bytes;
    }
  }

  idZeroCopyBufPtrMap_ = std::move(other.idZeroCopyBufPtrMap_);
  idZeroCopyBufInfoMap_ = std::move(other.idZeroCopyBufInfoMap_);
}

void AsyncSocket::drainZeroCopyQueue() {
  if (idZeroCopyBufPtrMap_.empty()) {
    return;
  }

  // try to drain ZC writes if any - this is best effort
  handleErrMessages();

  // Enable SO_LINGER if requested.
  if (!idZeroCopyBufPtrMap_.empty() &&
      zeroCopyDrainConfig_.linger.has_value()) {
    struct linger optLinger = {1, zeroCopyDrainConfig_.linger.value()};
    if (setSockOpt(SOL_SOCKET, SO_LINGER, &optLinger) != 0) {
      VLOG(2) << "AsyncSocket::drainZeroCopyQueue(): error setting SO_LINGER "
              << "on " << fd_ << ": errno=" << errno;
    }
  }
}

void AsyncSocket::scheduleZeroCopyDrain() {
  if (idZeroCopyBufPtrMap_.empty()) {
    return;
  }
  auto* FOLLY_NONNULL eventBase = CHECK_NOTNULL(eventBase_);
  eventBase->scheduleAt(
      [this,
       guard = DestructorGuard(this),
       keepAlive = getKeepAliveToken(*eventBase)]() mutable {
        handleErrMessages();
        if (idZeroCopyBufPtrMap_.empty()) {
          return;
        }
        if (std::chrono::steady_clock::now() >= zeroCopyDrainDeadline_) {
          forceReleaseZeroCopyBufs();
          return;
        }
        scheduleZeroCopyDrain();
      },
      std::chrono::steady_clock::now() + zeroCopyDrainConfig_.drainDelay);
}

void AsyncSocket::forceReleaseZeroCopyBufs() {
  LOG(ERROR)
      << "AsyncSocket::forceReleaseZeroCopyBufs(this=" << this << ", fd=" << fd_
      << "): giving up after " << zeroCopyDrainConfig_.maxDrainDuration.count()
      << "ms waiting for " << idZeroCopyBufPtrMap_.size()
      << " MSG_ZEROCOPY completion(s); releasing "
      << idZeroCopyBufInfoMap_.size()
      << " buffer(s) the kernel may still reference";

  while (!idZeroCopyBufPtrMap_.empty()) {
    releaseZeroCopyBuf(idZeroCopyBufPtrMap_.begin()->first);
  }
  closeNetworkSocket();
}

void AsyncSocket::setZeroCopyBuf(
    std::unique_ptr<folly::IOBuf>&& buf, ReleaseIOBufCallback* cb) {
  folly::IOBuf* ptr = buf.get();
  auto iter = idZeroCopyBufInfoMap_.find(ptr);
  CHECK(iter != idZeroCopyBufInfoMap_.end());
  auto& p = iter->second;
  CHECK(p.buf_.get() == nullptr);

  p.buf_ = std::move(buf);
  p.cb_ = cb;
}

bool AsyncSocket::containsZeroCopyBuf(folly::IOBuf* ptr) {
  return (idZeroCopyBufInfoMap_.find(ptr) != idZeroCopyBufInfoMap_.end());
}

bool AsyncSocket::isZeroCopyMsg(const cmsghdr& cmsg) const {
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  if ((cmsg.cmsg_level == SOL_IP && cmsg.cmsg_type == IP_RECVERR) ||
      (cmsg.cmsg_level == SOL_IPV6 && cmsg.cmsg_type == IPV6_RECVERR)) {
    auto serr =
        reinterpret_cast<const struct sock_extended_err*>(CMSG_DATA(&cmsg));
    return (
        (serr->ee_errno == 0) && (serr->ee_origin == SO_EE_ORIGIN_ZEROCOPY));
  }
#endif
  (void)cmsg;
  return false;
}

void AsyncSocket::processZeroCopyMsg(const cmsghdr& cmsg) {
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  auto serr =
      reinterpret_cast<const struct sock_extended_err*>(CMSG_DATA(&cmsg));
  uint32_t hi = serr->ee_data;
  uint32_t lo = serr->ee_info;
  // disable zero copy if the buffer was actually copied
  if ((serr->ee_code & SO_EE_CODE_ZEROCOPY_COPIED) && zeroCopyEnabled_) {
    VLOG(2) << "AsyncSocket::processZeroCopyMsg(): setting "
            << "zeroCopyEnabled_ = false due to SO_EE_CODE_ZEROCOPY_COPIED "
            << "on " << fd_;
    zeroCopyEnabled_ = false;
  }

  for (uint32_t i = lo; i <= hi; i++) {
    releaseZeroCopyBuf(i);
  }
#else
  (void)cmsg;
#endif
}

void AsyncSocket::detachIOBuf(const folly::IOBuf& buf) {
  size_t bytes = buf.computeChainCapacity();
  DCHECK_GE(allocatedBytesBuffered_, bytes);
  allocatedBytesBuffered_ -= bytes;
}

void AsyncSocket::releaseIOBuf(
    std::unique_ptr<folly::IOBuf> buf, ReleaseIOBufCallback* callback) {
  if (!buf) {
    return;
  }
  detachIOBuf(*buf);
  if (callback) {
    callback->releaseIOBuf(std::move(buf));
  }
}

void AsyncSocket::enableByteEvents() {
  if (!byteEventHelper_) {
    byteEventHelper_ = std::make_unique<ByteEventHelper>();
  }

  if (byteEventHelper_->byteEventsEnabled ||
      byteEventHelper_->maybeEx.has_value()) {
    return;
  }

  try {
#if FOLLY_HAVE_SO_TIMESTAMPING
    if (useIoUring_) {
      throw AsyncSocketException(
          AsyncSocketException::NOT_SUPPORTED,
          withAddr(
              "failed to enable byte events: "
              "EVB is using io_uring"));
    }

    // make sure we have a connected IP socket that supports error queues
    // (Unix sockets do not support error queues)
    if (NetworkSocket() == fd_ || !good()) {
      throw AsyncSocketException(
          AsyncSocketException::INVALID_STATE,
          withAddr(
              "failed to enable byte events: "
              "socket is not open or not in a good state"));
    }
    folly::SocketAddress addr = {};
    try {
      // explicitly fetch local address (instead of using cache)
      // to ensure socket is currently healthy
      addr.setFromLocalAddress(fd_);
    } catch (const std::system_error&) {
      throw AsyncSocketException(
          AsyncSocketException::INVALID_STATE,
          withAddr(
              "failed to enable byte events: "
              "socket is not open or not in a good state"));
    }
    const auto family = addr.getFamily();
    if (family != AF_INET && family != AF_INET6) {
      throw AsyncSocketException(
          AsyncSocketException::NOT_SUPPORTED,
          withAddr("failed to enable byte events: socket type not supported"));
    }

    // check if timestamping is already enabled on the socket by another source
    {
      uint32_t flags = 0;
      socklen_t len = sizeof(flags);
      const auto ret =
          getSockOptVirtual(SOL_SOCKET, SO_TIMESTAMPING, &flags, &len);
      int getSockOptErrno = errno;
      if (0 != ret) {
        throw AsyncSocketException(
            AsyncSocketException::INTERNAL_ERROR,
            withAddr(
                "failed to enable byte events: "
                "timestamps may not be supported for this socket type "
                "or socket be closed"),
            getSockOptErrno);
      }
      if (0 != flags) {
        throw AsyncSocketException(
            AsyncSocketException::INTERNAL_ERROR,
            withAddr(
                "failed to enable byte events: "
                "timestamps may have already been enabled"),
            getSockOptErrno);
      }
    }

    // enable control messages for software and hardware timestamps
    // WriteFlags will determine which messages are generated
    //
    // SOF_TIMESTAMPING_OPT_ID: see discussion in ByteEventHelper::processCmsg
    // SOF_TIMESTAMPING_OPT_TSONLY: only get timestamps, not original packet
    // SOF_TIMESTAMPING_SOFTWARE: get software timestamps if generated
    // SOF_TIMESTAMPING_RAW_HARDWARE: get hardware timestamps if generated
    // SOF_TIMESTAMPING_OPT_TX_SWHW: get both sw + hw timestamps if generated
    const uint32_t flags =
        (folly::netops::SOF_TIMESTAMPING_OPT_ID |
         folly::netops::SOF_TIMESTAMPING_OPT_TSONLY |
         folly::netops::SOF_TIMESTAMPING_SOFTWARE |
         folly::netops::SOF_TIMESTAMPING_RAW_HARDWARE |
         folly::netops::SOF_TIMESTAMPING_OPT_TX_SWHW);
    socklen_t len = sizeof(flags);

    size_t byteEventsEnabledMaxAttempts = 0;
    int setSockOptErrno = errno;

    // When enabling byte events, the kernel resets the offset it uses for
    // timestamps to 0 (see discussion in ByteEventHelper::processCmsg). By
    // keeping track of the AsyncSocket raw byte offset when byte events were
    // enabled, we can align the kernel and AsyncSocket byte offsets for future
    // byte events.
    //
    // However, the kernel offset tracks the last unacknowledged byte, not the
    // last written byte. This prevents us from aligning AsyncSocket's raw byte
    // offset with the kernel offset in two scenarios:
    //
    // (1) If the kernel is still sending (packetizing) the bytes written before
    //     enabling byte events, then the kernel offset is reset before all
    //     bytes written to the kernel by AsyncSocket are sent.
    //
    // (2) If there are unacknowledged bytes in the TCP send buffer, because the
    //     kernel offset tracks the last unacknowledged byte, not the last
    //     written byte, the offset will end up being off.
    //
    // Linux provides SOF_TIMESTAMPING_OPT_ID_TCP to initialize the offset from
    // write_seq (written bytes) instead of snd_una (unacknowledged bytes) [1].
    // AsyncSocket retains the following correction for kernels that predate
    // that option.
    //
    // (1) We record the number of sent bytes and unacknowledged bytes before
    //     and after enabling byte events. If they change, we disable and
    //     re-enable. We repeat the process for a fixed number of times or until
    //     the numbers before and after do not change. This fix is meant to
    //     ensure that we can record the number of unacknowledged bytes at the
    //     moment when we reset the kernel's timestamp offset.
    //
    // (2) We adjust the AsyncSocket byte offset when byte events were enabled
    //     by the number of unacknowledged bytes at that point. Per (1) above,
    //     we know that we captured the number of unacknowledged bytes when the
    //     kernel's timestamp offset was reset, and thus can confidently adjust
    //     offsets reported by the kernel going forward.
    //
    // [1]
    // https://github.com/torvalds/linux/commit/b534dc46c8ae0165b1b2509be24dbea4fa9c4011

    while (byteEventsEnabledMaxAttempts++ < kMaxAttemptsEnableByteEvents) {
      folly::TcpInfo::LookupOptions options = {};
      options.getMemInfo = true;
      const auto expectTInfoBefore = getTcpInfo(options);
      const auto ret =
          setSockOptVirtual(SOL_SOCKET, SO_TIMESTAMPING, &flags, len);
      const auto expectTInfoAfter = getTcpInfo(options);

      if (ret != 0) {
        throw AsyncSocketException(
            AsyncSocketException::INTERNAL_ERROR,
            withAddr("failed to enable byte events: setsockopt failed"),
            setSockOptErrno);
      }

      if (!expectTInfoBefore.hasValue() || !expectTInfoAfter.hasValue()) {
        throw AsyncSocketException(
            AsyncSocketException::INTERNAL_ERROR,
            withAddr("failed to enable byte events: getTcpInfo failed"),
            setSockOptErrno);
      }

      const auto tInfoBefore = expectTInfoBefore.value();
      const auto tInfoAfter = expectTInfoAfter.value();

      if (tInfoBefore.bytesSent() != tInfoAfter.bytesSent() ||
          tInfoBefore.sendBufInUseBytes() != tInfoAfter.sendBufInUseBytes() ||
          !tInfoAfter.sendBufInUseBytes().has_value()) {
        const uint32_t disableFlag = 0;
        const auto disableReturnValue = setSockOptVirtual(
            SOL_SOCKET, SO_TIMESTAMPING, &disableFlag, sizeof(disableFlag));
        if (disableReturnValue != 0) {
          throw AsyncSocketException(
              AsyncSocketException::INTERNAL_ERROR,
              withAddr(
                  "error when enabling byte events: "
                  "failed to disable byte events after byte sent counters not matching"),
              setSockOptErrno);
        }
      } else {
        const auto rawBytesWritten = getRawBytesWritten();
        const auto bytesNotAcknowledged =
            tInfoAfter.sendBufInUseBytes().value();

        byteEventHelper_->byteEventsEnabled = true;
        // it is possible for rawBytesWrittenWhenByteEventsEnabled to be
        // negative if bytes were written to the underlying socket before
        // this AsyncSocket was constructed
        byteEventHelper_->rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWritten - bytesNotAcknowledged;

        for (const auto& observer : lifecycleObservers_) {
          if (observer->getConfig().byteEvents) {
            observer->byteEventsEnabled(this);
          }
        }
        return;
      }
    }

    if (byteEventsEnabledMaxAttempts > kMaxAttemptsEnableByteEvents) {
      throw AsyncSocketException(
          AsyncSocketException::INTERNAL_ERROR,
          withAddr(
              "failed to enable byte events: "
              "could not account for bytes in flight in kernel byte offset"),
          setSockOptErrno);
    }
#endif // FOLLY_HAVE_SO_TIMESTAMPING
    // unsupported by platform
    throw AsyncSocketException(
        AsyncSocketException::NOT_SUPPORTED,
        withAddr("failed to enable byte events: platform not supported"));

  } catch (const AsyncSocketException& ex) {
    failByteEvents(ex);
  }
}

void AsyncSocket::write(
    WriteCallback* callback, const void* buf, size_t bytes, WriteFlags flags) {
  iovec op{};
  op.iov_base = const_cast<void*>(buf);
  op.iov_len = bytes;
  writeImpl(callback, &op, 1, unique_ptr<IOBuf>(), bytes, flags);
}

void AsyncSocket::writev(
    WriteCallback* callback, const iovec* vec, size_t count, WriteFlags flags) {
  size_t totalBytes = 0;
  for (size_t i = 0; i < count; ++i) {
    totalBytes += vec[i].iov_len;
  }
  writeImpl(callback, vec, count, unique_ptr<IOBuf>(), totalBytes, flags);
}

void AsyncSocket::writeChain(
    WriteCallback* callback, unique_ptr<IOBuf>&& buf, WriteFlags flags) {
  adjustZeroCopyFlags(flags);

  size_t count = buf->countChainElements();
  if (count <= kSmallIoVecSize) {
    // suppress "warning: variable length array 'vec' is used [-Wvla]"
    FOLLY_PUSH_WARNING
    FOLLY_GNU_DISABLE_WARNING("-Wvla")
    iovec vec[FOLLY_DETAIL_VLA_SIZE(count, kSmallIoVecSize)];
    FOLLY_POP_WARNING

    writeChainImpl(callback, vec, count, std::move(buf), flags);
  } else {
    std::unique_ptr<iovec[]> vec(new iovec[count]);
    writeChainImpl(callback, vec.get(), count, std::move(buf), flags);
  }
}

void AsyncSocket::writeChainImpl(
    WriteCallback* callback,
    iovec* vec,
    size_t count,
    unique_ptr<IOBuf>&& buf,
    WriteFlags flags) {
  auto res = buf->fillIov(vec, count);
  if (zeroCopyEnabled_ && !isSet(flags, WriteFlags::WRITE_MSG_ZEROCOPY) &&
      buf->isManaged()) {
    bool enableZeroCopy = std::visit(
        folly::overload(
            [](std::monostate) { return false; },
            [&buf](const AsyncWriter::ZeroCopyEnableFunc& func) {
              return func && func(buf);
            },
            [totalLength = res.totalLength](size_t threshold) {
              return threshold > 0 && totalLength >= threshold;
            }),
        zeroCopyEnablePolicy_);
    if (enableZeroCopy) {
      flags |= WriteFlags::WRITE_MSG_ZEROCOPY;
    }
  }
  writeImpl(
      callback, vec, res.numIovecs, std::move(buf), res.totalLength, flags);
}

void AsyncSocket::writeImpl(
    WriteCallback* callback,
    const iovec* vec,
    size_t count,
    unique_ptr<IOBuf>&& buf,
    size_t totalBytes,
    WriteFlags flags) {
  VLOG(6) << "AsyncSocket::writev() this=" << this << ", fd=" << fd_
          << ", callback=" << callback << ", count=" << count
          << ", state=" << state_;
  DestructorGuard dg(this);
  unique_ptr<IOBuf> ioBuf(std::move(buf));
  eventBase_->dcheckIsInEventBaseThread();
  WriteCallbackWithState callbackWithState(callback);

  auto* releaseIOBufCallback =
      callback ? callback->getReleaseIOBufCallback() : nullptr;

  SCOPE_EXIT {
    releaseIOBuf(std::move(ioBuf), releaseIOBufCallback);
  };

  totalAppBytesScheduledForWrite_ += totalBytes;
  if (ioBuf) {
    allocatedBytesBuffered_ += ioBuf->computeChainCapacity();
  }

  if (shutdownFlags_ & (SHUT_WRITE | SHUT_WRITE_PENDING)) {
    // No new writes may be performed after the write side of the socket has
    // been shutdown.
    //
    // We could just call callback->writeError() here to fail just this write.
    // However, fail hard and use invalidState() to fail all outstanding
    // callbacks and move the socket into the error state.  There's most likely
    // a bug in the caller's code, so we abort everything rather than trying to
    // proceed as best we can.
    return invalidState(callback);
  }

  uint32_t countWritten = 0;
  uint32_t partialWritten = 0;
  ssize_t bytesWritten = 0;
  bool mustRegister = false;
  if ((state_ == StateEnum::ESTABLISHED || state_ == StateEnum::FAST_OPEN) &&
      !connecting()) {
    if (!hasPendingWrites()) {
      // If we are established and there are no other writes pending,
      // we can attempt to perform the write immediately.
      assert(writeReqTail_ == nullptr);
      assert(iouSendHandle_ ? iouSendHandle_->empty() : true);
      assert((eventFlags_ & EventHandler::WRITE) == 0);

      callbackWithState.notifyOnWrite();

      if (useIoUring_ && isZeroCopyRequest(flags) &&
          state_ != StateEnum::FAST_OPEN) {
        mustRegister = true;
      } else {
        // For io_uring + zero-copy + TFO, strip the zero-copy flag on the first
        // (FAST_OPEN) write: the TFO SYN-data probe has to be a synchronous
        // sendmsg while io_uring SEND_ZC is async. A copy here is fine; later
        // writes use SEND_ZC once ESTABLISHED. This only applies to io_uring --
        // a non-io_uring TFO socket keeps its zero-copy semantics.
        if (useIoUring_ && isZeroCopyRequest(flags) &&
            state_ == StateEnum::FAST_OPEN) {
          flags = unSet(flags, folly::WriteFlags::WRITE_MSG_ZEROCOPY);
        }
        auto writeResult = performWrite(
            vec,
            uint32_t(count),
            flags,
            &countWritten,
            &partialWritten,
            WriteRequestTag{ioBuf.get()});
        bytesWritten = writeResult.writeReturn;
        if (bytesWritten < 0) {
          auto errnoCopy = errno;
          if (writeResult.exception) {
            return failWrite(__func__, callback, 0, *writeResult.exception);
          }
          AsyncSocketException ex(
              AsyncSocketException::INTERNAL_ERROR,
              withAddr("writev failed"),
              errnoCopy);
          return failWrite(__func__, callback, 0, ex);
        } else if (countWritten == count) {
          // done, add the whole buffer
          if (countWritten && isZeroCopyRequest(flags)) {
            addZeroCopyBuf(std::move(ioBuf), releaseIOBufCallback);
          } else {
            releaseIOBuf(std::move(ioBuf), releaseIOBufCallback);
          }

          // We successfully wrote everything.
          // Invoke the callback and return.
          if (callback) {
            callback->writeSuccess();
          }
          return;
        } else { // continue writing the next writeReq
          // add just the ptr
          if (bytesWritten && isZeroCopyRequest(flags)) {
            addZeroCopyBuf(ioBuf.get());
          }
        }
        if (!connecting()) {
          // Writes might put the socket back into connecting state
          // if TFO is enabled, and using TFO fails.
          // This means that write timeouts would not be active, however
          // connect timeouts would affect this stage.
          mustRegister = true;
        }
      }
    }
  } else if (!connecting()) {
    // Invalid state for writing
    return invalidState(callback);
  }

  if (!useIoUring_) {
    // Create a new WriteRequest to add to the queue
    WriteRequest* req;
    try {
      req = BytesWriteRequest::newRequest(
          this,
          callbackWithState,
          vec + countWritten,
          uint32_t(count - countWritten),
          partialWritten,
          uint32_t(bytesWritten),
          std::move(ioBuf),
          flags);
    } catch (const std::exception& ex) {
      // we mainly expect to catch std::bad_alloc here
      AsyncSocketException tex(
          AsyncSocketException::INTERNAL_ERROR,
          withAddr(string("failed to append new WriteRequest: ") + ex.what()));
      return failWrite(__func__, callback, size_t(bytesWritten), tex);
    }
    req->consume();
    if (writeReqTail_ == nullptr) {
      assert(writeReqHead_ == nullptr);
      writeReqHead_ = writeReqTail_ = req;
    } else {
      writeReqTail_->append(req);
      writeReqTail_ = req;
    }
  } else {
    auto cb = callback ? callback->getReleaseIOBufCallback() : nullptr;
    if (ioBuf) {
      for (uint32_t i = countWritten; i != 0; --i) {
        assert(ioBuf);
        auto next = ioBuf->pop();
        releaseIOBuf(std::move(ioBuf), cb);
        ioBuf = std::move(next);
      }
    }
    iouSendHandle_->write(
        callbackWithState,
        vec + countWritten,
        count - countWritten,
        partialWritten,
        bytesWritten,
        std::move(ioBuf),
        flags);
  }

  if (bufferCallback_) {
    bufferCallback_->onEgressBuffered();
  }

  // Register for write events if are established and not currently
  // waiting on write events
  if (mustRegister) {
    assert(state_ == StateEnum::ESTABLISHED);
    assert((eventFlags_ & EventHandler::WRITE) == 0);
    if (!updateEventRegistration(EventHandler::WRITE, 0)) {
      assert(state_ == StateEnum::ERROR);
      return;
    }
    if (sendTimeout_ > 0) {
      // Schedule a timeout to fire if the write takes too long.
      if (!writeTimeout_.scheduleTimeout(sendTimeout_)) {
        AsyncSocketException ex(
            AsyncSocketException::INTERNAL_ERROR,
            withAddr("failed to schedule send timeout"));
        return failWrite(__func__, ex);
      }
    }
  }
}

void AsyncSocket::writeRequest(WriteRequest* req) {
  if (writeReqTail_ == nullptr) {
    assert(writeReqHead_ == nullptr);
    writeReqHead_ = writeReqTail_ = req;
    req->start();
  } else {
    writeReqTail_->append(req);
    writeReqTail_ = req;
  }
}

void AsyncSocket::close() {
  VLOG(5) << "AsyncSocket::close(): this=" << this << ", fd_=" << fd_
          << ", state=" << state_ << ", shutdownFlags=" << std::hex
          << (int)shutdownFlags_;

  // close() is only different from closeNow() when there are pending writes
  // that need to drain before we can close.  In all other cases, just call
  // closeNow().
  //
  // Note that writeReqHead_ can be non-nullptr even in STATE_CLOSED or
  // STATE_ERROR if close() is invoked while a previous closeNow() or failure
  // is still running.  (e.g., If there are multiple pending writes, and we
  // call writeError() on the first one, it may call close().  In this case we
  // will already be in STATE_CLOSED or STATE_ERROR, but the remaining pending
  // writes will still be in the queue.)
  //
  // We only need to drain pending writes if we are still in STATE_CONNECTING
  // or STATE_ESTABLISHED
  if ((!hasPendingWrites()) ||
      !(state_ == StateEnum::CONNECTING || state_ == StateEnum::ESTABLISHED)) {
    closeNow();
    return;
  }

  // Declare a DestructorGuard to ensure that the AsyncSocket cannot be
  // destroyed until close() returns.
  DestructorGuard dg(this);
  eventBase_->dcheckIsInEventBaseThread();

  // Since there are write requests pending, we have to set the
  // SHUT_WRITE_PENDING flag, and wait to perform the real close until the
  // connect finishes and we finish writing these requests.
  //
  // Set SHUT_READ to indicate that reads are shut down, and set the
  // SHUT_WRITE_PENDING flag to mark that we want to shutdown once the
  // pending writes complete.
  shutdownFlags_ |= (SHUT_READ | SHUT_WRITE_PENDING);

  // If a read callback is set, invoke readEOF() immediately to inform it that
  // the socket has been closed and no more data can be read.
  if (readCallback_) {
    // Disable reads if they are enabled
    if (!updateEventRegistration(0, EventHandler::READ)) {
      // We're now in the error state; callbacks have been cleaned up
      assert(state_ == StateEnum::ERROR);
      assert(readCallback_ == nullptr);
    } else {
      ReadCallback* callback = readCallback_;
      readCallback_ = nullptr;
      callback->readEOF();
    }
  }
}

void AsyncSocket::closeNow() {
  VLOG(5) << "AsyncSocket::closeNow(): this=" << this << ", fd_=" << fd_
          << ", state=" << state_ << ", shutdownFlags=" << std::hex
          << (int)shutdownFlags_;
  DestructorGuard dg(this);
  if (eventBase_) {
    eventBase_->dcheckIsInEventBaseThread();
  }

  if (iouRecvHandle_) {
    if (!iouRecvHandleDetached_) {
      iouRecvHandle_->cancel();
    }
    iouRecvHandle_.reset();
    iouRecvHandleDetached_ = false;
  }

  switch (state_) {
    case StateEnum::ESTABLISHED:
    case StateEnum::CONNECTING:
    case StateEnum::FAST_OPEN: {
      shutdownFlags_ |= (SHUT_READ | SHUT_WRITE);
      state_ = StateEnum::CLOSED;

      // If the write timeout was set, cancel it.
      writeTimeout_.cancelTimeout();

      // If we are registered for I/O events, unregister.
      if (eventFlags_ != EventHandler::NONE) {
        eventFlags_ = EventHandler::NONE;
        if (!updateEventRegistration()) {
          // We will have been moved into the error state.
          assert(state_ == StateEnum::ERROR);
          return;
        }
      }

      if (immediateReadHandler_.isLoopCallbackScheduled()) {
        immediateReadHandler_.cancelLoopCallback();
      }

      if (fd_ != NetworkSocket()) {
        ioHandler_.changeHandlerFD(NetworkSocket());
        doClose();
      }

      invokeConnectErr(getSocketClosedLocallyEx());
      if (iouConnectHandle_) {
        if (iouConnectHandle_->cancel()) {
          iouConnectHandle_.release();
        } else {
          iouConnectHandle_.reset();
        }
      }

      failAllWrites(getSocketClosedLocallyEx());

      if (readCallback_) {
        ReadCallback* callback = readCallback_;
        readCallback_ = nullptr;
        callback->readEOF();
      }
      return;
    }
    case StateEnum::CLOSED:
      // Do nothing.  It's possible that we are being called recursively
      // from inside a callback that we invoked inside another call to close()
      // that is still running.
      return;
    case StateEnum::ERROR:
      // Do nothing.  The error handling code has performed (or is performing)
      // cleanup.
      return;
    case StateEnum::UNINIT:
      assert(eventFlags_ == EventHandler::NONE);
      assert(connectCallback_ == nullptr);
      assert(readCallback_ == nullptr);
      assert(!hasPendingWrites());
      shutdownFlags_ |= (SHUT_READ | SHUT_WRITE);
      state_ = StateEnum::CLOSED;
      return;
  }

  LOG(DFATAL) << "AsyncSocket::closeNow() (this=" << this << ", fd=" << fd_
              << ") called in unknown state " << state_;
}

void AsyncSocket::closeWithReset() {
  // Enable SO_LINGER, with the linger timeout set to 0.
  // This will trigger a TCP reset when we close the socket.
  if (fd_ != NetworkSocket()) {
    struct linger optLinger = {1, 0};
    if (setSockOpt(SOL_SOCKET, SO_LINGER, &optLinger) != 0) {
      VLOG(2) << "AsyncSocket::closeWithReset(): error setting SO_LINGER "
              << "on " << fd_ << ": errno=" << errno;
    }
  }

  // Then let closeNow() take care of the rest
  closeNow();
}

void AsyncSocket::shutdownWrite() {
  VLOG(5) << "AsyncSocket::shutdownWrite(): this=" << this << ", fd=" << fd_
          << ", state=" << state_ << ", shutdownFlags=" << std::hex
          << (int)shutdownFlags_;

  // If there are no pending writes, shutdownWrite() is identical to
  // shutdownWriteNow().
  if (!hasPendingWrites()) {
    shutdownWriteNow();
    return;
  }

  eventBase_->dcheckIsInEventBaseThread();

  // There are pending writes.  Set SHUT_WRITE_PENDING so that the actual
  // shutdown will be performed once all writes complete.
  shutdownFlags_ |= SHUT_WRITE_PENDING;
}

void AsyncSocket::shutdownWriteNow() {
  VLOG(5) << "AsyncSocket::shutdownWriteNow(): this=" << this << ", fd=" << fd_
          << ", state=" << state_ << ", shutdownFlags=" << std::hex
          << (int)shutdownFlags_;

  if (shutdownFlags_ & SHUT_WRITE) {
    // Writes are already shutdown; nothing else to do.
    return;
  }

  // If SHUT_READ is already set, just call closeNow() to completely
  // close the socket.  This can happen if close() was called with writes
  // pending, and then shutdownWriteNow() is called before all pending writes
  // complete.
  if (shutdownFlags_ & SHUT_READ) {
    closeNow();
    return;
  }

  DestructorGuard dg(this);
  if (eventBase_) {
    eventBase_->dcheckIsInEventBaseThread();
  }

  switch (static_cast<StateEnum>(state_)) {
    case StateEnum::ESTABLISHED: {
      shutdownFlags_ |= SHUT_WRITE;

      // If the write timeout was set, cancel it.
      writeTimeout_.cancelTimeout();

      // If we are registered for write events, unregister.
      if (!updateEventRegistration(0, EventHandler::WRITE)) {
        // We will have been moved into the error state.
        assert(state_ == StateEnum::ERROR);
        return;
      }

      // Shutdown writes on the file descriptor
      netops_->shutdown(fd_, SHUT_WR);

      // Immediately fail all write requests
      failAllWrites(getSocketShutdownForWritesEx());
      return;
    }
    case StateEnum::CONNECTING: {
      // Set the SHUT_WRITE_PENDING flag.
      // When the connection completes, it will check this flag,
      // shutdown the write half of the socket, and then set SHUT_WRITE.
      shutdownFlags_ |= SHUT_WRITE_PENDING;

      // Immediately fail all write requests
      failAllWrites(getSocketShutdownForWritesEx());
      return;
    }
    case StateEnum::UNINIT:
      // Callers normally shouldn't call shutdownWriteNow() before the socket
      // even starts connecting.  Nonetheless, go ahead and set
      // SHUT_WRITE_PENDING.  Once the socket eventually connects it will
      // immediately shut down the write side of the socket.
      shutdownFlags_ |= SHUT_WRITE_PENDING;
      return;
    case StateEnum::FAST_OPEN:
      // In fast open state we haven't call connected yet, and if we shutdown
      // the writes, we will never try to call connect, so shut everything down
      shutdownFlags_ |= SHUT_WRITE;
      // Immediately fail all write requests
      failAllWrites(getSocketShutdownForWritesEx());
      return;
    case StateEnum::CLOSED:
    case StateEnum::ERROR:
      // We should never get here.  SHUT_WRITE should always be set
      // in STATE_CLOSED and STATE_ERROR.
      VLOG(4)
          << "AsyncSocket::shutdownWriteNow() (this=" << this << ", fd=" << fd_
          << ") in unexpected state " << state_ << " with SHUT_WRITE not set ("
          << std::hex << (int)shutdownFlags_ << ")";
      assert(false);
      return;
  }

  LOG(DFATAL) << "AsyncSocket::shutdownWriteNow() (this=" << this
              << ", fd=" << fd_ << ") called in unknown state " << state_;
}

bool AsyncSocket::readable() const {
  if (fd_ == NetworkSocket()) {
    return false;
  }

  if (preReceivedData_ && !preReceivedData_->empty()) {
    return true;
  }
  netops::PollDescriptor fds[1];
  fds[0].fd = fd_;
  fds[0].events = POLLIN;
  fds[0].revents = 0;
  int rc = netops_->poll(fds, 1, 0);
  return rc == 1;
}

bool AsyncSocket::writable() const {
  if (fd_ == NetworkSocket()) {
    return false;
  }
  netops::PollDescriptor fds[1];
  fds[0].fd = fd_;
  fds[0].events = POLLOUT;
  fds[0].revents = 0;
  int rc = netops_->poll(fds, 1, 0);
  return rc == 1;
}

bool AsyncSocket::isPending() const {
  return ioHandler_.isPending();
}

bool AsyncSocket::hangup() const {
  if (fd_ == NetworkSocket()) {
    // sanity check, no one should ask for hangup if we are not connected.
    assert(false);
    return false;
  }
#ifdef POLLRDHUP // Linux-only
  netops::PollDescriptor fds[1];
  fds[0].fd = fd_;
  fds[0].events = POLLRDHUP | POLLHUP;
  fds[0].revents = 0;
  netops_->poll(fds, 1, 0);
  return (fds[0].revents & (POLLRDHUP | POLLHUP)) != 0;
#else
  return false;
#endif
}

bool AsyncSocket::good() const {
  return (
      (state_ == StateEnum::CONNECTING || state_ == StateEnum::FAST_OPEN ||
       state_ == StateEnum::ESTABLISHED) &&
      (shutdownFlags_ == 0) && (eventBase_ != nullptr));
}

bool AsyncSocket::error() const {
  return (state_ == StateEnum::ERROR);
}

void AsyncSocket::attachEventBase(EventBase* eventBase) {
  VLOG(5)
      << "AsyncSocket::attachEventBase(this=" << this << ", fd=" << fd_
      << ", old evb=" << eventBase_ << ", new evb=" << eventBase
      << ", state=" << state_ << ", events=" << std::hex << eventFlags_ << ")";
  assert(eventBase_ == nullptr);
  eventBase->dcheckIsInEventBaseThread();

  eventBase_ = eventBase;
  if (useIoUring_) {
    if (eventFlags_ & EventHandler::READ) {
      CHECK(iouRecvHandle_ != nullptr);
      CHECK(readCallback_ != nullptr);
      iouRecvHandle_ = IoUringRecvHandle::clone(
          eventBase, fd_, addr_, this, std::move(iouRecvHandle_));
      iouRecvHandleDetached_ = false;
    }
    if (iouSendHandle_) {
      iouSendHandle_ =
          IoUringSendHandle::clone(eventBase, std::move(iouSendHandle_));
    }
  }

  ioHandler_.attachEventBase(eventBase);

  updateEventRegistration();

  writeTimeout_.attachEventBase(eventBase);
  if (evbChangeCb_) {
    evbChangeCb_->evbAttached(this);
  }

  // legacy observer support
  for (const auto& cb : lifecycleObservers_) {
    cb->evbAttach(this, eventBase_);
  }

  // folly::ObserverContainer observer support
  if (auto list = getAsyncSocketObserverContainer()) {
    list->invokeInterfaceMethodAllObservers([&](auto observer, auto observed) {
      observer->evbAttach(observed, eventBase_);
    });
  }
}

void AsyncSocket::detachEventBase() {
  VLOG(5) << "AsyncSocket::detachEventBase(this=" << this << ", fd=" << fd_
          << ", old evb=" << eventBase_ << ", state=" << state_
          << ", events=" << std::hex << eventFlags_ << ")";
  assert(eventBase_ != nullptr);
  eventBase_->dcheckIsInEventBaseThread();

  // Make a copy of the existing event base, to invoke lifecycle observer
  // callbacks
  EventBase* existingEvb = eventBase_;

  eventBase_ = nullptr;

  if (useIoUring_) {
    if (iouRecvHandle_ && !iouRecvHandleDetached_) {
      iouRecvHandle_->detachEventBase();
      iouRecvHandleDetached_ = true;
    }

    if (iouSendHandle_) {
      iouSendHandle_->update(EventHandler::NONE);
      iouSendHandle_->detachEventBase();
    }
  }

  ioHandler_.unregisterHandler();

  ioHandler_.detachEventBase();
  writeTimeout_.detachEventBase();
  if (evbChangeCb_) {
    evbChangeCb_->evbDetached(this);
  }

  // legacy observer support
  for (const auto& cb : lifecycleObservers_) {
    cb->evbDetach(this, existingEvb);
  }

  // folly::ObserverContainer observer support
  if (auto list = getAsyncSocketObserverContainer()) {
    list->invokeInterfaceMethodAllObservers(
        [existingEvb](auto observer, auto observed) {
          observer->evbDetach(observed, existingEvb);
        });
  }
}

bool AsyncSocket::isDetachable() const {
  DCHECK(eventBase_ != nullptr);
  eventBase_->dcheckIsInEventBaseThread();

  return !writeTimeout_.isScheduled() && idZeroCopyBufPtrMap_.empty();
}

void AsyncSocket::cacheAddresses() {
  if (fd_ != NetworkSocket()) {
    try {
      cacheLocalAddress();
      cachePeerAddress();
    } catch (const std::system_error& e) {
      if (e.code() !=
          std::error_code(ENOTCONN, errorCategoryForErrnoDomain())) {
        VLOG(2) << "Error caching addresses: " << e.code().value() << ", "
                << e.code().message();
      }
    }
  }
}

void AsyncSocket::cacheLocalAddress() const {
  if (!localAddr_.isInitialized() && fd_ != NetworkSocket()) {
    localAddr_.setFromLocalAddress(fd_);
  }
}

void AsyncSocket::cachePeerAddress() const {
  if (!addr_.isInitialized() && fd_ != NetworkSocket()) {
    addr_.setFromPeerAddress(fd_);
  }
}

void AsyncSocket::applyOptions(
    const SocketOptionMap& options, SocketOptionKey::ApplyPos pos) {
  auto result = applySocketOptions(fd_, options, pos);
  if (result != 0) {
    throw AsyncSocketException(
        AsyncSocketException::INTERNAL_ERROR,
        withAddr("failed to set socket option"),
        result);
  }
}

bool AsyncSocket::isZeroCopyWriteInProgress() const noexcept {
  eventBase_->dcheckIsInEventBaseThread();
  return (!idZeroCopyBufPtrMap_.empty());
}

void AsyncSocket::getLocalAddress(folly::SocketAddress* address) const {
  cacheLocalAddress();
  *address = localAddr_;
}

void AsyncSocket::getPeerAddress(folly::SocketAddress* address) const {
  cachePeerAddress();
  *address = addr_;
}

bool AsyncSocket::getTFOSucceeded() const {
  return detail::tfo_succeeded(fd_);
}

int AsyncSocket::setNoDelay(bool noDelay) {
  if (fd_ == NetworkSocket()) {
    VLOG(4) << "AsyncSocket::setNoDelay() called on non-open socket " << this
            << "(state=" << state_ << ")";
    return EINVAL;
  }

  int value = noDelay ? 1 : 0;
  if (netops_->setsockopt(
          fd_, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)) != 0) {
    int errnoCopy = errno;
    VLOG(2) << "failed to update TCP_NODELAY option on AsyncSocket " << this
            << " (fd=" << fd_ << ", state=" << state_
            << "): " << errnoStr(errnoCopy);
    return errnoCopy;
  }

  return 0;
}

int AsyncSocket::setCongestionFlavor(const std::string& cname) {
#ifndef TCP_CONGESTION
#define TCP_CONGESTION 13
#endif

  if (fd_ == NetworkSocket()) {
    VLOG(4) << "AsyncSocket::setCongestionFlavor() called on non-open "
            << "socket " << this << "(state=" << state_ << ")";
    return EINVAL;
  }

  if (netops_->setsockopt(
          fd_,
          IPPROTO_TCP,
          TCP_CONGESTION,
          cname.c_str(),
          socklen_t(cname.length() + 1)) != 0) {
    int errnoCopy = errno;
    VLOG(2) << "failed to update TCP_CONGESTION option on AsyncSocket " << this
            << "(fd=" << fd_ << ", state=" << state_
            << "): " << errnoStr(errnoCopy);
    return errnoCopy;
  }

  return 0;
}

int AsyncSocket::setQuickAck(bool quickack) {
  (void)quickack;
  if (fd_ == NetworkSocket()) {
    VLOG(4) << "AsyncSocket::setQuickAck() called on non-open socket " << this
            << "(state=" << state_ << ")";
    return EINVAL;
  }

#ifdef TCP_QUICKACK // Linux-only
  int value = quickack ? 1 : 0;
  if (netops_->setsockopt(
          fd_, IPPROTO_TCP, TCP_QUICKACK, &value, sizeof(value)) != 0) {
    int errnoCopy = errno;
    VLOG(2) << "failed to update TCP_QUICKACK option on AsyncSocket" << this
            << "(fd=" << fd_ << ", state=" << state_
            << "): " << errnoStr(errnoCopy);
    return errnoCopy;
  }

  return 0;
#else
  return ENOSYS;
#endif
}

int AsyncSocket::setSendBufSize(size_t bufsize) {
  if (fd_ == NetworkSocket()) {
    VLOG(4) << "AsyncSocket::setSendBufSize() called on non-open socket "
            << this << "(state=" << state_ << ")";
    return EINVAL;
  }

  if (netops_->setsockopt(
          fd_, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize)) != 0) {
    int errnoCopy = errno;
    VLOG(2) << "failed to update SO_SNDBUF option on AsyncSocket" << this
            << "(fd=" << fd_ << ", state=" << state_
            << "): " << errnoStr(errnoCopy);
    return errnoCopy;
  }

  return 0;
}

int AsyncSocket::setRecvBufSize(size_t bufsize) {
  if (fd_ == NetworkSocket()) {
    VLOG(4) << "AsyncSocket::setRecvBufSize() called on non-open socket "
            << this << "(state=" << state_ << ")";
    return EINVAL;
  }

  if (netops_->setsockopt(
          fd_, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize)) != 0) {
    int errnoCopy = errno;
    VLOG(2) << "failed to update SO_RCVBUF option on AsyncSocket" << this
            << "(fd=" << fd_ << ", state=" << state_
            << "): " << errnoStr(errnoCopy);
    return errnoCopy;
  }

  return 0;
}

#if defined(__linux__)
size_t AsyncSocket::getSendBufInUse() const {
  if (fd_ == NetworkSocket()) {
    std::stringstream issueString;
    issueString << "AsyncSocket::getSendBufInUse() called on non-open socket "
                << this << "(state=" << state_ << ")";
    VLOG(4) << issueString.str();
    throw std::logic_error(issueString.str());
  }

  size_t returnValue = 0;
  if (-1 == ::ioctl(fd_.toFd(), SIOCOUTQ, &returnValue)) {
    int errnoCopy = errno;
    std::stringstream issueString;
    issueString
        << "Failed to get the tx used bytes on Socket: " << this << "(fd="
        << fd_ << ", state=" << state_ << "): " << errnoStr(errnoCopy);
    VLOG(2) << issueString.str();
    throw std::logic_error(issueString.str());
  }

  return returnValue;
}

size_t AsyncSocket::getRecvBufInUse() const {
  if (fd_ == NetworkSocket()) {
    std::stringstream issueString;
    issueString << "AsyncSocket::getRecvBufInUse() called on non-open socket "
                << this << "(state=" << state_ << ")";
    VLOG(4) << issueString.str();
    throw std::logic_error(issueString.str());
  }

  size_t returnValue = 0;
  if (-1 == ::ioctl(fd_.toFd(), SIOCINQ, &returnValue)) {
    std::stringstream issueString;
    int errnoCopy = errno;
    issueString
        << "Failed to get the rx used bytes on Socket: " << this << "(fd="
        << fd_ << ", state=" << state_ << "): " << errnoStr(errnoCopy);
    VLOG(2) << issueString.str();
    throw std::logic_error(issueString.str());
  }

  return returnValue;
}
#endif

int AsyncSocket::setTCPProfile(int profd) {
  if (fd_ == NetworkSocket()) {
    VLOG(4) << "AsyncSocket::setTCPProfile() called on non-open socket " << this
            << "(state=" << state_ << ")";
    return EINVAL;
  }

  if (netops_->setsockopt(
          fd_, SOL_SOCKET, SO_SET_NAMESPACE, &profd, sizeof(int)) != 0) {
    int errnoCopy = errno;
    VLOG(2) << "failed to set socket namespace option on AsyncSocket" << this
            << "(fd=" << fd_ << ", state=" << state_
            << "): " << errnoStr(errnoCopy);
    return errnoCopy;
  }

  return 0;
}

void AsyncSocket::ioReady(uint16_t events) noexcept {
  VLOG(7) << "AsyncSocket::ioRead() this=" << this << ", fd=" << fd_
          << ", events=" << std::hex << events << ", state=" << state_;
  DestructorGuard dg(this);
  assert(events & EventHandler::READ_WRITE);
  eventBase_->dcheckIsInEventBaseThread();

  auto relevantEvents = uint16_t(events & EventHandler::READ_WRITE);
  EventBase* originalEventBase = eventBase_;
  // If we got there it means that either EventHandler::READ or
  // EventHandler::WRITE is set. Any of these flags can
  // indicate that there are messages available in the socket
  // error message queue.
  // Return if we handle any error messages - this is to avoid
  // unnecessary read/write calls
  if (handleErrMessages()) {
    return;
  }

  // Return now if handleErrMessages() detached us from our EventBase
  if (eventBase_ != originalEventBase) {
    return;
  }

  const auto startRawBytesReceived = getRawBytesReceived();
  const auto startAppBytesReceived = getAppBytesReceived();
  const auto startRawBytesWritten = getRawBytesWritten();

  if (relevantEvents == EventHandler::READ) {
    handleRead();
  } else if (relevantEvents == EventHandler::WRITE) {
    handleWrite();
  } else if (relevantEvents == EventHandler::READ_WRITE) {
    // If both read and write events are ready, process writes first.
    handleWrite();

    // Return now if handleWrite() detached us from our EventBase
    if (eventBase_ != originalEventBase) {
      return;
    }

    // Only call handleRead() if a read callback is still installed.
    // (It's possible that the read callback was uninstalled during
    // handleWrite().)
    if (readCallback_) {
      handleRead();
    }
  } else {
    VLOG(4) << "AsyncSocket::ioRead() called with unexpected events "
            << std::hex << events << "(this=" << this << ")";
    abort();
  }

  // It is possible that there are messages in the error queue yet
  // `handleErrMessages()` returns without reading them because no error
  // message callback is set and byte events are disabled. This could happen
  // when a write is performed to an AsyncSocket with byte events enabled,
  // triggers timestamps, and the fd is detached from the AsyncSocket and
  // attached to a new AsyncSocket before the error messages generated for those
  // timestamps arrive on the error queue. These `orphan` messages in the error
  // queue will cause us to spin: `ioReady()` will be repeatedly invoked, but
  // because we are not reading from the socket error queue, the state will
  // never be cleared.
  //
  // To prevent spinning under such circumstances, we
  // drain the queue of AF_INET sockets if the read or write handlers did not
  // read or write anything on an invocation of `ioReady()`. We check both raw
  // and app bytes received because raw bytes received are 0 for AsyncSSLSockets
  // with no BIO. Because the read or write handlers can modify the event flags,
  // we check these flags again before draining the error queue. We restrict
  // the drain to AF_INET sockets as other socket family do not support
  // MSG_ERRQUEUE (e.g., AF_UNIX) or their support is not well documented
  if (startRawBytesReceived == getRawBytesReceived() &&
      startAppBytesReceived == getAppBytesReceived() &&
      startRawBytesWritten == getRawBytesWritten() &&
      eventFlags_ != EventHandler::NONE && eventFlags_ == events &&
      (localAddr_.getFamily() == AF_INET ||
       localAddr_.getFamily() == AF_INET6)) {
    drainErrorQueue();
  }
}

AsyncSocket::ReadResult AsyncSocket::performReadMsg(
    struct ::msghdr& msg,
    // This is here only to preserve AsyncSSLSocket's legacy semi-broken
    // behavior (D43648653 for context).
    AsyncReader::ReadCallback::ReadMode readMode) {
  VLOG(5) << "AsyncSocket::performReadMsg() this=" << this
          << ", iovs=" << msg.msg_iov << ", num=" << msg.msg_iovlen;

  if (!msg.msg_iovlen) {
    return ReadResult(READ_ERROR);
  }

  if (preReceivedData_ && !preReceivedData_->empty()) {
    VLOG(5) << "AsyncSocket::performReadMsg() this=" << this
            << ", reading pre-received data";

    ssize_t len = 0;
    for (size_t i = 0;
         // MacOS `msg_iovlen` is an `int` :(
         (i < static_cast<size_t>(msg.msg_iovlen)) &&
         (!preReceivedData_->empty());
         ++i) {
      io::Cursor cursor(preReceivedData_.get());
      auto ret =
          cursor.pullAtMost(msg.msg_iov[i].iov_base, msg.msg_iov[i].iov_len);
      len += ret;

      IOBufQueue queue;
      queue.append(std::move(preReceivedData_));
      queue.trimStart(ret);
      preReceivedData_ = queue.move();
    }

    appBytesReceived_ += len;
    return ReadResult(len);
  }

  ssize_t bytes = 0;
  if (readMode == AsyncReader::ReadCallback::ReadMode::ReadZC) {
    DestructorGuard dg(this);
    // ReadZC is async, where the current code path triggered from POLLIN will
    // issue the request but the completion happens at a later point.
    // AsyncSocket reads are level triggered so it is possible for another
    // ReadZC request to be issued before an already issued request has
    // completed. To fix this, unset the readCB.
    auto readCallback = readCallback_;
    setReadCB(nullptr);
    auto backend = getEventBase()->getBackend();
    auto readZcCallback = [guard = std::move(dg), readCallback](ssize_t len) {
      if (len < 0) {
        AsyncSocketException ex(
            AsyncSocketException::INTERNAL_ERROR, "ReadZC failed", len);
        readCallback->readErr(ex);
      } else {
        readCallback->readDataAvailable(len);
      }
    };
    backend->queueRecvZc(
        fd_.toFd(),
        msg.msg_iov[0].iov_base,
        msg.msg_iov[0].iov_len,
        std::move(readZcCallback));
    return ReadResult(READ_ASYNC);
  } else if (readAncillaryDataCallback_ == nullptr && msg.msg_iovlen == 1) {
    bytes = netops_->recv(
        fd_, msg.msg_iov[0].iov_base, msg.msg_iov[0].iov_len, MSG_DONTWAIT);
  } else {
    int recvFlags = 0;
    if (readAncillaryDataCallback_) {
      auto buf = readAncillaryDataCallback_->getAncillaryDataCtrlBuffer();
      msg.msg_control = buf.data();
      msg.msg_controllen = buf.size();
#if defined(__linux__)
      // On BSD / MacOS, `AsyncFdSocket` has to do 2 extra `fcntl`s per FD.
      recvFlags |= MSG_CMSG_CLOEXEC;
#endif
    } else {
      msg.msg_control = nullptr;
      msg.msg_controllen = 0;
    }

    // `msg.msg_iov*` were set by the caller, we're ready.
    bytes = netops::recvmsg(fd_, &msg, recvFlags);

    // KEY INVARIANT: If `bytes > 0`, we must proceed to `ancillaryData` --
    // no error branches must interrupt this flow.  The reason is that
    // otherwise, received FDs could be irretrievably leaked, causing
    // eventual process failure due to `EMFILE`.
    //
    // NB: We do not check for MSG_CTRUNC here for the reason above.  We do
    // not check for it _after_ the callbacks have fired because our
    // `ReadCallback` could move the socket to a different thread, which
    // would make a subsequent `failRead` unsafe.  Instead we require
    // `ReadAncillaryDataCallback` implementations to check this.
  }

  if (bytes < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // No more data to read right now.
      return ReadResult(READ_BLOCKING);
    } else {
      return ReadResult(READ_ERROR);
    }
  } else {
    appBytesReceived_ += bytes;
    return ReadResult(bytes);
  }
}

void AsyncSocket::prepareReadBuffer(void** buf, size_t* buflen) {
  // no matter what, buffer should be prepared for non-ssl socket
  CHECK(readCallback_);
  readCallback_->getReadBuffer(buf, buflen);
}

void AsyncSocket::prepareReadBuffers(IOBufIovecBuilder::IoVecVec& iovs) {
  // no matter what, buffers should be prepared for non-ssl socket
  CHECK(readCallback_);
  readCallback_->getReadBuffers(iovs);
}

void AsyncSocket::drainErrorQueue() noexcept {
  VLOG(5) << "AsyncSocket::drainErrorQueue() this=" << this << ", fd=" << fd_
          << ", state=" << state_;

  if (errMessageCallback_ != nullptr ||
      (byteEventHelper_ && byteEventHelper_->byteEventsEnabled)) {
    VLOG(7) << "AsyncSocket::drainErrorQueue(): "
            << "err message callback installed or "
            << "ByteEvents enabled - exiting.";
    return;
  }

#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  int ret = 0;
  while (ret >= 0 && fd_ != NetworkSocket()) {
    uint8_t ctrl[1024];
    unsigned char data;

    struct iovec entry{};
    entry.iov_base = &data;
    entry.iov_len = sizeof(data);

    struct msghdr msg{};
    msg.msg_iov = &entry;
    msg.msg_iovlen = 1;
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof(ctrl);
    msg.msg_flags = 0;

    ret = netops_->recvmsg(fd_, &msg, MSG_ERRQUEUE);
  }
#endif
}

size_t AsyncSocket::handleErrMessages() noexcept {
  // This method has non-empty implementation only for platforms
  // supporting per-socket error queues.
  VLOG(5) << "AsyncSocket::handleErrMessages() this=" << this << ", fd=" << fd_
          << ", state=" << state_;
  if (errMessageCallback_ == nullptr && idZeroCopyBufPtrMap_.empty() &&
      (!byteEventHelper_ || !byteEventHelper_->byteEventsEnabled)) {
    VLOG(7) << "AsyncSocket::handleErrMessages(): "
            << "no err message callback installed and "
            << "ByteEvents not enabled - exiting.";
    return 0;
  }

#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  uint8_t ctrl[1024];
  unsigned char data;
  struct msghdr msg{};
  iovec entry{};

  entry.iov_base = &data;
  entry.iov_len = sizeof(data);
  msg.msg_iov = &entry;
  msg.msg_iovlen = 1;
  msg.msg_name = nullptr;
  msg.msg_namelen = 0;
  msg.msg_control = ctrl;
  msg.msg_controllen = sizeof(ctrl);
  msg.msg_flags = 0;

  int ret;
  size_t num = 0;
  // the socket may be closed by errMessage callback, so check on each iteration
  while (fd_ != NetworkSocket()) {
    ret = netops_->recvmsg(fd_, &msg, MSG_ERRQUEUE);
    VLOG(5) << "AsyncSocket::handleErrMessages(): recvmsg returned " << ret;

    if (ret < 0) {
      if (errno != EAGAIN) {
        auto errnoCopy = errno;
        LOG(ERROR) << "::recvmsg exited with code " << ret
                   << ", errno: " << errnoCopy << ", fd: " << fd_;
        AsyncSocketException ex(
            AsyncSocketException::INTERNAL_ERROR,
            withAddr("recvmsg() failed"),
            errnoCopy);
        failErrMessageRead(__func__, ex);
      }

      return num;
    }

    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
         cmsg != nullptr && cmsg->cmsg_len != 0;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      ++num;
      if (isZeroCopyMsg(*cmsg)) {
        processZeroCopyMsg(*cmsg);
        continue;
      }

      // try to process it as a ByteEvent and forward to observers
      //
      // observers cannot throw and thus we expect only exceptions from
      // ByteEventHelper, but we guard against other cases for safety
      if (byteEventHelper_) {
        try {
          if (const auto maybeByteEvent =
                  byteEventHelper_->processCmsg(*cmsg, getRawBytesWritten())) {
            const auto& byteEvent = maybeByteEvent.value();
            for (const auto& observer : lifecycleObservers_) {
              if (observer->getConfig().byteEvents) {
                observer->byteEvent(this, byteEvent);
              }
            }
          }
        } catch (const ByteEventHelper::Exception& behEx) {
          // rewrap the ByteEventHelper::Exception with extra information
          AsyncSocketException ex(
              AsyncSocketException::INTERNAL_ERROR,
              withAddr(
                  string(
                      "AsyncSocket::handleErrMessages(), "
                      "internal exception during ByteEvent processing: ") +
                  behEx.what()));
          failByteEvents(ex);
        } catch (const std::exception& ex) {
          AsyncSocketException tex(
              AsyncSocketException::UNKNOWN,
              string(
                  "AsyncSocket::handleErrMessages(), "
                  "unhandled exception during ByteEvent processing, "
                  "threw exception: ") +
                  ex.what());
          failByteEvents(tex);
        } catch (...) {
          AsyncSocketException tex(
              AsyncSocketException::UNKNOWN,
              string(
                  "AsyncSocket::handleErrMessages(), "
                  "unhandled exception during ByteEvent processing, "
                  "threw non-exception type"));
          failByteEvents(tex);
        }
      }

      // even if it is a timestamp, hand it off to the errMessageCallback,
      // the application may want it as well.
      if (errMessageCallback_) {
        errMessageCallback_->errMessage(*cmsg);
      }
    }
  }
  return num;
#else
  return 0;
#endif // FOLLY_HAVE_MSG_ERRQUEUE
}

bool AsyncSocket::processZeroCopyWriteInProgress() noexcept {
  eventBase_->dcheckIsInEventBaseThread();
  if (idZeroCopyBufPtrMap_.empty()) {
    return true;
  }

  handleErrMessages();

  return idZeroCopyBufPtrMap_.empty();
}

folly::Expected<folly::TcpInfo, std::errc> AsyncSocket::getTcpInfo(
    const TcpInfo::LookupOptions& options) {
  if (NetworkSocket() == fd_) {
    return folly::makeUnexpected(std::errc::invalid_argument);
  }
  return tcpInfoDispatcher_->initFromFd(fd_, options);
}

void AsyncSocket::addLifecycleObserver(
    AsyncSocket::LegacyLifecycleObserver* observer) {
  if (eventBase_) {
    eventBase_->dcheckIsInEventBaseThread();
  }

  // adding the same observer multiple times is not allowed
  auto& observers = lifecycleObservers_;
  CHECK(
      std::find(observers.begin(), observers.end(), observer) ==
      observers.end());

  observers.push_back(observer);
  observer->observerAttach(this);
  if (observer->getConfig().byteEvents) {
    if (byteEventHelper_ && byteEventHelper_->maybeEx.has_value()) {
      observer->byteEventsUnavailable(this, *byteEventHelper_->maybeEx);
    } else if (byteEventHelper_ && byteEventHelper_->byteEventsEnabled) {
      observer->byteEventsEnabled(this);
    } else if (state_ == StateEnum::ESTABLISHED) {
      enableByteEvents(); // try to enable now
    }
    // do nothing right now; wait until we're connected
  }
}

bool AsyncSocket::removeLifecycleObserver(
    AsyncSocket::LegacyLifecycleObserver* observer) {
  auto& observers = lifecycleObservers_;
  auto it = std::find(observers.begin(), observers.end(), observer);
  if (it == observers.end()) {
    return false;
  }
  observer->observerDetach(this);
  observers.erase(it);
  return true;
}

std::vector<AsyncSocket::LegacyLifecycleObserver*>
AsyncSocket::getLifecycleObservers() const {
  if (eventBase_) {
    eventBase_->dcheckIsInEventBaseThread();
  }
  return std::vector<AsyncSocket::LegacyLifecycleObserver*>(
      lifecycleObservers_.begin(), lifecycleObservers_.end());
}

void AsyncSocket::splitIovecArray(
    const size_t startOffset,
    const size_t endOffset,
    const iovec* srcVec,
    const size_t srcCount,
    iovec* dstVec,
    size_t& dstCount) {
  CHECK_GE(endOffset, startOffset);
  CHECK_GE(dstCount, srcCount);
  dstCount = 0;

  const size_t targetBytes = endOffset - startOffset + 1;
  size_t dstBytes = 0;
  size_t processedBytes = 0;
  for (size_t i = 0; i < srcCount; processedBytes += srcVec[i].iov_len, i++) {
    iovec currentOp = srcVec[i];
    if (currentOp.iov_len == 0) { // to handle the oddballs
      continue;
    }

    // if we haven't found the start offset yet, see if it is in this op
    if (dstCount == 0) {
      if (processedBytes + currentOp.iov_len < startOffset + 1) {
        continue; // start offset isn't in this op
      }

      // offset iov_base to get the start offset
      const size_t trimFromStart = startOffset - processedBytes;
      currentOp.iov_base =
          reinterpret_cast<uint8_t*>(currentOp.iov_base) + trimFromStart;
      currentOp.iov_len -= trimFromStart;
    }

    // trim the end of the iovec, if needed
    ssize_t trimFromEnd = (dstBytes + currentOp.iov_len) - targetBytes;
    if (trimFromEnd > 0) {
      currentOp.iov_len -= trimFromEnd;
    }

    dstVec[dstCount] = currentOp;
    dstCount++;
    dstBytes += currentOp.iov_len;
    CHECK_GE(targetBytes, dstBytes);
    if (targetBytes == dstBytes) {
      break; // done
    }
  }

  CHECK_EQ(targetBytes, dstBytes);
}

AsyncSocket::ReadCode AsyncSocket::processZeroCopyRead() {
#if defined(TCP_ZEROCOPY_RECEIVE)
  if (zerocopyReadDisabled_) {
    return ReadCode::READ_NOT_SUPPORTED;
  }

  auto* memStore = readCallback_->readZeroCopyEnabled();
  if (!memStore) {
    // set zerocopyReadDisabled_ to true to avoid further virtual calls
    zerocopyReadDisabled_ = true;
    return ReadCode::READ_NOT_SUPPORTED;
  }

  if (preReceivedData_ && !preReceivedData_->empty()) {
    VLOG(5) << "AsyncSocket::processZeroCopyRead() this=" << this
            << ", reading pre-received data";

    auto len = preReceivedData_->computeChainDataLength();
    readCallback_->readZeroCopyDataAvailable(
        std::move(preReceivedData_), 0 /*additionalBytes*/);
    appBytesReceived_ += len;

    return ReadCode::READ_DONE;
  }

  auto ptr = memStore->get();
  if (!ptr) {
    return ReadCode::READ_NOT_SUPPORTED;
  }

  void* copybuf = nullptr;
  size_t copybuf_len = 0;
  readCallback_->getZeroCopyFallbackBuffer(&copybuf, &copybuf_len);

  folly::netops::tcp_zerocopy_receive zc = {};
  socklen_t zc_len = sizeof(zc);

  zc.address = reinterpret_cast<uint64_t>(ptr->data);
  zc.length = ptr->capacity;
  assert(uintptr_t(zc.address) % sysconf(_SC_PAGESIZE) == 0);
  assert(zc.length % sysconf(_SC_PAGESIZE) == 0);
  auto zc_length = zc.length;

  zc.copybuf_address = reinterpret_cast<__u64>(copybuf);
  zc.copybuf_len = copybuf_len;
  auto zc_copybuf_len = zc.copybuf_len;

  auto ret =
      ::getsockopt(fd_.toFd(), IPPROTO_TCP, TCP_ZEROCOPY_RECEIVE, &zc, &zc_len);
  if (!ret) {
    // zc.err can be set even if there is still more data buffered in the
    // kernel that we have not fully read yet.  When zc.err is set, just
    // remember the error code, and keep reading until we get 0 data back from
    // the kernel. Only once we have seen 0 bytes returned from the kernel do we
    // want to return the error to the caller.
    if (zc.err) {
      zerocopyReadErr_ = -zc.err; // note: zc.err <= 0
    }

    assert(zc.length % sysconf(_SC_PAGESIZE) == 0);

    auto len = zc.length + zc.copybuf_len;

    if (zerocopyReadErr_ && len == 0) {
      auto err = zerocopyReadErr_;
      zerocopyReadErr_ = 0;

      readErr_ = READ_ERROR;
      AsyncSocketException ex(
          AsyncSocketException::INTERNAL_ERROR,
          withAddr("TCP_ZEROCOPY_RECEIVE failed"),
          err);
      return failRead(__func__, ex);
    }

    std::unique_ptr<folly::IOBuf> buf;
    if (zc.length) {
      // adjust the len
      ptr->len = zc.length;
      auto tmp = getRXZeroCopyIOBuf(std::move(ptr));
      buf = std::move(tmp);
      // ZC buffers must be marked externally shared
      // since they are "shared" with the kernel networking stack
      // and must not be written to
      // so that Fizz does not attempt to perform
      // in place decryption and write to these buffers.
      buf->markExternallyShared();
    }

    if (len) {
      readCallback_->readZeroCopyDataAvailable(std::move(buf), zc.copybuf_len);
      appBytesReceived_ += len;

      // If we completely filled up the zerocopy buffer then we likely have
      // more data buffered in the kernel, so return READ_CONTINUE to try again.
      // We also want the caller to retry reading if we have a deferred error
      // code to give them.
      if ((zc.copybuf_len == 0 && zc.length == zc_length) ||
          zc.copybuf_len == zc_copybuf_len || zerocopyReadErr_ != 0) {
        return ReadCode::READ_CONTINUE;
      }
      return ReadCode::READ_DONE;
    } else {
      // No more data to read right now.
      return ReadCode::READ_DONE;
    }
  } else {
    if (errno == EIO) {
      // EOF
      readErr_ = READ_EOF;
      // EOF
      shutdownFlags_ |= SHUT_READ;
      if (!updateEventRegistration(0, EventHandler::READ)) {
        // we've already been moved into STATE_ERROR
        assert(state_ == StateEnum::ERROR);
        assert(readCallback_ == nullptr);
        return ReadCode::READ_DONE;
      }

      ReadCallback* callback = readCallback_;
      readCallback_ = nullptr;
      callback->readEOF();
      return ReadCode::READ_DONE;
    }

    // treat any other error as not supported, fall back to regular read
    zerocopyReadDisabled_ = true;
  }
#endif
  return ReadCode::READ_NOT_SUPPORTED;
}

AsyncSocket::ReadCode AsyncSocket::processNormalRead() {
  auto readMode = readCallback_->getReadMode();
  // Get the buffer(s) to read into.
  void* buf = nullptr;
  size_t buflen = 0;
  IOBufIovecBuilder::IoVecVec iovs; // this can be an AsyncSocket member too

  try {
    if (readMode == AsyncReader::ReadCallback::ReadMode::ReadVec) {
      prepareReadBuffers(iovs);
      VLOG(5) << "prepareReadBuffers() bufs=" << iovs.data()
              << ", num=" << iovs.size();
    } else {
      prepareReadBuffer(&buf, &buflen);
      VLOG(5) << "prepareReadBuffer() buf=" << buf << ", buflen=" << buflen;
    }
  } catch (const AsyncSocketException& ex) {
    return failRead(__func__, ex);
  } catch (const std::exception& ex) {
    AsyncSocketException tex(
        AsyncSocketException::BAD_ARGS,
        string(
            "ReadCallback::getReadBuffer() "
            "threw exception: ") +
            ex.what());
    return failRead(__func__, tex);
  } catch (...) {
    AsyncSocketException ex(
        AsyncSocketException::BAD_ARGS,
        "ReadCallback::getReadBuffer() threw "
        "non-exception type");
    return failRead(__func__, ex);
  }
  if (iovs.empty() && (buf == nullptr || buflen == 0)) {
    AsyncSocketException ex(
        AsyncSocketException::BAD_ARGS,
        "ReadCallback::getReadBuffer() returned "
        "empty buffer");
    return failRead(__func__, ex);
  }

  // Perform the read; we want `msg` for the `ancillaryData` callback.
  //
  // Zero-initialization is crucial here because not all code paths in
  // `performReadMsg` go through `recvmsg` (e.g., "pre-received data" and
  // "recv" are possibilities).  For those that do not, we want at a minimum
  // `msg_controllen` and `msg_flags` to be zero.
  struct ::msghdr msg{};
  // Dest address info
  msg.msg_name = nullptr;
  msg.msg_namelen = 0;
  struct ::iovec iov{}; // unused in `ReadMode::ReadVec`
  if (readMode == AsyncReader::ReadCallback::ReadMode::ReadVec) {
    msg.msg_iov = iovs.data();
    msg.msg_iovlen = iovs.size();
  } else {
    iov.iov_base = buf;
    iov.iov_len = buflen;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
  }
  auto readResult = performReadMsg(msg, readMode);

  auto bytesRead = readResult.readReturn;
  VLOG(4) << "this=" << this << ", AsyncSocket::handleRead() got " << bytesRead
          << " bytes";
  if (bytesRead > 0) {
    DCHECK(readCallback_);
    if (readAncillaryDataCallback_) {
      auto prevReadCallback = readCallback_;

      auto ancillaryDataRes = readAncillaryDataCallback_->ancillaryData(msg);
      if (ancillaryDataRes.hasError()) {
        return failRead(__func__, ancillaryDataRes.error());
      }

      if (readCallback_ != prevReadCallback) [[unlikely]] {
        // The `ancillaryData` callback is allowed to close the socket,
        // but otherwise is not allowed to change/replace the read callback.
        CHECK_EQ((shutdownFlags_ & SHUT_READ), SHUT_READ);
        CHECK(readCallback_ == nullptr);
        // Return now since the socket has been closed, and discard
        // the (real, non-ancillary) data that was read.
        return ReadCode::READ_DONE;
      }
      // `ancillaryData()` is expected to check and error on this, since
      // it's probably incorrect to process truncated ancillary data.  If
      // some bizarre callback wants to treat this as recoverable, it can
      // clear `MSG_CTRUNC` on `msg_flags` before returning.
      //
      // Don't move this: `performReadMsg` doesn't guarantee that `msg_flags`
      // is valid without `readAncillaryDataCallback_`.  Also, the
      // `readCallback_ != prevReadCallback` test means that we can safely
      // call `failRead()` since a prior error would clear the read CB.
      if (msg.msg_flags & MSG_CTRUNC) {
        VLOG(5) << "AsyncSocket::performReadInternal() this=" << this
                << ", ancillary data was truncated: " << msg.msg_flags;
        readErr_ = READ_ERROR;
        AsyncSocketException ex(
            AsyncSocketException::INTERNAL_ERROR,
            withAddr("recvmsg() got MSG_CTRUNC"));
        return failRead(__func__, ex);
      }
    }
    readCallback_->readDataAvailable(size_t(bytesRead));

    // Continue reading if we filled the available buffer
    return (size_t(bytesRead) < buflen)
        ? ReadCode::READ_DONE
        : ReadCode::READ_CONTINUE;

  } else if (bytesRead == READ_BLOCKING) {
    // No more data to read right now.
    return ReadCode::READ_DONE;
  } else if (bytesRead == READ_ERROR) {
    auto errnoCopy = errno;
    recvErr(errnoCopy, std::move(readResult.exception));
    return AsyncSocket::ReadCode::READ_DONE;
  } else if (bytesRead == READ_ASYNC) {
    return ReadCode::READ_DONE;
  } else {
    assert(bytesRead == READ_EOF);
    recvEOF();
    return ReadCode::READ_DONE;
  }
}

void AsyncSocket::handleRead() noexcept {
  VLOG(5) << "AsyncSocket::handleRead() this=" << this << ", fd=" << fd_
          << ", state=" << state_;
  assert(state_ == StateEnum::ESTABLISHED);
  assert((shutdownFlags_ & SHUT_READ) == 0);
  assert(readCallback_ != nullptr);
  assert(eventFlags_ & EventHandler::READ);

  // Loop until:
  // - a read attempt would block
  // - readCallback_ is uninstalled
  // - the number of loop iterations exceeds the optional maximum
  // - this AsyncSocket is moved to another EventBase
  //
  // When we invoke readDataAvailable() it may uninstall the readCallback_,
  // which is why need to check for it here.
  //
  // The last bullet point is slightly subtle.  readDataAvailable() may also
  // detach this socket from this EventBase.  However, before
  // readDataAvailable() returns another thread may pick it up, attach it to
  // a different EventBase, and install another readCallback_.  We need to
  // exit immediately after readDataAvailable() returns if the eventBase_ has
  // changed.  (The caller must perform some sort of locking to transfer the
  // AsyncSocket between threads properly.  This will be sufficient to ensure
  // that this thread sees the updated eventBase_ variable after
  // readDataAvailable() returns.)
  size_t numReads = maxReadsPerEvent_ ? maxReadsPerEvent_ : size_t(-1);
  EventBase* originalEventBase = eventBase_;
  while (readCallback_ && eventBase_ == originalEventBase && numReads--) {
    auto ret = processZeroCopyRead();
    if (ret == ReadCode::READ_NOT_SUPPORTED) {
      ret = processNormalRead();
    }

    switch (ret) {
      case ReadCode::READ_NOT_SUPPORTED:
        CHECK(false);
      case ReadCode::READ_CONTINUE:
        break;
      case ReadCode::READ_DONE:
        return;
    }
  }

  if (readCallback_ && eventBase_ == originalEventBase) {
    // We might still have data in the socket.
    // (e.g. see comment in AsyncSSLSocket::checkForImmediateRead)
    scheduleImmediateRead();
  }
}

/**
 * This function attempts to write as much data as possible, until no more
 * data can be written.
 *
 * - If it sends all available data, it unregisters for write events, and
 * stops the writeTimeout_.
 *
 * - If not all of the data can be sent immediately, it reschedules
 *   writeTimeout_ (if a non-zero timeout is set), and ensures the handler is
 *   registered for write events.
 */
void AsyncSocket::handleWrite() noexcept {
  VLOG(5) << "AsyncSocket::handleWrite() this=" << this << ", fd=" << fd_
          << ", state=" << state_;
  DestructorGuard dg(this);

  if (state_ == StateEnum::CONNECTING) {
    handleConnect();
    return;
  }

  // Normal write
  assert(state_ == StateEnum::ESTABLISHED);
  assert((shutdownFlags_ & SHUT_WRITE) == 0);
  assert(writeReqHead_ != nullptr);

  // Loop until we run out of write requests,
  // or until this socket is moved to another EventBase.
  // (See the comment in handleRead() explaining how this can happen.)
  EventBase* originalEventBase = eventBase_;
  while (writeReqHead_ != nullptr && eventBase_ == originalEventBase) {
    writeReqHead_->getCallbackWithState().notifyOnWrite();

    auto writeResult = writeReqHead_->performWrite();
    if (writeResult.writeReturn < 0) {
      if (writeResult.exception) {
        return failWrite(__func__, *writeResult.exception);
      }
      auto errnoCopy = errno;
      AsyncSocketException ex(
          AsyncSocketException::INTERNAL_ERROR,
          withAddr("writev() failed"),
          errnoCopy);
      return failWrite(__func__, ex);
    } else if (writeReqHead_->isComplete()) {
      // We finished this request
      WriteRequest* req = writeReqHead_;
      writeReqHead_ = req->getNext();

      if (writeReqHead_ == nullptr) {
        writeReqTail_ = nullptr;
        sendDone();
      }

      // Invoke the callback
      WriteCallback* callback = req->getCallback();
      req->destroy();
      if (callback) {
        callback->writeSuccess();
      }
      // We'll continue around the loop, trying to write another request
    } else {
      // Partial write.
      writeReqHead_->consume();
      sendPartial();
      return;
    }
  }
}

void AsyncSocket::checkForImmediateRead() noexcept {
  // We currently don't attempt to perform optimistic reads in AsyncSocket.
  // (However, note that some subclasses do override this method.)
  //
  // Simply calling handleRead() here would be bad, as this would call
  // readCallback_->getReadBuffer(), forcing the callback to allocate a read
  // buffer even though no data may be available.  This would waste lots of
  // memory, since the buffer will sit around unused until the socket actually
  // becomes readable.
  //
  // Checking if the socket is readable now also seems like it would probably
  // be a pessimism.  In most cases it probably wouldn't be readable, and we
  // would just waste an extra system call.  Even if it is readable, waiting
  // to find out from libevent on the next event loop doesn't seem that bad.
  //
  // The exception to this is if we have pre-received data. In that case there
  // is definitely data available immediately.
  auto* callback = readCallback_;
  auto* originalEventBase = eventBase_;
  if (iouRecvHandle_) {
    CHECK(callback->isBufferMovable())
        << "native io_uring recv requires a movable ReadCallback";
  }
  if (preReceivedData_ && !preReceivedData_->empty()) {
    if (iouRecvHandle_) {
      callback->readBufferAvailable(std::move(preReceivedData_));
    } else {
      handleRead();
    }
  }

  if (iouRecvHandle_ && readCallback_ == callback &&
      eventBase_ == originalEventBase) {
    iouRecvHandle_->submit();
    iouRecvHandle_->drainCompletedReads();
  }
}

void AsyncSocket::handleInitialReadWrite() noexcept {
  // Our callers should already be holding a DestructorGuard, but grab
  // one here just to make sure, in case one of our calling code paths ever
  // changes.
  DestructorGuard dg(this);
  // If we have a readCallback_, make sure we enable read events.  We
  // may already be registered for reads if connectSuccess() set
  // the read callback.
  if (readCallback_ && !(eventFlags_ & EventHandler::READ)) {
    assert(state_ == StateEnum::ESTABLISHED);
    assert((shutdownFlags_ & SHUT_READ) == 0);
    if (useIoUring_ && !iouRecvHandle_) {
      iouRecvHandle_ = IoUringRecvHandle::create(eventBase_, fd_, addr_, this);
      CHECK(iouRecvHandle_);
    }
    if (!updateEventRegistration(EventHandler::READ, 0)) {
      assert(state_ == StateEnum::ERROR);
      return;
    }
    checkForImmediateRead();
  } else if (readCallback_ == nullptr) {
    // Unregister for read events.
    updateEventRegistration(0, EventHandler::READ);
  }

  // If we have write requests pending, try to send them immediately.
  // Since we just finished accepting, there is a very good chance that we can
  // write without blocking.
  //
  // However, we only process them if EventHandler::WRITE is not already set,
  // which means that we're already blocked on a write attempt.  (This can
  // happen if connectSuccess() called write() before returning.)
  if (writeReqHead_ && !(eventFlags_ & EventHandler::WRITE)) {
    // Call handleWrite() to perform write processing.
    handleWrite();
  } else if (iouSendHandle_ && !iouSendHandle_->empty()) {
    updateEventRegistration(EventHandler::WRITE, 0);
  } else if (writeReqHead_ == nullptr) {
    // Unregister for write event.
    updateEventRegistration(0, EventHandler::WRITE);
  }
}

bool AsyncSocket::hasPendingWrites() noexcept {
  if (useIoUring_) {
    return iouSendHandle_ && !iouSendHandle_->empty();
  }

  return writeReqHead_ != nullptr;
}

void AsyncSocket::handleConnect() noexcept {
  VLOG(5) << "AsyncSocket::handleConnect() this=" << this << ", fd=" << fd_
          << ", state=" << state_;
  assert(state_ == StateEnum::CONNECTING);
  // SHUT_WRITE can never be set while we are still connecting;
  // SHUT_WRITE_PENDING may be set, be we only set SHUT_WRITE once the connect
  // finishes
  assert((shutdownFlags_ & SHUT_WRITE) == 0);

  if (!useIoUring_) {
    // In case we had a connect timeout, cancel the timeout
    writeTimeout_.cancelTimeout();
    // We don't use a persistent registration when waiting on a connect event,
    // so we have been automatically unregistered now.  Update eventFlags_ to
    // reflect reality.
    assert(eventFlags_ == EventHandler::WRITE);
    eventFlags_ = EventHandler::NONE;
  }

  // Call getsockopt() to check if the connect succeeded
  int error;
  socklen_t len = sizeof(error);
  int rv = netops_->getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &len);
  if (rv != 0) {
    auto errnoCopy = errno;
    AsyncSocketException ex(
        AsyncSocketException::INTERNAL_ERROR,
        withAddr("error calling getsockopt() after connect"),
        errnoCopy);
    VLOG(4) << "AsyncSocket::handleConnect(this=" << this << ", fd=" << fd_
            << " host=" << addr_.describe() << ") exception:" << ex.what();
    return failConnect(__func__, ex);
  }

  if (error != 0) {
    AsyncSocketException ex(
        AsyncSocketException::NOT_OPEN, "connect failed", error);
    VLOG(2) << "AsyncSocket::handleConnect(this=" << this << ", fd=" << fd_
            << " host=" << addr_.describe() << ") exception: " << ex.what();
    return failConnect(__func__, ex);
  }

  // Move into STATE_ESTABLISHED
  state_ = StateEnum::ESTABLISHED;
  if (useIoUring_ && netops_->set_socket_blocking(fd_)) {
    auto errnoCopy = errno;
    AsyncSocketException tex(
        AsyncSocketException::INTERNAL_ERROR,
        withAddr("failed to restore socket in blocking mode"),
        errnoCopy);
    return failConnect(__func__, tex);
  }

  // If SHUT_WRITE_PENDING is set and we don't have any write requests to
  // perform, immediately shutdown the write half of the socket.
  if ((shutdownFlags_ & SHUT_WRITE_PENDING) && !hasPendingWrites()) {
    // SHUT_READ shouldn't be set.  If close() is called on the socket while
    // we are still connecting we just abort the connect rather than waiting
    // for it to complete.
    assert((shutdownFlags_ & SHUT_READ) == 0);
    netops_->shutdown(fd_, SHUT_WR);
    shutdownFlags_ |= SHUT_WRITE;
  }

  VLOG(7) << "AsyncSocket " << this << ": fd " << fd_
          << "successfully connected; state=" << state_;

  // Remember the EventBase we are attached to, before we start invoking any
  // callbacks (since the callbacks may call detachEventBase()).
  EventBase* originalEventBase = eventBase_;

  invokeConnectSuccess();
  // Note that the connect callback may have changed our state.
  // (set or unset the read callback, called write(), closed the socket, etc.)
  // The following code needs to handle these situations correctly.
  //
  // If the socket has been closed, readCallback_ and writeReqHead_ will
  // always be nullptr, so that will prevent us from trying to read or write.
  //
  // The main thing to check for is if eventBase_ is still originalEventBase.
  // If not, we have been detached from this event base, so we shouldn't
  // perform any more operations.
  if (eventBase_ != originalEventBase) {
    return;
  }

  handleInitialReadWrite();
}

void AsyncSocket::timeoutExpired() noexcept {
  VLOG(7) << "AsyncSocket " << this << ", fd " << fd_ << ": timeout expired: "
          << "state=" << state_ << ", events=" << std::hex << eventFlags_;
  DestructorGuard dg(this);
  eventBase_->dcheckIsInEventBaseThread();

  if (state_ == StateEnum::CONNECTING) {
    // connect() timed out
    // Unregister for I/O events.
    if (connectCallback_) {
      AsyncSocketException ex(
          AsyncSocketException::TIMED_OUT,
          fmt::format("connect timed out after {}ms", connectTimeout_.count()));
      failConnect(__func__, ex);
    } else {
      // we faced a connect error without a connect callback, which could
      // happen due to TFO.
      AsyncSocketException ex(
          AsyncSocketException::TIMED_OUT, "write timed out during connection");
      failWrite(__func__, ex);
    }
  } else {
    // a normal write operation timed out
    AsyncSocketException ex(
        AsyncSocketException::TIMED_OUT,
        fmt::format("write timed out after {}ms", sendTimeout_));
    failWrite(__func__, ex);
  }
}

void AsyncSocket::handleNetworkSocketAttached() {
  VLOG(6) << "AsyncSocket::attachFd(this=" << this << ", fd=" << fd_
          << ", evb=" << eventBase_ << " , state=" << state_
          << ", events=" << std::hex << eventFlags_ << ")";

  // legacy observer support
  for (const auto& cb : lifecycleObservers_) {
    cb->fdAttach(this);
  }

  // folly::ObserverContainer observer support
  if (auto list = getAsyncSocketObserverContainer()) {
    list->invokeInterfaceMethodAllObservers([](auto observer, auto observed) {
      observer->fdAttach(observed);
    });
  }

  if (const auto shutdownSocketSet = wShutdownSocketSet_.lock()) {
    shutdownSocketSet->add(fd_);
  }
  ioHandler_.changeHandlerFD(fd_);
}

ssize_t AsyncSocket::tfoSendMsg(
    NetworkSocket fd, struct msghdr* msg, int msg_flags) {
  return detail::tfo_sendmsg(fd, msg, msg_flags);
}

AsyncSocket::WriteResult AsyncSocket::sendSocketMessage(
    const iovec* vec,
    size_t count,
    WriteFlags flags,
    WriteRequestTag writeTag) {
  // lambda to gather and merge PrewriteRequests from observers
  auto gatherAndMergePrewriteRequests =
      [this,
       vec,
       count,
       flags,
       maybeVecTotalBytes = folly::Optional<size_t>()]() mutable {
        AsyncSocketObserverInterface::PrewriteRequest mergedRequest = {};
        if (lifecycleObservers_.empty()) {
          return mergedRequest;
        }

        // determine total number of bytes in vec, reuse once determined
        if (!maybeVecTotalBytes.has_value()) {
          maybeVecTotalBytes = 0;
          for (size_t i = 0; i < count; ++i) {
            maybeVecTotalBytes.value() += vec[i].iov_len;
          }
        }
        auto& vecTotalBytes = maybeVecTotalBytes.value();

        // build our PrewriteState
        const auto startOffset = getRawBytesWritten();
        const auto endOffset = getRawBytesWritten() + vecTotalBytes - 1;
        const AsyncSocketObserverInterface::PrewriteState prewriteState = [&] {
          AsyncSocketObserverInterface::PrewriteState state = {};
          state.startOffset = startOffset;
          state.endOffset = endOffset;
          state.writeFlags = flags;
          state.ts = std::chrono::steady_clock::now();
          return state;
        }();

        // enable observers to add PrewriteRequests to container
        AsyncSocketObserverInterface::PrewriteRequestContainer
            prewriteRequestContainer(prewriteState);
        for (const auto& observer : lifecycleObservers_) {
          if (!observer->getConfig().prewrite) {
            continue;
          }
          observer->prewrite(this, prewriteState, prewriteRequestContainer);
        }

        return prewriteRequestContainer.getMergedRequest();
      };

  // lambda to prepare and send a message, and handle byte events
  // parameters have L at the end to prevent shadowing warning from gcc
  auto prepSendMsg =
      [this, writeTag = std::move(writeTag)](
          const iovec* vecL, const size_t countL, const WriteFlags flagsL) {
        const bool byteEventsEnabled =
            (byteEventHelper_ && byteEventHelper_->byteEventsEnabled &&
             !byteEventHelper_->maybeEx.has_value());

        struct msghdr msg = {};
        msg.msg_name = nullptr;
        msg.msg_namelen = 0;
        msg.msg_iov = const_cast<struct iovec*>(vecL);
        msg.msg_iovlen = std::min<size_t>(countL, kIovMax);
        msg.msg_flags = 0; // passed to sendSocketMessage below, it sets them
        msg.msg_control = nullptr;
        msg.msg_controllen = sendMsgParamCallback_->getAncillaryDataSize(
            flagsL, writeTag, byteEventsEnabled);
        CHECK_GE(
            AsyncSocket::SendMsgParamsCallback::maxAncillaryDataSize,
            msg.msg_controllen);

        if (msg.msg_controllen != 0) {
          msg.msg_control = reinterpret_cast<char*>(alloca(msg.msg_controllen));
          sendMsgParamCallback_->getAncillaryData(
              flagsL, msg.msg_control, writeTag, byteEventsEnabled);
        }

        const auto prewriteRawBytesWritten = getRawBytesWritten();
        int msg_flags =
            sendMsgParamCallback_->getFlags(flagsL, zeroCopyEnabled_);
        auto writeResult = sendSocketMessage(fd_, &msg, msg_flags);

        if (writeResult.writeReturn < 0 && zeroCopyEnabled_ &&
            errno == ENOBUFS) {
          // workaround for running with zerocopy enabled but without a big
          // enough memlock value - see ulimit -l
          zeroCopyEnabled_ = false;
          zeroCopyReenableCounter_ = zeroCopyReenableThreshold_;
          msg_flags = sendMsgParamCallback_->getFlags(flagsL, zeroCopyEnabled_);
          writeResult = sendSocketMessage(fd_, &msg, msg_flags);
        }

        if (writeResult.writeReturn > 0) {
          if (msg.msg_controllen != 0) {
            sendMsgParamCallback_->wroteBytes(writeTag);
          }
          if (byteEventsEnabled && isSet(flagsL, WriteFlags::TIMESTAMP_WRITE)) {
            CHECK_GT(
                getRawBytesWritten(), prewriteRawBytesWritten); // sanity check
            ByteEvent byteEvent = {};
            byteEvent.type = ByteEvent::Type::WRITE;
            byteEvent.offset = getRawBytesWritten() - 1;
            byteEvent.maybeRawBytesWritten = writeResult.writeReturn;
            byteEvent.maybeRawBytesTriedToWrite = 0;
            for (size_t i = 0; i < countL; ++i) {
              byteEvent.maybeRawBytesTriedToWrite.value() += vecL[i].iov_len;
            }
            byteEvent.maybeWriteFlags = flagsL;
            for (const auto& observer : lifecycleObservers_) {
              if (observer->getConfig().byteEvents) {
                observer->byteEvent(this, byteEvent);
              }
            }
          }
        }

        return writeResult;
      };

  // get PrewriteRequests (if any), merge flags with write flags
  const auto prewriteRequest = gatherAndMergePrewriteRequests();
  auto mergedFlags = flags | prewriteRequest.writeFlagsToAdd |
      prewriteRequest.writeFlagsToAddAtOffset;

  // if no PrewriteRequests, or none requiring the write to be split, proceed
  if (!prewriteRequest.maybeOffsetToSplitWrite.has_value()) {
    return prepSendMsg(vec, count, mergedFlags);
  }

  // we need to split the write...
  // add CORK flag to inform the OS that more data is on the way...
  mergedFlags |= WriteFlags::CORK;

  // TODO(bschlinker): When prewrite splits a write, try to continue writing
  // after a write returns; this will improve efficiency.
  const auto splitWriteAtOffset = *prewriteRequest.maybeOffsetToSplitWrite;
  if (count <= kSmallIoVecSize) {
    // suppress "warning: variable length array 'vec' is used [-Wvla]"
    FOLLY_PUSH_WARNING
    FOLLY_GNU_DISABLE_WARNING("-Wvla")
    iovec tmpVec[FOLLY_DETAIL_VLA_SIZE(count, kSmallIoVecSize)];
    FOLLY_POP_WARNING

    size_t tmpVecCount = count;
    splitIovecArray(
        0,
        splitWriteAtOffset - getRawBytesWritten(),
        vec,
        count,
        tmpVec,
        tmpVecCount);
    return prepSendMsg(tmpVec, tmpVecCount, mergedFlags);
  } else {
    auto tmpVecPtr = std::make_unique<iovec[]>(count);
    auto tmpVec = tmpVecPtr.get();
    size_t tmpVecCount = count;
    splitIovecArray(
        0,
        splitWriteAtOffset - getRawBytesWritten(),
        vec,
        count,
        tmpVec,
        tmpVecCount);
    return prepSendMsg(tmpVec, tmpVecCount, mergedFlags);
  }
}

AsyncSocket::WriteResult AsyncSocket::sendSocketMessage(
    NetworkSocket fd, struct msghdr* msg, int msg_flags) {
  ssize_t totalWritten = 0;
  SCOPE_EXIT {
    if (totalWritten > 0) {
      rawBytesWritten_ += totalWritten;
    }
  };
  if (state_ == StateEnum::FAST_OPEN) {
    sockaddr_storage addr{};
    auto len = addr_.getAddress(&addr);
    msg->msg_name = &addr;
    msg->msg_namelen = len;
    totalWritten = tfoSendMsg(fd_, msg, msg_flags);
    if (totalWritten >= 0) {
      tfoInfo_.finished = true;
      state_ = StateEnum::ESTABLISHED;
      if (useIoUring_ && netops_->set_socket_blocking(fd_)) {
        auto errnoCopy = errno;
        AsyncSocketException ex(
            AsyncSocketException::INTERNAL_ERROR,
            withAddr("failed to restore socket in blocking mode"),
            errnoCopy);
        return WriteResult(
            WRITE_ERROR, std::make_unique<AsyncSocketException>(ex));
      }
      // We schedule this asynchronously so that we don't end up
      // invoking initial read or write while a write is in progress.
      scheduleInitialReadWrite();
    } else if (errno == EINPROGRESS) {
      VLOG(4) << "TFO falling back to connecting";
      // A normal sendmsg doesn't return EINPROGRESS, however
      // TFO might fallback to connecting if there is no
      // cookie.
      state_ = StateEnum::CONNECTING;
      try {
        if (!useIoUring_) {
          scheduleConnectTimeout();
          registerForConnectEvents();
        } else {
          iouConnectHandle_ = IoUringConnectHandle::create(
              eventBase_, fd_, this, connectTimeout_);
        }
      } catch (const AsyncSocketException& ex) {
        return WriteResult(
            WRITE_ERROR, std::make_unique<AsyncSocketException>(ex));
      }
      // Let's fake it that no bytes were written and return an errno.
      errno = EAGAIN;
      totalWritten = -1;
    } else if (errno == EOPNOTSUPP) {
      // Try falling back to connecting.
      VLOG(4) << "TFO not supported";
      state_ = StateEnum::CONNECTING;
      try {
        int ret = socketConnect((const sockaddr*)&addr, len);
        if (ret == 0) {
          // connect succeeded immediately
          // Treat this like no data was written.
          state_ = StateEnum::ESTABLISHED;
          if (useIoUring_ && netops_->set_socket_blocking(fd_)) {
            auto errnoCopy = errno;
            AsyncSocketException ex(
                AsyncSocketException::INTERNAL_ERROR,
                withAddr("failed to restore socket in blocking mode"),
                errnoCopy);
            return WriteResult(
                WRITE_ERROR, std::make_unique<AsyncSocketException>(ex));
          }
          scheduleInitialReadWrite();
        }
        // If there was no exception during connections,
        // we would return that no bytes were written.
        errno = EAGAIN;
        totalWritten = -1;
      } catch (const AsyncSocketException& ex) {
        return WriteResult(
            WRITE_ERROR, std::make_unique<AsyncSocketException>(ex));
      }
    } else if (errno == EAGAIN) {
      // Normally sendmsg would indicate that the write would block.
      // However in the fast open case, it would indicate that sendmsg
      // fell back to a connect. This is a return code from connect()
      // instead, and is an error condition indicating no fds available.
      return WriteResult(
          WRITE_ERROR,
          std::make_unique<AsyncSocketException>(
              AsyncSocketException::UNKNOWN, "No more free local ports"));
    }
  } else {
    totalWritten = netops_->sendmsg(fd, msg, msg_flags);
  }
  return WriteResult(totalWritten);
}

AsyncSocket::WriteResult AsyncSocket::performWrite(
    const iovec* vec,
    uint32_t count,
    WriteFlags flags,
    uint32_t* countWritten,
    uint32_t* partialWritten,
    WriteRequestTag writeTag) {
  auto writeResult = sendSocketMessage(vec, count, flags, std::move(writeTag));
  auto totalWritten = writeResult.writeReturn;
  if (totalWritten < 0) {
    bool tryAgain = (errno == EAGAIN);
#ifdef __APPLE__
    // Apple has a bug where doing a second write on a socket which we
    // have opened with TFO causes an ENOTCONN to be thrown. However the
    // socket is really connected, so treat ENOTCONN as a EAGAIN until
    // this bug is fixed.
    tryAgain |= (errno == ENOTCONN);
#endif

    if (!writeResult.exception && tryAgain) {
      // TCP buffer is full; we can't write any more data right now.
      *countWritten = 0;
      *partialWritten = 0;
      return WriteResult(0);
    }
    // error
    *countWritten = 0;
    *partialWritten = 0;
    return writeResult;
  }

  appBytesWritten_ += totalWritten;

  uint32_t bytesWritten;
  uint32_t n;
  for (bytesWritten = uint32_t(totalWritten), n = 0; n < count; ++n) {
    const iovec* v = vec + n;
    if (v->iov_len > bytesWritten) {
      // Partial write finished in the middle of this iovec
      *countWritten = n;
      *partialWritten = bytesWritten;
      return WriteResult(totalWritten);
    }

    bytesWritten -= uint32_t(v->iov_len);
  }

  assert(bytesWritten == 0);
  *countWritten = n;
  *partialWritten = 0;
  return WriteResult(totalWritten);
}

/**
 * Re-register the EventHandler after eventFlags_ has changed.
 *
 * If an error occurs, fail() is called to move the socket into the error
 * state and call all currently installed callbacks.  After an error, the
 * AsyncSocket is completely unregistered.
 *
 * @return Returns true on success, or false on error.
 */
bool AsyncSocket::updateEventRegistration() {
  VLOG(5) << "AsyncSocket::updateEventRegistration(this=" << this
          << ", fd=" << fd_ << ", evb=" << eventBase_ << ", state=" << state_
          << ", events=" << std::hex << eventFlags_;
  if (useIoUring_) {
    if (iouSendHandle_) {
      iouSendHandle_->update(eventFlags_);
    }
    if (iouRecvHandle_) {
      iouRecvHandle_->update(eventFlags_);
    }
    return true;
  }

  if (eventFlags_ == EventHandler::NONE) {
    if (ioHandler_.isHandlerRegistered()) {
      DCHECK(eventBase_ != nullptr);
      eventBase_->dcheckIsInEventBaseThread();
    }
    ioHandler_.unregisterHandler();
    return true;
  }

  eventBase_->dcheckIsInEventBaseThread();

  // Always register for persistent events, so we don't have to re-register
  // after being called back.
  if (!ioHandler_.registerHandler(
          uint16_t(eventFlags_ | EventHandler::PERSIST))) {
    eventFlags_ = EventHandler::NONE; // we're not registered after error
    AsyncSocketException ex(
        AsyncSocketException::INTERNAL_ERROR,
        withAddr("failed to update AsyncSocket event registration"));
    fail("updateEventRegistration", ex);
    return false;
  }

  return true;
}

bool AsyncSocket::updateEventRegistration(uint16_t enable, uint16_t disable) {
  uint16_t oldFlags = eventFlags_;
  eventFlags_ |= enable;
  eventFlags_ &= ~disable;
  if (eventFlags_ == oldFlags) {
    return true;
  } else {
    return updateEventRegistration();
  }
}

void AsyncSocket::startFail() {
  // startFail() should only be called once
  assert(state_ != StateEnum::ERROR);
  assert(getDestructorGuardCount() > 0);
  state_ = StateEnum::ERROR;
  // Ensure that SHUT_READ and SHUT_WRITE are set,
  // so all future attempts to read or write will be rejected
  shutdownFlags_ |= (SHUT_READ | SHUT_WRITE);

  // Cancel any scheduled immediate read.
  if (immediateReadHandler_.isLoopCallbackScheduled()) {
    immediateReadHandler_.cancelLoopCallback();
  }

  if (eventFlags_ != EventHandler::NONE) {
    eventFlags_ = EventHandler::NONE;
    if (iouSendHandle_) {
      iouSendHandle_->update(eventFlags_);
    }
    if (iouRecvHandle_) {
      iouRecvHandle_->update(eventFlags_);
    }
    ioHandler_.unregisterHandler();
  }
  writeTimeout_.cancelTimeout();

  if (fd_ != NetworkSocket()) {
    ioHandler_.changeHandlerFD(NetworkSocket());
    doClose();
  }
}

void AsyncSocket::invokeAllErrors(const AsyncSocketException& ex) {
  invokeConnectErr(ex);
  failAllWrites(ex);

  if (readCallback_) {
    ReadCallback* callback = readCallback_;
    readCallback_ = nullptr;
    callback->readErr(ex);
  }
}

void AsyncSocket::finishFail() {
  assert(state_ == StateEnum::ERROR);
  assert(getDestructorGuardCount() > 0);

  AsyncSocketException ex(
      AsyncSocketException::INTERNAL_ERROR,
      withAddr("socket closing after error"));
  invokeAllErrors(ex);
}

void AsyncSocket::finishFail(const AsyncSocketException& ex) {
  assert(state_ == StateEnum::ERROR);
  assert(getDestructorGuardCount() > 0);
  invokeAllErrors(ex);
}

void AsyncSocket::fail(const char* fn, const AsyncSocketException& ex) {
  VLOG(4) << "AsyncSocket(this=" << this << ", fd=" << fd_
          << ", state=" << state_ << " host=" << addr_.describe()
          << "): failed in " << fn << "(): " << ex.what();
  startFail();
  finishFail(ex);
}

void AsyncSocket::failConnect(const char* fn, const AsyncSocketException& ex) {
  VLOG(5) << "AsyncSocket(this=" << this << ", fd=" << fd_
          << ", state=" << state_ << " host=" << addr_.describe()
          << "): failed while connecting in " << fn << "(): " << ex.what();
  startFail();

  invokeConnectErr(ex);
  finishFail(ex);
}

AsyncSocket::ReadCode AsyncSocket::failRead(
    const char* fn, const AsyncSocketException& ex) {
  VLOG(5) << "AsyncSocket(this=" << this << ", fd=" << fd_
          << ", state=" << state_ << " host=" << addr_.describe()
          << "): failed while reading in " << fn << "(): " << ex.what();
  startFail();

  if (readCallback_ != nullptr) {
    ReadCallback* callback = readCallback_;
    readCallback_ = nullptr;
    callback->readErr(ex);
  }

  finishFail(ex);

  // done handling the error, we can exit the loop
  return AsyncSocket::ReadCode::READ_DONE;
}

void AsyncSocket::failErrMessageRead(
    const char* fn, const AsyncSocketException& ex) {
  VLOG(5) << "AsyncSocket(this=" << this << ", fd=" << fd_
          << ", state=" << state_ << " host=" << addr_.describe()
          << "): failed while reading message in " << fn << "(): " << ex.what();
  startFail();

  if (errMessageCallback_ != nullptr) {
    ErrMessageCallback* callback = errMessageCallback_;
    errMessageCallback_ = nullptr;
    callback->errMessageError(ex);
  }

  finishFail(ex);
}

void AsyncSocket::failWrite(const char* fn, const AsyncSocketException& ex) {
  VLOG(5) << "AsyncSocket(this=" << this << ", fd=" << fd_
          << ", state=" << state_ << " host=" << addr_.describe()
          << "): failed while writing in " << fn << "(): " << ex.what();
  startFail();

  if (iouSendHandle_) {
    assert(writeReqHead_ == nullptr);
    iouSendHandle_->failWrite(ex);
  }

  // Only invoke the first write callback, since the error occurred while
  // writing this request.  Let any other pending write callbacks be invoked
  // in finishFail().
  if (writeReqHead_ != nullptr) {
    WriteRequest* req = writeReqHead_;
    writeReqHead_ = req->getNext();
    WriteCallback* callback = req->getCallback();
    uint32_t bytesWritten = req->getTotalBytesWritten();
    req->destroy();
    if (callback) {
      callback->writeErr(bytesWritten, ex);
    }
  }

  finishFail(ex);
}

void AsyncSocket::failWrite(
    const char* fn,
    WriteCallback* callback,
    size_t bytesWritten,
    const AsyncSocketException& ex) {
  // This version of failWrite() is used when the failure occurs before
  // we've added the callback to writeReqHead_.
  VLOG(4) << "AsyncSocket(this=" << this << ", fd=" << fd_
          << ", state=" << state_ << " host=" << addr_.describe()
          << "): failed while writing in " << fn << "(): " << ex.what();
  if (closeOnFailedWrite_) {
    startFail();
  }

  if (callback != nullptr) {
    callback->writeErr(bytesWritten, ex);
  }

  if (closeOnFailedWrite_) {
    finishFail(ex);
  }
}

void AsyncSocket::failAllWrites(const AsyncSocketException& ex) {
  if (iouSendHandle_) {
    assert(writeReqHead_ == nullptr);
    assert(iouSendHandle_ != nullptr);
    iouSendHandle_->failAllWrites(ex);
  }
  // Invoke writeError() on all write callbacks.
  // This is used when writes are forcibly shutdown with write requests
  // pending, or when an error occurs with writes pending.
  while (writeReqHead_ != nullptr) {
    WriteRequest* req = writeReqHead_;
    writeReqHead_ = req->getNext();
    WriteCallback* callback = req->getCallback();
    if (callback) {
      callback->writeErr(req->getTotalBytesWritten(), ex);
    }
    req->destroy();
  }

  // All pending writes have failed - reset totalAppBytesScheduledForWrite_
  totalAppBytesScheduledForWrite_ = appBytesWritten_;
}

void AsyncSocket::failByteEvents(const AsyncSocketException& ex) {
  CHECK(byteEventHelper_) << "failByteEvents called without ByteEventHelper";
  byteEventHelper_->maybeEx = ex;
  // inform any observers that want ByteEvents
  for (const auto& observer : lifecycleObservers_) {
    if (observer->getConfig().byteEvents) {
      observer->byteEventsUnavailable(this, ex);
    }
  }
}

void AsyncSocket::invalidState(ConnectCallback* callback) {
  VLOG(5) << "AsyncSocket(this=" << this << ", fd=" << fd_
          << "): connect() called in invalid state " << state_;

  /*
   * The invalidState() methods don't use the normal failure mechanisms,
   * since we don't know what state we are in.  We don't want to call
   * startFail()/finishFail() recursively if we are already in the middle of
   * cleaning up.
   */

  AsyncSocketException ex(
      AsyncSocketException::ALREADY_OPEN,
      "connect() called with socket in invalid state");
  connectEndTime_ = std::chrono::steady_clock::now();
  if ((state_ == StateEnum::CONNECTING) || (state_ == StateEnum::ERROR)) {
    // legacy observer support
    for (const auto& cb : lifecycleObservers_) {
      // inform any lifecycle observes that the connection failed
      cb->connectError(this, ex);
    }

    // folly::ObserverContainer observer support
    if (auto list = getAsyncSocketObserverContainer()) {
      list->invokeInterfaceMethodAllObservers(
          [ex](auto observer, auto observed) {
            observer->connectError(observed, ex);
          });
    }
  }
  if (state_ == StateEnum::CLOSED || state_ == StateEnum::ERROR) {
    if (callback) {
      callback->connectErr(ex);
    }
  } else {
    // We can't use failConnect() here since connectCallback_
    // may already be set to another callback.  Invoke this ConnectCallback
    // here; any other connectCallback_ will be invoked in finishFail()
    startFail();
    if (callback) {
      callback->connectErr(ex);
    }
    finishFail(ex);
  }
}

void AsyncSocket::invalidState(ErrMessageCallback* callback) {
  VLOG(4) << "AsyncSocket(this=" << this << ", fd=" << fd_
          << "): setErrMessageCB(" << callback << ") called in invalid state "
          << state_;

  AsyncSocketException ex(
      AsyncSocketException::NOT_OPEN,
      msgErrQueueSupported
          ? "setErrMessageCB() called with socket in invalid state"
          : "This platform does not support socket error message notifications");
  if (state_ == StateEnum::CLOSED || state_ == StateEnum::ERROR) {
    if (callback) {
      callback->errMessageError(ex);
    }
  } else {
    startFail();
    if (callback) {
      callback->errMessageError(ex);
    }
    finishFail(ex);
  }
}

void AsyncSocket::invokeConnectErr(const AsyncSocketException& ex) {
  VLOG(5) << "AsyncSocket(this=" << this << ", fd=" << fd_
          << "): connect err invoked with ex: " << ex.what();
  connectEndTime_ = std::chrono::steady_clock::now();
  if ((state_ == StateEnum::CONNECTING) || (state_ == StateEnum::ERROR)) {
    // invokeConnectErr() can be invoked when state is {FAST_OPEN, CLOSED,
    // ESTABLISHED} (!?) and a bunch of other places that are not what this
    // call back wants. This seems like a bug but work around here while we
    // explore it independently

    // legacy observer support
    for (const auto& cb : lifecycleObservers_) {
      cb->connectError(this, ex);
    }

    // folly::ObserverContainer observer support
    if (auto list = getAsyncSocketObserverContainer()) {
      list->invokeInterfaceMethodAllObservers(
          [ex](auto observer, auto observed) {
            observer->connectError(observed, ex);
          });
    }
  }
  if (connectCallback_) {
    ConnectCallback* callback = connectCallback_;
    connectCallback_ = nullptr;
    callback->connectErr(ex);
  }
}

void AsyncSocket::invokeConnectSuccess() {
  VLOG(5) << "AsyncSocket(this=" << this << ", fd=" << fd_
          << "): connect success invoked";
  connectEndTime_ = std::chrono::steady_clock::now();
  maybeConnectionEstablishTime_ = connectEndTime_;
  bool enableByteEventsForObserver = false;

  // legacy observer support
  for (const auto& cb : lifecycleObservers_) {
    cb->connectSuccess(this);
    enableByteEventsForObserver |= ((cb->getConfig().byteEvents) ? 1 : 0);
  }

  // folly::ObserverContainer observer support
  if (auto list = getAsyncSocketObserverContainer()) {
    list->invokeInterfaceMethodAllObservers([](auto observer, auto observed) {
      observer->connectSuccess(observed);
    });
  }

  if (enableByteEventsForObserver) {
    enableByteEvents();
  }
  if (connectCallback_) {
    ConnectCallback* callback = connectCallback_;
    connectCallback_ = nullptr;
    callback->connectSuccess();
  }
}

void AsyncSocket::invokeConnectAttempt() {
  VLOG(5) << "AsyncSocket(this=" << this << ", fd=" << fd_
          << "): connect attempt";
  // legacy observer support
  for (const auto& cb : lifecycleObservers_) {
    cb->connectAttempt(this);
  }

  // folly::ObserverContainer observer support
  if (auto list = getAsyncSocketObserverContainer()) {
    list->invokeInterfaceMethodAllObservers([](auto observer, auto observed) {
      observer->connectAttempt(observed);
    });
  }
}

void AsyncSocket::invalidState(ReadCallback* callback) {
  VLOG(4) << "AsyncSocket(this=" << this << ", fd=" << fd_
          << "): setReadCallback(" << callback << ") called in invalid state "
          << state_;

  AsyncSocketException ex(
      AsyncSocketException::NOT_OPEN,
      "setReadCallback() called with socket in "
      "invalid state");
  if (state_ == StateEnum::CLOSED || state_ == StateEnum::ERROR) {
    if (callback) {
      callback->readErr(ex);
    }
  } else {
    startFail();
    if (callback) {
      callback->readErr(ex);
    }
    finishFail(ex);
  }
}

void AsyncSocket::invalidState(WriteCallback* callback) {
  VLOG(4) << "AsyncSocket(this=" << this << ", fd=" << fd_
          << "): write() called in invalid state " << state_;

  AsyncSocketException ex(
      AsyncSocketException::NOT_OPEN,
      withAddr("write() called with socket in invalid state"));
  if (state_ == StateEnum::CLOSED || state_ == StateEnum::ERROR) {
    if (callback) {
      callback->writeErr(0, ex);
    }
  } else {
    startFail();
    if (callback) {
      callback->writeErr(0, ex);
    }
    finishFail(ex);
  }
}

void AsyncSocket::doClose() {
  // legacy observer support
  for (const auto& cb : lifecycleObservers_) {
    cb->close(this);
  }

  // folly::ObserverContainer support
  if (auto list = getAsyncSocketObserverContainer()) {
    list->invokeInterfaceMethodAllObservers([](auto observer, auto observed) {
      observer->close(observed);
    });
  }

  if (fd_ == NetworkSocket()) {
    return;
  }

  drainZeroCopyQueue();

  if (!idZeroCopyBufPtrMap_.empty()) {
    zeroCopyDrainDeadline_ = std::chrono::steady_clock::now() +
        zeroCopyDrainConfig_.maxDrainDuration;
    errMessageCallback_ = nullptr;
    auto lifecycleObservers = std::exchange(lifecycleObservers_, {});
    for (const auto& cb : lifecycleObservers) {
      cb->destroy(this);
    }
    scheduleZeroCopyDrain();
    return;
  }

  closeNetworkSocket();
}

void AsyncSocket::closeNetworkSocket() {
  if (fd_ == NetworkSocket()) {
    return;
  }
  if (const auto shutdownSocketSet = wShutdownSocketSet_.lock()) {
    shutdownSocketSet->close(fd_);
  } else {
    netops_->close(fd_);
  }
  fd_ = NetworkSocket();
}

std::ostream& operator<<(
    std::ostream& os, const AsyncSocket::StateEnum& state) {
  os << static_cast<int>(state);
  return os;
}

std::string AsyncSocket::withAddr(folly::StringPiece s) {
  // Don't use addr_ directly because it may not be initialized
  // e.g. if constructed from fd
  folly::SocketAddress peer, local;
  try {
    getLocalAddress(&local);
  } catch (...) {
    // ignore
  }
  try {
    getPeerAddress(&peer);
  } catch (...) {
    // ignore
  }

  return fmt::format(
      "{} (peer={}{})",
      s,
      peer.describe(),
      kIsMobile ? "" : fmt::format(", local={}", local.describe()));
}

void AsyncSocket::setTosOrTrafficClass(int tosOrTrafficClass) {
#if defined(_WIN32)
  (void)tosOrTrafficClass;
  throw AsyncSocketException(
      AsyncSocketException::INTERNAL_ERROR,
      withAddr("setting tos or traffic class not supported on windows"));
#else
  tosOrTrafficClass_ = tosOrTrafficClass;
  cachePeerAddress();
  if (!addr_.isInitialized()) {
    return;
  }
  auto family = addr_.getFamily();
  int ret = 0;
  if (family == AF_INET6) {
    // For IPv6 set the traffic class field
    ret = netops_->setsockopt(
        fd_,
        IPPROTO_IPV6,
        IPV6_TCLASS,
        &tosOrTrafficClass,
        sizeof(tosOrTrafficClass));
  } else {
    // For IPv4 set the TOS field
    ret = netops_->setsockopt(
        fd_, IPPROTO_IP, IP_TOS, &tosOrTrafficClass, sizeof(tosOrTrafficClass));
  }
  if (ret != 0) {
    auto errnoCopy = errno;
    throw AsyncSocketException(
        AsyncSocketException::INTERNAL_ERROR,
        withAddr("failed to set tos"),
        errnoCopy);
  }
#endif
}

void AsyncSocket::setBufferCallback(BufferCallback* cb) {
  bufferCallback_ = cb;
}

void AsyncSocket::connectSuccess() {
  DestructorGuard dg(this);
  iouConnectHandle_.reset();
  handleConnect();
}

void AsyncSocket::connectTimeout() {
  DestructorGuard dg(this);
  if (iouConnectHandle_->cancel()) {
    iouConnectHandle_.release();
  } else {
    iouConnectHandle_.reset();
  }
  timeoutExpired();
}

void AsyncSocket::sendPartial(size_t bytesWritten) {
  DestructorGuard dg(this);
  // Only non-zero for io_uring.
  appBytesWritten_ += bytesWritten;
  rawBytesWritten_ += bytesWritten;

  if (bufferCallback_) {
    bufferCallback_->onEgressBuffered();
  }
  // Stop after a partial write; it's highly likely that a subsequent
  // write attempt will just return EAGAIN.
  //
  // Ensure that we are registered for write events.
  if (!useIoUring_ && (eventFlags_ & EventHandler::WRITE) == 0) {
    if (!updateEventRegistration(EventHandler::WRITE, 0)) {
      assert(state_ == StateEnum::ERROR);
      return;
    }
  }

  // Reschedule the send timeout, since we have made some write progress.
  if (sendTimeout_ > 0) {
    if (!writeTimeout_.scheduleTimeout(sendTimeout_)) {
      AsyncSocketException ex(
          AsyncSocketException::INTERNAL_ERROR,
          withAddr("failed to reschedule write timeout"));
      return failWrite(__func__, ex);
    }
  }
  return;
}

void AsyncSocket::sendDone(size_t bytesWritten) {
  DestructorGuard dg(this);
  // Only non-zero for io_uring.
  appBytesWritten_ += bytesWritten;
  rawBytesWritten_ += bytesWritten;

  // This is the last write request.
  // Unregister for write events and cancel the send timer
  // before we invoke the callback.  We have to update the state
  // properly before calling the callback, since it may want to detach
  // us from the EventBase.
  if (eventFlags_ & EventHandler::WRITE) {
    if (!updateEventRegistration(0, EventHandler::WRITE)) {
      assert(state_ == StateEnum::ERROR);
      return;
    }
    // Stop the send timeout
    writeTimeout_.cancelTimeout();
  }
  assert(!writeTimeout_.isScheduled());

  // If SHUT_WRITE_PENDING is set, we should shutdown the socket after
  // we finish sending the last write request.
  //
  // We have to do this before invoking writeSuccess(), since
  // writeSuccess() may detach us from our EventBase.
  if (shutdownFlags_ & SHUT_WRITE_PENDING) {
    assert(connectCallback_ == nullptr);
    shutdownFlags_ |= SHUT_WRITE;

    if (shutdownFlags_ & SHUT_READ) {
      // Reads have already been shutdown.  Fully close the socket and
      // move to STATE_CLOSED.
      //
      // Note: This code currently moves us to STATE_CLOSED even if
      // close() hasn't ever been called.  This can occur if we have
      // received EOF from the peer and shutdownWrite() has been called
      // locally.  Should we bother staying in STATE_ESTABLISHED in this
      // case, until close() is actually called?  I can't think of a
      // reason why we would need to do so.  No other operations besides
      // calling close() or destroying the socket can be performed at
      // this point.
      assert(readCallback_ == nullptr);
      state_ = StateEnum::CLOSED;
      if (fd_ != NetworkSocket()) {
        ioHandler_.changeHandlerFD(NetworkSocket());
        doClose();
      }
    } else {
      // Reads are still enabled, so we are only doing a half-shutdown
      netops_->shutdown(fd_, SHUT_WR);
    }
  }

  if (!hasPendingWrites() && bufferCallback_) {
    bufferCallback_->onEgressBufferCleared();
  }
}

void AsyncSocket::sendErr(int err) {
  DestructorGuard dg(this);
  AsyncSocketException ex(
      AsyncSocketException::INTERNAL_ERROR,
      withAddr("async sendmsg() failed"),
      err);
  failWrite(__func__, ex);
}

void AsyncSocket::recvSuccess(std::unique_ptr<IOBuf> buf) {
  DestructorGuard dg(this);
  DCHECK(readAncillaryDataCallback_ == nullptr);
  CHECK(readCallback_);

  if (preReceivedData_ && !preReceivedData_->empty()) {
    preReceivedData_->appendToChain(std::move(buf));
    auto bytes = preReceivedData_->computeChainDataLength();
    readCallback_->readBufferAvailable(std::move(preReceivedData_));
    appBytesReceived_ += bytes;
    return;
  }

  auto bytes = buf->computeChainDataLength();
  readCallback_->readBufferAvailable(std::move(buf));
  appBytesReceived_ += bytes;
}

void AsyncSocket::recvEOF() noexcept {
  DestructorGuard dg(this);
  readErr_ = READ_EOF;
  // EOF
  shutdownFlags_ |= SHUT_READ;
  if (!updateEventRegistration(0, EventHandler::READ)) {
    // we've already been moved into STATE_ERROR
    assert(state_ == StateEnum::ERROR);
    assert(readCallback_ == nullptr);
    return;
  }

  if (readCallback_ != nullptr) {
    ReadCallback* callback = readCallback_;
    readCallback_ = nullptr;
    callback->readEOF();
  }
}

void AsyncSocket::recvErr(
    int err, std::unique_ptr<const AsyncSocketException> exception) noexcept {
  DestructorGuard dg(this);
  readErr_ = READ_ERROR;
  if (exception) {
    failRead(__func__, *exception);
    return;
  }
  AsyncSocketException ex(
      AsyncSocketException::INTERNAL_ERROR, withAddr("recv() failed"), err);
  failRead(__func__, ex);
}

void AsyncSocket::recvBuffersScarce(bool scarce) noexcept {
  DestructorGuard dg(this);
  if (readCallback_) {
    readCallback_->readBuffersScarce(scarce);
  }
}

std::ostream& operator<<(
    std::ostream& os, const folly::AsyncSocket::WriteRequestTag& tag) {
  os << tag.buf_;
  return os;
}

} // namespace folly
