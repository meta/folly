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

#include <folly/io/async/test/AsyncSocketTest2.h>

#include <fcntl.h>
#include <sys/types.h>

#include <time.h>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include <folly/ExceptionWrapper.h>
#include <folly/Random.h>
#include <folly/SocketAddress.h>
#include <folly/io/IOBuf.h>
#include <folly/io/SocketOptionMap.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/IoUringBackend.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/io/async/test/AsyncSocketTest.h>
#include <folly/io/async/test/IoUringTestUtil.h>
#include <folly/io/async/test/MockAsyncSocketLegacyObserver.h>
#include <folly/io/async/test/MockAsyncSocketObserver.h>
#include <folly/io/async/test/TFOUtil.h>
#include <folly/io/async/test/Util.h>
#include <folly/net/test/MockNetOpsDispatcher.h>
#include <folly/net/test/MockTcpInfoDispatcher.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <folly/portability/Sockets.h>
#include <folly/portability/Unistd.h>
#include <folly/synchronization/Baton.h>
#include <folly/test/SocketAddressTestHelper.h>
#include <folly/testing/TestUtil.h>

using std::min;
using std::string;
using std::unique_ptr;
using std::vector;
using std::chrono::milliseconds;
using testing::MatchesRegex;

using namespace folly;
using namespace folly::test;
using namespace testing;

static constexpr auto kMaxAttemptsEnableByteEvents =
    folly::AsyncSocket::kMaxAttemptsEnableByteEvents;

namespace {
// string and corresponding vector with 100 characters
const std::string kOneHundredCharacterString(
    "ThisIsAVeryLongStringThatHas100Characters"
    "AndIsUniqueEnoughToBeInterestingForTestUsageNowEndOfMessage");
const std::vector<uint8_t> kOneHundredCharacterVec(
    kOneHundredCharacterString.begin(), kOneHundredCharacterString.end());

WriteFlags msgFlagsToWriteFlags(const int msg_flags) {
  WriteFlags flags = WriteFlags::NONE;
#ifdef MSG_MORE
  if (msg_flags & MSG_MORE) {
    flags = flags | WriteFlags::CORK;
  }
#endif // MSG_MORE

#ifdef MSG_EOR
  if (msg_flags & MSG_EOR) {
    flags = flags | WriteFlags::EOR;
  }
#endif

#ifdef MSG_ZEROCOPY
  if (msg_flags & MSG_ZEROCOPY) {
    flags = flags | WriteFlags::WRITE_MSG_ZEROCOPY;
  }
#endif
  return flags;
}

WriteFlags getMsgAncillaryTsFlags(const struct msghdr& msg) {
  const struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
      cmsg->cmsg_type != SO_TIMESTAMPING ||
      cmsg->cmsg_len != CMSG_LEN(sizeof(uint32_t))) {
    return WriteFlags::NONE;
  }

  const uint32_t* sofFlags =
      (reinterpret_cast<const uint32_t*>(CMSG_DATA(cmsg)));
  WriteFlags flags = WriteFlags::NONE;
  if (*sofFlags & folly::netops::SOF_TIMESTAMPING_TX_SCHED) {
    flags = flags | WriteFlags::TIMESTAMP_SCHED;
  }
  if (*sofFlags & folly::netops::SOF_TIMESTAMPING_TX_SOFTWARE) {
    flags = flags | WriteFlags::TIMESTAMP_TX;
  }
  if (*sofFlags & folly::netops::SOF_TIMESTAMPING_TX_ACK) {
    flags = flags | WriteFlags::TIMESTAMP_ACK;
  }

  return flags;
}

WriteFlags getMsgAncillaryTsFlags(const struct msghdr* msg) {
  return getMsgAncillaryTsFlags(*msg);
}

MATCHER_P(SendmsgMsghdrHasTotalIovLen, len, "") {
  size_t iovLen = 0;
  for (size_t i = 0; i < arg.msg_iovlen; i++) {
    iovLen += arg.msg_iov[i].iov_len;
  }
  return len == iovLen;
}

MATCHER_P(SendmsgInvocHasTotalIovLen, len, "") {
  size_t iovLen = 0;
  for (const auto& iov : arg.iovs) {
    iovLen += iov.iov_len;
  }
  return len == iovLen;
}

MATCHER_P(SendmsgInvocHasIovFirstByte, firstBytePtr, "") {
  if (arg.iovs.empty()) {
    return false;
  }

  const auto& firstIov = arg.iovs.front();
  auto iovFirstBytePtr = const_cast<void*>(
      static_cast<const void*>(reinterpret_cast<uint8_t*>(firstIov.iov_base)));
  return firstBytePtr == iovFirstBytePtr;
}

MATCHER_P(SendmsgInvocHasIovLastByte, lastBytePtr, "") {
  if (arg.iovs.empty()) {
    return false;
  }

  const auto& lastIov = arg.iovs.back();
  auto iovLastBytePtr = const_cast<void*>(static_cast<const void*>(
      reinterpret_cast<uint8_t*>(lastIov.iov_base) + lastIov.iov_len - 1));
  return lastBytePtr == iovLastBytePtr;
}

MATCHER_P(SendmsgInvocMsgFlagsEq, writeFlags, "") {
  return writeFlags == arg.writeFlagsInMsgFlags;
}

MATCHER_P(SendmsgInvocAncillaryFlagsEq, writeFlags, "") {
  return writeFlags == arg.writeFlagsInAncillary;
}

MATCHER_P2(ByteEventMatching, type, offset, "") {
  if (type != arg.type || (size_t)offset != arg.offset) {
    return false;
  }
  return true;
}
} // namespace

class DelayedWrite : public AsyncTimeout {
 public:
  DelayedWrite(
      const std::shared_ptr<AsyncSocket>& socket,
      unique_ptr<IOBuf>&& bufs,
      AsyncTransportWrapper::WriteCallback* wcb,
      bool cork,
      bool lastWrite = false)
      : AsyncTimeout(socket->getEventBase()),
        socket_(socket),
        bufs_(std::move(bufs)),
        wcb_(wcb),
        cork_(cork),
        lastWrite_(lastWrite) {}

 private:
  void timeoutExpired() noexcept override {
    WriteFlags flags = cork_ ? WriteFlags::CORK : WriteFlags::NONE;
    socket_->writeChain(wcb_, std::move(bufs_), flags);
    if (lastWrite_) {
      socket_->shutdownWrite();
    }
  }

  std::shared_ptr<AsyncSocket> socket_;
  unique_ptr<IOBuf> bufs_;
  AsyncTransportWrapper::WriteCallback* wcb_;
  bool cork_;
  bool lastWrite_;
};

enum class BackendType {
  DEFAULT,
  IO_URING,
};

enum class TFOState {
  DISABLED,
  ENABLED,
};

std::vector<BackendType> getBackendTestingValues() {
  std::vector<BackendType> vals;
  vals.emplace_back(BackendType::DEFAULT);
  vals.emplace_back(BackendType::IO_URING);
  return vals;
}

using ConnectTestParam = std::tuple<BackendType, TFOState>;
std::vector<ConnectTestParam> getBackendTFOTestingValues() {
  std::vector<ConnectTestParam> vals;
  vals.emplace_back(BackendType::DEFAULT, TFOState::DISABLED);
  vals.emplace_back(BackendType::IO_URING, TFOState::DISABLED);

#if FOLLY_ALLOW_TFO
  vals.emplace_back(BackendType::DEFAULT, TFOState::ENABLED);
  vals.emplace_back(BackendType::IO_URING, TFOState::ENABLED);
#endif
  return vals;
}

///////////////////////////////////////////////////////////////////////////
// constructor related tests
///////////////////////////////////////////////////////////////////////////

class AsyncSocketTest : public ::testing::TestWithParam<BackendType> {
 protected:
  void SetUp() override {
    if (GetParam() == BackendType::IO_URING) {
      try {
        evb_ = makeIoUringEventBase(ioUringOptionsWithProvidedBuffers);
      } catch (IoUringBackend::NotAvailable const&) {
        GTEST_SKIP() << "IoUringBackend not available";
      }
    } else {
      evb_ = std::make_unique<EventBase>();
    }
  }

  EventBase& getEventBase() { return *evb_; }

  std::unique_ptr<EventBase> makeEventBase() {
    if (GetParam() == BackendType::IO_URING) {
      return makeIoUringEventBase(ioUringOptionsWithProvidedBuffers);
    } else {
      return std::make_unique<EventBase>();
    }
  }

 private:
  std::unique_ptr<EventBase> evb_;
};

INSTANTIATE_TEST_SUITE_P(
    AsyncSocketTests,
    AsyncSocketTest,
    ::testing::ValuesIn(getBackendTestingValues()),
    [](const ::testing::TestParamInfo<BackendType>& info) {
      return info.param == BackendType::IO_URING
          ? "IoUringBackend"
          : "DefaultBackend";
    });

class AsyncSocketIoUringTest : public AsyncSocketTest {};

INSTANTIATE_TEST_SUITE_P(
    AsyncSocketIoUringTests,
    AsyncSocketIoUringTest,
    ::testing::Values(BackendType::IO_URING),
    [](const ::testing::TestParamInfo<BackendType>&) {
      return "IoUringBackend";
    });

class AsyncSocketDefaultBackendTest : public AsyncSocketTest {};

INSTANTIATE_TEST_SUITE_P(
    AsyncSocketDefaultBackendTests,
    AsyncSocketDefaultBackendTest,
    ::testing::Values(BackendType::DEFAULT),
    [](const ::testing::TestParamInfo<BackendType>&) {
      return "DefaultBackend";
    });

/**
 * Test constructing with an existing fd.
 */
TEST_P(AsyncSocketTest, ConstructWithFd) {
  // construct a pair of unix sockets
  NetworkSocket fds[2];
  {
    auto ret = netops::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    EXPECT_EQ(0, ret);
  }

  // "client" socket
  auto cfd = fds[0];
  ASSERT_NE(cfd, NetworkSocket());

  // instantiate AsyncSocket w/o any connectionEstablishTimestamp
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::UniquePtr(new AsyncSocket(&evb, cfd));

  // should be no connect timestamps
  EXPECT_EQ(
      std::chrono::steady_clock::time_point(), socket->getConnectStartTime());
  EXPECT_EQ(
      std::chrono::steady_clock::time_point(), socket->getConnectEndTime());

  // should be no establish time, since not passed on construction
  EXPECT_FALSE(socket->getConnectionEstablishTime().has_value());
}

/**
 * Test constructing with an existing fd, passing a connection establish ts.
 */
TEST_P(AsyncSocketTest, ConstructWithFdAndTimestamp) {
  // construct a pair of unix sockets
  NetworkSocket fds[2];
  {
    auto ret = netops::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    EXPECT_EQ(0, ret);
  }

  // "client" socket
  auto cfd = fds[0];
  ASSERT_NE(cfd, NetworkSocket());

  // instantiate AsyncSocket w/ a connectionEstablishTimestamp
  const auto connectionEstablishTime = std::chrono::steady_clock::now();
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::UniquePtr(
      new AsyncSocket(&evb, cfd, 0, nullptr, connectionEstablishTime));

  // should be no connect timestamps
  EXPECT_EQ(
      std::chrono::steady_clock::time_point(), socket->getConnectStartTime());
  EXPECT_EQ(
      std::chrono::steady_clock::time_point(), socket->getConnectEndTime());

  // should have connection establish time, as passed on construction
  ASSERT_TRUE(socket->getConnectionEstablishTime().has_value());
  EXPECT_EQ(
      connectionEstablishTime, socket->getConnectionEstablishTime().value());
}

/**
 * Test constructing with an existing fd, then moving.
 */
TEST_P(AsyncSocketTest, ConstructWithFdThenMove) {
  // construct a pair of unix sockets
  NetworkSocket fds[2];
  {
    auto ret = netops::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    EXPECT_EQ(0, ret);
  }

  // "client" socket
  auto cfd = fds[0];
  ASSERT_NE(cfd, NetworkSocket());

  // instantiate AsyncSocket
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::UniquePtr(new AsyncSocket(&evb, cfd));

  // should be no connect timestamps
  EXPECT_EQ(
      std::chrono::steady_clock::time_point(), socket->getConnectStartTime());
  EXPECT_EQ(
      std::chrono::steady_clock::time_point(), socket->getConnectEndTime());

  // should be no establish time, since not passed on construction
  EXPECT_FALSE(socket->getConnectionEstablishTime().has_value());

  // move the socket
  auto socket2 = AsyncSocket::UniquePtr(new AsyncSocket(std::move(socket)));

  // should still be no connect timestamps
  EXPECT_EQ(
      std::chrono::steady_clock::time_point(), socket2->getConnectStartTime());
  EXPECT_EQ(
      std::chrono::steady_clock::time_point(), socket2->getConnectEndTime());

  // should still be no establish time, since not passed on orig construction
  EXPECT_FALSE(socket2->getConnectionEstablishTime().has_value());
}

/**
 * Test constructing with an existing fd, then moving.
 */
TEST_P(AsyncSocketTest, ConstructWithFdAndTimestampThenMove) {
  // construct a pair of unix sockets
  NetworkSocket fds[2];
  {
    auto ret = netops::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    EXPECT_EQ(0, ret);
  }

  // "client" socket
  auto cfd = fds[0];
  ASSERT_NE(cfd, NetworkSocket());

  // instantiate AsyncSocket w/ a connectionEstablishTimestamp
  const auto connectionEstablishTime = std::chrono::steady_clock::now();
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::UniquePtr(
      new AsyncSocket(&evb, cfd, 0, nullptr, connectionEstablishTime));

  // should be no connect timestamps
  EXPECT_EQ(
      std::chrono::steady_clock::time_point(), socket->getConnectStartTime());
  EXPECT_EQ(
      std::chrono::steady_clock::time_point(), socket->getConnectEndTime());

  // should have connection establish time, as passed on construction
  ASSERT_TRUE(socket->getConnectionEstablishTime().has_value());
  EXPECT_EQ(
      connectionEstablishTime, socket->getConnectionEstablishTime().value());

  // move the socket
  auto socket2 = AsyncSocket::UniquePtr(new AsyncSocket(std::move(socket)));

  // should still be no connect timestamps
  EXPECT_EQ(
      std::chrono::steady_clock::time_point(), socket2->getConnectStartTime());
  EXPECT_EQ(
      std::chrono::steady_clock::time_point(), socket2->getConnectEndTime());

  // should have connection  establish time, as passed on orig construction
  ASSERT_TRUE(socket2->getConnectionEstablishTime().has_value());
  EXPECT_EQ(
      connectionEstablishTime, socket2->getConnectionEstablishTime().value());
}

///////////////////////////////////////////////////////////////////////////
// connect() tests
///////////////////////////////////////////////////////////////////////////

class AsyncSocketConnectTFOTest
    : public ::testing::TestWithParam<ConnectTestParam> {
 protected:
  void SetUp() override {
    if (std::get<0>(GetParam()) == BackendType::IO_URING) {
      try {
        evb_ = makeIoUringEventBase(ioUringOptionsWithProvidedBuffers);
      } catch (IoUringBackend::NotAvailable const&) {
        GTEST_SKIP() << "IoUringBackend not available";
      }
    } else {
      evb_ = std::make_unique<EventBase>();
    }
  }

  EventBase& getEventBase() { return *evb_; }

  BackendType getBackendType() const { return std::get<0>(GetParam()); }
  TFOState getTFOState() const { return std::get<1>(GetParam()); }

 private:
  std::unique_ptr<EventBase> evb_;
};

INSTANTIATE_TEST_SUITE_P(
    ConnectTFOTests,
    AsyncSocketConnectTFOTest,
    ::testing::ValuesIn(getBackendTFOTestingValues()),
    [](const ::testing::TestParamInfo<ConnectTestParam>& info) {
      std::string name = std::get<0>(info.param) == BackendType::IO_URING
          ? "IoUringBackend"
          : "DefaultBackend";
      name += std::get<1>(info.param) == TFOState::ENABLED
          ? "_TFOEnabled"
          : "_TFODisabled";
      return name;
    });

/**
 * Test connecting to a server
 */
TEST_P(AsyncSocketTest, Connect) {
  // Start listening on a local port
  TestServer server;

  // Connect using a AsyncSocket
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::UniquePtr(new AsyncSocket(&evb));
  EXPECT_EQ(
      std::chrono::steady_clock::time_point(), socket->getConnectStartTime());
  EXPECT_EQ(
      std::chrono::steady_clock::time_point(), socket->getConnectEndTime());
  EXPECT_FALSE(socket->getConnectionEstablishTime().has_value());
  ConnCallback cb;
  const auto startedAt = std::chrono::steady_clock::now();
  socket->connect(&cb, server.getAddress(), 30);

  evb.loop();
  const auto finishedAt = std::chrono::steady_clock::now();

  ASSERT_EQ(cb.state, STATE_SUCCEEDED);
  EXPECT_LE(0, socket->getConnectTime().count());
  EXPECT_EQ(std::chrono::milliseconds(30), socket->getConnectTimeout());

  EXPECT_GE(socket->getConnectStartTime(), startedAt);
  EXPECT_LE(socket->getConnectStartTime(), socket->getConnectEndTime());
  EXPECT_LE(socket->getConnectEndTime(), finishedAt);

  // since connect() successful, the establish time == connect() end time
  ASSERT_TRUE(socket->getConnectionEstablishTime().has_value());
  EXPECT_EQ(
      socket->getConnectEndTime(),
      socket->getConnectionEstablishTime().value());
}

TEST_P(AsyncSocketTest, ConnectWithBoundFd) {
  TestServer server;
  EventBase& evb = getEventBase();
  const auto& serverAddr = server.getAddress();
  int sockfd = ::socket(serverAddr.getFamily(), SOCK_STREAM, 0);

  uint16_t boundPort = 0;
  for (uint16_t port = 1024; port <= 65535; port++) {
    struct sockaddr_storage bindStorage{};
    socklen_t bindLen;
    if (serverAddr.getFamily() == AF_INET6) {
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
    if (::bind(sockfd, (struct sockaddr*)&bindStorage, bindLen) == 0) {
      boundPort = port;
      break;
    }
  }
  if (boundPort == 0) {
    GTEST_SKIP() << "Unable to find a free port to bind() to";
  }

  ConnCallback cb;
  auto socket = AsyncSocket::UniquePtr(new AsyncSocket(&evb));
  socket->connect(
      &cb,
      serverAddr,
      30,
      folly::emptySocketOptionMap,
      folly::AsyncSocketTransport::BindOptions{folly::NetworkSocket(sockfd)});

  evb.loop();

  ASSERT_EQ(cb.state, STATE_SUCCEEDED);
  folly::SocketAddress localAddr;
  socket->getLocalAddress(&localAddr);
  EXPECT_EQ(localAddr.getPort(), boundPort);
}

TEST_P(AsyncSocketTest, ConnectWithUnboundFd) {
  TestServer server;
  EventBase& evb = getEventBase();
  const auto& serverAddr = server.getAddress();
  int sockfd = ::socket(serverAddr.getFamily(), SOCK_STREAM, 0);
  ASSERT_GE(sockfd, 0);

  ConnCallback cb;
  auto socket = AsyncSocket::UniquePtr(new AsyncSocket(&evb));
  socket->connect(
      &cb,
      serverAddr,
      30,
      folly::emptySocketOptionMap,
      folly::AsyncSocketTransport::BindOptions{folly::NetworkSocket(sockfd)});

  evb.loop();

  ASSERT_EQ(cb.state, STATE_SUCCEEDED);
  folly::SocketAddress localAddr;
  socket->getLocalAddress(&localAddr);
  EXPECT_GT(localAddr.getPort(), 0);
}

TEST_P(AsyncSocketTest, ConnectWithAlreadyConnectedFd) {
  TestServer server;
  EventBase& evb = getEventBase();
  const auto& serverAddr = server.getAddress();

  int connectedFd = ::socket(serverAddr.getFamily(), SOCK_STREAM, 0);
  ASSERT_GE(connectedFd, 0);
  struct sockaddr_storage ss{};
  serverAddr.getAddress(&ss);
  ASSERT_EQ(
      ::connect(connectedFd, (struct sockaddr*)&ss, serverAddr.getActualSize()),
      0);

  ConnCallback cb;
  auto socket = AsyncSocket::UniquePtr(new AsyncSocket(&evb));
  socket->connect(
      &cb,
      serverAddr,
      30,
      folly::emptySocketOptionMap,
      folly::AsyncSocketTransport::BindOptions{
          folly::NetworkSocket(connectedFd)});

  evb.loop();

  ASSERT_EQ(cb.state, STATE_FAILED);
  EXPECT_EQ(cb.exception.getType(), AsyncSocketException::INVALID_STATE);
  EXPECT_EQ(fcntl(connectedFd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
}

/**
 * Test connecting to a server, then move the socket.¸
 */
TEST_P(AsyncSocketTest, ConnectThenMove) {
  // Start listening on a local port
  TestServer server;

  // Connect using a AsyncSocket
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::UniquePtr(new AsyncSocket(&evb));
  EXPECT_EQ(
      std::chrono::steady_clock::time_point(), socket->getConnectStartTime());
  EXPECT_EQ(
      std::chrono::steady_clock::time_point(), socket->getConnectEndTime());
  EXPECT_FALSE(socket->getConnectionEstablishTime().has_value());
  ConnCallback cb;
  const auto startedAt = std::chrono::steady_clock::now();
  socket->connect(&cb, server.getAddress(), 30);

  evb.loop();
  const auto finishedAt = std::chrono::steady_clock::now();

  ASSERT_EQ(cb.state, STATE_SUCCEEDED);
  EXPECT_LE(0, socket->getConnectTime().count());
  EXPECT_EQ(std::chrono::milliseconds(30), socket->getConnectTimeout());

  EXPECT_GE(socket->getConnectStartTime(), startedAt);
  EXPECT_LE(socket->getConnectStartTime(), socket->getConnectEndTime());
  EXPECT_LE(socket->getConnectEndTime(), finishedAt);

  // since connect() successful, the establish time == connect() end time
  ASSERT_TRUE(socket->getConnectionEstablishTime().has_value());
  EXPECT_EQ(
      socket->getConnectEndTime(),
      socket->getConnectionEstablishTime().value());

  // store timings, then move the socket
  const auto connectStartTime = socket->getConnectStartTime();
  const auto connectEndTime = socket->getConnectEndTime();
  auto socket2 = AsyncSocket::UniquePtr(new AsyncSocket(std::move(socket)));

  // timings should have been moved with the socket
  EXPECT_EQ(connectStartTime, socket2->getConnectStartTime());
  EXPECT_EQ(connectEndTime, socket2->getConnectEndTime());
  ASSERT_TRUE(socket2->getConnectionEstablishTime().has_value());
  EXPECT_EQ(connectEndTime, socket2->getConnectionEstablishTime().value());
}

/**
 * Test connecting to a server that isn't listening.
 */
TEST_P(AsyncSocketTest, ConnectRefused) {
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::UniquePtr(new AsyncSocket(&evb));
  EXPECT_EQ(
      std::chrono::steady_clock::time_point(), socket->getConnectStartTime());
  EXPECT_EQ(
      std::chrono::steady_clock::time_point(), socket->getConnectEndTime());
  EXPECT_FALSE(socket->getConnectionEstablishTime().has_value());

  // Hopefully nothing is actually listening on this address
  folly::SocketAddress addr("127.0.0.1", 65535);
  ConnCallback cb;
  const auto startedAt = std::chrono::steady_clock::now();
  socket->connect(&cb, addr, 30);

  evb.loop();
  const auto finishedAt = std::chrono::steady_clock::now();

  EXPECT_EQ(STATE_FAILED, cb.state);
  EXPECT_EQ(AsyncSocketException::NOT_OPEN, cb.exception.getType());
  EXPECT_LE(0, socket->getConnectTime().count());
  EXPECT_EQ(std::chrono::milliseconds(30), socket->getConnectTimeout());

  EXPECT_GE(socket->getConnectStartTime(), startedAt);
  EXPECT_LE(socket->getConnectStartTime(), socket->getConnectEndTime());
  EXPECT_LE(socket->getConnectEndTime(), finishedAt);

  // since connect() failed, the establish time is empty.
  EXPECT_FALSE(socket->getConnectionEstablishTime().has_value());
}

/**
 * Test connection timeout
 */
TEST_P(AsyncSocketTest, ConnectTimeout) {
  EventBase& evb = getEventBase();

  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);

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
  SocketAddress addr(host, 65535);
  ConnCallback cb;
  const auto startedAt = std::chrono::steady_clock::now();
  socket->connect(&cb, addr, 1); // also set a ridiculously small timeout

  evb.loop();
  const auto finishedAt = std::chrono::steady_clock::now();

  ASSERT_EQ(cb.state, STATE_FAILED);
  if (cb.exception.getType() == AsyncSocketException::NOT_OPEN) {
    // This can happen if we could not route to the IP address picked above.
    // In this case the connect will fail immediately rather than timing out.
    // Just skip the test in this case.
    SKIP() << "do not have a routable but unreachable IP address";
  }
  ASSERT_EQ(cb.exception.getType(), AsyncSocketException::TIMED_OUT);

  EXPECT_LE(0, socket->getConnectTime().count());
  EXPECT_EQ(std::chrono::milliseconds(1), socket->getConnectTimeout());

  EXPECT_GE(socket->getConnectStartTime(), startedAt);
  EXPECT_LE(socket->getConnectStartTime(), socket->getConnectEndTime());
  EXPECT_LE(socket->getConnectEndTime(), finishedAt);

  // since connect() failed, the establish time is empty.
  EXPECT_FALSE(socket->getConnectionEstablishTime().has_value());

  // Verify that we can still get the peer address after a timeout.
  // Use case is if the client was created from a client pool, and we want
  // to log which peer failed.
  folly::SocketAddress peer;
  socket->getPeerAddress(&peer);
  ASSERT_EQ(peer, addr);
  EXPECT_LE(0, socket->getConnectTime().count());
  EXPECT_EQ(socket->getConnectTimeout(), std::chrono::milliseconds(1));
}

class AsyncSocketToSTest : public ::testing::TestWithParam<BackendType> {
 protected:
  using MockDispatcher = ::testing::NiceMock<netops::test::MockDispatcher>;
  using TestObserver = MockAsyncSocketLegacyLifecycleObserverForByteEvents;
  using ByteEventType = AsyncSocket::ByteEvent::Type;

  void SetUp() override {
    if (GetParam() == BackendType::IO_URING) {
      try {
        evb = makeIoUringEventBase(ioUringOptions);
      } catch (IoUringBackend::NotAvailable const&) {
        GTEST_SKIP() << "IoUringBackend not available";
      }
    } else {
      evb = std::make_unique<EventBase>();
    }

    netOpsDispatcher = std::make_shared<MockDispatcher>();
    netOpsDispatcher->forwardToDefaultImpl();
    socket = AsyncSocket::newSocket(evb.get());
    socket->setOverrideNetOpsDispatcher(netOpsDispatcher);
    v6Addr = SocketAddress(
        SocketAddressTestHelper::kGooglePublicDnsAAddrIPv6, 65535);
    v4Addr = SocketAddress(
        SocketAddressTestHelper::kGooglePublicDnsAAddrIPv4, 65535);
  }

  void setupDefaultReturn() {
    EXPECT_CALL(*netOpsDispatcher, setsockopt(_, _, _, _, _))
        .Times(AnyNumber())
        .WillRepeatedly(Return(0));
  }

  void expectNoCallsToTOS() {
    EXPECT_CALL(*netOpsDispatcher, setsockopt(_, IPPROTO_IP, IP_TOS, _, _))
        .Times(0);
    EXPECT_CALL(
        *netOpsDispatcher, setsockopt(_, IPPROTO_IPV6, IPV6_TCLASS, _, _))
        .Times(0);
  }

  std::unique_ptr<EventBase> evb;
  std::shared_ptr<AsyncSocket> socket;
  std::shared_ptr<MockDispatcher> netOpsDispatcher;

  SocketAddress v6Addr;
  SocketAddress v4Addr;
};

INSTANTIATE_TEST_SUITE_P(
    ConnectToSTests,
    AsyncSocketToSTest,
    ::testing::ValuesIn(getBackendTestingValues()),
    [](const ::testing::TestParamInfo<BackendType>& info) {
      return info.param == BackendType::IO_URING
          ? "IoUringBackend"
          : "DefaultBackend";
    });

TEST_P(AsyncSocketToSTest, SetTosOrTrafficClassBeforeConnect) {
  setupDefaultReturn();
  int tos = 1;
  socket->setTosOrTrafficClass(tos);

  EXPECT_CALL(*netOpsDispatcher, setsockopt(_, IPPROTO_IPV6, IPV6_TCLASS, _, _))
      .Times(1)
      .WillOnce(Return(0));
  socket->connect(nullptr, v6Addr, 1000);
}

TEST_P(AsyncSocketToSTest, SetTosOrTrafficClassAfterConnect) {
  setupDefaultReturn();
  int tos = 1;
  expectNoCallsToTOS();
  // EXPECT_CALL(*netOpsDispatcher, connect(_, _, _)).WillOnce(Return(0));
  socket->connect(nullptr, v6Addr, 1000);
  EXPECT_CALL(*netOpsDispatcher, setsockopt(_, IPPROTO_IPV6, IPV6_TCLASS, _, _))
      .Times(1)
      .WillOnce([]() { return 0; });
  socket->setTosOrTrafficClass(tos);
}

TEST_P(AsyncSocketToSTest, SetTosOrTrafficClassIPV4) {
  setupDefaultReturn();
  int tos = 1;
  socket->setTosOrTrafficClass(tos);

  EXPECT_CALL(*netOpsDispatcher, setsockopt(_, IPPROTO_IP, IP_TOS, _, _))
      .Times(1)
      .WillOnce(Return(0));
  socket->connect(nullptr, v4Addr, 1000);
}

TEST_P(AsyncSocketToSTest, SetTosOrTrafficClassError) {
  setupDefaultReturn();
  int tos = 1;
  socket->setTosOrTrafficClass(tos);

  EXPECT_CALL(*netOpsDispatcher, setsockopt(_, IPPROTO_IP, IP_TOS, _, _))
      .Times(1)
      .WillOnce([]() { return -1; });
  ConnCallback ccb;
  socket->connect(&ccb, v4Addr, 1000);
  EXPECT_EQ(ccb.state, StateEnum::STATE_FAILED);
}

/**
 * Test writing immediately after connecting, without waiting for connect
 * to finish.
 */
TEST_P(AsyncSocketConnectTFOTest, ConnectAndWrite) {
  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);

  if (getTFOState() == TFOState::ENABLED) {
    socket->enableTFO();
  }

  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  // write()
  char buf[128];
  memset(buf, 'a', sizeof(buf));
  WriteCallback wcb(true /*enableReleaseIOBufCallback*/);
  // use writeChain so we can pass an IOBuf
  socket->writeChain(&wcb, IOBuf::copyBuffer(buf, sizeof(buf)));

  // Loop.  We don't bother accepting on the server socket yet.
  // The kernel should be able to buffer the write request so it can succeed.
  evb.loop();

  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);
  ASSERT_EQ(wcb.state, STATE_SUCCEEDED);
  ASSERT_EQ(wcb.numIoBufCount, 1);
  ASSERT_EQ(wcb.numIoBufBytes, sizeof(buf));

  // Make sure the server got a connection and received the data
  socket->close();
  server.verifyConnection(buf, sizeof(buf));

  ASSERT_TRUE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
  EXPECT_EQ(socket->getConnectTimeout(), std::chrono::milliseconds(30));
}

/**
 * Test connecting using a nullptr connect callback.
 */
TEST_P(AsyncSocketConnectTFOTest, ConnectNullCallback) {
  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  if (getTFOState() == TFOState::ENABLED) {
    socket->enableTFO();
  }

  socket->connect(nullptr, server.getAddress(), 30);

  // write some data, just so we have some way of verifying
  // that the socket works correctly after connecting
  char buf[128];
  memset(buf, 'a', sizeof(buf));
  WriteCallback wcb;
  socket->write(&wcb, buf, sizeof(buf));

  evb.loop();

  ASSERT_EQ(wcb.state, STATE_SUCCEEDED);

  // Make sure the server got a connection and received the data
  socket->close();
  server.verifyConnection(buf, sizeof(buf));

  ASSERT_TRUE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

/**
 * Test calling both write() and close() immediately after connecting, without
 * waiting for connect to finish.
 *
 * This exercises the STATE_CONNECTING_CLOSING code.
 */
TEST_P(AsyncSocketConnectTFOTest, ConnectWriteAndClose) {
  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  if (getTFOState() == TFOState::ENABLED) {
    socket->enableTFO();
  }
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  // write()
  char buf[128];
  memset(buf, 'a', sizeof(buf));
  WriteCallback wcb;
  socket->write(&wcb, buf, sizeof(buf));

  // close()
  socket->close();

  // Loop.  We don't bother accepting on the server socket yet.
  // The kernel should be able to buffer the write request so it can succeed.
  evb.loop();

  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);
  ASSERT_EQ(wcb.state, STATE_SUCCEEDED);
  // Make sure the server got a connection and received the data
  server.verifyConnection(buf, sizeof(buf));

  ASSERT_TRUE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

/**
 * Test calling two sequential writes for zero-copy. 1st with TFO
 * we fallback to turn off the zc flag and for following writes
 * we continue as usual.
 *
 * Only exercises the io_uring SEND_ZC + TFO first send, so this is its own
 * test suite instantiated with just that one param combo rather than a
 * TEST_P on AsyncSocketConnectTFOTest, which would otherwise generate
 * param variants that can never apply.
 */
#if FOLLY_ALLOW_TFO
class AsyncSocketConnectTFOZeroCopyTest : public AsyncSocketConnectTFOTest {};

INSTANTIATE_TEST_SUITE_P(
    ConnectTFOZeroCopyTests,
    AsyncSocketConnectTFOZeroCopyTest,
    ::testing::Values(
        ConnectTestParam(BackendType::IO_URING, TFOState::ENABLED)),
    [](const ::testing::TestParamInfo<ConnectTestParam>&) {
      return "IoUringBackend_TFOEnabled";
    });

TEST_P(AsyncSocketConnectTFOZeroCopyTest, ConnectWriteZeroCopyFastOpen) {
  TestServer server(/*enableTFO=*/true);
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  socket->enableTFO();
  ASSERT_TRUE(socket->setZeroCopy(true));
  EXPECT_TRUE(socket->getZeroCopy());
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  // 1st write w/ TFO + zc = turn off zc flag and do a copy potentially
  constexpr size_t kLen = 128;
  char firstBuf[kLen];
  memset(firstBuf, 'a', kLen);
  WriteCallback wcb1(true /*enableReleaseIOBufCallback*/);
  socket->writeChain(
      &wcb1, IOBuf::copyBuffer(firstBuf, kLen), WriteFlags::WRITE_MSG_ZEROCOPY);
  evb.loop();
  ASSERT_EQ(wcb1.state, STATE_SUCCEEDED);
  ASSERT_TRUE(socket->getTFOAttempted());

  // 2nd write with just zc, should work as expected.
  char secondBuf[kLen];
  memset(secondBuf, 'b', kLen);
  WriteCallback wcb2(true /*enableReleaseIOBufCallback*/);
  socket->writeChain(
      &wcb2,
      IOBuf::copyBuffer(secondBuf, kLen),
      WriteFlags::WRITE_MSG_ZEROCOPY);
  evb.loop();
  ASSERT_EQ(wcb2.state, STATE_SUCCEEDED);

  socket->close();

  char expected[2 * kLen];
  memset(expected, 'a', kLen);
  memset(expected + kLen, 'b', kLen);
  server.verifyConnection(expected, sizeof(expected));

  ASSERT_TRUE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}
#endif // FOLLY_ALLOW_TFO

/**
 * Zero-copy write on an established (non-TFO) io_uring socket. Exercises the
 * IORING_OP_SENDMSG_ZC fast path directly -- no TFO/FAST_OPEN involved, so the
 * zero-copy flag is not stripped.
 */
TEST_P(AsyncSocketIoUringTest, ConnectWriteZeroCopy) {
  TestServer server;
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  ASSERT_TRUE(socket->setZeroCopy(true));
  EXPECT_TRUE(socket->getZeroCopy());

  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);
  evb.loop();
  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);

  // Established + zero-copy: takes the io_uring SEND_ZC path (mustRegister),
  // not the synchronous performWrite path.
  constexpr size_t kLen = 128;
  char buf[kLen];
  memset(buf, 'a', kLen);
  WriteCallback wcb(true /*enableReleaseIOBufCallback*/);
  socket->writeChain(
      &wcb, IOBuf::copyBuffer(buf, kLen), WriteFlags::WRITE_MSG_ZEROCOPY);
  evb.loop();
  ASSERT_EQ(wcb.state, STATE_SUCCEEDED);

  auto* backend = dynamic_cast<IoUringBackend*>(evb.getBackend());
  ASSERT_NE(backend, nullptr);
  EXPECT_EQ(1, backend->getStats().cqe.zeroCopyNotifCount);

  socket->close();
  server.verifyConnection(buf, kLen);
}

/**
 * Move AsyncSocket between EVBs while a SEND_ZC write is in
 * flight, validating IoUringSendHandle's detach/clone two-CQE path.
 */
TEST_P(AsyncSocketIoUringTest, MoveEventBaseWithInflightZeroCopyWrite) {
  TestServer server;
  EventBase& evb = getEventBase();
  auto evb2 = makeEventBase();

  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  ASSERT_TRUE(socket->setZeroCopy(true));

  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);
  evb.loop();
  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);

  constexpr size_t kLen = 128;
  char buf[kLen];
  memset(buf, 'a', kLen);
  WriteCallback wcb(true /*enableReleaseIOBufCallback*/);
  socket->writeChain(
      &wcb, IOBuf::copyBuffer(buf, kLen), WriteFlags::WRITE_MSG_ZEROCOPY);
  evb.loopOnce();

  ASSERT_TRUE(socket->isDetachable());
  socket->detachEventBase();
  socket->attachEventBase(evb2.get());

  evb.loop();
  evb2->loop();
  ASSERT_EQ(wcb.state, STATE_SUCCEEDED);

  socket->close();
  server.verifyConnection(buf, kLen);
}

/**
 * A partially-written buffer is handed to IoUringSendHandle and re-submitted to
 * io_uring as the socket buffer drains. writeStarting() must reach the
 * WriteCallback exactly once, not once per (re-)submission. Regression test for
 * the double writeStarting() that aborted RocketServerConnection's DCHECK on
 * the native AsyncSocket + io_uring send path.
 */
TEST_P(AsyncSocketIoUringTest, PartialWriteFiresWriteStartingOnce) {
  // Small socket buffers so a large write can't complete in one shot.
  constexpr size_t kSockBufSize = 8 * 1024;
  TestServer server(false, kSockBufSize);

  SocketOptionMap options{
      {{SOL_SOCKET, SO_SNDBUF}, int(kSockBufSize)},
      {{SOL_SOCKET, SO_RCVBUF}, int(kSockBufSize)},
      {{IPPROTO_TCP, TCP_NODELAY}, 1},
  };

  // The receiver drains on this thread; the sender gets its own EVB thread.
  EventBase& senderEvb = getEventBase();
  std::thread senderThread([&]() { senderEvb.loopForever(); });

  ConnCallback ccb;
  WriteCallback wcb;
  std::shared_ptr<AsyncSocket> socket;

  senderEvb.runInEventBaseThreadAndWait([&]() {
    socket = AsyncSocket::newSocket(&senderEvb);
    socket->connect(&ccb, server.getAddress(), 30, options);
  });

  std::shared_ptr<BlockingSocket> acceptedSocket = server.accept();

  // Completion is signalled via an atomic set on the sender thread; wcb's
  // non-atomic fields are only read after that thread is joined.
  std::atomic<bool> writeDone{false};
  wcb.successCallback = [&writeDone]() { writeDone = true; };

  // Big enough to overflow the send+recv buffers, so the first write is partial
  // and its remainder is re-submitted through IoUringSendHandle as we drain.
  constexpr size_t kSendSize = 100 * 1024;
  auto const sendBuf = std::vector<char>(kSendSize, 'a');

  senderEvb.runInEventBaseThreadAndWait([&]() {
    socket->write(&wcb, sendBuf.data(), kSendSize);
  });

  // Drain everything so the write ultimately succeeds.
  std::vector<uint8_t> recvBuf(kSendSize);
  auto bytesRead = acceptedSocket->readAll(recvBuf.data(), recvBuf.size());
  ASSERT_EQ(kSendSize, bytesRead);
  EXPECT_EQ(0, memcmp(recvBuf.data(), sendBuf.data(), bytesRead));

  using clock = std::chrono::steady_clock;
  auto const deadline = clock::now() + std::chrono::seconds(30);
  while (!writeDone.load() && clock::now() < deadline) {
    std::this_thread::yield();
  }

  senderEvb.terminateLoopSoon();
  senderThread.join();
  socket.reset();

  EXPECT_EQ(STATE_SUCCEEDED, wcb.state);
  // The core assertion: exactly one writeStarting despite multiple submits.
  EXPECT_EQ(1, wcb.writeStartingInvocations);
}

/**
 * Test calling close() immediately after connect()
 */
TEST_P(AsyncSocketTest, ConnectAndClose) {
  TestServer server;

  // Connect using a AsyncSocket
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  // Hopefully the connect didn't succeed immediately.
  // If it did, we can't exercise the close-while-connecting code path.
  if (ccb.state == STATE_SUCCEEDED) {
    LOG(INFO) << "connect() succeeded immediately; aborting test "
                 "of close-during-connect behavior";
    return;
  }

  socket->close();

  // Loop, although there shouldn't be anything to do.
  evb.loop();

  // Make sure the connection was aborted
  ASSERT_EQ(ccb.state, STATE_FAILED);

  ASSERT_TRUE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

/**
 * Test calling closeNow() immediately after connect()
 *
 * This should be identical to the normal close behavior.
 */
TEST_P(AsyncSocketTest, ConnectAndCloseNow) {
  TestServer server;

  // Connect using a AsyncSocket
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  // Hopefully the connect didn't succeed immediately.
  // If it did, we can't exercise the close-while-connecting code path.
  if (ccb.state == STATE_SUCCEEDED) {
    LOG(INFO) << "connect() succeeded immediately; aborting test "
                 "of closeNow()-during-connect behavior";
    return;
  }

  socket->closeNow();

  // Loop, although there shouldn't be anything to do.
  evb.loop();

  // Make sure the connection was aborted
  ASSERT_EQ(ccb.state, STATE_FAILED);

  ASSERT_TRUE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

/**
 * Test calling both write() and closeNow() immediately after connecting,
 * without waiting for connect to finish.
 *
 * This should abort the pending write.
 */
TEST_P(AsyncSocketTest, ConnectWriteAndCloseNow) {
  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  // Hopefully the connect didn't succeed immediately.
  // If it did, we can't exercise the close-while-connecting code path.
  if (ccb.state == STATE_SUCCEEDED) {
    LOG(INFO) << "connect() succeeded immediately; aborting test "
                 "of write-during-connect behavior";
    return;
  }

  // write()
  char buf[128];
  memset(buf, 'a', sizeof(buf));
  WriteCallback wcb;
  socket->write(&wcb, buf, sizeof(buf));

  // close()
  socket->closeNow();

  // Loop, although there shouldn't be anything to do.
  evb.loop();

  ASSERT_EQ(ccb.state, STATE_FAILED);
  ASSERT_EQ(wcb.state, STATE_FAILED);

  ASSERT_TRUE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

/**
 * Test installing a read callback immediately, before connect() finishes.
 */
TEST_P(AsyncSocketConnectTFOTest, ConnectAndRead) {
  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  if (getTFOState() == TFOState::ENABLED) {
    socket->enableTFO();
  }

  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  ReadCallback rcb;
  socket->setReadCB(&rcb);

  if (getTFOState() == TFOState::ENABLED) {
    // Trigger a connection
    socket->writeChain(nullptr, IOBuf::copyBuffer("hey"));
  }

  // Even though we haven't looped yet, we should be able to accept
  // the connection and send data to it.
  std::shared_ptr<BlockingSocket> acceptedSocket = server.accept();
  uint8_t buf[128];
  memset(buf, 'a', sizeof(buf));
  acceptedSocket->write(buf, sizeof(buf));
  acceptedSocket->flush();
  acceptedSocket->close();

  // Loop, although there shouldn't be anything to do.
  evb.loop();

  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);
  ASSERT_EQ(rcb.buffers.size(), 1);
  ASSERT_EQ(rcb.buffers[0].length, sizeof(buf));
  ASSERT_EQ(memcmp(rcb.buffers[0].buffer, buf, sizeof(buf)), 0);

  ASSERT_FALSE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

TEST_P(AsyncSocketConnectTFOTest, ConnectAndReadZC) {
  TestServer server;

  // connect()
  EventBase::Options opt;
  opt.setBackendFactory([]() -> std::unique_ptr<folly::EventBaseBackendBase> {
    return std::make_unique<TestEventBaseBackend>();
  });
  EventBase evb(std::move(opt));
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  if (getTFOState() == TFOState::ENABLED) {
    socket->enableTFO();
  }
  auto backend = dynamic_cast<TestEventBaseBackend*>(evb.getBackend());

  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  ReadCallback rcb;
  rcb.setReadMode(AsyncReader::ReadCallback::ReadMode::ReadZC);
  socket->setReadCB(&rcb);
  if (getTFOState() == TFOState::ENABLED) {
    // Trigger a connection
    socket->writeChain(nullptr, IOBuf::copyBuffer("hey"));
  }

  // Even though we haven't looped yet, we should be able to accept
  // the connection and send data to it.
  std::shared_ptr<BlockingSocket> acceptedSocket = server.accept();
  uint8_t buf[128];
  memset(buf, 'a', sizeof(buf));
  acceptedSocket->write(buf, sizeof(buf));
  acceptedSocket->flush();
  acceptedSocket->close();

  // Loop, although there shouldn't be anything to do.
  evb.loop();

  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);
  ASSERT_TRUE(backend->queued);
  // ReadZC is async and oneshot. To prevent level triggering of the socket
  // while the request is issued async but not yet completed, the callback is
  // uninstalled.
  ASSERT_EQ(socket->getReadCallback(), nullptr);

  // The real backend would call this on completion of the ReadZC request. But
  // in the test we have to call it explicitly.
  backend->recvZcCb(backend->bytes);
  ASSERT_EQ(rcb.buffers.size(), 1);
  ASSERT_EQ(rcb.buffers[0].length, sizeof(buf));
  ASSERT_EQ(memcmp(rcb.buffers[0].buffer, buf, sizeof(buf)), 0);

  ASSERT_FALSE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

TEST_P(AsyncSocketConnectTFOTest, ConnectAndReadv) {
  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  if (getTFOState() == TFOState::ENABLED) {
    socket->enableTFO();
  }

  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  static constexpr size_t kBuffSize = 10;
  static constexpr size_t kLen = 40;
  static constexpr size_t kDataSize = 128;

  ReadvCallback rcb(kBuffSize, kLen);
  socket->setReadCB(&rcb);

  if (getTFOState() == TFOState::ENABLED) {
    // Trigger a connection
    socket->writeChain(nullptr, IOBuf::copyBuffer("hey"));
  }

  // Even though we haven't looped yet, we should be able to accept
  // the connection and send data to it.
  std::shared_ptr<BlockingSocket> acceptedSocket = server.accept();
  std::string data(kDataSize, 'A');
  acceptedSocket->write(
      reinterpret_cast<unsigned char*>(data.data()), data.size());
  acceptedSocket->flush();
  acceptedSocket->close();

  // Loop, although there shouldn't be anything to do.
  evb.loop();

  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);
  rcb.verifyData(data);

  ASSERT_FALSE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

TEST_P(AsyncSocketConnectTFOTest, ConnectAndZeroCopyRead) {
  if (getTFOState() == TFOState::ENABLED && folly::kIsArchAArch64) {
    GTEST_SKIP() << "TFO and ZC Read has different alignments";
  }

  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  if (getTFOState() == TFOState::ENABLED) {
    socket->enableTFO();
  }

  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  static const size_t kBuffSize = sysconf(_SC_PAGESIZE);
  static const size_t kPageMult = kBuffSize / 4096;
  static const size_t kDataSize = kPageMult * 8 * 1024;
  assert(kDataSize > kBuffSize);
  assert(kDataSize % kBuffSize == 0);

  static const size_t kNumEntries = 1024;
  static const size_t kEntrySize = kPageMult * 32 * 1024;

  auto memStore =
      AsyncSocket::createDefaultZeroCopyMemStore(kNumEntries, kEntrySize);
  ZeroCopyReadCallback rcb(memStore.get(), kBuffSize);
  socket->setReadCB(&rcb);

  if (getTFOState() == TFOState::ENABLED) {
    // Trigger a connection
    socket->writeChain(nullptr, IOBuf::copyBuffer("hey"));
  }

  // Even though we haven't looped yet, we should be able to accept
  // the connection and send data to it.
  std::shared_ptr<BlockingSocket> acceptedSocket = server.accept();
  std::string data(kDataSize, ' ');
  // generate random data
  std::mt19937 rng(folly::randomNumberSeed());
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<char>(rng());
  }
  auto ret = acceptedSocket->write(
      reinterpret_cast<unsigned char*>(data.data()), data.size());
  ASSERT_EQ(ret, data.size());
  acceptedSocket->flush();
  acceptedSocket->close();

  // Loop
  evb.loop();

  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);
  rcb.verifyData(data);
  ASSERT_EQ(socket->getAppBytesReceived(), data.size());

  ASSERT_FALSE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

/**
 * Test installing a read callback and then closing immediately before the
 * connect attempt finishes.
 */
TEST_P(AsyncSocketTest, ConnectReadAndClose) {
  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  // Hopefully the connect didn't succeed immediately.
  // If it did, we can't exercise the close-while-connecting code path.
  if (ccb.state == STATE_SUCCEEDED) {
    LOG(INFO) << "connect() succeeded immediately; aborting test "
                 "of read-during-connect behavior";
    return;
  }

  ReadCallback rcb;
  socket->setReadCB(&rcb);

  // close()
  socket->close();

  // Loop, although there shouldn't be anything to do.
  evb.loop();

  ASSERT_EQ(ccb.state, STATE_FAILED); // we aborted the close attempt
  ASSERT_EQ(rcb.buffers.size(), 0);
  ASSERT_EQ(rcb.state, STATE_SUCCEEDED); // this indicates EOF

  ASSERT_TRUE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

/**
 * Test both writing and installing a read callback immediately,
 * before connect() finishes.
 */
TEST_P(AsyncSocketConnectTFOTest, ConnectWriteAndRead) {
  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  if (getTFOState() == TFOState::ENABLED) {
    socket->enableTFO();
  }
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  // write()
  char buf1[128];
  memset(buf1, 'a', sizeof(buf1));
  WriteCallback wcb;
  socket->write(&wcb, buf1, sizeof(buf1));

  // set a read callback
  ReadCallback rcb;
  socket->setReadCB(&rcb);

  // Even though we haven't looped yet, we should be able to accept
  // the connection and send data to it.
  std::shared_ptr<BlockingSocket> acceptedSocket = server.accept();
  uint8_t buf2[128];
  memset(buf2, 'b', sizeof(buf2));
  acceptedSocket->write(buf2, sizeof(buf2));
  acceptedSocket->flush();

  // shut down the write half of acceptedSocket, so that the AsyncSocket
  // will stop reading and we can break out of the event loop.
  netops::shutdown(acceptedSocket->getNetworkSocket(), SHUT_WR);

  // Loop
  evb.loop();

  // Make sure the connect succeeded
  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);

  // Make sure the AsyncSocket read the data written by the accepted socket
  ASSERT_EQ(rcb.state, STATE_SUCCEEDED);
  ASSERT_EQ(rcb.buffers.size(), 1);
  ASSERT_EQ(rcb.buffers[0].length, sizeof(buf2));
  ASSERT_EQ(memcmp(rcb.buffers[0].buffer, buf2, sizeof(buf2)), 0);

  // Close the AsyncSocket so we'll see EOF on acceptedSocket
  socket->close();

  // Make sure the accepted socket saw the data written by the AsyncSocket
  uint8_t readbuf[sizeof(buf1)];
  acceptedSocket->readAll(readbuf, sizeof(readbuf));
  ASSERT_EQ(memcmp(buf1, readbuf, sizeof(buf1)), 0);
  uint32_t bytesRead = acceptedSocket->read(readbuf, sizeof(readbuf));
  ASSERT_EQ(bytesRead, 0);

  ASSERT_FALSE(socket->isClosedBySelf());
  ASSERT_TRUE(socket->isClosedByPeer());
}

/**
 * Installing a read callback after the peer has shut down its write side must
 * be reported through readErr(), not by aborting. invalidState() runs the
 * failure path, which can destroy the socket via its callbacks, so setReadCB()
 * has to hold a DestructorGuard before it -- including on the early-return
 * path taken when SHUT_READ is already set.
 */
TEST_P(AsyncSocketTest, SetReadCallbackAfterEOFReportsError) {
  TestServer server;

  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  ReadCallback rcb;
  socket->setReadCB(&rcb);

  // Send a FIN so the socket sees EOF and marks reads shut down while it is
  // otherwise still established.
  std::shared_ptr<BlockingSocket> acceptedSocket = server.accept();
  netops::shutdown(acceptedSocket->getNetworkSocket(), SHUT_WR);

  evb.loop();

  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);
  ASSERT_EQ(rcb.state, STATE_SUCCEEDED); // readEOF()

  ReadCallback rcb2;
  socket->setReadCB(&rcb2);
  EXPECT_EQ(rcb2.state, STATE_FAILED);
  EXPECT_EQ(rcb2.exception.getType(), AsyncSocketException::NOT_OPEN);

  socket->close();
}

/**
 * Test writing to the socket then shutting down writes before the connect
 * attempt finishes.
 */
TEST_P(AsyncSocketTest, ConnectWriteAndShutdownWrite) {
  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  // Hopefully the connect didn't succeed immediately.
  // If it did, we can't exercise the write-while-connecting code path.
  if (ccb.state == STATE_SUCCEEDED) {
    LOG(INFO) << "connect() succeeded immediately; skipping test";
    return;
  }

  // Ask to write some data
  char wbuf[128];
  memset(wbuf, 'a', sizeof(wbuf));
  WriteCallback wcb;
  socket->write(&wcb, wbuf, sizeof(wbuf));
  socket->shutdownWrite();

  // Shutdown writes
  socket->shutdownWrite();

  // Even though we haven't looped yet, we should be able to accept
  // the connection.
  std::shared_ptr<BlockingSocket> acceptedSocket = server.accept();

  // Since the connection is still in progress, there should be no data to
  // read yet.  Verify that the accepted socket is not readable.
  netops::PollDescriptor fds[1];
  fds[0].fd = acceptedSocket->getNetworkSocket();
  fds[0].events = POLLIN;
  fds[0].revents = 0;
  int rc = netops::poll(fds, 1, 0);
  ASSERT_EQ(rc, 0);

  // Write data to the accepted socket
  uint8_t acceptedWbuf[192];
  memset(acceptedWbuf, 'b', sizeof(acceptedWbuf));
  acceptedSocket->write(acceptedWbuf, sizeof(acceptedWbuf));
  acceptedSocket->flush();

  // Loop
  evb.loop();

  // The loop should have completed the connection, written the queued data,
  // and shutdown writes on the socket.
  //
  // Check that the connection was completed successfully and that the write
  // callback succeeded.
  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);
  ASSERT_EQ(wcb.state, STATE_SUCCEEDED);

  // Check that we can read the data that was written to the socket, and that
  // we see an EOF, since its socket was half-shutdown.
  uint8_t readbuf[sizeof(wbuf)];
  acceptedSocket->readAll(readbuf, sizeof(readbuf));
  ASSERT_EQ(memcmp(wbuf, readbuf, sizeof(wbuf)), 0);
  uint32_t bytesRead = acceptedSocket->read(readbuf, sizeof(readbuf));
  ASSERT_EQ(bytesRead, 0);

  // Close the accepted socket.  This will cause it to see EOF
  // and uninstall the read callback when we loop next.
  acceptedSocket->close();

  // Install a read callback, then loop again.
  ReadCallback rcb;
  socket->setReadCB(&rcb);
  evb.loop();

  // This loop should have read the data and seen the EOF
  ASSERT_EQ(rcb.state, STATE_SUCCEEDED);
  ASSERT_EQ(rcb.buffers.size(), 1);
  ASSERT_EQ(rcb.buffers[0].length, sizeof(acceptedWbuf));
  ASSERT_EQ(
      memcmp(rcb.buffers[0].buffer, acceptedWbuf, sizeof(acceptedWbuf)), 0);

  ASSERT_FALSE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

/**
 * Test reading, writing, and shutting down writes before the connect attempt
 * finishes.
 */
TEST_P(AsyncSocketTest, ConnectReadWriteAndShutdownWrite) {
  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  // Hopefully the connect didn't succeed immediately.
  // If it did, we can't exercise the write-while-connecting code path.
  if (ccb.state == STATE_SUCCEEDED) {
    LOG(INFO) << "connect() succeeded immediately; skipping test";
    return;
  }

  // Install a read callback
  ReadCallback rcb;
  socket->setReadCB(&rcb);

  // Ask to write some data
  char wbuf[128];
  memset(wbuf, 'a', sizeof(wbuf));
  WriteCallback wcb;
  socket->write(&wcb, wbuf, sizeof(wbuf));

  // Shutdown writes
  socket->shutdownWrite();

  // Even though we haven't looped yet, we should be able to accept
  // the connection.
  std::shared_ptr<BlockingSocket> acceptedSocket = server.accept();

  // Since the connection is still in progress, there should be no data to
  // read yet.  Verify that the accepted socket is not readable.
  netops::PollDescriptor fds[1];
  fds[0].fd = acceptedSocket->getNetworkSocket();
  fds[0].events = POLLIN;
  fds[0].revents = 0;
  int rc = netops::poll(fds, 1, 0);
  ASSERT_EQ(rc, 0);

  // Write data to the accepted socket
  uint8_t acceptedWbuf[192];
  memset(acceptedWbuf, 'b', sizeof(acceptedWbuf));
  acceptedSocket->write(acceptedWbuf, sizeof(acceptedWbuf));
  acceptedSocket->flush();
  // Shutdown writes to the accepted socket.  This will cause it to see EOF
  // and uninstall the read callback.
  netops::shutdown(acceptedSocket->getNetworkSocket(), SHUT_WR);

  // Loop
  evb.loop();

  // The loop should have completed the connection, written the queued data,
  // shutdown writes on the socket, read the data we wrote to it, and see the
  // EOF.
  //
  // Check that the connection was completed successfully and that the read
  // and write callbacks were invoked as expected.
  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);
  ASSERT_EQ(rcb.state, STATE_SUCCEEDED);
  ASSERT_EQ(rcb.buffers.size(), 1);
  ASSERT_EQ(rcb.buffers[0].length, sizeof(acceptedWbuf));
  ASSERT_EQ(
      memcmp(rcb.buffers[0].buffer, acceptedWbuf, sizeof(acceptedWbuf)), 0);
  ASSERT_EQ(wcb.state, STATE_SUCCEEDED);

  // Check that we can read the data that was written to the socket, and that
  // we see an EOF, since its socket was half-shutdown.
  uint8_t readbuf[sizeof(wbuf)];
  acceptedSocket->readAll(readbuf, sizeof(readbuf));
  ASSERT_EQ(memcmp(wbuf, readbuf, sizeof(wbuf)), 0);
  uint32_t bytesRead = acceptedSocket->read(readbuf, sizeof(readbuf));
  ASSERT_EQ(bytesRead, 0);

  // Fully close both sockets
  acceptedSocket->close();
  socket->close();

  ASSERT_FALSE(socket->isClosedBySelf());
  ASSERT_TRUE(socket->isClosedByPeer());
}

/**
 * Test reading, writing, and calling shutdownWriteNow() before the
 * connect attempt finishes.
 */
TEST_P(AsyncSocketTest, ConnectReadWriteAndShutdownWriteNow) {
  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  // Hopefully the connect didn't succeed immediately.
  // If it did, we can't exercise the write-while-connecting code path.
  if (ccb.state == STATE_SUCCEEDED) {
    LOG(INFO) << "connect() succeeded immediately; skipping test";
    return;
  }

  // Install a read callback
  ReadCallback rcb;
  socket->setReadCB(&rcb);

  // Ask to write some data
  char wbuf[128];
  memset(wbuf, 'a', sizeof(wbuf));
  WriteCallback wcb;
  socket->write(&wcb, wbuf, sizeof(wbuf));

  // Shutdown writes immediately.
  // This should immediately discard the data that we just tried to write.
  socket->shutdownWriteNow();

  // Verify that writeError() was invoked on the write callback.
  ASSERT_EQ(wcb.state, STATE_FAILED);
  ASSERT_EQ(wcb.bytesWritten, 0);

  // Even though we haven't looped yet, we should be able to accept
  // the connection.
  std::shared_ptr<BlockingSocket> acceptedSocket = server.accept();

  // Since the connection is still in progress, there should be no data to
  // read yet.  Verify that the accepted socket is not readable.
  netops::PollDescriptor fds[1];
  fds[0].fd = acceptedSocket->getNetworkSocket();
  fds[0].events = POLLIN;
  fds[0].revents = 0;
  int rc = netops::poll(fds, 1, 0);
  ASSERT_EQ(rc, 0);

  // Write data to the accepted socket
  uint8_t acceptedWbuf[192];
  memset(acceptedWbuf, 'b', sizeof(acceptedWbuf));
  acceptedSocket->write(acceptedWbuf, sizeof(acceptedWbuf));
  acceptedSocket->flush();
  // Shutdown writes to the accepted socket.  This will cause it to see EOF
  // and uninstall the read callback.
  netops::shutdown(acceptedSocket->getNetworkSocket(), SHUT_WR);

  // Loop
  evb.loop();

  // The loop should have completed the connection, written the queued data,
  // shutdown writes on the socket, read the data we wrote to it, and see the
  // EOF.
  //
  // Check that the connection was completed successfully and that the read
  // callback was invoked as expected.
  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);
  ASSERT_EQ(rcb.state, STATE_SUCCEEDED);
  ASSERT_EQ(rcb.buffers.size(), 1);
  ASSERT_EQ(rcb.buffers[0].length, sizeof(acceptedWbuf));
  ASSERT_EQ(
      memcmp(rcb.buffers[0].buffer, acceptedWbuf, sizeof(acceptedWbuf)), 0);

  // Since we used shutdownWriteNow(), it should have discarded all pending
  // write data.  Verify we see an immediate EOF when reading from the accepted
  // socket.
  uint8_t readbuf[sizeof(wbuf)];
  uint32_t bytesRead = acceptedSocket->read(readbuf, sizeof(readbuf));
  ASSERT_EQ(bytesRead, 0);

  // Fully close both sockets
  acceptedSocket->close();
  socket->close();

  ASSERT_FALSE(socket->isClosedBySelf());
  ASSERT_TRUE(socket->isClosedByPeer());
}

// Helper function for use in testConnectOptWrite()
// Temporarily disable the read callback
void tmpDisableReads(AsyncSocket* socket, ReadCallback* rcb) {
  // Uninstall the read callback
  socket->setReadCB(nullptr);
  // Schedule the read callback to be reinstalled after 1ms
  socket->getEventBase()->runInLoop(
      std::bind(&AsyncSocket::setReadCB, socket, rcb));
}

/**
 * Test connect+write, then have the connect callback perform another write.
 *
 * This tests interaction of the optimistic writing after connect with
 * additional write attempts that occur in the connect callback.
 */
void testConnectOptWrite(size_t size1, size_t size2, bool close = false) {
  TestServer server;
  EventBase evb;
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);

  // connect()
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  // Hopefully the connect didn't succeed immediately.
  // If it did, we can't exercise the optimistic write code path.
  if (ccb.state == STATE_SUCCEEDED) {
    LOG(INFO) << "connect() succeeded immediately; aborting test "
                 "of optimistic write behavior";
    return;
  }

  // Tell the connect callback to perform a write when the connect succeeds
  WriteCallback wcb2;
  std::unique_ptr<char[]> buf2(new char[size2]);
  memset(buf2.get(), 'b', size2);
  if (size2 > 0) {
    ccb.successCallback = [&] { socket->write(&wcb2, buf2.get(), size2); };
    // Tell the second write callback to close the connection when it is done
    wcb2.successCallback = [&] { socket->closeNow(); };
  }

  // Schedule one write() immediately, before the connect finishes
  std::unique_ptr<char[]> buf1(new char[size1]);
  memset(buf1.get(), 'a', size1);
  WriteCallback wcb1;
  if (size1 > 0) {
    socket->write(&wcb1, buf1.get(), size1);
  }

  if (close) {
    // immediately perform a close, before connect() completes
    socket->close();
  }

  // Start reading from the other endpoint after 10ms.
  // If we're using large buffers, we have to read so that the writes don't
  // block forever.
  std::shared_ptr<AsyncSocket> acceptedSocket = server.acceptAsync(&evb);
  ReadCallback rcb;
  rcb.dataAvailableCallback =
      std::bind(tmpDisableReads, acceptedSocket.get(), &rcb);
  socket->getEventBase()->tryRunAfterDelay(
      std::bind(&AsyncSocket::setReadCB, acceptedSocket.get(), &rcb), 10);

  // Loop.  We don't bother accepting on the server socket yet.
  // The kernel should be able to buffer the write request so it can succeed.
  evb.loop();

  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);
  if (size1 > 0) {
    ASSERT_EQ(wcb1.state, STATE_SUCCEEDED);
  }
  if (size2 > 0) {
    ASSERT_EQ(wcb2.state, STATE_SUCCEEDED);
  }

  socket->close();

  // Make sure the read callback received all of the data
  size_t bytesRead = 0;
  for (const auto& buffer : rcb.buffers) {
    size_t start = bytesRead;
    bytesRead += buffer.length;
    size_t end = bytesRead;
    if (start < size1) {
      size_t cmpLen = min(size1, end) - start;
      ASSERT_EQ(memcmp(buffer.buffer, buf1.get() + start, cmpLen), 0);
    }
    if (end > size1 && end <= size1 + size2) {
      size_t itOffset;
      size_t buf2Offset;
      size_t cmpLen;
      if (start >= size1) {
        itOffset = 0;
        buf2Offset = start - size1;
        cmpLen = end - start;
      } else {
        itOffset = size1 - start;
        buf2Offset = 0;
        cmpLen = end - size1;
      }
      ASSERT_EQ(
          memcmp(buffer.buffer + itOffset, buf2.get() + buf2Offset, cmpLen), 0);
    }
  }
  ASSERT_EQ(bytesRead, size1 + size2);
}

TEST(AsyncSocketTest, ConnectCallbackWrite) {
  // Test using small writes that should both succeed immediately
  testConnectOptWrite(100, 200);

  // Test using a large buffer in the connect callback, that should block
  const size_t largeSize = 32 * 1024 * 1024;
  testConnectOptWrite(100, largeSize);

  // Test using a large initial write
  testConnectOptWrite(largeSize, 100);

  // Test using two large buffers
  testConnectOptWrite(largeSize, largeSize);

  // Test a small write in the connect callback,
  // but no immediate write before connect completes
  testConnectOptWrite(0, 64);

  // Test a large write in the connect callback,
  // but no immediate write before connect completes
  testConnectOptWrite(0, largeSize);

  // Test connect, a small write, then immediately call close() before connect
  // completes
  testConnectOptWrite(211, 0, true);

  // Test connect, a large immediate write (that will block), then immediately
  // call close() before connect completes
  testConnectOptWrite(largeSize, 0, true);
}

///////////////////////////////////////////////////////////////////////////
// write() related tests
///////////////////////////////////////////////////////////////////////////

/**
 * Test writing using a nullptr callback
 */
TEST_P(AsyncSocketTest, WriteNullCallback) {
  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket =
      AsyncSocket::newSocket(&evb, server.getAddress(), 30);
  evb.loop(); // loop until the socket is connected

  // write() with a nullptr callback
  char buf[128];
  memset(buf, 'a', sizeof(buf));
  socket->write(nullptr, buf, sizeof(buf));

  evb.loop(); // loop until the data is sent

  // Make sure the server got a connection and received the data
  socket->close();
  server.verifyConnection(buf, sizeof(buf));

  ASSERT_TRUE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

/**
 * Test writing with a send timeout
 */
TEST_P(AsyncSocketTest, WriteTimeout) {
  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket =
      AsyncSocket::newSocket(&evb, server.getAddress(), 30);
  evb.loop(); // loop until the socket is connected

  // write() a large chunk of data, with no-one on the other end reading.
  // Tricky: the kernel caches the connection metrics for recently-used
  // routes (see tcp_no_metrics_save) so a freshly opened connection can
  // have a send buffer size bigger than wmem_default.  This makes the test
  // flaky on contbuild if writeLength is < wmem_max (20M on our systems).
  size_t writeLength = 32 * 1024 * 1024;
  uint32_t timeout = 200;
  socket->setSendTimeout(timeout);
  std::unique_ptr<char[]> buf(new char[writeLength]);
  memset(buf.get(), 'a', writeLength);
  WriteCallback wcb;
  socket->write(&wcb, buf.get(), writeLength);

  TimePoint start;
  evb.loop();
  TimePoint end;

  // Make sure the write attempt timed out as requested
  ASSERT_EQ(wcb.state, STATE_FAILED);
  ASSERT_EQ(wcb.exception.getType(), AsyncSocketException::TIMED_OUT);

  // Check that the write timed out within a reasonable period of time.
  // We don't check for exactly the specified timeout, since AsyncSocket only
  // times out when it hasn't made progress for that period of time.
  //
  // On linux, the first write sends a few hundred kb of data, then blocks for
  // writability, and then unblocks again after 40ms and is able to write
  // another smaller of data before blocking permanently.  Therefore it doesn't
  // time out until 40ms + timeout.
  //
  // I haven't fully verified the cause of this, but I believe it probably
  // occurs because the receiving end delays sending an ack for up to 40ms.
  // (This is the default value for TCP_DELACK_MIN.)  Once the sender receives
  // the ack, it can send some more data.  However, after that point the
  // receiver's kernel buffer is full.  This 40ms delay happens even with
  // TCP_NODELAY and TCP_QUICKACK enabled on both endpoints.  However, the
  // kernel may be automatically disabling TCP_QUICKACK after receiving some
  // data.
  //
  // For now, we simply check that the timeout occurred within 160ms of
  // the requested value.
  T_CHECK_TIMEOUT(start, end, milliseconds(timeout), milliseconds(160));
}

/**
 * Test getting local and peer addresses with no fd.
 *
 * Value returned should be empty; no failure should occur.
 */
TEST_P(AsyncSocketTest, GetAddressesNoFd) {
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::newSocket(&evb);

  {
    folly::SocketAddress address;
    socket->getLocalAddress(&address);
    EXPECT_TRUE(address.empty());
  }

  {
    folly::SocketAddress address;
    socket->getPeerAddress(&address);
    EXPECT_TRUE(address.empty());
  }
}

/**
 * Test getting local and peer addresses after connecting.
 */
TEST_P(AsyncSocketTest, GetAddressesAfterConnectGetwhileopenandonclose) {
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::newSocket(&evb);

  // Start listening on a local port
  TestServer server;

  // Connect
  {
    ConnCallback cb;
    socket->connect(&cb, server.getAddress(), 30);
    evb.loop();
    ASSERT_EQ(cb.state, STATE_SUCCEEDED);
  }

  // Get local, make sure it's not empty and not equal to server
  const folly::SocketAddress localAddress = [&socket]() {
    folly::SocketAddress address;
    socket->getLocalAddress(&address);
    return address;
  }();
  EXPECT_FALSE(localAddress.empty());
  EXPECT_NE(server.getAddress(), localAddress);

  const folly::SocketAddress peerAddress = [&socket]() {
    folly::SocketAddress address;
    socket->getPeerAddress(&address);
    return address;
  }();
  EXPECT_FALSE(peerAddress.empty());
  EXPECT_EQ(server.getAddress(), peerAddress);

  // Close
  socket->closeNow();

  // Addresses should still be available as they're cached
  const folly::SocketAddress localAddress2 = [&socket]() {
    folly::SocketAddress address;
    socket->getLocalAddress(&address);
    return address;
  }();
  EXPECT_EQ(localAddress2, localAddress);

  const folly::SocketAddress peerAddress2 = [&socket]() {
    folly::SocketAddress address;
    socket->getPeerAddress(&address);
    return address;
  }();
  EXPECT_EQ(peerAddress2, peerAddress);
}

/**
 * Test getting local and peer addresses after closing.
 *
 * Only peer address is available under these conditions.
 */
TEST_P(AsyncSocketTest, GetAddressesAfterConnectGetonlyafterclose) {
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::newSocket(&evb);

  // Start listening on a local port
  TestServer server;

  // Connect
  {
    ConnCallback cb;
    socket->connect(&cb, server.getAddress(), 30);
    evb.loop();
    ASSERT_EQ(cb.state, STATE_SUCCEEDED);
  }

  // Close
  socket->closeNow();

  // Local address unavailable since never fetched
  {
    folly::SocketAddress address;
    socket->getLocalAddress(&address);
    EXPECT_TRUE(address.empty());
  }

  // Peer address available since it was passed to connect()
  {
    folly::SocketAddress address;
    socket->getPeerAddress(&address);
    EXPECT_FALSE(address.empty());
    EXPECT_EQ(server.getAddress(), address);
  }
}

/**
 * Test getting local and peer addresses after connecting.
 */
TEST_P(AsyncSocketTest, GetAddressesAfterInitFromFdGetoninitandonclose) {
  EventBase& evb = getEventBase();

  // Start listening on a local port
  TestServer server;

  // Create a socket, connect, then create another AsyncSocket from just fd
  auto socket = [&server, &evb]() {
    auto socket1 = AsyncSocket::newSocket(&evb);
    ConnCallback cb;
    socket1->connect(&cb, server.getAddress(), 30);
    evb.loop();
    return AsyncSocket::newSocket(&evb, socket1->detachNetworkSocket());
  }();

  // Get local, make sure it's not empty and not equal to server
  const folly::SocketAddress localAddress = [&socket]() {
    folly::SocketAddress address;
    socket->getLocalAddress(&address);
    return address;
  }();
  EXPECT_FALSE(localAddress.empty());
  EXPECT_NE(server.getAddress(), localAddress);

  const folly::SocketAddress peerAddress = [&socket]() {
    folly::SocketAddress address;
    socket->getPeerAddress(&address);
    return address;
  }();
  EXPECT_FALSE(peerAddress.empty());
  EXPECT_EQ(server.getAddress(), peerAddress);

  // Close
  socket->closeNow();

  // Addresses should still be available as they're cached
  const folly::SocketAddress localAddress2 = [&socket]() {
    folly::SocketAddress address;
    socket->getLocalAddress(&address);
    return address;
  }();
  EXPECT_EQ(localAddress2, localAddress);

  const folly::SocketAddress peerAddress2 = [&socket]() {
    folly::SocketAddress address;
    socket->getPeerAddress(&address);
    return address;
  }();
  EXPECT_EQ(peerAddress2, peerAddress);
}

/**
 * Test writing to a socket that the remote endpoint has closed
 */
TEST_P(AsyncSocketTest, WritePipeError) {
  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket =
      AsyncSocket::newSocket(&evb, server.getAddress(), 30);
  socket->setSendTimeout(1000);
  evb.loop(); // loop until the socket is connected

  // accept and immediately close the socket
  std::shared_ptr<BlockingSocket> acceptedSocket = server.accept();
  acceptedSocket->close();

  // write() a large chunk of data
  size_t writeLength = 32 * 1024 * 1024;
  std::unique_ptr<char[]> buf(new char[writeLength]);
  memset(buf.get(), 'a', writeLength);
  WriteCallback wcb;
  socket->write(&wcb, buf.get(), writeLength);

  evb.loop();

  // Make sure the write failed.
  // It would be nice if AsyncSocketException could convey the errno value,
  // so that we could check for EPIPE
  ASSERT_EQ(wcb.state, STATE_FAILED);
  ASSERT_EQ(wcb.exception.getType(), AsyncSocketException::INTERNAL_ERROR);
  if (GetParam() == BackendType::DEFAULT) {
    ASSERT_THAT(
        wcb.exception.what(),
        MatchesRegex(
            kIsMobile
                ? "AsyncSocketException: writev\\(\\) failed \\(peer=.+\\), type = Internal error, errno = .+ \\(Broken pipe\\)"
                : "AsyncSocketException: writev\\(\\) failed \\(peer=.+, local=.+\\), type = Internal error, errno = .+ \\(Broken pipe\\)"));
  } else {
    ASSERT_THAT(
        wcb.exception.what(),
        MatchesRegex(
            "AsyncSocketException: async sendmsg\\(\\) failed \\(peer=.+\\), type = Internal error, errno = .+ \\(Broken pipe\\)"));
  }
  ASSERT_FALSE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

/**
 * Test writing to a socket that has its read side closed
 */
TEST_P(AsyncSocketTest, WriteAfterReadEOF) {
  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket =
      AsyncSocket::newSocket(&evb, server.getAddress(), 30);
  evb.loop(); // loop until the socket is connected

  // Accept the connection
  std::shared_ptr<AsyncSocket> acceptedSocket = server.acceptAsync(&evb);
  ReadCallback rcb;
  acceptedSocket->setReadCB(&rcb);

  // Shutdown the write side of client socket (read side of server socket)
  socket->shutdownWrite();
  evb.loop();

  // Check that accepted socket is still writable
  ASSERT_FALSE(acceptedSocket->good());
  ASSERT_TRUE(acceptedSocket->writable());

  // Write data to accepted socket
  constexpr size_t simpleBufLength = 5;
  char simpleBuf[simpleBufLength];
  memset(simpleBuf, 'a', simpleBufLength);
  WriteCallback wcb;
  acceptedSocket->write(&wcb, simpleBuf, simpleBufLength);
  evb.loop();

  // Make sure we were able to write even after getting a read EOF
  ASSERT_EQ(rcb.state, STATE_SUCCEEDED); // this indicates EOF
  ASSERT_EQ(wcb.state, STATE_SUCCEEDED);
}

/**
 * Test that bytes written is correctly computed in case of write failure
 */
TEST_P(AsyncSocketTest, WriteErrorCallbackBytesWritten) {
  // Send and receive buffer sizes for the sockets.
  // Note that Linux will double this value to allow space for bookkeeping
  // overhead.
  constexpr size_t kSockBufSize = 8 * 1024;
  constexpr size_t kEffectiveSockBufSize = 2 * kSockBufSize;

  TestServer server(false, kSockBufSize);

  SocketOptionMap options{
      {{SOL_SOCKET, SO_SNDBUF}, int(kSockBufSize)},
      {{SOL_SOCKET, SO_RCVBUF}, int(kSockBufSize)},
      {{IPPROTO_TCP, TCP_NODELAY}, 1},
  };

  // The current thread will be used by the receiver - use a separate thread
  // for the sender.
  EventBase& senderEvb = getEventBase();
  std::thread senderThread([&]() { senderEvb.loopForever(); });

  ConnCallback ccb;
  WriteCallback wcb;
  std::shared_ptr<AsyncSocket> socket;

  senderEvb.runInEventBaseThreadAndWait([&]() {
    socket = AsyncSocket::newSocket(&senderEvb);
    socket->connect(&ccb, server.getAddress(), 30, options);
  });

  // accept the socket on the server side
  std::shared_ptr<BlockingSocket> acceptedSocket = server.accept();

  // Send a big (100KB) write so that it is partially written.
  constexpr size_t kSendSize = 100 * 1024;
  auto const sendBuf = std::vector<char>(kSendSize, 'a');

  senderEvb.runInEventBaseThreadAndWait([&]() {
    socket->write(&wcb, sendBuf.data(), kSendSize);
  });

  // Read 20KB of data from the socket to allow the sender to send a bit more
  // data after it initially blocks.
  constexpr size_t kRecvSize = 20 * 1024;
  uint8_t recvBuf[kRecvSize];
  auto bytesRead = acceptedSocket->readAll(recvBuf, sizeof(recvBuf));
  ASSERT_EQ(kRecvSize, bytesRead);
  EXPECT_EQ(0, memcmp(recvBuf, sendBuf.data(), bytesRead));

  // We should be able to send at least the amount of data received plus the
  // send buffer size.  In practice we should probably be able to send
  constexpr size_t kMinExpectedBytesWritten = kRecvSize + kSockBufSize;

  // We shouldn't be able to send more than the amount of data received plus
  // the send buffer size of the sending socket (kEffectiveSockBufSize) plus
  // the receive buffer size on the receiving socket (kEffectiveSockBufSize)
  constexpr size_t kMaxExpectedBytesWritten =
      kRecvSize + kEffectiveSockBufSize + kEffectiveSockBufSize;
  static_assert(
      kMaxExpectedBytesWritten < kSendSize, "kSendSize set too small");

  // Need to delay after receiving 20KB and before closing the receive side so
  // that the send side has a chance to fill the send buffer past.
  using clock = std::chrono::steady_clock;
  auto const deadline = clock::now() + std::chrono::seconds(2);
  while (
      wcb.bytesWritten < kMinExpectedBytesWritten && clock::now() < deadline) {
    std::this_thread::yield();
  }
  acceptedSocket->closeWithReset();

  senderEvb.terminateLoopSoon();
  senderThread.join();
  socket.reset();

  ASSERT_EQ(STATE_FAILED, wcb.state);
  ASSERT_LE(kMinExpectedBytesWritten, wcb.bytesWritten);
  ASSERT_GE(kMaxExpectedBytesWritten, wcb.bytesWritten);
}

/**
 * Test writing a mix of simple buffers and IOBufs
 */
TEST_P(AsyncSocketTest, WriteIOBuf) {
  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  // Accept the connection
  std::shared_ptr<AsyncSocket> acceptedSocket = server.acceptAsync(&evb);
  ReadCallback rcb;
  acceptedSocket->setReadCB(&rcb);

  // Check if EOR tracking flag can be set and reset.
  EXPECT_FALSE(socket->isEorTrackingEnabled());
  socket->setEorTracking(true);
  EXPECT_TRUE(socket->isEorTrackingEnabled());
  socket->setEorTracking(false);
  EXPECT_FALSE(socket->isEorTrackingEnabled());

  // Write a simple buffer to the socket
  constexpr size_t simpleBufLength = 5;
  char simpleBuf[simpleBufLength];
  memset(simpleBuf, 'a', simpleBufLength);
  WriteCallback wcb;
  socket->write(&wcb, simpleBuf, simpleBufLength);

  // Write a single-element IOBuf chain
  size_t buf1Length = 7;
  unique_ptr<IOBuf> buf1(IOBuf::create(buf1Length));
  memset(buf1->writableData(), 'b', buf1Length);
  buf1->append(buf1Length);
  unique_ptr<IOBuf> buf1Copy(buf1->clone());
  WriteCallback wcb2;
  socket->writeChain(&wcb2, std::move(buf1));

  // Write a multiple-element IOBuf chain
  size_t buf2Length = 11;
  unique_ptr<IOBuf> buf2(IOBuf::create(buf2Length));
  memset(buf2->writableData(), 'c', buf2Length);
  buf2->append(buf2Length);
  size_t buf3Length = 13;
  unique_ptr<IOBuf> buf3(IOBuf::create(buf3Length));
  memset(buf3->writableData(), 'd', buf3Length);
  buf3->append(buf3Length);
  buf2->appendToChain(std::move(buf3));
  unique_ptr<IOBuf> buf2Copy(buf2->clone());
  buf2Copy->coalesce();
  WriteCallback wcb3;
  socket->writeChain(&wcb3, std::move(buf2));
  socket->shutdownWrite();

  // Let the reads and writes run to completion
  evb.loop();

  ASSERT_EQ(wcb.state, STATE_SUCCEEDED);
  ASSERT_EQ(wcb2.state, STATE_SUCCEEDED);
  ASSERT_EQ(wcb3.state, STATE_SUCCEEDED);

  // Make sure the reader got the right data in the right order
  ASSERT_EQ(rcb.state, STATE_SUCCEEDED);
  rcb.coalesce();
  ASSERT_EQ(rcb.buffers.size(), 1);
  ASSERT_EQ(
      rcb.buffers[0].length,
      simpleBufLength + buf1Length + buf2Length + buf3Length);
  ASSERT_EQ(memcmp(rcb.buffers[0].buffer, simpleBuf, simpleBufLength), 0);
  ASSERT_EQ(
      memcmp(
          rcb.buffers[0].buffer + simpleBufLength,
          buf1Copy->data(),
          buf1Copy->length()),
      0);
  ASSERT_EQ(
      memcmp(
          rcb.buffers[0].buffer + simpleBufLength + buf1Length,
          buf2Copy->data(),
          buf2Copy->length()),
      0);

  acceptedSocket->close();
  socket->close();

  ASSERT_TRUE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

TEST_P(AsyncSocketTest, WriteIOBufCorked) {
  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  // Accept the connection
  std::shared_ptr<AsyncSocket> acceptedSocket = server.acceptAsync(&evb);
  ReadCallback rcb;
  acceptedSocket->setReadCB(&rcb);

  // Do three writes, 100ms apart, with the "cork" flag set
  // on the second write.  The reader should see the first write
  // arrive by itself, followed by the second and third writes
  // arriving together.
  size_t buf1Length = 5;
  unique_ptr<IOBuf> buf1(IOBuf::create(buf1Length));
  memset(buf1->writableData(), 'a', buf1Length);
  buf1->append(buf1Length);
  size_t buf2Length = 7;
  unique_ptr<IOBuf> buf2(IOBuf::create(buf2Length));
  memset(buf2->writableData(), 'b', buf2Length);
  buf2->append(buf2Length);
  size_t buf3Length = 11;
  unique_ptr<IOBuf> buf3(IOBuf::create(buf3Length));
  memset(buf3->writableData(), 'c', buf3Length);
  buf3->append(buf3Length);
  WriteCallback wcb1;
  socket->writeChain(&wcb1, std::move(buf1));
  WriteCallback wcb2;
  DelayedWrite write2(socket, std::move(buf2), &wcb2, true);
  write2.scheduleTimeout(100);
  WriteCallback wcb3;
  DelayedWrite write3(socket, std::move(buf3), &wcb3, false, true);
  write3.scheduleTimeout(140);

  evb.loop();
  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);
  ASSERT_EQ(wcb1.state, STATE_SUCCEEDED);
  ASSERT_EQ(wcb2.state, STATE_SUCCEEDED);
  if (wcb3.state != STATE_SUCCEEDED) {
    throw(wcb3.exception);
  }
  ASSERT_EQ(wcb3.state, STATE_SUCCEEDED);

  // Make sure the reader got the data with the right grouping
  ASSERT_EQ(rcb.state, STATE_SUCCEEDED);
  ASSERT_EQ(rcb.buffers.size(), 2);
  ASSERT_EQ(rcb.buffers[0].length, buf1Length);
  ASSERT_EQ(rcb.buffers[1].length, buf2Length + buf3Length);

  acceptedSocket->close();
  socket->close();

  ASSERT_TRUE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

/**
 * Test performing a zero-length write
 */
TEST_P(AsyncSocketTest, ZeroLengthWrite) {
  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket =
      AsyncSocket::newSocket(&evb, server.getAddress(), 30);
  evb.loop(); // loop until the socket is connected

  auto acceptedSocket = server.acceptAsync(&evb);
  ReadCallback rcb;
  acceptedSocket->setReadCB(&rcb);

  size_t len1 = 1024 * 1024;
  size_t len2 = 1024 * 1024;
  std::unique_ptr<char[]> buf(new char[len1 + len2]);
  memset(buf.get(), 'a', len1);
  memset(buf.get() + len1, 'b', len2);

  WriteCallback wcb1;
  WriteCallback wcb2;
  WriteCallback wcb3;
  WriteCallback wcb4;
  socket->write(&wcb1, buf.get(), 0);
  socket->write(&wcb2, buf.get(), len1);
  socket->write(&wcb3, buf.get() + len1, 0);
  socket->write(&wcb4, buf.get() + len1, len2);
  socket->close();

  evb.loop(); // loop until the data is sent

  ASSERT_EQ(wcb1.state, STATE_SUCCEEDED);
  ASSERT_EQ(wcb2.state, STATE_SUCCEEDED);
  ASSERT_EQ(wcb3.state, STATE_SUCCEEDED);
  ASSERT_EQ(wcb4.state, STATE_SUCCEEDED);
  rcb.verifyData(buf.get(), len1 + len2);

  ASSERT_TRUE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

TEST_P(AsyncSocketTest, ZeroLengthWritev) {
  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket =
      AsyncSocket::newSocket(&evb, server.getAddress(), 30);
  evb.loop(); // loop until the socket is connected

  auto acceptedSocket = server.acceptAsync(&evb);
  ReadCallback rcb;
  acceptedSocket->setReadCB(&rcb);

  size_t len1 = 1024 * 1024;
  size_t len2 = 1024 * 1024;
  std::unique_ptr<char[]> buf(new char[len1 + len2]);
  memset(buf.get(), 'a', len1);
  memset(buf.get(), 'b', len2);

  WriteCallback wcb;
  constexpr size_t iovCount = 4;
  struct iovec iov[iovCount];
  iov[0].iov_base = buf.get();
  iov[0].iov_len = len1;
  iov[1].iov_base = buf.get() + len1;
  iov[1].iov_len = 0;
  iov[2].iov_base = buf.get() + len1;
  iov[2].iov_len = len2;
  iov[3].iov_base = buf.get() + len1 + len2;
  iov[3].iov_len = 0;

  socket->writev(&wcb, iov, iovCount);
  socket->close();
  evb.loop(); // loop until the data is sent

  ASSERT_EQ(wcb.state, STATE_SUCCEEDED);
  rcb.verifyData(buf.get(), len1 + len2);

  ASSERT_TRUE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

///////////////////////////////////////////////////////////////////////////
// close() related tests
///////////////////////////////////////////////////////////////////////////

/**
 * Test calling close() with pending writes when the socket is already closing.
 */
TEST_P(AsyncSocketTest, ClosePendingWritesWhileClosing) {
  TestServer server;

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  // accept the socket on the server side
  std::shared_ptr<BlockingSocket> acceptedSocket = server.accept();

  // Loop to ensure the connect has completed
  evb.loop();

  // Make sure we are connected
  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);

  // Schedule pending writes, until several write attempts have blocked
  char buf[128];
  memset(buf, 'a', sizeof(buf));
  using WriteCallbackVector = vector<std::shared_ptr<WriteCallback>>;
  WriteCallbackVector writeCallbacks;

  writeCallbacks.reserve(5);
  while (writeCallbacks.size() < 5) {
    std::shared_ptr<WriteCallback> wcb(new WriteCallback);

    socket->write(wcb.get(), buf, sizeof(buf));
    if (wcb->state == STATE_SUCCEEDED) {
      // Succeeded immediately.  Keep performing more writes
      continue;
    }

    // This write is blocked.
    // Have the write callback call close() when writeError() is invoked
    wcb->errorCallback = std::bind(&AsyncSocket::close, socket.get());
    writeCallbacks.push_back(wcb);
  }

  // Call closeNow() to immediately fail the pending writes
  socket->closeNow();

  // Make sure writeError() was invoked on all of the pending write callbacks
  for (const auto& writeCallback : writeCallbacks) {
    ASSERT_EQ((writeCallback)->state, STATE_FAILED);
  }

  ASSERT_TRUE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

///////////////////////////////////////////////////////////////////////////
// ImmediateRead related tests
///////////////////////////////////////////////////////////////////////////

/* AsyncSocket use to verify immediate read works */
class AsyncSocketImmediateRead : public folly::AsyncSocket {
 public:
  bool immediateReadCalled = false;
  explicit AsyncSocketImmediateRead(folly::EventBase* evb) : AsyncSocket(evb) {}

 protected:
  void checkForImmediateRead() noexcept override {
    immediateReadCalled = true;
    AsyncSocket::handleRead();
  }
};

TEST(AsyncSocket, ConnectReadImmediateRead) {
  TestServer server;

  const size_t maxBufferSz = 100;
  const size_t maxReadsPerEvent = 1;
  const size_t expectedDataSz = maxBufferSz * 3;
  char expectedData[expectedDataSz];
  memset(expectedData, 'j', expectedDataSz);

  EventBase evb;
  ReadCallback rcb(maxBufferSz);
  AsyncSocketImmediateRead socket(&evb);
  socket.connect(nullptr, server.getAddress(), 30);

  evb.loop(); // loop until the socket is connected

  socket.setReadCB(&rcb);
  socket.setMaxReadsPerEvent(maxReadsPerEvent);
  socket.immediateReadCalled = false;

  auto acceptedSocket = server.acceptAsync(&evb);

  ReadCallback rcbServer;
  WriteCallback wcbServer;
  rcbServer.dataAvailableCallback = [&]() {
    if (rcbServer.dataRead() == expectedDataSz) {
      // write back all data read
      rcbServer.verifyData(expectedData, expectedDataSz);
      acceptedSocket->write(&wcbServer, expectedData, expectedDataSz);
      acceptedSocket->close();
    }
  };
  acceptedSocket->setReadCB(&rcbServer);

  // write data
  WriteCallback wcb1;
  socket.write(&wcb1, expectedData, expectedDataSz);
  evb.loop();
  ASSERT_EQ(wcb1.state, STATE_SUCCEEDED);
  rcb.verifyData(expectedData, expectedDataSz);
  ASSERT_EQ(socket.immediateReadCalled, true);

  ASSERT_FALSE(socket.isClosedBySelf());
  ASSERT_FALSE(socket.isClosedByPeer());
}

TEST(AsyncSocket, ConnectReadUninstallRead) {
  TestServer server;

  const size_t maxBufferSz = 100;
  const size_t maxReadsPerEvent = 1;
  const size_t expectedDataSz = maxBufferSz * 3;
  char expectedData[expectedDataSz];
  memset(expectedData, 'k', expectedDataSz);

  EventBase evb;
  ReadCallback rcb(maxBufferSz);
  AsyncSocketImmediateRead socket(&evb);
  socket.connect(nullptr, server.getAddress(), 30);

  evb.loop(); // loop until the socket is connected

  socket.setReadCB(&rcb);
  socket.setMaxReadsPerEvent(maxReadsPerEvent);
  socket.immediateReadCalled = false;

  auto acceptedSocket = server.acceptAsync(&evb);

  ReadCallback rcbServer;
  WriteCallback wcbServer;
  rcbServer.dataAvailableCallback = [&]() {
    if (rcbServer.dataRead() == expectedDataSz) {
      // write back all data read
      rcbServer.verifyData(expectedData, expectedDataSz);
      acceptedSocket->write(&wcbServer, expectedData, expectedDataSz);
      acceptedSocket->close();
    }
  };
  acceptedSocket->setReadCB(&rcbServer);

  rcb.dataAvailableCallback = [&]() {
    // we read data and reset readCB
    socket.setReadCB(nullptr);
  };

  // write data
  WriteCallback wcb;
  socket.write(&wcb, expectedData, expectedDataSz);
  evb.loop();
  ASSERT_EQ(wcb.state, STATE_SUCCEEDED);

  /* we should've only read maxBufferSz data since readCallback_
   * was reset in dataAvailableCallback */
  ASSERT_EQ(rcb.dataRead(), maxBufferSz);
  ASSERT_EQ(socket.immediateReadCalled, false);

  ASSERT_FALSE(socket.isClosedBySelf());
  ASSERT_FALSE(socket.isClosedByPeer());
}

// TODO:
// - Test connect() and have the connect callback set the read callback
// - Test connect() and have the connect callback unset the read callback
// - Test reading/writing/closing/destroying the socket in the connect callback
// - Test reading/writing/closing/destroying the socket in the read callback
// - Test reading/writing/closing/destroying the socket in the write callback
// - Test one-way shutdown behavior
// - Test changing the EventBase
//
// - TODO: test multiple threads sharing a AsyncSocket, and detaching from it
//   in connectSuccess(), readDataAvailable(), writeSuccess()

///////////////////////////////////////////////////////////////////////////
// AsyncServerSocket tests
///////////////////////////////////////////////////////////////////////////

/**
 * Make sure accepted sockets have O_NONBLOCK and TCP_NODELAY set
 */
TEST_P(AsyncSocketTest, ServerAcceptOptions) {
  EventBase& eventBase = getEventBase();

  // Create a server socket
  std::shared_ptr<AsyncServerSocket> serverSocket(
      AsyncServerSocket::newSocket(&eventBase));
  serverSocket->bind(0);
  serverSocket->listen(16);
  folly::SocketAddress serverAddress;
  serverSocket->getAddress(&serverAddress);

  // Add a callback to accept one connection then stop the loop
  TestAcceptCallback acceptCallback;
  acceptCallback.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {
        serverSocket->removeAcceptCallback(&acceptCallback, &eventBase);
      });
  acceptCallback.setAcceptErrorFn([&](const std::exception& /* ex */) {
    serverSocket->removeAcceptCallback(&acceptCallback, &eventBase);
  });
  serverSocket->addAcceptCallback(&acceptCallback, &eventBase);
  serverSocket->startAccepting();

  // Connect to the server socket
  std::shared_ptr<AsyncSocket> socket(
      AsyncSocket::newSocket(&eventBase, serverAddress));

  eventBase.loop();

  // Verify that the server accepted a connection
  ASSERT_EQ(acceptCallback.getEvents()->size(), 3);
  ASSERT_EQ(
      acceptCallback.getEvents()->at(0).type, TestAcceptCallback::TYPE_START);
  ASSERT_EQ(
      acceptCallback.getEvents()->at(1).type, TestAcceptCallback::TYPE_ACCEPT);
  ASSERT_EQ(
      acceptCallback.getEvents()->at(2).type, TestAcceptCallback::TYPE_STOP);
  auto fd = acceptCallback.getEvents()->at(1).fd;

#ifndef _WIN32
  // It is not possible to check if a socket is already in non-blocking mode on
  // Windows. Yes really. The accepted connection should already be in
  // non-blocking mode
  int flags = fcntl(fd.toFd(), F_GETFL, 0);
  ASSERT_EQ(flags & O_NONBLOCK, O_NONBLOCK);
#endif

#ifndef TCP_NOPUSH
  // The accepted connection should already have TCP_NODELAY set
  int value;
  socklen_t valueLength = sizeof(value);
  int rc =
      netops::getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, &valueLength);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(value, 1);
#endif
}

TEST(AsyncSocketTest, NapiDispatch) {
  EventBase eventBase;
  std::shared_ptr<AsyncServerSocket> serverSocket(
      AsyncServerSocket::newSocket(&eventBase));
  serverSocket->bind(0);
  serverSocket->listen(16);
  folly::SocketAddress serverAddress;
  serverSocket->getAddress(&serverAddress);

  // Add several EventBases
  EventBase::Options opt1;
  opt1.setBackendFactory([]() -> std::unique_ptr<folly::EventBaseBackendBase> {
    return EventBase::getTestBackend(0);
  });
  EventBase evb1(std::move(opt1));

  EventBase::Options opt2;
  opt2.setBackendFactory([]() -> std::unique_ptr<folly::EventBaseBackendBase> {
    return EventBase::getTestBackend(1);
  });
  EventBase evb2(std::move(opt2));

  EventBase::Options opt3;
  opt3.setBackendFactory([]() -> std::unique_ptr<folly::EventBaseBackendBase> {
    return EventBase::getTestBackend(2);
  });
  EventBase evb3(std::move(opt3));

  int cb1Count = 0;
  int cb2Count = 0;
  int cb3Count = 0;

  // Add several accept callbacks
  TestAcceptCallback cb1;
  TestAcceptCallback cb2;
  TestAcceptCallback cb3;
  cb1.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {
        if (++cb1Count == 3) {
          eventBase.runInEventBaseThread([&] {
            serverSocket->removeAcceptCallback(&cb1, &evb1);
          });
        }
      });

  cb2.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {
        if (++cb2Count == 2) {
          eventBase.runInEventBaseThread([&] {
            serverSocket->removeAcceptCallback(&cb2, &evb2);
          });
        }
      });

  cb3.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {
        if (++cb3Count == 1) {
          eventBase.runInEventBaseThread([&] {
            serverSocket->removeAcceptCallback(&cb3, &evb3);
          });
        }
      });

  // Make several connections to the socket
  std::shared_ptr<AsyncSocket> sock1(
      AsyncSocket::newSocket(&eventBase, serverAddress)); // cb1
  std::shared_ptr<AsyncSocket> sock2(
      AsyncSocket::newSocket(&eventBase, serverAddress)); // cb1
  std::shared_ptr<AsyncSocket> sock3(
      AsyncSocket::newSocket(&eventBase, serverAddress)); // cb1
  std::shared_ptr<AsyncSocket> sock4(
      AsyncSocket::newSocket(&eventBase, serverAddress)); // cb2
  std::shared_ptr<AsyncSocket> sock5(
      AsyncSocket::newSocket(&eventBase, serverAddress)); // cb2
  std::shared_ptr<AsyncSocket> sock6(
      AsyncSocket::newSocket(&eventBase, serverAddress)); // cb3
  folly::SocketAddress sock1addr;
  folly::SocketAddress sock2addr;
  folly::SocketAddress sock3addr;
  folly::SocketAddress sock4addr;
  folly::SocketAddress sock5addr;
  folly::SocketAddress sock6addr;
  sock1->getAddress(&sock1addr);
  sock2->getAddress(&sock2addr);
  sock3->getAddress(&sock3addr);
  sock4->getAddress(&sock4addr);
  sock5->getAddress(&sock5addr);
  sock6->getAddress(&sock6addr);

  serverSocket->setCallbackAssignFunction(
      [&](AsyncServerSocket*, NetworkSocket sock) {
        struct sockaddr_in remoteAddr;
        socklen_t addrLen = sizeof(sockaddr_in);
        ::getpeername(sock.toFd(), (struct sockaddr*)&remoteAddr, &addrLen);
        auto remotePort = ::ntohs(remoteAddr.sin_port);
        if (remotePort == sock1addr.getPort() ||
            remotePort == sock2addr.getPort() ||
            remotePort == sock3addr.getPort()) {
          return 0;
        } else if (
            remotePort == sock4addr.getPort() ||
            remotePort == sock5addr.getPort()) {
          return 1;
        } else if (remotePort == sock6addr.getPort()) {
          return 2;
        }
        return -1;
      });

  // Test having callbacks remove other callbacks before them on the list,
  serverSocket->addAcceptCallback(&cb1, &evb1);
  serverSocket->addAcceptCallback(&cb2, &evb2);
  serverSocket->addAcceptCallback(&cb3, &evb3);
  serverSocket->startAccepting();

  std::vector<std::thread> threads;
  threads.emplace_back([&]() { eventBase.loop(); });
  threads.emplace_back([&]() { evb1.loop(); });
  threads.emplace_back([&]() { evb2.loop(); });
  threads.emplace_back([&]() { evb3.loop(); });

  for (auto& t : threads) {
    t.join();
  }

  ASSERT_EQ(cb1Count, 3);
  ASSERT_EQ(cb2Count, 2);
  ASSERT_EQ(cb3Count, 1);
}

/**
 * Test AsyncServerSocket::removeAcceptCallback()
 */
TEST_P(AsyncSocketTest, RemoveAcceptCallback) {
  // Create a new AsyncServerSocket
  EventBase& eventBase = getEventBase();
  std::shared_ptr<AsyncServerSocket> serverSocket(
      AsyncServerSocket::newSocket(&eventBase));
  serverSocket->bind(0);
  serverSocket->listen(16);
  folly::SocketAddress serverAddress;
  serverSocket->getAddress(&serverAddress);

  // Add several accept callbacks
  TestAcceptCallback cb1;
  TestAcceptCallback cb2;
  TestAcceptCallback cb3;
  TestAcceptCallback cb4;
  TestAcceptCallback cb5;
  TestAcceptCallback cb6;
  TestAcceptCallback cb7;

  // Test having callbacks remove other callbacks before them on the list,
  // after them on the list, or removing themselves.
  //
  // Have callback 2 remove callback 3 and callback 5 the first time it is
  // called.
  int cb2Count = 0;
  cb1.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {
        std::shared_ptr<AsyncSocket> sock2(
            AsyncSocket::newSocket(
                &eventBase, serverAddress)); // cb2: -cb3 -cb5
      });
  cb3.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {});
  cb4.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {
        std::shared_ptr<AsyncSocket> sock3(
            AsyncSocket::newSocket(&eventBase, serverAddress)); // cb4
      });
  cb5.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {
        std::shared_ptr<AsyncSocket> sock5(
            AsyncSocket::newSocket(&eventBase, serverAddress)); // cb7: -cb7
      });
  cb2.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {
        if (cb2Count == 0) {
          serverSocket->removeAcceptCallback(&cb3, nullptr);
          serverSocket->removeAcceptCallback(&cb5, nullptr);
        }
        ++cb2Count;
      });
  // Have callback 6 remove callback 4 the first time it is called,
  // and destroy the server socket the second time it is called
  int cb6Count = 0;
  cb6.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {
        if (cb6Count == 0) {
          serverSocket->removeAcceptCallback(&cb4, nullptr);
          std::shared_ptr<AsyncSocket> sock6(
              AsyncSocket::newSocket(&eventBase, serverAddress)); // cb1
          std::shared_ptr<AsyncSocket> sock7(
              AsyncSocket::newSocket(&eventBase, serverAddress)); // cb2
          std::shared_ptr<AsyncSocket> sock8(
              AsyncSocket::newSocket(&eventBase, serverAddress)); // cb6: stop

        } else {
          serverSocket.reset();
        }
        ++cb6Count;
      });
  // Have callback 7 remove itself
  cb7.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {
        serverSocket->removeAcceptCallback(&cb7, nullptr);
      });

  serverSocket->addAcceptCallback(&cb1, &eventBase);
  serverSocket->addAcceptCallback(&cb2, &eventBase);
  serverSocket->addAcceptCallback(&cb3, &eventBase);
  serverSocket->addAcceptCallback(&cb4, &eventBase);
  serverSocket->addAcceptCallback(&cb5, &eventBase);
  serverSocket->addAcceptCallback(&cb6, &eventBase);
  serverSocket->addAcceptCallback(&cb7, &eventBase);
  serverSocket->startAccepting();

  // Make several connections to the socket
  std::shared_ptr<AsyncSocket> sock1(
      AsyncSocket::newSocket(&eventBase, serverAddress)); // cb1
  std::shared_ptr<AsyncSocket> sock4(
      AsyncSocket::newSocket(&eventBase, serverAddress)); // cb6: -cb4

  // Loop until we are stopped
  eventBase.loop();

  // Check to make sure that the expected callbacks were invoked.
  //
  // NOTE: This code depends on the AsyncServerSocket operating calling all of
  // the AcceptCallbacks in round-robin fashion, in the order that they were
  // added.  The code is implemented this way right now, but the API doesn't
  // explicitly require it be done this way.  If we change the code not to be
  // exactly round robin in the future, we can simplify the test checks here.
  // (We'll also need to update the termination code, since we expect cb6 to
  // get called twice to terminate the loop.)
  ASSERT_EQ(cb1.getEvents()->size(), 4);
  ASSERT_EQ(cb1.getEvents()->at(0).type, TestAcceptCallback::TYPE_START);
  ASSERT_EQ(cb1.getEvents()->at(1).type, TestAcceptCallback::TYPE_ACCEPT);
  ASSERT_EQ(cb1.getEvents()->at(2).type, TestAcceptCallback::TYPE_ACCEPT);
  ASSERT_EQ(cb1.getEvents()->at(3).type, TestAcceptCallback::TYPE_STOP);

  ASSERT_EQ(cb2.getEvents()->size(), 4);
  ASSERT_EQ(cb2.getEvents()->at(0).type, TestAcceptCallback::TYPE_START);
  ASSERT_EQ(cb2.getEvents()->at(1).type, TestAcceptCallback::TYPE_ACCEPT);
  ASSERT_EQ(cb2.getEvents()->at(2).type, TestAcceptCallback::TYPE_ACCEPT);
  ASSERT_EQ(cb2.getEvents()->at(3).type, TestAcceptCallback::TYPE_STOP);

  ASSERT_EQ(cb3.getEvents()->size(), 2);
  ASSERT_EQ(cb3.getEvents()->at(0).type, TestAcceptCallback::TYPE_START);
  ASSERT_EQ(cb3.getEvents()->at(1).type, TestAcceptCallback::TYPE_STOP);

  ASSERT_EQ(cb4.getEvents()->size(), 3);
  ASSERT_EQ(cb4.getEvents()->at(0).type, TestAcceptCallback::TYPE_START);
  ASSERT_EQ(cb4.getEvents()->at(1).type, TestAcceptCallback::TYPE_ACCEPT);
  ASSERT_EQ(cb4.getEvents()->at(2).type, TestAcceptCallback::TYPE_STOP);

  ASSERT_EQ(cb5.getEvents()->size(), 2);
  ASSERT_EQ(cb5.getEvents()->at(0).type, TestAcceptCallback::TYPE_START);
  ASSERT_EQ(cb5.getEvents()->at(1).type, TestAcceptCallback::TYPE_STOP);

  ASSERT_EQ(cb6.getEvents()->size(), 4);
  ASSERT_EQ(cb6.getEvents()->at(0).type, TestAcceptCallback::TYPE_START);
  ASSERT_EQ(cb6.getEvents()->at(1).type, TestAcceptCallback::TYPE_ACCEPT);
  ASSERT_EQ(cb6.getEvents()->at(2).type, TestAcceptCallback::TYPE_ACCEPT);
  ASSERT_EQ(cb6.getEvents()->at(3).type, TestAcceptCallback::TYPE_STOP);

  ASSERT_EQ(cb7.getEvents()->size(), 3);
  ASSERT_EQ(cb7.getEvents()->at(0).type, TestAcceptCallback::TYPE_START);
  ASSERT_EQ(cb7.getEvents()->at(1).type, TestAcceptCallback::TYPE_ACCEPT);
  ASSERT_EQ(cb7.getEvents()->at(2).type, TestAcceptCallback::TYPE_STOP);
}

/**
 * Test AsyncServerSocket::removeAcceptCallback()
 */
TEST_P(AsyncSocketTest, OtherThreadAcceptCallback) {
  // Create a new AsyncServerSocket
  EventBase& eventBase = getEventBase();
  std::shared_ptr<AsyncServerSocket> serverSocket(
      AsyncServerSocket::newSocket(&eventBase));
  serverSocket->bind(0);
  serverSocket->listen(16);
  folly::SocketAddress serverAddress;
  serverSocket->getAddress(&serverAddress);

  // Add several accept callbacks
  TestAcceptCallback cb1;
  auto thread_id = std::this_thread::get_id();
  cb1.setAcceptStartedFn([&]() {
    CHECK_NE(thread_id, std::this_thread::get_id());
    thread_id = std::this_thread::get_id();
  });
  cb1.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {
        ASSERT_EQ(thread_id, std::this_thread::get_id());
        serverSocket->removeAcceptCallback(&cb1, &eventBase);
      });
  cb1.setAcceptStoppedFn([&]() {
    ASSERT_EQ(thread_id, std::this_thread::get_id());
  });

  // Test having callbacks remove other callbacks before them on the list,
  serverSocket->addAcceptCallback(&cb1, &eventBase);
  serverSocket->startAccepting();

  // Make several connections to the socket
  std::shared_ptr<AsyncSocket> sock1(
      AsyncSocket::newSocket(&eventBase, serverAddress)); // cb1

  // Loop in another thread
  auto other = std::thread([&]() { eventBase.loop(); });
  other.join();

  // Check to make sure that the expected callbacks were invoked.
  //
  // NOTE: This code depends on the AsyncServerSocket operating calling all of
  // the AcceptCallbacks in round-robin fashion, in the order that they were
  // added.  The code is implemented this way right now, but the API doesn't
  // explicitly require it be done this way.  If we change the code not to be
  // exactly round robin in the future, we can simplify the test checks here.
  // (We'll also need to update the termination code, since we expect cb6 to
  // get called twice to terminate the loop.)
  ASSERT_EQ(cb1.getEvents()->size(), 3);
  ASSERT_EQ(cb1.getEvents()->at(0).type, TestAcceptCallback::TYPE_START);
  ASSERT_EQ(cb1.getEvents()->at(1).type, TestAcceptCallback::TYPE_ACCEPT);
  ASSERT_EQ(cb1.getEvents()->at(2).type, TestAcceptCallback::TYPE_STOP);
}

void serverSocketSanityTest(
    AsyncServerSocket* serverSocket,
    std::optional<folly::SocketAddress> address = std::nullopt) {
  EventBase* eventBase = serverSocket->getEventBase();
  CHECK(eventBase);

  // Add a callback to accept one connection then stop accepting
  TestAcceptCallback acceptCallback;
  acceptCallback.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {
        serverSocket->removeAcceptCallback(&acceptCallback, eventBase);
      });
  acceptCallback.setAcceptErrorFn([&](const std::exception& /* ex */) {
    serverSocket->removeAcceptCallback(&acceptCallback, eventBase);
  });
  serverSocket->addAcceptCallback(&acceptCallback, eventBase);
  serverSocket->startAccepting();

  // Connect to the server socket
  folly::SocketAddress serverAddress;
  if (address) {
    serverAddress = *address;
  } else {
    serverSocket->getAddress(&serverAddress);
  }
  AsyncSocket::UniquePtr socket(new AsyncSocket(eventBase, serverAddress));

  // Loop to process all events
  eventBase->loop();

  // Verify that the server accepted a connection
  ASSERT_EQ(acceptCallback.getEvents()->size(), 3);
  ASSERT_EQ(
      acceptCallback.getEvents()->at(0).type, TestAcceptCallback::TYPE_START);
  ASSERT_EQ(
      acceptCallback.getEvents()->at(1).type, TestAcceptCallback::TYPE_ACCEPT);
  ASSERT_EQ(
      acceptCallback.getEvents()->at(2).type, TestAcceptCallback::TYPE_STOP);
}

/* Verify that we don't leak sockets if we are destroyed()
 * and there are still writes pending
 *
 * If destroy() only calls close() instead of closeNow(),
 * it would shutdown(writes) on the socket, but it would
 * never be close()'d, and the socket would leak
 */
TEST_P(AsyncSocketTest, DestroyCloseTest) {
  TestServer server;

  // connect()
  auto clientEB = makeEventBase();
  auto serverEB = makeEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(clientEB.get());
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  // Accept the connection
  std::shared_ptr<AsyncSocket> acceptedSocket =
      server.acceptAsync(serverEB.get());
  ReadCallback rcb;
  acceptedSocket->setReadCB(&rcb);

  // Write a large buffer to the socket that is larger than kernel buffer
  size_t simpleBufLength = 5000000;
  char* simpleBuf = new char[simpleBufLength];
  memset(simpleBuf, 'a', simpleBufLength);
  WriteCallback wcb;

  // Let the reads and writes run to completion
  int fd = acceptedSocket->getNetworkSocket().toFd();

  acceptedSocket->write(&wcb, simpleBuf, simpleBufLength);
  socket.reset();
  acceptedSocket.reset();

  // Test that server socket was closed
  folly::test::msvcSuppressAbortOnInvalidParams([&] {
    ssize_t sz = fileops::read(fd, simpleBuf, simpleBufLength);
    ASSERT_EQ(sz, -1);
    ASSERT_EQ(errno, EBADF);
  });
  delete[] simpleBuf;
}

/**
 * Test AsyncServerSocket::useExistingSocket()
 */
TEST_P(AsyncSocketTest, ServerExistingSocket) {
  EventBase& eventBase = getEventBase();

  // Test creating a socket, and letting AsyncServerSocket bind and listen
  {
    // Manually create a socket
    auto fd = netops::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(fd, NetworkSocket());

    // Create a server socket
    AsyncServerSocket::UniquePtr serverSocket(
        new AsyncServerSocket(&eventBase));
    serverSocket->useExistingSocket(fd);
    folly::SocketAddress address;
    serverSocket->getAddress(&address);
    address.setPort(0);
    serverSocket->bind(address);
    serverSocket->listen(16);

    // Make sure the socket works
    serverSocketSanityTest(serverSocket.get());
  }

  // Test creating a socket and binding manually,
  // then letting AsyncServerSocket listen
  {
    // Manually create a socket
    auto fd = netops::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(fd, NetworkSocket());
    // bind
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = INADDR_ANY;
    ASSERT_EQ(
        netops::bind(
            fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)),
        0);
    // Look up the address that we bound to
    folly::SocketAddress boundAddress;
    boundAddress.setFromLocalAddress(fd);

    // Create a server socket
    AsyncServerSocket::UniquePtr serverSocket(
        new AsyncServerSocket(&eventBase));
    serverSocket->useExistingSocket(fd);
    serverSocket->listen(16);

    // Make sure AsyncServerSocket reports the same address that we bound to
    folly::SocketAddress serverSocketAddress;
    serverSocket->getAddress(&serverSocketAddress);
    ASSERT_EQ(boundAddress, serverSocketAddress);

    // Make sure the socket works
    serverSocketSanityTest(serverSocket.get());
  }

  // Test creating a socket, binding and listening manually,
  // then giving it to AsyncServerSocket
  {
    // Manually create a socket
    auto fd = netops::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(fd, NetworkSocket());
    // bind
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = INADDR_ANY;
    ASSERT_EQ(
        netops::bind(
            fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)),
        0);
    // Look up the address that we bound to
    folly::SocketAddress boundAddress;
    boundAddress.setFromLocalAddress(fd);
    // listen
    ASSERT_EQ(netops::listen(fd, 16), 0);

    // Create a server socket
    AsyncServerSocket::UniquePtr serverSocket(
        new AsyncServerSocket(&eventBase));
    serverSocket->useExistingSocket(fd);

    // Make sure AsyncServerSocket reports the same address that we bound to
    folly::SocketAddress serverSocketAddress;
    serverSocket->getAddress(&serverSocketAddress);
    ASSERT_EQ(boundAddress, serverSocketAddress);

    // Make sure the socket works
    serverSocketSanityTest(serverSocket.get());
  }
}

TEST_P(AsyncSocketTest, UnixDomainSocketTest) {
  EventBase& eventBase = getEventBase();

  // Create a server socket
  std::shared_ptr<AsyncServerSocket> serverSocket(
      AsyncServerSocket::newSocket(&eventBase));
  string path(1, 0);
  path.append(folly::to<string>("/anonymous", folly::Random::rand64()));
  folly::SocketAddress serverAddress;
  serverAddress.setFromPath(path);
  serverSocket->bind(serverAddress);
  serverSocket->listen(16);

  // Add a callback to accept one connection then stop the loop
  TestAcceptCallback acceptCallback;
  acceptCallback.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {
        serverSocket->removeAcceptCallback(&acceptCallback, &eventBase);
      });
  acceptCallback.setAcceptErrorFn([&](const std::exception& /* ex */) {
    serverSocket->removeAcceptCallback(&acceptCallback, &eventBase);
  });
  serverSocket->addAcceptCallback(&acceptCallback, &eventBase);
  serverSocket->startAccepting();

  // Connect to the server socket
  std::shared_ptr<AsyncSocket> socket(
      AsyncSocket::newSocket(&eventBase, serverAddress));

  eventBase.loop();

  // Verify that the server accepted a connection
  ASSERT_EQ(acceptCallback.getEvents()->size(), 3);
  ASSERT_EQ(
      acceptCallback.getEvents()->at(0).type, TestAcceptCallback::TYPE_START);
  ASSERT_EQ(
      acceptCallback.getEvents()->at(1).type, TestAcceptCallback::TYPE_ACCEPT);
  ASSERT_EQ(
      acceptCallback.getEvents()->at(2).type, TestAcceptCallback::TYPE_STOP);
  auto fd = acceptCallback.getEvents()->at(1).fd;

#ifndef _WIN32
  // It is not possible to check if a socket is already in non-blocking mode on
  // Windows. Yes really. The accepted connection should already be in
  // non-blocking mode
  int flags = fcntl(fd.toFd(), F_GETFL, 0);
  ASSERT_EQ(flags & O_NONBLOCK, O_NONBLOCK);
#endif
}

#if defined(__linux__)
TEST(AsyncSocketTest, VsockSocketLocal) {
  EventBase eventBase;

  sockaddr_vm addr{};
  memset(&addr, 0, sizeof(addr));
  addr.svm_family = AF_VSOCK;
  addr.svm_cid = VMADDR_CID_LOCAL;
  addr.svm_port = VMADDR_PORT_ANY;

  folly::SocketAddress address;
  address.setFromSockaddr(&addr);

  AsyncServerSocket::UniquePtr serverSocket(new AsyncServerSocket(&eventBase));
  serverSocket->bind(address);
  serverSocket->listen(16);

  auto actualAddress = serverSocket->getAddress();
  EXPECT_NE(actualAddress.getVsockPort(), VMADDR_PORT_ANY);

  serverSocketSanityTest(serverSocket.get());
}
#endif

#if defined(__linux__)
TEST(AsyncSocketTest, VsockSocketAny) {
  EventBase eventBase;

  sockaddr_vm addr{};
  memset(&addr, 0, sizeof(addr));
  addr.svm_family = AF_VSOCK;
  addr.svm_cid = VMADDR_CID_ANY;
  addr.svm_port = VMADDR_PORT_ANY;

  folly::SocketAddress address;
  address.setFromSockaddr(&addr);

  AsyncServerSocket::UniquePtr serverSocket(new AsyncServerSocket(&eventBase));
  serverSocket->bind(address);
  serverSocket->listen(16);

  auto actualAddress = serverSocket->getAddress();
  EXPECT_NE(actualAddress.getVsockPort(), VMADDR_PORT_ANY);

  addr.svm_cid = VMADDR_CID_LOCAL;
  addr.svm_port = actualAddress.getVsockPort();
  address.setFromSockaddr(&addr);

  serverSocketSanityTest(serverSocket.get(), address);
}
#endif

#if defined(__linux__)
TEST(AsyncSocketTest, VsockSocketPortAny) {
  EventBase eventBase;

  sockaddr_vm addr{};
  memset(&addr, 0, sizeof(addr));
  addr.svm_family = AF_VSOCK;
  addr.svm_cid = VMADDR_CID_LOCAL;
  addr.svm_port = VMADDR_PORT_ANY;

  folly::SocketAddress address;
  address.setFromSockaddr(&addr);

  AsyncServerSocket::UniquePtr serverSocket1(new AsyncServerSocket(&eventBase));
  serverSocket1->bind(address);
  serverSocket1->listen(16);

  AsyncServerSocket::UniquePtr serverSocket2(new AsyncServerSocket(&eventBase));
  serverSocket2->bind(address);
  serverSocket2->listen(16);

  EXPECT_NE(serverSocket1->getAddress().getVsockPort(), VMADDR_PORT_ANY);
  EXPECT_NE(
      serverSocket1->getAddress().getVsockPort(),
      serverSocket2->getAddress().getVsockPort());
}
#endif

TEST_P(AsyncSocketTest, ConnectionEventCallbackDefault) {
  EventBase& eventBase = getEventBase();
  TestConnectionEventCallback connectionEventCallback;

  // Create a server socket
  std::shared_ptr<AsyncServerSocket> serverSocket(
      AsyncServerSocket::newSocket(&eventBase));
  serverSocket->setConnectionEventCallback(&connectionEventCallback);
  serverSocket->bind(0);
  serverSocket->listen(16);
  folly::SocketAddress serverAddress;
  serverSocket->getAddress(&serverAddress);

  // Add a callback to accept one connection then stop the loop
  TestAcceptCallback acceptCallback;
  acceptCallback.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {
        serverSocket->removeAcceptCallback(&acceptCallback, nullptr);
      });
  acceptCallback.setAcceptErrorFn([&](const std::exception& /* ex */) {
    serverSocket->removeAcceptCallback(&acceptCallback, nullptr);
  });
  serverSocket->addAcceptCallback(&acceptCallback, &eventBase);
  serverSocket->startAccepting();

  // Connect to the server socket
  std::shared_ptr<AsyncSocket> socket(
      AsyncSocket::newSocket(&eventBase, serverAddress));

  eventBase.loop();

  // Validate the connection event counters
  ASSERT_EQ(connectionEventCallback.getConnectionAccepted(), 1);
  ASSERT_EQ(connectionEventCallback.getConnectionAcceptedError(), 0);
  ASSERT_EQ(connectionEventCallback.getConnectionDropped(), 0);
  ASSERT_EQ(
      connectionEventCallback.getConnectionEnqueuedForAcceptCallback(), 0);
  ASSERT_EQ(connectionEventCallback.getConnectionDequeuedByAcceptCallback(), 0);
  ASSERT_EQ(connectionEventCallback.getBackoffStarted(), 0);
  ASSERT_EQ(connectionEventCallback.getBackoffEnded(), 0);
  ASSERT_EQ(connectionEventCallback.getBackoffError(), 0);
}

TEST_P(AsyncSocketTest, CallbackInPrimaryEventBase) {
  EventBase& eventBase = getEventBase();
  TestConnectionEventCallback connectionEventCallback;

  // Create a server socket
  std::shared_ptr<AsyncServerSocket> serverSocket(
      AsyncServerSocket::newSocket(&eventBase));
  serverSocket->setConnectionEventCallback(&connectionEventCallback);
  serverSocket->bind(0);
  serverSocket->listen(16);
  folly::SocketAddress serverAddress;
  serverSocket->getAddress(&serverAddress);

  // Add a callback to accept one connection then stop the loop
  TestAcceptCallback acceptCallback;
  acceptCallback.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {
        serverSocket->removeAcceptCallback(&acceptCallback, nullptr);
      });
  acceptCallback.setAcceptErrorFn([&](const std::exception& /* ex */) {
    serverSocket->removeAcceptCallback(&acceptCallback, nullptr);
  });
  bool acceptStartedFlag{false};
  acceptCallback.setAcceptStartedFn([&acceptStartedFlag]() {
    acceptStartedFlag = true;
  });
  bool acceptStoppedFlag{false};
  acceptCallback.setAcceptStoppedFn([&acceptStoppedFlag]() {
    acceptStoppedFlag = true;
  });
  serverSocket->addAcceptCallback(&acceptCallback, nullptr);
  serverSocket->startAccepting();

  // Connect to the server socket
  std::shared_ptr<AsyncSocket> socket(
      AsyncSocket::newSocket(&eventBase, serverAddress));

  eventBase.loop();

  ASSERT_TRUE(acceptStartedFlag);
  ASSERT_TRUE(acceptStoppedFlag);
  // Validate the connection event counters
  ASSERT_EQ(connectionEventCallback.getConnectionAccepted(), 1);
  ASSERT_EQ(connectionEventCallback.getConnectionAcceptedError(), 0);
  ASSERT_EQ(connectionEventCallback.getConnectionDropped(), 0);
  ASSERT_EQ(
      connectionEventCallback.getConnectionEnqueuedForAcceptCallback(), 0);
  ASSERT_EQ(connectionEventCallback.getConnectionDequeuedByAcceptCallback(), 0);
  ASSERT_EQ(connectionEventCallback.getBackoffStarted(), 0);
  ASSERT_EQ(connectionEventCallback.getBackoffEnded(), 0);
  ASSERT_EQ(connectionEventCallback.getBackoffError(), 0);
}

TEST_P(AsyncSocketTest, CallbackInSecondaryEventBase) {
  EventBase& eventBase = getEventBase();
  TestConnectionEventCallback connectionEventCallback;

  // Create a server socket
  std::shared_ptr<AsyncServerSocket> serverSocket(
      AsyncServerSocket::newSocket(&eventBase));
  serverSocket->setConnectionEventCallback(&connectionEventCallback);
  serverSocket->bind(0);
  serverSocket->listen(16);
  SocketAddress serverAddress;
  serverSocket->getAddress(&serverAddress);

  // Add a callback to accept one connection then stop the loop
  TestAcceptCallback acceptCallback;
  ScopedEventBaseThread cobThread("ioworker_test");
  acceptCallback.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const SocketAddress& /* addr */) {
        eventBase.runInEventBaseThread([&] {
          serverSocket->removeAcceptCallback(&acceptCallback, nullptr);
        });
      });
  acceptCallback.setAcceptErrorFn([&](const std::exception& /* ex */) {
    eventBase.runInEventBaseThread([&] {
      serverSocket->removeAcceptCallback(&acceptCallback, nullptr);
    });
  });
  std::atomic<bool> acceptStartedFlag{false};
  acceptCallback.setAcceptStartedFn([&]() { acceptStartedFlag = true; });
  Baton<> acceptStoppedFlag;
  acceptCallback.setAcceptStoppedFn([&]() { acceptStoppedFlag.post(); });
  serverSocket->addAcceptCallback(&acceptCallback, cobThread.getEventBase());
  serverSocket->startAccepting();

  // Connect to the server socket
  std::shared_ptr<AsyncSocket> socket(
      AsyncSocket::newSocket(&eventBase, serverAddress));

  eventBase.loop();

  ASSERT_TRUE(acceptStoppedFlag.try_wait_for(std::chrono::seconds(1)));
  ASSERT_TRUE(acceptStartedFlag);
  // Validate the connection event counters
  ASSERT_EQ(connectionEventCallback.getConnectionAccepted(), 1);
  ASSERT_EQ(connectionEventCallback.getConnectionAcceptedError(), 0);
  ASSERT_EQ(connectionEventCallback.getConnectionDropped(), 0);
  ASSERT_EQ(
      connectionEventCallback.getConnectionEnqueuedForAcceptCallback(), 1);
  ASSERT_EQ(connectionEventCallback.getConnectionDequeuedByAcceptCallback(), 1);
  ASSERT_EQ(connectionEventCallback.getBackoffStarted(), 0);
  ASSERT_EQ(connectionEventCallback.getBackoffEnded(), 0);
  ASSERT_EQ(connectionEventCallback.getBackoffError(), 0);
}

/**
 * Test AsyncServerSocket::getNumPendingMessagesInQueue()
 */
TEST_P(AsyncSocketTest, NumPendingMessagesInQueue) {
  EventBase& eventBase = getEventBase();

  // Counter of how many connections have been accepted
  int count = 0;

  // Create a server socket
  auto serverSocket(AsyncServerSocket::newSocket(&eventBase));
  serverSocket->bind(0);
  serverSocket->listen(16);
  folly::SocketAddress serverAddress;
  serverSocket->getAddress(&serverAddress);

  // Add a callback to accept connections
  TestAcceptCallback acceptCallback;
  folly::ScopedEventBaseThread cobThread("ioworker_test");
  acceptCallback.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {
        count++;
        eventBase.runInEventBaseThreadAndWait([&] {
          ASSERT_EQ(4 - count, serverSocket->getNumPendingMessagesInQueue());
        });
        if (count == 4) {
          eventBase.runInEventBaseThread([&] {
            serverSocket->removeAcceptCallback(&acceptCallback, nullptr);
          });
        }
      });
  acceptCallback.setAcceptErrorFn([&](const std::exception& /* ex */) {
    eventBase.runInEventBaseThread([&] {
      serverSocket->removeAcceptCallback(&acceptCallback, nullptr);
    });
  });
  serverSocket->addAcceptCallback(&acceptCallback, cobThread.getEventBase());
  serverSocket->startAccepting();

  // Connect to the server socket, 4 clients, there are 4 connections
  auto socket1(AsyncSocket::newSocket(&eventBase, serverAddress));
  auto socket2(AsyncSocket::newSocket(&eventBase, serverAddress));
  auto socket3(AsyncSocket::newSocket(&eventBase, serverAddress));
  auto socket4(AsyncSocket::newSocket(&eventBase, serverAddress));

  eventBase.loop();
  ASSERT_EQ(4, count);
}

/**
 * Test AsyncTransport::BufferCallback
 */
TEST_P(AsyncSocketTest, BufferTest) {
  TestServer server(false, 1024 * 1024);

  EventBase& evb = getEventBase();
  SocketOptionMap option{{{SOL_SOCKET, SO_SNDBUF}, 128}};
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30, option);

  char buf[100 * 1024];
  memset(buf, 'c', sizeof(buf));
  WriteCallback wcb;
  BufferCallback bcb(socket.get(), sizeof(buf));
  socket->setBufferCallback(&bcb);
  socket->write(&wcb, buf, sizeof(buf), WriteFlags::NONE);

  std::thread t1([&]() { server.verifyConnection(buf, sizeof(buf)); });

  evb.loop();
  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);
  ASSERT_EQ(wcb.state, STATE_SUCCEEDED);

  ASSERT_TRUE(bcb.hasBuffered());
  ASSERT_TRUE(bcb.hasBufferCleared());

  socket->close();

  ASSERT_TRUE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());

  t1.join();
}

TEST_P(AsyncSocketTest, BufferTestChain) {
  TestServer server(false, 1024 * 1024);

  EventBase& evb = getEventBase();
  SocketOptionMap option{{{SOL_SOCKET, SO_SNDBUF}, 128}};
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30, option);

  char buf1[100 * 1024];
  memset(buf1, 'c', sizeof(buf1));
  char buf2[100 * 1024];
  memset(buf2, 'f', sizeof(buf2));

  auto buf = folly::IOBuf::copyBuffer(buf1, sizeof(buf1));
  buf->appendToChain(folly::IOBuf::copyBuffer(buf2, sizeof(buf2)));
  ASSERT_EQ(sizeof(buf1) + sizeof(buf2), buf->computeChainDataLength());

  BufferCallback bcb(socket.get(), buf->computeChainDataLength());
  socket->setBufferCallback(&bcb);

  WriteCallback wcb;
  socket->writeChain(&wcb, buf->clone(), WriteFlags::NONE);

  std::thread t1([&]() {
    buf->coalesce();
    server.verifyConnection(
        reinterpret_cast<const char*>(buf->data()), buf->length());
  });

  evb.loop();
  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);
  ASSERT_EQ(wcb.state, STATE_SUCCEEDED);

  ASSERT_TRUE(bcb.hasBuffered());
  ASSERT_TRUE(bcb.hasBufferCleared());

  socket->close();

  ASSERT_TRUE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
  t1.join();
}

TEST_P(AsyncSocketTest, BufferCallbackKill) {
  TestServer server(false, 1024 * 1024);

  EventBase& evb = getEventBase();
  SocketOptionMap option{{{SOL_SOCKET, SO_SNDBUF}, 128}};
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30, option);
  evb.loopOnce();

  char buf[100 * 1024];
  memset(buf, 'c', sizeof(buf));
  BufferCallback bcb(socket.get(), sizeof(buf));
  socket->setBufferCallback(&bcb);
  WriteCallback wcb;
  wcb.successCallback = [&] {
    ASSERT_TRUE(socket.unique());
    socket.reset();
  };

  // This will trigger AsyncSocket::handleWrite,
  // which calls WriteCallback::writeSuccess,
  // which calls wcb.successCallback above,
  // which tries to delete socket
  // Then, the socket will also try to use this BufferCallback
  // And that should crash us, if there is no DestructorGuard on the stack
  socket->write(&wcb, buf, sizeof(buf), WriteFlags::NONE);

  std::thread t1([&]() { server.verifyConnection(buf, sizeof(buf)); });

  evb.loop();
  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);

  t1.join();
}

TEST_P(AsyncSocketTest, BufferCallbackKillWithIOBuf) {
  TestServer server(false, 1024 * 1024);

  EventBase& evb = getEventBase();
  SocketOptionMap option{{{SOL_SOCKET, SO_SNDBUF}, 128}};
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30, option);
  evb.loopOnce();

  char buf[100 * 1024];
  memset(buf, 'c', sizeof(buf));
  BufferCallback bcb(socket.get(), sizeof(buf));
  socket->setBufferCallback(&bcb);
  WriteCallback wcb(true);
  wcb.successCallback = [&] {
    ASSERT_TRUE(socket.unique());
    socket.reset();
  };

  socket->writeChain(
      &wcb, folly::IOBuf::wrapBuffer(buf, sizeof(buf)), WriteFlags::NONE);

  std::thread t1([&]() { server.verifyConnection(buf, sizeof(buf)); });

  evb.loop();
  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);

  t1.join();
  ASSERT_TRUE(wcb.releaseIOBufCallbackCalled);
}

#if FOLLY_ALLOW_TFO
TEST_P(AsyncSocketTest, ConnectTFO) {
  if (!folly::test::isTFOAvailable()) {
    GTEST_SKIP() << "TFO not supported.";
  }

  // Start listening on a local port
  TestServer server(true);

  // Connect using a AsyncSocket
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  socket->enableTFO();
  ConnCallback cb;
  socket->connect(&cb, server.getAddress(), 30);

  std::array<uint8_t, 128> buf;
  memset(buf.data(), 'a', buf.size());

  std::array<uint8_t, 3> readBuf;
  auto sendBuf = IOBuf::copyBuffer("hey");

  std::thread t([&] {
    auto acceptedSocket = server.accept();
    acceptedSocket->write(buf.data(), buf.size());
    acceptedSocket->flush();
    acceptedSocket->readAll(readBuf.data(), readBuf.size());
    acceptedSocket->close();
  });

  evb.loop();

  ASSERT_EQ(cb.state, STATE_SUCCEEDED);
  EXPECT_LE(0, socket->getConnectTime().count());
  EXPECT_EQ(socket->getConnectTimeout(), std::chrono::milliseconds(30));
  EXPECT_TRUE(socket->getTFOAttempted());

  // Should trigger the connect
  WriteCallback write;
  ReadCallback rcb;
  socket->writeChain(&write, sendBuf->clone());
  socket->setReadCB(&rcb);
  evb.loop();

  t.join();

  EXPECT_EQ(STATE_SUCCEEDED, write.state);
  EXPECT_EQ(0, memcmp(readBuf.data(), sendBuf->data(), readBuf.size()));
  EXPECT_EQ(STATE_SUCCEEDED, rcb.state);
  ASSERT_EQ(1, rcb.buffers.size());
  ASSERT_EQ(sizeof(buf), rcb.buffers[0].length);
  EXPECT_EQ(0, memcmp(rcb.buffers[0].buffer, buf.data(), buf.size()));
  EXPECT_EQ(socket->getTFOFinished(), socket->getTFOSucceeded());
}

TEST_P(AsyncSocketTest, ConnectTFOSupplyEarlyReadCB) {
  if (!folly::test::isTFOAvailable()) {
    GTEST_SKIP() << "TFO not supported.";
  }

  // Start listening on a local port
  TestServer server(true);

  // Connect using a AsyncSocket
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  socket->enableTFO();
  ConnCallback cb;
  socket->connect(&cb, server.getAddress(), 30);
  ReadCallback rcb;
  socket->setReadCB(&rcb);

  std::array<uint8_t, 128> buf;
  memset(buf.data(), 'a', buf.size());

  std::array<uint8_t, 3> readBuf;
  auto sendBuf = IOBuf::copyBuffer("hey");

  std::thread t([&] {
    auto acceptedSocket = server.accept();
    acceptedSocket->write(buf.data(), buf.size());
    acceptedSocket->flush();
    acceptedSocket->readAll(readBuf.data(), readBuf.size());
    acceptedSocket->close();
  });

  evb.loop();

  ASSERT_EQ(cb.state, STATE_SUCCEEDED);
  EXPECT_LE(0, socket->getConnectTime().count());
  EXPECT_EQ(socket->getConnectTimeout(), std::chrono::milliseconds(30));
  EXPECT_TRUE(socket->getTFOAttempted());

  // Should trigger the connect
  WriteCallback write;
  socket->writeChain(&write, sendBuf->clone());
  evb.loop();

  t.join();

  EXPECT_EQ(STATE_SUCCEEDED, write.state);
  EXPECT_EQ(0, memcmp(readBuf.data(), sendBuf->data(), readBuf.size()));
  EXPECT_EQ(STATE_SUCCEEDED, rcb.state);
  ASSERT_EQ(1, rcb.buffers.size());
  ASSERT_EQ(sizeof(buf), rcb.buffers[0].length);
  EXPECT_EQ(0, memcmp(rcb.buffers[0].buffer, buf.data(), buf.size()));
  EXPECT_EQ(socket->getTFOFinished(), socket->getTFOSucceeded());
}

/**
 * Test connecting to a server that isn't listening
 */
TEST_P(AsyncSocketTest, ConnectRefusedImmediatelyTFO) {
  EventBase& evb = getEventBase();

  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);

  socket->enableTFO();

  // Hopefully nothing is actually listening on this address
  folly::SocketAddress addr("::1", 65535);
  ConnCallback cb;
  socket->connect(&cb, addr, 30);

  evb.loop();

  WriteCallback write1;
  // Trigger the connect if TFO attempt is supported.
  socket->writeChain(&write1, IOBuf::copyBuffer("hey"));
  WriteCallback write2;
  socket->writeChain(&write2, IOBuf::copyBuffer("hey"));
  evb.loop();

  if (!socket->getTFOFinished()) {
    EXPECT_EQ(STATE_FAILED, write1.state);
  } else {
    EXPECT_EQ(STATE_SUCCEEDED, write1.state);
    EXPECT_FALSE(socket->getTFOSucceeded());
  }

  EXPECT_EQ(STATE_FAILED, write2.state);

  EXPECT_EQ(STATE_SUCCEEDED, cb.state);
  EXPECT_LE(0, socket->getConnectTime().count());
  EXPECT_EQ(std::chrono::milliseconds(30), socket->getConnectTimeout());
  EXPECT_TRUE(socket->getTFOAttempted());
}

/**
 * Test calling closeNow() immediately after connecting.
 */
TEST_P(AsyncSocketTest, ConnectWriteAndCloseNowTFO) {
  TestServer server(true);

  // connect()
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  socket->enableTFO();

  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  // write()
  std::array<char, 128> buf;
  memset(buf.data(), 'a', buf.size());

  // close()
  socket->closeNow();

  // Loop, although there shouldn't be anything to do.
  evb.loop();

  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);

  ASSERT_TRUE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

/**
 * Test calling close() immediately after connect()
 */
TEST_P(AsyncSocketTest, ConnectAndCloseTFO) {
  TestServer server(true);

  // Connect using a AsyncSocket
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  socket->enableTFO();

  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  socket->close();

  // Loop, although there shouldn't be anything to do.
  evb.loop();

  // Make sure the connection was aborted
  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);

  ASSERT_TRUE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

class MockAsyncTFOSocket : public AsyncSocket {
 public:
  using UniquePtr = std::unique_ptr<MockAsyncTFOSocket, Destructor>;

  explicit MockAsyncTFOSocket(EventBase* evb) : AsyncSocket(evb) {}

  MOCK_METHOD(
      ssize_t,
      tfoSendMsg,
      (NetworkSocket fd, struct msghdr* msg, int msg_flags));
};

TEST_P(AsyncSocketTest, TestTFOUnsupported) {
  TestServer server(true);

  // Connect using a AsyncSocket
  EventBase& evb = getEventBase();
  auto socket = MockAsyncTFOSocket::UniquePtr(new MockAsyncTFOSocket(&evb));
  socket->enableTFO();

  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);
  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);

  ReadCallback rcb;
  socket->setReadCB(&rcb);

  EXPECT_CALL(*socket, tfoSendMsg(_, _, _))
      .WillOnce(SetErrnoAndReturn(EOPNOTSUPP, -1));
  WriteCallback write;
  auto sendBuf = IOBuf::copyBuffer("hey");
  socket->writeChain(&write, sendBuf->clone());
  EXPECT_EQ(STATE_WAITING, write.state);

  std::array<uint8_t, 128> buf;
  memset(buf.data(), 'a', buf.size());

  std::array<uint8_t, 3> readBuf;

  std::thread t([&] {
    std::shared_ptr<BlockingSocket> acceptedSocket = server.accept();
    acceptedSocket->write(buf.data(), buf.size());
    acceptedSocket->flush();
    acceptedSocket->readAll(readBuf.data(), readBuf.size());
    acceptedSocket->close();
  });

  evb.loop();

  t.join();
  EXPECT_EQ(STATE_SUCCEEDED, ccb.state);
  EXPECT_EQ(STATE_SUCCEEDED, write.state);

  EXPECT_EQ(0, memcmp(readBuf.data(), sendBuf->data(), readBuf.size()));
  EXPECT_EQ(STATE_SUCCEEDED, rcb.state);
  ASSERT_EQ(1, rcb.buffers.size());
  ASSERT_EQ(sizeof(buf), rcb.buffers[0].length);
  EXPECT_EQ(0, memcmp(rcb.buffers[0].buffer, buf.data(), buf.size()));
  EXPECT_EQ(socket->getTFOFinished(), socket->getTFOSucceeded());
}

TEST_P(AsyncSocketTest, ConnectRefusedDelayedTFO) {
  EventBase& evb = getEventBase();

  auto socket = MockAsyncTFOSocket::UniquePtr(new MockAsyncTFOSocket(&evb));
  socket->enableTFO();

  // Hopefully this fails
  folly::SocketAddress fakeAddr("127.0.0.1", 65535);
  EXPECT_CALL(*socket, tfoSendMsg(_, _, _))
      .WillOnce(Invoke([&](NetworkSocket fd, struct msghdr*, int) {
        sockaddr_storage addr;
        auto len = fakeAddr.getAddress(&addr);
        auto ret = netops::connect(fd, (const struct sockaddr*)&addr, len);
        LOG(INFO) << "connecting the socket " << fd << " : " << ret << " : "
                  << errno;
        return ret;
      }));

  // Hopefully nothing is actually listening on this address
  ConnCallback cb;
  socket->connect(&cb, fakeAddr, 30);

  WriteCallback write1;
  // Trigger the connect if TFO attempt is supported.
  socket->writeChain(&write1, IOBuf::copyBuffer("hey"));

  if (socket->getTFOFinished()) {
    // This test is useless now.
    return;
  }
  WriteCallback write2;
  // Trigger the connect if TFO attempt is supported.
  socket->writeChain(&write2, IOBuf::copyBuffer("hey"));
  evb.loop();

  EXPECT_EQ(STATE_FAILED, write1.state);
  EXPECT_EQ(STATE_FAILED, write2.state);
  EXPECT_FALSE(socket->getTFOSucceeded());

  EXPECT_EQ(STATE_SUCCEEDED, cb.state);
  EXPECT_LE(0, socket->getConnectTime().count());
  EXPECT_EQ(std::chrono::milliseconds(30), socket->getConnectTimeout());
  EXPECT_TRUE(socket->getTFOAttempted());
}

TEST_P(AsyncSocketTest, TestTFOUnsupportedTimeout) {
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
  SocketAddress addr(host, 65535);

  // Connect using a AsyncSocket
  EventBase& evb = getEventBase();
  auto socket = MockAsyncTFOSocket::UniquePtr(new MockAsyncTFOSocket(&evb));
  socket->enableTFO();

  ConnCallback ccb;
  // Set a very small timeout
  socket->connect(&ccb, addr, 1);
  EXPECT_EQ(STATE_SUCCEEDED, ccb.state);

  ReadCallback rcb;
  socket->setReadCB(&rcb);

  EXPECT_CALL(*socket, tfoSendMsg(_, _, _))
      .WillOnce(SetErrnoAndReturn(EOPNOTSUPP, -1));
  WriteCallback write;
  socket->writeChain(&write, IOBuf::copyBuffer("hey"));

  evb.loop();

  EXPECT_EQ(STATE_FAILED, write.state);
}

TEST_P(AsyncSocketTest, TestTFOFallbackToConnect) {
  TestServer server(true);

  // Connect using a AsyncSocket
  EventBase& evb = getEventBase();
  auto socket = MockAsyncTFOSocket::UniquePtr(new MockAsyncTFOSocket(&evb));
  socket->enableTFO();

  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);
  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);

  ReadCallback rcb;
  socket->setReadCB(&rcb);

  EXPECT_CALL(*socket, tfoSendMsg(_, _, _))
      .WillOnce(Invoke([&](NetworkSocket fd, struct msghdr*, int) {
        sockaddr_storage addr;
        auto len = server.getAddress().getAddress(&addr);
        return netops::connect(fd, (const struct sockaddr*)&addr, len);
      }));
  WriteCallback write;
  auto sendBuf = IOBuf::copyBuffer("hey");
  socket->writeChain(&write, sendBuf->clone());
  EXPECT_EQ(STATE_WAITING, write.state);

  std::array<uint8_t, 128> buf;
  memset(buf.data(), 'a', buf.size());

  std::array<uint8_t, 3> readBuf;

  std::thread t([&] {
    std::shared_ptr<BlockingSocket> acceptedSocket = server.accept();
    acceptedSocket->write(buf.data(), buf.size());
    acceptedSocket->flush();
    acceptedSocket->readAll(readBuf.data(), readBuf.size());
    acceptedSocket->close();
  });

  evb.loop();

  t.join();
  EXPECT_EQ(0, memcmp(readBuf.data(), sendBuf->data(), readBuf.size()));

  EXPECT_EQ(STATE_SUCCEEDED, ccb.state);
  EXPECT_EQ(STATE_SUCCEEDED, write.state);

  EXPECT_EQ(STATE_SUCCEEDED, rcb.state);
  ASSERT_EQ(1, rcb.buffers.size());
  ASSERT_EQ(buf.size(), rcb.buffers[0].length);
  EXPECT_EQ(0, memcmp(rcb.buffers[0].buffer, buf.data(), buf.size()));
}

TEST_P(AsyncSocketTest, TestTFOFallbackTimeout) {
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
  SocketAddress addr(host, 65535);

  // Connect using a AsyncSocket
  EventBase& evb = getEventBase();
  auto socket = MockAsyncTFOSocket::UniquePtr(new MockAsyncTFOSocket(&evb));
  socket->enableTFO();

  ConnCallback ccb;
  // Set a very small timeout
  socket->connect(&ccb, addr, 1);
  EXPECT_EQ(STATE_SUCCEEDED, ccb.state);

  ReadCallback rcb;
  socket->setReadCB(&rcb);

  EXPECT_CALL(*socket, tfoSendMsg(_, _, _))
      .WillOnce(Invoke([&](NetworkSocket fd, struct msghdr*, int) {
        sockaddr_storage addr2;
        auto len = addr.getAddress(&addr2);
        return netops::connect(fd, (const struct sockaddr*)&addr2, len);
      }));
  WriteCallback write;
  socket->writeChain(&write, IOBuf::copyBuffer("hey"));

  evb.loop();

  EXPECT_EQ(STATE_FAILED, write.state);
}

TEST_P(AsyncSocketTest, TestTFOEagain) {
  TestServer server(true);

  // Connect using a AsyncSocket
  EventBase& evb = getEventBase();
  auto socket = MockAsyncTFOSocket::UniquePtr(new MockAsyncTFOSocket(&evb));
  socket->enableTFO();

  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  EXPECT_CALL(*socket, tfoSendMsg(_, _, _))
      .WillOnce(SetErrnoAndReturn(EAGAIN, -1));
  WriteCallback write;
  socket->writeChain(&write, IOBuf::copyBuffer("hey"));

  evb.loop();

  EXPECT_EQ(STATE_SUCCEEDED, ccb.state);
  EXPECT_EQ(STATE_FAILED, write.state);
}

// Sending a large amount of data in the first write which will
// definitely not fit into MSS.
TEST_P(AsyncSocketTest, ConnectTFOWithBigData) {
  if (!folly::test::isTFOAvailable()) {
    GTEST_SKIP() << "TFO not supported.";
  }

  // Start listening on a local port
  TestServer server(true);

  // Connect using a AsyncSocket
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  socket->enableTFO();
  ConnCallback cb;
  socket->connect(&cb, server.getAddress(), 30);

  std::array<uint8_t, 128> buf;
  memset(buf.data(), 'a', buf.size());

  constexpr size_t len = 10 * 1024;
  auto sendBuf = IOBuf::create(len);
  sendBuf->append(len);
  std::array<uint8_t, len> readBuf;

  std::thread t([&] {
    auto acceptedSocket = server.accept();
    acceptedSocket->write(buf.data(), buf.size());
    acceptedSocket->flush();
    acceptedSocket->readAll(readBuf.data(), readBuf.size());
    acceptedSocket->close();
  });

  evb.loop();

  ASSERT_EQ(cb.state, STATE_SUCCEEDED);
  EXPECT_LE(0, socket->getConnectTime().count());
  EXPECT_EQ(socket->getConnectTimeout(), std::chrono::milliseconds(30));
  EXPECT_TRUE(socket->getTFOAttempted());

  // Should trigger the connect
  WriteCallback write;
  ReadCallback rcb;
  socket->writeChain(&write, sendBuf->clone());
  socket->setReadCB(&rcb);
  evb.loop();

  t.join();

  EXPECT_EQ(STATE_SUCCEEDED, write.state);
  EXPECT_EQ(0, memcmp(readBuf.data(), sendBuf->data(), readBuf.size()));
  EXPECT_EQ(STATE_SUCCEEDED, rcb.state);
  ASSERT_EQ(1, rcb.buffers.size());
  ASSERT_EQ(sizeof(buf), rcb.buffers[0].length);
  EXPECT_EQ(0, memcmp(rcb.buffers[0].buffer, buf.data(), buf.size()));
  EXPECT_EQ(socket->getTFOFinished(), socket->getTFOSucceeded());
}

#endif // FOLLY_ALLOW_TFO

class MockEvbChangeCallback : public AsyncSocket::EvbChangeCallback {
 public:
  MOCK_METHOD(void, evbAttached, (AsyncSocket*));
  MOCK_METHOD(void, evbDetached, (AsyncSocket*));
};

TEST_P(AsyncSocketTest, EvbCallbacks) {
  auto cb = std::make_unique<MockEvbChangeCallback>();
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);

  InSequence seq;
  EXPECT_CALL(*cb, evbDetached(socket.get())).Times(1);
  EXPECT_CALL(*cb, evbAttached(socket.get())).Times(1);

  socket->setEvbChangedCallback(std::move(cb));
  socket->detachEventBase();
  socket->attachEventBase(&evb);
}

TEST_P(AsyncSocketTest, TestEvbDetachWtRegisteredIOHandlers) {
  // Start listening on a local port
  TestServer server;

  // Connect using a AsyncSocket
  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  ConnCallback cb;
  socket->connect(&cb, server.getAddress(), 30);

  evb.loop();

  ASSERT_EQ(cb.state, STATE_SUCCEEDED);
  EXPECT_LE(0, socket->getConnectTime().count());
  EXPECT_EQ(socket->getConnectTimeout(), std::chrono::milliseconds(30));

  // After the ioHandlers are registered, still should be able to detach/attach
  ReadCallback rcb;
  socket->setReadCB(&rcb);

  auto cbEvbChg = std::make_unique<MockEvbChangeCallback>();
  InSequence seq;
  EXPECT_CALL(*cbEvbChg, evbDetached(socket.get())).Times(1);
  EXPECT_CALL(*cbEvbChg, evbAttached(socket.get())).Times(1);

  socket->setEvbChangedCallback(std::move(cbEvbChg));
  EXPECT_TRUE(socket->isDetachable());
  socket->detachEventBase();
  socket->attachEventBase(&evb);

  socket->close();
}

// Not a TEST_P(AsyncSocketTest, ...): detachNetworkSocket() is structurally
// unsupported under the io_uring backend (in-flight multishot recv SQEs and
// per-fd provided buffer rings can't be safely handed off synchronously; see
// D91003790 for an abandoned attempt at an async-aware variant). A plain,
// non-parameterized fixture keeps tests like this one off the IoUringBackend
// instantiation entirely, rather than registering them there and skipping at
// runtime.
class AsyncSocketNoIoUringTest : public ::testing::Test {
 protected:
  EventBase& getEventBase() { return evb_; }

 private:
  EventBase evb_;
};

TEST_F(AsyncSocketNoIoUringTest, TestEvbDetachThenClose) {
  // Start listening on a local port
  TestServer server;

  // Connect an AsyncSocket to the server
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::newSocket(&evb);
  ConnCallback cb;
  socket->connect(&cb, server.getAddress(), 30);

  evb.loop();

  ASSERT_EQ(cb.state, STATE_SUCCEEDED);
  EXPECT_LE(0, socket->getConnectTime().count());
  EXPECT_EQ(socket->getConnectTimeout(), std::chrono::milliseconds(30));

  // After the ioHandlers are registered, still should be able to detach/attach
  ReadCallback rcb;
  socket->setReadCB(&rcb);

  auto cbEvbChg = std::make_unique<MockEvbChangeCallback>();
  InSequence seq;
  EXPECT_CALL(*cbEvbChg, evbDetached(socket.get())).Times(1);

  socket->setEvbChangedCallback(std::move(cbEvbChg));

  // Should be possible to destroy/call closeNow() without an attached EventBase
  EXPECT_TRUE(socket->isDetachable());
  socket->detachEventBase();
  socket.reset();
}

TEST_P(AsyncSocketTest, BytesWrittenWithMove) {
  TestServer server;

  EventBase& evb = getEventBase();
  auto socket1 = AsyncSocket::UniquePtr(new AsyncSocket(&evb));
  ConnCallback ccb;
  socket1->connect(&ccb, server.getAddress(), 30);
  std::shared_ptr<BlockingSocket> acceptedSocket = server.accept();

  EXPECT_EQ(0, socket1->getRawBytesWritten());
  std::vector<uint8_t> wbuf(128, 'a');
  WriteCallback wcb;
  socket1->write(&wcb, wbuf.data(), wbuf.size());
  evb.loop();
  ASSERT_EQ(wcb.state, STATE_SUCCEEDED);
  EXPECT_EQ(wbuf.size(), socket1->getRawBytesWritten());
  EXPECT_EQ(wbuf.size(), socket1->getAppBytesWritten());

  auto socket2 = AsyncSocket::UniquePtr(new AsyncSocket(std::move(socket1)));
  EXPECT_EQ(wbuf.size(), socket2->getRawBytesWritten());
  EXPECT_EQ(wbuf.size(), socket2->getAppBytesWritten());
}

#ifdef FOLLY_HAVE_MSG_ERRQUEUE
struct AsyncSocketErrMessageCallbackTestParams {
  BackendType backendType{BackendType::DEFAULT};
  folly::Optional<int> resetCallbackAfter;
  folly::Optional<int> closeSocketAfter;
  int gotTimestampExpected{0};
  int gotByteSeqExpected{0};
};

class AsyncSocketErrMessageCallbackTest
    : public ::testing::TestWithParam<AsyncSocketErrMessageCallbackTestParams> {
 public:
  static std::vector<AsyncSocketErrMessageCallbackTestParams>
  getTestingValues() {
    std::vector<AsyncSocketErrMessageCallbackTestParams> vals;
    // each socket err message triggers two socket callbacks:
    //   (1) timestamp callback
    //   (2) byteseq callback

    // reset callback cases
    // resetting the callback should prevent any further callbacks
    {
      AsyncSocketErrMessageCallbackTestParams params;
      params.resetCallbackAfter = 1;
      params.gotTimestampExpected = 1;
      params.gotByteSeqExpected = 0;
      vals.push_back(params);
    }
    {
      AsyncSocketErrMessageCallbackTestParams params;
      params.resetCallbackAfter = 2;
      params.gotTimestampExpected = 1;
      params.gotByteSeqExpected = 1;
      vals.push_back(params);
    }
    {
      AsyncSocketErrMessageCallbackTestParams params;
      params.resetCallbackAfter = 3;
      params.gotTimestampExpected = 2;
      params.gotByteSeqExpected = 1;
      vals.push_back(params);
    }
    {
      AsyncSocketErrMessageCallbackTestParams params;
      params.resetCallbackAfter = 4;
      params.gotTimestampExpected = 2;
      params.gotByteSeqExpected = 2;
      vals.push_back(params);
    }

    // close socket cases
    // closing the socket will prevent callbacks after the current err message
    // callbacks (both timestamp and byteseq) are completed
    {
      AsyncSocketErrMessageCallbackTestParams params;
      params.closeSocketAfter = 1;
      params.gotTimestampExpected = 1;
      params.gotByteSeqExpected = 1;
      vals.push_back(params);
    }
    {
      AsyncSocketErrMessageCallbackTestParams params;
      params.closeSocketAfter = 2;
      params.gotTimestampExpected = 1;
      params.gotByteSeqExpected = 1;
      vals.push_back(params);
    }
    {
      AsyncSocketErrMessageCallbackTestParams params;
      params.closeSocketAfter = 3;
      params.gotTimestampExpected = 2;
      params.gotByteSeqExpected = 2;
      vals.push_back(params);
    }
    {
      AsyncSocketErrMessageCallbackTestParams params;
      params.closeSocketAfter = 4;
      params.gotTimestampExpected = 2;
      params.gotByteSeqExpected = 2;
      vals.push_back(params);
    }
    return vals;
  }
};

INSTANTIATE_TEST_SUITE_P(
    ErrMessageTests,
    AsyncSocketErrMessageCallbackTest,
    ::testing::ValuesIn(AsyncSocketErrMessageCallbackTest::getTestingValues()));

class TestErrMessageCallback : public folly::AsyncSocket::ErrMessageCallback {
 public:
  TestErrMessageCallback()
      : exception_(folly::AsyncSocketException::UNKNOWN, "none") {}

  void errMessage(const cmsghdr& cmsg) noexcept override {
    if (cmsg.cmsg_level == SOL_SOCKET && cmsg.cmsg_type == SCM_TIMESTAMPING) {
      gotTimestamp_++;
      checkResetCallback();
      checkCloseSocket();
    } else if (
        (cmsg.cmsg_level == SOL_IP && cmsg.cmsg_type == IP_RECVERR) ||
        (cmsg.cmsg_level == SOL_IPV6 && cmsg.cmsg_type == IPV6_RECVERR)) {
      gotByteSeq_++;
      checkResetCallback();
      checkCloseSocket();
    }
  }

  void errMessageError(
      const folly::AsyncSocketException& ex) noexcept override {
    exception_ = ex;
  }

  void checkResetCallback() noexcept {
    if (socket_ != nullptr && resetCallbackAfter_ != -1 &&
        gotTimestamp_ + gotByteSeq_ == resetCallbackAfter_) {
      socket_->setErrMessageCB(nullptr);
    }
  }

  void checkCloseSocket() noexcept {
    if (socket_ != nullptr && closeSocketAfter_ != -1 &&
        gotTimestamp_ + gotByteSeq_ == closeSocketAfter_) {
      socket_->close();
    }
  }

  folly::AsyncSocket* socket_{nullptr};
  folly::AsyncSocketException exception_;
  int gotTimestamp_{0};
  int gotByteSeq_{0};
  int resetCallbackAfter_{-1};
  int closeSocketAfter_{-1};
};

TEST_P(AsyncSocketErrMessageCallbackTest, ErrMessageCallback) {
  TestServer server;

  // connect()
  EventBase evb;
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);

  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);
  LOG(INFO) << "Client socket fd=" << socket->getNetworkSocket();

  // Let the socket
  evb.loop();

  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);

  // Set read callback to keep the socket subscribed for event
  // notifications. Though we're no planning to read anything from
  // this side of the connection.
  ReadCallback rcb(1);
  socket->setReadCB(&rcb);

  // Set up timestamp callbacks
  TestErrMessageCallback errMsgCB;
  socket->setErrMessageCB(&errMsgCB);
  ASSERT_EQ(
      socket->getErrMessageCallback(),
      static_cast<folly::AsyncSocket::ErrMessageCallback*>(&errMsgCB));

  // set the number of error messages before socket is closed or callback reset
  const auto testParams = GetParam();
  errMsgCB.socket_ = socket.get();
  if (testParams.resetCallbackAfter.has_value()) {
    errMsgCB.resetCallbackAfter_ = testParams.resetCallbackAfter.value();
  }
  if (testParams.closeSocketAfter.has_value()) {
    errMsgCB.closeSocketAfter_ = testParams.closeSocketAfter.value();
  }

  // Enable timestamp notifications
  ASSERT_NE(socket->getNetworkSocket(), NetworkSocket());
  int flags = folly::netops::SOF_TIMESTAMPING_OPT_ID |
      folly::netops::SOF_TIMESTAMPING_OPT_TSONLY |
      folly::netops::SOF_TIMESTAMPING_SOFTWARE |
      folly::netops::SOF_TIMESTAMPING_OPT_CMSG |
      folly::netops::SOF_TIMESTAMPING_TX_SCHED;
  SocketOptionKey tstampingOpt = {SOL_SOCKET, SO_TIMESTAMPING};
  EXPECT_EQ(tstampingOpt.apply(socket->getNetworkSocket(), flags), 0);

  // write()
  std::vector<uint8_t> wbuf(128, 'a');
  WriteCallback wcb;
  // Send two packets to get two EOM notifications
  socket->write(&wcb, wbuf.data(), wbuf.size() / 2);
  socket->write(&wcb, wbuf.data() + wbuf.size() / 2, wbuf.size() / 2);

  // Accept the connection.
  std::shared_ptr<BlockingSocket> acceptedSocket = server.accept();
  LOG(INFO) << "Server socket fd=" << acceptedSocket->getNetworkSocket();

  // Loop
  evb.loopOnce();
  ASSERT_EQ(wcb.state, STATE_SUCCEEDED);

  // Check that we can read the data that was written to the socket
  std::vector<uint8_t> rbuf(wbuf.size(), 0);
  uint32_t bytesRead = acceptedSocket->readAll(rbuf.data(), rbuf.size());
  ASSERT_EQ(bytesRead, wbuf.size());
  ASSERT_TRUE(std::equal(wbuf.begin(), wbuf.end(), rbuf.begin()));

  // Close both sockets
  acceptedSocket->close();
  socket->close();

  ASSERT_TRUE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());

  // Check for the timestamp notifications.
  ASSERT_EQ(
      errMsgCB.exception_.getType(), folly::AsyncSocketException::UNKNOWN);
  ASSERT_EQ(errMsgCB.gotByteSeq_, testParams.gotByteSeqExpected);
  ASSERT_EQ(errMsgCB.gotTimestamp_, testParams.gotTimestampExpected);
}

TEST(
    AsyncSocketErrMessageCallbackTest, SetErrMessageCBFailsWithIoUringBackend) {
  std::unique_ptr<EventBase> evb;
  try {
    evb = makeIoUringEventBase(ioUringOptions);
  } catch (IoUringBackend::NotAvailable const&) {
    GTEST_SKIP() << "IoUringBackend not available";
  }

  auto socket = AsyncSocket::newSocket(evb.get());
  TestErrMessageCallback errMsgCB;
  EXPECT_DEATH(socket->setErrMessageCB(&errMsgCB), ".*");
}

#endif // FOLLY_HAVE_MSG_ERRQUEUE

#if FOLLY_HAVE_SO_TIMESTAMPING

class AsyncSocketByteEventTest : public ::testing::Test {
 protected:
  using MockDispatcher = ::testing::NiceMock<netops::test::MockDispatcher>;
  using TestObserver = MockAsyncSocketLegacyLifecycleObserverForByteEvents;
  using ByteEventType = AsyncSocket::ByteEvent::Type;

  /**
   * Components of a client connection to TestServer.
   *
   * Includes EventBase, client's AsyncSocket, and corresponding server socket.
   */
  class ClientConn {
   public:
    /**
     * Call to sendmsg intercepted and recorded by netops::Dispatcher.
     */
    struct SendmsgInvocation {
      // the iovecs in the msghdr
      std::vector<iovec> iovs;

      // WriteFlags encoded in msg_flags
      WriteFlags writeFlagsInMsgFlags{WriteFlags::NONE};

      // WriteFlags encoded in the msghdr's ancillary data
      WriteFlags writeFlagsInAncillary{WriteFlags::NONE};
    };

    explicit ClientConn(
        std::shared_ptr<TestServer> server,
        std::shared_ptr<AsyncSocket> socket = nullptr,
        std::shared_ptr<BlockingSocket> acceptedSocket = nullptr)
        : server_(std::move(server)),
          socket_(std::move(socket)),
          acceptedSocket_(std::move(acceptedSocket)) {
      if (!socket_) {
        socket_ = AsyncSocket::newSocket(&getEventBase());
      } else {
        setReadCb();
      }
      socket_->setOverrideNetOpsDispatcher(netOpsDispatcher_);
      netOpsDispatcher_->forwardToDefaultImpl();
    }

    void connect() {
      CHECK_NOTNULL(socket_.get());
      CHECK_NOTNULL(socket_->getEventBase());
      socket_->connect(&connCb_, server_->getAddress(), 30);
      socket_->getEventBase()->loop();
      ASSERT_EQ(connCb_.state, STATE_SUCCEEDED);
      setReadCb();

      // accept the socket at the server
      acceptedSocket_ = server_->accept();
    }

    void setReadCb() {
      // Due to how libevent works, we currently need to be subscribed to
      // EV_READ events in order to get error messages.
      //
      // TODO(bschlinker): Resolve this with libevent modification.
      // See https://github.com/libevent/libevent/issues/1038 for details.
      socket_->setReadCB(&readCb_);
    }

    void setMockTcpInfoDispatcher(
        std::shared_ptr<MockTcpInfoDispatcher> mockTcpInfoDispatcher) {
      socket_->setOverrideTcpInfoDispatcher(mockTcpInfoDispatcher);
    }

    std::shared_ptr<NiceMock<TestObserver>> attachObserver(
        bool enableByteEvents, bool enablePrewrite = false) {
      auto observer = AsyncSocketByteEventTest::attachObserver(
          socket_.get(), enableByteEvents, enablePrewrite);
      observers_.push_back(observer);
      return observer;
    }

    /**
     * Write to client socket and read at server.
     */
    void writeAtClientReadAtServer(
        const iovec* iov, const size_t count, const WriteFlags writeFlags) {
      CHECK_NOTNULL(socket_.get());
      CHECK_NOTNULL(socket_->getEventBase());

      // read buffer for server
      std::vector<uint8_t> rbuf(iovsToNumBytes(iov, count), 0);
      uint64_t rbufReadBytes = 0;

      // write to the client socket, incrementally read at the server
      WriteCallback wcb;
      socket_->writev(&wcb, iov, count, writeFlags);
      while (wcb.state == STATE_WAITING) {
        socket_->getEventBase()->loopOnce();
        rbufReadBytes += acceptedSocket_->readNoBlock(
            rbuf.data() + rbufReadBytes, rbuf.size() - rbufReadBytes);
      }
      ASSERT_EQ(wcb.state, STATE_SUCCEEDED);

      // finish reading, then compare
      rbufReadBytes += acceptedSocket_->readAll(
          rbuf.data() + rbufReadBytes, rbuf.size() - rbufReadBytes);
      const auto cBuf = iovsToVector(iov, count);
      ASSERT_EQ(rbufReadBytes, cBuf.size());
      ASSERT_TRUE(std::equal(cBuf.begin(), cBuf.end(), rbuf.begin()));
    }

    /**
     * Write to client socket and read at server.
     */
    void writeAtClientReadAtServer(
        const std::vector<uint8_t>& wbuf, const WriteFlags writeFlags) {
      iovec op;
      op.iov_base = const_cast<void*>(static_cast<const void*>(wbuf.data()));
      op.iov_len = wbuf.size();
      writeAtClientReadAtServer(&op, 1, writeFlags);
    }

    /**
     * Write to client socket, echo at server, and wait for echo at client.
     *
     * Waiting for echo at client ensures that we have given opportunity for
     * timestamps to be generated by the kernel.
     */
    void writeAtClientReadAtServerReflectReadAtClient(
        const iovec* iov, const size_t count, const WriteFlags writeFlags) {
      writeAtClientReadAtServer(iov, count, writeFlags);

      // reflect
      const auto wbuf = iovsToVector(iov, count);
      acceptedSocket_->write(wbuf.data(), wbuf.size());
      while (wbuf.size() != readCb_.dataRead()) {
        socket_->getEventBase()->loopOnce();
      }
      readCb_.verifyData(wbuf.data(), wbuf.size());
      readCb_.clearData();
    }

    /**
     * Write to the client and wait for the client to read.
     */
    void writeAtServerReadAtClient(const iovec* iov, const size_t count) {
      const auto wbuf = iovsToVector(iov, count);
      acceptedSocket_->write(wbuf.data(), wbuf.size());
      while (wbuf.size() != readCb_.dataRead()) {
        socket_->getEventBase()->loopOnce();
      }
      readCb_.verifyData(wbuf.data(), wbuf.size());
      readCb_.clearData();
    }

    /**
     * Write to client socket, echo at server, and wait for echo at client.
     *
     * Waiting for echo at client ensures that we have given opportunity for
     * timestamps to be generated by the kernel.
     */
    void writeAtClientReadAtServerReflectReadAtClient(
        const std::vector<uint8_t>& wbuf, const WriteFlags writeFlags) {
      iovec op = {};
      op.iov_base = const_cast<void*>(static_cast<const void*>(wbuf.data()));
      op.iov_len = wbuf.size();
      writeAtClientReadAtServerReflectReadAtClient(&op, 1, writeFlags);
    }

    /**
     * Write directly to the NetworkSocket, bypassing AsyncSocket.
     */
    void writeAtClientDirectlyToNetworkSocket(
        const std::vector<uint8_t>& wbuf) {
      struct msghdr msg = {};
      struct iovec iovec = {};
      iovec.iov_base = (void*)wbuf.data();
      iovec.iov_len = wbuf.size();

      msg.msg_name = nullptr;
      msg.msg_namelen = 0;
      msg.msg_iov = &iovec;
      msg.msg_iovlen = 1;
      msg.msg_flags = 0;
      msg.msg_controllen = 0;
      msg.msg_control = nullptr;

      auto ret = netops::Dispatcher::getDefaultInstance()->sendmsg(
          socket_->getNetworkSocket(), &msg, 0);
      ASSERT_EQ(ret, wbuf.size());
    }

    std::shared_ptr<AsyncSocket> getRawSocket() { return socket_; }

    std::shared_ptr<BlockingSocket> getAcceptedSocket() {
      return acceptedSocket_;
    }

    EventBase& getEventBase() {
      static EventBase evb; // use same EventBase for all client sockets
      return evb;
    }

    std::shared_ptr<MockDispatcher> getNetOpsDispatcher() const {
      return netOpsDispatcher_;
    }

    /**
     * Get recorded SendmsgInvocations.
     */
    const std::vector<SendmsgInvocation>& getSendmsgInvocations() {
      return sendmsgInvocations_;
    }

    /**
     * Get successful error queue reads.
     */
    int getErrorQueueReads() { return errorQueueReads_; }

    /**
     * Expect a call to setsockopt with optname SO_TIMESTAMPING.
     */
    void netOpsExpectTimestampingSetSockOpt() {
      // must whitelist other calls
      EXPECT_CALL(*netOpsDispatcher_, setsockopt(_, _, _, _, _))
          .Times(AnyNumber());
      EXPECT_CALL(
          *netOpsDispatcher_, setsockopt(_, SOL_SOCKET, SO_TIMESTAMPING, _, _))
          .Times(1);
    }

    /**
     * Expect NO calls to setsockopt with optname SO_TIMESTAMPING.
     */
    void netOpsExpectNoTimestampingSetSockOpt() {
      // must whitelist other calls
      EXPECT_CALL(*netOpsDispatcher_, setsockopt(_, _, _, _, _))
          .Times(AnyNumber());
      EXPECT_CALL(*netOpsDispatcher_, setsockopt(_, _, SO_TIMESTAMPING, _, _))
          .Times(0);
    }

    /**
     * Expect sendmsg to be called with the passed WriteFlags in ancillary data.
     */
    void netOpsExpectSendmsgWithAncillaryTsFlags(WriteFlags writeFlags) {
      auto getMsgAncillaryTsFlags = std::bind(
          (WriteFlags (*)(const struct msghdr* msg)) & ::getMsgAncillaryTsFlags,
          std::placeholders::_1);
      EXPECT_CALL(
          *netOpsDispatcher_,
          sendmsg(_, ResultOf(getMsgAncillaryTsFlags, Eq(writeFlags)), _))
          .WillOnce(DoDefault());
    }

    /**
     * When sendmsg is called, record details and then forward to real sendmsg.
     *
     * This creates a default action.
     */
    void netOpsOnSendmsgRecordIovecsAndFlagsAndFwd() {
      ON_CALL(*netOpsDispatcher_, sendmsg(_, _, _))
          .WillByDefault(
              ::testing::Invoke(
                  [this](NetworkSocket s, const msghdr* message, int flags) {
                    recordSendmsgInvocation(s, message, flags);
                    return netops::Dispatcher::getDefaultInstance()->sendmsg(
                        s, message, flags);
                  }));
    }

    /**
     * When recvmsg is called, forward to real recv message and record details
     * on return.
     *
     * This creates a default action.
     */
    void netOpsOnRecvmsg() {
      ON_CALL(*netOpsDispatcher_, recvmsg(_, _, _))
          .WillByDefault(
              ::testing::Invoke(
                  [this](NetworkSocket s, msghdr* message, int flags) {
                    int ret = netops::Dispatcher::getDefaultInstance()->recvmsg(
                        s, message, flags);
                    recordRecvmsgInvocation(s, message, flags, ret);
                    return ret;
                  }));
    }

    void netOpsVerifyAndClearExpectations() {
      Mock::VerifyAndClearExpectations(netOpsDispatcher_.get());
    }

   private:
    void recordSendmsgInvocation(
        NetworkSocket /* s */, const msghdr* message, int flags) {
      SendmsgInvocation invoc = {};
      invoc.iovs = getMsgIovecs(message);
      invoc.writeFlagsInMsgFlags = msgFlagsToWriteFlags(flags);
      invoc.writeFlagsInAncillary = getMsgAncillaryTsFlags(message);
      sendmsgInvocations_.emplace_back(std::move(invoc));
    }

    void recordRecvmsgInvocation(
        NetworkSocket /* s */,
        msghdr* /* message */,
        int flags,
        int returnValue) {
      if (flags == MSG_ERRQUEUE && returnValue >= 0) {
        errorQueueReads_ += 1;
      }
    }

    // server
    std::shared_ptr<TestServer> server_;

    // managed observers
    std::vector<std::shared_ptr<TestObserver>> observers_;

    // socket components
    ConnCallback connCb_;
    ReadCallback readCb_;
    std::shared_ptr<MockDispatcher> netOpsDispatcher_{
        std::make_shared<MockDispatcher>()};
    std::shared_ptr<AsyncSocket> socket_;

    // accepted socket at server
    std::shared_ptr<BlockingSocket> acceptedSocket_;

    // sendmsg invocations observed
    std::vector<SendmsgInvocation> sendmsgInvocations_;

    // successful error queue reads observer
    int errorQueueReads_{0};
  };

  ClientConn getClientConn() { return ClientConn(server_); }

  /**
   * Static utility functions.
   */

  static std::shared_ptr<NiceMock<TestObserver>> attachObserver(
      AsyncSocket* socket, bool enableByteEvents, bool enablePrewrite = false) {
    AsyncSocket::LegacyLifecycleObserver::Config config = {};
    config.byteEvents = enableByteEvents;
    config.prewrite = enablePrewrite;
    return std::make_shared<NiceMock<TestObserver>>(socket, config);
  }

  static std::vector<uint8_t> getHundredBytesOfData() {
    return std::vector<uint8_t>(
        kOneHundredCharacterString.begin(), kOneHundredCharacterString.end());
  }

  static std::vector<uint8_t> get10KBOfData() {
    std::vector<uint8_t> vec;
    vec.reserve(kOneHundredCharacterString.size() * 100);
    for (auto i = 0; i < 100; i++) {
      vec.insert(
          vec.end(),
          kOneHundredCharacterString.begin(),
          kOneHundredCharacterString.end());
    }
    CHECK_EQ(10000, vec.size());
    return vec;
  }

  static std::vector<uint8_t> get1000KBOfData() {
    std::vector<uint8_t> vec;
    vec.reserve(kOneHundredCharacterString.size() * 10000);
    for (auto i = 0; i < 10000; i++) {
      vec.insert(
          vec.end(),
          kOneHundredCharacterString.begin(),
          kOneHundredCharacterString.end());
    }
    CHECK_EQ(1000000, vec.size());
    return vec;
  }

  static WriteFlags dropWriteFromFlags(WriteFlags writeFlags) {
    return writeFlags & ~WriteFlags::TIMESTAMP_WRITE;
  }

  static std::vector<iovec> getMsgIovecs(const struct msghdr& msg) {
    std::vector<iovec> iovecs;
    for (size_t i = 0; i < msg.msg_iovlen; i++) {
      iovecs.emplace_back(msg.msg_iov[i]);
    }
    return iovecs;
  }

  static std::vector<iovec> getMsgIovecs(const struct msghdr* msg) {
    return getMsgIovecs(*msg);
  }

  static std::vector<uint8_t> iovsToVector(
      const iovec* iov, const size_t count) {
    std::vector<uint8_t> vec;
    for (size_t i = 0; i < count; i++) {
      if (iov[i].iov_len == 0) {
        continue;
      }
      const auto ptr = reinterpret_cast<uint8_t*>(iov[i].iov_base);
      vec.insert(vec.end(), ptr, ptr + iov[i].iov_len);
    }
    return vec;
  }

  static size_t iovsToNumBytes(const iovec* iov, const size_t count) {
    size_t bytes = 0;
    for (size_t i = 0; i < count; i++) {
      bytes += iov[i].iov_len;
    }
    return bytes;
  }

  std::vector<AsyncSocket::ByteEvent> filterToWriteEvents(
      const std::vector<AsyncSocket::ByteEvent>& input) {
    std::vector<AsyncSocket::ByteEvent> result;
    std::copy_if(
        input.begin(),
        input.end(),
        std::back_inserter(result),
        [](auto& event) {
          return event.type == AsyncSocket::ByteEvent::WRITE;
        });
    return result;
  }

  // server
  std::shared_ptr<TestServer> server_{std::make_shared<TestServer>()};
};

TEST_F(AsyncSocketByteEventTest, MsgFlagsToWriteFlags) {
#ifdef MSG_MORE
  EXPECT_EQ(WriteFlags::CORK, msgFlagsToWriteFlags(MSG_MORE));
#endif // MSG_MORE

#ifdef MSG_EOR
  EXPECT_EQ(WriteFlags::EOR, msgFlagsToWriteFlags(MSG_EOR));
#endif

#ifdef MSG_ZEROCOPY
  EXPECT_EQ(WriteFlags::WRITE_MSG_ZEROCOPY, msgFlagsToWriteFlags(MSG_ZEROCOPY));
#endif

#if defined(MSG_MORE) && defined(MSG_EOR)
  EXPECT_EQ(
      WriteFlags::CORK | WriteFlags::EOR,
      msgFlagsToWriteFlags(MSG_MORE | MSG_EOR));
#endif
}

TEST_F(AsyncSocketByteEventTest, GetMsgAncillaryTsFlags) {
  auto ancillaryDataSize = CMSG_LEN(sizeof(uint32_t));
  auto ancillaryData = reinterpret_cast<char*>(alloca(ancillaryDataSize));

  auto getMsg = [&ancillaryDataSize, &ancillaryData](uint32_t sofFlags) {
    struct msghdr msg = {};
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = nullptr;
    msg.msg_iovlen = 0;
    msg.msg_flags = 0;
    msg.msg_controllen = 0;
    msg.msg_control = nullptr;
    if (sofFlags) {
      msg.msg_controllen = ancillaryDataSize;
      msg.msg_control = ancillaryData;
      struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
      CHECK_NOTNULL(cmsg);
      cmsg->cmsg_level = SOL_SOCKET;
      cmsg->cmsg_type = SO_TIMESTAMPING;
      cmsg->cmsg_len = CMSG_LEN(sizeof(uint32_t));
      memcpy(CMSG_DATA(cmsg), &sofFlags, sizeof(sofFlags));
    }
    return msg;
  };

  // SCHED
  {
    auto msg = getMsg(folly::netops::SOF_TIMESTAMPING_TX_SCHED);
    EXPECT_EQ(WriteFlags::TIMESTAMP_SCHED, getMsgAncillaryTsFlags(msg));
  }

  // TX
  {
    auto msg = getMsg(folly::netops::SOF_TIMESTAMPING_TX_SOFTWARE);
    EXPECT_EQ(WriteFlags::TIMESTAMP_TX, getMsgAncillaryTsFlags(msg));
  }

  // ACK
  {
    auto msg = getMsg(folly::netops::SOF_TIMESTAMPING_TX_ACK);
    EXPECT_EQ(WriteFlags::TIMESTAMP_ACK, getMsgAncillaryTsFlags(msg));
  }

  // SCHED + TX + ACK
  {
    auto msg = getMsg(
        folly::netops::SOF_TIMESTAMPING_TX_SCHED |
        folly::netops::SOF_TIMESTAMPING_TX_SOFTWARE |
        folly::netops::SOF_TIMESTAMPING_TX_ACK);
    EXPECT_EQ(
        WriteFlags::TIMESTAMP_SCHED | WriteFlags::TIMESTAMP_TX |
            WriteFlags::TIMESTAMP_ACK,
        getMsgAncillaryTsFlags(msg));
  }
}

TEST_F(AsyncSocketByteEventTest, ObserverAttachedBeforeConnect) {
  const auto flags = WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
      WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK;
  const std::vector<uint8_t> wbuf(1, 'a');

  auto clientConn = getClientConn();
  auto observer = clientConn.attachObserver(true /* enableByteEvents */);
  clientConn.netOpsExpectTimestampingSetSockOpt();
  clientConn.connect();
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());
  clientConn.netOpsVerifyAndClearExpectations();

  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(dropWriteFromFlags(flags));
  clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn.netOpsVerifyAndClearExpectations();
  EXPECT_THAT(observer->byteEvents, SizeIs(4));
  EXPECT_EQ(0U, observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
  EXPECT_EQ(0U, observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
  EXPECT_EQ(0U, observer->maxOffsetForByteEventReceived(ByteEventType::TX));
  EXPECT_EQ(0U, observer->maxOffsetForByteEventReceived(ByteEventType::ACK));

  // write again to check offsets
  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(dropWriteFromFlags(flags));
  clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn.netOpsVerifyAndClearExpectations();
  EXPECT_THAT(observer->byteEvents, SizeIs(8));
  EXPECT_EQ(1U, observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
  EXPECT_EQ(1U, observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
  EXPECT_EQ(1U, observer->maxOffsetForByteEventReceived(ByteEventType::TX));
  EXPECT_EQ(1U, observer->maxOffsetForByteEventReceived(ByteEventType::ACK));
}

TEST_F(AsyncSocketByteEventTest, ObserverAttachedAfterConnect) {
  const auto flags = WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
      WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK;
  const std::vector<uint8_t> wbuf(1, 'a');

  auto clientConn = getClientConn();
  clientConn.netOpsExpectNoTimestampingSetSockOpt();
  clientConn.connect();
  clientConn.netOpsVerifyAndClearExpectations();

  clientConn.netOpsExpectTimestampingSetSockOpt();
  auto observer = clientConn.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());
  clientConn.netOpsVerifyAndClearExpectations();

  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(dropWriteFromFlags(flags));
  clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn.netOpsVerifyAndClearExpectations();
  EXPECT_THAT(observer->byteEvents, SizeIs(4));
  EXPECT_EQ(0U, observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
  EXPECT_EQ(0U, observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
  EXPECT_EQ(0U, observer->maxOffsetForByteEventReceived(ByteEventType::TX));
  EXPECT_EQ(0U, observer->maxOffsetForByteEventReceived(ByteEventType::ACK));

  // write again to check offsets
  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(dropWriteFromFlags(flags));
  clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn.netOpsVerifyAndClearExpectations();
  EXPECT_THAT(observer->byteEvents, SizeIs(8));
  EXPECT_EQ(1U, observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
  EXPECT_EQ(1U, observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
  EXPECT_EQ(1U, observer->maxOffsetForByteEventReceived(ByteEventType::TX));
  EXPECT_EQ(1U, observer->maxOffsetForByteEventReceived(ByteEventType::ACK));
}

TEST_F(
    AsyncSocketByteEventTest, ObserverAttachedBeforeConnectByteEventsDisabled) {
  const auto flags = WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
      WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK;
  const std::vector<uint8_t> wbuf(1, 'a');

  auto clientConn = getClientConn();
  auto observer = clientConn.attachObserver(false /* enableByteEvents */);
  clientConn.netOpsExpectNoTimestampingSetSockOpt();

  clientConn.connect(); // connect after observer attached
  EXPECT_EQ(0, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());
  clientConn.netOpsVerifyAndClearExpectations();

  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(
      WriteFlags::NONE); // events disabled
  clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  EXPECT_THAT(observer->byteEvents, IsEmpty());
  clientConn.netOpsVerifyAndClearExpectations();

  // now enable ByteEvents with another observer, then write again
  clientConn.netOpsExpectTimestampingSetSockOpt();
  auto observer2 = clientConn.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(0, observer->byteEventsEnabledCalled); // observer 1 doesn't want
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());
  EXPECT_EQ(1, observer2->byteEventsEnabledCalled); // should be set
  EXPECT_EQ(0, observer2->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());
  EXPECT_NE(WriteFlags::NONE, flags);
  EXPECT_NE(WriteFlags::NONE, dropWriteFromFlags(flags));
  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(dropWriteFromFlags(flags));
  clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn.netOpsVerifyAndClearExpectations();

  // expect no ByteEvents for first observer, four for the second
  EXPECT_THAT(observer->byteEvents, IsEmpty());
  EXPECT_THAT(observer2->byteEvents, SizeIs(4));
  EXPECT_EQ(1U, observer2->maxOffsetForByteEventReceived(ByteEventType::WRITE));
  EXPECT_EQ(1U, observer2->maxOffsetForByteEventReceived(ByteEventType::SCHED));
  EXPECT_EQ(1U, observer2->maxOffsetForByteEventReceived(ByteEventType::TX));
  EXPECT_EQ(1U, observer2->maxOffsetForByteEventReceived(ByteEventType::ACK));
}

TEST_F(
    AsyncSocketByteEventTest, ObserverAttachedAfterConnectByteEventsDisabled) {
  const auto flags = WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
      WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK;
  const std::vector<uint8_t> wbuf(1, 'a');

  auto clientConn = getClientConn();
  clientConn.netOpsExpectNoTimestampingSetSockOpt();

  clientConn.connect(); // connect before observer attached

  auto observer = clientConn.attachObserver(false /* enableByteEvents */);
  EXPECT_EQ(0, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());
  clientConn.netOpsVerifyAndClearExpectations();

  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(
      WriteFlags::NONE); // events disabled
  clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  EXPECT_THAT(observer->byteEvents, IsEmpty());
  clientConn.netOpsVerifyAndClearExpectations();

  // now enable ByteEvents with another observer, then write again
  clientConn.netOpsExpectTimestampingSetSockOpt();
  auto observer2 = clientConn.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(0, observer->byteEventsEnabledCalled); // observer 1 doesn't want
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());
  EXPECT_EQ(1, observer2->byteEventsEnabledCalled); // should be set
  EXPECT_EQ(0, observer2->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());
  EXPECT_NE(WriteFlags::NONE, flags);
  EXPECT_NE(WriteFlags::NONE, dropWriteFromFlags(flags));
  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(dropWriteFromFlags(flags));
  clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn.netOpsVerifyAndClearExpectations();

  // expect no ByteEvents for first observer, four for the second
  EXPECT_THAT(observer->byteEvents, IsEmpty());
  EXPECT_THAT(observer2->byteEvents, SizeIs(4));
  EXPECT_EQ(1U, observer2->maxOffsetForByteEventReceived(ByteEventType::WRITE));
  EXPECT_EQ(1U, observer2->maxOffsetForByteEventReceived(ByteEventType::SCHED));
  EXPECT_EQ(1U, observer2->maxOffsetForByteEventReceived(ByteEventType::TX));
  EXPECT_EQ(1U, observer2->maxOffsetForByteEventReceived(ByteEventType::ACK));
}

TEST_F(AsyncSocketByteEventTest, ObserverAttachedAfterWrite) {
  const auto flags = WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
      WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK;
  const std::vector<uint8_t> wbuf(1, 'a');

  auto clientConn = getClientConn();
  clientConn.netOpsExpectNoTimestampingSetSockOpt();
  clientConn.connect(); // connect before observer attached
  clientConn.netOpsVerifyAndClearExpectations();

  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(
      WriteFlags::NONE); // events disabled
  clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn.netOpsVerifyAndClearExpectations();

  clientConn.netOpsExpectTimestampingSetSockOpt();
  auto observer = clientConn.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());
  clientConn.netOpsVerifyAndClearExpectations();

  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(dropWriteFromFlags(flags));
  clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn.netOpsVerifyAndClearExpectations();

  EXPECT_THAT(observer->byteEvents, SizeIs(4));
  EXPECT_EQ(1U, observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
  EXPECT_EQ(1U, observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
  EXPECT_EQ(1U, observer->maxOffsetForByteEventReceived(ByteEventType::TX));
  EXPECT_EQ(1U, observer->maxOffsetForByteEventReceived(ByteEventType::ACK));
}

TEST_F(AsyncSocketByteEventTest, ObserverAttachedAfterClose) {
  auto clientConn = getClientConn();
  clientConn.connect();
  clientConn.getRawSocket()->close();
  EXPECT_TRUE(clientConn.getRawSocket()->isClosedBySelf());

  auto observer = clientConn.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(0, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());
}

TEST_F(AsyncSocketByteEventTest, MultipleObserverAttached) {
  const auto flags = WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
      WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK;
  const std::vector<uint8_t> wbuf(50, 'a');

  // attach observer 1 before connect
  auto clientConn = getClientConn();
  auto observer = clientConn.attachObserver(true /* enableByteEvents */);
  clientConn.netOpsExpectTimestampingSetSockOpt();
  clientConn.connect();
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());
  clientConn.netOpsVerifyAndClearExpectations();

  // attach observer 2 after connect
  auto observer2 = clientConn.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(1, observer2->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer2->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  // write
  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(dropWriteFromFlags(flags));
  clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn.netOpsVerifyAndClearExpectations();

  // check observer1
  EXPECT_THAT(observer->byteEvents, SizeIs(4));
  EXPECT_EQ(49U, observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
  EXPECT_EQ(49U, observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
  EXPECT_EQ(49U, observer->maxOffsetForByteEventReceived(ByteEventType::TX));
  EXPECT_EQ(49U, observer->maxOffsetForByteEventReceived(ByteEventType::ACK));

  // check observer2
  EXPECT_THAT(observer2->byteEvents, SizeIs(4));
  EXPECT_EQ(
      49U, observer2->maxOffsetForByteEventReceived(ByteEventType::WRITE));
  EXPECT_EQ(
      49U, observer2->maxOffsetForByteEventReceived(ByteEventType::SCHED));
  EXPECT_EQ(49U, observer2->maxOffsetForByteEventReceived(ByteEventType::TX));
  EXPECT_EQ(49U, observer2->maxOffsetForByteEventReceived(ByteEventType::ACK));
}

/**
 * Test when kernel offset (uint32_t) wraps around.
 */
TEST_F(AsyncSocketByteEventTest, KernelOffsetWrap) {
  auto clientConn = getClientConn();
  clientConn.connect();
  clientConn.netOpsExpectTimestampingSetSockOpt();
  auto observer = clientConn.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());
  clientConn.netOpsVerifyAndClearExpectations();

  const uint64_t wbufSize = 3000000;
  const std::vector<uint8_t> wbuf(wbufSize, 'a');

  // part 1: write close to the wrap point with no ByteEvents to speed things up
  const uint64_t bytesToWritePt1 =
      static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) -
      (wbufSize * 5);
  while (clientConn.getRawSocket()->getRawBytesWritten() < bytesToWritePt1) {
    clientConn.writeAtClientReadAtServer(
        wbuf, WriteFlags::NONE); // no reflect needed
  }

  // part 2: write over the wrap point with ByteEvents
  const auto flags = WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
      WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK;
  const uint64_t bytesToWritePt2 =
      static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) +
      (wbufSize * 5);
  while (clientConn.getRawSocket()->getRawBytesWritten() < bytesToWritePt2) {
    clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(
        dropWriteFromFlags(flags));
    clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
    clientConn.netOpsVerifyAndClearExpectations();
    const uint64_t expectedOffset =
        clientConn.getRawSocket()->getRawBytesWritten() - 1;
    EXPECT_EQ(
        expectedOffset,
        observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
    EXPECT_EQ(
        expectedOffset,
        observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
    EXPECT_EQ(
        expectedOffset,
        observer->maxOffsetForByteEventReceived(ByteEventType::TX));
    EXPECT_EQ(
        expectedOffset,
        observer->maxOffsetForByteEventReceived(ByteEventType::ACK));
  }

  // part 3: one more write outside of a loop with extra checks
  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(dropWriteFromFlags(flags));
  clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn.netOpsVerifyAndClearExpectations();
  const auto expectedOffset =
      clientConn.getRawSocket()->getRawBytesWritten() - 1;
  EXPECT_LT(std::numeric_limits<uint32_t>::max(), expectedOffset);
  EXPECT_EQ(
      expectedOffset,
      observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
  EXPECT_EQ(
      expectedOffset,
      observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
  EXPECT_EQ(
      expectedOffset,
      observer->maxOffsetForByteEventReceived(ByteEventType::TX));
  EXPECT_EQ(
      expectedOffset,
      observer->maxOffsetForByteEventReceived(ByteEventType::ACK));
}

/**
 * Ensure that ErrMessageCallback still works when ByteEvents enabled.
 */
TEST_F(AsyncSocketByteEventTest, ErrMessageCallbackStillTriggered) {
  auto clientConn = getClientConn();
  clientConn.connect();
  clientConn.netOpsExpectTimestampingSetSockOpt();
  auto observer = clientConn.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());
  clientConn.netOpsVerifyAndClearExpectations();

  TestErrMessageCallback errMsgCB;
  clientConn.getRawSocket()->setErrMessageCB(&errMsgCB);

  const auto flags = WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
      WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK;

  std::vector<uint8_t> wbuf(1, 'a');
  EXPECT_NE(WriteFlags::NONE, flags);
  EXPECT_NE(WriteFlags::NONE, dropWriteFromFlags(flags));
  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(dropWriteFromFlags(flags));
  clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn.netOpsVerifyAndClearExpectations();

  // observer should get events
  EXPECT_THAT(observer->byteEvents, SizeIs(4));
  EXPECT_EQ(0U, observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
  EXPECT_EQ(0U, observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
  EXPECT_EQ(0U, observer->maxOffsetForByteEventReceived(ByteEventType::TX));
  EXPECT_EQ(0U, observer->maxOffsetForByteEventReceived(ByteEventType::ACK));

  // err message callbach should get events, too
  EXPECT_EQ(3, errMsgCB.gotByteSeq_);
  EXPECT_EQ(3, errMsgCB.gotTimestamp_);

  // write again, more events for both
  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(dropWriteFromFlags(flags));
  clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn.netOpsVerifyAndClearExpectations();
  EXPECT_THAT(observer->byteEvents, SizeIs(8));
  EXPECT_EQ(1U, observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
  EXPECT_EQ(1U, observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
  EXPECT_EQ(1U, observer->maxOffsetForByteEventReceived(ByteEventType::TX));
  EXPECT_EQ(1U, observer->maxOffsetForByteEventReceived(ByteEventType::ACK));
  EXPECT_EQ(6, errMsgCB.gotByteSeq_);
  EXPECT_EQ(6, errMsgCB.gotTimestamp_);
}

/**
 * Ensure that ByteEvents disabled for unix sockets (not supported).
 */
TEST_F(AsyncSocketByteEventTest, FailUnixSocket) {
  std::shared_ptr<NiceMock<TestObserver>> observer;
  auto netOpsDispatcher = std::make_shared<MockDispatcher>();

  NetworkSocket fd[2];
  EXPECT_EQ(netops::socketpair(AF_UNIX, SOCK_STREAM, 0, fd), 0);
  ASSERT_NE(fd[0], NetworkSocket());
  ASSERT_NE(fd[1], NetworkSocket());
  SCOPE_EXIT {
    netops::close(fd[1]);
  };

  EXPECT_EQ(netops::set_socket_non_blocking(fd[0]), 0);
  EXPECT_EQ(netops::set_socket_non_blocking(fd[1]), 0);

  auto clientSocketRaw = AsyncSocket::newSocket(nullptr, fd[0]);
  auto clientBlockingSocket = BlockingSocket(std::move(clientSocketRaw));
  clientBlockingSocket.getSocket()->setOverrideNetOpsDispatcher(
      netOpsDispatcher);

  // make sure no SO_TIMESTAMPING setsockopt on observer attach
  EXPECT_CALL(*netOpsDispatcher, setsockopt(_, _, _, _, _)).Times(AnyNumber());
  EXPECT_CALL(
      *netOpsDispatcher, setsockopt(_, SOL_SOCKET, SO_TIMESTAMPING, _, _))
      .Times(0); // no calls
  observer = attachObserver(
      clientBlockingSocket.getSocket(), true /* enableByteEvents */);
  EXPECT_EQ(0, observer->byteEventsEnabledCalled);
  EXPECT_EQ(1, observer->byteEventsUnavailableCalled);
  EXPECT_TRUE(observer->byteEventsUnavailableCalledEx.has_value());
  Mock::VerifyAndClearExpectations(netOpsDispatcher.get());

  // do a write, we should see it has no timestamp flags
  const std::vector<uint8_t> wbuf(1, 'a');
  EXPECT_CALL(*netOpsDispatcher, sendmsg(_, _, _))
      .WillOnce(WithArgs<1>(Invoke([](const msghdr* message) {
        EXPECT_EQ(WriteFlags::NONE, getMsgAncillaryTsFlags(*message));
        return 1;
      })));
  clientBlockingSocket.write(
      wbuf.data(),
      wbuf.size(),
      WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
          WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK);
  Mock::VerifyAndClearExpectations(netOpsDispatcher.get());
}

/**
 * If socket timestamps already enabled, do not enable ByteEvents.
 */
TEST_F(AsyncSocketByteEventTest, FailTimestampsAlreadyEnabled) {
  auto clientConn = getClientConn();
  clientConn.connect();

  // enable timestamps via setsockopt
  const uint32_t flags = folly::netops::SOF_TIMESTAMPING_OPT_ID |
      folly::netops::SOF_TIMESTAMPING_OPT_TSONLY |
      folly::netops::SOF_TIMESTAMPING_SOFTWARE |
      folly::netops::SOF_TIMESTAMPING_RAW_HARDWARE |
      folly::netops::SOF_TIMESTAMPING_OPT_TX_SWHW;
  const auto ret = clientConn.getRawSocket()->setSockOpt(
      SOL_SOCKET, SO_TIMESTAMPING, &flags);
  EXPECT_EQ(0, ret);

  clientConn.netOpsExpectNoTimestampingSetSockOpt();
  auto observer = clientConn.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(0, observer->byteEventsEnabledCalled);
  EXPECT_EQ(1, observer->byteEventsUnavailableCalled); // fail
  EXPECT_TRUE(observer->byteEventsUnavailableCalledEx.has_value());
  clientConn.netOpsVerifyAndClearExpectations();

  std::vector<uint8_t> wbuf(1, 'a');
  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(WriteFlags::NONE);
  clientConn.writeAtClientReadAtServerReflectReadAtClient(
      wbuf,
      WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
          WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK);
  clientConn.netOpsVerifyAndClearExpectations();
  EXPECT_THAT(observer->byteEvents, IsEmpty());
}

/**
 * Verify that ByteEvent information is properly copied during socket moves.
 */

TEST_F(AsyncSocketByteEventTest, MoveByteEventsEnabled) {
  const auto flags = WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
      WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK;
  const std::vector<uint8_t> wbuf(50, 'a');

  auto clientConn = getClientConn();
  clientConn.connect();

  // observer with ByteEvents enabled
  auto observer = clientConn.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  // move the socket immediately and add an observer with ByteEvents enabled
  auto clientConn2 = ClientConn(
      server_,
      AsyncSocket::UniquePtr(new AsyncSocket(clientConn.getRawSocket().get())),
      clientConn.getAcceptedSocket());
  auto observer2 = clientConn2.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(1, observer2->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer2->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  // write following move, make sure the offsets are correct
  clientConn2.netOpsExpectSendmsgWithAncillaryTsFlags(
      dropWriteFromFlags(flags));
  clientConn2.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn2.netOpsVerifyAndClearExpectations();
  EXPECT_THAT(observer2->byteEvents, SizeIs(Ge(4)));
  {
    const auto expectedOffset = 49U;
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::WRITE));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::SCHED));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::TX));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::ACK));
  }

  // write again
  clientConn2.netOpsExpectSendmsgWithAncillaryTsFlags(
      dropWriteFromFlags(flags));
  clientConn2.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn2.netOpsVerifyAndClearExpectations();
  EXPECT_THAT(observer2->byteEvents, SizeIs(Ge(8)));
  {
    const auto expectedOffset = 99U;
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::WRITE));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::SCHED));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::TX));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::ACK));
  }
}

TEST_F(AsyncSocketByteEventTest, WriteThenDetachThenEnableByteEvents) {
  const auto flags = WriteFlags::TIMESTAMP_SCHED | WriteFlags::TIMESTAMP_TX |
      WriteFlags::TIMESTAMP_ACK;
  const std::vector<uint8_t> wbuf(20, 'a');
  auto clientConn = getClientConn();
  clientConn.connect();

  // observer with ByteEvents enabled
  auto observer = clientConn.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  // write
  EXPECT_CALL(*clientConn.getNetOpsDispatcher(), recvmsg(_, _, _)).Times(0);
  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(dropWriteFromFlags(flags));
  iovec op = {};
  op.iov_base = const_cast<void*>(static_cast<const void*>(wbuf.data()));
  op.iov_len = wbuf.size();
  clientConn.writeAtClientReadAtServer(&op, 1, flags);

  // now detach the fd and create a new AsyncSocket with the same fd and add an
  // observer with ByteEvents enabled
  auto fd = clientConn.getRawSocket().get()->detachNetworkSocket();
  auto clientConn2 = ClientConn(
      server_,
      AsyncSocket::UniquePtr(new AsyncSocket(&clientConn.getEventBase(), fd)),
      clientConn.getAcceptedSocket());
  clientConn2.netOpsOnRecvmsg();
  // initialize socket family from underlying network socket
  clientConn2.getRawSocket()->cacheAddresses();

  // byte events should not be enabled because the fd already has timestamping
  // enabled (from when it was controlled by clientConn)
  auto observer2 = clientConn2.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(0, observer2->byteEventsEnabledCalled);
  EXPECT_EQ(1, observer2->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  // now have the server reflect the data previously written by the client and
  // check that the client is able to read the data
  clientConn2.writeAtServerReadAtClient(&op, 1);
  // now that we've read everything reflected by the server, loop once more to
  // allow the error queue to be read
  if (clientConn2.getErrorQueueReads() != 3) {
    clientConn2.getEventBase().loopOnce();
  }
  // we should read three timestamping (SCHED, TX, ACK) messages from the
  // error queue
  EXPECT_EQ(clientConn2.getErrorQueueReads(), 3);
}

TEST_F(AsyncSocketByteEventTest, WriteThenDetachThenDoNotEnableByteEvents) {
  const auto flags = WriteFlags::TIMESTAMP_SCHED | WriteFlags::TIMESTAMP_TX |
      WriteFlags::TIMESTAMP_ACK;
  const std::vector<uint8_t> wbuf(20, 'a');
  auto clientConn = getClientConn();
  clientConn.connect();

  // observer with ByteEvents enabled
  auto observer = clientConn.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  // write
  EXPECT_CALL(*clientConn.getNetOpsDispatcher(), recvmsg(_, _, _)).Times(0);
  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(dropWriteFromFlags(flags));
  iovec op = {};
  op.iov_base = const_cast<void*>(static_cast<const void*>(wbuf.data()));
  op.iov_len = wbuf.size();
  clientConn.writeAtClientReadAtServer(&op, 1, flags);

  // now detach the fd and create a new AsyncSocket with the same fd
  // do not enable byte events on the new socket
  auto fd = clientConn.getRawSocket().get()->detachNetworkSocket();
  auto clientConn2 = ClientConn(
      server_,
      AsyncSocket::UniquePtr(new AsyncSocket(&clientConn.getEventBase(), fd)),
      clientConn.getAcceptedSocket());
  clientConn2.netOpsOnRecvmsg();
  // initialize socket family from underlying network socket
  clientConn2.getRawSocket()->cacheAddresses();

  // now have the server reflect the data previously written by the client and
  // check that the client is able to read the data
  clientConn2.writeAtServerReadAtClient(&op, 1);
  // now that we've read everything reflected by the server, loop once more to
  // allow the error queue to be read
  if (clientConn2.getErrorQueueReads() != 3) {
    clientConn2.getEventBase().loopOnce();
  }
  // we should read three timestamping (SCHED, TX, ACK) messages from the error
  // queue
  EXPECT_EQ(clientConn2.getErrorQueueReads(), 3);
}

TEST_F(AsyncSocketByteEventTest, WriteThenMoveByteEventsEnabled) {
  const auto flags = WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
      WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK;
  const std::vector<uint8_t> wbuf(50, 'a');

  auto clientConn = getClientConn();
  clientConn.connect();

  // observer with ByteEvents enabled
  auto observer = clientConn.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  // write
  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(dropWriteFromFlags(flags));
  clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn.netOpsVerifyAndClearExpectations();
  EXPECT_THAT(observer->byteEvents, SizeIs(Ge(4)));
  {
    const auto expectedOffset = 49U;
    EXPECT_EQ(
        expectedOffset,
        observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
    EXPECT_EQ(
        expectedOffset,
        observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
    EXPECT_EQ(
        expectedOffset,
        observer->maxOffsetForByteEventReceived(ByteEventType::TX));
    EXPECT_EQ(
        expectedOffset,
        observer->maxOffsetForByteEventReceived(ByteEventType::ACK));
  }

  // now move the socket and add an observer with ByteEvents enabled
  auto clientConn2 = ClientConn(
      server_,
      AsyncSocket::UniquePtr(
          new AsyncSocket(std::move(clientConn.getRawSocket().get()))),
      clientConn.getAcceptedSocket());
  auto observer2 = clientConn2.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(1, observer2->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer2->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  // write following move, make sure the offsets are correct
  clientConn2.netOpsExpectSendmsgWithAncillaryTsFlags(
      dropWriteFromFlags(flags));
  clientConn2.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn2.netOpsVerifyAndClearExpectations();
  EXPECT_THAT(observer2->byteEvents, SizeIs(Ge(4)));
  {
    const auto expectedOffset = 99U;
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::WRITE));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::SCHED));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::TX));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::ACK));
  }

  // write again
  clientConn2.netOpsExpectSendmsgWithAncillaryTsFlags(
      dropWriteFromFlags(flags));
  clientConn2.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn2.netOpsVerifyAndClearExpectations();
  EXPECT_THAT(observer2->byteEvents, SizeIs(Ge(8)));
  {
    const auto expectedOffset = 149U;
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::WRITE));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::SCHED));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::TX));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::ACK));
  }
}

TEST_F(AsyncSocketByteEventTest, MoveThenEnableByteEvents) {
  const auto flags = WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
      WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK;
  const std::vector<uint8_t> wbuf(50, 'a');

  auto clientConn = getClientConn();
  clientConn.connect();

  // observer with ByteEvents disabled
  auto observer = clientConn.attachObserver(false /* enableByteEvents */);
  EXPECT_EQ(0, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  // move the socket immediately and add an observer with ByteEvents enabled
  auto clientConn2 = ClientConn(
      server_,
      AsyncSocket::UniquePtr(new AsyncSocket(clientConn.getRawSocket().get())),
      clientConn.getAcceptedSocket());
  auto observer2 = clientConn2.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(1, observer2->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer2->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  // write following move, make sure the offsets are correct
  clientConn2.netOpsExpectSendmsgWithAncillaryTsFlags(
      dropWriteFromFlags(flags));
  clientConn2.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn2.netOpsVerifyAndClearExpectations();
  EXPECT_THAT(observer2->byteEvents, SizeIs(Ge(4)));
  {
    const auto expectedOffset = 49U;
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::WRITE));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::SCHED));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::TX));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::ACK));
  }

  // write again
  clientConn2.netOpsExpectSendmsgWithAncillaryTsFlags(
      dropWriteFromFlags(flags));
  clientConn2.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn2.netOpsVerifyAndClearExpectations();
  EXPECT_THAT(observer2->byteEvents, SizeIs(Ge(8)));
  {
    const auto expectedOffset = 99U;
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::WRITE));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::SCHED));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::TX));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::ACK));
  }
}

TEST_F(AsyncSocketByteEventTest, WriteThenMoveThenEnableByteEvents) {
  const auto flags = WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
      WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK;
  const std::vector<uint8_t> wbuf(50, 'a');

  auto clientConn = getClientConn();
  clientConn.connect();

  // observer with ByteEvents disabled
  auto observer = clientConn.attachObserver(false /* enableByteEvents */);
  EXPECT_EQ(0, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  // write, ByteEvents disabled
  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(
      WriteFlags::NONE); // events diabled
  clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn.netOpsVerifyAndClearExpectations();

  // now move the socket and add an observer with ByteEvents enabled
  auto clientConn2 = ClientConn(
      server_,
      AsyncSocket::UniquePtr(new AsyncSocket(clientConn.getRawSocket().get())),
      clientConn.getAcceptedSocket());
  auto observer2 = clientConn2.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(1, observer2->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer2->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  // write following move, make sure the offsets are correct
  clientConn2.netOpsExpectSendmsgWithAncillaryTsFlags(
      dropWriteFromFlags(flags));
  clientConn2.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn2.netOpsVerifyAndClearExpectations();
  EXPECT_THAT(observer2->byteEvents, SizeIs(Ge(4)));
  {
    const auto expectedOffset = 99U;
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::WRITE));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::SCHED));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::TX));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::ACK));
  }

  // write again
  clientConn2.netOpsExpectSendmsgWithAncillaryTsFlags(
      dropWriteFromFlags(flags));
  clientConn2.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn2.netOpsVerifyAndClearExpectations();
  EXPECT_THAT(observer2->byteEvents, SizeIs(Ge(8)));
  {
    const auto expectedOffset = 149U;
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::WRITE));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::SCHED));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::TX));
    EXPECT_EQ(
        expectedOffset,
        observer2->maxOffsetForByteEventReceived(ByteEventType::ACK));
  }
}

TEST_F(AsyncSocketByteEventTest, NoObserverMoveThenEnableByteEvents) {
  const auto flags = WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
      WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK;
  const std::vector<uint8_t> wbuf(50, 'a');

  auto clientConn = getClientConn();
  clientConn.connect();

  // no observer

  // move the socket immediately and add an observer with ByteEvents enabled
  auto clientConn2 = ClientConn(
      server_,
      AsyncSocket::UniquePtr(new AsyncSocket(clientConn.getRawSocket().get())),
      clientConn.getAcceptedSocket());
  auto observer = clientConn2.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  // write following move, make sure the offsets are correct
  clientConn2.netOpsExpectSendmsgWithAncillaryTsFlags(
      dropWriteFromFlags(flags));
  clientConn2.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn2.netOpsVerifyAndClearExpectations();
  EXPECT_THAT(observer->byteEvents, SizeIs(Ge(4)));
  {
    const auto expectedOffset = 49U;
    EXPECT_EQ(
        expectedOffset,
        observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
    EXPECT_EQ(
        expectedOffset,
        observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
    EXPECT_EQ(
        expectedOffset,
        observer->maxOffsetForByteEventReceived(ByteEventType::TX));
    EXPECT_EQ(
        expectedOffset,
        observer->maxOffsetForByteEventReceived(ByteEventType::ACK));
  }

  // write again
  clientConn2.netOpsExpectSendmsgWithAncillaryTsFlags(
      dropWriteFromFlags(flags));
  clientConn2.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn2.netOpsVerifyAndClearExpectations();
  EXPECT_THAT(observer->byteEvents, SizeIs(Ge(8)));
  {
    const auto expectedOffset = 99U;
    EXPECT_EQ(
        expectedOffset,
        observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
    EXPECT_EQ(
        expectedOffset,
        observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
    EXPECT_EQ(
        expectedOffset,
        observer->maxOffsetForByteEventReceived(ByteEventType::TX));
    EXPECT_EQ(
        expectedOffset,
        observer->maxOffsetForByteEventReceived(ByteEventType::ACK));
  }
}

/**
 * Inspect ByteEvent fields, including xTimestampRequested in WRITE events.
 *
 * See CheckByteEventDetailsRawBytesWrittenAndTriedToWrite and
 * AsyncSocketByteEventDetailsTest::CheckByteEventDetails as well.
 */
TEST_F(AsyncSocketByteEventTest, CheckByteEventDetails) {
  const auto flags = WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
      WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK;
  const std::vector<uint8_t> wbuf(1, 'a');

  auto clientConn = getClientConn();
  clientConn.connect();
  auto observer = clientConn.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  EXPECT_NE(WriteFlags::NONE, dropWriteFromFlags(flags));
  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(dropWriteFromFlags(flags));
  clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
  clientConn.netOpsVerifyAndClearExpectations();
  EXPECT_THAT(observer->byteEvents, SizeIs(Eq(4)));
  const auto expectedOffset = wbuf.size() - 1;

  // check WRITE
  {
    auto maybeByteEvent = observer->getByteEventReceivedWithOffset(
        expectedOffset, ByteEventType::WRITE);
    ASSERT_TRUE(maybeByteEvent.has_value());
    auto& byteEvent = maybeByteEvent.value();

    EXPECT_EQ(ByteEventType::WRITE, byteEvent.type);
    EXPECT_EQ(expectedOffset, byteEvent.offset);
    EXPECT_GE(std::chrono::steady_clock::now(), byteEvent.ts);
    EXPECT_LT(
        std::chrono::steady_clock::now() - std::chrono::seconds(60),
        byteEvent.ts);

    EXPECT_EQ(flags, byteEvent.maybeWriteFlags);
    EXPECT_TRUE(byteEvent.schedTimestampRequestedOnWrite());
    EXPECT_TRUE(byteEvent.txTimestampRequestedOnWrite());
    EXPECT_TRUE(byteEvent.ackTimestampRequestedOnWrite());

    EXPECT_FALSE(byteEvent.maybeSoftwareTs.has_value());
    EXPECT_FALSE(byteEvent.maybeHardwareTs.has_value());

    // maybeRawBytesWritten and maybeRawBytesTriedToWrite are tested in
    // CheckByteEventDetailsRawBytesWrittenAndTriedToWrite
  }

  // check SCHED, TX, ACK
  for (const auto& byteEventType :
       {ByteEventType::SCHED, ByteEventType::TX, ByteEventType::ACK}) {
    auto maybeByteEvent =
        observer->getByteEventReceivedWithOffset(expectedOffset, byteEventType);
    ASSERT_TRUE(maybeByteEvent.has_value());
    auto& byteEvent = maybeByteEvent.value();

    EXPECT_EQ(byteEventType, byteEvent.type);
    EXPECT_EQ(expectedOffset, byteEvent.offset);
    EXPECT_GE(std::chrono::steady_clock::now(), byteEvent.ts);
    EXPECT_LT(
        std::chrono::steady_clock::now() - std::chrono::seconds(60),
        byteEvent.ts);

    EXPECT_FALSE(byteEvent.maybeWriteFlags.has_value());
    EXPECT_DEATH((void)byteEvent.schedTimestampRequestedOnWrite(), ".*");
    EXPECT_DEATH((void)byteEvent.txTimestampRequestedOnWrite(), ".*");
    EXPECT_DEATH((void)byteEvent.ackTimestampRequestedOnWrite(), ".*");

    EXPECT_TRUE(byteEvent.maybeSoftwareTs.has_value());
    EXPECT_FALSE(byteEvent.maybeHardwareTs.has_value());
  }
}

/**
 * Inspect ByteEvent fields maybeRawBytesWritten and maybeRawBytesTriedToWrite.
 */
TEST_F(
    AsyncSocketByteEventTest,
    CheckByteEventDetailsRawBytesWrittenAndTriedToWrite) {
  auto clientConn = getClientConn();
  clientConn.connect();
  auto observer = clientConn.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  struct ExpectedSendmsgInvocation {
    size_t expectedTotalIovLen{0};
    ssize_t returnVal{0}; // number of bytes written or error val
    folly::Optional<size_t> maybeWriteEventExpectedOffset{};
    folly::Optional<WriteFlags> maybeWriteEventExpectedFlags{};
  };

  const auto flags = WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK |
      WriteFlags::TIMESTAMP_SCHED | WriteFlags::TIMESTAMP_WRITE;

  // first write
  //
  // no splits triggered by observer
  //
  // sendmsg will incrementally accept the bytes so we can test the values of
  // maybeRawBytesWritten and maybeRawBytesTriedToWrite
  {
    // bytes written per sendmsg call: 20, 10, 50, -1 (EAGAIN), 11, 99
    const std::vector<ExpectedSendmsgInvocation> expectedSendmsgInvocations{
        // {
        //    expectedTotalIovLen, returnVal,
        //    maybeWriteEventExpectedOffset, maybeWriteEventExpectedFlags
        // },
        {100, 20, 19, flags},
        {80, 10, 29, flags},
        {70, 50, 79, flags},
        {20, -1, folly::none, flags},
        {20, 11, 90, flags},
        {9, 9, 99, flags}};

    // sendmsg will be called, we return # of bytes written
    {
      InSequence s;
      for (const auto& expectedInvocation : expectedSendmsgInvocations) {
        EXPECT_CALL(
            *(clientConn.getNetOpsDispatcher()),
            sendmsg(
                _,
                Pointee(SendmsgMsghdrHasTotalIovLen(
                    expectedInvocation.expectedTotalIovLen)),
                _))
            .WillOnce(::testing::InvokeWithoutArgs([expectedInvocation]() {
              if (expectedInvocation.returnVal < 0) {
                errno = EAGAIN; // returning error, set EAGAIN
              }
              return expectedInvocation.returnVal;
            }));
      }
    }

    // write
    // writes will be intercepted, so we don't need to read at other end
    WriteCallback wcb;
    clientConn.getRawSocket()->write(
        &wcb,
        kOneHundredCharacterVec.data(),
        kOneHundredCharacterVec.size(),
        flags);
    while (STATE_WAITING == wcb.state) {
      clientConn.getRawSocket()->getEventBase()->loopOnce();
    }
    ASSERT_EQ(STATE_SUCCEEDED, wcb.state);

    // check write events
    for (const auto& expectedInvocation : expectedSendmsgInvocations) {
      if (expectedInvocation.returnVal < 0) {
        // should be no WriteEvent since the return value was an error
        continue;
      }

      ASSERT_TRUE(expectedInvocation.maybeWriteEventExpectedOffset.has_value());
      const auto& expectedOffset =
          *expectedInvocation.maybeWriteEventExpectedOffset;

      auto maybeByteEvent = observer->getByteEventReceivedWithOffset(
          expectedOffset, ByteEventType::WRITE);
      ASSERT_TRUE(maybeByteEvent.has_value());
      auto& byteEvent = maybeByteEvent.value();

      EXPECT_EQ(ByteEventType::WRITE, byteEvent.type);
      EXPECT_EQ(expectedOffset, byteEvent.offset);
      EXPECT_GE(std::chrono::steady_clock::now(), byteEvent.ts);
      EXPECT_LT(
          std::chrono::steady_clock::now() - std::chrono::seconds(60),
          byteEvent.ts);

      EXPECT_EQ(
          expectedInvocation.maybeWriteEventExpectedFlags,
          byteEvent.maybeWriteFlags);
      EXPECT_TRUE(byteEvent.schedTimestampRequestedOnWrite());
      EXPECT_TRUE(byteEvent.txTimestampRequestedOnWrite());
      EXPECT_TRUE(byteEvent.ackTimestampRequestedOnWrite());

      EXPECT_FALSE(byteEvent.maybeSoftwareTs.has_value());
      EXPECT_FALSE(byteEvent.maybeHardwareTs.has_value());

      // what we really want to test
      EXPECT_EQ(
          folly::to_unsigned(expectedInvocation.returnVal),
          byteEvent.maybeRawBytesWritten);
      EXPECT_EQ(
          expectedInvocation.expectedTotalIovLen,
          byteEvent.maybeRawBytesTriedToWrite);
    }
  }

  // everything should have occurred by now
  clientConn.netOpsVerifyAndClearExpectations();

  // second write
  //
  // sendmsg will incrementally accept the bytes so we can test the values of
  // maybeRawBytesWritten and maybeRawBytesTriedToWrite
  {
    // bytes written per sendmsg call: 20, 30, 50
    const std::vector<ExpectedSendmsgInvocation> expectedSendmsgInvocations{
        {100, 20, 119, flags}, {80, 30, 149, flags}, {50, 50, 199, flags}};

    // sendmsg will be called, we return # of bytes written
    {
      InSequence s;
      for (const auto& expectedInvocation : expectedSendmsgInvocations) {
        EXPECT_CALL(
            *(clientConn.getNetOpsDispatcher()),
            sendmsg(
                _,
                Pointee(SendmsgMsghdrHasTotalIovLen(
                    expectedInvocation.expectedTotalIovLen)),
                _))
            .WillOnce(::testing::InvokeWithoutArgs([expectedInvocation]() {
              return expectedInvocation.returnVal;
            }));
      }
    }

    // write
    // writes will be intercepted, so we don't need to read at other end
    WriteCallback wcb;
    clientConn.getRawSocket()->write(
        &wcb,
        kOneHundredCharacterVec.data(),
        kOneHundredCharacterVec.size(),
        flags);
    while (STATE_WAITING == wcb.state) {
      clientConn.getRawSocket()->getEventBase()->loopOnce();
    }
    ASSERT_EQ(STATE_SUCCEEDED, wcb.state);

    // check write events
    for (const auto& expectedInvocation : expectedSendmsgInvocations) {
      ASSERT_TRUE(expectedInvocation.maybeWriteEventExpectedOffset.has_value());
      const auto& expectedOffset =
          *expectedInvocation.maybeWriteEventExpectedOffset;

      auto maybeByteEvent = observer->getByteEventReceivedWithOffset(
          expectedOffset, ByteEventType::WRITE);
      ASSERT_TRUE(maybeByteEvent.has_value());
      auto& byteEvent = maybeByteEvent.value();

      EXPECT_EQ(ByteEventType::WRITE, byteEvent.type);
      EXPECT_EQ(expectedOffset, byteEvent.offset);
      EXPECT_GE(std::chrono::steady_clock::now(), byteEvent.ts);
      EXPECT_LT(
          std::chrono::steady_clock::now() - std::chrono::seconds(60),
          byteEvent.ts);

      EXPECT_EQ(
          expectedInvocation.maybeWriteEventExpectedFlags,
          byteEvent.maybeWriteFlags);
      EXPECT_TRUE(byteEvent.schedTimestampRequestedOnWrite());
      EXPECT_TRUE(byteEvent.txTimestampRequestedOnWrite());
      EXPECT_TRUE(byteEvent.ackTimestampRequestedOnWrite());

      EXPECT_FALSE(byteEvent.maybeSoftwareTs.has_value());
      EXPECT_FALSE(byteEvent.maybeHardwareTs.has_value());

      // what we really want to test
      EXPECT_EQ(
          folly::to_unsigned(expectedInvocation.returnVal),
          byteEvent.maybeRawBytesWritten);
      EXPECT_EQ(
          expectedInvocation.expectedTotalIovLen,
          byteEvent.maybeRawBytesTriedToWrite);
    }
  }
}

TEST_F(AsyncSocketByteEventTest, SplitIoVecArraySingleIoVec) {
  // get srciov from lambda to enable us to keep it const during test
  const char* buf = kOneHundredCharacterString.c_str();
  auto getSrcIov = [&buf]() {
    std::vector<struct iovec> srcIov(2);
    srcIov[0].iov_base = const_cast<void*>(static_cast<const void*>(buf));
    srcIov[0].iov_len = kOneHundredCharacterString.size();
    return srcIov;
  };

  std::vector<struct iovec> srcIov = getSrcIov();
  const auto data = srcIov.data();

  // split 0 -> 0 (first byte)
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        0, 0, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(1, dstIovCount);
    EXPECT_EQ(1, dstIov[0].iov_len);
    EXPECT_EQ(srcIov[0].iov_base, dstIov[0].iov_base);
    EXPECT_EQ(buf, dstIov[0].iov_base);
  }

  // split 0 -> 49 (50th byte)
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        0, 49, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(1, dstIovCount);
    EXPECT_EQ(srcIov[0].iov_base, dstIov[0].iov_base);
    EXPECT_EQ(50, dstIov[0].iov_len);
  }

  // split 0 -> 98 (penultimate byte)
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        0, 98, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(1, dstIovCount);
    EXPECT_EQ(srcIov[0].iov_base, dstIov[0].iov_base);
    EXPECT_EQ(99, dstIov[0].iov_len);
  }

  // split 0 -> 99 (pointless split)
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        0, 99, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(1, dstIovCount);
    EXPECT_EQ(srcIov[0].iov_base, dstIov[0].iov_base);
    EXPECT_EQ(srcIov[0].iov_len, dstIov[0].iov_len);
  }
}

TEST_F(AsyncSocketByteEventTest, SplitIoVecArrayMultiIoVecInvalid) {
  // get srciov from lambda to enable us to keep it const during test
  const char* buf = kOneHundredCharacterString.c_str();
  auto getSrcIov = [&buf]() {
    std::vector<struct iovec> srcIov(4);
    srcIov[0].iov_base = const_cast<void*>(static_cast<const void*>(buf));
    srcIov[0].iov_len = 50;
    srcIov[1].iov_base = const_cast<void*>(static_cast<const void*>(buf + 50));
    srcIov[1].iov_len = 50;
    return srcIov;
  };

  std::vector<struct iovec> srcIov = getSrcIov();
  const auto data = srcIov.data();

  // dstIov.size() < srcIov.size(); this is not allowed
  std::vector<struct iovec> dstIov(1);
  size_t dstIovCount = dstIov.size();
  EXPECT_LT(dstIovCount, srcIov.size());
  EXPECT_DEATH(
      AsyncSocket::splitIovecArray(
          0, 0, data, srcIov.size(), dstIov.data(), dstIovCount),
      ".*");
}

TEST_F(AsyncSocketByteEventTest, SplitIoVecArrayMultiIoVec) {
  // get srciov from lambda to enable us to keep it const during test
  const char* buf = kOneHundredCharacterString.c_str();
  auto getSrcIov = [&buf]() {
    std::vector<struct iovec> srcIov(4);
    srcIov[0].iov_base = const_cast<void*>(static_cast<const void*>(buf));
    srcIov[0].iov_len = 25;
    srcIov[1].iov_base = const_cast<void*>(static_cast<const void*>(buf + 25));
    srcIov[1].iov_len = 25;
    srcIov[2].iov_base = const_cast<void*>(static_cast<const void*>(buf + 50));
    srcIov[2].iov_len = 25;
    srcIov[3].iov_base = const_cast<void*>(static_cast<const void*>(buf + 75));
    srcIov[3].iov_len = 25;
    return srcIov;
  };

  std::vector<struct iovec> srcIov = getSrcIov();
  const auto data = srcIov.data();

  // split 0 -> 0 (first byte)
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        0, 0, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(1, dstIovCount);
    EXPECT_EQ(1, dstIov[0].iov_len);
    EXPECT_EQ(srcIov[0].iov_base, dstIov[0].iov_base);
    EXPECT_EQ(buf, dstIov[0].iov_base);
  }

  // split 0 -> 98 (penultimate byte)
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        0, 98, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(4, dstIovCount);
    EXPECT_EQ(srcIov[0].iov_base, dstIov[0].iov_base);
    EXPECT_EQ(srcIov[0].iov_len, dstIov[0].iov_len);
    EXPECT_EQ(srcIov[1].iov_base, dstIov[1].iov_base);
    EXPECT_EQ(srcIov[1].iov_len, dstIov[1].iov_len);
    EXPECT_EQ(srcIov[2].iov_base, dstIov[2].iov_base);
    EXPECT_EQ(srcIov[2].iov_len, dstIov[2].iov_len);

    // last iovec is different
    EXPECT_EQ(24, dstIov[3].iov_len);
    EXPECT_EQ(srcIov[3].iov_base, dstIov[3].iov_base);
  }

  // split 0 -> 99 (pointless split)
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        0, 99, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(4, dstIovCount);
    EXPECT_EQ(srcIov[0].iov_base, dstIov[0].iov_base);
    EXPECT_EQ(srcIov[0].iov_len, dstIov[0].iov_len);
    EXPECT_EQ(srcIov[1].iov_base, dstIov[1].iov_base);
    EXPECT_EQ(srcIov[1].iov_len, dstIov[1].iov_len);
    EXPECT_EQ(srcIov[2].iov_base, dstIov[2].iov_base);
    EXPECT_EQ(srcIov[2].iov_len, dstIov[2].iov_len);
    EXPECT_EQ(srcIov[3].iov_base, dstIov[3].iov_base);
    EXPECT_EQ(srcIov[3].iov_len, dstIov[3].iov_len);
  }

  //
  // test when endOffset is near a iovec boundary
  //

  // split 0 -> 49 (50th byte)
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        0, 49, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(2, dstIovCount);
    EXPECT_EQ(srcIov[0].iov_base, dstIov[0].iov_base);
    EXPECT_EQ(srcIov[0].iov_len, dstIov[0].iov_len);
    EXPECT_EQ(srcIov[1].iov_base, dstIov[1].iov_base);
    EXPECT_EQ(srcIov[1].iov_len, dstIov[1].iov_len);
  }

  // split 0 -> 50 (51st byte)
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        0, 50, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(3, dstIovCount);
    EXPECT_EQ(srcIov[0].iov_base, dstIov[0].iov_base);
    EXPECT_EQ(srcIov[0].iov_len, dstIov[0].iov_len);
    EXPECT_EQ(srcIov[1].iov_base, dstIov[1].iov_base);
    EXPECT_EQ(srcIov[1].iov_len, dstIov[1].iov_len);

    // last iovec is one byte
    EXPECT_EQ(1, dstIov[2].iov_len);
    EXPECT_EQ(srcIov[2].iov_base, dstIov[2].iov_base);
  }

  // split 0 -> 51 (52nd byte)
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        0, 51, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(3, dstIovCount);
    EXPECT_EQ(srcIov[0].iov_base, dstIov[0].iov_base);
    EXPECT_EQ(srcIov[0].iov_len, dstIov[0].iov_len);
    EXPECT_EQ(srcIov[1].iov_base, dstIov[1].iov_base);
    EXPECT_EQ(srcIov[1].iov_len, dstIov[1].iov_len);

    // last iovec is two bytes
    EXPECT_EQ(2, dstIov[2].iov_len);
    EXPECT_EQ(srcIov[2].iov_base, dstIov[2].iov_base);
  }

  //
  // test when startOffset is near a iovec boundary
  //

  // split 49 -> 99
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        49, 99, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(3, dstIovCount);

    // first dst iovec is one byte, starts 24 bytes in to the second src iovec
    EXPECT_EQ(1, dstIov[0].iov_len);
    EXPECT_EQ(
        dstIov[0].iov_base,
        const_cast<void*>(static_cast<const void*>(
            reinterpret_cast<uint8_t*>(srcIov[1].iov_base) + 24)));

    // second dst iovec is third src iovec
    // third dst iovec is fourth src iovec
    EXPECT_EQ(dstIov[1].iov_base, srcIov[2].iov_base);
    EXPECT_EQ(dstIov[1].iov_len, srcIov[2].iov_len);
    EXPECT_EQ(dstIov[2].iov_base, srcIov[3].iov_base);
    EXPECT_EQ(dstIov[2].iov_len, srcIov[3].iov_len);
  }

  // split 50 -> 99
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        50, 99, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(2, dstIovCount);

    // first dst iovec is third src iovec
    // second dst iovec is fourth src iovec
    EXPECT_EQ(dstIov[0].iov_base, srcIov[2].iov_base);
    EXPECT_EQ(dstIov[0].iov_len, srcIov[2].iov_len);
    EXPECT_EQ(dstIov[1].iov_base, srcIov[3].iov_base);
    EXPECT_EQ(dstIov[1].iov_len, srcIov[3].iov_len);
  }

  // split 51 -> 99
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        51, 99, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(2, dstIovCount);

    // first dst iovec is 24 bytes, starts 1 byte in to the third src iovec
    EXPECT_EQ(24, dstIov[0].iov_len);
    EXPECT_EQ(
        dstIov[0].iov_base,
        const_cast<void*>(static_cast<const void*>(
            reinterpret_cast<uint8_t*>(srcIov[2].iov_base) + 1)));

    // second dst iovec is fourth src iovec
    EXPECT_EQ(dstIov[1].iov_base, srcIov[3].iov_base);
    EXPECT_EQ(dstIov[1].iov_len, srcIov[3].iov_len);
  }

  //
  // test when startOffset and endOffset are near iovec boundaries
  //

  // split 49 -> 49
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        49, 49, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(1, dstIovCount);

    // first dst iovec is one byte, starts 24 bytes in to the second src iovec
    EXPECT_EQ(1, dstIov[0].iov_len);
    EXPECT_EQ(
        dstIov[0].iov_base,
        const_cast<void*>(static_cast<const void*>(
            reinterpret_cast<uint8_t*>(srcIov[1].iov_base) + 24)));
  }

  // split 49 -> 50
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        49, 50, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(2, dstIovCount);

    // first dst iovec is one byte, starts 24 bytes in to the second src iovec
    EXPECT_EQ(1, dstIov[0].iov_len);
    EXPECT_EQ(
        dstIov[0].iov_base,
        const_cast<void*>(static_cast<const void*>(
            reinterpret_cast<uint8_t*>(srcIov[1].iov_base) + 24)));

    // second iovec is one byte, starts at the third src iovec
    EXPECT_EQ(1, dstIov[1].iov_len);
    EXPECT_EQ(dstIov[1].iov_base, srcIov[2].iov_base);
  }

  // split 49 -> 51
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        49, 51, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(2, dstIovCount);

    // first dst iovec is one byte, starts 24 bytes in to the second src iovec
    EXPECT_EQ(1, dstIov[0].iov_len);
    EXPECT_EQ(
        dstIov[0].iov_base,
        const_cast<void*>(static_cast<const void*>(
            reinterpret_cast<uint8_t*>(srcIov[1].iov_base) + 24)));

    // second iovec is two bytes, starts at the third src iovec
    EXPECT_EQ(2, dstIov[1].iov_len);
    EXPECT_EQ(dstIov[1].iov_base, srcIov[2].iov_base);
  }

  // split 50 -> 50
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        50, 50, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(1, dstIovCount);

    // first dst iovec is one byte, starts at the third src iovec
    EXPECT_EQ(1, dstIov[0].iov_len);
    EXPECT_EQ(dstIov[0].iov_base, srcIov[2].iov_base);
  }

  // split 50 -> 51
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        50, 51, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(1, dstIovCount);

    // first dst iovec is two bytes, starts at the third src iovec
    EXPECT_EQ(2, dstIov[0].iov_len);
    EXPECT_EQ(dstIov[0].iov_base, srcIov[2].iov_base);
  }

  // split 51 -> 51
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        51, 51, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(1, dstIovCount);

    // first dst iovec is one byte, starts 1 byte into the third src iovec
    EXPECT_EQ(1, dstIov[0].iov_len);
    EXPECT_EQ(
        dstIov[0].iov_base,
        const_cast<void*>(static_cast<const void*>(
            reinterpret_cast<uint8_t*>(srcIov[2].iov_base) + 1)));
  }

  // split 48 -> 98
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        48, 98, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(3, dstIovCount);

    // first dst iovec is two bytes, starts 23 bytes in to the second src iovec
    EXPECT_EQ(2, dstIov[0].iov_len);
    EXPECT_EQ(
        dstIov[0].iov_base,
        const_cast<void*>(static_cast<const void*>(
            reinterpret_cast<uint8_t*>(srcIov[1].iov_base) + 23)));

    // second dst iovec is third src iovec
    EXPECT_EQ(dstIov[1].iov_base, srcIov[2].iov_base);
    EXPECT_EQ(dstIov[1].iov_len, srcIov[2].iov_len);

    // third dst iovec is 24 bytes, starts at the fourth src iovec
    EXPECT_EQ(24, dstIov[2].iov_len);
    EXPECT_EQ(dstIov[2].iov_base, srcIov[3].iov_base);
  }

  // split 49 -> 98
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        49, 98, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(3, dstIovCount);

    // first dst iovec is one byte, starts 24 bytes in to the second src iovec
    EXPECT_EQ(1, dstIov[0].iov_len);
    EXPECT_EQ(
        dstIov[0].iov_base,
        const_cast<void*>(static_cast<const void*>(
            reinterpret_cast<uint8_t*>(srcIov[1].iov_base) + 24)));

    // second dst iovec is third src iovec
    EXPECT_EQ(dstIov[1].iov_base, srcIov[2].iov_base);
    EXPECT_EQ(dstIov[1].iov_len, srcIov[2].iov_len);

    // third dst iovec is 24 bytes, starts at the fourth src iovec
    EXPECT_EQ(24, dstIov[2].iov_len);
    EXPECT_EQ(dstIov[2].iov_base, srcIov[3].iov_base);
  }

  // split 50 -> 98
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        50, 98, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(2, dstIovCount);

    // first dst iovec is third src iovec
    EXPECT_EQ(dstIov[0].iov_base, srcIov[2].iov_base);
    EXPECT_EQ(dstIov[0].iov_len, srcIov[2].iov_len);

    // second dst iovec is 24 bytes, starts at the fourth src iovec
    EXPECT_EQ(24, dstIov[1].iov_len);
    EXPECT_EQ(dstIov[1].iov_base, srcIov[3].iov_base);
  }

  // split 51 -> 98
  {
    std::vector<struct iovec> dstIov(4);
    size_t dstIovCount = dstIov.size();
    AsyncSocket::splitIovecArray(
        51, 98, data, srcIov.size(), dstIov.data(), dstIovCount);

    ASSERT_EQ(2, dstIovCount);

    // first dst iovec is 24 bytes, starts 1 byte in to the third src iovec
    EXPECT_EQ(24, dstIov[0].iov_len);
    EXPECT_EQ(
        dstIov[0].iov_base,
        const_cast<void*>(static_cast<const void*>(
            reinterpret_cast<uint8_t*>(srcIov[2].iov_base) + 1)));

    // second dst iovec is 24 bytes, starts at the fourth src iovec
    EXPECT_EQ(24, dstIov[1].iov_len);
    EXPECT_EQ(dstIov[1].iov_base, srcIov[3].iov_base);
  }
}

TEST_F(AsyncSocketByteEventTest, SendmsgMatchers) {
  // empty
  {
    const ClientConn::SendmsgInvocation sendmsgInvoc = {};
    // length
    EXPECT_THAT(sendmsgInvoc, SendmsgInvocHasTotalIovLen(size_t(0)));

    // iov first byte
    EXPECT_THAT(
        sendmsgInvoc,
        Not(SendmsgInvocHasIovFirstByte(kOneHundredCharacterVec.data())));
    EXPECT_THAT(
        sendmsgInvoc,
        Not(SendmsgInvocHasIovFirstByte(kOneHundredCharacterVec.data() + 5)));

    // iov last byte
    EXPECT_THAT(
        sendmsgInvoc,
        Not(SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data())));
    EXPECT_THAT(
        sendmsgInvoc,
        Not(SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 5)));
  }

  // single iov, last byte = end of kOneHundredCharacterVec
  {
    struct iovec iov = {};
    iov.iov_base = const_cast<void*>(
        static_cast<const void*>((kOneHundredCharacterVec.data())));
    iov.iov_len = kOneHundredCharacterVec.size();
    const ClientConn::SendmsgInvocation sendmsgInvoc = {.iovs = {iov}};

    struct msghdr msg = {};
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = const_cast<struct iovec*>(sendmsgInvoc.iovs.data());
    msg.msg_iovlen = sendmsgInvoc.iovs.size();

    // length
    EXPECT_THAT(sendmsgInvoc, SendmsgInvocHasTotalIovLen(size_t(100)));
    EXPECT_THAT(msg, SendmsgMsghdrHasTotalIovLen(size_t(100)));

    // iov first byte
    EXPECT_THAT(
        sendmsgInvoc,
        SendmsgInvocHasIovFirstByte(kOneHundredCharacterVec.data()));
    EXPECT_THAT(
        sendmsgInvoc,
        Not(SendmsgInvocHasIovFirstByte(
            kOneHundredCharacterVec.data() + kOneHundredCharacterVec.size() -
            1)));

    // iov last byte
    EXPECT_THAT(
        sendmsgInvoc,
        Not(SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data())));
    EXPECT_THAT(
        sendmsgInvoc,
        SendmsgInvocHasIovLastByte(
            kOneHundredCharacterVec.data() + kOneHundredCharacterVec.size() -
            1));
  }

  // single iov, first and last byte = start of kOneHundredCharacterVec
  {
    struct iovec iov = {};
    iov.iov_base = const_cast<void*>(
        static_cast<const void*>((kOneHundredCharacterVec.data())));
    iov.iov_len = 1;
    const ClientConn::SendmsgInvocation sendmsgInvoc = {.iovs = {iov}};

    struct msghdr msg = {};
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = const_cast<struct iovec*>(sendmsgInvoc.iovs.data());
    msg.msg_iovlen = sendmsgInvoc.iovs.size();

    // length
    EXPECT_THAT(sendmsgInvoc, SendmsgInvocHasTotalIovLen(size_t(1)));
    EXPECT_THAT(msg, SendmsgMsghdrHasTotalIovLen(size_t(1)));

    // iov first byte
    EXPECT_THAT(
        sendmsgInvoc,
        SendmsgInvocHasIovFirstByte(kOneHundredCharacterVec.data()));
    EXPECT_THAT(
        sendmsgInvoc,
        Not(SendmsgInvocHasIovFirstByte(
            kOneHundredCharacterVec.data() + kOneHundredCharacterVec.size() -
            1)));

    // iov last byte
    EXPECT_THAT(
        sendmsgInvoc,
        SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data()));
    EXPECT_THAT(
        sendmsgInvoc,
        Not(SendmsgInvocHasIovLastByte(
            kOneHundredCharacterVec.data() + kOneHundredCharacterVec.size() -
            1)));
  }

  // single iov, first and last byte = end of kOneHundredCharacterVec
  {
    struct iovec iov = {};
    iov.iov_base = const_cast<void*>(static_cast<const void*>(
        (kOneHundredCharacterVec.data() + kOneHundredCharacterVec.size())));
    iov.iov_len = 1;
    const ClientConn::SendmsgInvocation sendmsgInvoc = {.iovs = {iov}};

    struct msghdr msg = {};
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = const_cast<struct iovec*>(sendmsgInvoc.iovs.data());
    msg.msg_iovlen = sendmsgInvoc.iovs.size();

    // length
    EXPECT_THAT(sendmsgInvoc, SendmsgInvocHasTotalIovLen(size_t(1)));
    EXPECT_THAT(msg, SendmsgMsghdrHasTotalIovLen(size_t(1)));

    // iov first byte
    EXPECT_THAT(
        sendmsgInvoc,
        SendmsgInvocHasIovFirstByte(
            kOneHundredCharacterVec.data() + kOneHundredCharacterVec.size()));

    // iov last byte
    EXPECT_THAT(
        sendmsgInvoc,
        SendmsgInvocHasIovLastByte(
            kOneHundredCharacterVec.data() + kOneHundredCharacterVec.size()));
  }

  // two iov, (0 -> 0, 1 - > 99), last byte = end of kOneHundredCharacterVec
  {
    struct iovec iov1 = {};
    iov1.iov_base = const_cast<void*>(
        static_cast<const void*>((kOneHundredCharacterVec.data())));
    iov1.iov_len = 1;

    struct iovec iov2 = {};
    iov2.iov_base = const_cast<void*>(
        static_cast<const void*>((kOneHundredCharacterVec.data() + 1)));
    iov2.iov_len = 99;

    const ClientConn::SendmsgInvocation sendmsgInvoc = {.iovs = {iov1, iov2}};

    struct msghdr msg = {};
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = const_cast<struct iovec*>(sendmsgInvoc.iovs.data());
    msg.msg_iovlen = sendmsgInvoc.iovs.size();

    // length
    EXPECT_THAT(sendmsgInvoc, SendmsgInvocHasTotalIovLen(size_t(100)));
    EXPECT_THAT(msg, SendmsgMsghdrHasTotalIovLen(size_t(100)));

    // iov first byte
    EXPECT_THAT(
        sendmsgInvoc,
        SendmsgInvocHasIovFirstByte(kOneHundredCharacterVec.data()));

    // iov last byte
    EXPECT_THAT(
        sendmsgInvoc,
        SendmsgInvocHasIovLastByte(
            kOneHundredCharacterVec.data() + kOneHundredCharacterVec.size() -
            1));
  }

  // two iov, (0 -> 49, 50 - > 99), last byte = end of kOneHundredCharacterVec
  {
    struct iovec iov1 = {};
    iov1.iov_base = const_cast<void*>(
        static_cast<const void*>((kOneHundredCharacterVec.data())));
    iov1.iov_len = 50;

    struct iovec iov2 = {};
    iov2.iov_base = const_cast<void*>(
        static_cast<const void*>((kOneHundredCharacterVec.data() + 50)));
    iov2.iov_len = 50;

    const ClientConn::SendmsgInvocation sendmsgInvoc = {.iovs = {iov1, iov2}};

    struct msghdr msg = {};
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = const_cast<struct iovec*>(sendmsgInvoc.iovs.data());
    msg.msg_iovlen = sendmsgInvoc.iovs.size();

    // length
    EXPECT_THAT(sendmsgInvoc, SendmsgInvocHasTotalIovLen(size_t(100)));
    EXPECT_THAT(msg, SendmsgMsghdrHasTotalIovLen(size_t(100)));

    // iov first byte
    EXPECT_THAT(
        sendmsgInvoc,
        SendmsgInvocHasIovFirstByte(kOneHundredCharacterVec.data()));

    // iov last byte
    EXPECT_THAT(
        sendmsgInvoc,
        SendmsgInvocHasIovLastByte(
            kOneHundredCharacterVec.data() + kOneHundredCharacterVec.size() -
            1));
  }

  // two iov, (0 -> 49, 50 - > 98), last byte = penultimate byte
  {
    struct iovec iov1 = {};
    iov1.iov_base = const_cast<void*>(
        static_cast<const void*>((kOneHundredCharacterVec.data())));
    iov1.iov_len = 50;

    struct iovec iov2 = {};
    iov2.iov_base = const_cast<void*>(
        static_cast<const void*>((kOneHundredCharacterVec.data() + 50)));
    iov2.iov_len = 49;

    const ClientConn::SendmsgInvocation sendmsgInvoc = {.iovs = {iov1, iov2}};

    struct msghdr msg = {};
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = const_cast<struct iovec*>(sendmsgInvoc.iovs.data());
    msg.msg_iovlen = sendmsgInvoc.iovs.size();

    // length
    EXPECT_THAT(sendmsgInvoc, SendmsgInvocHasTotalIovLen(size_t(99)));
    EXPECT_THAT(msg, SendmsgMsghdrHasTotalIovLen(size_t(99)));

    // iov first byte
    EXPECT_THAT(
        sendmsgInvoc,
        SendmsgInvocHasIovFirstByte(kOneHundredCharacterVec.data()));

    // iov last byte
    EXPECT_THAT(
        sendmsgInvoc,
        SendmsgInvocHasIovLastByte(
            kOneHundredCharacterVec.data() + kOneHundredCharacterVec.size() -
            2));
  }
}

TEST_F(AsyncSocketByteEventTest, SendmsgInvocMsgFlagsEq) {
  // empty
  {
    const ClientConn::SendmsgInvocation sendmsgInvoc;
    EXPECT_THAT(sendmsgInvoc, SendmsgInvocMsgFlagsEq(WriteFlags::NONE));
    EXPECT_THAT(sendmsgInvoc, Not(SendmsgInvocMsgFlagsEq(WriteFlags::CORK)));
  }

  // flag set
  {
    ClientConn::SendmsgInvocation sendmsgInvoc = {};
    sendmsgInvoc.writeFlagsInMsgFlags = WriteFlags::CORK;
    EXPECT_THAT(sendmsgInvoc, Not(SendmsgInvocMsgFlagsEq(WriteFlags::NONE)));
    EXPECT_THAT(
        sendmsgInvoc,
        Not(SendmsgInvocMsgFlagsEq(
            WriteFlags::EOR | WriteFlags::CORK))); // should be exact match
    EXPECT_THAT(sendmsgInvoc, SendmsgInvocMsgFlagsEq(WriteFlags::CORK));
  }
}

TEST_F(AsyncSocketByteEventTest, SendmsgInvocAncillaryFlagsEq) {
  // empty
  {
    const ClientConn::SendmsgInvocation sendmsgInvoc;
    EXPECT_THAT(sendmsgInvoc, SendmsgInvocAncillaryFlagsEq(WriteFlags::NONE));
    EXPECT_THAT(
        sendmsgInvoc,
        Not(SendmsgInvocAncillaryFlagsEq(WriteFlags::TIMESTAMP_TX)));
  }

  // flag set
  {
    ClientConn::SendmsgInvocation sendmsgInvoc = {};
    sendmsgInvoc.writeFlagsInAncillary = WriteFlags::TIMESTAMP_TX;
    EXPECT_THAT(
        sendmsgInvoc, Not(SendmsgInvocAncillaryFlagsEq(WriteFlags::NONE)));
    EXPECT_THAT(
        sendmsgInvoc,
        Not(SendmsgInvocAncillaryFlagsEq(
            WriteFlags::TIMESTAMP_TX |
            WriteFlags::TIMESTAMP_ACK))); // should be exact match
    EXPECT_THAT(
        sendmsgInvoc, SendmsgInvocAncillaryFlagsEq(WriteFlags::TIMESTAMP_TX));
  }
}

TEST_F(AsyncSocketByteEventTest, ByteEventMatching) {
  // offset = 0, type = WRITE
  {
    AsyncSocket::ByteEvent event = {};
    event.type = ByteEventType::WRITE;
    event.offset = 0;
    EXPECT_THAT(event, ByteEventMatching(ByteEventType::WRITE, 0));

    // not matching
    EXPECT_THAT(event, Not(ByteEventMatching(ByteEventType::WRITE, 10)));
    EXPECT_THAT(event, Not(ByteEventMatching(ByteEventType::TX, 0)));
    EXPECT_THAT(event, Not(ByteEventMatching(ByteEventType::ACK, 0)));
    EXPECT_THAT(event, Not(ByteEventMatching(ByteEventType::SCHED, 0)));
  }

  // offset = 10, type = TX
  {
    AsyncSocket::ByteEvent event = {};
    event.type = ByteEventType::TX;
    event.offset = 10;
    EXPECT_THAT(event, ByteEventMatching(ByteEventType::TX, 10));

    // not matching
    EXPECT_THAT(event, Not(ByteEventMatching(ByteEventType::TX, 0)));
    EXPECT_THAT(event, Not(ByteEventMatching(ByteEventType::WRITE, 10)));
    EXPECT_THAT(event, Not(ByteEventMatching(ByteEventType::ACK, 10)));
    EXPECT_THAT(event, Not(ByteEventMatching(ByteEventType::SCHED, 10)));
  }
}

TEST_F(AsyncSocketByteEventTest, PrewriteSingleObserver) {
  auto clientConn = getClientConn();
  clientConn.connect();
  auto observer = clientConn.attachObserver(
      true /* enableByteEvents */, true /* enablePrewrite */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  clientConn.netOpsOnSendmsgRecordIovecsAndFlagsAndFwd();

  const auto flags = WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK |
      WriteFlags::TIMESTAMP_SCHED | WriteFlags::TIMESTAMP_WRITE;
  ON_CALL(*observer, prewriteMock(_, _, _))
      .WillByDefault(
          testing::Invoke(
              [](AsyncTransport*,
                 const AsyncSocketObserverInterface::PrewriteState& state,
                 AsyncSocketObserverInterface::PrewriteRequestContainer&
                     container) {
                AsyncSocketObserverInterface::PrewriteRequest request;
                if (state.startOffset == 0) {
                  request.maybeOffsetToSplitWrite = 0;
                } else if (state.startOffset <= 50) {
                  request.maybeOffsetToSplitWrite = 50;
                } else if (state.startOffset <= 98) {
                  request.maybeOffsetToSplitWrite = 98;
                }

                request.writeFlagsToAddAtOffset = flags;
                container.addRequest(request);
              }));
  clientConn.writeAtClientReadAtServerReflectReadAtClient(
      kOneHundredCharacterVec, WriteFlags::NONE);

  EXPECT_THAT(
      clientConn.getSendmsgInvocations(),
      ElementsAre(
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data()),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(dropWriteFromFlags(flags))),
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 50),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(dropWriteFromFlags(flags))),
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 98),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(dropWriteFromFlags(flags))),
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 99),
              SendmsgInvocMsgFlagsEq(WriteFlags::NONE),
              SendmsgInvocAncillaryFlagsEq(WriteFlags::NONE))));

  // verify WRITE events exist at the appropriate locations
  // we verify timestamp events are generated elsewhere
  //
  // should _not_ contain events for 99 as no prewrite for that
  EXPECT_THAT(
      filterToWriteEvents(observer->byteEvents),
      ElementsAre(
          ByteEventMatching(ByteEventType::WRITE, 0),
          ByteEventMatching(ByteEventType::WRITE, 50),
          ByteEventMatching(ByteEventType::WRITE, 98)));
}

/**
 * Test explicitly that CORK (MSG_MORE) is set if write is split in middle.
 */
TEST_F(AsyncSocketByteEventTest, PrewriteSingleObserverCorkIfSplitMiddle) {
  auto clientConn = getClientConn();
  clientConn.connect();
  auto observer = clientConn.attachObserver(
      true /* enableByteEvents */, true /* enablePrewrite */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  clientConn.netOpsOnSendmsgRecordIovecsAndFlagsAndFwd();

  const auto flags = WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK |
      WriteFlags::TIMESTAMP_SCHED | WriteFlags::TIMESTAMP_WRITE;
  ON_CALL(*observer, prewriteMock(_, _, _))
      .WillByDefault(
          testing::Invoke(
              [](AsyncTransport*,
                 const AsyncSocketObserverInterface::PrewriteState& state,
                 AsyncSocketObserverInterface::PrewriteRequestContainer&
                     container) {
                AsyncSocketObserverInterface::PrewriteRequest request;
                if (state.startOffset <= 50) {
                  request.maybeOffsetToSplitWrite = 50;
                }
                request.writeFlagsToAddAtOffset = flags;
                container.addRequest(request);
              }));
  clientConn.writeAtClientReadAtServerReflectReadAtClient(
      kOneHundredCharacterVec, WriteFlags::NONE);

  EXPECT_THAT(
      clientConn.getSendmsgInvocations(),
      ElementsAre(
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 50),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(dropWriteFromFlags(flags))),
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 99),
              SendmsgInvocMsgFlagsEq(WriteFlags::NONE),
              SendmsgInvocAncillaryFlagsEq(WriteFlags::NONE))));

  // verify WRITE events exist at the appropriate locations
  // we verify timestamp events are generated elsewhere
  EXPECT_THAT(
      filterToWriteEvents(observer->byteEvents),
      ElementsAre(ByteEventMatching(ByteEventType::WRITE, 50)));
}

/**
 * Test explicitly that CORK (MSG_MORE) is set if write is split in middle.
 */
TEST_F(AsyncSocketByteEventTest, PrewriteSingleObserverNoCorkIfSplitAtEnd) {
  auto clientConn = getClientConn();
  clientConn.connect();
  auto observer = clientConn.attachObserver(
      true /* enableByteEvents */, true /* enablePrewrite */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  clientConn.netOpsOnSendmsgRecordIovecsAndFlagsAndFwd();

  const auto flags = WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK |
      WriteFlags::TIMESTAMP_SCHED | WriteFlags::TIMESTAMP_WRITE;
  ON_CALL(*observer, prewriteMock(_, _, _))
      .WillByDefault(
          testing::Invoke(
              [](AsyncTransport*,
                 const AsyncSocketObserverInterface::PrewriteState& state,
                 AsyncSocketObserverInterface::PrewriteRequestContainer&
                     container) {
                AsyncSocketObserverInterface::PrewriteRequest request;
                if (state.startOffset <= 99) {
                  request.maybeOffsetToSplitWrite = 99;
                }
                request.writeFlagsToAddAtOffset = flags;
                container.addRequest(request);
              }));
  clientConn.writeAtClientReadAtServerReflectReadAtClient(
      kOneHundredCharacterVec, WriteFlags::NONE);

  EXPECT_THAT(
      clientConn.getSendmsgInvocations(),
      ElementsAre(AllOf(
          SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 99),
          SendmsgInvocMsgFlagsEq(WriteFlags::NONE), // no cork!
          SendmsgInvocAncillaryFlagsEq(dropWriteFromFlags(flags)))));

  // verify WRITE events exist at the appropriate locations
  // we verify timestamp events are generated elsewhere
  EXPECT_THAT(
      filterToWriteEvents(observer->byteEvents),
      ElementsAre(ByteEventMatching(ByteEventType::WRITE, 99)));
}

/**
 * Test explicitly that split flags are NOT added if no split.
 */
TEST_F(AsyncSocketByteEventTest, PrewriteSingleObserverNoSplitFlagsIfNoSplit) {
  auto clientConn = getClientConn();
  clientConn.connect();
  auto observer = clientConn.attachObserver(
      true /* enableByteEvents */, true /* enablePrewrite */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  clientConn.netOpsOnSendmsgRecordIovecsAndFlagsAndFwd();

  const auto flags = WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK |
      WriteFlags::TIMESTAMP_SCHED | WriteFlags::TIMESTAMP_WRITE;
  ON_CALL(*observer, prewriteMock(_, _, _))
      .WillByDefault(
          testing::Invoke(
              [](AsyncTransport*,
                 const AsyncSocketObserverInterface::PrewriteState& /* state */,
                 AsyncSocketObserverInterface::PrewriteRequestContainer&
                     container) {
                AsyncSocketObserverInterface::PrewriteRequest request;
                request.writeFlagsToAddAtOffset = flags;
                container.addRequest(request);
              }));
  clientConn.writeAtClientReadAtServerReflectReadAtClient(
      kOneHundredCharacterVec, WriteFlags::NONE);

  EXPECT_THAT(
      clientConn.getSendmsgInvocations(),
      ElementsAre(AllOf(
          SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 99),
          SendmsgInvocMsgFlagsEq(WriteFlags::NONE),
          SendmsgInvocAncillaryFlagsEq(WriteFlags::NONE))));
}

/**
 * Test more combinations of prewrite flags, including writeFlagsToAdd.
 */
TEST_F(AsyncSocketByteEventTest, PrewriteSingleObserverFlagsOnAll) {
  auto clientConn = getClientConn();
  clientConn.connect();
  auto observer = clientConn.attachObserver(
      true /* enableByteEvents */, true /* enablePrewrite */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  clientConn.netOpsOnSendmsgRecordIovecsAndFlagsAndFwd();
  ON_CALL(*observer, prewriteMock(_, _, _))
      .WillByDefault(
          testing::Invoke(
              [](AsyncTransport*,
                 const AsyncSocketObserverInterface::PrewriteState& state,
                 AsyncSocketObserverInterface::PrewriteRequestContainer&
                     container) {
                AsyncSocketObserverInterface::PrewriteRequest request;
                if (state.startOffset == 0) {
                  request.maybeOffsetToSplitWrite = 0;
                  request.writeFlagsToAddAtOffset |=
                      WriteFlags::TIMESTAMP_WRITE;
                } else if (state.startOffset <= 10) {
                  request.maybeOffsetToSplitWrite = 10;
                  request.writeFlagsToAddAtOffset |=
                      WriteFlags::TIMESTAMP_SCHED;
                } else if (state.startOffset <= 20) {
                  request.writeFlagsToAddAtOffset |= WriteFlags::TIMESTAMP_TX;
                  request.maybeOffsetToSplitWrite = 20;
                } else if (state.startOffset <= 30) {
                  request.writeFlagsToAddAtOffset |= WriteFlags::TIMESTAMP_ACK;
                  request.maybeOffsetToSplitWrite = 30;
                } else if (state.startOffset <= 40) {
                  request.writeFlagsToAddAtOffset |= WriteFlags::TIMESTAMP_TX;
                  request.writeFlagsToAdd |= WriteFlags::TIMESTAMP_WRITE;
                  request.maybeOffsetToSplitWrite = 40;
                } else {
                  request.writeFlagsToAdd |= WriteFlags::TIMESTAMP_WRITE;
                }

                container.addRequest(request);
              }));
  clientConn.writeAtClientReadAtServerReflectReadAtClient(
      kOneHundredCharacterVec, WriteFlags::NONE);

  EXPECT_THAT(
      clientConn.getSendmsgInvocations(),
      ElementsAre(
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data()),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(WriteFlags::NONE)),
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 10),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(WriteFlags::TIMESTAMP_SCHED)),
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 20),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(WriteFlags::TIMESTAMP_TX)),
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 30),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(WriteFlags::TIMESTAMP_ACK)),
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 40),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(WriteFlags::TIMESTAMP_TX)),
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 99),
              SendmsgInvocMsgFlagsEq(WriteFlags::NONE),
              SendmsgInvocAncillaryFlagsEq(WriteFlags::NONE))));

  // verify WRITE events exist at the appropriate locations
  // we verify timestamp events are generated elsewhere
  EXPECT_THAT(
      filterToWriteEvents(observer->byteEvents),
      ElementsAre(
          ByteEventMatching(ByteEventType::WRITE, 0),
          ByteEventMatching(ByteEventType::WRITE, 40),
          ByteEventMatching(ByteEventType::WRITE, 99)));
}

/**
 * Test merging of write flags with those passed to AsyncSocket::write().
 */
TEST_F(AsyncSocketByteEventTest, PrewriteSingleObserverFlagsOnWrite) {
  auto clientConn = getClientConn();
  clientConn.connect();
  auto observer = clientConn.attachObserver(
      true /* enableByteEvents */, true /* enablePrewrite */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  clientConn.netOpsOnSendmsgRecordIovecsAndFlagsAndFwd();

  // first byte, observer adds TX and WRITE, onwards, it just adds WRITE
  ON_CALL(*observer, prewriteMock(_, _, _))
      .WillByDefault(
          testing::Invoke(
              [](AsyncTransport*,
                 const AsyncSocketObserverInterface::PrewriteState& state,
                 AsyncSocketObserverInterface::PrewriteRequestContainer&
                     container) {
                AsyncSocketObserverInterface::PrewriteRequest request;
                if (state.startOffset == 0) {
                  request.maybeOffsetToSplitWrite = 0;
                  request.writeFlagsToAddAtOffset |= WriteFlags::TIMESTAMP_TX;
                }
                request.writeFlagsToAdd |= WriteFlags::TIMESTAMP_WRITE;

                container.addRequest(request);
              }));

  // application does a write with ACK and CORK set
  clientConn.writeAtClientReadAtServerReflectReadAtClient(
      kOneHundredCharacterVec, WriteFlags::CORK | WriteFlags::TIMESTAMP_ACK);

  // make sure we have the merge
  //   first write, TX is added
  //   second write, CORK is passed through
  EXPECT_THAT(
      clientConn.getSendmsgInvocations(),
      ElementsAre(
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data()),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK), // set by split
              SendmsgInvocAncillaryFlagsEq(
                  WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK)),
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 99),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK), // still set
              SendmsgInvocAncillaryFlagsEq(
                  dropWriteFromFlags(WriteFlags::TIMESTAMP_ACK)))));

  // verify WRITE events exist at the appropriate locations
  // we verify timestamp events are generated elsewhere
  EXPECT_THAT(
      filterToWriteEvents(observer->byteEvents),
      ElementsAre(
          ByteEventMatching(ByteEventType::WRITE, 0),
          ByteEventMatching(ByteEventType::WRITE, 99)));
}

/**
 * Test invalid offset for prewrite, ensure death via CHECK.
 */
TEST_F(AsyncSocketByteEventTest, PrewriteSingleObserverInvalidOffset) {
  auto clientConn = getClientConn();
  clientConn.connect();
  auto observer = clientConn.attachObserver(
      true /* enableByteEvents */, true /* enablePrewrite */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  clientConn.netOpsOnSendmsgRecordIovecsAndFlagsAndFwd();

  ON_CALL(*observer, prewriteMock(_, _, _))
      .WillByDefault(
          testing::Invoke(
              [](AsyncTransport*,
                 const AsyncSocketObserverInterface::PrewriteState& state,
                 AsyncSocketObserverInterface::PrewriteRequestContainer&
                     container) {
                AsyncSocketObserverInterface::PrewriteRequest request;
                EXPECT_GT(200, state.endOffset);
                request.maybeOffsetToSplitWrite = 200; // invalid
                container.addRequest(request);
              }));

  // check will fail due to invalid offset
  EXPECT_DEATH(
      clientConn.writeAtClientReadAtServerReflectReadAtClient(
          kOneHundredCharacterVec, WriteFlags::NONE),
      ".*");
}

/**
 * Test prewrite with multiple iovec.
 */
TEST_F(AsyncSocketByteEventTest, PrewriteSingleObserverTwoIovec) {
  // two iovec, each with half of the kOneHundredCharacterVec
  std::vector<iovec> iovs;
  {
    iovec iov = {};
    iov.iov_base = const_cast<void*>(
        static_cast<const void*>((kOneHundredCharacterVec.data())));
    iov.iov_len = 50;
    iovs.push_back(iov);
  }
  {
    iovec iov = {};
    iov.iov_base = const_cast<void*>(
        static_cast<const void*>((kOneHundredCharacterVec.data() + 50)));
    iov.iov_len = 50;
    iovs.push_back(iov);
  }

  auto clientConn = getClientConn();
  clientConn.connect();
  auto observer = clientConn.attachObserver(
      true /* enableByteEvents */, true /* enablePrewrite */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  clientConn.netOpsOnSendmsgRecordIovecsAndFlagsAndFwd();

  const auto flags = WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK |
      WriteFlags::TIMESTAMP_SCHED | WriteFlags::TIMESTAMP_WRITE;
  ON_CALL(*observer, prewriteMock(_, _, _))
      .WillByDefault(
          testing::Invoke(
              [](AsyncTransport*,
                 const AsyncSocketObserverInterface::PrewriteState& state,
                 AsyncSocketObserverInterface::PrewriteRequestContainer&
                     container) {
                AsyncSocketObserverInterface::PrewriteRequest request;
                if (state.startOffset == 0) {
                  request.maybeOffsetToSplitWrite = 0;
                } else if (state.startOffset <= 49) {
                  request.maybeOffsetToSplitWrite = 49;
                } else if (state.startOffset <= 99) {
                  request.maybeOffsetToSplitWrite = 99;
                }

                request.writeFlagsToAddAtOffset = flags;
                container.addRequest(request);
              }));

  clientConn.writeAtClientReadAtServerReflectReadAtClient(
      iovs.data(), iovs.size(), WriteFlags::NONE);

  EXPECT_THAT(
      clientConn.getSendmsgInvocations(),
      ElementsAre(
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data()),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(dropWriteFromFlags(flags))),
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 49),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(dropWriteFromFlags(flags))),
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 99),
              SendmsgInvocMsgFlagsEq(WriteFlags::NONE),
              SendmsgInvocAncillaryFlagsEq(dropWriteFromFlags(flags)))));

  // verify WRITE events exist at the appropriate locations
  // we verify timestamp events are generated elsewhere
  EXPECT_THAT(
      filterToWriteEvents(observer->byteEvents),
      ElementsAre(
          ByteEventMatching(ByteEventType::WRITE, 0),
          ByteEventMatching(ByteEventType::WRITE, 49),
          ByteEventMatching(ByteEventType::WRITE, 99)));
}

/**
 * Test prewrite with large number of iovec to trigger malloc codepath.
 */
TEST_F(AsyncSocketByteEventTest, PrewriteSingleObserverManyIovec) {
  // make a long vector, 10000 bytes long
  auto tenThousandByteVec = get10KBOfData();
  ASSERT_THAT(tenThousandByteVec, SizeIs(10000));

  // put each byte in the vector into its own iovec
  std::vector<iovec> tenThousandIovec;
  for (size_t i = 0; i < tenThousandByteVec.size(); i++) {
    iovec iov = {};
    iov.iov_base = tenThousandByteVec.data() + i;
    iov.iov_len = 1;
    tenThousandIovec.push_back(iov);
  }

  auto clientConn = getClientConn();
  clientConn.connect();
  auto observer = clientConn.attachObserver(
      true /* enableByteEvents */, true /* enablePrewrite */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  clientConn.netOpsOnSendmsgRecordIovecsAndFlagsAndFwd();

  const auto flags = WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK |
      WriteFlags::TIMESTAMP_SCHED | WriteFlags::TIMESTAMP_WRITE;
  ON_CALL(*observer, prewriteMock(_, _, _))
      .WillByDefault(
          testing::Invoke(
              [](AsyncTransport*,
                 const AsyncSocketObserverInterface::PrewriteState& state,
                 AsyncSocketObserverInterface::PrewriteRequestContainer&
                     container) {
                AsyncSocketObserverInterface::PrewriteRequest request;
                if (state.startOffset == 0) {
                  request.maybeOffsetToSplitWrite = 0;
                } else if (state.startOffset <= 1000) {
                  request.maybeOffsetToSplitWrite = 1000;
                } else if (state.startOffset <= 5000) {
                  request.maybeOffsetToSplitWrite = 5000;
                }

                request.writeFlagsToAddAtOffset = flags;
                container.addRequest(request);
              }));

  clientConn.writeAtClientReadAtServerReflectReadAtClient(
      tenThousandIovec.data(), tenThousandIovec.size(), WriteFlags::NONE);

  EXPECT_THAT(
      clientConn.getSendmsgInvocations(),
      AllOf(
          Contains(AllOf(
              SendmsgInvocHasIovLastByte(tenThousandByteVec.data()),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(dropWriteFromFlags(flags)))),
          Contains(AllOf(
              SendmsgInvocHasIovLastByte(tenThousandByteVec.data() + 1000),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(dropWriteFromFlags(flags)))),
          Contains(AllOf(
              SendmsgInvocHasIovLastByte(tenThousandByteVec.data() + 5000),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(dropWriteFromFlags(flags)))),
          Contains(AllOf(
              SendmsgInvocHasIovLastByte(tenThousandByteVec.data() + 9999),
              SendmsgInvocMsgFlagsEq(WriteFlags::NONE),
              SendmsgInvocAncillaryFlagsEq(WriteFlags::NONE)))));

  // verify WRITE events exist at the appropriate locations
  // we verify timestamp events are generated elsewhere
  //
  // should _not_ contain events for 99 as no prewrite for that
  EXPECT_THAT(
      filterToWriteEvents(observer->byteEvents),
      AllOf(
          Contains(ByteEventMatching(ByteEventType::WRITE, 0)),
          Contains(ByteEventMatching(ByteEventType::WRITE, 1000)),
          Contains(ByteEventMatching(ByteEventType::WRITE, 5000))));
}

TEST_F(AsyncSocketByteEventTest, PrewriteMultipleObservers) {
  auto clientConn = getClientConn();
  clientConn.connect();

  // five observers
  // observer1 - 4 have byte events and prewrite enabled
  // observer5 has byte events enabled
  // observer6 has neither byte events or prewrite
  auto observer1 = clientConn.attachObserver(
      true /* enableByteEvents */, true /* enablePrewrite */);
  auto observer2 = clientConn.attachObserver(
      true /* enableByteEvents */, true /* enablePrewrite */);
  auto observer3 = clientConn.attachObserver(
      true /* enableByteEvents */, true /* enablePrewrite */);
  auto observer4 = clientConn.attachObserver(
      true /* enableByteEvents */, true /* enablePrewrite */);
  auto observer5 = clientConn.attachObserver(
      true /* enableByteEvents */, false /* enablePrewrite */);
  auto observer6 = clientConn.attachObserver(
      false /* enableByteEvents */, false /* enablePrewrite */);

  clientConn.netOpsOnSendmsgRecordIovecsAndFlagsAndFwd();

  // observer 1 wants TX timestamps at 25, 50, 75
  ON_CALL(*observer1, prewriteMock(_, _, _))
      .WillByDefault(
          testing::Invoke(
              [](AsyncTransport*,
                 const AsyncSocketObserverInterface::PrewriteState& state,
                 AsyncSocketObserverInterface::PrewriteRequestContainer&
                     container) {
                AsyncSocketObserverInterface::PrewriteRequest request;
                if (state.startOffset <= 25) {
                  request.maybeOffsetToSplitWrite = 25;
                } else if (state.startOffset <= 50) {
                  request.maybeOffsetToSplitWrite = 50;
                } else if (state.startOffset <= 75) {
                  request.maybeOffsetToSplitWrite = 75;
                }
                request.writeFlagsToAddAtOffset = WriteFlags::TIMESTAMP_TX;
                container.addRequest(request);
              }));

  // observer 2 wants ACK timestamps at 35, 65, 75
  ON_CALL(*observer2, prewriteMock(_, _, _))
      .WillByDefault(
          testing::Invoke(
              [](AsyncTransport*,
                 const AsyncSocketObserverInterface::PrewriteState& state,
                 AsyncSocketObserverInterface::PrewriteRequestContainer&
                     container) {
                AsyncSocketObserverInterface::PrewriteRequest request;
                if (state.startOffset <= 35) {
                  request.maybeOffsetToSplitWrite = 35;
                } else if (state.startOffset <= 65) {
                  request.maybeOffsetToSplitWrite = 65;
                } else if (state.startOffset <= 75) {
                  request.maybeOffsetToSplitWrite = 75;
                }
                request.writeFlagsToAddAtOffset = WriteFlags::TIMESTAMP_ACK;
                container.addRequest(request);
              }));

  // observer 3 wants WRITE and SCHED flag on every write that occurs
  ON_CALL(*observer3, prewriteMock(_, _, _))
      .WillByDefault(
          testing::Invoke(
              [](AsyncTransport*,
                 const AsyncSocketObserverInterface::PrewriteState& /* state */,
                 AsyncSocketObserverInterface::PrewriteRequestContainer&
                     container) {
                AsyncSocketObserverInterface::PrewriteRequest request;
                request.writeFlagsToAdd =
                    WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED;
                container.addRequest(request);
              }));

  // observer 4 has prewrite but makes no requests
  ON_CALL(*observer4, prewriteMock(_, _, _))
      .WillByDefault(
          testing::Invoke(
              [](AsyncTransport*,
                 const AsyncSocketObserverInterface::PrewriteState& /* state */,
                 AsyncSocketObserverInterface::
                     PrewriteRequestContainer& /* container */) {
                return; // do nothing
              }));

  // no calls for observer 5 or observer 6
  EXPECT_CALL(*observer5, prewriteMock(_, _, _)).Times(0);
  EXPECT_CALL(*observer6, prewriteMock(_, _, _)).Times(0);

  // write
  clientConn.writeAtClientReadAtServerReflectReadAtClient(
      kOneHundredCharacterVec, WriteFlags::NONE);

  EXPECT_THAT(
      clientConn.getSendmsgInvocations(),
      ElementsAre(
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 25),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(
                  WriteFlags::TIMESTAMP_SCHED | WriteFlags::TIMESTAMP_TX)),
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 35),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(
                  WriteFlags::TIMESTAMP_SCHED | WriteFlags::TIMESTAMP_ACK)),
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 50),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(
                  WriteFlags::TIMESTAMP_SCHED | WriteFlags::TIMESTAMP_TX)),
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 65),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(
                  WriteFlags::TIMESTAMP_SCHED | WriteFlags::TIMESTAMP_ACK)),
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 75),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(
                  WriteFlags::TIMESTAMP_SCHED | WriteFlags::TIMESTAMP_TX |
                  WriteFlags::TIMESTAMP_ACK)),
          AllOf(
              SendmsgInvocHasIovLastByte(kOneHundredCharacterVec.data() + 99),
              SendmsgInvocMsgFlagsEq(WriteFlags::NONE),
              SendmsgInvocAncillaryFlagsEq(WriteFlags::TIMESTAMP_SCHED))));

  // verify WRITE events exist at the appropriate locations
  // we verify timestamp events are generated elsewhere
  for (const auto& observer : {observer1, observer2, observer3}) {
    EXPECT_THAT(
        filterToWriteEvents(observer->byteEvents),
        ElementsAre(
            ByteEventMatching(ByteEventType::WRITE, 25),
            ByteEventMatching(ByteEventType::WRITE, 35),
            ByteEventMatching(ByteEventType::WRITE, 50),
            ByteEventMatching(ByteEventType::WRITE, 65),
            ByteEventMatching(ByteEventType::WRITE, 75),
            ByteEventMatching(ByteEventType::WRITE, 99)));
  }
}

/**
 * Test prewrite with large write that enables testing of timestamps.
 *
 * We need to use a long vector to ensure that the kernel will not coalesce
 * the writes into a single SKB due to MSG_MORE.
 */
TEST_F(AsyncSocketByteEventTest, PrewriteTimestampedByteEvents) {
  // need a large block of data to ensure that MSG_MORE doesn't limit us
  const auto hundredKBVec = get1000KBOfData();
  ASSERT_THAT(hundredKBVec, SizeIs(1000000));

  auto clientConn = getClientConn();
  clientConn.connect();
  auto observer = clientConn.attachObserver(
      true /* enableByteEvents */, true /* enablePrewrite */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  clientConn.netOpsOnSendmsgRecordIovecsAndFlagsAndFwd();

  const auto flags = WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK |
      WriteFlags::TIMESTAMP_SCHED | WriteFlags::TIMESTAMP_WRITE;
  ON_CALL(*observer, prewriteMock(_, _, _))
      .WillByDefault(
          testing::Invoke(
              [](AsyncTransport*,
                 const AsyncSocketObserverInterface::PrewriteState& state,
                 AsyncSocketObserverInterface::PrewriteRequestContainer&
                     container) {
                AsyncSocketObserverInterface::PrewriteRequest request;
                if (state.startOffset == 0) {
                  request.maybeOffsetToSplitWrite = 0;
                } else if (state.startOffset <= 500000) {
                  request.maybeOffsetToSplitWrite = 500000;
                } else {
                  request.maybeOffsetToSplitWrite = 999999;
                }

                request.writeFlagsToAdd = flags;
                container.addRequest(request);
              }));

  clientConn.writeAtClientReadAtServerReflectReadAtClient(
      hundredKBVec, WriteFlags::NONE);

  EXPECT_THAT(
      clientConn.getSendmsgInvocations(),
      AllOf(
          Contains(AllOf(
              SendmsgInvocHasIovLastByte(hundredKBVec.data()),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(dropWriteFromFlags(flags)))),
          Contains(AllOf(
              SendmsgInvocHasIovLastByte(hundredKBVec.data() + 500000),
              SendmsgInvocMsgFlagsEq(WriteFlags::CORK),
              SendmsgInvocAncillaryFlagsEq(dropWriteFromFlags(flags)))),
          Contains(AllOf(
              SendmsgInvocHasIovLastByte(hundredKBVec.data() + 999999),
              SendmsgInvocMsgFlagsEq(WriteFlags::NONE),
              SendmsgInvocAncillaryFlagsEq(dropWriteFromFlags(flags))))));

  // verify WRITE events exist at the appropriate locations
  EXPECT_THAT(
      filterToWriteEvents(observer->byteEvents),
      AllOf(
          Contains(ByteEventMatching(ByteEventType::WRITE, 0)),
          Contains(ByteEventMatching(ByteEventType::WRITE, 500000)),
          Contains(ByteEventMatching(ByteEventType::WRITE, 999999))));

  // verify SCHED, TX, and ACK events available at specified locations
  EXPECT_THAT(
      observer->byteEvents,
      AllOf(
          Contains(ByteEventMatching(ByteEventType::SCHED, 0)),
          Contains(ByteEventMatching(ByteEventType::TX, 0)),
          Contains(ByteEventMatching(ByteEventType::ACK, 0)),
          Contains(ByteEventMatching(ByteEventType::SCHED, 500000)),
          Contains(ByteEventMatching(ByteEventType::TX, 500000)),
          Contains(ByteEventMatching(ByteEventType::ACK, 500000)),
          Contains(ByteEventMatching(ByteEventType::SCHED, 999999)),
          Contains(ByteEventMatching(ByteEventType::TX, 999999)),
          Contains(ByteEventMatching(ByteEventType::ACK, 999999))));
}

/**
 * Test raw bytes written and bytes tried to write with prewrite.
 */
TEST_F(AsyncSocketByteEventTest, PrewriteRawBytesWrittenAndTriedToWrite) {
  auto clientConn = getClientConn();
  clientConn.connect();
  auto observer = clientConn.attachObserver(
      true /* enableByteEvents */, true /* enablePrewrite */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  struct ExpectedSendmsgInvocation {
    size_t expectedTotalIovLen{0};
    ssize_t returnVal{0}; // number of bytes written or error val
    folly::Optional<size_t> maybeWriteEventExpectedOffset{};
    folly::Optional<WriteFlags> maybeWriteEventExpectedFlags{};
  };

  const auto flags = WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK |
      WriteFlags::TIMESTAMP_SCHED | WriteFlags::TIMESTAMP_WRITE;

  // first write
  //
  // no splits triggered by observer
  //
  // sendmsg will incrementally accept the bytes so we can test the values of
  // maybeRawBytesWritten and maybeRawBytesTriedToWrite
  {
    // bytes written per sendmsg call: 20, 10, 50, -1 (EAGAIN), 11, 99
    const std::vector<ExpectedSendmsgInvocation> expectedSendmsgInvocations{
        // {
        //    expectedTotalIovLen, returnVal,
        //    maybeWriteEventExpectedOffset, maybeWriteEventExpectedFlags
        // },
        {100, 20, 19, flags},
        {80, 10, 29, flags},
        {70, 50, 79, flags},
        {20, -1, folly::none, flags},
        {20, 11, 90, flags},
        {9, 9, 99, flags}};

    // prewrite will be called, we request all events
    EXPECT_CALL(*observer, prewriteMock(_, _, _))
        .Times(expectedSendmsgInvocations.size())
        .WillRepeatedly(
            testing::Invoke(
                [](AsyncTransport*,
                   const AsyncSocketObserverInterface::
                       PrewriteState& /* state */,
                   AsyncSocketObserverInterface::PrewriteRequestContainer&
                       container) {
                  AsyncSocketObserverInterface::PrewriteRequest request = {};
                  request.writeFlagsToAdd = flags;
                  container.addRequest(request);
                }));

    // sendmsg will be called, we return # of bytes written
    {
      InSequence s;
      for (const auto& expectedInvocation : expectedSendmsgInvocations) {
        EXPECT_CALL(
            *(clientConn.getNetOpsDispatcher()),
            sendmsg(
                _,
                Pointee(SendmsgMsghdrHasTotalIovLen(
                    expectedInvocation.expectedTotalIovLen)),
                _))
            .WillOnce(::testing::InvokeWithoutArgs([expectedInvocation]() {
              if (expectedInvocation.returnVal < 0) {
                errno = EAGAIN; // returning error, set EAGAIN
              }
              return expectedInvocation.returnVal;
            }));
      }
    }

    // write
    // writes will be intercepted, so we don't need to read at other end
    WriteCallback wcb;
    clientConn.getRawSocket()->write(
        &wcb,
        kOneHundredCharacterVec.data(),
        kOneHundredCharacterVec.size(),
        WriteFlags::NONE);
    while (STATE_WAITING == wcb.state) {
      clientConn.getRawSocket()->getEventBase()->loopOnce();
    }
    ASSERT_EQ(STATE_SUCCEEDED, wcb.state);

    // check write events
    for (const auto& expectedInvocation : expectedSendmsgInvocations) {
      if (expectedInvocation.returnVal < 0) {
        // should be no WriteEvent since the return value was an error
        continue;
      }

      ASSERT_TRUE(expectedInvocation.maybeWriteEventExpectedOffset.has_value());
      const auto& expectedOffset =
          *expectedInvocation.maybeWriteEventExpectedOffset;

      auto maybeByteEvent = observer->getByteEventReceivedWithOffset(
          expectedOffset, ByteEventType::WRITE);
      ASSERT_TRUE(maybeByteEvent.has_value());
      auto& byteEvent = maybeByteEvent.value();

      EXPECT_EQ(ByteEventType::WRITE, byteEvent.type);
      EXPECT_EQ(expectedOffset, byteEvent.offset);
      EXPECT_GE(std::chrono::steady_clock::now(), byteEvent.ts);
      EXPECT_LT(
          std::chrono::steady_clock::now() - std::chrono::seconds(60),
          byteEvent.ts);

      EXPECT_EQ(
          expectedInvocation.maybeWriteEventExpectedFlags,
          byteEvent.maybeWriteFlags);
      EXPECT_TRUE(byteEvent.schedTimestampRequestedOnWrite());
      EXPECT_TRUE(byteEvent.txTimestampRequestedOnWrite());
      EXPECT_TRUE(byteEvent.ackTimestampRequestedOnWrite());

      EXPECT_FALSE(byteEvent.maybeSoftwareTs.has_value());
      EXPECT_FALSE(byteEvent.maybeHardwareTs.has_value());

      // what we really want to test
      EXPECT_EQ(
          folly::to_unsigned(expectedInvocation.returnVal),
          byteEvent.maybeRawBytesWritten);
      EXPECT_EQ(
          expectedInvocation.expectedTotalIovLen,
          byteEvent.maybeRawBytesTriedToWrite);
    }
  }

  // everything should have occurred by now
  clientConn.netOpsVerifyAndClearExpectations();

  // second write
  //
  // start offset is 100
  //
  // split at 150th byte triggered by observer
  //
  // sendmsg will incrementally accept the bytes so we can test the values of
  // maybeRawBytesWritten and maybeRawBytesTriedToWrite
  {
    // due to the split at the 150th byte, we expect sendmsg invocation to
    // only be called with bytes 100 -> 150 until after the 150th byte has been
    // written; in addition, the socket only accepts 20 of the 50 bytes the
    // first write.
    //
    // bytes written per sendmsg call: 20, 30, 50
    const std::vector<ExpectedSendmsgInvocation> expectedSendmsgInvocations{
        {50, 20, 119, flags | WriteFlags::CORK},
        {30, 30, 149, flags | WriteFlags::CORK},
        {50, 50, 199, flags}};

    // prewrite will be called, split at 50th byte (offset = 49)
    EXPECT_CALL(*observer, prewriteMock(_, _, _))
        .Times(expectedSendmsgInvocations.size())
        .WillRepeatedly(
            testing::Invoke(
                [](AsyncTransport*,
                   const AsyncSocketObserverInterface::PrewriteState& state,
                   AsyncSocketObserverInterface::PrewriteRequestContainer&
                       container) {
                  AsyncSocketObserverInterface::PrewriteRequest request;
                  if (state.startOffset <= 149) {
                    request.maybeOffsetToSplitWrite = 149; // start offset = 100
                  }
                  request.writeFlagsToAdd = flags;
                  container.addRequest(request);
                }));

    // sendmsg will be called, we return # of bytes written
    {
      InSequence s;
      for (const auto& expectedInvocation : expectedSendmsgInvocations) {
        EXPECT_CALL(
            *(clientConn.getNetOpsDispatcher()),
            sendmsg(
                _,
                Pointee(SendmsgMsghdrHasTotalIovLen(
                    expectedInvocation.expectedTotalIovLen)),
                _))
            .WillOnce(::testing::InvokeWithoutArgs([expectedInvocation]() {
              return expectedInvocation.returnVal;
            }));
      }
    }

    // write
    // writes will be intercepted, so we don't need to read at other end
    WriteCallback wcb;
    clientConn.getRawSocket()->write(
        &wcb,
        kOneHundredCharacterVec.data(),
        kOneHundredCharacterVec.size(),
        WriteFlags::NONE);
    while (STATE_WAITING == wcb.state) {
      clientConn.getRawSocket()->getEventBase()->loopOnce();
    }
    ASSERT_EQ(STATE_SUCCEEDED, wcb.state);

    // check write events
    for (const auto& expectedInvocation : expectedSendmsgInvocations) {
      ASSERT_TRUE(expectedInvocation.maybeWriteEventExpectedOffset.has_value());
      const auto& expectedOffset =
          *expectedInvocation.maybeWriteEventExpectedOffset;

      auto maybeByteEvent = observer->getByteEventReceivedWithOffset(
          expectedOffset, ByteEventType::WRITE);
      ASSERT_TRUE(maybeByteEvent.has_value());
      auto& byteEvent = maybeByteEvent.value();

      EXPECT_EQ(ByteEventType::WRITE, byteEvent.type);
      EXPECT_EQ(expectedOffset, byteEvent.offset);
      EXPECT_GE(std::chrono::steady_clock::now(), byteEvent.ts);
      EXPECT_LT(
          std::chrono::steady_clock::now() - std::chrono::seconds(60),
          byteEvent.ts);

      EXPECT_EQ(
          expectedInvocation.maybeWriteEventExpectedFlags,
          byteEvent.maybeWriteFlags);
      EXPECT_TRUE(byteEvent.schedTimestampRequestedOnWrite());
      EXPECT_TRUE(byteEvent.txTimestampRequestedOnWrite());
      EXPECT_TRUE(byteEvent.ackTimestampRequestedOnWrite());

      EXPECT_FALSE(byteEvent.maybeSoftwareTs.has_value());
      EXPECT_FALSE(byteEvent.maybeHardwareTs.has_value());

      // what we really want to test
      EXPECT_EQ(
          folly::to_unsigned(expectedInvocation.returnVal),
          byteEvent.maybeRawBytesWritten);
      EXPECT_EQ(
          expectedInvocation.expectedTotalIovLen,
          byteEvent.maybeRawBytesTriedToWrite);
    }
  }
}

TEST_F(AsyncSocketByteEventTest, GetTcpInfo_SocketStates) {
  const folly::TcpInfo::LookupOptions options = {};

  auto clientConn = getClientConn();

  // not open
  auto expectedTcpInfo = clientConn.getRawSocket()->getTcpInfo(options);
  EXPECT_FALSE(expectedTcpInfo.hasValue());

  // connected
  clientConn.connect();
  expectedTcpInfo = clientConn.getRawSocket()->getTcpInfo(options);
  EXPECT_TRUE(expectedTcpInfo.hasValue());

  // connected then closed
  clientConn.getRawSocket()->close();
  expectedTcpInfo = clientConn.getRawSocket()->getTcpInfo(options);
  EXPECT_FALSE(expectedTcpInfo.hasValue());
}

/**
 * Enable byte events and have offset correction immediately succeed.
 *
 * bytesSent and sendBufBytes stay the same and thus offset correction completes
 * on the first attempt.
 */
TEST_F(
    AsyncSocketByteEventTest,
    EnableByteEvents_OffsetCorrection_ValuesStaySame) {
  std::shared_ptr<MockTcpInfoDispatcher> mockTcpInfoDispatcher =
      std::make_shared<MockTcpInfoDispatcher>();

  folly::TcpInfo::tcp_info tInfoBefore = {};
  folly::TcpInfo::tcp_info tInfoAfter = {};
  tInfoBefore.tcpi_bytes_sent = 35;
  tInfoAfter.tcpi_bytes_sent = 35;

  folly::TcpInfo wrappedTcpInfoBefore{tInfoBefore};
  folly::TcpInfo wrappedTcpInfoAfter{tInfoAfter};

  wrappedTcpInfoBefore.setSendBufInUseBytes(0);
  wrappedTcpInfoAfter.setSendBufInUseBytes(0);

  auto clientConn = getClientConn();
  clientConn.netOpsExpectNoTimestampingSetSockOpt();
  clientConn.connect();
  clientConn.netOpsVerifyAndClearExpectations();

  clientConn.setMockTcpInfoDispatcher(mockTcpInfoDispatcher);

  {
    InSequence s;

    EXPECT_CALL(*mockTcpInfoDispatcher, initFromFd(_, _, _, _))
        .WillOnce(Return(wrappedTcpInfoBefore))
        .RetiresOnSaturation();

    EXPECT_CALL(*mockTcpInfoDispatcher, initFromFd(_, _, _, _))
        .WillOnce(Return(wrappedTcpInfoAfter))
        .RetiresOnSaturation();
  }

  auto observer = clientConn.attachObserver(true /* enableByteEvents */);

  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());
  clientConn.netOpsVerifyAndClearExpectations();
}

/**
 * Enable byte events and have offset correction repeat due to sendBufInUseBytes
 * changing in between calls to the kernel trying to enable timestamping.
 *
 * The operation should be retried kMaxAttemptsEnableByteEvents times and
 * then fail.
 */
TEST_F(
    AsyncSocketByteEventTest,
    EnableByteEvents_OffsetCorrection_sendBufInUseBytesChangingFail) {
  std::shared_ptr<MockTcpInfoDispatcher> mockTcpInfoDispatcher =
      std::make_shared<MockTcpInfoDispatcher>();

  folly::TcpInfo::tcp_info tInfoBefore = {};
  folly::TcpInfo::tcp_info tInfoAfter = {};
  tInfoBefore.tcpi_bytes_sent = 35;
  tInfoAfter.tcpi_bytes_sent = 35;

  folly::TcpInfo wrappedTcpInfoBefore{tInfoBefore};
  folly::TcpInfo wrappedTcpInfoAfter{tInfoAfter};

  wrappedTcpInfoBefore.setSendBufInUseBytes(1);
  wrappedTcpInfoAfter.setSendBufInUseBytes(0);

  auto clientConn = getClientConn();
  clientConn.netOpsExpectNoTimestampingSetSockOpt();
  clientConn.connect();
  clientConn.netOpsVerifyAndClearExpectations();

  clientConn.setMockTcpInfoDispatcher(mockTcpInfoDispatcher);

  size_t byteEventsEnabledAttempts = 0;

  {
    InSequence s;

    for (; byteEventsEnabledAttempts < kMaxAttemptsEnableByteEvents;
         byteEventsEnabledAttempts++) {
      EXPECT_CALL(*mockTcpInfoDispatcher, initFromFd(_, _, _, _))
          .WillOnce(Return(wrappedTcpInfoBefore))
          .RetiresOnSaturation();

      EXPECT_CALL(*mockTcpInfoDispatcher, initFromFd(_, _, _, _))
          .WillOnce(Return(wrappedTcpInfoAfter))
          .RetiresOnSaturation();
    }
  }

  auto observer = clientConn.attachObserver(true /* enableByteEvents */);

  EXPECT_EQ(byteEventsEnabledAttempts, kMaxAttemptsEnableByteEvents);
  EXPECT_EQ(0, observer->byteEventsEnabledCalled);
  EXPECT_EQ(1, observer->byteEventsUnavailableCalled);
  EXPECT_TRUE(observer->byteEventsUnavailableCalledEx.has_value());
  clientConn.netOpsVerifyAndClearExpectations();
}

/**
 * Enable byte events and have offset correction repeat due to sentBytes
 * changing in between calls to the kernel trying to enable timestamping.
 *
 * The operation should be retried kMaxAttemptsEnableByteEvents times and
 * then fail.
 */
TEST_F(
    AsyncSocketByteEventTest,
    EnableByteEvents_OffsetCorrection_sentBytesChangingFail) {
  std::shared_ptr<MockTcpInfoDispatcher> mockTcpInfoDispatcher =
      std::make_shared<MockTcpInfoDispatcher>();

  folly::TcpInfo::tcp_info tInfoBefore = {};
  folly::TcpInfo::tcp_info tInfoAfter = {};
  tInfoBefore.tcpi_bytes_sent = 35;
  tInfoAfter.tcpi_bytes_sent = 36;

  folly::TcpInfo wrappedTcpInfoBefore{tInfoBefore};
  folly::TcpInfo wrappedTcpInfoAfter{tInfoAfter};

  wrappedTcpInfoBefore.setSendBufInUseBytes(0);
  wrappedTcpInfoAfter.setSendBufInUseBytes(0);

  auto clientConn = getClientConn();
  clientConn.netOpsExpectNoTimestampingSetSockOpt();
  clientConn.connect();
  clientConn.netOpsVerifyAndClearExpectations();

  clientConn.setMockTcpInfoDispatcher(mockTcpInfoDispatcher);

  size_t byteEventsEnabledAttempts = 0;

  {
    InSequence s;

    for (; byteEventsEnabledAttempts < kMaxAttemptsEnableByteEvents;
         byteEventsEnabledAttempts++) {
      EXPECT_CALL(*mockTcpInfoDispatcher, initFromFd(_, _, _, _))
          .WillOnce(Return(wrappedTcpInfoBefore))
          .RetiresOnSaturation();

      EXPECT_CALL(*mockTcpInfoDispatcher, initFromFd(_, _, _, _))
          .WillOnce(Return(wrappedTcpInfoAfter))
          .RetiresOnSaturation();
    }
  }

  auto observer = clientConn.attachObserver(true /* enableByteEvents */);

  EXPECT_EQ(byteEventsEnabledAttempts, kMaxAttemptsEnableByteEvents);
  EXPECT_EQ(0, observer->byteEventsEnabledCalled);
  EXPECT_EQ(1, observer->byteEventsUnavailableCalled);
  EXPECT_TRUE(observer->byteEventsUnavailableCalledEx.has_value());
  clientConn.netOpsVerifyAndClearExpectations();
}

/**
 * Enable byte events and have offset correction repeat due to sendBufInUseBytes
 * changing in between calls to the kernel trying to enable timestamping.
 *
 * The operation should be retried at most kMaxAttemptsEnableByteEvents
 * times and then succeed when sendBufInUseBytes does not change.
 */
TEST_F(
    AsyncSocketByteEventTest,
    EnableByteEvents_OffsetCorrection_sendBufInUseBytesChangingSuccess) {
  std::shared_ptr<MockTcpInfoDispatcher> mockTcpInfoDispatcher =
      std::make_shared<MockTcpInfoDispatcher>();

  folly::TcpInfo::tcp_info tInfoBefore = {};
  folly::TcpInfo::tcp_info tInfoAfter = {};
  tInfoBefore.tcpi_bytes_sent = 36;
  tInfoAfter.tcpi_bytes_sent = 36;

  folly::TcpInfo::tcp_info tInfoBefore2 = {};
  folly::TcpInfo::tcp_info tInfoAfter2 = {};
  tInfoBefore2.tcpi_bytes_sent = 36;
  tInfoAfter2.tcpi_bytes_sent = 36;

  folly::TcpInfo wrappedTcpInfoBefore{tInfoBefore};
  folly::TcpInfo wrappedTcpInfoAfter{tInfoAfter};
  folly::TcpInfo wrappedTcpInfoBefore2{tInfoBefore2};
  folly::TcpInfo wrappedTcpInfoAfter2{tInfoAfter2};

  wrappedTcpInfoBefore.setSendBufInUseBytes(1);
  wrappedTcpInfoAfter.setSendBufInUseBytes(0);
  wrappedTcpInfoBefore2.setSendBufInUseBytes(0);
  wrappedTcpInfoAfter2.setSendBufInUseBytes(0);

  auto clientConn = getClientConn();
  clientConn.netOpsExpectNoTimestampingSetSockOpt();
  clientConn.connect();
  clientConn.netOpsVerifyAndClearExpectations();

  clientConn.setMockTcpInfoDispatcher(mockTcpInfoDispatcher);

  size_t byteEventsEnabledAttempts = 0;
  size_t constexpr kRetriesUntilByteEventsSuccessful = 5;
  EXPECT_LE(kRetriesUntilByteEventsSuccessful, kMaxAttemptsEnableByteEvents);

  {
    InSequence s;

    for (; byteEventsEnabledAttempts < kMaxAttemptsEnableByteEvents;
         byteEventsEnabledAttempts++) {
      if (byteEventsEnabledAttempts == kRetriesUntilByteEventsSuccessful) {
        EXPECT_CALL(*mockTcpInfoDispatcher, initFromFd(_, _, _, _))
            .WillOnce(Return(wrappedTcpInfoBefore2))
            .RetiresOnSaturation();

        EXPECT_CALL(*mockTcpInfoDispatcher, initFromFd(_, _, _, _))
            .WillOnce(Return(wrappedTcpInfoAfter2))
            .RetiresOnSaturation();

        break;
      } else {
        EXPECT_CALL(*mockTcpInfoDispatcher, initFromFd(_, _, _, _))
            .WillOnce(Return(wrappedTcpInfoBefore))
            .RetiresOnSaturation();

        EXPECT_CALL(*mockTcpInfoDispatcher, initFromFd(_, _, _, _))
            .WillOnce(Return(wrappedTcpInfoAfter))
            .RetiresOnSaturation();
      }
    }
  }

  auto observer = clientConn.attachObserver(true /* enableByteEvents */);

  EXPECT_EQ(byteEventsEnabledAttempts, kRetriesUntilByteEventsSuccessful);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());
  clientConn.netOpsVerifyAndClearExpectations();
}

/**
 * Enable byte events and have offset correction repeat due to sentBytes
 * changing in between calls to the kernel trying to enable timestamping.
 *
 * The operation should be retried at most kMaxAttemptsEnableByteEvents
 * times and then succeed when sentBytes does not change.
 */
TEST_F(
    AsyncSocketByteEventTest,
    EnableByteEvents_OffsetCorrection_sentBytesChangingSuccess) {
  std::shared_ptr<MockTcpInfoDispatcher> mockTcpInfoDispatcher =
      std::make_shared<MockTcpInfoDispatcher>();

  folly::TcpInfo::tcp_info tInfoBefore = {};
  folly::TcpInfo::tcp_info tInfoAfter = {};
  tInfoBefore.tcpi_bytes_sent = 35;
  tInfoAfter.tcpi_bytes_sent = 36;

  folly::TcpInfo::tcp_info tInfoBefore2 = {};
  folly::TcpInfo::tcp_info tInfoAfter2 = {};
  tInfoBefore2.tcpi_bytes_sent = 36;
  tInfoAfter2.tcpi_bytes_sent = 36;

  folly::TcpInfo wrappedTcpInfoBefore{tInfoBefore};
  folly::TcpInfo wrappedTcpInfoAfter{tInfoAfter};
  folly::TcpInfo wrappedTcpInfoBefore2{tInfoBefore2};
  folly::TcpInfo wrappedTcpInfoAfter2{tInfoAfter2};

  wrappedTcpInfoBefore.setSendBufInUseBytes(0);
  wrappedTcpInfoAfter.setSendBufInUseBytes(0);
  wrappedTcpInfoBefore2.setSendBufInUseBytes(0);
  wrappedTcpInfoAfter2.setSendBufInUseBytes(0);

  auto clientConn = getClientConn();
  clientConn.netOpsExpectNoTimestampingSetSockOpt();
  clientConn.connect();
  clientConn.netOpsVerifyAndClearExpectations();

  clientConn.setMockTcpInfoDispatcher(mockTcpInfoDispatcher);

  size_t byteEventsEnabledAttempts = 0;
  size_t constexpr kRetriesUntilByteEventsSuccessful = 5;
  EXPECT_LE(kRetriesUntilByteEventsSuccessful, kMaxAttemptsEnableByteEvents);

  {
    InSequence s;

    for (; byteEventsEnabledAttempts < kMaxAttemptsEnableByteEvents;
         byteEventsEnabledAttempts++) {
      if (byteEventsEnabledAttempts == kRetriesUntilByteEventsSuccessful) {
        EXPECT_CALL(*mockTcpInfoDispatcher, initFromFd(_, _, _, _))
            .WillOnce(Return(wrappedTcpInfoBefore2))
            .RetiresOnSaturation();

        EXPECT_CALL(*mockTcpInfoDispatcher, initFromFd(_, _, _, _))
            .WillOnce(Return(wrappedTcpInfoAfter2))
            .RetiresOnSaturation();

        break;
      } else {
        EXPECT_CALL(*mockTcpInfoDispatcher, initFromFd(_, _, _, _))
            .WillOnce(Return(wrappedTcpInfoBefore))
            .RetiresOnSaturation();

        EXPECT_CALL(*mockTcpInfoDispatcher, initFromFd(_, _, _, _))
            .WillOnce(Return(wrappedTcpInfoAfter))
            .RetiresOnSaturation();
      }
    }
  }

  auto observer = clientConn.attachObserver(true /* enableByteEvents */);

  EXPECT_EQ(byteEventsEnabledAttempts, kRetriesUntilByteEventsSuccessful);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());
  clientConn.netOpsVerifyAndClearExpectations();
}

TEST_F(AsyncSocketByteEventTest, EnableByteEventsThrowsWithIoUringBackend) {
  std::unique_ptr<EventBase> evb;
  try {
    evb = makeIoUringEventBase(ioUringOptions);
  } catch (IoUringBackend::NotAvailable const&) {
    GTEST_SKIP() << "IoUringBackend not available";
  }

  auto socket = AsyncSocket::newSocket(evb.get());
  ConnCallback ccb;
  socket->connect(&ccb, server_->getAddress(), 30);
  evb->loop();
  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);

  auto observer = attachObserver(socket.get(), true /* enableByteEvents */);
  EXPECT_EQ(0, observer->byteEventsEnabledCalled);
  EXPECT_EQ(1, observer->byteEventsUnavailableCalled);
  EXPECT_TRUE(observer->byteEventsUnavailableCalledEx.has_value());
  socket.reset();
}

class AsyncSocketByteEventRawOffsetTest
    : public AsyncSocketByteEventTest,
      public testing::WithParamInterface<size_t> {
 public:
  // byte offset of the AsyncSocket when ByteEvents are enabled
  //
  // for some of the tests, the value returned by sendBufInUseBytes
  // will be greater than this value to simulate a case in which
  // bytes are written to the socket prior to the AsyncSocket being
  // initialized, and those bytes still not yet been acked.
  static constexpr size_t kRawByteOffsetWhenByteEventsEnabled = 20;

  // values returned by sendBufInUseBytes()
  static std::vector<size_t> getTestingValues() {
    std::vector<size_t> vals{
        /* Values for sendBufInUseBytes */
        0,
        1,
        10,
        kRawByteOffsetWhenByteEventsEnabled,
        // simulate cases where bytes have already been
        // written to the kernel socket prior to the
        // AsyncSocket being initialized and are still
        // in the sendbuf (either not sent, or not ACKed).
        kRawByteOffsetWhenByteEventsEnabled + 1,
        kRawByteOffsetWhenByteEventsEnabled + 10};
    return vals;
  }
};

INSTANTIATE_TEST_SUITE_P(
    ByteEventRawOffsets,
    AsyncSocketByteEventRawOffsetTest,
    ::testing::ValuesIn(AsyncSocketByteEventRawOffsetTest::getTestingValues()));

/**
 * Enable byte events with varying values of sendBufInUseBytes.
 *
 * This is an end-to-end test verifying proper delivery of timestamps with
 * different byte offset corrections. sendBufInUseBytes varies between zero
 * and a value greater than that reported by getRawBytesWritten(), with the
 * latter providing coverage of a case where bytes were written to the
 * kernel socket prior to the AsyncSocket being initialized and are still
 * in the sendbuf.
 */
TEST_P(AsyncSocketByteEventRawOffsetTest, EnableByteEvents_CheckRawByteOffset) {
  const auto flags = WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
      WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK;

  const auto bytesInSendBuf = GetParam();
  const std::vector<uint8_t> wbuf1(kRawByteOffsetWhenByteEventsEnabled, 'a');
  const std::vector<uint8_t> wbuf2(1, 'a');
  const std::vector<uint8_t> wbufBytesInSendBufOnEnable(bytesInSendBuf, 'a');

  std::shared_ptr<MockTcpInfoDispatcher> mockTcpInfoDispatcher =
      std::make_shared<MockTcpInfoDispatcher>();

  folly::TcpInfo::tcp_info tInfoBefore = {};
  folly::TcpInfo::tcp_info tInfoAfter = {};
  folly::TcpInfo wrappedTcpInfoBefore{tInfoBefore};
  folly::TcpInfo wrappedTcpInfoAfter{tInfoAfter};
  wrappedTcpInfoBefore.setSendBufInUseBytes(bytesInSendBuf);
  wrappedTcpInfoAfter.setSendBufInUseBytes(bytesInSendBuf);

  auto clientConn = getClientConn();
  clientConn.netOpsExpectNoTimestampingSetSockOpt();
  clientConn.connect();
  clientConn.netOpsVerifyAndClearExpectations();

  clientConn.setMockTcpInfoDispatcher(mockTcpInfoDispatcher);

  {
    InSequence s;

    EXPECT_CALL(*mockTcpInfoDispatcher, initFromFd(_, _, _, _))
        .WillOnce(Return(wrappedTcpInfoBefore))
        .RetiresOnSaturation();

    EXPECT_CALL(*mockTcpInfoDispatcher, initFromFd(_, _, _, _))
        .WillOnce(Return(wrappedTcpInfoAfter))
        .RetiresOnSaturation();
  }

  // Write any bytes that we wanted to have sent through the AsyncSocket
  // prior to timestamps being enabled to adjust the rawByteOffset
  clientConn.writeAtClientReadAtServer(wbuf1, WriteFlags::NONE);

  // Enable timestamps
  //
  // AsyncSocket will record the value returned by TcpInfo::sendBufInUseBytes
  // when enabling timestamps to determine the correction factor for timestamp
  // byte offsets. This test controls the value returned by sendBufInUseBytes.
  clientConn.netOpsExpectTimestampingSetSockOpt();
  auto observer = clientConn.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());
  clientConn.netOpsVerifyAndClearExpectations();

  // Write wbufBytesInSendBufOnEnable to the socket (bypassing AsyncSocket)
  //
  // We can't control the actual number of bytes in the socket sendbuf when
  // we enable timestamping in the previous step. Instead, we create the
  // scenario where there are bytes in the sendBuf that we need to correct for
  // by writing bytes directly to the network socket after enabling
  // timestamping. The number of bytes written is identical to the number of
  // bytes that we reported were in the buffer in the previous step, thereby
  // causing kernel timestamp byte offsets to be offset by this amount.
  clientConn.writeAtClientDirectlyToNetworkSocket(wbufBytesInSendBufOnEnable);

  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(dropWriteFromFlags(flags));
  clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf2, flags);
  clientConn.netOpsVerifyAndClearExpectations();
  EXPECT_THAT(observer->byteEvents, SizeIs(4));
  EXPECT_EQ(
      kRawByteOffsetWhenByteEventsEnabled,
      observer->maxOffsetForByteEventReceived(ByteEventType::WRITE).value());
  EXPECT_EQ(
      kRawByteOffsetWhenByteEventsEnabled,
      observer->maxOffsetForByteEventReceived(ByteEventType::SCHED).value());
  EXPECT_EQ(
      kRawByteOffsetWhenByteEventsEnabled,
      observer->maxOffsetForByteEventReceived(ByteEventType::TX).value());
  EXPECT_EQ(
      kRawByteOffsetWhenByteEventsEnabled,
      observer->maxOffsetForByteEventReceived(ByteEventType::ACK).value());

  // write again to check offsets
  clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(dropWriteFromFlags(flags));
  clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf2, flags);
  clientConn.netOpsVerifyAndClearExpectations();
  EXPECT_THAT(observer->byteEvents, SizeIs(8));
  EXPECT_EQ(
      kRawByteOffsetWhenByteEventsEnabled + 1,
      observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
  EXPECT_EQ(
      kRawByteOffsetWhenByteEventsEnabled + 1,
      observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
  EXPECT_EQ(
      kRawByteOffsetWhenByteEventsEnabled + 1,
      observer->maxOffsetForByteEventReceived(ByteEventType::TX));
  EXPECT_EQ(
      kRawByteOffsetWhenByteEventsEnabled + 1,
      observer->maxOffsetForByteEventReceived(ByteEventType::ACK));
}

struct AsyncSocketByteEventDetailsTestParams {
  struct WriteParams {
    WriteParams(uint64_t bufferSize, WriteFlags writeFlags)
        : bufferSize(bufferSize), writeFlags(writeFlags) {}
    uint64_t bufferSize{0};
    WriteFlags writeFlags{WriteFlags::NONE};
  };

  std::vector<WriteParams> writesWithParams;
};

class AsyncSocketByteEventDetailsTest
    : public AsyncSocketByteEventTest,
      public testing::WithParamInterface<
          AsyncSocketByteEventDetailsTestParams> {
 public:
  static std::vector<AsyncSocketByteEventDetailsTestParams> getTestingValues() {
    const std::array<WriteFlags, 9> writeFlagCombinations{
        // SCHED
        WriteFlags::TIMESTAMP_SCHED,
        // TX
        WriteFlags::TIMESTAMP_TX,
        // ACK
        WriteFlags::TIMESTAMP_ACK,
        // SCHED + TX + ACK
        WriteFlags::TIMESTAMP_SCHED | WriteFlags::TIMESTAMP_TX |
            WriteFlags::TIMESTAMP_ACK,
        // WRITE
        WriteFlags::TIMESTAMP_WRITE,
        // WRITE + SCHED
        WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED,
        // WRITE + TX
        WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_TX,
        // WRITE + ACK
        WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_ACK,
        // WRITE + SCHED + TX + ACK
        WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
            WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK,
    };

    std::vector<AsyncSocketByteEventDetailsTestParams> vals;
    for (const auto& writeFlags : writeFlagCombinations) {
      // write 1 byte
      {
        AsyncSocketByteEventDetailsTestParams params;
        params.writesWithParams.emplace_back(1, writeFlags);
        vals.push_back(params);
      }

      // write 1 byte twice
      {
        AsyncSocketByteEventDetailsTestParams params;
        params.writesWithParams.emplace_back(1, writeFlags);
        params.writesWithParams.emplace_back(1, writeFlags);
        vals.push_back(params);
      }

      // write 10 bytes
      {
        AsyncSocketByteEventDetailsTestParams params;
        params.writesWithParams.emplace_back(10, writeFlags);
        vals.push_back(params);
      }

      // write 10 bytes twice
      {
        AsyncSocketByteEventDetailsTestParams params;
        params.writesWithParams.emplace_back(10, writeFlags);
        params.writesWithParams.emplace_back(10, writeFlags);
        vals.push_back(params);
      }
    }

    return vals;
  }
};

INSTANTIATE_TEST_SUITE_P(
    ByteEventDetailsTest,
    AsyncSocketByteEventDetailsTest,
    ::testing::ValuesIn(AsyncSocketByteEventDetailsTest::getTestingValues()));

/**
 * Inspect ByteEvent fields, including xTimestampRequested in WRITE events.
 */
TEST_P(AsyncSocketByteEventDetailsTest, CheckByteEventDetails) {
  auto params = GetParam();

  auto clientConn = getClientConn();
  clientConn.connect();
  auto observer = clientConn.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());

  uint64_t expectedNumByteEvents = 0;
  for (const auto& writeParams : params.writesWithParams) {
    const std::vector<uint8_t> wbuf(writeParams.bufferSize, 'a');
    const auto flags = writeParams.writeFlags;
    clientConn.netOpsExpectSendmsgWithAncillaryTsFlags(
        dropWriteFromFlags(flags));
    clientConn.writeAtClientReadAtServerReflectReadAtClient(wbuf, flags);
    clientConn.netOpsVerifyAndClearExpectations();
    const auto expectedOffset =
        clientConn.getRawSocket()->getRawBytesWritten() - 1;

    // check WRITE
    if ((flags & WriteFlags::TIMESTAMP_WRITE) != WriteFlags::NONE) {
      expectedNumByteEvents++;

      auto maybeByteEvent = observer->getByteEventReceivedWithOffset(
          expectedOffset, ByteEventType::WRITE);
      ASSERT_TRUE(maybeByteEvent.has_value());
      auto& byteEvent = maybeByteEvent.value();

      EXPECT_EQ(ByteEventType::WRITE, byteEvent.type);
      EXPECT_EQ(expectedOffset, byteEvent.offset);
      EXPECT_GE(std::chrono::steady_clock::now(), byteEvent.ts);
      EXPECT_LT(
          std::chrono::steady_clock::now() - std::chrono::seconds(60),
          byteEvent.ts);

      EXPECT_EQ(flags, byteEvent.maybeWriteFlags);
      EXPECT_EQ(
          isSet(flags, WriteFlags::TIMESTAMP_SCHED),
          byteEvent.schedTimestampRequestedOnWrite());
      EXPECT_EQ(
          isSet(flags, WriteFlags::TIMESTAMP_TX),
          byteEvent.txTimestampRequestedOnWrite());
      EXPECT_EQ(
          isSet(flags, WriteFlags::TIMESTAMP_ACK),
          byteEvent.ackTimestampRequestedOnWrite());

      EXPECT_FALSE(byteEvent.maybeSoftwareTs.has_value());
      EXPECT_FALSE(byteEvent.maybeHardwareTs.has_value());
    }

    // check SCHED, TX, ACK
    for (const auto& byteEventType :
         {ByteEventType::SCHED, ByteEventType::TX, ByteEventType::ACK}) {
      auto maybeByteEvent = observer->getByteEventReceivedWithOffset(
          expectedOffset, byteEventType);
      switch (byteEventType) {
        case ByteEventType::WRITE:
          FAIL();
        case ByteEventType::SCHED:
          if ((flags & WriteFlags::TIMESTAMP_SCHED) == WriteFlags::NONE) {
            EXPECT_FALSE(maybeByteEvent.has_value());
            continue;
          }
          break;
        case ByteEventType::TX:
          if ((flags & WriteFlags::TIMESTAMP_TX) == WriteFlags::NONE) {
            EXPECT_FALSE(maybeByteEvent.has_value());
            continue;
          }
          break;
        case ByteEventType::ACK:
          if ((flags & WriteFlags::TIMESTAMP_ACK) == WriteFlags::NONE) {
            EXPECT_FALSE(maybeByteEvent.has_value());
            continue;
          }
          break;
      }

      expectedNumByteEvents++;
      ASSERT_TRUE(maybeByteEvent.has_value());
      auto& byteEvent = maybeByteEvent.value();

      EXPECT_EQ(byteEventType, byteEvent.type);
      EXPECT_EQ(expectedOffset, byteEvent.offset);
      EXPECT_GE(std::chrono::steady_clock::now(), byteEvent.ts);
      EXPECT_LT(
          std::chrono::steady_clock::now() - std::chrono::seconds(60),
          byteEvent.ts);
      EXPECT_FALSE(byteEvent.maybeWriteFlags.has_value());
      // don't check *TimestampRequestedOnWrite* fields to avoid CHECK_DEATH,
      // already checked in CheckByteEventDetailsApplicationSetsFlags

      EXPECT_TRUE(byteEvent.maybeSoftwareTs.has_value());
      EXPECT_FALSE(byteEvent.maybeHardwareTs.has_value());
    }
  }

  // should have at least expectedNumByteEvents
  // may be more if writes were split up by kernel
  EXPECT_THAT(observer->byteEvents, SizeIs(Ge(expectedNumByteEvents)));
}

class AsyncSocketByteEventHelperTest : public ::testing::Test {
 protected:
  using ByteEventType = AsyncSocket::ByteEvent::Type;

  /**
   * Wrapper around a vector containing cmsg header + data.
   */
  class WrappedCMsg {
   public:
    explicit WrappedCMsg(std::vector<char>&& data) : data_(std::move(data)) {}

    operator const struct cmsghdr&() {
      return *reinterpret_cast<struct cmsghdr*>(data_.data());
    }

   protected:
    std::vector<char> data_;
  };

  /**
   * Wrapper around a vector containing cmsg header + data.
   */
  class WrappedSockExtendedErrTsCMsg : public WrappedCMsg {
   public:
    using WrappedCMsg::WrappedCMsg;

    // ts[0] -> software timestamp
    // ts[1] -> hardware timestamp transformed to userspace time (deprecated)
    // ts[2] -> hardware timestamp

    void setSoftwareTimestamp(
        const std::chrono::seconds seconds,
        const std::chrono::nanoseconds nanoseconds) {
      struct cmsghdr* cmsg{reinterpret_cast<cmsghdr*>(data_.data())};
      struct scm_timestamping* tss{
          reinterpret_cast<struct scm_timestamping*>(CMSG_DATA(cmsg))};
      tss->ts[0].tv_sec = seconds.count();
      tss->ts[0].tv_nsec = nanoseconds.count();
    }

    void setHardwareTimestamp(
        const std::chrono::seconds seconds,
        const std::chrono::nanoseconds nanoseconds) {
      struct cmsghdr* cmsg{reinterpret_cast<cmsghdr*>(data_.data())};
      struct scm_timestamping* tss{
          reinterpret_cast<struct scm_timestamping*>(CMSG_DATA(cmsg))};
      tss->ts[2].tv_sec = seconds.count();
      tss->ts[2].tv_nsec = nanoseconds.count();
    }
  };

  static std::vector<char> cmsgData(int level, int type, size_t len) {
    std::vector<char> data(CMSG_LEN(len), 0);
    struct cmsghdr* cmsg{reinterpret_cast<cmsghdr*>(data.data())};
    cmsg->cmsg_level = level;
    cmsg->cmsg_type = type;
    cmsg->cmsg_len = CMSG_LEN(len);
    return data;
  }

  static WrappedSockExtendedErrTsCMsg cmsgForSockExtendedErrTimestamping() {
    return WrappedSockExtendedErrTsCMsg(
        cmsgData(SOL_SOCKET, SO_TIMESTAMPING, sizeof(struct scm_timestamping)));
  }

  static WrappedCMsg cmsgForScmTimestamping(
      const uint32_t type, const uint32_t kernelByteOffset) {
    auto data = cmsgData(SOL_IP, IP_RECVERR, sizeof(struct sock_extended_err));
    struct cmsghdr* cmsg{reinterpret_cast<cmsghdr*>(data.data())};
    struct sock_extended_err* serr{
        reinterpret_cast<struct sock_extended_err*>(CMSG_DATA(cmsg))};
    serr->ee_errno = ENOMSG;
    serr->ee_origin = SO_EE_ORIGIN_TIMESTAMPING;
    serr->ee_info = type;
    serr->ee_data = kernelByteOffset;
    return WrappedCMsg(std::move(data));
  }
};

TEST_F(AsyncSocketByteEventHelperTest, ByteOffsetThenTs) {
  auto scmTs = cmsgForScmTimestamping(folly::netops::SCM_TSTAMP_SND, 0);
  const auto softwareTsSec = std::chrono::seconds(59);
  const auto softwareTsNs = std::chrono::nanoseconds(11);
  auto serrTs = cmsgForSockExtendedErrTimestamping();
  serrTs.setSoftwareTimestamp(softwareTsSec, softwareTsNs);

  AsyncSocket::ByteEventHelper helper = {};
  helper.byteEventsEnabled = true;
  helper.rawBytesWrittenWhenByteEventsEnabled = 0;

  EXPECT_FALSE(helper.processCmsg(scmTs, 1 /* rawBytesWritten */));
  EXPECT_TRUE(helper.processCmsg(serrTs, 1 /* rawBytesWritten */));
}

TEST_F(AsyncSocketByteEventHelperTest, TsThenByteOffset) {
  auto scmTs = cmsgForScmTimestamping(folly::netops::SCM_TSTAMP_SND, 0);
  const auto softwareTsSec = std::chrono::seconds(59);
  const auto softwareTsNs = std::chrono::nanoseconds(11);
  auto serrTs = cmsgForSockExtendedErrTimestamping();
  serrTs.setSoftwareTimestamp(softwareTsSec, softwareTsNs);

  AsyncSocket::ByteEventHelper helper = {};
  helper.byteEventsEnabled = true;
  helper.rawBytesWrittenWhenByteEventsEnabled = 0;

  EXPECT_FALSE(helper.processCmsg(serrTs, 1 /* rawBytesWritten */));
  EXPECT_TRUE(helper.processCmsg(scmTs, 1 /* rawBytesWritten */));
}

TEST_F(AsyncSocketByteEventHelperTest, RejectInvalidFutureByteOffset) {
  constexpr size_t kBytesWrittenSinceEnable = 4'325'057;
  constexpr uint32_t kInvalidKernelByteOffset = 0x93e01b40;
  constexpr size_t kBytesPerOffsetWrap =
      static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1;

  auto timestampCmsg = cmsgForSockExtendedErrTimestamping();
  auto invalidOffsetCmsg = cmsgForScmTimestamping(
      folly::netops::SCM_TSTAMP_SND, kInvalidKernelByteOffset);

  for (const auto rawBytesWrittenWhenByteEventsEnabled :
       std::array<size_t, 2>{0, 2 * kBytesPerOffsetWrap}) {
    const auto rawBytesWritten =
        rawBytesWrittenWhenByteEventsEnabled + kBytesWrittenSinceEnable;
    AsyncSocket::ByteEventHelper helper = {};
    helper.byteEventsEnabled = true;
    helper.rawBytesWrittenWhenByteEventsEnabled =
        rawBytesWrittenWhenByteEventsEnabled;

    EXPECT_FALSE(helper.processCmsg(timestampCmsg, rawBytesWritten));
    EXPECT_FALSE(helper.processCmsg(invalidOffsetCmsg, rawBytesWritten));

    // Rejecting a corrupt event must not prevent later valid events.
    auto validOffsetCmsg = cmsgForScmTimestamping(
        folly::netops::SCM_TSTAMP_SND,
        static_cast<uint32_t>(kBytesWrittenSinceEnable - 1));
    EXPECT_FALSE(helper.processCmsg(timestampCmsg, rawBytesWritten));
    const auto validEvent =
        helper.processCmsg(validOffsetCmsg, rawBytesWritten);
    ASSERT_TRUE(validEvent.has_value());
    EXPECT_EQ(rawBytesWritten - 1, validEvent->offset);
  }
}

TEST_F(AsyncSocketByteEventHelperTest, NegativeEnableOffset) {
  constexpr size_t kRawBytesWritten = 30;

  auto timestampCmsg = cmsgForSockExtendedErrTimestamping();
  auto beforeAsyncSocketOffsetCmsg =
      cmsgForScmTimestamping(folly::netops::SCM_TSTAMP_SND, 50);
  auto validOffsetCmsg =
      cmsgForScmTimestamping(folly::netops::SCM_TSTAMP_SND, 125);

  AsyncSocket::ByteEventHelper helper = {};
  helper.byteEventsEnabled = true;
  helper.rawBytesWrittenWhenByteEventsEnabled = -100;

  EXPECT_FALSE(helper.processCmsg(timestampCmsg, kRawBytesWritten));
  EXPECT_FALSE(
      helper.processCmsg(beforeAsyncSocketOffsetCmsg, kRawBytesWritten));

  EXPECT_FALSE(helper.processCmsg(timestampCmsg, kRawBytesWritten));
  const auto validEvent = helper.processCmsg(validOffsetCmsg, kRawBytesWritten);
  ASSERT_TRUE(validEvent.has_value());
  EXPECT_EQ(25, validEvent->offset);
}

TEST_F(AsyncSocketByteEventHelperTest, LateEnableOffsetPreviousEpoch) {
  constexpr size_t kBytesPerOffsetWrap =
      static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1;
  constexpr size_t kRawBytesWrittenWhenByteEventsEnabled =
      2 * kBytesPerOffsetWrap + 100;
  constexpr size_t kByteTimestamped = 3 * kBytesPerOffsetWrap - 5;
  constexpr size_t kRawBytesWritten = 3 * kBytesPerOffsetWrap + 10;
  constexpr uint32_t kKernelByteOffset =
      kByteTimestamped - kRawBytesWrittenWhenByteEventsEnabled;

  auto timestampCmsg = cmsgForSockExtendedErrTimestamping();
  auto offsetCmsg =
      cmsgForScmTimestamping(folly::netops::SCM_TSTAMP_SND, kKernelByteOffset);

  AsyncSocket::ByteEventHelper helper = {};
  helper.byteEventsEnabled = true;
  helper.rawBytesWrittenWhenByteEventsEnabled =
      kRawBytesWrittenWhenByteEventsEnabled;

  EXPECT_FALSE(helper.processCmsg(timestampCmsg, kRawBytesWritten));
  const auto event = helper.processCmsg(offsetCmsg, kRawBytesWritten);
  ASSERT_TRUE(event.has_value());
  EXPECT_EQ(kByteTimestamped, event->offset);
}

TEST_F(AsyncSocketByteEventHelperTest, ByteEventsDisabled) {
  auto scmTs = cmsgForScmTimestamping(folly::netops::SCM_TSTAMP_SND, 0);
  const auto softwareTsSec = std::chrono::seconds(59);
  const auto softwareTsNs = std::chrono::nanoseconds(11);
  auto serrTs = cmsgForSockExtendedErrTimestamping();
  serrTs.setSoftwareTimestamp(softwareTsSec, softwareTsNs);

  AsyncSocket::ByteEventHelper helper = {};
  helper.byteEventsEnabled = false;
  helper.rawBytesWrittenWhenByteEventsEnabled = 0;

  // fails because disabled
  EXPECT_FALSE(helper.processCmsg(scmTs, 1 /* rawBytesWritten */));
  EXPECT_FALSE(helper.processCmsg(serrTs, 1 /* rawBytesWritten */));

  // enable, try again to prove this works
  helper.byteEventsEnabled = true;
  EXPECT_FALSE(helper.processCmsg(scmTs, 1 /* rawBytesWritten */));
  EXPECT_TRUE(helper.processCmsg(serrTs, 1 /* rawBytesWritten */));
}

TEST_F(AsyncSocketByteEventHelperTest, IgnoreUnsupportedEvent) {
  auto scmType =
      folly::netops::SCM_TSTAMP_ACK + 10; // imaginary new type of SCM event
  auto scmTs = cmsgForScmTimestamping(scmType, 0);
  const auto softwareTsSec = std::chrono::seconds(59);
  const auto softwareTsNs = std::chrono::nanoseconds(11);
  auto serrTs = cmsgForSockExtendedErrTimestamping();
  serrTs.setSoftwareTimestamp(softwareTsSec, softwareTsNs);

  AsyncSocket::ByteEventHelper helper = {};
  helper.byteEventsEnabled = true;
  helper.rawBytesWrittenWhenByteEventsEnabled = 0;

  // unsupported event is eaten
  EXPECT_FALSE(helper.processCmsg(scmTs, 1 /* rawBytesWritten */));
  EXPECT_FALSE(helper.processCmsg(serrTs, 1 /* rawBytesWritten */));

  // change type, try again to prove this works
  scmTs = cmsgForScmTimestamping(folly::netops::SCM_TSTAMP_ACK, 0);
  EXPECT_FALSE(helper.processCmsg(scmTs, 1 /* rawBytesWritten */));
  EXPECT_TRUE(helper.processCmsg(serrTs, 1 /* rawBytesWritten */));
}

TEST_F(AsyncSocketByteEventHelperTest, ErrorDoubleScmCmsg) {
  auto scmTs = cmsgForScmTimestamping(folly::netops::SCM_TSTAMP_SND, 0);

  AsyncSocket::ByteEventHelper helper = {};
  helper.byteEventsEnabled = true;
  helper.rawBytesWrittenWhenByteEventsEnabled = 0;
  EXPECT_FALSE(helper.processCmsg(scmTs, 1 /* rawBytesWritten */));
  EXPECT_THROW(
      helper.processCmsg(scmTs, 1 /* rawBytesWritten */),
      AsyncSocket::ByteEventHelper::Exception);
}

TEST_F(AsyncSocketByteEventHelperTest, ErrorDoubleSerrCmsg) {
  const auto softwareTsSec = std::chrono::seconds(59);
  const auto softwareTsNs = std::chrono::nanoseconds(11);
  auto serrTs = cmsgForSockExtendedErrTimestamping();
  serrTs.setSoftwareTimestamp(softwareTsSec, softwareTsNs);

  AsyncSocket::ByteEventHelper helper = {};
  helper.byteEventsEnabled = true;
  helper.rawBytesWrittenWhenByteEventsEnabled = 0;
  EXPECT_FALSE(helper.processCmsg(serrTs, 1 /* rawBytesWritten */));
  EXPECT_THROW(
      helper.processCmsg(serrTs, 1 /* rawBytesWritten */),
      AsyncSocket::ByteEventHelper::Exception);
}

TEST_F(AsyncSocketByteEventHelperTest, ErrorExceptionSet) {
  auto scmTs = cmsgForScmTimestamping(folly::netops::SCM_TSTAMP_SND, 0);
  const auto softwareTsSec = std::chrono::seconds(59);
  const auto softwareTsNs = std::chrono::nanoseconds(11);
  auto serrTs = cmsgForSockExtendedErrTimestamping();
  serrTs.setSoftwareTimestamp(softwareTsSec, softwareTsNs);

  AsyncSocket::ByteEventHelper helper = {};
  helper.byteEventsEnabled = true;
  helper.rawBytesWrittenWhenByteEventsEnabled = 0;
  helper.maybeEx = AsyncSocketException(
      AsyncSocketException::AsyncSocketExceptionType::UNKNOWN, "");

  // fails due to existing exception
  EXPECT_FALSE(helper.processCmsg(scmTs, 1 /* rawBytesWritten */));
  EXPECT_FALSE(helper.processCmsg(serrTs, 1 /* rawBytesWritten */));

  // delete the exception, then repeat to prove exception was blocking
  helper.maybeEx = folly::none;
  EXPECT_FALSE(helper.processCmsg(scmTs, 1 /* rawBytesWritten */));
  EXPECT_TRUE(helper.processCmsg(serrTs, 1 /* rawBytesWritten */));
}

struct AsyncSocketByteEventHelperTimestampTestParams {
  AsyncSocketByteEventHelperTimestampTestParams(
      uint32_t scmType,
      AsyncSocket::ByteEvent::Type expectedByteEventType,
      bool includeSoftwareTs,
      bool includeHardwareTs)
      : scmType(scmType),
        expectedByteEventType(expectedByteEventType),
        includeSoftwareTs(includeSoftwareTs),
        includeHardwareTs(includeHardwareTs) {}
  uint32_t scmType{0};
  AsyncSocket::ByteEvent::Type expectedByteEventType;
  bool includeSoftwareTs{false};
  bool includeHardwareTs{false};
};

class AsyncSocketByteEventHelperTimestampTest
    : public AsyncSocketByteEventHelperTest,
      public testing::WithParamInterface<
          AsyncSocketByteEventHelperTimestampTestParams> {
 public:
  static std::vector<AsyncSocketByteEventHelperTimestampTestParams>
  getTestingValues() {
    std::vector<AsyncSocketByteEventHelperTimestampTestParams> vals;

    // software + hardware timestamps
    {
      vals.emplace_back(
          folly::netops::SCM_TSTAMP_SCHED, ByteEventType::SCHED, true, true);
      vals.emplace_back(
          folly::netops::SCM_TSTAMP_SND, ByteEventType::TX, true, true);
      vals.emplace_back(
          folly::netops::SCM_TSTAMP_ACK, ByteEventType::ACK, true, true);
    }

    // software ts only
    {
      vals.emplace_back(
          folly::netops::SCM_TSTAMP_SCHED, ByteEventType::SCHED, true, false);
      vals.emplace_back(
          folly::netops::SCM_TSTAMP_SND, ByteEventType::TX, true, false);
      vals.emplace_back(
          folly::netops::SCM_TSTAMP_ACK, ByteEventType::ACK, true, false);
    }

    // hardware ts only
    {
      vals.emplace_back(
          folly::netops::SCM_TSTAMP_SCHED, ByteEventType::SCHED, false, true);
      vals.emplace_back(
          folly::netops::SCM_TSTAMP_SND, ByteEventType::TX, false, true);
      vals.emplace_back(
          folly::netops::SCM_TSTAMP_ACK, ByteEventType::ACK, false, true);
    }

    return vals;
  }
};

INSTANTIATE_TEST_SUITE_P(
    ByteEventTimestampTest,
    AsyncSocketByteEventHelperTimestampTest,
    ::testing::ValuesIn(
        AsyncSocketByteEventHelperTimestampTest::getTestingValues()));

/**
 * Check timestamp parsing for software and hardware timestamps.
 */
TEST_P(AsyncSocketByteEventHelperTimestampTest, CheckEventTimestamps) {
  const auto softwareTsSec = std::chrono::seconds(59);
  const auto softwareTsNs = std::chrono::nanoseconds(11);
  const auto hardwareTsSec = std::chrono::seconds(79);
  const auto hardwareTsNs = std::chrono::nanoseconds(31);

  auto params = GetParam();
  auto scmTs = cmsgForScmTimestamping(params.scmType, 0);
  auto serrTs = cmsgForSockExtendedErrTimestamping();
  if (params.includeSoftwareTs) {
    serrTs.setSoftwareTimestamp(softwareTsSec, softwareTsNs);
  }
  if (params.includeHardwareTs) {
    serrTs.setHardwareTimestamp(hardwareTsSec, hardwareTsNs);
  }

  AsyncSocket::ByteEventHelper helper = {};
  helper.byteEventsEnabled = true;
  helper.rawBytesWrittenWhenByteEventsEnabled = 0;
  folly::Optional<AsyncSocket::ByteEvent> maybeByteEvent;
  maybeByteEvent = helper.processCmsg(serrTs, 1 /* rawBytesWritten */);
  EXPECT_FALSE(maybeByteEvent.has_value());
  maybeByteEvent = helper.processCmsg(scmTs, 1 /* rawBytesWritten */);

  // common checks
  ASSERT_TRUE(maybeByteEvent.has_value());
  const auto& byteEvent = *maybeByteEvent;
  EXPECT_EQ(0, byteEvent.offset);
  EXPECT_GE(std::chrono::steady_clock::now(), byteEvent.ts);

  EXPECT_EQ(params.expectedByteEventType, byteEvent.type);
  if (params.includeSoftwareTs) {
    EXPECT_EQ(softwareTsSec + softwareTsNs, byteEvent.maybeSoftwareTs);
  }
  if (params.includeHardwareTs) {
    EXPECT_EQ(hardwareTsSec + hardwareTsNs, byteEvent.maybeHardwareTs);
  }
}

struct AsyncSocketByteEventHelperOffsetTestParams {
  uint64_t rawBytesWrittenWhenByteEventsEnabled{0};
  uint64_t byteTimestamped;
  uint64_t rawBytesWrittenWhenTimestampReceived;
};

class AsyncSocketByteEventHelperOffsetTest
    : public AsyncSocketByteEventHelperTest,
      public testing::WithParamInterface<
          AsyncSocketByteEventHelperOffsetTestParams> {
 public:
  static std::vector<AsyncSocketByteEventHelperOffsetTestParams>
  getTestingValues() {
    std::vector<AsyncSocketByteEventHelperOffsetTestParams> vals;
    constexpr uint64_t kBytesPerOffsetWrap =
        static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1;
    const std::array<uint64_t, 7> rawBytesWrittenWhenByteEventsEnabledVals{
        0,
        1,
        100,
        kBytesPerOffsetWrap - 1,
        kBytesPerOffsetWrap,
        2 * kBytesPerOffsetWrap,
        2 * kBytesPerOffsetWrap + 100};
    for (const auto& rawBytesWrittenWhenByteEventsEnabled :
         rawBytesWrittenWhenByteEventsEnabledVals) {
      auto addParams = [&](auto params) {
        // check if case is valid based on rawBytesWrittenWhenByteEventsEnabled
        if (rawBytesWrittenWhenByteEventsEnabled <= params.byteTimestamped) {
          vals.push_back(params);
        }
      };

      // first byte timestamped after byte events are enabled
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = rawBytesWrittenWhenByteEventsEnabled;
        params.rawBytesWrittenWhenTimestampReceived =
            rawBytesWrittenWhenByteEventsEnabled + 1;
        addParams(params);
      }

      // case 1
      // bytes sent on receipt of timestamp == byte timestamped
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = 0;
        params.rawBytesWrittenWhenTimestampReceived = 0;
        addParams(params);
      }
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = 1;
        params.rawBytesWrittenWhenTimestampReceived = 1;
        addParams(params);
      }
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = 101;
        params.rawBytesWrittenWhenTimestampReceived = 101;
        addParams(params);
      }

      // bytes sent on receipt of timestamp > byte timestamped
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = 1;
        params.rawBytesWrittenWhenTimestampReceived = 2;
        addParams(params);
      }
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = 101;
        params.rawBytesWrittenWhenTimestampReceived = 102;
        addParams(params);
      }

      // case 2
      // bytes sent on receipt of timestamp == byte timestamped, boundary test
      // (boundary is at 2^32)
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = 4294967294;
        params.rawBytesWrittenWhenTimestampReceived = 4294967294;
        addParams(params);
      }
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = 4294967295;
        params.rawBytesWrittenWhenTimestampReceived = 4294967295;
        addParams(params);
      }
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = 4294967296;
        params.rawBytesWrittenWhenTimestampReceived = 4294967296;
        addParams(params);
      }
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = 4294967297;
        params.rawBytesWrittenWhenTimestampReceived = 4294967297;
        addParams(params);
      }
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = 4294967298;
        params.rawBytesWrittenWhenTimestampReceived = 4294967298;
        addParams(params);
      }

      // case 3
      // bytes sent on receipt of timestamp > byte timestamped, boundary test
      // (boundary is at 2^32)
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = 4294967293;
        params.rawBytesWrittenWhenTimestampReceived = 4294967294;
        addParams(params);
      }
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = 4294967294;
        params.rawBytesWrittenWhenTimestampReceived = 4294967295;
        addParams(params);
      }
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = 4294967295;
        params.rawBytesWrittenWhenTimestampReceived = 4294967296;
        addParams(params);
      }
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = 4294967296;
        params.rawBytesWrittenWhenTimestampReceived = 4294967297;
        addParams(params);
      }

      // case 4
      // bytes sent on receipt of timestamp > byte timestamped, wrap test
      // (boundary is at 2^32)
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = 4294967275;
        params.rawBytesWrittenWhenTimestampReceived = 4294967305;
        addParams(params);
      }
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = 4294967295;
        params.rawBytesWrittenWhenTimestampReceived = 4294967296;
        addParams(params);
      }
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = 4294967285;
        params.rawBytesWrittenWhenTimestampReceived = 4294967305;
        addParams(params);
      }

      // case 5
      // special case when timestamp enabled when bytes transferred > (2^32)
      // bytes sent on receipt of timestamp == byte timestamped, boundary test
      // (boundary is at 2^32)
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = 6442450943;
        params.rawBytesWrittenWhenTimestampReceived = 6442450943;
        addParams(params);
      }

      // case 6
      // special case when timestamp enabled when bytes transferred > (2^32)
      // bytes sent on receipt of timestamp > byte timestamped, boundary test
      // (boundary is at 2^32)
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = 6442450943;
        params.rawBytesWrittenWhenTimestampReceived = 6442450944;
        addParams(params);
      }

      // case 7
      // special case when timestamp enabled when bytes transferred > (2^32)
      // bytes sent on receipt of timestamp > byte timestamped, wrap test
      // (boundary is at 2^32)
      {
        AsyncSocketByteEventHelperOffsetTestParams params;
        params.rawBytesWrittenWhenByteEventsEnabled =
            rawBytesWrittenWhenByteEventsEnabled;
        params.byteTimestamped = 6442450943;
        params.rawBytesWrittenWhenTimestampReceived = 8589934591;
        addParams(params);
      }
    }

    return vals;
  }
};

INSTANTIATE_TEST_SUITE_P(
    ByteEventOffsetTest,
    AsyncSocketByteEventHelperOffsetTest,
    ::testing::ValuesIn(
        AsyncSocketByteEventHelperOffsetTest::getTestingValues()));

/**
 * Check byte offset handling, including boundary cases.
 *
 * See AsyncSocket::ByteEventHelper::processCmsg for details.
 */
TEST_P(AsyncSocketByteEventHelperOffsetTest, CheckCalculatedOffset) {
  auto params = GetParam();

  // because we use SOF_TIMESTAMPING_OPT_ID, byte offsets delivered from the
  // kernel are offset (relative to bytes written by AsyncSocket) by the number
  // of bytes AsyncSocket had written to the socket when enabling timestamps
  //
  // here we calculate what the kernel offset would be for the given byte offset
  const uint64_t bytesPerOffsetWrap =
      static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1;

  auto kernelByteOffset =
      params.byteTimestamped - params.rawBytesWrittenWhenByteEventsEnabled;
  if (kernelByteOffset > 0) {
    kernelByteOffset = kernelByteOffset % bytesPerOffsetWrap;
  }

  auto scmTs =
      cmsgForScmTimestamping(folly::netops::SCM_TSTAMP_SND, kernelByteOffset);
  const auto softwareTsSec = std::chrono::seconds(59);
  const auto softwareTsNs = std::chrono::nanoseconds(11);
  auto serrTs = cmsgForSockExtendedErrTimestamping();
  serrTs.setSoftwareTimestamp(softwareTsSec, softwareTsNs);

  AsyncSocket::ByteEventHelper helper = {};
  helper.byteEventsEnabled = true;
  helper.rawBytesWrittenWhenByteEventsEnabled =
      params.rawBytesWrittenWhenByteEventsEnabled;

  EXPECT_FALSE(helper.processCmsg(
      scmTs,
      params.rawBytesWrittenWhenTimestampReceived /* rawBytesWritten */));
  const auto maybeByteEvent = helper.processCmsg(
      serrTs,
      params.rawBytesWrittenWhenTimestampReceived /* rawBytesWritten */);
  ASSERT_TRUE(maybeByteEvent.has_value());
  const auto& byteEvent = *maybeByteEvent;

  EXPECT_EQ(params.byteTimestamped, byteEvent.offset);
  EXPECT_EQ(softwareTsSec + softwareTsNs, byteEvent.maybeSoftwareTs);
}

#endif // FOLLY_HAVE_SO_TIMESTAMPING

TEST(AsyncSocket, LifecycleCtorCallback) {
  EventBase evb;
  // create socket and verify that w/o a ctor callback, nothing happens
  auto socket1 = AsyncSocket::UniquePtr(new AsyncSocket(&evb));
  EXPECT_EQ(socket1->getLifecycleObservers().size(), 0);

  // Then register a ctor callback that registers a mock lifecycle observer
  // NB: use nicemock instead of strict b/c the actual lifecycle testing
  // is done below and this simplifies the test
  auto lifecycleCB =
      std::make_shared<NiceMock<MockAsyncSocketLifecycleObserver>>();
  // NB: ConstructorCallbackList has no removal API, so this callback stays
  // registered process-wide after this test finishes. Capture a weak_ptr and
  // skip once the observer is destroyed; a raw pointer here would dangle and
  // corrupt every AsyncSocket constructed by later tests in the process.
  auto lifecycleWeak = std::weak_ptr{lifecycleCB};
  // verify the first part of the lifecycle was processed
  ConstructorCallbackList<AsyncSocket>::addCallback(
      [lifecycleWeak](AsyncSocket* s) {
        if (auto cb = lifecycleWeak.lock()) {
          s->addLifecycleObserver(cb.get());
        }
      });
  auto socket2 = AsyncSocket::UniquePtr(new AsyncSocket(&evb));
  EXPECT_EQ(socket2->getLifecycleObservers().size(), 1);
  EXPECT_THAT(
      socket2->getLifecycleObservers(),
      UnorderedElementsAre(lifecycleCB.get()));
  Mock::VerifyAndClearExpectations(lifecycleCB.get());
}

TEST_P(AsyncSocketTest, LifecycleObserverDetachAndAttachEvb) {
  auto cb = std::make_unique<StrictMock<MockAsyncSocketLifecycleObserver>>();
  EventBase& evb = getEventBase();
  auto evb2 = makeEventBase();
  auto socket = AsyncSocket::UniquePtr(new AsyncSocket(&evb));
  EXPECT_CALL(*cb, observerAttachMock(socket.get()));
  socket->addLifecycleObserver(cb.get());
  EXPECT_THAT(socket->getLifecycleObservers(), UnorderedElementsAre(cb.get()));
  Mock::VerifyAndClearExpectations(cb.get());

  // Detach the evb and attach a new evb2
  EXPECT_CALL(*cb, evbDetachMock(socket.get(), &evb));
  socket->detachEventBase();
  EXPECT_EQ(nullptr, socket->getEventBase());
  Mock::VerifyAndClearExpectations(cb.get());

  EXPECT_CALL(*cb, evbAttachMock(socket.get(), evb2.get()));
  socket->attachEventBase(evb2.get());
  EXPECT_EQ(evb2.get(), socket->getEventBase());
  Mock::VerifyAndClearExpectations(cb.get());

  // detach the new evb2 and re-attach the old evb.
  EXPECT_CALL(*cb, evbDetachMock(socket.get(), evb2.get()));
  socket->detachEventBase();
  EXPECT_EQ(nullptr, socket->getEventBase());
  Mock::VerifyAndClearExpectations(cb.get());

  EXPECT_CALL(*cb, evbAttachMock(socket.get(), &evb));
  socket->attachEventBase(&evb);
  EXPECT_EQ(&evb, socket->getEventBase());
  Mock::VerifyAndClearExpectations(cb.get());

  InSequence s;
  EXPECT_CALL(*cb, destroyMock(socket.get()));
  socket = nullptr;
  Mock::VerifyAndClearExpectations(cb.get());
}

TEST_P(AsyncSocketTest, LifecycleObserverAttachThenDestroySocket) {
  auto cb = std::make_unique<StrictMock<MockAsyncSocketLifecycleObserver>>();
  TestServer server;

  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::UniquePtr(new AsyncSocket(&evb));
  EXPECT_CALL(*cb, observerAttachMock(socket.get()));
  socket->addLifecycleObserver(cb.get());
  EXPECT_THAT(socket->getLifecycleObservers(), UnorderedElementsAre(cb.get()));
  Mock::VerifyAndClearExpectations(cb.get());

  EXPECT_CALL(*cb, connectAttemptMock(socket.get()));
  EXPECT_CALL(*cb, fdAttachMock(socket.get()));
  EXPECT_CALL(*cb, connectSuccessMock(socket.get()));
  socket->connect(nullptr, server.getAddress(), 30);
  evb.loop();
  Mock::VerifyAndClearExpectations(cb.get());

  InSequence s;
  EXPECT_CALL(*cb, closeMock(socket.get()));
  EXPECT_CALL(*cb, destroyMock(socket.get()));
  socket = nullptr;
  Mock::VerifyAndClearExpectations(cb.get());
}

TEST_P(AsyncSocketTest, LifecycleObserverAttachThenConnectError) {
  auto cb = std::make_unique<StrictMock<MockAsyncSocketLifecycleObserver>>();
  // port =1 is unreachble on localhost
  folly::SocketAddress unreachable{"::1", 1};

  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::UniquePtr(new AsyncSocket(&evb));
  EXPECT_CALL(*cb, observerAttachMock(socket.get()));
  socket->addLifecycleObserver(cb.get());
  EXPECT_THAT(socket->getLifecycleObservers(), UnorderedElementsAre(cb.get()));
  Mock::VerifyAndClearExpectations(cb.get());

  // the current state machine calls AsyncSocket::invokeConnectionError() twice
  // for this use-case...
  EXPECT_CALL(*cb, connectAttemptMock(socket.get()));
  EXPECT_CALL(*cb, fdAttachMock(socket.get()));
  EXPECT_CALL(*cb, connectErrorMock(socket.get(), _)).Times(2);
  EXPECT_CALL(*cb, closeMock(socket.get()));
  socket->connect(nullptr, unreachable, 1);
  evb.loop();
  Mock::VerifyAndClearExpectations(cb.get());

  EXPECT_CALL(*cb, destroyMock(socket.get()));
  socket = nullptr;
  Mock::VerifyAndClearExpectations(cb.get());
}

TEST_P(AsyncSocketTest, LifecycleObserverMultipleAttachThenDestroySocket) {
  auto cb1 = std::make_unique<StrictMock<MockAsyncSocketLifecycleObserver>>();
  auto cb2 = std::make_unique<StrictMock<MockAsyncSocketLifecycleObserver>>();
  TestServer server;

  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::UniquePtr(new AsyncSocket(&evb));
  EXPECT_CALL(*cb1, observerAttachMock(socket.get()));
  socket->addLifecycleObserver(cb1.get());
  EXPECT_THAT(socket->getLifecycleObservers(), UnorderedElementsAre(cb1.get()));
  Mock::VerifyAndClearExpectations(cb1.get());
  Mock::VerifyAndClearExpectations(cb2.get());

  EXPECT_CALL(*cb2, observerAttachMock(socket.get()));
  socket->addLifecycleObserver(cb2.get());
  EXPECT_THAT(
      socket->getLifecycleObservers(),
      UnorderedElementsAre(cb1.get(), cb2.get()));
  Mock::VerifyAndClearExpectations(cb1.get());
  Mock::VerifyAndClearExpectations(cb2.get());

  InSequence s;
  EXPECT_CALL(*cb1, connectAttemptMock(socket.get()));
  EXPECT_CALL(*cb2, connectAttemptMock(socket.get()));
  EXPECT_CALL(*cb1, fdAttachMock(socket.get()));
  EXPECT_CALL(*cb2, fdAttachMock(socket.get()));
  EXPECT_CALL(*cb1, connectSuccessMock(socket.get()));
  EXPECT_CALL(*cb2, connectSuccessMock(socket.get()));
  socket->connect(nullptr, server.getAddress(), 30);
  evb.loop();
  Mock::VerifyAndClearExpectations(cb1.get());
  Mock::VerifyAndClearExpectations(cb2.get());

  EXPECT_CALL(*cb1, closeMock(socket.get()));
  EXPECT_CALL(*cb2, closeMock(socket.get()));
  EXPECT_CALL(*cb1, destroyMock(socket.get()));
  EXPECT_CALL(*cb2, destroyMock(socket.get()));
  socket = nullptr;
  Mock::VerifyAndClearExpectations(cb1.get());
  Mock::VerifyAndClearExpectations(cb2.get());
}

TEST_P(AsyncSocketTest, LifecycleObserverAttachRemove) {
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::UniquePtr(new AsyncSocket(&evb));

  auto cb = std::make_unique<StrictMock<MockAsyncSocketLifecycleObserver>>();
  EXPECT_CALL(*cb, observerAttachMock(socket.get()));
  socket->addLifecycleObserver(cb.get());
  Mock::VerifyAndClearExpectations(cb.get());

  EXPECT_THAT(socket->getLifecycleObservers(), UnorderedElementsAre(cb.get()));
  EXPECT_CALL(*cb, observerDetachMock(socket.get()));
  EXPECT_TRUE(socket->removeLifecycleObserver(cb.get()));
  EXPECT_THAT(socket->getLifecycleObservers(), IsEmpty());
  Mock::VerifyAndClearExpectations(cb.get());
}

TEST_P(AsyncSocketTest, LifecycleObserverAttachRemoveMultiple) {
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::UniquePtr(new AsyncSocket(&evb));

  auto cb1 = std::make_unique<StrictMock<MockAsyncSocketLifecycleObserver>>();
  EXPECT_CALL(*cb1, observerAttachMock(socket.get()));
  socket->addLifecycleObserver(cb1.get());
  Mock::VerifyAndClearExpectations(cb1.get());
  EXPECT_THAT(socket->getLifecycleObservers(), UnorderedElementsAre(cb1.get()));

  auto cb2 = std::make_unique<StrictMock<MockAsyncSocketLifecycleObserver>>();
  EXPECT_CALL(*cb2, observerAttachMock(socket.get()));
  socket->addLifecycleObserver(cb2.get());
  Mock::VerifyAndClearExpectations(cb2.get());
  EXPECT_THAT(
      socket->getLifecycleObservers(),
      UnorderedElementsAre(cb1.get(), cb2.get()));

  EXPECT_CALL(*cb1, observerDetachMock(socket.get()));
  EXPECT_TRUE(socket->removeLifecycleObserver(cb1.get()));
  Mock::VerifyAndClearExpectations(cb1.get());
  EXPECT_THAT(socket->getLifecycleObservers(), UnorderedElementsAre(cb2.get()));

  EXPECT_CALL(*cb2, observerDetachMock(socket.get()));
  EXPECT_TRUE(socket->removeLifecycleObserver(cb2.get()));
  Mock::VerifyAndClearExpectations(cb2.get());
  EXPECT_THAT(socket->getLifecycleObservers(), IsEmpty());
}

TEST_P(AsyncSocketTest, LifecycleObserverAttachRemoveMultipleReverse) {
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::UniquePtr(new AsyncSocket(&evb));

  auto cb1 = std::make_unique<StrictMock<MockAsyncSocketLifecycleObserver>>();
  EXPECT_CALL(*cb1, observerAttachMock(socket.get()));
  socket->addLifecycleObserver(cb1.get());
  Mock::VerifyAndClearExpectations(cb1.get());
  EXPECT_THAT(socket->getLifecycleObservers(), UnorderedElementsAre(cb1.get()));

  auto cb2 = std::make_unique<StrictMock<MockAsyncSocketLifecycleObserver>>();
  EXPECT_CALL(*cb2, observerAttachMock(socket.get()));
  socket->addLifecycleObserver(cb2.get());
  Mock::VerifyAndClearExpectations(cb2.get());
  EXPECT_THAT(
      socket->getLifecycleObservers(),
      UnorderedElementsAre(cb1.get(), cb2.get()));

  EXPECT_CALL(*cb2, observerDetachMock(socket.get()));
  EXPECT_TRUE(socket->removeLifecycleObserver(cb2.get()));
  Mock::VerifyAndClearExpectations(cb2.get());
  EXPECT_THAT(socket->getLifecycleObservers(), UnorderedElementsAre(cb1.get()));

  EXPECT_CALL(*cb1, observerDetachMock(socket.get()));
  EXPECT_TRUE(socket->removeLifecycleObserver(cb1.get()));
  Mock::VerifyAndClearExpectations(cb1.get());
  EXPECT_THAT(socket->getLifecycleObservers(), IsEmpty());
}

TEST_P(AsyncSocketTest, LifecycleObserverRemoveMissing) {
  auto cb = std::make_unique<StrictMock<MockAsyncSocketLifecycleObserver>>();
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::UniquePtr(new AsyncSocket(&evb));
  EXPECT_FALSE(socket->removeLifecycleObserver(cb.get()));
}

TEST_P(AsyncSocketTest, LifecycleObserverMultipleAttachThenRemove) {
  auto cb1 = std::make_unique<StrictMock<MockAsyncSocketLifecycleObserver>>();
  auto cb2 = std::make_unique<StrictMock<MockAsyncSocketLifecycleObserver>>();
  TestServer server;

  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::UniquePtr(new AsyncSocket(&evb));
  EXPECT_CALL(*cb1, observerAttachMock(socket.get()));
  socket->addLifecycleObserver(cb1.get());
  EXPECT_THAT(socket->getLifecycleObservers(), UnorderedElementsAre(cb1.get()));
  Mock::VerifyAndClearExpectations(cb1.get());
  Mock::VerifyAndClearExpectations(cb2.get());

  EXPECT_CALL(*cb2, observerAttachMock(socket.get()));
  socket->addLifecycleObserver(cb2.get());
  EXPECT_THAT(
      socket->getLifecycleObservers(),
      UnorderedElementsAre(cb1.get(), cb2.get()));
  Mock::VerifyAndClearExpectations(cb1.get());
  Mock::VerifyAndClearExpectations(cb2.get());

  EXPECT_CALL(*cb2, observerDetachMock(socket.get()));
  EXPECT_TRUE(socket->removeLifecycleObserver(cb2.get()));
  EXPECT_THAT(socket->getLifecycleObservers(), UnorderedElementsAre(cb1.get()));
  Mock::VerifyAndClearExpectations(cb1.get());
  Mock::VerifyAndClearExpectations(cb2.get());

  EXPECT_CALL(*cb1, observerDetachMock(socket.get()));
  socket->removeLifecycleObserver(cb1.get());
  EXPECT_THAT(socket->getLifecycleObservers(), IsEmpty());
  Mock::VerifyAndClearExpectations(cb1.get());
  Mock::VerifyAndClearExpectations(cb2.get());
}

TEST_P(AsyncSocketTest, LifecycleObserverDetach) {
  auto cb = std::make_unique<StrictMock<MockAsyncSocketLifecycleObserver>>();
  TestServer server;

  EventBase& evb = getEventBase();
  auto socket1 = AsyncSocket::UniquePtr(new AsyncSocket(&evb));
  EXPECT_CALL(*cb, observerAttachMock(socket1.get()));
  socket1->addLifecycleObserver(cb.get());
  EXPECT_THAT(socket1->getLifecycleObservers(), UnorderedElementsAre(cb.get()));
  Mock::VerifyAndClearExpectations(cb.get());

  EXPECT_CALL(*cb, connectAttemptMock(socket1.get()));
  EXPECT_CALL(*cb, fdAttachMock(socket1.get()));
  EXPECT_CALL(*cb, connectSuccessMock(socket1.get()));
  socket1->connect(nullptr, server.getAddress(), 30);
  evb.loop();
  Mock::VerifyAndClearExpectations(cb.get());

  EXPECT_CALL(*cb, fdDetachMock(socket1.get()));
  auto fd = socket1->detachNetworkSocket();
  Mock::VerifyAndClearExpectations(cb.get());

  // create socket2, then immediately destroy it, should get no callbacks
  auto socket2 = AsyncSocket::UniquePtr(new AsyncSocket(&evb, fd));
  socket2 = nullptr;

  // finally, destroy socket1
  EXPECT_CALL(*cb, destroyMock(socket1.get()));
}

TEST_P(AsyncSocketTest, LifecycleObserverMoveResubscribe) {
  auto cb = std::make_unique<StrictMock<MockAsyncSocketLifecycleObserver>>();
  TestServer server;

  EventBase& evb = getEventBase();
  auto socket1 = AsyncSocket::UniquePtr(new AsyncSocket(&evb));
  EXPECT_CALL(*cb, observerAttachMock(socket1.get()));
  socket1->addLifecycleObserver(cb.get());
  EXPECT_THAT(socket1->getLifecycleObservers(), UnorderedElementsAre(cb.get()));
  Mock::VerifyAndClearExpectations(cb.get());

  EXPECT_CALL(*cb, connectAttemptMock(socket1.get()));
  EXPECT_CALL(*cb, fdAttachMock(socket1.get()));
  EXPECT_CALL(*cb, connectSuccessMock(socket1.get()));
  socket1->connect(nullptr, server.getAddress(), 30);
  evb.loop();
  Mock::VerifyAndClearExpectations(cb.get());

  AsyncSocket* socket2PtrCapturedmoved = nullptr;
  {
    InSequence s;
    EXPECT_CALL(*cb, fdDetachMock(socket1.get()));
    EXPECT_CALL(*cb, moveMock(socket1.get(), Not(socket1.get())))
        .WillOnce(Invoke(
            [&socket2PtrCapturedmoved, &cb](auto oldSocket, auto newSocket) {
              socket2PtrCapturedmoved = newSocket;
              EXPECT_CALL(*cb, observerDetachMock(oldSocket));
              EXPECT_CALL(*cb, observerAttachMock(newSocket));
              EXPECT_TRUE(oldSocket->removeLifecycleObserver(cb.get()));
              EXPECT_THAT(oldSocket->getLifecycleObservers(), IsEmpty());
              newSocket->addLifecycleObserver(cb.get());
              EXPECT_THAT(
                  newSocket->getLifecycleObservers(),
                  UnorderedElementsAre(cb.get()));
            }));
  }
  auto socket2 = AsyncSocket::UniquePtr(new AsyncSocket(std::move(socket1)));
  Mock::VerifyAndClearExpectations(cb.get());
  EXPECT_EQ(socket2.get(), socket2PtrCapturedmoved);

  {
    InSequence s;
    EXPECT_CALL(*cb, closeMock(socket2.get()));
    EXPECT_CALL(*cb, destroyMock(socket2.get()));
  }
  socket2 = nullptr;
}

TEST_P(AsyncSocketTest, LifecycleObserverMoveDoNotResubscribe) {
  auto cb = std::make_unique<StrictMock<MockAsyncSocketLifecycleObserver>>();
  TestServer server;

  EventBase& evb = getEventBase();
  auto socket1 = AsyncSocket::UniquePtr(new AsyncSocket(&evb));
  EXPECT_CALL(*cb, observerAttachMock(socket1.get()));
  socket1->addLifecycleObserver(cb.get());
  EXPECT_THAT(socket1->getLifecycleObservers(), UnorderedElementsAre(cb.get()));
  Mock::VerifyAndClearExpectations(cb.get());

  EXPECT_CALL(*cb, connectAttemptMock(socket1.get()));
  EXPECT_CALL(*cb, fdAttachMock(socket1.get()));
  EXPECT_CALL(*cb, connectSuccessMock(socket1.get()));
  socket1->connect(nullptr, server.getAddress(), 30);
  evb.loop();
  Mock::VerifyAndClearExpectations(cb.get());

  // close will not be called on socket1 because the fd is detached
  AsyncSocket* socket2PtrCapturedMoved = nullptr;
  InSequence s;
  EXPECT_CALL(*cb, fdDetachMock(socket1.get()));
  EXPECT_CALL(*cb, moveMock(socket1.get(), Not(socket1.get())))
      .WillOnce(Invoke(
          [&socket2PtrCapturedMoved](auto /* oldSocket */, auto newSocket) {
            socket2PtrCapturedMoved = newSocket;
          }));
  EXPECT_CALL(*cb, destroyMock(socket1.get()));
  auto socket2 = AsyncSocket::UniquePtr(new AsyncSocket(std::move(socket1)));
  Mock::VerifyAndClearExpectations(cb.get());
  EXPECT_EQ(socket2.get(), socket2PtrCapturedMoved);
}

TEST_P(AsyncSocketTest, LifecycleObserverDetachCallbackImmediately) {
  auto cb = std::make_unique<StrictMock<MockAsyncSocketLifecycleObserver>>();
  TestServer server;

  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  EXPECT_CALL(*cb, observerAttachMock(socket.get()));
  socket->addLifecycleObserver(cb.get());
  EXPECT_THAT(socket->getLifecycleObservers(), UnorderedElementsAre(cb.get()));
  Mock::VerifyAndClearExpectations(cb.get());

  EXPECT_CALL(*cb, observerDetachMock(socket.get()));
  EXPECT_TRUE(socket->removeLifecycleObserver(cb.get()));
  EXPECT_THAT(socket->getLifecycleObservers(), IsEmpty());
  Mock::VerifyAndClearExpectations(cb.get());

  // keep going to ensure no further callbacks
  socket->connect(nullptr, server.getAddress(), 30);
  evb.loop();
}

TEST_P(AsyncSocketTest, LifecycleObserverDetachCallbackAfterConnect) {
  auto cb = std::make_unique<StrictMock<MockAsyncSocketLifecycleObserver>>();
  TestServer server;

  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  EXPECT_CALL(*cb, observerAttachMock(socket.get()));
  socket->addLifecycleObserver(cb.get());
  Mock::VerifyAndClearExpectations(cb.get());

  EXPECT_CALL(*cb, connectAttemptMock(socket.get()));
  EXPECT_CALL(*cb, fdAttachMock(socket.get()));
  EXPECT_CALL(*cb, connectSuccessMock(socket.get()));
  socket->connect(nullptr, server.getAddress(), 30);
  evb.loop();
  Mock::VerifyAndClearExpectations(cb.get());

  EXPECT_CALL(*cb, observerDetachMock(socket.get()));
  EXPECT_TRUE(socket->removeLifecycleObserver(cb.get()));
  Mock::VerifyAndClearExpectations(cb.get());
}

TEST_P(AsyncSocketTest, LifecycleObserverDetachCallbackAfterClose) {
  auto cb = std::make_unique<StrictMock<MockAsyncSocketLifecycleObserver>>();
  TestServer server;

  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  EXPECT_CALL(*cb, observerAttachMock(socket.get()));
  socket->addLifecycleObserver(cb.get());
  Mock::VerifyAndClearExpectations(cb.get());

  EXPECT_CALL(*cb, connectAttemptMock(socket.get()));
  EXPECT_CALL(*cb, fdAttachMock(socket.get()));
  EXPECT_CALL(*cb, connectSuccessMock(socket.get()));
  socket->connect(nullptr, server.getAddress(), 30);
  evb.loop();
  Mock::VerifyAndClearExpectations(cb.get());

  EXPECT_CALL(*cb, closeMock(socket.get()));
  socket->closeNow();
  Mock::VerifyAndClearExpectations(cb.get());

  EXPECT_CALL(*cb, observerDetachMock(socket.get()));
  EXPECT_TRUE(socket->removeLifecycleObserver(cb.get()));
  Mock::VerifyAndClearExpectations(cb.get());
}

TEST_P(AsyncSocketTest, LifecycleObserverDetachCallbackcloseDuringDestroy) {
  auto cb = std::make_unique<StrictMock<MockAsyncSocketLifecycleObserver>>();
  TestServer server;

  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  EXPECT_CALL(*cb, observerAttachMock(socket.get()));
  socket->addLifecycleObserver(cb.get());
  Mock::VerifyAndClearExpectations(cb.get());

  EXPECT_CALL(*cb, connectAttemptMock(socket.get()));
  EXPECT_CALL(*cb, fdAttachMock(socket.get()));
  EXPECT_CALL(*cb, connectSuccessMock(socket.get()));
  socket->connect(nullptr, server.getAddress(), 30);
  evb.loop();
  Mock::VerifyAndClearExpectations(cb.get());

  InSequence s;
  EXPECT_CALL(*cb, closeMock(socket.get()))
      .WillOnce(Invoke([&cb](auto callbackSocket) {
        EXPECT_TRUE(callbackSocket->removeLifecycleObserver(cb.get()));
      }));
  EXPECT_CALL(*cb, observerDetachMock(socket.get()));
  socket = nullptr;
  Mock::VerifyAndClearExpectations(cb.get());
}

TEST_P(AsyncSocketTest, PreReceivedData) {
  TestServer server;

  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  socket->connect(nullptr, server.getAddress(), 30);
  evb.loop();

  socket->writeChain(nullptr, IOBuf::copyBuffer("hello"));

  auto acceptedSocket = server.acceptAsync(&evb);

  ReadCallback peekCallback(2);
  ReadCallback readCallback;
  peekCallback.dataAvailableCallback = [&]() {
    const auto peeked = peekCallback.dataRead();
    if (peeked < 2) {
      return;
    }
    peekCallback.verifyData("hello", peeked);
    if (GetParam() == BackendType::IO_URING) {
      acceptedSocket->setPreReceivedData(IOBuf::copyBuffer("hello", peeked));
    } else {
      acceptedSocket->setPreReceivedData(IOBuf::copyBuffer("h"));
      acceptedSocket->setPreReceivedData(IOBuf::copyBuffer("e"));
    }
    acceptedSocket->setReadCB(nullptr);
    acceptedSocket->setReadCB(&readCallback);
  };
  readCallback.dataAvailableCallback = [&]() {
    if (readCallback.dataRead() == 5) {
      readCallback.verifyData("hello", 5);
      acceptedSocket->setReadCB(nullptr);
      acceptedSocket.reset();
    }
  };

  acceptedSocket->setReadCB(&peekCallback);

  evb.loop();
}

class TrackingMovableReadCallback : public ReadCallback {
 public:
  using ReadCallback::ReadCallback;

  void readDataAvailable(size_t len) noexcept override {
    ++readDataAvailableCalls;
    ReadCallback::readDataAvailable(len);
  }

  void readBufferAvailable(std::unique_ptr<IOBuf> readBuf) noexcept override {
    ++readBufferAvailableCalls;
    maxReadBufferSize =
        std::max(maxReadBufferSize, readBuf->computeChainDataLength());
    ReadCallback::readBufferAvailable(std::move(readBuf));
  }

  size_t readDataAvailableCalls{0};
  size_t readBufferAvailableCalls{0};
  size_t maxReadBufferSize{0};
};

class TestAsyncDetachFdCallback : public AsyncDetachFdCallback {
 public:
  explicit TestAsyncDetachFdCallback(EventBase* evb = nullptr) : evb_(evb) {}

  void fdDetached(
      NetworkSocket socket,
      std::unique_ptr<IOBuf> pendingData) noexcept override {
    fd = socket;
    unread = std::move(pendingData);
    called = true;
    if (evb_) {
      evb_->terminateLoopSoon();
    }
  }

  void fdDetachFail(const AsyncSocketException& ex) noexcept override {
    error = std::make_unique<AsyncSocketException>(ex);
    called = true;
    if (evb_) {
      evb_->terminateLoopSoon();
    }
  }

  bool called{false};
  NetworkSocket fd;
  std::unique_ptr<IOBuf> unread;
  std::unique_ptr<AsyncSocketException> error;

 private:
  EventBase* evb_;
};

TEST_P(AsyncSocketTest, AsyncDetachFdRequiresEstablishedSocket) {
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::newSocket(&evb);
  TestAsyncDetachFdCallback callback;

  socket->asyncDetachFd(&callback);

  ASSERT_TRUE(callback.called);
  ASSERT_TRUE(callback.error);
  EXPECT_EQ(callback.error->getType(), AsyncSocketException::INVALID_STATE);
  EXPECT_EQ(callback.fd, NetworkSocket());
}

TEST_P(AsyncSocketTest, AsyncDetachFdWithoutNativeReceiveCompletesInline) {
  TestServer server;
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::newSocket(&evb);
  socket->connect(nullptr, server.getAddress(), 30);
  evb.loop();
  auto acceptedSocket = server.accept();

  TestAsyncDetachFdCallback callback;
  socket->asyncDetachFd(&callback);

  ASSERT_TRUE(callback.called);
  ASSERT_FALSE(callback.error);
  ASSERT_NE(callback.fd, NetworkSocket());
  EXPECT_EQ(socket->getNetworkSocket(), NetworkSocket());

  netops::close(callback.fd);
  callback.fd = NetworkSocket();
  acceptedSocket->close();
}

TEST_P(
    AsyncSocketIoUringTest, IoUringMovableCallbackHandsOffToMovableCallback) {
  TestServer server;
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::newSocket(&evb);
  socket->connect(nullptr, server.getAddress(), 30);
  evb.loop();
  auto acceptedSocket = server.accept();

  TrackingMovableReadCallback firstCallback(5);
  TrackingMovableReadCallback secondCallback;
  firstCallback.dataAvailableCallback = [&] {
    socket->setReadCB(nullptr);
    socket->setReadCB(&secondCallback);
    evb.terminateLoopSoon();
  };
  secondCallback.dataAvailableCallback = [&] {
    if (secondCallback.dataRead() == 6) {
      evb.terminateLoopSoon();
    }
  };

  socket->setReadCB(&firstCallback);
  const std::array<uint8_t, 5> first{'f', 'i', 'r', 's', 't'};
  acceptedSocket->write(first.data(), first.size());
  evb.loop();

  EXPECT_EQ(firstCallback.dataRead(), first.size());
  firstCallback.verifyData(first.data(), first.size());
  EXPECT_EQ(firstCallback.readDataAvailableCalls, 0);
  EXPECT_GT(firstCallback.readBufferAvailableCalls, 0);

  const std::array<uint8_t, 6> second{'s', 'e', 'c', 'o', 'n', 'd'};
  acceptedSocket->write(second.data(), second.size());
  evb.loop();

  EXPECT_EQ(secondCallback.dataRead(), 6);
  secondCallback.verifyData("second", 6);
  EXPECT_EQ(secondCallback.readDataAvailableCalls, 0);
  EXPECT_GT(secondCallback.readBufferAvailableCalls, 0);

  socket->close();
  acceptedSocket->close();
}

TEST_P(
    AsyncSocketDefaultBackendTest,
    SwitchToEventHandlerModeWithoutIoUringIsNoOp) {
  TestServer server;
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::newSocket(&evb);
  socket->connect(nullptr, server.getAddress(), 30);
  evb.loop();
  auto acceptedSocket = server.accept();

  ReadCallback readCallback;
  readCallback.dataAvailableCallback = [&] { socket->setReadCB(nullptr); };
  socket->setReadCB(&readCallback);

  auto pendingSwitch = socket->asyncSwitchToEventHandlerMode();
  ASSERT_TRUE(pendingSwitch.isReady());
  EXPECT_EQ(std::move(pendingSwitch).get(), nullptr);
  EXPECT_EQ(socket->getReadCallback(), &readCallback);

  const std::string payload = "hello";
  acceptedSocket->write(
      reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  evb.loop();
  readCallback.verifyData(payload.data(), payload.size());

  socket->close();
  acceptedSocket->close();
}

TEST_P(
    AsyncSocketIoUringTest,
    IoUringSwitchToEventHandlerModeRejectsInvalidState) {
  TestServer server(false, 4096);
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::newSocket(&evb);
  auto expectInvalidState = [&](const std::string& conditions) {
    auto pendingSwitch = socket->asyncSwitchToEventHandlerMode();
    ASSERT_TRUE(pendingSwitch.isReady());
    ASSERT_TRUE(pendingSwitch.hasException());
    const auto* exception =
        pendingSwitch.result()
            .exception()
            .get_exception<AsyncSocketException>();
    ASSERT_NE(exception, nullptr);
    EXPECT_EQ(exception->getType(), AsyncSocketException::INVALID_STATE);
    EXPECT_THAT(exception->what(), HasSubstr(conditions));
  };

  expectInvalidState(
      "established=false, pending connect=false, pending writes=false");

  ConnCallback connectCallback;
  SocketOptionMap options{{{SOL_SOCKET, SO_SNDBUF}, 4096}};
  socket->connect(&connectCallback, server.getAddress(), 30, options);
  ASSERT_EQ(connectCallback.state, STATE_WAITING);
  expectInvalidState(
      "established=false, pending connect=true, pending writes=false");
  evb.loop();
  ASSERT_EQ(connectCallback.state, STATE_SUCCEEDED);
  auto acceptedSocket = server.accept();

  ReadCallback readCallback;
  socket->setReadCB(&readCallback);
  WriteCallback writeCallback;
  const std::string payload(1024 * 1024, 'w');
  socket->write(&writeCallback, payload.data(), payload.size());
  ASSERT_EQ(writeCallback.state, STATE_WAITING);
  expectInvalidState(
      "established=true, pending connect=false, pending writes=true");
  EXPECT_TRUE(socket->good());
  EXPECT_EQ(socket->getReadCallback(), &readCallback);
  EXPECT_EQ(writeCallback.state, STATE_WAITING);

  socket->closeNow();
  acceptedSocket->close();
  evb.loop();
}

TEST_P(AsyncSocketIoUringTest, IoUringAsyncDetachFdKeepsInFlightBytes) {
  TestServer server;
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::newSocket(&evb);
  socket->connect(nullptr, server.getAddress(), 30);
  evb.loop();
  auto acceptedSocket = server.accept();

  TrackingMovableReadCallback nativeCallback;
  socket->setReadCB(&nativeCallback);
  evb.loopOnce(EVLOOP_NONBLOCK);

  std::string payload(8192, '\0');
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<char>('a' + i % 26);
  }
  acceptedSocket->write(
      reinterpret_cast<const uint8_t*>(payload.data()), payload.size());

  TestAsyncDetachFdCallback detachCallback(&evb);
  socket->asyncDetachFd(&detachCallback);
  EXPECT_FALSE(detachCallback.called);
  evb.loop();

  ASSERT_TRUE(detachCallback.called);
  ASSERT_FALSE(detachCallback.error);
  ASSERT_NE(detachCallback.fd, NetworkSocket());
  EXPECT_EQ(socket->getNetworkSocket(), NetworkSocket());
  EXPECT_EQ(nativeCallback.dataRead(), 0);

  auto detachedSocket = AsyncSocket::newSocket(
      &evb, std::exchange(detachCallback.fd, NetworkSocket()));
  if (detachCallback.unread) {
    detachedSocket->setPreReceivedData(std::move(detachCallback.unread));
  }

  ReadCallback detachedCallback;
  detachedCallback.dataAvailableCallback = [&] {
    if (detachedCallback.dataRead() == payload.size()) {
      detachedSocket->setReadCB(nullptr);
      evb.terminateLoopSoon();
    }
  };
  detachedSocket->setReadCB(&detachedCallback);
  if (detachedCallback.dataRead() < payload.size()) {
    evb.loop();
  }
  detachedCallback.verifyData(payload.data(), payload.size());

  detachedSocket->close();
  acceptedSocket->close();
}

TEST_P(
    AsyncSocketIoUringTest, IoUringSwitchToEventHandlerModeKeepsInFlightBytes) {
  TestServer server;
  EventBase& evb = getEventBase();
  auto socket = AsyncSocket::newSocket(&evb);
  socket->connect(nullptr, server.getAddress(), 30);
  evb.loop();
  auto acceptedSocket = server.accept();

  TrackingMovableReadCallback nativeCallback;
  socket->setReadCB(&nativeCallback);
  evb.loopOnce(EVLOOP_NONBLOCK);

  std::string payload(8192, '\0');
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<char>('a' + i % 26);
  }
  acceptedSocket->write(
      reinterpret_cast<const uint8_t*>(payload.data()), payload.size());

  const auto fd = socket->getNetworkSocket();
  const auto originalFlags = fcntl(fd.toFd(), F_GETFL, 0);
  ASSERT_NE(originalFlags, -1);
  EXPECT_EQ(originalFlags & O_NONBLOCK, 0);
  auto pendingSwitch = socket->asyncSwitchToEventHandlerMode();
  EXPECT_FALSE(pendingSwitch.isReady());
  EXPECT_EQ(fcntl(fd.toFd(), F_GETFL, 0), originalFlags);
  auto drained = std::move(pendingSwitch).via(&evb).getVia(&evb);
  EXPECT_EQ(fcntl(fd.toFd(), F_GETFL, 0), originalFlags);
  ASSERT_EQ(netops::set_socket_non_blocking(fd), 0);
  EXPECT_EQ(nativeCallback.dataRead(), 0);
  if (drained) {
    socket->setPreReceivedData(std::move(drained));
  }

  ReadCallback readinessCallback;
  readinessCallback.dataAvailableCallback = [&] {
    if (readinessCallback.dataRead() == payload.size()) {
      socket->setReadCB(nullptr);
      evb.terminateLoopSoon();
    }
  };
  socket->setReadCB(&readinessCallback);
  if (readinessCallback.dataRead() < payload.size()) {
    evb.loop();
  }
  readinessCallback.verifyData(payload.data(), payload.size());

  socket->close();
  acceptedSocket->close();
}

TEST_P(AsyncSocketTest, PreReceivedDataOnly) {
  TestServer server;

  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  socket->connect(nullptr, server.getAddress(), 30);
  evb.loop();

  socket->writeChain(nullptr, IOBuf::copyBuffer("hello"));

  auto acceptedSocket = server.acceptAsync(&evb);

  ReadCallback peekCallback;
  ReadCallback readCallback;
  peekCallback.dataAvailableCallback = [&]() {
    peekCallback.verifyData("hello", 5);
    acceptedSocket->setPreReceivedData(IOBuf::copyBuffer("hello"));
    EXPECT_TRUE(acceptedSocket->readable());
    acceptedSocket->setReadCB(nullptr);
    acceptedSocket->setReadCB(&readCallback);
  };
  readCallback.dataAvailableCallback = [&]() {
    readCallback.verifyData("hello", 5);
    acceptedSocket->setReadCB(nullptr);
    acceptedSocket.reset();
  };

  acceptedSocket->setReadCB(&peekCallback);

  evb.loop();
}

TEST_P(AsyncSocketTest, PreReceivedDataPartial) {
  TestServer server;

  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  socket->connect(nullptr, server.getAddress(), 30);
  evb.loop();

  socket->writeChain(nullptr, IOBuf::copyBuffer("hello"));

  auto acceptedSocket = server.acceptAsync(&evb);

  ReadCallback peekCallback;
  TrackingMovableReadCallback smallReadCallback(3);
  ReadCallback normalReadCallback;
  peekCallback.dataAvailableCallback = [&]() {
    peekCallback.verifyData("hello", 5);
    acceptedSocket->setPreReceivedData(IOBuf::copyBuffer("hello"));
    acceptedSocket->setReadCB(&smallReadCallback);
  };
  smallReadCallback.dataAvailableCallback = [&]() {
    if (GetParam() == BackendType::IO_URING) {
      smallReadCallback.verifyData("hello", 5);
    } else {
      smallReadCallback.verifyData("hel", 3);
    }
    acceptedSocket->setReadCB(nullptr);
    if (GetParam() == BackendType::IO_URING) {
      acceptedSocket->setPreReceivedData(IOBuf::copyBuffer("lo"));
    }
    acceptedSocket->setReadCB(&normalReadCallback);
  };
  normalReadCallback.dataAvailableCallback = [&]() {
    normalReadCallback.verifyData("lo", 2);
    acceptedSocket->setReadCB(nullptr);
    acceptedSocket.reset();
  };

  acceptedSocket->setReadCB(&peekCallback);

  evb.loop();
}

TEST_P(AsyncSocketTest, PreReceivedDataTakeover) {
  TestServer server;

  EventBase& evb = getEventBase();
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);
  socket->connect(nullptr, server.getAddress(), 30);
  evb.loop();

  socket->writeChain(nullptr, IOBuf::copyBuffer("hello"));

  auto fd = server.acceptFD();
  SocketAddress peerAddress;
  peerAddress.setFromPeerAddress(fd);
  auto acceptedSocket =
      AsyncSocket::UniquePtr(new AsyncSocket(&evb, fd, 0, &peerAddress));
  AsyncSocket::UniquePtr takeoverSocket;

  ReadCallback peekCallback(3);
  ReadCallback readCallback;
  peekCallback.dataAvailableCallback = [&]() {
    const auto peeked = peekCallback.dataRead();
    if (peeked < 3) {
      return;
    }
    peekCallback.verifyData("hello", peeked);
    acceptedSocket->setPreReceivedData(IOBuf::copyBuffer("hello", peeked));
    acceptedSocket->setReadCB(nullptr);
    takeoverSocket =
        AsyncSocket::UniquePtr(new AsyncSocket(std::move(acceptedSocket)));
    takeoverSocket->setReadCB(&readCallback);
  };
  readCallback.dataAvailableCallback = [&]() {
    if (readCallback.dataRead() < 5) {
      return;
    }
    readCallback.verifyData("hello", 5);
    takeoverSocket->setReadCB(nullptr);
  };

  acceptedSocket->setReadCB(&peekCallback);

  evb.loop();
  // Verify we can still get the peer address after the peer socket is reset.
  socket->closeWithReset();
  evb.loopOnce();
  SocketAddress socketPeerAddress;
  takeoverSocket->getPeerAddress(&socketPeerAddress);
  EXPECT_EQ(socketPeerAddress, peerAddress);
}

#ifdef MSG_NOSIGNAL
TEST(AsyncSocketTest, SendMessageFlags) {
  TestServer server;
  TestSendMsgParamsCallback sendMsgCB(
      MSG_DONTWAIT | MSG_NOSIGNAL | MSG_MORE, 0, nullptr);

  // connect()
  EventBase evb;
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);

  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);
  std::shared_ptr<BlockingSocket> acceptedSocket = server.accept();

  evb.loop();
  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);

  // Set SendMsgParamsCallback
  socket->setSendMsgParamCB(&sendMsgCB);
  ASSERT_EQ(socket->getSendMsgParamsCB(), &sendMsgCB);

  // Write the first portion of data. This data is expected to be
  // sent out immediately.
  std::vector<uint8_t> buf(128, 'a');
  WriteCallback wcb;
  sendMsgCB.reset(MSG_DONTWAIT | MSG_NOSIGNAL);
  socket->write(&wcb, buf.data(), buf.size());
  ASSERT_EQ(wcb.state, STATE_SUCCEEDED);
  ASSERT_TRUE(sendMsgCB.queriedFlags_);
  ASSERT_FALSE(sendMsgCB.queriedData_);

  // Using different flags for the second write operation.
  // MSG_MORE flag is expected to delay sending this
  // data to the wire.
  sendMsgCB.reset(MSG_DONTWAIT | MSG_NOSIGNAL | MSG_MORE);
  socket->write(&wcb, buf.data(), buf.size());
  ASSERT_EQ(wcb.state, STATE_SUCCEEDED);
  ASSERT_TRUE(sendMsgCB.queriedFlags_);
  ASSERT_FALSE(sendMsgCB.queriedData_);

  // Make sure the accepted socket saw only the data from
  // the first write request.
  std::vector<uint8_t> readbuf(2 * buf.size());
  uint32_t bytesRead = acceptedSocket->read(readbuf.data(), readbuf.size());
  ASSERT_TRUE(std::equal(buf.begin(), buf.end(), readbuf.begin()));
  ASSERT_EQ(bytesRead, buf.size());

  // Make sure the server got a connection and received the data
  acceptedSocket->close();
  socket->close();

  ASSERT_TRUE(socket->isClosedBySelf());
  ASSERT_FALSE(socket->isClosedByPeer());
}

TEST(AsyncSocketTest, SendMessageAncillaryData) {
  NetworkSocket fds[2];
  EXPECT_EQ(netops::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  // "Client" socket
  auto cfd = fds[0];
  ASSERT_NE(cfd, NetworkSocket());

  // "Server" socket
  auto sfd = fds[1];
  ASSERT_NE(sfd, NetworkSocket());
  SCOPE_EXIT {
    netops::close(sfd);
  };

  // Instantiate AsyncSocket object for the connected socket
  EventBase evb;
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb, cfd);

  // Open a temporary file and write a magic string to it
  // We'll transfer the file handle to test the message parameters
  // callback logic.
  TemporaryFile file(
      StringPiece(), fs::path(), TemporaryFile::Scope::UNLINK_IMMEDIATELY);
  int tmpfd = file.fd();
  ASSERT_NE(tmpfd, -1) << "Failed to open a temporary file";
  std::string magicString("Magic string");
  ASSERT_EQ(
      fileops::write(tmpfd, magicString.c_str(), magicString.length()),
      magicString.length());

  // Send message
  union {
    // Space large enough to hold an 'int'
    char control[CMSG_SPACE(sizeof(int))];
    struct cmsghdr cmh;
  } s_u;
  s_u.cmh.cmsg_len = CMSG_LEN(sizeof(int));
  s_u.cmh.cmsg_level = SOL_SOCKET;
  s_u.cmh.cmsg_type = SCM_RIGHTS;
  memcpy(CMSG_DATA(&s_u.cmh), &tmpfd, sizeof(int));

  // Set up the callback providing message parameters
  TestSendMsgParamsCallback sendMsgCB(
      MSG_DONTWAIT | MSG_NOSIGNAL, sizeof(s_u.control), s_u.control);
  socket->setSendMsgParamCB(&sendMsgCB);

  // We must transmit at least 1 byte of real data in order
  // to send ancillary data
  int s_data = 12345;
  WriteCallback wcb;
  auto ioBuf = folly::IOBuf::wrapBuffer(&s_data, sizeof(s_data));
  sendMsgCB.expectedTag_ = folly::AsyncSocket::WriteRequestTag{
      ioBuf.get()}; // Also test write tagging.
  ASSERT_FALSE(sendMsgCB.tagLastWritten_.has_value());
  socket->writeChain(&wcb, std::move(ioBuf));
  ASSERT_EQ(wcb.state, STATE_SUCCEEDED);
  ASSERT_TRUE(sendMsgCB.queriedData_); // Did the tag check run?
  ASSERT_EQ(sendMsgCB.expectedTag_, *sendMsgCB.tagLastWritten_);

  // Receive the message
  union {
    // Space large enough to hold an 'int'
    char control[CMSG_SPACE(sizeof(int))];
    struct cmsghdr cmh;
  } r_u;
  struct msghdr msgh;
  struct iovec iov;
  int r_data = 0;

  msgh.msg_control = r_u.control;
  msgh.msg_controllen = sizeof(r_u.control);
  msgh.msg_name = nullptr;
  msgh.msg_namelen = 0;
  msgh.msg_iov = &iov;
  msgh.msg_iovlen = 1;
  iov.iov_base = &r_data;
  iov.iov_len = sizeof(r_data);

  // Receive data
  ASSERT_NE(netops::recvmsg(sfd, &msgh, 0), -1) << "recvmsg failed: " << errno;

  // Validate the received message
  ASSERT_EQ(r_u.cmh.cmsg_len, CMSG_LEN(sizeof(int)));
  ASSERT_EQ(r_u.cmh.cmsg_level, SOL_SOCKET);
  ASSERT_EQ(r_u.cmh.cmsg_type, SCM_RIGHTS);
  ASSERT_EQ(r_data, s_data);
  int fd = 0;
  memcpy(&fd, CMSG_DATA(&r_u.cmh), sizeof(int));
  ASSERT_NE(fd, 0);
  SCOPE_EXIT {
    fileops::close(fd);
  };

  std::vector<uint8_t> transferredMagicString(magicString.length() + 1, 0);

  // Reposition to the beginning of the file
  ASSERT_EQ(0, lseek(fd, 0, SEEK_SET));

  // Read the magic string back, and compare it with the original
  ASSERT_EQ(
      magicString.length(),
      folly::fileops::read(
          fd, transferredMagicString.data(), transferredMagicString.size()));
  ASSERT_TRUE(
      std::equal(
          magicString.begin(),
          magicString.end(),
          transferredMagicString.begin()));
}

namespace {

// Child classes of AsyncSocket (e.g. AsyncFdSocket) want to be able to
// fail reads from the read ancillary data or regular read callback. Test this.
struct FailableSocket : public AsyncSocket {
  FailableSocket(EventBase* evb, NetworkSocket fd) : AsyncSocket(evb, fd) {}
  void testFailRead() {
    AsyncSocketException ex(
        AsyncSocketException::INTERNAL_ERROR, "FailableSocket::testFailRead");
    AsyncSocket::failRead(__func__, ex);
  }
};

class TruncateAncillaryDataAndCallFn
    : public folly::AsyncSocket::ReadAncillaryDataCallback {
 public:
  explicit TruncateAncillaryDataAndCallFn(VoidCallback cob)
      : callback_(std::move(cob)) {}

  folly::Expected<folly::Unit, AsyncSocketException> ancillaryData(
      struct msghdr& msg) noexcept override {
    sawCtrunc_ = sawCtrunc_ || (msg.msg_flags & MSG_CTRUNC);
    callback_();

    return folly::unit;
  }
  folly::MutableByteRange getAncillaryDataCtrlBuffer() override {
    return folly::MutableByteRange(ancillaryDataCtrlBuffer_);
  }

  bool sawCtrunc_{false};

 private:
  VoidCallback callback_;
  // Empty to trigger MSG_CTRUNC
  std::array<uint8_t, 0> ancillaryDataCtrlBuffer_;
};

// Returns the error string from the read callback (can be "none")
std::string testTruncateAncillaryDataAndCall(
    std::function<void(FailableSocket*)> fn,
    std::function<void(FailableSocket*)> postConditionCheck) {
  NetworkSocket fds[2];
  CHECK_EQ(netops::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  EventBase evb;
  std::shared_ptr<AsyncSocket> sendSock = AsyncSocket::newSocket(&evb, fds[0]);
  ReadCallback rcb; // outlives socket since ~AsyncSocket calls rcb.readEOF
  FailableSocket recvSock(&evb, fds[1]);

  TruncateAncillaryDataAndCallFn ancillaryCob{[&]() { fn(&recvSock); }};
  recvSock.setReadAncillaryDataCB(&ancillaryCob);

  // Send the stderr FD with ancillary data
  int tmpfd = 2;
  union { // `man cmsg` suggests this idiom for a "large enough" `cmsghdr`
    char buf[CMSG_SPACE(sizeof(tmpfd))];
    struct cmsghdr cmh;
  } u;
  u.cmh.cmsg_len = CMSG_LEN(sizeof(tmpfd));
  u.cmh.cmsg_level = SOL_SOCKET;
  u.cmh.cmsg_type = SCM_RIGHTS;
  memcpy(CMSG_DATA(&u.cmh), &tmpfd, sizeof(tmpfd));

  TestSendMsgParamsCallback sendMsgCB(
      MSG_DONTWAIT | MSG_NOSIGNAL, sizeof(u.buf), u.buf);
  sendSock->setSendMsgParamCB(&sendMsgCB);

  // Transmit at least 1 byte of real data to send ancillary data
  int s_data = 12345;
  WriteCallback wcb;
  sendSock->write(&wcb, &s_data, sizeof(s_data));
  CHECK_EQ(wcb.state, STATE_SUCCEEDED);

  // The FD will be discarded (MSG_CTRUNC) since our ancillary data callback
  // deliberately misconfigures the `recvmsg`.
  recvSock.setReadCB(&rcb);
  CHECK(!ancillaryCob.sawCtrunc_);
  evb.loopOnce();

  // Ensure that `ancillaryData()` actually ran, and saw the error condition.
  CHECK(ancillaryCob.sawCtrunc_);
  postConditionCheck(&recvSock);

  return rcb.exception.what();
}

} // namespace

// These tests do double-duty:
//  - show that `ReadAncillaryDataCallback` can safely close or fail a socket
//  - exercise getting & handling `MSG_CTRUNC`

TEST(AsyncSocketTest, ReceiveTruncatedAncillaryDataAndFail) {
  EXPECT_THAT(
      testTruncateAncillaryDataAndCall(
          [](FailableSocket* sock) { sock->testFailRead(); },
          [](FailableSocket* sock) { ASSERT_TRUE(sock->error()); }),
      testing::HasSubstr("FailableSocket::testFailRead"));
}

TEST(AsyncSocketTest, ReceiveTruncatedAncillaryDataAndClose) {
  EXPECT_THAT(
      testTruncateAncillaryDataAndCall(
          [](FailableSocket* sock) { sock->close(); },
          [](FailableSocket* sock) { ASSERT_TRUE(sock->isClosedBySelf()); }),
      testing::HasSubstr("AsyncSocketException: none, type =")); // no error
}

TEST(AsyncSocketTest, ReceiveTruncatedAncillaryDataUnhandled) {
  // Since this `ancillaryData` fails to check MSG_CTRUNG, the last-ditch
  // check in `AsyncSocket::processNormalRead` will fire.
  EXPECT_THAT(
      testTruncateAncillaryDataAndCall(
          [](FailableSocket*) {},
          [](FailableSocket* sock) { ASSERT_TRUE(sock->error()); }),
      testing::HasSubstr("recvmsg() got MSG_CTRUNC"));
}

TEST(AsyncSocketTest, UnixDomainSocketErrMessageCB) {
  // Linux AF_UNIX ignores MSG_ERRQUEUE, so recvmsg() consumes ordinary data.
  // Verify that AsyncSocket declines the callback and that the behavior it
  // protects against is still present.

  NetworkSocket fd[2];
  EXPECT_EQ(netops::socketpair(AF_UNIX, SOCK_STREAM, 0, fd), 0);
  ASSERT_NE(fd[0], NetworkSocket());
  ASSERT_NE(fd[1], NetworkSocket());
  SCOPE_EXIT {
    netops::close(fd[1]);
  };

  EXPECT_EQ(netops::set_socket_non_blocking(fd[0]), 0);
  EXPECT_EQ(netops::set_socket_non_blocking(fd[1]), 0);

  EventBase evb;
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb, fd[0]);

  // setErrMessageCB should fail for unix domain socket
  TestErrMessageCallback errMsgCB;
  ASSERT_NE(&errMsgCB, nullptr);
  socket->setErrMessageCB(&errMsgCB);
  ASSERT_EQ(socket->getErrMessageCallback(), nullptr);

#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  // The following verifies that MSG_ERRQUEUE does not work for UDS,
  // and recvmsg reads application data
  union {
    // Space large enough to hold an 'int'
    char control[CMSG_SPACE(sizeof(int))];
    struct cmsghdr cmh;
  } r_u;
  struct msghdr msgh;
  struct iovec iov;
  int recv_data = 0;

  msgh.msg_control = r_u.control;
  msgh.msg_controllen = sizeof(r_u.control);
  msgh.msg_name = nullptr;
  msgh.msg_namelen = 0;
  msgh.msg_iov = &iov;
  msgh.msg_iovlen = 1;
  iov.iov_base = &recv_data;
  iov.iov_len = sizeof(recv_data);

  // there is no data, recvmsg should fail
  EXPECT_EQ(netops::recvmsg(fd[1], &msgh, MSG_ERRQUEUE), -1);
  EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);

  // provide some application data, error queue should be empty if it exists
  // However, UDS reads application data as error message
  int test_data = 123456;
  WriteCallback wcb;
  socket->write(&wcb, &test_data, sizeof(test_data));
  recv_data = 0;
  ASSERT_NE(netops::recvmsg(fd[1], &msgh, MSG_ERRQUEUE), -1);
  ASSERT_EQ(recv_data, test_data);
#endif // FOLLY_HAVE_MSG_ERRQUEUE
}

TEST_P(AsyncSocketTest, V6TosReflectTest) {
  EventBase& eventBase = getEventBase();

  // Create a server socket
  std::shared_ptr<AsyncServerSocket> serverSocket(
      AsyncServerSocket::newSocket(&eventBase));
  folly::IPAddress ip("::1");
  std::vector<folly::IPAddress> serverIp;
  serverIp.push_back(ip);
  serverSocket->bind(serverIp, 0);
  serverSocket->listen(16);
  folly::SocketAddress serverAddress;
  serverSocket->getAddress(&serverAddress);

  // Enable TOS reflect
  serverSocket->setTosReflect(true);

  // Add a callback to accept one connection then stop the loop
  TestAcceptCallback acceptCallback;
  acceptCallback.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {
        serverSocket->removeAcceptCallback(&acceptCallback, &eventBase);
      });
  acceptCallback.setAcceptErrorFn([&](const std::exception& /* ex */) {
    serverSocket->removeAcceptCallback(&acceptCallback, &eventBase);
  });
  serverSocket->addAcceptCallback(&acceptCallback, &eventBase);
  serverSocket->startAccepting();

  // Create a client socket, setsockopt() the TOS before connecting
  auto clientThread =
      [](std::shared_ptr<AsyncSocket>& clientSock,
         ConnCallback* ccb,
         EventBase* evb,
         folly::SocketAddress sAddr) {
        clientSock = AsyncSocket::newSocket(evb);
        SocketOptionKey v6Opts = {IPPROTO_IPV6, IPV6_TCLASS};
        SocketOptionMap optionMap;
        optionMap.insert({v6Opts, 0x2c});
        SocketAddress bindAddr("0.0.0.0", 0);
        clientSock->connect(ccb, sAddr, 30, optionMap, bindAddr);
      };

  std::shared_ptr<AsyncSocket> socket(nullptr);
  ConnCallback cb;
  clientThread(socket, &cb, &eventBase, serverAddress);

  eventBase.loop();

  // Verify if the connection is accepted and if the accepted socket has
  // setsockopt on the TOS for the same value that was on the client socket
  auto fd = acceptCallback.getEvents()->at(1).fd;
  ASSERT_NE(fd, NetworkSocket());
  int value;
  socklen_t valueLength = sizeof(value);
  int rc =
      netops::getsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &value, &valueLength);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(value, 0x2c);

  // Additional Test for ConnectCallback without bindAddr
  serverSocket->addAcceptCallback(&acceptCallback, &eventBase);
  serverSocket->startAccepting();

  auto newClientSock = AsyncSocket::newSocket(&eventBase);
  TestConnectCallback callback;
  // connect call will not set this SO_REUSEADDR if we do not
  // pass the bindAddress in its call; so we can safely verify this.
  newClientSock->connect(&callback, serverAddress, 30);

  // Collect events
  eventBase.loop();

  auto acceptedFd = acceptCallback.getEvents()->at(1).fd;
  ASSERT_NE(acceptedFd, NetworkSocket());
  int reuseAddrVal;
  socklen_t reuseAddrValLen = sizeof(reuseAddrVal);
  // Get the socket created underneath connect call of AsyncSocket
  auto usedSockFd = newClientSock->getNetworkSocket();
  int getOptRet = netops::getsockopt(
      usedSockFd, SOL_SOCKET, SO_REUSEADDR, &reuseAddrVal, &reuseAddrValLen);
  ASSERT_EQ(getOptRet, 0);
  ASSERT_EQ(reuseAddrVal, 1 /* configured through preConnect*/);
}

TEST_P(AsyncSocketTest, V4TosReflectTest) {
  EventBase& eventBase = getEventBase();

  // Create a server socket
  std::shared_ptr<AsyncServerSocket> serverSocket(
      AsyncServerSocket::newSocket(&eventBase));
  folly::IPAddress ip("127.0.0.1");
  std::vector<folly::IPAddress> serverIp;
  serverIp.push_back(ip);
  serverSocket->bind(serverIp, 0);
  serverSocket->listen(16);
  folly::SocketAddress serverAddress;
  serverSocket->getAddress(&serverAddress);

  // Enable TOS reflect
  serverSocket->setTosReflect(true);

  // Add a callback to accept one connection then stop the loop
  TestAcceptCallback acceptCallback;
  acceptCallback.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {
        serverSocket->removeAcceptCallback(&acceptCallback, &eventBase);
      });
  acceptCallback.setAcceptErrorFn([&](const std::exception& /* ex */) {
    serverSocket->removeAcceptCallback(&acceptCallback, &eventBase);
  });
  serverSocket->addAcceptCallback(&acceptCallback, &eventBase);
  serverSocket->startAccepting();

  // Create a client socket, setsockopt() the TOS before connecting
  auto clientThread =
      [](std::shared_ptr<AsyncSocket>& clientSock,
         ConnCallback* ccb,
         EventBase* evb,
         folly::SocketAddress sAddr) {
        clientSock = AsyncSocket::newSocket(evb);
        SocketOptionKey v4Opts = {IPPROTO_IP, IP_TOS};
        SocketOptionMap optionMap;
        optionMap.insert({v4Opts, 0x2c});
        SocketAddress bindAddr("0.0.0.0", 0);
        clientSock->connect(ccb, sAddr, 30, optionMap, bindAddr);
      };

  std::shared_ptr<AsyncSocket> socket(nullptr);
  ConnCallback cb;
  clientThread(socket, &cb, &eventBase, serverAddress);

  eventBase.loop();

  // Verify if the connection is accepted and if the accepted socket has
  // setsockopt on the TOS for the same value that was on the client socket
  auto fd = acceptCallback.getEvents()->at(1).fd;
  ASSERT_NE(fd, NetworkSocket());
  int value;
  socklen_t valueLength = sizeof(value);
  int rc = netops::getsockopt(fd, IPPROTO_IP, IP_TOS, &value, &valueLength);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(value, 0x2c);
}

TEST_P(AsyncSocketTest, V6AcceptedTosTest) {
  EventBase& eventBase = getEventBase();

  // This test verifies if the ListenerTos set on a socket is
  // propagated properly to accepted socket connections

  // Create a server socket
  std::shared_ptr<AsyncServerSocket> serverSocket(
      AsyncServerSocket::newSocket(&eventBase));
  folly::IPAddress ip("::1");
  std::vector<folly::IPAddress> serverIp;
  serverIp.push_back(ip);
  serverSocket->bind(serverIp, 0);
  serverSocket->listen(16);
  folly::SocketAddress serverAddress;
  serverSocket->getAddress(&serverAddress);

  // Set listener TOS to 0x74 i.e. dscp 29
  serverSocket->setListenerTos(0x74);

  // Add a callback to accept one connection then stop the loop
  TestAcceptCallback acceptCallback;
  acceptCallback.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {
        serverSocket->removeAcceptCallback(&acceptCallback, &eventBase);
      });
  acceptCallback.setAcceptErrorFn([&](const std::exception& /* ex */) {
    serverSocket->removeAcceptCallback(&acceptCallback, &eventBase);
  });
  serverSocket->addAcceptCallback(&acceptCallback, &eventBase);
  serverSocket->startAccepting();

  // Create a client socket, setsockopt() the TOS before connecting
  auto clientThread =
      [](std::shared_ptr<AsyncSocket>& clientSock,
         ConnCallback* ccb,
         EventBase* evb,
         folly::SocketAddress sAddr) {
        clientSock = AsyncSocket::newSocket(evb);
        SocketOptionKey v6Opts = {IPPROTO_IPV6, IPV6_TCLASS};
        SocketOptionMap optionMap;
        optionMap.insert({v6Opts, 0x2c});
        SocketAddress bindAddr("0.0.0.0", 0);
        clientSock->connect(ccb, sAddr, 30, optionMap, bindAddr);
      };

  std::shared_ptr<AsyncSocket> socket(nullptr);
  ConnCallback cb;
  clientThread(socket, &cb, &eventBase, serverAddress);

  eventBase.loop();

  // Verify if the connection is accepted and if the accepted socket has
  // setsockopt on the TOS for the same value that the listener was set to
  auto fd = acceptCallback.getEvents()->at(1).fd;
  ASSERT_NE(fd, NetworkSocket());
  int value;
  socklen_t valueLength = sizeof(value);
  int rc =
      netops::getsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &value, &valueLength);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(value, 0x74);
}

TEST_P(AsyncSocketTest, V4AcceptedTosTest) {
  EventBase& eventBase = getEventBase();

  // This test verifies if the ListenerTos set on a socket is
  // propagated properly to accepted socket connections

  // Create a server socket
  std::shared_ptr<AsyncServerSocket> serverSocket(
      AsyncServerSocket::newSocket(&eventBase));
  folly::IPAddress ip("127.0.0.1");
  std::vector<folly::IPAddress> serverIp;
  serverIp.push_back(ip);
  serverSocket->bind(serverIp, 0);
  serverSocket->listen(16);
  folly::SocketAddress serverAddress;
  serverSocket->getAddress(&serverAddress);

  // Set listener TOS to 0x74 i.e. dscp 29
  serverSocket->setListenerTos(0x74);

  // Add a callback to accept one connection then stop the loop
  TestAcceptCallback acceptCallback;
  acceptCallback.setConnectionAcceptedFn(
      [&](NetworkSocket /* fd */, const folly::SocketAddress& /* addr */) {
        serverSocket->removeAcceptCallback(&acceptCallback, &eventBase);
      });
  acceptCallback.setAcceptErrorFn([&](const std::exception& /* ex */) {
    serverSocket->removeAcceptCallback(&acceptCallback, &eventBase);
  });
  serverSocket->addAcceptCallback(&acceptCallback, &eventBase);
  serverSocket->startAccepting();

  // Create a client socket, setsockopt() the TOS before connecting
  auto clientThread =
      [](std::shared_ptr<AsyncSocket>& clientSock,
         ConnCallback* ccb,
         EventBase* evb,
         folly::SocketAddress sAddr) {
        clientSock = AsyncSocket::newSocket(evb);
        SocketOptionKey v4Opts = {IPPROTO_IP, IP_TOS};
        SocketOptionMap optionMap;
        optionMap.insert({v4Opts, 0x2c});
        SocketAddress bindAddr("0.0.0.0", 0);
        clientSock->connect(ccb, sAddr, 30, optionMap, bindAddr);
      };

  std::shared_ptr<AsyncSocket> socket(nullptr);
  ConnCallback cb;
  clientThread(socket, &cb, &eventBase, serverAddress);

  eventBase.loop();

  // Verify if the connection is accepted and if the accepted socket has
  // setsockopt on the TOS for the same value that the listener was set to
  auto fd = acceptCallback.getEvents()->at(1).fd;
  ASSERT_NE(fd, NetworkSocket());
  int value;
  socklen_t valueLength = sizeof(value);
  int rc = netops::getsockopt(fd, IPPROTO_IP, IP_TOS, &value, &valueLength);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(value, 0x74);
}
#endif

#if defined(__linux__)
TEST(AsyncSocketTest, getBufInUse) {
  EventBase eventBase;
  std::shared_ptr<AsyncServerSocket> server(
      AsyncServerSocket::newSocket(&eventBase));
  server->bind(0);
  server->listen(5);

  std::shared_ptr<AsyncSocket> client = AsyncSocket::newSocket(&eventBase);
  client->connect(nullptr, server->getAddress());

  NetworkSocket servfd = server->getNetworkSocket();
  NetworkSocket accepted;
  uint64_t maxTries = 5;

  do {
    std::this_thread::yield();
    eventBase.loop();
    accepted = netops::accept(servfd, nullptr, nullptr);
  } while (accepted == NetworkSocket() && --maxTries);

  // Exhaustion number of tries to accept client connection, good bye
  ASSERT_TRUE(accepted != NetworkSocket());

  auto clientAccepted = AsyncSocket::newSocket(nullptr, accepted);

  // Use minimum receive buffer size
  clientAccepted->setRecvBufSize(0);

  // Use maximum send buffer size
  client->setSendBufSize((unsigned)-1);

  std::string testData;
  for (int i = 0; i < 10000; ++i) {
    testData += "0123456789";
  }

  client->write(nullptr, (const void*)testData.c_str(), testData.size());

  std::this_thread::yield();
  eventBase.loop();

  size_t recvBufSize = clientAccepted->getRecvBufInUse();
  size_t sendBufSize = client->getSendBufInUse();

  EXPECT_GT(recvBufSize, 0);
  EXPECT_GT(sendBufSize, 0);
  EXPECT_LE(recvBufSize, testData.size());
  EXPECT_LE(sendBufSize, testData.size());
}
#endif

TEST(AsyncSocketTest, QueueTimeout) {
  // Create a new AsyncServerSocket
  EventBase eventBase;
  std::shared_ptr<AsyncServerSocket> serverSocket(
      AsyncServerSocket::newSocket(&eventBase));
  serverSocket->bind(0);
  serverSocket->listen(16);
  folly::SocketAddress serverAddress;
  serverSocket->getAddress(&serverAddress);

  constexpr auto kConnectionTimeout = milliseconds(10);
  serverSocket->setQueueTimeout(kConnectionTimeout);

  TestAcceptCallback acceptCb;
  acceptCb.setConnectionAcceptedFn([&, called = false](auto&&...) mutable {
    ASSERT_FALSE(called)
        << "Only the first connection should have been dequeued";
    called = true;
    // Allow plenty of time for the AsyncSocketServer's event loop to run.
    // This should leave no doubt that the acceptor thread has enough time
    // to dequeue. If the dequeue succeeds, then our expiry code is broken.
    static constexpr auto kEventLoopTime = kConnectionTimeout * 5;
    eventBase.runInEventBaseThread([&]() {
      eventBase.tryRunAfterDelay(
          [&]() { serverSocket->removeAcceptCallback(&acceptCb, nullptr); },
          milliseconds(kEventLoopTime).count());
    });
    // After the first message is enqueued, sleep long enough so that the
    // second message expires before it has a chance to dequeue.
    std::this_thread::sleep_for(kConnectionTimeout);
  });
  ScopedEventBaseThread acceptThread("ioworker_test");

  TestConnectionEventCallback connectionEventCb;
  serverSocket->setConnectionEventCallback(&connectionEventCb);
  serverSocket->addAcceptCallback(&acceptCb, acceptThread.getEventBase());
  serverSocket->startAccepting();

  std::shared_ptr<AsyncSocket> clientSocket1(
      AsyncSocket::newSocket(&eventBase, serverAddress));
  std::shared_ptr<AsyncSocket> clientSocket2(
      AsyncSocket::newSocket(&eventBase, serverAddress));

  // Loop until we are stopped
  eventBase.loop();

  EXPECT_EQ(connectionEventCb.getConnectionEnqueuedForAcceptCallback(), 2);
  // Since the second message is expired, it should NOT be dequeued
  EXPECT_EQ(connectionEventCb.getConnectionDequeuedByAcceptCallback(), 1);
}

class TestRXTimestampsCallback
    : public folly::AsyncSocket::ReadAncillaryDataCallback {
 public:
  explicit TestRXTimestampsCallback(AsyncSocket* sock) : socket_(sock) {}

  folly::Expected<folly::Unit, AsyncSocketException> ancillaryData(
      struct msghdr& msgh) noexcept override {
    if (closeSocket_) {
      socket_->close();
      return folly::unit;
    }

    struct cmsghdr* cmsg;
    for (cmsg = CMSG_FIRSTHDR(&msgh); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&msgh, cmsg)) {
      if (cmsg->cmsg_level != SOL_SOCKET ||
          cmsg->cmsg_type != SO_TIMESTAMPING) {
        continue;
      }
      callCount_++;
      timespec* ts = (struct timespec*)CMSG_DATA(cmsg);
      actualRxTimestampSec_ = ts[0].tv_sec;
    }
    return folly::unit;
  }
  folly::MutableByteRange getAncillaryDataCtrlBuffer() override {
    return folly::MutableByteRange(ancillaryDataCtrlBuffer_);
  }

  uint32_t callCount_{0};
  long actualRxTimestampSec_{0};
  bool closeSocket_{false};

 private:
  AsyncSocket* socket_;
  std::array<uint8_t, 1024> ancillaryDataCtrlBuffer_;
};

/**
 * Test read ancillary data callback
 */
TEST(AsyncSocketTest, readAncillaryData) {
  TestServer server;

  // connect()
  EventBase evb;
  std::shared_ptr<AsyncSocket> socket = AsyncSocket::newSocket(&evb);

  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 1);
  LOG(INFO) << "Client socket fd=" << socket->getNetworkSocket();

  // Enable rx timestamp notifications
  ASSERT_NE(socket->getNetworkSocket(), NetworkSocket());
  int flags = folly::netops::SOF_TIMESTAMPING_SOFTWARE |
      folly::netops::SOF_TIMESTAMPING_RX_SOFTWARE |
      folly::netops::SOF_TIMESTAMPING_RX_HARDWARE;
  SocketOptionKey tstampingOpt = {SOL_SOCKET, SO_TIMESTAMPING};
  EXPECT_EQ(tstampingOpt.apply(socket->getNetworkSocket(), flags), 0);

  // Accept the connection.
  std::shared_ptr<BlockingSocket> acceptedSocket = server.accept();
  LOG(INFO) << "Server socket fd=" << acceptedSocket->getNetworkSocket();

  // Wait for connection
  evb.loop();
  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);

  TestRXTimestampsCallback rxcb{socket.get()};

  // Set read callback
  ReadCallback rcb(100);
  socket->setReadCB(&rcb);

  // Get the timestamp when the message was write
  struct timespec currentTime;
  clock_gettime(CLOCK_REALTIME, &currentTime);
  long writeTimestampSec = currentTime.tv_sec;

  // write bytes from server (acceptedSocket) to client (socket).
  std::vector<uint8_t> wbuf(128, 'a');
  acceptedSocket->write(wbuf.data(), wbuf.size());

  // Wait for reading to complete.
  evb.loopOnce();
  ASSERT_NE(rcb.buffers.size(), 0);

  // Verify that if the callback is not set, it will not be called
  ASSERT_EQ(rxcb.callCount_, 0);

  // Set up rx timestamp callbacks
  socket->setReadAncillaryDataCB(&rxcb);
  acceptedSocket->write(wbuf.data(), wbuf.size());

  // Wait for reading to complete.
  evb.loopOnce();
  ASSERT_NE(rcb.buffers.size(), 0);

  // Verify that after setting callback, the callback was called
  ASSERT_GT(rxcb.callCount_, 0);
  // Compare the received timestamp is within an expected range
  clock_gettime(CLOCK_REALTIME, &currentTime);
  ASSERT_TRUE(rxcb.actualRxTimestampSec_ <= currentTime.tv_sec);
  ASSERT_TRUE(rxcb.actualRxTimestampSec_ >= writeTimestampSec);

  // Check that the callback can close the socket.
  rxcb.closeSocket_ = true;
  ASSERT_FALSE(socket->isClosedBySelf());
  acceptedSocket->write(wbuf.data(), wbuf.size());
  evb.loopOnce();
  ASSERT_TRUE(socket->isClosedBySelf());
}

class AsyncSocketWriteCallbackTest : public ::testing::Test {
 protected:
  using MockDispatcher = ::testing::NiceMock<netops::test::MockDispatcher>;
  void SetUp() override {
    socket_ = AsyncSocket::newSocket(&evb_);
    socket_->setOverrideNetOpsDispatcher(netOpsDispatcher_);
    netOpsDispatcher_->forwardToDefaultImpl();

    socket_->connect(nullptr, server_.getAddress());
  }

  void netOpsOnSendmsg() {
    ON_CALL(*netOpsDispatcher_, sendmsg(_, _, _))
        .WillByDefault(
            ::testing::Invoke(
                [this](NetworkSocket s, const msghdr* message, int flags) {
                  sendMsgInvocations_++;
                  return netops::Dispatcher::getDefaultInstance()->sendmsg(
                      s, message, flags);
                }));
  }

  // simulate spliting a write into two parts by returning less than the amount
  // of bytes that was written if this is the first invocation of sendMsg
  void netOpsOnSendmsgPartial() {
    ON_CALL(*netOpsDispatcher_, sendmsg(_, _, _))
        .WillByDefault(
            ::testing::Invoke(
                [this](NetworkSocket s, const msghdr* message, int flags) {
                  sendMsgInvocations_++;
                  auto totalWritten =
                      netops::Dispatcher::getDefaultInstance()->sendmsg(
                          s, message, flags);
                  if (splitNextWrite_) {
                    splitNextWrite_ = false;
                    return totalWritten - 1;
                  } else {
                    splitNextWrite_ = true;
                    return totalWritten;
                  }
                }));
  }

  // simulate a failed write by returning -1 on sendMsg
  void netOpsOnSendmsgFail() {
    ON_CALL(*netOpsDispatcher_, sendmsg(_, _, _))
        .WillByDefault(
            ::testing::Invoke(
                [this](NetworkSocket s, const msghdr* message, int flags) {
                  sendMsgInvocations_++;
                  netops::Dispatcher::getDefaultInstance()->sendmsg(
                      s, message, flags);
                  return -1;
                }));
  }

  WriteCallback writeCallback1_;
  WriteCallback writeCallback2_;
  TestServer server_;
  std::shared_ptr<AsyncSocket> socket_;
  folly::EventBase evb_;
  std::shared_ptr<MockDispatcher> netOpsDispatcher_{
      std::make_shared<MockDispatcher>()};
  size_t sendMsgInvocations_{0};
  bool splitNextWrite_{false};
};

/**
 * Call write once successfully and expect `writeStarting` to be called once.
 */
TEST_F(AsyncSocketWriteCallbackTest, WriteStartingTests_WriteOnceSuccess) {
  const std::vector<uint8_t> wbuf(20, 'a');
  iovec op = {};
  op.iov_base = const_cast<void*>(static_cast<const void*>(wbuf.data()));
  op.iov_len = wbuf.size();
  WriteFlags flags = WriteFlags::NONE;

  netOpsOnSendmsg();

  ASSERT_THAT(writeCallback1_.writeStartingInvocations, Eq(0));
  ASSERT_THAT(writeCallback2_.writeStartingInvocations, Eq(0));
  socket_->writev(&writeCallback1_, &op, 1, flags);
  while (writeCallback1_.state == STATE_WAITING) {
    socket_->getEventBase()->loopOnce();
  }
  ASSERT_EQ(writeCallback1_.state, STATE_SUCCEEDED);
  ASSERT_EQ(writeCallback2_.state, STATE_WAITING);
  EXPECT_EQ(writeCallback1_.writeStartingInvocations, 1);
  EXPECT_EQ(writeCallback2_.writeStartingInvocations, 0);
  EXPECT_EQ(sendMsgInvocations_, 1);
}

/**
 * Call write once but do not write all bytes the first time; expect
 * `writeStarting` to be called once.
 */
TEST_F(AsyncSocketWriteCallbackTest, WriteStartingTests_WriteOnceIncomplete) {
  const std::vector<uint8_t> wbuf(20, 'a');
  iovec op = {};
  op.iov_base = const_cast<void*>(static_cast<const void*>(wbuf.data()));
  op.iov_len = wbuf.size();
  WriteFlags flags = WriteFlags::NONE;

  // make sure there are no pending WriteRequests
  socket_->getEventBase()->loopOnce();

  splitNextWrite_ = true;
  netOpsOnSendmsgPartial();

  ASSERT_THAT(writeCallback1_.writeStartingInvocations, Eq(0));
  socket_->writev(&writeCallback1_, &op, 1, flags);
  while (writeCallback1_.state == STATE_WAITING) {
    socket_->getEventBase()->loopOnce();
  }

  ASSERT_EQ(writeCallback1_.state, STATE_SUCCEEDED);
  EXPECT_EQ(writeCallback1_.writeStartingInvocations, 1);
  EXPECT_EQ(sendMsgInvocations_, 2);
}

/**
 * Call write twice successfully and expect `writeStarting` to be called twice.
 */
TEST_F(AsyncSocketWriteCallbackTest, WriteStartingTests_WriteTwiceSuccess) {
  const std::vector<uint8_t> wbuf(20, 'a');
  iovec op = {};
  op.iov_base = const_cast<void*>(static_cast<const void*>(wbuf.data()));
  op.iov_len = wbuf.size();
  WriteFlags flags = WriteFlags::NONE;

  netOpsOnSendmsg();

  ASSERT_THAT(writeCallback1_.writeStartingInvocations, Eq(0));
  socket_->writev(&writeCallback1_, &op, 1, flags);
  socket_->writev(&writeCallback1_, &op, 1, flags);
  while (writeCallback1_.state == STATE_WAITING) {
    socket_->getEventBase()->loopOnce();
  }
  ASSERT_EQ(writeCallback1_.state, STATE_SUCCEEDED);
  EXPECT_EQ(writeCallback1_.writeStartingInvocations, 2);
  EXPECT_EQ(sendMsgInvocations_, 2);
}

/**
 * Call write twice, with the first write incomplete; expect `writeStarting` to
 * be called twice
 */
TEST_F(
    AsyncSocketWriteCallbackTest,
    WriteStartingTests_WriteTwiceIncompleteThenSuccess) {
  const std::vector<uint8_t> wbuf(20, 'a');
  iovec op = {};
  op.iov_base = const_cast<void*>(static_cast<const void*>(wbuf.data()));
  op.iov_len = wbuf.size();
  WriteFlags flags = WriteFlags::NONE;

  ASSERT_THAT(writeCallback1_.writeStartingInvocations, Eq(0));

  // We split the first write in two parts. The first part is written
  // immediately and a WriteRequest is created to write the bytes from the
  // second part
  splitNextWrite_ = true;
  netOpsOnSendmsgPartial();
  socket_->writev(&writeCallback1_, &op, 1, flags);
  while (writeCallback1_.state == STATE_WAITING) {
    socket_->getEventBase()->loopOnce();
  }
  ASSERT_EQ(writeCallback1_.state, STATE_SUCCEEDED);
  EXPECT_EQ(writeCallback1_.writeStartingInvocations, 1);

  // We do not split the second write
  netOpsOnSendmsg();
  socket_->writev(&writeCallback2_, &op, 1, flags);
  while (writeCallback2_.state == STATE_WAITING) {
    socket_->getEventBase()->loopOnce();
  }
  ASSERT_EQ(writeCallback1_.state, STATE_SUCCEEDED);
  EXPECT_EQ(writeCallback1_.writeStartingInvocations, 1);
  ASSERT_EQ(writeCallback2_.state, STATE_SUCCEEDED);
  EXPECT_EQ(writeCallback2_.writeStartingInvocations, 1);
  EXPECT_EQ(sendMsgInvocations_, 3);
}

/**
 * Call write twice, both times incomplete; expect `writeStarting` to be called
 * twice.
 */
TEST_F(
    AsyncSocketWriteCallbackTest,
    WriteStartingTests_WriteTwiceIncompleteThenIncomplete) {
  const std::vector<uint8_t> wbuf(20, 'a');
  iovec op = {};
  op.iov_base = const_cast<void*>(static_cast<const void*>(wbuf.data()));
  op.iov_len = wbuf.size();
  WriteFlags flags = WriteFlags::NONE;

  ASSERT_THAT(writeCallback1_.writeStartingInvocations, Eq(0));

  // We split the first write in two parts. The first part is written
  // immediately and a WriteRequest is created to write the bytes from the
  // second part
  splitNextWrite_ = true;
  netOpsOnSendmsgPartial();
  socket_->writev(&writeCallback1_, &op, 1, flags);
  socket_->getEventBase()->loopOnce();

  EXPECT_EQ(sendMsgInvocations_, 1);
  ASSERT_EQ(writeCallback1_.state, STATE_WAITING);
  EXPECT_EQ(writeCallback1_.writeStartingInvocations, 1);

  // We also split the second write. Since the WriteRequest queue is not empty,
  // a new WriteRequest for all bytes in the second write is created when writev
  // is called. The write will be split into two parts when we process this
  // request. The first part is written when the request is processed and
  // another WriteRequest will be created for the second part. This new
  // WriteRequest will be processed on the next event loop iteration.
  socket_->writev(&writeCallback2_, &op, 1, flags);
  socket_->getEventBase()->loopOnce();

  EXPECT_EQ(sendMsgInvocations_, 3);
  ASSERT_EQ(writeCallback1_.state, STATE_SUCCEEDED);
  EXPECT_EQ(writeCallback1_.writeStartingInvocations, 1);
  ASSERT_EQ(writeCallback2_.state, STATE_WAITING);
  EXPECT_EQ(writeCallback2_.writeStartingInvocations, 1);

  socket_->getEventBase()->loopOnce();

  ASSERT_EQ(writeCallback1_.state, STATE_SUCCEEDED);
  EXPECT_EQ(writeCallback1_.writeStartingInvocations, 1);
  ASSERT_EQ(writeCallback2_.state, STATE_SUCCEEDED);
  EXPECT_EQ(writeCallback2_.writeStartingInvocations, 1);
  EXPECT_EQ(sendMsgInvocations_, 4);
}

/**
 * Call write once and fail immediately; expect 'writeStarting` to not be
 * called.
 */
TEST_F(
    AsyncSocketWriteCallbackTest, WriteStartingTests_WriteOnceFailImmediately) {
  const std::vector<uint8_t> wbuf(20, 'a');
  iovec op = {};
  op.iov_base = const_cast<void*>(static_cast<const void*>(wbuf.data()));
  op.iov_len = wbuf.size();
  WriteFlags flags = WriteFlags::NONE;

  ASSERT_THAT(writeCallback1_.writeStartingInvocations, Eq(0));
  socket_->writev(&writeCallback1_, &op, 1, flags);
  socket_->shutdownWriteNow();
  ASSERT_EQ(writeCallback1_.state, STATE_FAILED);
  EXPECT_EQ(writeCallback1_.writeStartingInvocations, 0);
}

/**
 * Call write once and fail after `sendMsg` was called; expect 'writeStarting`
 * to be called once.
 */
TEST_F(AsyncSocketWriteCallbackTest, WriteStartingTests_WriteOnceFail) {
  const std::vector<uint8_t> wbuf(20, 'a');
  iovec op = {};
  op.iov_base = const_cast<void*>(static_cast<const void*>(wbuf.data()));
  op.iov_len = wbuf.size();
  WriteFlags flags = WriteFlags::NONE;

  netOpsOnSendmsgFail();
  writeCallback1_.errorCallback = std::bind(&AsyncSocket::close, socket_.get());

  ASSERT_THAT(writeCallback1_.writeStartingInvocations, Eq(0));
  socket_->writev(&writeCallback1_, &op, 1, flags);
  while (writeCallback1_.state == STATE_WAITING) {
    socket_->getEventBase()->loopOnce();
  }
  ASSERT_EQ(writeCallback1_.state, STATE_FAILED);
  EXPECT_EQ(writeCallback1_.writeStartingInvocations, 1);
}

TEST(AsyncSocketTest, BindAddressNoPort) {
  EventBase eventBase;
  TestServer server(true);

  // When setBindAddressNoPort is disabled, verifies that a port is assigned
  // before the connect call
  auto socket = AsyncSocket::newSocket(&eventBase);
  socket->setBindAddressNoPort(false);
  SocketAddress bindAddr("127.0.0.1", 0);
  TestPortAssignmentCallback callback;
  socket->connect(
      &callback, server.getAddress(), 30, emptySocketOptionMap, bindAddr);
  eventBase.loop();
  EXPECT_NE(callback.assignedPort, 0);
  socket->close();
}

TEST_P(AsyncSocketTest, MoveSocketWithActiveRead) {
  TestServer server;
  EventBase& evb = getEventBase();

  auto socket = AsyncSocket::newSocket(&evb);
  ConnCallback connCb;
  socket->connect(&connCb, server.getAddress(), 30);
  evb.loop();
  ASSERT_EQ(connCb.state, STATE_SUCCEEDED);

  auto acceptedFd = server.acceptFD();
  auto serverSock = AsyncSocket::UniquePtr(new AsyncSocket(&evb, acceptedFd));

  // Read first batch on the original socket
  ReadCallback readCb;
  readCb.dataAvailableCallback = [&] { evb.terminateLoopSoon(); };
  serverSock->setReadCB(&readCb);

  const std::string data = "hello from client";
  WriteCallback writeCb;
  socket->write(&writeCb, data.data(), data.size());
  evb.loop();

  ASSERT_FALSE(readCb.buffers.empty());
  size_t firstBytes = 0;
  for (auto& buf : readCb.buffers) {
    firstBytes += buf.length;
  }
  EXPECT_EQ(firstBytes, data.size());

  // Move socket — triggers async detach of io_uring recv handle
  serverSock->setReadCB(nullptr);
  auto movedSock = AsyncSocket::UniquePtr(new AsyncSocket(serverSock.get()));
  serverSock.reset();

  // Read second batch on the moved socket
  ReadCallback readCb2;
  readCb2.dataAvailableCallback = [&] { evb.terminateLoopSoon(); };
  movedSock->setReadCB(&readCb2);

  socket->write(&writeCb, data.data(), data.size());
  evb.loop();

  ASSERT_FALSE(readCb2.buffers.empty());
  size_t secondBytes = 0;
  for (auto& buf : readCb2.buffers) {
    secondBytes += buf.length;
  }
  EXPECT_EQ(secondBytes, data.size());

  movedSock->close();
  socket->close();
}

TEST_P(AsyncSocketTest, PreReceivedDataOrderedBeforeRecvAcrossMove) {
  TestServer server;
  EventBase& evb = getEventBase();

  auto socket = AsyncSocket::newSocket(&evb);
  ConnCallback connCb;
  socket->connect(&connCb, server.getAddress(), 30);
  evb.loop();
  ASSERT_EQ(connCb.state, STATE_SUCCEEDED);

  auto acceptedFd = server.acceptFD();
  auto serverSock = AsyncSocket::UniquePtr(new AsyncSocket(&evb, acceptedFd));

  const std::string pre = "PRE";
  serverSock->setPreReceivedData(IOBuf::copyBuffer(pre.data(), pre.size()));

  auto movedSock = AsyncSocket::UniquePtr(new AsyncSocket(serverSock.get()));
  serverSock.reset();

  const std::string wire = "WIRE";
  ReadCallback readCb;
  readCb.dataAvailableCallback = [&] {
    if (readCb.dataRead() >= pre.size() + wire.size()) {
      evb.terminateLoopSoon();
    }
  };
  movedSock->setReadCB(&readCb);

  WriteCallback writeCb;
  socket->write(&writeCb, wire.data(), wire.size());
  evb.loop();

  readCb.verifyData((pre + wire).data(), pre.size() + wire.size());

  movedSock->close();
  socket->close();
}

TEST_P(AsyncSocketTest, MoveEventBaseWithActiveRead) {
  TestServer server;
  EventBase& evb = getEventBase();
  auto evb2 = makeEventBase();

  auto socket = AsyncSocket::newSocket(&evb);
  ConnCallback connCb;
  socket->connect(&connCb, server.getAddress(), 30);
  evb.loop();
  ASSERT_EQ(connCb.state, STATE_SUCCEEDED);

  auto acceptedFd = server.acceptFD();
  auto serverSock = AsyncSocket::UniquePtr(new AsyncSocket(&evb, acceptedFd));

  ReadCallback readCb;
  readCb.dataAvailableCallback = [&] { evb.terminateLoopSoon(); };
  serverSock->setReadCB(&readCb);

  const std::string first = "first";
  WriteCallback writeCb;
  socket->write(&writeCb, first.data(), first.size());
  evb.loop();
  ASSERT_EQ(readCb.dataRead(), first.size());

  const std::string second = "second";
  const size_t want = first.size() + second.size();
  readCb.dataAvailableCallback = [&] {
    if (readCb.dataRead() >= want) {
      evb2->terminateLoopSoon();
    }
  };

  ASSERT_TRUE(serverSock->isDetachable());
  serverSock->detachEventBase();
  serverSock->attachEventBase(evb2.get());

  socket->write(&writeCb, second.data(), second.size());

  for (int i = 0; i < 50 && readCb.dataRead() < want; ++i) {
    evb.runAfterDelay([&] { evb.terminateLoopSoon(); }, 20);
    evb.loop();
    evb2->runAfterDelay([&] { evb2->terminateLoopSoon(); }, 20);
    evb2->loop();
  }

  readCb.verifyData((first + second).data(), want);

  serverSock->detachEventBase();
  serverSock->attachEventBase(&evb);
  serverSock->close();
  socket->close();
}

TEST_P(AsyncSocketTest, MoveEventBaseWithReadCallbackCleared) {
  TestServer server;
  EventBase& evb = getEventBase();
  auto evb2 = makeEventBase();

  auto socket = AsyncSocket::newSocket(&evb);
  ConnCallback connCb;
  socket->connect(&connCb, server.getAddress(), 30);
  evb.loop();
  ASSERT_EQ(connCb.state, STATE_SUCCEEDED);

  auto acceptedFd = server.acceptFD();
  auto serverSock = AsyncSocket::UniquePtr(new AsyncSocket(&evb, acceptedFd));

  ReadCallback readCb;
  serverSock->setReadCB(&readCb);
  serverSock->setReadCB(nullptr);

  ASSERT_TRUE(serverSock->isDetachable());
  serverSock->detachEventBase();
  serverSock->attachEventBase(evb2.get());

  readCb.dataAvailableCallback = [&] { evb2->terminateLoopSoon(); };
  serverSock->setReadCB(&readCb);

  const std::string data = "read after moving event base";
  WriteCallback writeCb;
  socket->write(&writeCb, data.data(), data.size());

  for (int i = 0; i < 50 && readCb.dataRead() < data.size(); ++i) {
    evb.runAfterDelay([&] { evb.terminateLoopSoon(); }, 20);
    evb.loop();
    evb2->runAfterDelay([&] { evb2->terminateLoopSoon(); }, 20);
    evb2->loop();
  }

  readCb.verifyData(data.data(), data.size());

  serverSock->detachEventBase();
  serverSock->attachEventBase(&evb);
  serverSock->close();
  socket->close();
}

TEST_P(AsyncSocketTest, CloseWithDetachedReadHandle) {
  TestServer server;
  EventBase& evb = getEventBase();

  auto socket = AsyncSocket::newSocket(&evb);
  ConnCallback connCb;
  socket->connect(&connCb, server.getAddress(), 30);
  evb.loop();
  ASSERT_EQ(connCb.state, STATE_SUCCEEDED);

  auto acceptedFd = server.acceptFD();
  auto serverSock = AsyncSocket::UniquePtr(new AsyncSocket(&evb, acceptedFd));

  ReadCallback readCb;
  serverSock->setReadCB(&readCb);

  ASSERT_TRUE(serverSock->isDetachable());
  serverSock->detachEventBase();
  serverSock->closeNow();

  EXPECT_TRUE(serverSock->isClosedBySelf());
  EXPECT_FALSE(serverSock->isClosedByPeer());
  EXPECT_EQ(readCb.state, STATE_SUCCEEDED);

  socket->close();
  evb.loop();
}

TEST_P(AsyncSocketTest, MoveSocketWithAlreadyDetachedReadHandle) {
  TestServer server;
  EventBase& evb = getEventBase();

  auto socket = AsyncSocket::newSocket(&evb);
  ConnCallback connCb;
  socket->connect(&connCb, server.getAddress(), 30);
  evb.loop();
  ASSERT_EQ(connCb.state, STATE_SUCCEEDED);

  auto acceptedFd = server.acceptFD();
  auto serverSock = AsyncSocket::UniquePtr(new AsyncSocket(&evb, acceptedFd));

  ReadCallback initialReadCb;
  serverSock->setReadCB(&initialReadCb);
  serverSock->setReadCB(nullptr);

  auto movedOnce = AsyncSocket::UniquePtr(new AsyncSocket(serverSock.get()));
  serverSock.reset();
  auto movedTwice = AsyncSocket::UniquePtr(new AsyncSocket(movedOnce.get()));
  movedOnce.reset();

  ReadCallback readCb;
  readCb.dataAvailableCallback = [&] { evb.terminateLoopSoon(); };
  movedTwice->setReadCB(&readCb);

  const std::string data = "read after moving socket twice";
  WriteCallback writeCb;
  socket->write(&writeCb, data.data(), data.size());

  for (int i = 0; i < 50 && readCb.dataRead() < data.size(); ++i) {
    evb.runAfterDelay([&] { evb.terminateLoopSoon(); }, 20);
    evb.loop();
  }

  EXPECT_EQ(writeCb.state, STATE_SUCCEEDED);
  readCb.verifyData(data.data(), data.size());

  movedTwice->close();
  socket->close();
}
