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

#include <folly/ssl/SSLSession.h>

#include <memory>

#include <folly/io/async/testing/AsyncSSLSocketTest.h>
#include <folly/net/NetOps.h>
#include <folly/net/NetworkSocket.h>
#include <folly/portability/GTest.h>
#include <folly/portability/OpenSSL.h>
#include <folly/portability/Sockets.h>
#include <folly/ssl/detail/OpenSSLSession.h>
#include <folly/testing/TestUtil.h>

using folly::ssl::SSLSession;
using folly::ssl::detail::OpenSSLSession;

using namespace folly;
using namespace folly::test;

class SSLSessionTest : public testing::Test {
 public:
  void SetUp() override {
    clientCtx_.reset(new folly::SSLContext());
    dfServerCtx_.reset(new folly::SSLContext());
    hskServerCtx_.reset(new folly::SSLContext());
    serverName_ = "xyz.newdev.facebook.com";
    getctx(clientCtx_, dfServerCtx_);
  }

  void TearDown() override {}

  void getfds(NetworkSocket fds[2]) {
    if (netops::socketpair(PF_LOCAL, SOCK_STREAM, 0, fds) != 0) {
      FAIL() << "failed to create socketpair: " << errnoStr(errno);
    }
    for (int idx = 0; idx < 2; ++idx) {
      if (netops::set_socket_non_blocking(fds[idx]) != 0) {
        FAIL() << "failed to put socket " << idx
               << " in non-blocking mode: " << errnoStr(errno);
      }
    }
  }

  void getctx(
      std::shared_ptr<folly::SSLContext> clientCtx,
      std::shared_ptr<folly::SSLContext> serverCtx) {
    clientCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
    clientCtx->loadTrustedCertificates(find_resource(kTestCA).string().c_str());

    serverCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
    serverCtx->loadCertificate(find_resource(kTestCert).string().c_str());
    serverCtx->loadPrivateKey(find_resource(kTestKey).string().c_str());
  }

  folly::EventBase eventBase_;
  std::shared_ptr<SSLContext> clientCtx_;
  std::shared_ptr<SSLContext> dfServerCtx_;
  // Use the same SSLContext to continue the handshake after
  // tlsext_hostname match.
  std::shared_ptr<SSLContext> hskServerCtx_;
  std::string serverName_;
};

// TLS 1.2 and TLS 1.3 deliver session tickets in different ways, but we can use
// SSLContext::SessionLifecycleCallbacks to receive them in a similar manner so
// tests can work regardless of version.
class SimpleSessionLifecycleCallback
    : public SSLContext::SessionLifecycleCallbacks {
 public:
  void onNewSession(SSL*, ssl::SSLSessionUniquePtr session) override {
    // This can be called multiple times. OpenSSL sends two session tickets by
    // default). Grab the last one.
    session_ = std::move(session);
    ASSERT_TRUE(socket_ != nullptr);
    // At this point we have what we need to resume a session. Detach the
    // ReadCallback, allowing the socket's EventBase to stop looping.
    socket_->setReadCB(nullptr);
  }

  // set when session is available
  ssl::SSLSessionUniquePtr session_;
  // set after object construction
  folly::AsyncSSLSocket* socket_;
};

class SimpleReadCallback : public AsyncTransport::ReadCallback {
 public:
  void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
    *bufReturn = buffer_;
    *lenReturn = sizeof(buffer_);
  }

  void readDataAvailable(size_t) noexcept override {
    // this callback should only be used to read session tickets, which
    // aren't delivered to callbacks
    FAIL();
  }

  void readEOF() noexcept override { FAIL(); }

  void readErr(const AsyncSocketException& ex) noexcept override {
    FAIL() << ex;
  }

  char buffer_[1024];
};

TEST_F(SSLSessionTest, BasicTest) {
  ssl::SSLSessionUniquePtr sslSession;
  // Full handshake
  {
    NetworkSocket fds[2];
    getfds(fds);
    auto sessionCb = std::make_unique<SimpleSessionLifecycleCallback>();
    auto sessionCbPtr = sessionCb.get();
    clientCtx_->setSessionLifecycleCallbacks(std::move(sessionCb));

    AsyncSSLSocket::UniquePtr clientSock(
        new AsyncSSLSocket(clientCtx_, &eventBase_, fds[0], serverName_));
    auto clientPtr = clientSock.get();

    AsyncSSLSocket::UniquePtr serverSock(
        new AsyncSSLSocket(dfServerCtx_, &eventBase_, fds[1], true));
    SSLHandshakeClient client(std::move(clientSock), false, false);
    SSLHandshakeServerParseClientHello server(
        std::move(serverSock), false, false);
    sessionCbPtr->socket_ = clientPtr;
    SimpleReadCallback readCb;
    // register read callback to read incoming session tickets (for TLS 1.3)
    clientPtr->setReadCB(&readCb);
    // should stop when the session ticket is received
    eventBase_.loop();
    ASSERT_TRUE(client.handshakeSuccess_);
    sslSession = std::move(sessionCbPtr->session_);
    ASSERT_TRUE(sslSession != nullptr);
    ASSERT_FALSE(clientPtr->getSSLSessionReused());
  }

  // Session resumption
  {
    NetworkSocket fds[2];
    getfds(fds);
    AsyncSSLSocket::UniquePtr clientSock(
        new AsyncSSLSocket(clientCtx_, &eventBase_, fds[0], serverName_));
    auto clientPtr = clientSock.get();

    clientPtr->setRawSSLSession(std::move(sslSession));

    AsyncSSLSocket::UniquePtr serverSock(
        new AsyncSSLSocket(dfServerCtx_, &eventBase_, fds[1], true));
    SSLHandshakeClient client(std::move(clientSock), false, false);
    SSLHandshakeServerParseClientHello server(
        std::move(serverSock), false, false);

    eventBase_.loop();
    ASSERT_TRUE(client.handshakeSuccess_);
    ASSERT_TRUE(clientPtr->getSSLSessionReused());
  }
}

TEST_F(SSLSessionTest, NullSessionResumptionTest) {
  // Set null session, should result in full handshake
  {
    NetworkSocket fds[2];
    getfds(fds);
    AsyncSSLSocket::UniquePtr clientSock(
        new AsyncSSLSocket(clientCtx_, &eventBase_, fds[0], serverName_));
    auto clientPtr = clientSock.get();

    clientPtr->setSSLSession(nullptr);

    AsyncSSLSocket::UniquePtr serverSock(
        new AsyncSSLSocket(dfServerCtx_, &eventBase_, fds[1], true));
    SSLHandshakeClient client(std::move(clientSock), false, false);
    SSLHandshakeServerParseClientHello server(
        std::move(serverSock), false, false);

    eventBase_.loop();
    ASSERT_TRUE(client.handshakeSuccess_);
    ASSERT_FALSE(clientPtr->getSSLSessionReused());
  }
}
