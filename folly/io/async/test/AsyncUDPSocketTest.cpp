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

#include <folly/io/SocketOptionMap.h>
#include <folly/io/async/AsyncUDPSocket.h>

#include <thread>

#include <folly/Conv.h>
#include <folly/SocketAddress.h>
#include <folly/String.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/AsyncUDPServerSocket.h>
#include <folly/io/async/EventBase.h>
#include <folly/net/test/MockNetOpsDispatcher.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <folly/portability/Sockets.h>
#include <folly/testing/TestUtil.h>

using folly::AsyncTimeout;
using folly::AsyncUDPServerSocket;
using folly::AsyncUDPSocket;
using folly::errnoStr;
using folly::EventBase;
using folly::SocketAddress;
using namespace testing;

using OnDataAvailableParams =
    folly::AsyncUDPSocket::ReadCallback::OnDataAvailableParams;

class UDPAcceptor : public AsyncUDPServerSocket::Callback {
 public:
  UDPAcceptor(
      EventBase* evb,
      int n,
      bool changePortForWrites,
      const folly::SocketAddress& serverAddress)
      : evb_(evb),
        n_(n),
        changePortForWrites_(changePortForWrites),
        serverAddress_(serverAddress) {}

  void onListenStarted() noexcept override {}

  void onListenStopped() noexcept override {}

  void onDataAvailable(
      std::shared_ptr<folly::AsyncUDPSocket> /* socket */,
      const folly::SocketAddress& client,
      std::unique_ptr<folly::IOBuf> data,
      bool truncated,
      OnDataAvailableParams) noexcept override {
    lastClient_ = client;
    lastMsg_ = data->to<std::string>();

    auto len = data->computeChainDataLength();
    VLOG(4) << "Worker " << n_ << " read " << len << " bytes " << "(trun:"
            << truncated << ") from " << client.describe() << " - " << lastMsg_;

    sendPong();
  }

  void sendPong() noexcept {
    try {
      auto writeSocket = std::make_shared<folly::AsyncUDPSocket>(evb_);
      if (changePortForWrites_) {
        writeSocket->setReuseAddr(false);
        writeSocket->bind(folly::SocketAddress("127.0.0.1", 0));
      } else {
        writeSocket->setReusePort(true);
        writeSocket->bind(serverAddress_);
      }
      writeSocket->setTosOrTrafficClass(2);
      writeSocket->write(lastClient_, folly::IOBuf::copyBuffer(lastMsg_));
    } catch (const std::exception& ex) {
      VLOG(4) << "Failed to send PONG " << ex.what();
    }
  }

 private:
  EventBase* const evb_{nullptr};
  const int n_{-1};
  // Whether to create a new port per write.
  bool changePortForWrites_{true};

  folly::SocketAddress serverAddress_;
  folly::SocketAddress lastClient_;
  std::string lastMsg_;
};

class UDPServer {
 public:
  UDPServer(EventBase* evb, folly::SocketAddress addr, int n)
      : evb_(evb), addr_(addr), evbs_(n) {}

  void start() {
    CHECK(evb_->isInEventBaseThread());

    socket_ = std::make_unique<AsyncUDPServerSocket>(evb_, 1500);
    socket_->setReusePort(true);

    try {
      socket_->bind(addr_);
      VLOG(4) << "Server listening on " << socket_->address().describe();
    } catch (const std::exception& ex) {
      LOG(FATAL) << ex.what();
    }

    acceptors_.reserve(evbs_.size());
    threads_.reserve(evbs_.size());

    // Add numWorkers thread
    int i = 0;
    for (auto& evb : evbs_) {
      acceptors_.emplace_back(
          &evb, i, changePortForWrites_, socket_->address());

      std::thread t([&]() { evb.loopForever(); });

      evb.waitUntilRunning();

      socket_->addListener(&evb, &acceptors_[i]);
      threads_.emplace_back(std::move(t));
      ++i;
    }

    socket_->listen();
  }

  folly::SocketAddress address() const { return socket_->address(); }

  void shutdown() {
    CHECK(evb_->isInEventBaseThread());
    socket_->close();
    socket_.reset();

    for (auto& evb : evbs_) {
      evb.terminateLoopSoon();
    }

    for (auto& t : threads_) {
      t.join();
    }
  }

  void pauseAccepting() { socket_->pauseAccepting(); }

  void resumeAccepting() { socket_->resumeAccepting(); }

  bool isAccepting() { return socket_->isAccepting(); }

  // Whether writes from the UDP server should change the port for each message.
  void setChangePortForWrites(bool changePortForWrites) {
    changePortForWrites_ = changePortForWrites;
  }

 private:
  EventBase* const evb_{nullptr};
  const folly::SocketAddress addr_;

  std::unique_ptr<AsyncUDPServerSocket> socket_;
  std::vector<std::thread> threads_;
  std::vector<UDPAcceptor> acceptors_;
  // destroy evbs_ before acceptors_ so that onListenStopped not called on a
  // freed UDPAcceptor
  std::vector<folly::EventBase> evbs_;
  bool changePortForWrites_{true};
};

enum class BindSocket { YES, NO };

class UDPClient : private AsyncUDPSocket::ReadCallback, private AsyncTimeout {
 public:
  using AsyncUDPSocket::ReadCallback::OnDataAvailableParams;

  ~UDPClient() override = default;

  explicit UDPClient(EventBase* evb) : AsyncTimeout(evb), evb_(evb) {}

  void start(
      const folly::SocketAddress& server, int n, bool sendClustered = false) {
    CHECK(evb_->isInEventBaseThread());
    server_ = server;
    socket_ = std::make_unique<AsyncUDPSocket>(evb_);
    socket_->setRecvTos(recvTos_);
    ASSERT_EQ(socket_->getRecvTos(), recvTos_);
    socket_->setRecvTtl(recvTtl_);
    ASSERT_EQ(socket_->getRecvTtl(), recvTtl_);

    try {
      if (bindSocket_ == BindSocket::YES) {
        socket_->bind(folly::SocketAddress("127.0.0.1", 0));
      }
      if (connectAddr_) {
        socket_->connect(*connectAddr_);
        VLOG(2) << "Client connected to address=" << *connectAddr_;
      }
      VLOG(2) << "Client bound to " << socket_->address().describe();
    } catch (const std::exception& ex) {
      LOG(FATAL) << ex.what();
    }

    socket_->resumeRead(this);

    n_ = n;

    // Start playing ping pong
    if (sendClustered) {
      sendPingsClustered();
    } else {
      sendPing();
    }
  }

  void shutdown() {
    CHECK(evb_->isInEventBaseThread());
    if (socket_) {
      socket_->pauseRead();
      socket_->close();
      socket_.reset();
    }
    evb_->terminateLoopSoon();
  }

  void sendPing() {
    if (n_ == 0) {
      shutdown();
      return;
    }

    --n_;
    scheduleTimeout(5);
    writePing(folly::IOBuf::copyBuffer(folly::to<std::string>("PING ", n_)));
  }

  void sendPingsClustered() {
    // give the server a bit more time to burst befor we time out
    scheduleTimeout(50);
    while (n_ > 0) {
      --n_;
      writePing(folly::IOBuf::copyBuffer(folly::to<std::string>("PING ", n_)));
    }
  }

  virtual void writePing(std::unique_ptr<folly::IOBuf> buf) {
    auto ret = socket_->write(server_, buf);
    if (ret == -1) {
      error_ = true;
    }
  }

  void getReadBuffer(void** buf, size_t* len) noexcept override {
    *buf = buf_;
    *len = 1024;
  }

  void onDataAvailable(
      const folly::SocketAddress& client,
      size_t len,
      bool truncated,
      OnDataAvailableParams params) noexcept override {
    VLOG(4) << "Read " << len << " bytes (trun:" << truncated << ") from "
            << client.describe() << " - " << std::string(buf_, len);
    VLOG(4) << n_ << " left";
    VLOG(4) << "Type of Service value:" << params.tos;
    if (params.ttl) {
      VLOG(4) << "TTL value:" << static_cast<int>(*params.ttl);
    }

    ++pongRecvd_;
    if (params.tos != 0) {
      ++tosMessagesRecvd_;
    }
    if (params.ttl) {
      ++ttlMessagesRecvd_;
    }

    sendPing();
  }

  void onReadError(const folly::AsyncSocketException& ex) noexcept override {
    VLOG(4) << ex.what();

    // Start listening for next PONG
    socket_->resumeRead(this);
  }

  void onReadClosed() noexcept override {
    CHECK(false) << "We unregister reads before closing";
  }

  void timeoutExpired() noexcept override {
    VLOG(4) << "Timeout expired";
    sendPing();
  }

  int pongRecvd() const { return pongRecvd_; }

  int tosMessagesRecvd() const { return tosMessagesRecvd_; }

  int ttlMessagesRecvd() const { return ttlMessagesRecvd_; }

  AsyncUDPSocket& getSocket() { return *socket_; }

  void setShouldConnect(
      const folly::SocketAddress& connectAddr, BindSocket bindSocket) {
    connectAddr_ = connectAddr;
    bindSocket_ = bindSocket;
  }

  void setRecvTos(bool recvTos) { recvTos_ = recvTos; }

  void setRecvTtl(bool recvTtl) { recvTtl_ = recvTtl; }

  bool error() const { return error_; }

  void incrementPongCount(int n) { pongRecvd_ += n; }

 protected:
  folly::Optional<folly::SocketAddress> connectAddr_;
  BindSocket bindSocket_{BindSocket::YES};
  EventBase* const evb_{nullptr};

  folly::SocketAddress server_;
  std::unique_ptr<AsyncUDPSocket> socket_;
  bool error_{false};

 private:
  int pongRecvd_{0};
  int tosMessagesRecvd_{0};
  bool recvTos_{false};
  int ttlMessagesRecvd_{0};
  bool recvTtl_{false};

  int n_{0};
  char buf_[1024];
};

class UDPNotifyClient : public UDPClient {
 public:
  ~UDPNotifyClient() override = default;

  explicit UDPNotifyClient(
      EventBase* evb, bool useRecvmmsg = false, unsigned int numMsgs = 1)
      : UDPClient(evb), useRecvmmsg_(useRecvmmsg), numMsgs_(numMsgs) {}

  bool shouldOnlyNotify() override { return true; }

  void onRecvMsg(AsyncUDPSocket& sock) {
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));

    void* buf{};
    size_t len{};
    getReadBuffer(&buf, &len);

    iovec vec;
    vec.iov_base = buf;
    vec.iov_len = len;
    struct sockaddr_storage addrStorage;
    socklen_t addrLen = sizeof(addrStorage);
    memset(&addrStorage, 0, size_t(addrLen));
    auto rawAddr = reinterpret_cast<sockaddr*>(&addrStorage);
    rawAddr->sa_family = socket_->address().getFamily();

    msg.msg_name = rawAddr;
    msg.msg_namelen = addrLen;
    msg.msg_iov = &vec;
    msg.msg_iovlen = 1;

    ssize_t ret = sock.recvmsg(&msg, 0);
    if (ret < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        onReadError(
            folly::AsyncSocketException(
                folly::AsyncSocketException::NETWORK_ERROR, "error"));
      }
    } else {
      // Datagram sockets in various domains (e.g., the UNIX and Internet
      // domains) permit zero-length datagrams.  When such a datagram is
      // received, the return value is 0.
      auto numBytesRecv{static_cast<size_t>(ret)};

      SocketAddress addr;
      addr.setFromSockaddr(rawAddr, addrLen);

      onDataAvailable(addr, numBytesRecv, false, OnDataAvailableParams());
    }
  }

  void onRecvMmsg(AsyncUDPSocket& sock) {
    const socklen_t addrLen = sizeof(struct sockaddr_storage);

    const size_t dataSize = 1024;
    std::vector<char> buf(numMsgs_ * dataSize);
    std::vector<struct mmsghdr> msgs(numMsgs_);
    std::vector<struct sockaddr_storage> addrs(numMsgs_);
    std::vector<struct iovec> iovecs(numMsgs_);

    for (unsigned int i = 0; i < numMsgs_; ++i) {
      struct msghdr* msg = &msgs[i].msg_hdr;

      auto rawAddr = reinterpret_cast<sockaddr*>(&addrs[i]);
      rawAddr->sa_family = socket_->address().getFamily();

      iovecs[i].iov_base = &buf[i * dataSize];
      iovecs[i].iov_len = dataSize;

      msg->msg_name = rawAddr;
      msg->msg_namelen = addrLen;
      msg->msg_iov = &iovecs[i];
      msg->msg_iovlen = 1;
    }

    int ret = sock.recvmmsg(
        msgs.data(), numMsgs_, 0x10000 /* MSG_WAITFORONE */, nullptr);
    if (ret < 0) {
      if (errno != EAGAIN || errno != EWOULDBLOCK) {
        onReadError(
            folly::AsyncSocketException(
                folly::AsyncSocketException::NETWORK_ERROR, "error"));
      }
      return;
    }

    incrementPongCount(ret);

    if (pongRecvd() == (int)numMsgs_) {
      shutdown();
    } else {
      onRecvMmsg(sock);
    }
  }

  void onNotifyDataAvailable(AsyncUDPSocket& sock) noexcept override {
    notifyInvoked = true;
    if (useRecvmmsg_) {
      onRecvMmsg(sock);
    } else {
      onRecvMsg(sock);
    }
  }

  bool notifyInvoked{false};
  bool useRecvmmsg_{false};
  unsigned int numMsgs_{1};
};

class AsyncSocketIntegrationTest : public Test {
 public:
  void SetUp() override {
    server = std::make_unique<UDPServer>(
        &sevb, folly::SocketAddress("127.0.0.1", 0), 4);

    // Start event loop in a separate thread
    serverThread = std::make_unique<std::thread>([this]() {
      sevb.loopForever();
    });

    // Wait for event loop to start
    sevb.waitUntilRunning();
  }

  void startServer() {
    // Start the server
    sevb.runInEventBaseThreadAndWait([&]() { server->start(); });
    LOG(INFO) << "Server listening=" << server->address();
  }

  void TearDown() override {
    // Shutdown server
    sevb.runInEventBaseThread([&]() {
      server->shutdown();
      sevb.terminateLoopSoon();
    });

    // Wait for server thread to join
    serverThread->join();
  }

  std::unique_ptr<UDPClient> performPingPongTest(
      folly::SocketAddress writeAddress,
      folly::Optional<folly::SocketAddress> connectedAddress,
      BindSocket bindSocket = BindSocket::YES);

  std::unique_ptr<UDPNotifyClient> performPingPongNotifyTest(
      folly::SocketAddress writeAddress,
      folly::Optional<folly::SocketAddress> connectedAddress,
      BindSocket bindSocket = BindSocket::YES);

  std::unique_ptr<UDPNotifyClient> performPingPongNotifyMmsgTest(
      folly::SocketAddress writeAddress,
      unsigned int numMsgs,
      folly::Optional<folly::SocketAddress> connectedAddress,
      BindSocket bindSocket = BindSocket::YES);

  std::unique_ptr<UDPClient> performPingPongRecvTosTest(
      folly::SocketAddress writeAddress,
      folly::Optional<folly::SocketAddress> connectedAddress,
      BindSocket bindSocket = BindSocket::YES);

  std::unique_ptr<UDPClient> performPingPongRecvTtlTest(
      folly::SocketAddress writeAddress,
      folly::Optional<folly::SocketAddress> connectedAddress,
      BindSocket bindSocket = BindSocket::YES);

  std::unique_ptr<std::thread> serverThread;
  std::unique_ptr<UDPServer> server;
  folly::EventBase sevb;
  folly::EventBase cevb;
};

std::unique_ptr<UDPClient> AsyncSocketIntegrationTest::performPingPongTest(
    folly::SocketAddress writeAddress,
    folly::Optional<folly::SocketAddress> connectedAddress,
    BindSocket bindSocket) {
  auto client = std::make_unique<UDPClient>(&cevb);
  if (connectedAddress) {
    client->setShouldConnect(*connectedAddress, bindSocket);
  }
  // Start event loop in a separate thread
  auto clientThread = std::thread([this]() { cevb.loopForever(); });

  // Wait for event loop to start
  cevb.waitUntilRunning();

  // Send ping
  cevb.runInEventBaseThread([&]() { client->start(writeAddress, 100); });

  // Wait for client to finish
  clientThread.join();
  return client;
}

std::unique_ptr<UDPNotifyClient>
AsyncSocketIntegrationTest::performPingPongNotifyTest(
    folly::SocketAddress writeAddress,
    folly::Optional<folly::SocketAddress> connectedAddress,
    BindSocket bindSocket) {
  auto client = std::make_unique<UDPNotifyClient>(&cevb);
  if (connectedAddress) {
    client->setShouldConnect(*connectedAddress, bindSocket);
  }
  // Start event loop in a separate thread
  auto clientThread = std::thread([this]() { cevb.loopForever(); });

  // Wait for event loop to start
  cevb.waitUntilRunning();

  // Send ping
  cevb.runInEventBaseThread([&]() { client->start(writeAddress, 100); });

  // Wait for client to finish
  clientThread.join();
  return client;
}

std::unique_ptr<UDPNotifyClient>
AsyncSocketIntegrationTest::performPingPongNotifyMmsgTest(
    folly::SocketAddress writeAddress,
    unsigned int numMsgs,
    folly::Optional<folly::SocketAddress> connectedAddress,
    BindSocket bindSocket) {
  auto client = std::make_unique<UDPNotifyClient>(&cevb, true, numMsgs);
  if (connectedAddress) {
    client->setShouldConnect(*connectedAddress, bindSocket);
  }
  // Start event loop in a separate thread
  auto clientThread = std::thread([this]() { cevb.loopForever(); });

  // Wait for event loop to start
  cevb.waitUntilRunning();

  // Send ping
  cevb.runInEventBaseThread([&]() {
    client->start(writeAddress, numMsgs, true);
  });

  // Wait for client to finish
  clientThread.join();
  return client;
}

std::unique_ptr<UDPClient>
AsyncSocketIntegrationTest::performPingPongRecvTosTest(
    folly::SocketAddress writeAddress,
    folly::Optional<folly::SocketAddress> connectedAddress,
    BindSocket bindSocket) {
  auto client = std::make_unique<UDPClient>(&cevb);
  if (connectedAddress) {
    client->setShouldConnect(*connectedAddress, bindSocket);
  }
  // Start event loop in a separate thread
  auto clientThread = std::thread([this]() { cevb.loopForever(); });

  // Wait for event loop to start
  cevb.waitUntilRunning();

  // Enable receiving ToS value
  client->setRecvTos(true);

  // Send ping
  cevb.runInEventBaseThread([&]() { client->start(writeAddress, 100); });

  // Wait for client to finish
  clientThread.join();
  return client;
}

std::unique_ptr<UDPClient>
AsyncSocketIntegrationTest::performPingPongRecvTtlTest(
    folly::SocketAddress writeAddress,
    folly::Optional<folly::SocketAddress> connectedAddress,
    BindSocket bindSocket) {
  auto client = std::make_unique<UDPClient>(&cevb);
  if (connectedAddress) {
    client->setShouldConnect(*connectedAddress, bindSocket);
  }
  // Start event loop in a separate thread
  auto clientThread = std::thread([this]() { cevb.loopForever(); });

  // Wait for event loop to start
  cevb.waitUntilRunning();

  // Enable receiving the IP header TTL
  client->setRecvTtl(true);

  // Send ping
  cevb.runInEventBaseThread([&]() { client->start(writeAddress, 100); });

  // Wait for client to finish
  clientThread.join();
  return client;
}

TEST_F(AsyncSocketIntegrationTest, PingPong) {
  startServer();
  auto pingClient = performPingPongTest(server->address(), folly::none);
  // This should succeed.
  ASSERT_GT(pingClient->pongRecvd(), 0);
}

TEST_F(AsyncSocketIntegrationTest, PingPongNotify) {
  startServer();
  auto pingClient = performPingPongNotifyTest(server->address(), folly::none);
  // This should succeed.
  ASSERT_GT(pingClient->pongRecvd(), 0);
  ASSERT_TRUE(pingClient->notifyInvoked);
}

TEST_F(AsyncSocketIntegrationTest, PingPongNotifyMmsg) {
  startServer();
  auto pingClient =
      performPingPongNotifyMmsgTest(server->address(), 10, folly::none);
  // This should succeed.
  ASSERT_EQ(pingClient->pongRecvd(), 10);
  ASSERT_TRUE(pingClient->notifyInvoked);
}

TEST_F(AsyncSocketIntegrationTest, PingPongRecvTosDisabled) {
  startServer();
  auto pingClient = performPingPongTest(server->address(), folly::none);
  // This should succeed.
  ASSERT_GT(pingClient->pongRecvd(), 0);
  ASSERT_EQ(pingClient->tosMessagesRecvd(), 0);
}

TEST_F(AsyncSocketIntegrationTest, PingPongRecvTos) {
  startServer();
  auto pingClient = performPingPongRecvTosTest(server->address(), folly::none);
  // This should succeed.
  ASSERT_GT(pingClient->pongRecvd(), 0);
  ASSERT_GT(pingClient->tosMessagesRecvd(), 0);
}

TEST_F(AsyncSocketIntegrationTest, PingPongRecvTtlDisabled) {
  startServer();
  auto pingClient = performPingPongTest(server->address(), folly::none);
  // This should succeed.
  ASSERT_GT(pingClient->pongRecvd(), 0);
  ASSERT_EQ(pingClient->ttlMessagesRecvd(), 0);
}

TEST_F(AsyncSocketIntegrationTest, PingPongRecvTtl) {
  startServer();
  auto pingClient = performPingPongRecvTtlTest(server->address(), folly::none);
  // This should succeed.
  ASSERT_GT(pingClient->pongRecvd(), 0);
  ASSERT_GT(pingClient->ttlMessagesRecvd(), 0);
}

class ConnectedAsyncSocketIntegrationTest
    : public AsyncSocketIntegrationTest,
      public WithParamInterface<BindSocket> {};

TEST_P(ConnectedAsyncSocketIntegrationTest, ConnectedPingPong) {
  server->setChangePortForWrites(false);
  startServer();
  auto pingClient =
      performPingPongTest(server->address(), server->address(), GetParam());
  // This should succeed
  ASSERT_GT(pingClient->pongRecvd(), 0);
}

TEST_P(
    ConnectedAsyncSocketIntegrationTest, ConnectedPingPongServerWrongAddress) {
  server->setChangePortForWrites(true);
  startServer();
  auto pingClient =
      performPingPongTest(server->address(), server->address(), GetParam());
  // This should fail.
  ASSERT_EQ(pingClient->pongRecvd(), 0);
}

TEST_P(
    ConnectedAsyncSocketIntegrationTest, ConnectedPingPongClientWrongAddress) {
  server->setChangePortForWrites(false);
  startServer();
  folly::SocketAddress connectAddr(
      server->address().getIPAddress(), server->address().getPort() + 1);
  auto pingClient =
      performPingPongTest(server->address(), connectAddr, GetParam());
  // This should fail.
  ASSERT_EQ(pingClient->pongRecvd(), 0);
  EXPECT_TRUE(pingClient->error());
}

TEST_P(
    ConnectedAsyncSocketIntegrationTest,
    ConnectedPingPongDifferentWriteAddress) {
  server->setChangePortForWrites(false);
  startServer();
  folly::SocketAddress connectAddr(
      server->address().getIPAddress(), server->address().getPort() + 1);
  auto pingClient =
      performPingPongTest(connectAddr, server->address(), GetParam());
  // This should fail.
  ASSERT_EQ(pingClient->pongRecvd(), 0);
  EXPECT_TRUE(pingClient->error());
}

INSTANTIATE_TEST_SUITE_P(
    ConnectedAsyncSocketIntegrationTests,
    ConnectedAsyncSocketIntegrationTest,
    Values(BindSocket::YES, BindSocket::NO));

TEST_F(AsyncSocketIntegrationTest, PingPongPauseResumeListening) {
  startServer();

  // Exchange should not happen when paused.
  sevb.runInEventBaseThreadAndWait([&]() { server->pauseAccepting(); });
  EXPECT_FALSE(server->isAccepting());
  auto pausedClient = performPingPongTest(server->address(), folly::none);
  ASSERT_EQ(pausedClient->pongRecvd(), 0);

  // Exchange does occur after resuming.
  sevb.runInEventBaseThreadAndWait([&]() { server->resumeAccepting(); });
  EXPECT_TRUE(server->isAccepting());
  auto pingClient = performPingPongTest(server->address(), folly::none);
  ASSERT_GT(pingClient->pongRecvd(), 0);
}

class MockErrMessageCallback : public AsyncUDPSocket::ErrMessageCallback {
 public:
  ~MockErrMessageCallback() override = default;

  MOCK_METHOD(void, errMessage_, (const cmsghdr&));
  void errMessage(const cmsghdr& cmsg) noexcept override { errMessage_(cmsg); }

  MOCK_METHOD(void, errMessageError_, (const folly::AsyncSocketException&));
  void errMessageError(
      const folly::AsyncSocketException& ex) noexcept override {
    errMessageError_(ex);
  }
};

class MockUDPReadCallback : public AsyncUDPSocket::ReadCallback {
 public:
  ~MockUDPReadCallback() override = default;

  MOCK_METHOD(void, getReadBuffer_, (void**, size_t*));
  void getReadBuffer(void** buf, size_t* len) noexcept override {
    getReadBuffer_(buf, len);
  }

  MOCK_METHOD(bool, shouldOnlyNotify, ());
  MOCK_METHOD(void, onNotifyDataAvailable_, (folly::AsyncUDPSocket&));
  void onNotifyDataAvailable(folly::AsyncUDPSocket& sock) noexcept override {
    onNotifyDataAvailable_(sock);
  }

  MOCK_METHOD(
      void,
      onDataAvailable_,
      (const folly::SocketAddress&, size_t, bool, OnDataAvailableParams));
  void onDataAvailable(
      const folly::SocketAddress& client,
      size_t len,
      bool truncated,
      OnDataAvailableParams params) noexcept override {
    onDataAvailable_(client, len, truncated, params);
  }

  MOCK_METHOD(void, onReadError_, (const folly::AsyncSocketException&));
  void onReadError(const folly::AsyncSocketException& ex) noexcept override {
    onReadError_(ex);
  }

  MOCK_METHOD(void, onReadClosed_, ());
  void onReadClosed() noexcept override { onReadClosed_(); }
};

class AsyncUDPSocketTest : public Test {
 public:
  void SetUp() override {
    socket_ = std::make_shared<AsyncUDPSocket>(&evb_);
    addr_ = folly::SocketAddress("127.0.0.1", 0);
    socket_->bind(addr_);
  }

  EventBase evb_;
  MockErrMessageCallback err;
  MockUDPReadCallback readCb;
  std::shared_ptr<AsyncUDPSocket> socket_;
  folly::SocketAddress addr_;
};

TEST_F(AsyncUDPSocketTest, TestConnectAfterBind) {
  socket_->connect(addr_);
}

TEST_F(AsyncUDPSocketTest, TestConnect) {
  AsyncUDPSocket socket(&evb_);
  EXPECT_FALSE(socket.isBound());
  folly::SocketAddress address("127.0.0.1", 443);
  socket.connect(address);
  EXPECT_TRUE(socket.isBound());

  const auto& localAddr = socket.address();
  EXPECT_TRUE(localAddr.isInitialized());
  EXPECT_GT(localAddr.getPort(), 0);
}

TEST_F(AsyncUDPSocketTest, TestErrToNonExistentServer) {
  socket_->resumeRead(&readCb);
  socket_->setErrMessageCallback(&err);
  folly::SocketAddress addr("127.0.0.1", 10000);
  bool errRecvd = false;
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  EXPECT_CALL(err, errMessage_(_))
      .WillOnce(Invoke([this, &errRecvd](auto& cmsg) {
        if ((cmsg.cmsg_level == SOL_IP && cmsg.cmsg_type == IP_RECVERR) ||
            (cmsg.cmsg_level == SOL_IPV6 && cmsg.cmsg_type == IPV6_RECVERR)) {
          const struct sock_extended_err* serr =
              reinterpret_cast<const struct sock_extended_err*>(
                  CMSG_DATA(&cmsg));
          errRecvd =
              (serr->ee_origin == SO_EE_ORIGIN_ICMP || SO_EE_ORIGIN_ICMP6);
          LOG(ERROR) << "errno " << errnoStr(serr->ee_errno);
        }
        evb_.terminateLoopSoon();
      }));
#endif // FOLLY_HAVE_MSG_ERRQUEUE
  socket_->write(addr, folly::IOBuf::copyBuffer("hey"));
  evb_.loopForever();
  EXPECT_TRUE(errRecvd);
}

TEST_F(AsyncUDPSocketTest, TestUnsetErrCallback) {
  socket_->resumeRead(&readCb);
  socket_->setErrMessageCallback(&err);
  socket_->setErrMessageCallback(nullptr);
  folly::SocketAddress addr("127.0.0.1", 10000);
  EXPECT_CALL(err, errMessage_(_)).Times(0);
  socket_->write(addr, folly::IOBuf::copyBuffer("hey"));
  evb_.timer().scheduleTimeoutFn(
      [&] { evb_.terminateLoopSoon(); }, std::chrono::milliseconds(30));
  evb_.loopForever();
}

TEST_F(AsyncUDPSocketTest, CloseInErrorCallback) {
  socket_->resumeRead(&readCb);
  socket_->setErrMessageCallback(&err);
  folly::SocketAddress addr("127.0.0.1", 10000);
  bool errRecvd = false;
  EXPECT_CALL(err, errMessage_(_)).WillOnce(Invoke([this, &errRecvd](auto&) {
    errRecvd = true;
    socket_->close();
    evb_.terminateLoopSoon();
  }));
  socket_->write(addr, folly::IOBuf::copyBuffer("hey"));
  socket_->write(addr, folly::IOBuf::copyBuffer("hey"));
  evb_.loopForever();
  EXPECT_TRUE(errRecvd);
}

TEST_F(AsyncUDPSocketTest, TestNonExistentServerNoErrCb) {
  socket_->resumeRead(&readCb);
  folly::SocketAddress addr("127.0.0.1", 10000);
  bool errRecvd = false;
  folly::IOBufQueue readBuf;
  EXPECT_CALL(readCb, getReadBuffer_(_, _))
      .WillRepeatedly(Invoke([&readBuf](void** buf, size_t* len) {
        auto readSpace = readBuf.preallocate(2000, 10000);
        *buf = readSpace.first;
        *len = readSpace.second;
      }));
  ON_CALL(readCb, onReadError_(_)).WillByDefault(Invoke([&errRecvd](auto& ex) {
    LOG(ERROR) << ex.what();
    errRecvd = true;
  }));
  socket_->write(addr, folly::IOBuf::copyBuffer("hey"));
  evb_.timer().scheduleTimeoutFn(
      [&] { evb_.terminateLoopSoon(); }, std::chrono::milliseconds(30));
  evb_.loopForever();
  EXPECT_FALSE(errRecvd);
}

TEST_F(AsyncUDPSocketTest, TestBound) {
  AsyncUDPSocket socket(&evb_);
  EXPECT_FALSE(socket.isBound());
  folly::SocketAddress address("0.0.0.0", 0);
  socket.bind(address);
  EXPECT_TRUE(socket.isBound());
}

TEST_F(AsyncUDPSocketTest, TestBoundUnixSocket) {
  folly::test::TemporaryDirectory tmpDirectory;
  const auto kTmpUnixSocketPath{tmpDirectory.path() / "unix_socket_path"};
  AsyncUDPSocket socket(&evb_);
  EXPECT_FALSE(socket.isBound());
  socket.bind(folly::SocketAddress::makeFromPath(kTmpUnixSocketPath.string()));
  EXPECT_TRUE(socket.isBound());
  socket.close();
}

TEST_F(AsyncUDPSocketTest, TestAttachAfterDetachEvbWithReadCallback) {
  socket_->resumeRead(&readCb);
  EXPECT_TRUE(socket_->isHandlerRegistered());
  socket_->detachEventBase();
  EXPECT_FALSE(socket_->isHandlerRegistered());
  socket_->attachEventBase(&evb_);
  EXPECT_TRUE(socket_->isHandlerRegistered());
}

TEST_F(AsyncUDPSocketTest, TestAttachAfterDetachEvbNoReadCallback) {
  EXPECT_FALSE(socket_->isHandlerRegistered());
  socket_->detachEventBase();
  EXPECT_FALSE(socket_->isHandlerRegistered());
  socket_->attachEventBase(&evb_);
  EXPECT_FALSE(socket_->isHandlerRegistered());
}

TEST_F(AsyncUDPSocketTest, TestDetachAttach) {
  folly::EventBase evb2;
  auto writeSocket = std::make_shared<folly::AsyncUDPSocket>(&evb_);
  folly::SocketAddress address("127.0.0.1", 0);
  writeSocket->bind(address);
  std::array<uint8_t, 1024> data;
  std::atomic<int> packetsRecvd{0};
  EXPECT_CALL(readCb, getReadBuffer_(_, _))
      .WillRepeatedly(Invoke([&](void** buf, size_t* len) {
        *buf = data.data();
        *len = 1024;
      }));
  EXPECT_CALL(readCb, onDataAvailable_(_, _, _, _))
      .WillRepeatedly(Invoke(
          [&](const folly::SocketAddress&,
              size_t,
              bool,
              OnDataAvailableParams) { packetsRecvd++; }));
  socket_->resumeRead(&readCb);
  writeSocket->write(socket_->address(), folly::IOBuf::copyBuffer("hello"));
  while (packetsRecvd != 1) {
    evb_.loopOnce();
  }
  EXPECT_EQ(packetsRecvd, 1);

  socket_->detachEventBase();
  std::thread t([&] { evb2.loopForever(); });
  evb2.runInEventBaseThreadAndWait([&] { socket_->attachEventBase(&evb2); });
  writeSocket->write(socket_->address(), folly::IOBuf::copyBuffer("hello"));
  auto now = std::chrono::steady_clock::now();
  while (
      packetsRecvd != 2 ||
      std::chrono::steady_clock::now() < now + std::chrono::milliseconds(10)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  evb2.runInEventBaseThread([&] {
    socket_ = nullptr;
    evb2.terminateLoopSoon();
  });
  t.join();
  EXPECT_EQ(packetsRecvd, 2);
}

MATCHER_P(HasCmsgs, cmsgs, "") {
  struct msghdr* msg = const_cast<struct msghdr*>(arg);
  if (msg == nullptr) {
    return false;
  }
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  folly::SocketCmsgMap sentCmsgs;

  struct cmsghdr* cmsg;
  for (cmsg = CMSG_FIRSTHDR(msg); cmsg != nullptr;
       cmsg = CMSG_NXTHDR(msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_UDP) {
      if (cmsg->cmsg_type == UDP_SEGMENT) {
        uint16_t gso;
        memcpy(
            &gso,
            reinterpret_cast<struct timespec*>(CMSG_DATA(cmsg)),
            sizeof(gso));
        sentCmsgs[{SOL_UDP, UDP_SEGMENT}] = gso;
      }
    }
    if (cmsg->cmsg_level == SOL_SOCKET) {
      if (cmsg->cmsg_type == SO_MARK) {
        uint32_t somark;
        memcpy(
            &somark,
            reinterpret_cast<struct timespec*>(CMSG_DATA(cmsg)),
            sizeof(somark));
        sentCmsgs[{SOL_SOCKET, SO_MARK}] = somark;
      }
      if (cmsg->cmsg_type == SO_PRIORITY) {
        uint32_t priority;
        memcpy(
            &priority,
            reinterpret_cast<struct timespec*>(CMSG_DATA(cmsg)),
            sizeof(priority));
        sentCmsgs[{SOL_SOCKET, SO_PRIORITY}] = priority;
      }
    }
    if (cmsg->cmsg_level == IPPROTO_IP) {
      if (cmsg->cmsg_type == IP_TOS) {
        uint32_t tos;
        memcpy(
            &tos,
            reinterpret_cast<struct timespec*>(CMSG_DATA(cmsg)),
            sizeof(tos));
        sentCmsgs[{IPPROTO_IP, IP_TOS}] = tos;
      }
      if (cmsg->cmsg_type == IP_TTL) {
        uint32_t ttl;
        memcpy(
            &ttl,
            reinterpret_cast<struct timespec*>(CMSG_DATA(cmsg)),
            sizeof(ttl));
        sentCmsgs[{IPPROTO_IP, IP_TTL}] = ttl;
      }
    }
    if (cmsg->cmsg_level == IPPROTO_IPV6) {
      if (cmsg->cmsg_type == IPV6_TCLASS) {
        uint32_t tclass;
        memcpy(
            &tclass,
            reinterpret_cast<struct timespec*>(CMSG_DATA(cmsg)),
            sizeof(tclass));
        sentCmsgs[{IPPROTO_IPV6, IPV6_TCLASS}] = tclass;
      }
      if (cmsg->cmsg_type == IPV6_HOPLIMIT) {
        uint32_t hoplimit;
        memcpy(
            &hoplimit,
            reinterpret_cast<struct timespec*>(CMSG_DATA(cmsg)),
            sizeof(hoplimit));
        sentCmsgs[{IPPROTO_IPV6, IPV6_HOPLIMIT}] = hoplimit;
      }
    }
  }
  return sentCmsgs == cmsgs;
#else
  return false;
#endif
}

MATCHER_P(HasNontrivialCmsgs, cmsgs, "") {
  struct msghdr* msg = const_cast<struct msghdr*>(arg);
  if (msg == nullptr) {
    return false;
  }
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  folly::SocketNontrivialCmsgMap sentCmsgs;

  struct cmsghdr* cmsg;
  for (cmsg = CMSG_FIRSTHDR(msg); cmsg != nullptr;
       cmsg = CMSG_NXTHDR(msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET) {
      if (cmsg->cmsg_type == SO_LINGER) {
        struct linger sl{
            .l_onoff = 0,
            .l_linger = 0,
        };

        memcpy(
            &sl, reinterpret_cast<struct linger*>(CMSG_DATA(cmsg)), sizeof(sl));
        sentCmsgs[{SOL_SOCKET, SO_LINGER}] =
            std::string(reinterpret_cast<const char*>(&sl), sizeof(sl));
      }
    }
  }
  return sentCmsgs == cmsgs;
#else
  return false;
#endif
}

TEST_F(AsyncUDPSocketTest, TestWriteCmsg) {
  folly::SocketAddress addr("127.0.0.1", 10000);
  auto netOpsDispatcher =
      std::make_shared<NiceMock<folly::netops::test::MockDispatcher>>();
  socket_->setOverrideNetOpsDispatcher(netOpsDispatcher);
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  // empty
  {
    folly::SocketCmsgMap cmsgs;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(cmsgs), _));
    socket_->write(addr, folly::IOBuf::copyBuffer("hey"));
  }
  // writeGSO
  {
    folly::SocketCmsgMap cmsgs;
    cmsgs[{SOL_UDP, UDP_SEGMENT}] = 1;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(cmsgs), _));
    socket_->writeGSO(
        addr,
        folly::IOBuf::copyBuffer("hey"),
        folly::AsyncUDPSocket::WriteOptions(
            1 /*gsoVal*/, false /* zerocopyVal*/));
  }
  // SO_MARK
  {
    folly::SocketCmsgMap cmsgs;
    cmsgs[{SOL_SOCKET, SO_MARK}] = 123;
    socket_->setCmsgs(cmsgs);
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(cmsgs), _));
    socket_->write(addr, folly::IOBuf::copyBuffer("hey"));
  }
  // append IP_TOS
  {
    folly::SocketCmsgMap cmsgs;
    cmsgs[{IPPROTO_IP, IP_TOS}] = 456;
    socket_->appendCmsgs(cmsgs);
    folly::SocketCmsgMap expectedCmsgs;
    expectedCmsgs[{IPPROTO_IP, IP_TOS}] = 456;
    expectedCmsgs[{SOL_SOCKET, SO_MARK}] = 123;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(expectedCmsgs), _));
    socket_->write(addr, folly::IOBuf::copyBuffer("hey"));
  }
  // append IP_TOS with a different value
  {
    folly::SocketCmsgMap cmsgs;
    cmsgs[{IPPROTO_IP, IP_TOS}] = 789;
    socket_->appendCmsgs(cmsgs);
    folly::SocketCmsgMap expectedCmsgs;
    expectedCmsgs[{IPPROTO_IP, IP_TOS}] = 789;
    expectedCmsgs[{SOL_SOCKET, SO_MARK}] = 123;
    socket_->setCmsgs(expectedCmsgs);
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(expectedCmsgs), _));
    socket_->write(addr, folly::IOBuf::copyBuffer("hey"));
  }
  // writeGSO with IP_TOS and SO_MARK
  {
    folly::SocketCmsgMap expectedCmsgs;
    expectedCmsgs[{IPPROTO_IP, IP_TOS}] = 789;
    expectedCmsgs[{SOL_SOCKET, SO_MARK}] = 123;
    expectedCmsgs[{SOL_UDP, UDP_SEGMENT}] = 1;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(expectedCmsgs), _));
    socket_->writeGSO(
        addr,
        folly::IOBuf::copyBuffer("hey"),
        folly::AsyncUDPSocket::WriteOptions(
            1 /*gsoVal*/, false /* zerocopyVal*/));
  }
#endif // FOLLY_HAVE_MSG_ERRQUEUE
  socket_->close();
}

TEST_F(AsyncUDPSocketTest, TestWriteWithCmsgs) {
  folly::SocketAddress addr("127.0.0.1", 10000);
  auto netOpsDispatcher =
      std::make_shared<NiceMock<folly::netops::test::MockDispatcher>>();
  socket_->setOverrideNetOpsDispatcher(netOpsDispatcher);
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  // With nothing set on the socket, the caller's cmsgs are what the kernel
  // sees, and whatever sendmsg returns is handed back to the caller unchanged.
  {
    folly::SocketCmsgMap cmsgs;
    cmsgs[{IPPROTO_IP, IP_TOS}] = 0x90;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(cmsgs), _))
        .WillOnce(Return(1234));
    EXPECT_EQ(
        1234,
        socket_->writeWithCmsgs(addr, folly::IOBuf::copyBuffer("hey"), cmsgs));
  }
  // A sendmsg failure is propagated rather than swallowed.
  {
    folly::SocketCmsgMap cmsgs;
    cmsgs[{IPPROTO_IP, IP_TOS}] = 0x90;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(cmsgs), _))
        .WillOnce(Return(-1));
    EXPECT_EQ(
        -1,
        socket_->writeWithCmsgs(addr, folly::IOBuf::copyBuffer("hey"), cmsgs));
  }
  // The IPv4 and IPv6 TOS cmsgs together, which is what a relay marking a
  // dual-stack socket per destination actually sends.
  {
    folly::SocketCmsgMap cmsgs;
    cmsgs[{IPPROTO_IP, IP_TOS}] = 0x90;
    cmsgs[{IPPROTO_IPV6, IPV6_TCLASS}] = 0x90;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(cmsgs), _));
    socket_->writeWithCmsgs(addr, folly::IOBuf::copyBuffer("hey"), cmsgs);
  }
  // Nothing is retained on the socket, so a plain write that follows is clean.
  {
    folly::SocketCmsgMap expectedCmsgs;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(expectedCmsgs), _));
    socket_->write(addr, folly::IOBuf::copyBuffer("hey"));
  }
  // Two writes with different values do not leak into each other. This is the
  // case a shared server socket hits when marking traffic per destination.
  {
    folly::SocketCmsgMap first;
    first[{IPPROTO_IP, IP_TOS}] = 0x90;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(first), _));
    socket_->writeWithCmsgs(addr, folly::IOBuf::copyBuffer("hey"), first);

    folly::SocketCmsgMap second;
    second[{IPPROTO_IP, IP_TOS}] = 0x00;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(second), _));
    socket_->writeWithCmsgs(addr, folly::IOBuf::copyBuffer("hey"), second);
  }
  // With no socket-level cmsgs, an empty map sends no ancillary data at all.
  {
    folly::SocketCmsgMap empty;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(empty), _));
    socket_->writeWithCmsgs(addr, folly::IOBuf::copyBuffer("hey"), empty);
  }
  // More entries than the stack threshold, so this takes the heap path.
  {
    folly::SocketCmsgMap many;
    many[{IPPROTO_IP, IP_TOS}] = 0x90;
    many[{IPPROTO_IP, IP_TTL}] = 64;
    many[{IPPROTO_IPV6, IPV6_TCLASS}] = 0x90;
    many[{IPPROTO_IPV6, IPV6_HOPLIMIT}] = 64;
    many[{SOL_SOCKET, SO_MARK}] = 123;
    many[{SOL_SOCKET, SO_PRIORITY}] = 6;
    ASSERT_GT(many.size(), 5u) << "must exceed kSmallSizeMax to reach the heap";
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(many), _));
    socket_->writeWithCmsgs(addr, folly::IOBuf::copyBuffer("hey"), many);
  }
  // Socket-level cmsgs are the baseline: a key the caller does not mention
  // still goes out, and a key present in both takes the per-write value.
  folly::SocketCmsgMap socketLevel;
  socketLevel[{SOL_SOCKET, SO_MARK}] = 123;
  socketLevel[{IPPROTO_IP, IP_TOS}] = 0x20;
  socket_->setCmsgs(socketLevel);
  {
    folly::SocketCmsgMap perWrite;
    perWrite[{IPPROTO_IP, IP_TOS}] = 0x90;

    folly::SocketCmsgMap expectedCmsgs;
    expectedCmsgs[{SOL_SOCKET, SO_MARK}] = 123;
    expectedCmsgs[{IPPROTO_IP, IP_TOS}] = 0x90;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(expectedCmsgs), _));
    socket_->writeWithCmsgs(addr, folly::IOBuf::copyBuffer("hey"), perWrite);
  }
  // An empty map overrides nothing, so the socket-level cmsgs still go out.
  {
    folly::SocketCmsgMap empty;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(socketLevel), _));
    socket_->writeWithCmsgs(addr, folly::IOBuf::copyBuffer("hey"), empty);
  }
  // Nothing the caller passed is retained, so a plain write that follows
  // carries the socket-level cmsgs and only those.
  {
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(socketLevel), _));
    socket_->write(addr, folly::IOBuf::copyBuffer("hey"));
  }
  // Three levels of precedence, with all three claiming IP_TOS and the lower
  // two claiming SO_MARK. The relative order of the lower two is the existing
  // write() behaviour, which lets the dynamic cmsgs win.
  {
    socket_->setAdditionalCmsgsFunc([]() {
      folly::SocketCmsgMap additionalCmsgs;
      additionalCmsgs[{IPPROTO_IP, IP_TTL}] = 64;
      additionalCmsgs[{SOL_SOCKET, SO_MARK}] = 999;
      additionalCmsgs[{IPPROTO_IP, IP_TOS}] = 0x11;
      return additionalCmsgs;
    });

    folly::SocketCmsgMap perWrite;
    perWrite[{IPPROTO_IP, IP_TOS}] = 0x90;

    folly::SocketCmsgMap expectedCmsgs;
    expectedCmsgs[{IPPROTO_IP, IP_TTL}] = 64; // only the func sets this
    expectedCmsgs[{SOL_SOCKET, SO_MARK}] = 999; // the func beats setCmsgs
    expectedCmsgs[{IPPROTO_IP, IP_TOS}] = 0x90; // the caller beats both
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(expectedCmsgs), _));
    socket_->writeWithCmsgs(addr, folly::IOBuf::copyBuffer("hey"), perWrite);
  }
  // setNontrivialCmsgs() entries carry values the per-write map cannot
  // express, so they are always sent alongside it.
  {
    socket_->setAdditionalCmsgsFunc(AsyncUDPSocket::AdditionalCmsgsFunc{});
    socket_->setCmsgs(folly::SocketCmsgMap{});

    struct linger sl{
        .l_onoff = 1,
        .l_linger = 123,
    };
    folly::SocketNontrivialCmsgMap nontrivialCmsgs;
    nontrivialCmsgs[{SOL_SOCKET, SO_LINGER}] =
        std::string(reinterpret_cast<const char*>(&sl), sizeof(sl));
    socket_->setNontrivialCmsgs(nontrivialCmsgs);

    folly::SocketCmsgMap perWrite;
    perWrite[{IPPROTO_IP, IP_TOS}] = 0x90;
    EXPECT_CALL(
        *netOpsDispatcher,
        sendmsg(
            _,
            AllOf(HasCmsgs(perWrite), HasNontrivialCmsgs(nontrivialCmsgs)),
            _));
    socket_->writeWithCmsgs(addr, folly::IOBuf::copyBuffer("hey"), perWrite);
  }
#elif !defined(_WIN32)
  // Platforms with neither MSG_ERRQUEUE nor the Windows cmsg path cannot carry
  // ancillary data at all, so a non-empty map is rejected before the socket is
  // touched.
  {
    folly::SocketCmsgMap cmsgs;
    cmsgs[{IPPROTO_IP, IP_TOS}] = 0x90;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, _, _)).Times(0);
    errno = 0;
    EXPECT_EQ(
        -1,
        socket_->writeWithCmsgs(addr, folly::IOBuf::copyBuffer("hey"), cmsgs));
    EXPECT_EQ(EOPNOTSUPP, errno);
    testing::Mock::VerifyAndClearExpectations(netOpsDispatcher.get());
  }
  // An empty map asks for no ancillary data, so the write still goes out.
  {
    folly::SocketCmsgMap empty;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, _, _)).Times(1);
    socket_->writeWithCmsgs(addr, folly::IOBuf::copyBuffer("hey"), empty);
  }
#endif // FOLLY_HAVE_MSG_ERRQUEUE
  socket_->close();
}

TEST_F(AsyncUDPSocketTest, TestWriteWithCmsgsErrorPaths) {
  folly::SocketAddress addr("127.0.0.1", 10000);
  auto netOpsDispatcher =
      std::make_shared<NiceMock<folly::netops::test::MockDispatcher>>();
  socket_->setOverrideNetOpsDispatcher(netOpsDispatcher);

  folly::SocketCmsgMap cmsgs;
  cmsgs[{IPPROTO_IP, IP_TOS}] = 0x90;

  // A null buf is a caller error and must not reach the socket.
  EXPECT_CALL(*netOpsDispatcher, sendmsg(_, _, _)).Times(0);
  errno = 0;
  std::unique_ptr<folly::IOBuf> nullBuf;
  EXPECT_EQ(-1, socket_->writeWithCmsgs(addr, nullBuf, cmsgs));
  EXPECT_EQ(EINVAL, errno);

  // setNontrivialCmsgs() entries are unsupported on the single-IOBuf
  // writeChain() path (only writeWithCmsgs() and friends serialize them).
  {
    struct linger sl{
        .l_onoff = 1,
        .l_linger = 123,
    };
    folly::SocketNontrivialCmsgMap nontrivialCmsgs;
    nontrivialCmsgs[{SOL_SOCKET, SO_LINGER}] =
        std::string(reinterpret_cast<const char*>(&sl), sizeof(sl));
    socket_->setNontrivialCmsgs(nontrivialCmsgs);

    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, _, _)).Times(0);
    errno = 0;
    EXPECT_EQ(
        -1,
        socket_->writeChain(
            addr,
            folly::IOBuf::copyBuffer("hey"),
            folly::AsyncUDPSocket::WriteOptions(
                0 /*gsoVal*/, false /* zerocopyVal*/)));
    EXPECT_EQ(EOPNOTSUPP, errno);
    testing::Mock::VerifyAndClearExpectations(netOpsDispatcher.get());
    socket_->setNontrivialCmsgs(folly::SocketNontrivialCmsgMap{});
  }

  // On a connected socket, sending anywhere else is rejected.
  socket_->connect(addr);
  folly::SocketAddress other("127.0.0.1", 10001);
  errno = 0;
  EXPECT_EQ(
      -1,
      socket_->writeWithCmsgs(other, folly::IOBuf::copyBuffer("hey"), cmsgs));
  EXPECT_EQ(ENOTSUP, errno);

  socket_->close();
}

TEST_F(AsyncUDPSocketTest, TestWriteDynamicCmsg) {
  folly::SocketAddress addr("127.0.0.1", 10000);
  auto netOpsDispatcher =
      std::make_shared<NiceMock<folly::netops::test::MockDispatcher>>();
  socket_->setOverrideNetOpsDispatcher(netOpsDispatcher);

  MockFunction<AsyncUDPSocket::AdditionalCmsgsFunc> mockAdditionalCmsgs;
  int mockCallCount = 1;
  socket_->setAdditionalCmsgsFunc([&mockCallCount]() {
    folly::SocketCmsgMap additionalCmsgs;
    additionalCmsgs[{IPPROTO_IP, IP_TTL}] = mockCallCount++;
    return additionalCmsgs;
  });

#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  // Dynamic cmsgs only (IP_TTL)
  {
    folly::SocketCmsgMap expectedCmsgs;
    expectedCmsgs[{IPPROTO_IP, IP_TTL}] = mockCallCount;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(expectedCmsgs), _));
    socket_->write(addr, folly::IOBuf::copyBuffer("hey"));
  }
  // writeGSO with dynamic cmsgs (IP_TTL)
  {
    folly::SocketCmsgMap expectedCmsgs;
    expectedCmsgs[{SOL_UDP, UDP_SEGMENT}] = 1;
    expectedCmsgs[{IPPROTO_IP, IP_TTL}] = mockCallCount;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(expectedCmsgs), _));
    socket_->writeGSO(
        addr,
        folly::IOBuf::copyBuffer("hey"),
        folly::AsyncUDPSocket::WriteOptions(
            1 /*gsoVal*/, false /* zerocopyVal*/));
  }
  // SO_MARK with dynamic cmsgs (IP_TTL)
  {
    folly::SocketCmsgMap cmsgs;
    cmsgs[{SOL_SOCKET, SO_MARK}] = 123;
    socket_->setCmsgs(cmsgs);
    folly::SocketCmsgMap expectedCmsgs;
    expectedCmsgs[{SOL_SOCKET, SO_MARK}] = 123;
    expectedCmsgs[{IPPROTO_IP, IP_TTL}] = mockCallCount;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(expectedCmsgs), _));
    socket_->write(addr, folly::IOBuf::copyBuffer("hey"));
  }
  // append IP_TOS + IP_TTL + overwrite with dynamic cmsgs (IP_TTL)
  {
    folly::SocketCmsgMap cmsgs;
    cmsgs[{IPPROTO_IP, IP_TOS}] = 456;
    cmsgs[{IPPROTO_IP, IP_TTL}] = 9999;
    socket_->appendCmsgs(cmsgs);
    folly::SocketCmsgMap expectedCmsgs;
    expectedCmsgs[{IPPROTO_IP, IP_TOS}] = 456;
    expectedCmsgs[{SOL_SOCKET, SO_MARK}] = 123;
    expectedCmsgs[{IPPROTO_IP, IP_TTL}] = mockCallCount;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(expectedCmsgs), _));
    socket_->write(addr, folly::IOBuf::copyBuffer("hey"));
  }
  // append IP_TOS with a different value + dynamic cmsgs (IP_TTL)
  {
    folly::SocketCmsgMap cmsgs;
    cmsgs[{IPPROTO_IP, IP_TOS}] = 789;
    socket_->appendCmsgs(cmsgs);
    folly::SocketCmsgMap expectedCmsgs;
    expectedCmsgs[{IPPROTO_IP, IP_TOS}] = 789;
    expectedCmsgs[{SOL_SOCKET, SO_MARK}] = 123;
    expectedCmsgs[{IPPROTO_IP, IP_TTL}] = mockCallCount;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(expectedCmsgs), _));
    socket_->write(addr, folly::IOBuf::copyBuffer("hey"));
  }
  // writeGSO with IP_TOS and SO_MARK + dynamic cmsgs (IP_TTL)
  {
    folly::SocketCmsgMap expectedCmsgs;
    expectedCmsgs[{IPPROTO_IP, IP_TOS}] = 789;
    expectedCmsgs[{SOL_SOCKET, SO_MARK}] = 123;
    expectedCmsgs[{SOL_UDP, UDP_SEGMENT}] = 1;
    expectedCmsgs[{IPPROTO_IP, IP_TTL}] = mockCallCount;
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(expectedCmsgs), _));
    socket_->writeGSO(
        addr,
        folly::IOBuf::copyBuffer("hey"),
        folly::AsyncUDPSocket::WriteOptions(
            1 /*gsoVal*/, false /* zerocopyVal*/));
  }
  // Empty dynamic cmsgs should revert to default cmsgs
  socket_->setAdditionalCmsgsFunc([]() { return folly::none; });
  {
    folly::SocketCmsgMap expectedCmsgs;
    expectedCmsgs[{IPPROTO_IP, IP_TOS}] = 789;
    expectedCmsgs[{SOL_SOCKET, SO_MARK}] = 123;
    expectedCmsgs[{IPPROTO_IP, IP_TTL}] = 9999; // This won't be overwritten
    EXPECT_CALL(*netOpsDispatcher, sendmsg(_, HasCmsgs(expectedCmsgs), _));
    socket_->write(addr, folly::IOBuf::copyBuffer("hey"));
  }

#endif // FOLLY_HAVE_MSG_ERRQUEUE
  socket_->close();
}

MATCHER_P2(AllHaveCmsgs, cmsgs, count, "") {
  if (arg == nullptr) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    auto msg = arg[i].msg_hdr;
    if (!Matches(HasCmsgs(cmsgs))(&msg)) {
      return false;
    }
  }
  return true;
}

MATCHER_P3(AllHaveCmsgsAndNontrivialCmsgs, cmsgs, nontrivialCmsgs, count, "") {
  if (arg == nullptr) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    auto msg = arg[i].msg_hdr;
    if (!Matches(HasCmsgs(cmsgs))(&msg) &&
        !Matches(HasNontrivialCmsgs(nontrivialCmsgs))(&msg)) {
      return false;
    }
  }
  return true;
}

TEST(MatcherTest, AllHaveCmsgsTest) {
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  constexpr size_t count = 2;
  constexpr size_t controlSize = 2;
  mmsghdr msgvec[count];
  char control[count * controlSize * CMSG_SPACE(sizeof(uint16_t))];
  memset(control, 0, sizeof(control));
  // two messages, one with all cmsgs, the other with only one
  {
    auto& msg = msgvec[0].msg_hdr;
    msg.msg_control = &control[0];
    msg.msg_controllen = controlSize * CMSG_SPACE(sizeof(int));
    struct cmsghdr* cm = nullptr;
    cm = CMSG_FIRSTHDR(&msg);
    int val1 = 20;
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SO_MARK;
    cm->cmsg_len = CMSG_LEN(sizeof(val1));
    memcpy(CMSG_DATA(cm), &val1, sizeof(val1));
    cm = CMSG_NXTHDR(&msg, cm);
    int val2 = 30;
    cm->cmsg_level = IPPROTO_IP;
    cm->cmsg_type = IP_TOS;
    cm->cmsg_len = CMSG_LEN(sizeof(val2));
    memcpy(CMSG_DATA(cm), &val2, sizeof(val2));
  }
  {
    auto& msg = msgvec[1].msg_hdr;
    msg.msg_control = &control[controlSize * CMSG_SPACE(sizeof(uint16_t))];
    msg.msg_controllen = CMSG_SPACE(sizeof(int));
    struct cmsghdr* cm = nullptr;
    cm = CMSG_FIRSTHDR(&msg);
    int val = 20;
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SO_MARK;
    cm->cmsg_len = CMSG_LEN(sizeof(val));
    memcpy(CMSG_DATA(cm), &val, sizeof(val));
  }
  folly::SocketCmsgMap cmsgs;
  cmsgs[{SOL_SOCKET, SO_MARK}] = 20;
  cmsgs[{IPPROTO_IP, IP_TOS}] = 30;
  struct mmsghdr* msgvecPtr = nullptr;
  EXPECT_THAT(msgvecPtr, Not(AllHaveCmsgs(cmsgs, count)));
  msgvecPtr = msgvec;
  EXPECT_THAT(msgvecPtr, Not(AllHaveCmsgs(cmsgs, count)));
  // true cases are tested in TestWritemCmsg
#endif // FOLLY_HAVE_MSG_ERRQUEUE
}

TEST_F(AsyncUDPSocketTest, TestWritemCmsg) {
  folly::SocketAddress addr("127.0.0.1", 10000);
  auto netOpsDispatcher =
      std::make_shared<NiceMock<folly::netops::test::MockDispatcher>>();
  socket_->setOverrideNetOpsDispatcher(netOpsDispatcher);
  std::vector<std::unique_ptr<folly::IOBuf>> bufs;
  bufs.emplace_back(folly::IOBuf::copyBuffer("hey1"));
  bufs.emplace_back(folly::IOBuf::copyBuffer("hey2"));
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  // empty
  {
    folly::SocketCmsgMap cmsgs;
    EXPECT_CALL(
        *netOpsDispatcher, sendmmsg(_, AllHaveCmsgs(cmsgs, bufs.size()), _, _));
    socket_->writem(folly::range(&addr, &addr + 1), bufs.data(), bufs.size());
  }
  // set IP_TOS & SO_MARK
  {
    folly::SocketCmsgMap cmsgs;
    cmsgs[{IPPROTO_IP, IP_TOS}] = 456;
    cmsgs[{SOL_SOCKET, SO_MARK}] = 123;
    socket_->setCmsgs(cmsgs);
    EXPECT_CALL(
        *netOpsDispatcher, sendmmsg(_, AllHaveCmsgs(cmsgs, bufs.size()), _, _));
    socket_->writem(folly::range(&addr, &addr + 1), bufs.data(), bufs.size());
  }
  // writemGSO
  {
    folly::SocketCmsgMap expectedCmsgs;
    expectedCmsgs[{IPPROTO_IP, IP_TOS}] = 456;
    expectedCmsgs[{SOL_SOCKET, SO_MARK}] = 123;
    expectedCmsgs[{SOL_UDP, UDP_SEGMENT}] = 1;
    EXPECT_CALL(
        *netOpsDispatcher,
        sendmmsg(_, AllHaveCmsgs(expectedCmsgs, bufs.size()), _, _));
    std::vector<folly::AsyncUDPSocket::WriteOptions> options{
        {1, false}, {1, false}};
    socket_->writemGSO(
        folly::range(&addr, &addr + 1),
        bufs.data(),
        bufs.size(),
        options.data());
  }
#endif // FOLLY_HAVE_MSG_ERRQUEUE
  socket_->close();
}

TEST_F(AsyncUDPSocketTest, TestWritemDynamicCmsg) {
  folly::SocketAddress addr("127.0.0.1", 10000);
  auto netOpsDispatcher =
      std::make_shared<NiceMock<folly::netops::test::MockDispatcher>>();
  socket_->setOverrideNetOpsDispatcher(netOpsDispatcher);
  std::vector<std::unique_ptr<folly::IOBuf>> bufs;
  bufs.emplace_back(folly::IOBuf::copyBuffer("hey1"));
  bufs.emplace_back(folly::IOBuf::copyBuffer("hey2"));

  MockFunction<AsyncUDPSocket::AdditionalCmsgsFunc> mockAdditionalCmsgs;
  int mockCallCount = 1;
  socket_->setAdditionalCmsgsFunc([&mockCallCount]() {
    folly::SocketCmsgMap additionalCmsgs;
    additionalCmsgs[{IPPROTO_IP, IP_TTL}] = mockCallCount++;
    return additionalCmsgs;
  });

#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  // Dynamic cmsgs only (IP_TTL)
  {
    folly::SocketCmsgMap expectedCmsgs;
    expectedCmsgs[{IPPROTO_IP, IP_TTL}] = mockCallCount;
    EXPECT_CALL(
        *netOpsDispatcher,
        sendmmsg(_, AllHaveCmsgs(expectedCmsgs, bufs.size()), _, _));
    socket_->writem(folly::range(&addr, &addr + 1), bufs.data(), bufs.size());
  }
  // set IP_TOS & SO_MARK & IP_TTL + overwrite from dynamic cmsgs (IP_TTL)
  {
    folly::SocketCmsgMap cmsgs;
    cmsgs[{IPPROTO_IP, IP_TOS}] = 456;
    cmsgs[{SOL_SOCKET, SO_MARK}] = 123;
    cmsgs[{IPPROTO_IP, IP_TTL}] = 9999;
    socket_->setCmsgs(cmsgs);
    auto expectedCmsgs = cmsgs;
    expectedCmsgs[{IPPROTO_IP, IP_TTL}] = mockCallCount;
    EXPECT_CALL(
        *netOpsDispatcher,
        sendmmsg(_, AllHaveCmsgs(expectedCmsgs, bufs.size()), _, _));
    socket_->writem(folly::range(&addr, &addr + 1), bufs.data(), bufs.size());
  }
  // writemGSO + dynamic cmsgs (IP_TTL)
  {
    folly::SocketCmsgMap expectedCmsgs;
    expectedCmsgs[{IPPROTO_IP, IP_TOS}] = 456;
    expectedCmsgs[{SOL_SOCKET, SO_MARK}] = 123;
    expectedCmsgs[{SOL_UDP, UDP_SEGMENT}] = 1;
    expectedCmsgs[{IPPROTO_IP, IP_TTL}] = mockCallCount;
    EXPECT_CALL(
        *netOpsDispatcher,
        sendmmsg(_, AllHaveCmsgs(expectedCmsgs, bufs.size()), _, _));
    std::vector<folly::AsyncUDPSocket::WriteOptions> options{
        {1, false}, {1, false}};
    socket_->writemGSO(
        folly::range(&addr, &addr + 1),
        bufs.data(),
        bufs.size(),
        options.data());
  }
#endif // FOLLY_HAVE_MSG_ERRQUEUE
  socket_->close();
}

TEST_F(AsyncUDPSocketTest, TestApplyStringOptionsPostBind) {
  EventBase evb;
  AsyncUDPSocket socket(&evb);
  ASSERT_FALSE(socket.isBound());
  folly::SocketAddress address("127.0.0.1", 443);
  socket.connect(address);
  ASSERT_TRUE(socket.isBound());

  const auto& localAddr = socket.address();
  ASSERT_TRUE(localAddr.isInitialized());
  ASSERT_GT(localAddr.getPort(), 0);

  folly::SocketOptionMap options = folly::emptySocketOptionMap;
  struct linger sl{
      .l_onoff = 1,
      .l_linger = 123,
  };

  options.insert(
      {folly::SocketOptionKey{SOL_SOCKET, SO_LINGER},
       std::string((char*)&sl, sizeof(sl))});

  socket.applyOptions(options, folly::SocketOptionKey::ApplyPos::POST_BIND);
}

TEST_F(AsyncUDPSocketTest, TestWritemNontrivialCmsgs) {
  folly::SocketAddress addr("127.0.0.1", 10001);
  auto netOpsDispatcher =
      std::make_shared<NiceMock<folly::netops::test::MockDispatcher>>();
  socket_->setOverrideNetOpsDispatcher(netOpsDispatcher);
  std::vector<std::unique_ptr<folly::IOBuf>> bufs;
  bufs.emplace_back(folly::IOBuf::copyBuffer("hey1"));
  bufs.emplace_back(folly::IOBuf::copyBuffer("hey2"));
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  // empty
  {
    folly::SocketCmsgMap expectedCmsgs;
    folly::SocketNontrivialCmsgMap expectedNontrivialCmsgs;
    EXPECT_CALL(
        *netOpsDispatcher,
        sendmmsg(
            _,
            AllHaveCmsgsAndNontrivialCmsgs(
                expectedCmsgs, expectedNontrivialCmsgs, bufs.size()),
            _,
            _));
    socket_->writem(folly::range(&addr, &addr + 1), bufs.data(), bufs.size());
  }
  // set IP_TOS & SO_MARK
  {
    folly::SocketCmsgMap expectedCmsgs;
    folly::SocketNontrivialCmsgMap expectedNontrivialCmsgs;
    struct linger sl{
        .l_onoff = 1,
        .l_linger = 123,
    };
    expectedCmsgs[{IPPROTO_IP, IP_TOS}] = 456;
    expectedCmsgs[{SOL_SOCKET, SO_MARK}] = 123;
    expectedNontrivialCmsgs[{SOL_SOCKET, SO_LINGER}] =
        std::string(reinterpret_cast<const char*>(&sl), sizeof(sl));
    socket_->setCmsgs(expectedCmsgs);
    socket_->setNontrivialCmsgs(expectedNontrivialCmsgs);
    EXPECT_CALL(
        *netOpsDispatcher,
        sendmmsg(
            _,
            AllHaveCmsgsAndNontrivialCmsgs(
                expectedCmsgs, expectedNontrivialCmsgs, bufs.size()),
            _,
            _));
    socket_->writem(folly::range(&addr, &addr + 1), bufs.data(), bufs.size());
  }
  // writemGSO
  {
    folly::SocketCmsgMap expectedCmsgs;
    folly::SocketNontrivialCmsgMap expectedNontrivialCmsgs;
    struct linger sl{
        .l_onoff = 1,
        .l_linger = 123,
    };
    expectedCmsgs[{IPPROTO_IP, IP_TOS}] = 456;
    expectedCmsgs[{SOL_SOCKET, SO_MARK}] = 123;
    expectedCmsgs[{SOL_UDP, UDP_SEGMENT}] = 1;
    expectedNontrivialCmsgs[{SOL_SOCKET, SO_LINGER}] =
        std::string(reinterpret_cast<const char*>(&sl), sizeof(sl));
    EXPECT_CALL(
        *netOpsDispatcher,
        sendmmsg(
            _,
            AllHaveCmsgsAndNontrivialCmsgs(
                expectedCmsgs, expectedNontrivialCmsgs, bufs.size()),
            _,
            _));
    std::vector<folly::AsyncUDPSocket::WriteOptions> options{
        {1, false}, {1, false}};
    socket_->writemGSO(
        folly::range(&addr, &addr + 1),
        bufs.data(),
        bufs.size(),
        options.data());
  }
#endif // FOLLY_HAVE_MSG_ERRQUEUE
  socket_->close();
}
