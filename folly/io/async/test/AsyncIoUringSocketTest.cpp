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

#include <array>
#include <chrono>
#include <map>
#include <random>
#include <string_view>
#include <vector>

#include <folly/FileUtil.h>
#include <folly/Subprocess.h>
#include <folly/executors/GlobalExecutor.h>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/io/async/AsyncIoUringSocket.h>
#include <folly/io/async/AsyncServerSocket.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/IoUringBackend.h>
#include <folly/io/async/IoUringEvent.h>
#include <folly/io/async/test/AsyncIoUringSocketTestHelper.h>
#include <folly/io/async/test/AsyncSocketTest.h>
#include <folly/io/async/test/AsyncSocketTest2.h>
#include <folly/portability/GTest.h>
#include <folly/portability/Sockets.h>
#include <folly/portability/Unistd.h>
#include <folly/system/Shell.h>
#include <folly/test/SocketAddressTestHelper.h>

namespace folly {

namespace {

class FutureWriteCallback : public AsyncWriter::WriteCallback {
 public:
  void writeSuccess() noexcept override {
    auto& [promise, future] = promiseContract;
    promise.setValue(Unit{});
  }

  void writeErr(
      size_t bytesWritten, const AsyncSocketException& ex) noexcept override {
    auto& [promise, future] = promiseContract;
    promise.setValue(makeUnexpected(std::make_pair(bytesWritten, ex)));
  }

  using TResult = Expected<Unit, std::pair<size_t, AsyncSocketException>>;
  SemiPromiseContract<TResult> promiseContract = makePromiseContract<TResult>();
};

} // namespace

class ScarcityReadCallback : public AsyncReader::ReadCallback {
 public:
  enum class Event {
    BuffersAvailable,
    BuffersScarce,
    BufferDelivered,
  };

  bool isBufferMovable() noexcept override { return true; }

  void readBuffersScarce(bool scarce) noexcept override {
    events.push_back(scarce ? Event::BuffersScarce : Event::BuffersAvailable);
    if (scarce && socketToDetach) {
      socketToDetach->setReadCB(nullptr);
    }
  }

  void readBufferAvailable(std::unique_ptr<IOBuf>) noexcept override {
    events.push_back(Event::BufferDelivered);
  }

  void getReadBuffer(void**, size_t*) override {}
  void readDataAvailable(size_t) noexcept override {}
  void readEOF() noexcept override {}
  void readErr(const AsyncSocketException&) noexcept override {}

  std::vector<Event> events;
  AsyncIoUringSocket* socketToDetach{nullptr};
};

TEST_P(AsyncIoUringSocketTest, ConnectTimeout) {
  MAYBE_SKIP();
  struct CB : AsyncSocket::ConnectCallback {
    void connectSuccess() noexcept override {
      prom.setValue(makeExpected<AsyncSocketException>(Unit{}));
    }
    void connectErr(const AsyncSocketException& ex) noexcept override {
      prom.setValue(makeUnexpected(ex));
    }
    Promise<Expected<Unit, AsyncSocketException>> prom;
  } cb;

  // Try connecting to server that won't respond.
  //
  // This depends somewhat on the network where this test is run.
  // Hopefully this IP will be routable but unresponsive.
  // (Alternatively, we could try listening on a local raw socket, but that
  // normally requires root privileges.)
  auto host = SocketAddressTestHelper::isIPv6Enabled()
      ? SocketAddressTestHelper::kGooglePublicDnsAAddrIPv6
      : SocketAddressTestHelper::isIPv4Enabled()
      ? SocketAddressTestHelper::kGooglePublicDnsAAddrIPv4
      : nullptr;

  AsyncIoUringSocket::UniquePtr socket(new AsyncIoUringSocket(base.get()));
  socket->connect(
      &cb, SocketAddress{host, 65535}, std::chrono::milliseconds(1));

  auto res =
      cb.prom.getSemiFuture()
          .within(kTimeout)
          .via(base.get())
          .getVia(base.get());
  ASSERT_FALSE(res);
  if (res.error().getType() == AsyncSocketException::NOT_OPEN) {
    // This can happen if we could not route to the IP address picked above.
    // In this case the connect will fail immediately rather than timing out.
    // Just skip the test in this case.
    GTEST_SKIP() << "do not have a routable but unreachable IP address";
  }
  EXPECT_EQ(res.error().getType(), AsyncSocketException::TIMED_OUT)
      << res.error().what();
}

TEST_P(AsyncIoUringSocketTest, ZeroCopyReadNotifiesScarcityBeforeBuffer) {
  MAYBE_SKIP();
  AsyncIoUringSocket::UniquePtr socket(new AsyncIoUringSocket(base.get()));
  ScarcityReadCallback callback;
  socket->readSqe_->setReadCallback(&callback, false);

  socket->readSqe_->sendZeroCopyReadBuf(IOBuf::copyBuffer("a"), false);
  socket->readSqe_->sendZeroCopyReadBuf(IOBuf::copyBuffer("b"), true);

  const std::vector<ScarcityReadCallback::Event> expected = {
      ScarcityReadCallback::Event::BuffersAvailable,
      ScarcityReadCallback::Event::BufferDelivered,
      ScarcityReadCallback::Event::BuffersScarce,
      ScarcityReadCallback::Event::BufferDelivered,
  };
  EXPECT_EQ(callback.events, expected);

  ScarcityReadCallback detachingCallback;
  detachingCallback.socketToDetach = socket.get();
  socket->readSqe_->setReadCallback(&detachingCallback, false);
  socket->readSqe_->sendZeroCopyReadBuf(IOBuf::copyBuffer("c"), true);
  EXPECT_EQ(
      detachingCallback.events,
      std::vector{ScarcityReadCallback::Event::BuffersScarce});

  socket->readSqe_->setReadCallback(nullptr, false);
}

TEST_P(AsyncIoUringSocketTest, EoF) {
  MAYBE_SKIP();
  struct CB : AsyncReader::ReadCallback {
    void readDataAvailable(size_t) noexcept override {
      // will terminate...
      terminate_with<std::runtime_error>("unexpected data");
    }
    void readEOF() noexcept override {
      VLOG(1) << "CB Setting EOF";
      prom.setValue(makeExpected<AsyncSocketException>(Unit{}));
    }

    void readErr(const AsyncSocketException& ex) noexcept override {
      VLOG(1) << "CB Setting Err " << ex;
      prom.setValue(makeUnexpected(ex));
    }

    void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
      *bufReturn = &buff;
      *lenReturn = 1;
    }

    Promise<Expected<Unit, AsyncSocketException>> prom;
    char buff;
  };

  auto c = makeConnected(ConnectedOptions{}.withNoServerShouldRead());
  {
    CB cb_eof;
    c.server->setReadCB(&cb_eof);
    c.client->transport->closeNow();
    c.client.reset();
    EXPECT_TRUE(
        cb_eof.prom.getSemiFuture()
            .within(kTimeout)
            .via(base.get())
            .getVia(base.get())
            .hasValue());
    c.server->setReadCB(nullptr);
  }
  EXPECT_FALSE(c.server->good());
  {
    CB cb_invalid;
    c.server->setReadCB(&cb_invalid);
    auto ex =
        cb_invalid.prom.getSemiFuture()
            .within(kTimeout)
            .via(base.get())
            .getVia(base.get());
    ASSERT_TRUE(ex.hasError());
    auto er = ex.error();
    EXPECT_EQ(AsyncSocketException::NOT_OPEN, er.getType());
    c.server->setReadCB(nullptr);
  }
}

struct DetachCB : folly::AsyncDetachFdCallback {
  void fdDetached(
      NetworkSocket ns, std::unique_ptr<IOBuf> unread) noexcept override {
    promise.setValue(std::make_pair(ns, std::move(unread)));
  }

  void fdDetachFail(const AsyncSocketException& ex) noexcept override {
    promise.setException(ex);
  }

  folly::Promise<std::pair<NetworkSocket, std::unique_ptr<IOBuf>>> promise;
};

TEST_P(AsyncIoUringSocketTest, Detach) {
  MAYBE_SKIP();
  auto c = makeConnected();
  auto* was = c.server->getUnderlyingTransport<folly::AsyncIoUringSocket>();
  ASSERT_NE(was, nullptr);

  // write something
  c.client->transport->write(&nullWriteCallback, "hello", 5);
  // make sure it gets run
  backend->submitOutstanding();

  // sleep a bit to get the read all the way into the completion queue
  /* sleep override */ std::this_thread::sleep_for(
      std::chrono::milliseconds(5));

  // now detach before the write is completed
  DetachCB cb;
  was->asyncDetachFd(&cb);
  ASSERT_FALSE(cb.promise.isFulfilled()) << "must wait for read to finish";

  auto res =
      cb.promise.getSemiFuture()
          .within(kTimeout)
          .via(base.get())
          .getVia(base.get());
  EXPECT_GE(res.first.toFd(), 0);
  if (res.second) {
    // did not cancel in time
    EXPECT_EQ("hello", toString(res.second));
  } else {
    // did cancel in time
    char buff[128];
    memset(buff, 0, sizeof(buff));
    int ret;
    do {
      ret = fileops::read(res.first.toFd(), &buff, sizeof(buff));
    } while (ret == -1 && errno == EINTR);
    ASSERT_EQ(5, ret);
    EXPECT_EQ("hello", std::string(buff));
  }
}

TEST_P(AsyncIoUringSocketTest, DetachEventBase) {
  MAYBE_SKIP();
  auto c = makeConnected();

  // write something
  FutureWriteCallback fwc;
  auto* transport = c.client->transport.get();
  transport->write(&fwc, "hello", 5);

  // make sure it gets run
  backend->submitOutstanding();
  ASSERT_TRUE(transport->isDetachable());
  transport->detachEventBase();

  EventBase newBase{ioUringEbOptions()};
  transport->attachEventBase(&newBase);

  auto resFut = c.callback->waitFor(5).via(folly::getGlobalCPUExecutor().get());
  auto start = std::chrono::steady_clock::now();
  do {
    newBase.loopOnce(EVLOOP_NONBLOCK);
    base->loopOnce(EVLOOP_NONBLOCK);
    /* sleep override */ std::this_thread::sleep_for(
        std::chrono::milliseconds(20));
    auto res = resFut.poll();
    if (res) {
      EXPECT_EQ("hello", res->value());
      break;
    }
    if (std::chrono::steady_clock::now() > start + std::chrono::seconds(1)) {
      FAIL();
    }
  } while (true);

  // make sure write arrived, it should be on the new event base
  auto& [promise, future] = fwc.promiseContract;
  ASSERT_TRUE(std::move(future).via(&newBase).getVia(&newBase).hasValue());

  ASSERT_TRUE(transport->isDetachable());
  transport->detachEventBase();
  transport->attachEventBase(base.get());
}

TEST_P(AsyncIoUringSocketTest, DetachEventBaseClear) {
  MAYBE_SKIP();
  auto c = makeConnected();

  // write something
  c.client->transport->write(&nullWriteCallback, "hello", 5);

  backend->submitOutstanding();
  ASSERT_TRUE(c.client->transport->isDetachable());
  c.client->transport->detachEventBase();

  // now free things in the middle
  base->loopOnce(EVLOOP_NONBLOCK);
}

TEST_P(AsyncIoUringSocketTest, FastOpen) {
  MAYBE_SKIP();
  bool had_fastopen = false;
  bool can_fastopen = false;
  std::string fo;
  if (readFile("/proc/sys/net/ipv4/tcp_fastopen", fo)) {
    auto fast_open = folly::to<int>(fo);
    if (fast_open == 3) {
      can_fastopen = true;
    }
  }
  if (!can_fastopen) {
    LOG(INFO) << "/proc/sys/net/ipv4/tcp_fastopen must be 3 to do fastopen, "
                 "but we will test the code flow anyway";
  }
  // technically we could run ip tcp_metrics flush here, but messing with the
  // system in a test is awful
  folly::Subprocess subProc("/sbin/ip tcp_metrics show ::1"_shellify());
  int has_cookies_already = subProc.wait().exitStatus();
  if (has_cookies_already == 0) {
    LOG(INFO)
        << "already had cookies, so cannot do fastopen test, but will test code flow anyway. "
        << " you could do a `/sbin/ip tcp_metrics flush` to test this";
    had_fastopen = true;
  }
  auto opts = ConnectedOptions{}.withFastOpen("hello");
  {
    auto conn = makeConnected(opts);
    EXPECT_EQ(
        "hello", conn.callback->waitFor(5).via(base.get()).getVia(base.get()));
    if (!had_fastopen) {
      EXPECT_FALSE(conn.client->transport->getTFOSucceeded());
    }
  }
  {
    auto conn = makeConnected(opts);
    EXPECT_EQ(
        "hello", conn.callback->waitFor(5).via(base.get()).getVia(base.get()));
    if (can_fastopen) {
      EXPECT_TRUE(conn.client->transport->getTFOSucceeded());
    }
  }
}

// Regression test for use-after-free in ~AsyncIoUringSocket() when a
// FastOpenSqe is in flight. The destructor must cancel fastOpenSqe_ so a
// late TFO CQE does not invoke FastOpenSqe::callback against the freed
// parent (which then drops into WriteSqe::callback via
// processFastOpenResult and dereferences the dangling parent_).
//
// Without the fix this triggers a heap-use-after-free under ASAN whose
// allocation site is FastOpenSqe construction in writeChain() and whose
// use site is FastOpenSqe::callback / processFastOpenResult /
// WriteSqe::callback.
TEST_P(AsyncIoUringSocketTest, DestroyDuringFastOpen) {
  MAYBE_SKIP();
  if (!GetParam().ioUringClient) {
    return;
  }

  AsyncIoUringSocket::UniquePtr client(
      new AsyncIoUringSocket(base.get(), ioUringSocketOptions()));
  client->enableTFO();
  // The fixture itself implements ConnectCallback with no-op success and a
  // FATAL on error; that is fine because FastOpen places the socket into
  // State::FastOpen and fires connectSuccess() synchronously without
  // submitting a connect SQE.
  client->connect(this, serverAddress);

  // Pushes the WriteSqe through to construct and submitSoon() a
  // FastOpenSqe (state == State::FastOpen path in
  // AsyncIoUringSocket::writeChain).
  client->writeChain(
      &nullWriteCallback, IOBuf::copyBuffer("hello"), WriteFlags::NONE);

  // Get the SQE into the kernel without draining its CQE.
  backend->submitOutstanding();

  // Give the kernel a moment to start the TFO operation while
  // fastOpenSqe_->inFlight() is still true.
  /* sleep override */ std::this_thread::sleep_for(
      std::chrono::milliseconds(5));

  // Destroy the socket while the FastOpenSqe may still be in flight. The
  // unique_ptr's Destructor invokes ~AsyncIoUringSocket() via
  // DelayedDestruction.
  client.reset();

  // Drain any late CQEs so a (now-freed) FastOpenSqe's callback would
  // fire and trip the use-after-free under ASAN. This loop does not
  // assert: ASAN failing here is the only thing we care about.
  auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(1)) {
    base->loopOnce(EVLOOP_NONBLOCK);
    /* sleep override */ std::this_thread::sleep_for(
        std::chrono::milliseconds(10));
  }
}

TEST_P(AsyncIoUringSocketTest, BindAddressNoPort) {
  EventBase eventBase;
  test::TestServer server(true);

  // When setBindAddressNoPort is disabled, verifies that a port is assigned
  // before the connect call
  AsyncIoUringSocket::UniquePtr socket(new AsyncIoUringSocket(base.get()));
  socket->setBindAddressNoPort(false);
  SocketAddress bindAddr("127.0.0.1", 0);
  test::TestPortAssignmentCallback callback;
  socket->connect(
      &callback,
      server.getAddress(),
      std::chrono::milliseconds(30),
      emptySocketOptionMap,
      bindAddr);
  eventBase.loop();
  EXPECT_NE(callback.assignedPort, 0);
  socket->close();
}

TEST_P(AsyncIoUringSocketTest, ConnectSuccessStateEstablished) {
  MAYBE_SKIP();

  struct ConnectCB : AsyncSocket::ConnectCallback {
    explicit ConnectCB(AsyncIoUringSocket* s) : socket(s) {}
    void connectSuccess() noexcept override { prom.setValue(socket->good()); }
    void connectErr(const AsyncSocketException& ex) noexcept override {
      prom.setException(ex);
    }

    Promise<bool> prom;
    AsyncIoUringSocket* socket;
  };

  AsyncIoUringSocket::UniquePtr socket(
      new AsyncIoUringSocket(base.get(), ioUringSocketOptions()));

  ConnectCB cb(socket.get());
  socket->connect(&cb, serverAddress);
  auto fd =
      fdPromise.getFuture().within(kTimeout).via(base.get()).getVia(base.get());
  fdPromise = {};
  auto server = AsyncSocket::newSocket(base.get(), fd);

  auto isGood =
      cb.prom.getSemiFuture()
          .within(kTimeout)
          .via(base.get())
          .getVia(base.get());
  EXPECT_TRUE(isGood);
}

TEST_P(AsyncIoUringSocketTest, ConnectWithBoundFd) {
  MAYBE_SKIP();

  struct ConnectCB : AsyncSocket::ConnectCallback {
    explicit ConnectCB(AsyncIoUringSocket* s) : socket(s) {}
    void connectSuccess() noexcept override { prom.setValue(socket->good()); }
    void connectErr(const AsyncSocketException& ex) noexcept override {
      prom.setException(ex);
    }

    Promise<bool> prom;
    AsyncIoUringSocket* socket;
  };

  int sockfd = ::socket(serverAddress.getFamily(), SOCK_STREAM, 0);

  uint16_t boundPort = 0;
  for (uint16_t port = 1024; port <= 65535; port++) {
    struct sockaddr_storage bindStorage = {};
    socklen_t bindLen;
    if (serverAddress.getFamily() == AF_INET6) {
      auto* sa = reinterpret_cast<struct sockaddr_in6*>(&bindStorage);
      sa->sin6_family = AF_INET6;
      sa->sin6_port = htons(port);
      sa->sin6_addr = in6addr_any;
      bindLen = sizeof(struct sockaddr_in6);
    } else {
      auto* sa = reinterpret_cast<struct sockaddr_in*>(&bindStorage);
      sa->sin_family = AF_INET;
      sa->sin_port = htons(port);
      sa->sin_addr.s_addr = INADDR_ANY;
      bindLen = sizeof(struct sockaddr_in);
    }
    if (::bind(
            sockfd,
            reinterpret_cast<struct sockaddr*>(&bindStorage),
            bindLen) == 0) {
      boundPort = port;
      break;
    }
  }
  if (boundPort == 0) {
    GTEST_SKIP() << "Unable to find a free port to bind() to";
  }

  AsyncIoUringSocket::UniquePtr socket(
      new AsyncIoUringSocket(base.get(), ioUringSocketOptions()));
  ConnectCB cb(socket.get());
  socket->connect(
      &cb,
      serverAddress,
      std::chrono::milliseconds(0),
      folly::emptySocketOptionMap,
      folly::AsyncSocketTransport::BindOptions{NetworkSocket(sockfd)});

  auto fd =
      fdPromise.getFuture().within(kTimeout).via(base.get()).getVia(base.get());
  fdPromise = {};
  auto server = AsyncSocket::newSocket(base.get(), fd);

  auto isGood =
      cb.prom.getSemiFuture()
          .within(kTimeout)
          .via(base.get())
          .getVia(base.get());
  EXPECT_TRUE(isGood);

  SocketAddress localAddr;
  socket->getLocalAddress(&localAddr);
  EXPECT_EQ(localAddr.getPort(), boundPort);
}

TEST_P(AsyncIoUringSocketTest, ConnectWithUnboundFd) {
  MAYBE_SKIP();

  struct ConnectCB : AsyncSocket::ConnectCallback {
    explicit ConnectCB(AsyncIoUringSocket* s) : socket(s) {}
    void connectSuccess() noexcept override { prom.setValue(socket->good()); }
    void connectErr(const AsyncSocketException& ex) noexcept override {
      prom.setException(ex);
    }

    Promise<bool> prom;
    AsyncIoUringSocket* socket;
  };

  int sockfd = ::socket(serverAddress.getFamily(), SOCK_STREAM, 0);
  ASSERT_GE(sockfd, 0);

  AsyncIoUringSocket::UniquePtr socket(
      new AsyncIoUringSocket(base.get(), ioUringSocketOptions()));
  ConnectCB cb(socket.get());
  socket->connect(
      &cb,
      serverAddress,
      std::chrono::milliseconds(0),
      folly::emptySocketOptionMap,
      folly::AsyncSocketTransport::BindOptions{NetworkSocket(sockfd)});

  auto fd =
      fdPromise.getFuture().within(kTimeout).via(base.get()).getVia(base.get());
  fdPromise = {};
  auto server = AsyncSocket::newSocket(base.get(), fd);

  auto isGood =
      cb.prom.getSemiFuture()
          .within(kTimeout)
          .via(base.get())
          .getVia(base.get());
  EXPECT_TRUE(isGood);

  SocketAddress localAddr;
  socket->getLocalAddress(&localAddr);
  EXPECT_GT(localAddr.getPort(), 0);
}

TEST_P(AsyncIoUringSocketTest, ConnectWithAlreadyConnectedFd) {
  MAYBE_SKIP();

  struct ConnectCB : AsyncSocket::ConnectCallback {
    void connectSuccess() noexcept override {
      prom.setValue(makeExpected<AsyncSocketException>(Unit{}));
    }
    void connectErr(const AsyncSocketException& ex) noexcept override {
      prom.setValue(makeUnexpected(ex));
    }

    Promise<Expected<Unit, AsyncSocketException>> prom;
  };

  int connectedFd = ::socket(serverAddress.getFamily(), SOCK_STREAM, 0);
  struct sockaddr_storage ss{};
  serverAddress.getAddress(&ss);
  ASSERT_EQ(
      ::connect(
          connectedFd, (struct sockaddr*)&ss, serverAddress.getActualSize()),
      0);

  auto acceptedFd =
      fdPromise.getFuture().within(kTimeout).via(base.get()).getVia(base.get());
  fdPromise = {};
  ::close(acceptedFd.toFd());

  AsyncIoUringSocket::UniquePtr socket(
      new AsyncIoUringSocket(base.get(), ioUringSocketOptions()));
  ConnectCB cb;
  socket->connect(
      &cb,
      serverAddress,
      std::chrono::milliseconds(0),
      folly::emptySocketOptionMap,
      folly::AsyncSocketTransport::BindOptions{NetworkSocket(connectedFd)});

  auto res =
      cb.prom.getSemiFuture()
          .within(kTimeout)
          .via(base.get())
          .getVia(base.get());
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error().getType(), AsyncSocketException::INVALID_STATE);
  EXPECT_EQ(fcntl(connectedFd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
}

TEST_P(AsyncIoUringSocketTest, ReadCallbackSetDuringConnect) {
  MAYBE_SKIP();

  struct ReadCB : public AsyncReader::ReadCallback {
    void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
      *bufReturn = buff;
      *lenReturn = sizeof(buff);
    }

    void readDataAvailable(size_t len) noexcept override {
      prom.setValue(std::string{buff, len});
    }

    void readEOF() noexcept override {}
    void readErr(const AsyncSocketException& ex) noexcept override {
      prom.setException(ex);
    }

    bool isBufferMovable() noexcept override { return false; }

    Promise<std::string> prom;
    char buff[1024];
  };

  ReadCB readCB;
  AsyncIoUringSocket::UniquePtr socket(
      new AsyncIoUringSocket(base.get(), ioUringSocketOptions()));

  socket->connect(this, serverAddress);
  socket->setReadCB(&readCB);
  auto fd =
      fdPromise.getFuture().within(kTimeout).via(base.get()).getVia(base.get());
  fdPromise = {};
  auto server = AsyncSocket::newSocket(base.get(), fd);
  server->write(&nullWriteCallback, "hello", 5);
  auto result =
      readCB.prom.getSemiFuture()
          .within(kTimeout)
          .via(base.get())
          .getVia(base.get());
  EXPECT_EQ("hello", result);
}

TEST_P(AsyncIoUringSocketTestAll, WriteChain2) {
  MAYBE_SKIP();
  auto [e, s, cb] = makeConnected();
  s->writeChain(&nullWriteCallback, IOBuf::copyBuffer("hello"));
  EXPECT_EQ("hello", cb->waitFor(5).via(base.get()).getVia(base.get()));
  s->writeChain(&nullWriteCallback, IOBuf::copyBuffer("there"));
  EXPECT_EQ("there", cb->waitFor(5).via(base.get()).getVia(base.get()));
}

TEST_P(AsyncIoUringSocketTestAll, WriteChainOrder) {
  MAYBE_SKIP();
  auto [e, s, cb] = makeConnected();
  auto chain = IOBuf::copyBuffer("h");
  chain->appendToChain(IOBuf::copyBuffer("e"));
  chain->appendToChain(IOBuf::copyBuffer("ll"));
  chain->appendToChain(IOBuf::copyBuffer("o"));
  s->writeChain(&nullWriteCallback, std::move(chain));
  EXPECT_EQ("hello", cb->waitFor(5).via(base.get()).getVia(base.get()));
}

TEST_P(AsyncIoUringSocketTestAll, WriteChainLong) {
  MAYBE_SKIP();
  auto [e, s, cb] = makeConnected();
  auto chain = IOBuf::copyBuffer("?");
  std::string res = "?";
  for (int i = 0; i < 4096; i++) {
    std::string x(1, 'a' + i % 26);
    chain->appendToChain(IOBuf::copyBuffer(x));
    res += x;
  }
  s->writeChain(&nullWriteCallback, std::move(chain));
  EXPECT_EQ(res, cb->waitFor(res.size()).via(base.get()).getVia(base.get()));
}

TEST_P(AsyncIoUringSocketTestAll, Write) {
  MAYBE_SKIP();
  auto [e, s, cb] = makeConnected();
  s->write(&nullWriteCallback, "hello", 5);
  EXPECT_EQ("hello", cb->waitFor(5).via(base.get()).getVia(base.get()));
}

TEST_P(AsyncIoUringSocketTestAll, WriteAfterWait) {
  MAYBE_SKIP();
  auto conn = makeConnected();
  auto& s = conn.server;
  auto& cb = conn.callback;

  EXPECT_EQ(
      "hello",
      folly::futures::sleep(std::chrono::milliseconds(500))
          .via(base.get())
          .thenValue([&](auto&&) { s->write(&nullWriteCallback, "hello", 5); })
          .thenValue([&](auto&&) { return cb->waitFor(5); })
          .getVia(base.get()));
}

TEST_P(AsyncIoUringSocketTestAll, WriteBigChunked) {
  MAYBE_SKIP();
  auto [e, s, cb] = makeConnected();
  cb->setHoldData(true);
  std::string big = randomString(4000000);
  size_t at = 0;
  int const kChunkSize = 256;
  while (at < big.size()) {
    auto len = std::min<size_t>(big.size() - at, kChunkSize);
    s->write(&nullWriteCallback, big.c_str() + at, len);
    at += len;
  }
  auto res = cb->waitFor(big.size()).via(base.get()).getVia(base.get());
  EXPECT_TRUE(big == res) << big.size() << " vs " << res.size();
}

TEST_P(AsyncIoUringSocketTestAll, Writev) {
  MAYBE_SKIP();
  auto [e, s, cb] = makeConnected();
  std::array<iovec, 2> iov = {{{(void*)"hel", 3}, {(void*)"lo", 2}}};
  s->writev(&nullWriteCallback, iov.data(), iov.size());
  EXPECT_EQ("hello", cb->waitFor(5).via(base.get()).getVia(base.get()));
}

TEST_P(AsyncIoUringSocketTestAll, SendTimeout) {
  MAYBE_SKIP();
  if (!GetParam().ioUringServer) {
    // folly::AsyncSocket is not totally reliable with timeouts
    return;
  }
  if (GetParam().sendzc) {
    // sendmsg_zc pins user pages and returns success (CQE res > 0) before TCP
    // transmits the data. AsyncIoUringSocket's send timeout only fires while a
    // write SQE is in-flight — once the CQE returns success, the timeout is
    // cleared. To support ZC send timeouts, the timeout would need to track
    // NOTIF CQE lifetime, not just write CQE completion.
    // TODO(T272254448): remove AsyncIoUringSocket & associated tests
    return;
  }
  auto conn = makeConnected(ConnectedOptions{}.withNoServerShouldRead());
  FutureWriteCallback ecb;
  std::string big(40000000, 'X');
  std::vector<iovec> iov;
  iov.resize(100);
  for (size_t i = 0; i < iov.size(); i++) {
    iov[i].iov_base = big.data();
    iov[i].iov_len = big.size();
  }
  base->runInEventBaseThread([&]() {
    conn.server->setSendTimeout(1);
    conn.server->writev(&ecb, iov.data(), iov.size());
  });
  auto& [promise, future] = ecb.promiseContract;
  auto ex = std::move(future).via(base.get()).getVia(base.get());
  ASSERT_TRUE(ex.hasError());
  EXPECT_EQ(AsyncSocketException::TIMED_OUT, ex.error().second.getType());
}

INSTANTIATE_TEST_SUITE_P(
    AsyncIoUringSocketTest,
    AsyncIoUringSocketTestAll,
    ::testing::ValuesIn(mkAllTestParams()),
    [](const ::testing::TestParamInfo<TestParams>& info) {
      return info.param.testName();
    });

TestParams mkBasicTestParams() {
  TestParams t;
  t.ioUringClient = t.ioUringServer = true;
  return t;
}

INSTANTIATE_TEST_SUITE_P(
    AsyncIoUringSocketTest,
    AsyncIoUringSocketTest,
    ::testing::Values(mkBasicTestParams()),
    [](const ::testing::TestParamInfo<TestParams>& info) {
      return info.param.testName();
    });

class AsyncIoUringSocketTakeoverTest : public AsyncIoUringSocketTest {};

class AsyncSocketWithPreRead : public AsyncSocket {
 public:
  AsyncSocketWithPreRead(AsyncSocket::UniquePtr a, std::string const& pre_read)
      : AsyncSocket(std::move(a)) {
    preReceivedData_ = IOBuf::copyBuffer(pre_read);
  }
};

TEST_P(AsyncIoUringSocketTakeoverTest, PreRead) {
  MAYBE_SKIP();
  auto conn = makeConnected(ConnectedOptions{}.withNoServerShouldRead());
  AsyncSocket::UniquePtr sock(
      dynamic_cast<AsyncSocket*>(conn.server.release()));
  ASSERT_NE(sock, nullptr);
  AsyncIoUringSocket::UniquePtr io_uring(new AsyncIoUringSocket(
      AsyncSocket::UniquePtr(
          new AsyncSocketWithPreRead(std::move(sock), "hello"))));
  io_uring->setReadCB(conn.callback.get());
  io_uring->write(&nullWriteCallback, "there", 5);
  EXPECT_EQ(
      "hellothere",
      conn.callback->waitFor(10).via(base.get()).getVia(base.get()));
}

TestParams mkTakeoverParams() {
  TestParams t;
  t.ioUringClient = t.ioUringServer = false;
  return t;
}

INSTANTIATE_TEST_SUITE_P(
    AsyncIoUringSocketTakeoverTest,
    AsyncIoUringSocketTakeoverTest,
    ::testing::Values(mkTakeoverParams()),
    [](const ::testing::TestParamInfo<TestParams>& info) {
      return info.param.testName();
    });

TEST(AsyncIoUringSocketTest, RemoveNonBlockFlag) {
  Promise<NetworkSocket> fdPromise;
  test::TestAcceptCallback acceptCallback;
  acceptCallback.setConnectionAcceptedFn(
      [&fdPromise](NetworkSocket fd, const folly::SocketAddress& /*addr*/) {
        fdPromise.setValue(fd);
      });

  auto options = IoUringBackend::Options{};
  options.setUseRegisteredFds(64)
      .setInitialProvidedBuffers(64, 8)
      .setDeferTaskRun(true);
  EventBase evb{EventBase::Options{}.setBackendFactory(
      [&options]() -> std::unique_ptr<EventBaseBackendBase> {
        return std::make_unique<IoUringBackend>(std::move(options));
      })};

  std::shared_ptr<AsyncServerSocket> serverSocket(
      AsyncServerSocket::newSocket(&evb));
  serverSocket->bind(0);
  serverSocket->listen(1024);
  folly::SocketAddress serverAddress;
  serverSocket->getAddress(&serverAddress);
  serverSocket->addAcceptCallback(&acceptCallback, &evb);
  serverSocket->startAccepting();

  AsyncSocket::UniquePtr clientSocket(AsyncSocket::newSocket(&evb));
  clientSocket->connect(nullptr, serverAddress);

  auto fd = fdPromise.getFuture().within(kTimeout).via(&evb).getVia(&evb);
  AsyncSocket::UniquePtr acceptedSocket(AsyncSocket::newSocket(&evb, fd));

  int flags = fcntl(fd.toFd(), F_GETFL, 0);
  ASSERT_GE(flags, 0);
  EXPECT_EQ(flags & O_NONBLOCK, O_NONBLOCK);

  AsyncIoUringSocket::UniquePtr ioUringSocket(
      new AsyncIoUringSocket(std::move(acceptedSocket)));

  int newFlags = fcntl(fd.toFd(), F_GETFL, 0);
  ASSERT_GE(newFlags, 0);
  EXPECT_EQ(newFlags & O_NONBLOCK, 0);
}

namespace {

std::unique_ptr<EventBase> makeIoUringEventBase() {
  return std::make_unique<EventBase>(EventBase::Options{}.setBackendFactory(
      []() -> std::unique_ptr<EventBaseBackendBase> {
        IoUringBackend::Options options;
        options.setInitialProvidedBuffers(64, 8);
        return std::make_unique<IoUringBackend>(std::move(options));
      }));
}

// cb must outlive socket: on timeout the write is still in flight and the
// socket calls back into cb when it is destroyed.
FutureWriteCallback::TResult writeAndWait(
    EventBase& evb,
    AsyncIoUringSocket& socket,
    FutureWriteCallback& cb,
    std::string_view data) {
  socket.write(&cb, data.data(), data.size());
  auto& [promise, future] = cb.promiseContract;
  return std::move(future).within(kTimeout).via(&evb).getVia(&evb);
}

} // namespace

TEST(AsyncIoUringSocketTest, ZeroCopyWriteErrorKeepsErrno) {
  if (!IoUringBackend::isAvailable()) {
    GTEST_SKIP() << "io_uring not available";
  }
  auto evb = makeIoUringEventBase();
  // AF_UNIX sockets don't support zc, returning EOPNOTSUPP or EINVAL
  std::array<int, 2> fds{};
  ASSERT_EQ(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()));
  AsyncIoUringSocket::Options options;
  options.zeroCopyEnable = [](auto&&) { return true; };
  FutureWriteCallback cb;
  AsyncIoUringSocket::UniquePtr socket(new AsyncIoUringSocket(
      evb.get(), NetworkSocket::fromFd(fds[0]), std::move(options)));

  auto res = writeAndWait(*evb, *socket, cb, "hello");

  ASSERT_TRUE(res.hasError());
  EXPECT_NE(0, res.error().second.getErrno()) << res.error().second.what();
  ::close(fds[1]);
}

TEST(AsyncIoUringSocketTest, WriteErrorKeepsErrno) {
  if (!IoUringBackend::isAvailable()) {
    GTEST_SKIP() << "io_uring not available";
  }
  auto evb = makeIoUringEventBase();
  std::array<int, 2> fds{};
  ASSERT_EQ(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()));
  ::close(fds[1]);
  FutureWriteCallback cb;
  AsyncIoUringSocket::UniquePtr socket(
      new AsyncIoUringSocket(evb.get(), NetworkSocket::fromFd(fds[0])));

  auto res = writeAndWait(*evb, *socket, cb, "hello");

  ASSERT_TRUE(res.hasError());
  EXPECT_EQ(EPIPE, res.error().second.getErrno()) << res.error().second.what();
}

} // namespace folly
