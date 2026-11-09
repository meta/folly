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

#include <folly/io/async/test/AsyncSSLSocketTest.h>

#include <fcntl.h>
#include <signal.h>

#include <iostream>
#include <list>
#include <set>
#include <thread>

#include <folly/SocketAddress.h>
#include <folly/String.h>
#include <folly/io/Cursor.h>
#include <folly/io/async/AsyncPipe.h>
#include <folly/io/async/AsyncSSLSocket.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseThread.h>
#include <folly/io/async/SSLOptions.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/io/async/ssl/BasicTransportCertificate.h>
#include <folly/io/async/ssl/OpenSSLTransportCertificate.h>
#include <folly/io/async/test/BlockingSocket.h>
#include <folly/io/async/test/MockAsyncSocketLegacyObserver.h>
#include <folly/io/async/test/TFOUtil.h>
#include <folly/io/async/test/TestSSLServer.h>
#include <folly/net/NetOps.h>
#include <folly/net/NetworkSocket.h>
#include <folly/net/test/MockNetOpsDispatcher.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <folly/portability/OpenSSL.h>
#include <folly/portability/Unistd.h>
#include <folly/testing/TestUtil.h>

#ifdef __linux__
#include <dlfcn.h>
#endif

using std::cerr;
using std::endl;
using std::string;

using namespace folly;
using namespace folly::test;
using namespace testing;

namespace {

#if defined __linux__
// to store libc's original setsockopt()
using setsockopt_ptr = int (*)(int, int, int, const void*, socklen_t);
setsockopt_ptr real_setsockopt_ = nullptr;

// global struct to initialize before main runs. we can init within a test,
// or in main, but this method seems to be least intrsive and universal
struct GlobalStatic {
  GlobalStatic() {
    real_setsockopt_ = (setsockopt_ptr)dlsym(RTLD_NEXT, "setsockopt");
  }
  void reset() noexcept { ttlsDisabledSet.clear(); }
  // for each fd, tracks whether TTLS is disabled or not
  std::unordered_set<folly::NetworkSocket /* fd */> ttlsDisabledSet;
};

// the constructor will be called before main() which is all we care about
GlobalStatic globalStatic;

} // namespace

// Intercepting setsockopt to test system behavior with disabled TTLS.
// this name has to be global
int setsockopt(
    int sockfd, int level, int optname, const void* optval, socklen_t optlen) {
  if (optname == FOLLY_SO_TTLS_TRUSTED && optval != nullptr) {
    __u8 optValue = *(__u8*)optval;
    if (optValue == FOLLY_SO_TTLS_TRUSTED_VAL_ENCRYPTED) {
      globalStatic.ttlsDisabledSet.insert(folly::NetworkSocket::fromFd(sockfd));
      return 0;
    }
  }
  return real_setsockopt_(sockfd, level, optname, optval, optlen);
}
#endif

namespace {

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

  serverCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  serverCtx->loadCertificate(find_resource(kTestCert).c_str());
  serverCtx->loadPrivateKey(find_resource(kTestKey).c_str());
}

std::string getFileAsBuf(const char* fileName) {
  std::string buffer;
  folly::readFile(find_resource(fileName).c_str(), buffer);
  return buffer;
}

folly::ssl::X509UniquePtr readCertFromFile(const std::string& filename) {
  auto path = find_resource(filename);
  folly::ssl::BioUniquePtr bio(BIO_new(BIO_s_file()));
  if (!bio) {
    throw std::runtime_error("Couldn't create BIO");
  }

  if (BIO_read_filename(bio.get(), path.c_str()) != 1) {
    throw std::runtime_error("Couldn't read cert file: " + filename);
  }
  return folly::ssl::X509UniquePtr(
      PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
}

void connectWriteReadClose(
    folly::AsyncReader::ReadCallback::ReadMode readMode) {
  // Start listening on a local port
  folly::test::WriteCallbackBase writeCallback;
  folly::test::ReadCallback readCallback(&writeCallback);
  folly::test::HandshakeCallback handshakeCallback(&readCallback);
  folly::test::SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  folly::test::TestSSLServer server(&acceptCallback);

  // Set up SSL context.
  auto sslContext = std::make_shared<folly::SSLContext>();
  sslContext->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  // sslContext->loadTrustedCertificates("./trusted-ca-certificate.pem");
  // sslContext->authenticate(true, false);

  // connect
  auto socket = std::make_shared<folly::test::BlockingSocket>(
      server.getAddress(), sslContext);
  socket->setReadMode(readMode);
  socket->open(std::chrono::milliseconds(10000));

  // write()
  uint8_t buf[128];
  memset(buf, 'a', sizeof(buf));
  socket->write(buf, sizeof(buf));

  // read()
  uint8_t readbuf[128];
  uint32_t bytesRead = socket->readAll(readbuf, sizeof(readbuf));
  EXPECT_EQ(bytesRead, 128);
  EXPECT_EQ(memcmp(buf, readbuf, bytesRead), 0);

  // close()
  socket->close();

  cerr << "ConnectWriteReadClose test completed" << endl;
  EXPECT_EQ(socket->getSSLSocket()->getTotalConnectTimeout().count(), 10000);
}
} // namespace

TEST(AsyncSSLSocketTest, InitializesSSLContextModes) {
  EventBase eventBase;
  auto sslContext = std::make_shared<SSLContext>();
  constexpr auto requiredModes =
      SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_ENABLE_PARTIAL_WRITE;
  SSL_CTX_clear_mode(sslContext->getSSLCtx(), requiredModes);

  auto socket = AsyncSSLSocket::newSocket(sslContext, &eventBase);
  EXPECT_EQ(
      SSL_CTX_get_mode(sslContext->getSSLCtx()) & requiredModes, requiredModes);
}

/**
 * Test connecting to, writing to, reading from, and closing the
 * connection to the SSL server.
 */
TEST(AsyncSSLSocketTest, ConnectWriteReadClose) {
  connectWriteReadClose(folly::AsyncReader::ReadCallback::ReadMode::ReadBuffer);
}

TEST(AsyncSSLSocketTest, ConnectWriteReadvClose) {
  connectWriteReadClose(folly::AsyncReader::ReadCallback::ReadMode::ReadVec);
}

TEST(AsyncSSLSocketTest, ConnectWriteReadCloseReadable) {
  // Same as above, but test AsyncSSLSocket::readable along the way

  // Start listening on a local port
  WriteCallbackBase writeCallback;
  ReadCallback readCallback(&writeCallback);
  HandshakeCallback handshakeCallback(&readCallback);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback);

  // Set up SSL context.
  std::shared_ptr<SSLContext> sslContext(new SSLContext());
  sslContext->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  // sslContext->loadTrustedCertificates("./trusted-ca-certificate.pem");
  // sslContext->authenticate(true, false);

  // connect
  auto socket =
      std::make_shared<BlockingSocket>(server.getAddress(), sslContext);
  socket->open(std::chrono::milliseconds(10000));

  // write()
  uint8_t buf[128];
  memset(buf, 'a', sizeof(buf));
  socket->write(buf, sizeof(buf));

  // read()
  uint8_t readbuf[128];
  // The TLS record includes the full 128 bytes.  Even though we only read 1
  // byte out of the socket, the rest of the full record decrypted and buffered
  // in the underlying SSL session.
  uint32_t bytesRead = socket->readAll(readbuf, 1);
  EXPECT_EQ(bytesRead, 1);
  // The socket has no data pending in the kernel
  EXPECT_FALSE(socket->getSocket()->AsyncSocket::readable());
  // But the socket is readable
  EXPECT_TRUE(socket->getSocket()->readable());
  bytesRead += socket->readAll(readbuf + 1, sizeof(readbuf) - 1);
  EXPECT_EQ(bytesRead, 128);
  EXPECT_EQ(memcmp(buf, readbuf, bytesRead), 0);

  // close()
  socket->close();

  cerr << "ConnectWriteReadClose test completed" << endl;
  EXPECT_EQ(socket->getSSLSocket()->getTotalConnectTimeout().count(), 10000);
}

/**
 * Check that zero copy options are a noop under AsyncSSLSocket since they
 * aren't supported.
 */
TEST(AsyncSSLSocketTest, ZeroCopy) {
  // Set up SSL context.
  std::shared_ptr<SSLContext> sslContext(new SSLContext());
  sslContext->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");

  auto socket = AsyncSSLSocket::newSocket(sslContext, /*evb=*/nullptr);
  EXPECT_FALSE(socket->setZeroCopy(true));
  EXPECT_FALSE(socket->getZeroCopy());
}

/**
 * Same as above simple test, but with a large read len to test
 * clamping behavior.
 */
TEST(AsyncSSLSocketTest, ConnectWriteReadLargeClose) {
  // Start listening on a local port
  WriteCallbackBase writeCallback;
  ReadCallback readCallback(&writeCallback);
  HandshakeCallback handshakeCallback(&readCallback);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  std::unique_ptr<SSLContext> serverSslContext =
      TestSSLServer::getDefaultSSLContext();
  // With TLS 1.3, OpenSSL will send session tickets. These will arrive after
  // the handshake has completed and can interfere with the BlockingSocket
  // client's reads. Disable the session tickets so that doesn't happen.
  SSL_CTX_set_num_tickets(serverSslContext->getSSLCtx(), 0);
  TestSSLServer server(&acceptCallback, std::move(serverSslContext));

  // Set up SSL context.
  std::shared_ptr<SSLContext> sslContext(new SSLContext());
  sslContext->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  // sslContext->loadTrustedCertificates("./trusted-ca-certificate.pem");
  // sslContext->authenticate(true, false);

  // connect
  auto socket =
      std::make_shared<BlockingSocket>(server.getAddress(), sslContext);
  socket->open(std::chrono::milliseconds(10000));

  // write()
  uint8_t buf[128];
  memset(buf, 'a', sizeof(buf));
  socket->write(buf, sizeof(buf));

  // read()
  uint8_t readbuf[128];
  // we will fake the read len but that should be fine
  size_t readLen = 1LL << 33;
  uint32_t bytesRead = socket->read(readbuf, readLen);
  EXPECT_EQ(bytesRead, 128);
  EXPECT_EQ(memcmp(buf, readbuf, bytesRead), 0);

  // close()
  socket->close();

  cerr << "ConnectWriteReadClose test completed" << endl;
  EXPECT_EQ(socket->getSSLSocket()->getTotalConnectTimeout().count(), 10000);
}

/**
 * Test reading after server close.
 */
TEST(AsyncSSLSocketTest, ReadAfterClose) {
  // Start listening on a local port
  WriteCallbackBase writeCallback;
  ReadEOFCallback readCallback(&writeCallback);
  HandshakeCallback handshakeCallback(&readCallback);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  auto server = std::make_unique<TestSSLServer>(&acceptCallback);

  // Set up SSL context.
  auto sslContext = std::make_shared<SSLContext>();
  sslContext->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");

  auto socket =
      std::make_shared<BlockingSocket>(server->getAddress(), sslContext);
  socket->open();

  // This should trigger an EOF on the client.
  auto evb = handshakeCallback.getSocket()->getEventBase();
  evb->runInEventBaseThreadAndWait([&]() { handshakeCallback.closeSocket(); });
  std::array<uint8_t, 128> readbuf;
  auto bytesRead = socket->read(readbuf.data(), readbuf.size());
  EXPECT_EQ(0, bytesRead);
}

/**
 * Test bad renegotiation
 */
#if !defined(OPENSSL_IS_BORINGSSL)
TEST(AsyncSSLSocketTest, Renegotiate) {
  EventBase eventBase;
  // renegotation in TLS 1.2 only
  auto clientCtx =
      std::make_shared<SSLContext>(SSLContext::SSLVersion::TLSv1_2);
  clientCtx->disableTLS13();
  auto dfServerCtx =
      std::make_shared<SSLContext>(SSLContext::SSLVersion::TLSv1_2);
  dfServerCtx->disableTLS13();
  std::array<NetworkSocket, 2> fds;
  getfds(fds.data());
  getctx(clientCtx, dfServerCtx);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(dfServerCtx, &eventBase, fds[1], true));
  SSLHandshakeClient client(std::move(clientSock), true, true);
  RenegotiatingServer server(std::move(serverSock));

  while (!client.handshakeSuccess_ && !client.handshakeError_) {
    eventBase.loopOnce();
  }

  ASSERT_TRUE(client.handshakeSuccess_);

  auto sslSock = std::move(client).moveSocket();
  sslSock->detachEventBase();
  // This is nasty, however we don't want to add support for
  // renegotiation in AsyncSSLSocket.
  SSL_renegotiate(const_cast<SSL*>(sslSock->getSSL()));

  auto socket = std::make_shared<BlockingSocket>(std::move(sslSock));

  std::thread t([&]() { eventBase.loopForever(); });

  // Trigger the renegotiation.
  std::array<uint8_t, 128> buf;
  memset(buf.data(), 'a', buf.size());
  try {
    socket->write(buf.data(), buf.size());
  } catch (AsyncSocketException& e) {
    LOG(INFO) << "client got error " << e.what();
  }
  eventBase.terminateLoopSoon();
  t.join();

  eventBase.loop();
  ASSERT_TRUE(server.renegotiationError_);
}
#endif

/**
 * Negative test for handshakeError().
 */
TEST(AsyncSSLSocketTest, HandshakeError) {
  // Start listening on a local port
  WriteCallbackBase writeCallback;
  WriteErrorCallback readCallback(&writeCallback);
  HandshakeCallback handshakeCallback(&readCallback);
  HandshakeErrorCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback);

  // Set up SSL context.
  std::shared_ptr<SSLContext> sslContext(new SSLContext());
  sslContext->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");

  // connect
  auto socket =
      std::make_shared<BlockingSocket>(server.getAddress(), sslContext);
  // read()
  bool ex = false;
  try {
    socket->open();

    uint8_t readbuf[128];
    uint32_t bytesRead = socket->readAll(readbuf, sizeof(readbuf));
    LOG(ERROR) << "readAll returned " << bytesRead << " instead of throwing";
  } catch (AsyncSocketException&) {
    ex = true;
  }
  EXPECT_TRUE(ex);

  // close()
  socket->close();
  cerr << "HandshakeError test completed" << endl;
}

/**
 * Negative test for readError().
 */
TEST(AsyncSSLSocketTest, ReadError) {
  // Start listening on a local port
  WriteCallbackBase writeCallback;
  ReadErrorCallback readCallback(&writeCallback);
  HandshakeCallback handshakeCallback(&readCallback);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback);

  // Set up SSL context.
  std::shared_ptr<SSLContext> sslContext(new SSLContext());
  sslContext->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");

  // connect
  auto socket =
      std::make_shared<BlockingSocket>(server.getAddress(), sslContext);
  socket->open();

  // write something to trigger ssl handshake
  uint8_t buf[128];
  memset(buf, 'a', sizeof(buf));
  socket->write(buf, sizeof(buf));

  socket->close();
  cerr << "ReadError test completed" << endl;
}

/**
 * Negative test for writeError().
 */
TEST(AsyncSSLSocketTest, WriteError) {
  // Start listening on a local port
  WriteCallbackBase writeCallback;
  WriteErrorCallback readCallback(&writeCallback);
  HandshakeCallback handshakeCallback(&readCallback);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback);

  // Set up SSL context.
  std::shared_ptr<SSLContext> sslContext(new SSLContext());
  sslContext->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");

  // connect
  auto socket =
      std::make_shared<BlockingSocket>(server.getAddress(), sslContext);
  socket->open();

  // write something to trigger ssl handshake
  uint8_t buf[128];
  memset(buf, 'a', sizeof(buf));
  socket->write(buf, sizeof(buf));

  socket->close();
  cerr << "WriteError test completed" << endl;
}

/**
 * Test a socket with TCP_NODELAY unset.
 */
TEST(AsyncSSLSocketTest, SocketWithDelay) {
  // Start listening on a local port
  WriteCallbackBase writeCallback;
  ReadCallback readCallback(&writeCallback);
  HandshakeCallback handshakeCallback(&readCallback);
  SSLServerAcceptCallbackDelay acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback);

  // Set up SSL context.
  std::shared_ptr<SSLContext> sslContext(new SSLContext());
  sslContext->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");

  // connect
  auto socket =
      std::make_shared<BlockingSocket>(server.getAddress(), sslContext);
  socket->open();

  // write()
  uint8_t buf[128];
  memset(buf, 'a', sizeof(buf));
  socket->write(buf, sizeof(buf));

  // read()
  uint8_t readbuf[128];
  uint32_t bytesRead = socket->readAll(readbuf, sizeof(readbuf));
  EXPECT_EQ(bytesRead, 128);
  EXPECT_EQ(memcmp(buf, readbuf, bytesRead), 0);

  // close()
  socket->close();

  cerr << "SocketWithDelay test completed" << endl;
}

class NextProtocolTest : public Test {
  // For matching protos
 public:
  void SetUp() override { getctx(clientCtx, serverCtx); }

  void connect(bool unset = false) {
    getfds(fds);

    if (unset) {
      // unsetting NPN for any of [client, server] is enough to make NPN not
      // work
      clientCtx->unsetNextProtocols();
    }

    AsyncSSLSocket::UniquePtr clientSock(
        new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
    AsyncSSLSocket::UniquePtr serverSock(
        new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));
    client = std::make_unique<AlpnClient>(std::move(clientSock));
    server = std::make_unique<AlpnServer>(std::move(serverSock));

    eventBase.loop();
  }

  void expectProtocol(const std::string& proto) {
    expectHandshakeSuccess();
    EXPECT_NE(client->nextProtoLength, 0);
    EXPECT_EQ(client->nextProtoLength, server->nextProtoLength);
    EXPECT_EQ(
        memcmp(client->nextProto, server->nextProto, server->nextProtoLength),
        0);
    string selected((const char*)client->nextProto, client->nextProtoLength);
    EXPECT_EQ(proto, selected);
  }

  void expectNoProtocol() {
    expectHandshakeSuccess();
    EXPECT_EQ(client->nextProtoLength, 0);
    EXPECT_EQ(server->nextProtoLength, 0);
    EXPECT_EQ(client->nextProto, nullptr);
    EXPECT_EQ(server->nextProto, nullptr);
  }

  void expectHandshakeSuccess() {
    EXPECT_FALSE(client->except.has_value())
        << "client handshake error: " << client->except->what();
    EXPECT_FALSE(server->except.has_value())
        << "server handshake error: " << server->except->what();
  }

  void expectHandshakeError() {
    EXPECT_TRUE(client->except.has_value())
        << "Expected client handshake error!";
    EXPECT_TRUE(server->except.has_value())
        << "Expected server handshake error!";
  }

  EventBase eventBase;
  std::shared_ptr<SSLContext> clientCtx{std::make_shared<SSLContext>()};
  std::shared_ptr<SSLContext> serverCtx{std::make_shared<SSLContext>()};
  NetworkSocket fds[2];
  std::unique_ptr<AlpnClient> client;
  std::unique_ptr<AlpnServer> server;
};

TEST_F(NextProtocolTest, AlpnTestOverlap) {
  clientCtx->setAdvertisedNextProtocols({"blub", "baz"});
  serverCtx->setAdvertisedNextProtocols({"foo", "bar", "baz"});

  connect();

  expectProtocol("baz");
}

TEST_F(NextProtocolTest, AlpnTestUnset) {
  // Identical to above test, except that we want unset NPN before
  // looping.
  clientCtx->setAdvertisedNextProtocols({"blub", "baz"});
  serverCtx->setAdvertisedNextProtocols({"foo", "bar", "baz"});

  connect(true /* unset */);

  expectNoProtocol();
}

TEST_F(NextProtocolTest, AlpnTestNoOverlap) {
  clientCtx->setAdvertisedNextProtocols({"blub"});
  serverCtx->setAdvertisedNextProtocols({"foo", "bar", "baz"});
  connect();

  expectNoProtocol();
}

TEST_F(NextProtocolTest, RandomizedAlpnTest) {
  // Probability that this test will fail is 2^-64, which could be considered
  // as negligible.
  const int kTries = 64;

  clientCtx->setAdvertisedNextProtocols({"foo", "bar", "baz"});
  serverCtx->setRandomizedAdvertisedNextProtocols({{1, {"foo"}}, {1, {"bar"}}});

  std::set<string> selectedProtocols;
  for (int i = 0; i < kTries; ++i) {
    connect();

    EXPECT_NE(client->nextProtoLength, 0);
    EXPECT_EQ(client->nextProtoLength, server->nextProtoLength);
    EXPECT_EQ(
        memcmp(client->nextProto, server->nextProto, server->nextProtoLength),
        0);
    string selected((const char*)client->nextProto, client->nextProtoLength);
    selectedProtocols.insert(selected);
    expectHandshakeSuccess();
  }
  EXPECT_EQ(selectedProtocols.size(), 2);
}

TEST_F(NextProtocolTest, AlpnNotAllowMismatchNoClientProtocol) {
  clientCtx->setAdvertisedNextProtocols({});
  serverCtx->setAdvertisedNextProtocols({"foo", "bar", "baz"});
  serverCtx->setAlpnAllowMismatch(false);

  connect();

  expectHandshakeSuccess();
  expectNoProtocol();
  EXPECT_EQ(server->getClientAlpns(), std::vector<std::string>({}));
}

TEST_F(NextProtocolTest, AlpnNotAllowMismatchWithOverlap) {
  clientCtx->setAdvertisedNextProtocols({"blub", "baz"});
  serverCtx->setAdvertisedNextProtocols({"foo", "bar", "baz"});
  serverCtx->setAlpnAllowMismatch(false);

  connect();

  expectProtocol("baz");
  EXPECT_EQ(
      server->getClientAlpns(), std::vector<std::string>({"blub", "baz"}));
}

TEST_F(NextProtocolTest, AlpnNotAllowMismatchWithoutOverlap) {
  clientCtx->setAdvertisedNextProtocols({"blub"});
  serverCtx->setAdvertisedNextProtocols({"foo", "bar", "baz"});
  serverCtx->setAlpnAllowMismatch(false);

  connect();

  expectHandshakeError();
  EXPECT_EQ(server->getClientAlpns(), std::vector<std::string>({"blub"}));
}

/**
 * 1. Client sends TLSEXT_HOSTNAME in client hello.
 * 2. Server found a match SSL_CTX and use this SSL_CTX to
 *    continue the SSL handshake.
 * 3. Server sends back TLSEXT_HOSTNAME in server hello.
 */
TEST(AsyncSSLSocketTest, SNITestMatch) {
  EventBase eventBase;
  std::shared_ptr<SSLContext> clientCtx(new SSLContext);
  std::shared_ptr<SSLContext> dfServerCtx(new SSLContext);
  // Use the same SSLContext to continue the handshake after
  // tlsext_hostname match.
  std::shared_ptr<SSLContext> hskServerCtx(dfServerCtx);
  const std::string serverName("xyz.newdev.facebook.com");
  NetworkSocket fds[2];
  getfds(fds);
  getctx(clientCtx, dfServerCtx);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], serverName));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(dfServerCtx, &eventBase, fds[1], true));
  SNIClient client(std::move(clientSock));
  SNIServer server(
      std::move(serverSock), dfServerCtx, hskServerCtx, serverName);

  eventBase.loop();

  EXPECT_TRUE(client.serverNameMatch);
  EXPECT_TRUE(server.serverNameMatch);
}

/**
 * 1. Client sends TLSEXT_HOSTNAME in client hello.
 * 2. Server cannot find a matching SSL_CTX and continue to use
 *    the current SSL_CTX to do the handshake.
 * 3. Server does not send back TLSEXT_HOSTNAME in server hello.
 */
TEST(AsyncSSLSocketTest, SNITestNotMatch) {
  EventBase eventBase;
  std::shared_ptr<SSLContext> clientCtx(new SSLContext);
  std::shared_ptr<SSLContext> dfServerCtx(new SSLContext);
  // Use the same SSLContext to continue the handshake after
  // tlsext_hostname match.
  std::shared_ptr<SSLContext> hskServerCtx(dfServerCtx);
  const std::string clientRequestingServerName("foo.com");
  const std::string serverExpectedServerName("xyz.newdev.facebook.com");

  NetworkSocket fds[2];
  getfds(fds);
  getctx(clientCtx, dfServerCtx);

  AsyncSSLSocket::UniquePtr clientSock(new AsyncSSLSocket(
      clientCtx, &eventBase, fds[0], clientRequestingServerName));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(dfServerCtx, &eventBase, fds[1], true));
  SNIClient client(std::move(clientSock));
  SNIServer server(
      std::move(serverSock),
      dfServerCtx,
      hskServerCtx,
      serverExpectedServerName);

  eventBase.loop();

  EXPECT_TRUE(!client.serverNameMatch);
  EXPECT_TRUE(!server.serverNameMatch);
}
/**
 * 1. Client sends TLSEXT_HOSTNAME in client hello.
 * 2. We then change the serverName.
 * 3. We expect that we get 'false' as the result for serNameMatch.
 */

TEST(AsyncSSLSocketTest, SNITestChangeServerName) {
  EventBase eventBase;
  std::shared_ptr<SSLContext> clientCtx(new SSLContext);
  std::shared_ptr<SSLContext> dfServerCtx(new SSLContext);
  // Use the same SSLContext to continue the handshake after
  // tlsext_hostname match.
  std::shared_ptr<SSLContext> hskServerCtx(dfServerCtx);
  const std::string serverName("xyz.newdev.facebook.com");
  NetworkSocket fds[2];
  getfds(fds);
  getctx(clientCtx, dfServerCtx);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], serverName));
  // Change the server name
  std::string newName("new.com");
  clientSock->setServerName(newName);
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(dfServerCtx, &eventBase, fds[1], true));
  SNIClient client(std::move(clientSock));
  SNIServer server(
      std::move(serverSock), dfServerCtx, hskServerCtx, serverName);

  eventBase.loop();

  EXPECT_TRUE(!client.serverNameMatch);
}

/**
 * 1. Client does not send TLSEXT_HOSTNAME in client hello.
 * 2. Server does not send back TLSEXT_HOSTNAME in server hello.
 */
TEST(AsyncSSLSocketTest, SNITestClientHelloNoHostname) {
  EventBase eventBase;
  std::shared_ptr<SSLContext> clientCtx(new SSLContext);
  std::shared_ptr<SSLContext> dfServerCtx(new SSLContext);
  // Use the same SSLContext to continue the handshake after
  // tlsext_hostname match.
  std::shared_ptr<SSLContext> hskServerCtx(dfServerCtx);
  const std::string serverExpectedServerName("xyz.newdev.facebook.com");

  NetworkSocket fds[2];
  getfds(fds);
  getctx(clientCtx, dfServerCtx);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(dfServerCtx, &eventBase, fds[1], true));
  SNIClient client(std::move(clientSock));
  SNIServer server(
      std::move(serverSock),
      dfServerCtx,
      hskServerCtx,
      serverExpectedServerName);

  eventBase.loop();

  EXPECT_TRUE(!client.serverNameMatch);
  EXPECT_TRUE(!server.serverNameMatch);
}

/**
 * If the application-installed ServerNameCallback returns a
 * ServerNameCallbackResult outside the three documented enumerators (as if
 * the callback has gone haywire), the AsyncSSLSocket must not continue as
 * normal with establishing a connection. The process must crash rather than
 * silently proceed with a fail-open answer or hit undefined behavior. In the
 * future, though, we may choose to handle this another way.
 *
 * The bad value used (see SNIServerBadCallbackResult) is deliberately chosen
 * to be in-range per [dcl.enum] (a well-defined cast, not independently UB)
 * but not a declared enumerator, so this test exercises folly's own handling
 * specifically.
 */
static void runSniHandshakeWithBadCallbackResult() {
  EventBase eventBase;
  std::shared_ptr<SSLContext> clientCtx(new SSLContext);
  std::shared_ptr<SSLContext> dfServerCtx(new SSLContext);
  const std::string serverName("xyz.newdev.facebook.com");
  NetworkSocket fds[2];
  getfds(fds);
  getctx(clientCtx, dfServerCtx);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], serverName));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(dfServerCtx, &eventBase, fds[1], true));
  SNIClient client(std::move(clientSock));
  SNIServerBadCallbackResult server(std::move(serverSock), dfServerCtx);

  eventBase.loop();
}

TEST(AsyncSSLSocketTest, SNITestBadCallbackResultCrashesDeathTest) {
  EXPECT_DEATH(
      runSniHandshakeWithBadCallbackResult(),
      "unexpected ServerNameCallbackResult|invalid-enum-load");
}

/**
 * 1. Create an SSLContext that does not have an ALPN
 * 2. Use AsyncSSLSocket::setSupportedApplicationProtocols on the client and
 * server, and assert that a common ALPN was negotiated.
 */
TEST(AsyncSSLSocketTest, SetSupportedApplicationProtocols) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto dfServerCtx = std::make_shared<SSLContext>();
  // Use the same SSLContext to continue the handshake after
  // tlsext_hostname match.
  auto hskServerCtx = std::make_shared<SSLContext>();
  const std::string serverExpectedServerName("xyz.newdev.facebook.com");

  NetworkSocket fds[2];
  getfds(fds);
  getctx(clientCtx, dfServerCtx);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(dfServerCtx, &eventBase, fds[1], true));

  std::vector<std::string> protocols;
  protocols.emplace_back("rs");

  clientSock->setSupportedApplicationProtocols(protocols);
  serverSock->setSupportedApplicationProtocols(protocols);

  SNIClient client(std::move(clientSock));
  SNIServer server(
      std::move(serverSock),
      dfServerCtx,
      hskServerCtx,
      serverExpectedServerName);

  eventBase.loop();

  EXPECT_TRUE(
      client.getApplicationProtocol().compare(
          server.getApplicationProtocol()) == 0);
}

/**
 * Test SSL client socket
 */
TEST(AsyncSSLSocketTest, SSLClientTest) {
  // Start listening on a local port
  WriteCallbackBase writeCallback;
  ReadCallback readCallback(&writeCallback);
  HandshakeCallback handshakeCallback(&readCallback);
  SSLServerAcceptCallbackDelay acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback);

  // Set up SSL client
  EventBase eventBase;
  auto client = std::make_shared<SSLClient>(&eventBase, server.getAddress(), 1);
  client->setSSLOptions(SSL_OP_NO_TICKET);

  client->connect();
  EventBaseAborter eba(&eventBase, 3000);
  eventBase.loop();

  EXPECT_EQ(client->getMiss(), 1);
  EXPECT_EQ(client->getHit(), 0);

  cerr << "SSLClientTest test completed" << endl;
}

/**
 * Test SSL client socket session re-use
 */
TEST(AsyncSSLSocketTest, SSLClientTestReuse) {
  // Start listening on a local port
  WriteCallbackBase writeCallback;
  ReadCallback readCallback(&writeCallback);
  HandshakeCallback handshakeCallback(&readCallback);
  SSLServerAcceptCallbackDelay acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback);

  // Set up SSL client
  EventBase eventBase;
  auto client =
      std::make_shared<SSLClient>(&eventBase, server.getAddress(), 10);
  client->setSSLOptions(SSL_OP_NO_TICKET);

  client->connect();
  EventBaseAborter eba(&eventBase, 3000);
  eventBase.loop();

  EXPECT_EQ(client->getMiss(), 1);
  EXPECT_EQ(client->getHit(), 9);

  cerr << "SSLClientTestReuse test completed" << endl;
}

/**
 * Test SSL client socket timeout
 */
TEST(AsyncSSLSocketTest, SSLClientTimeoutTest) {
  // Start listening on a local port
  EmptyReadCallback readCallback;
  HandshakeCallback handshakeCallback(
      &readCallback, HandshakeCallback::EXPECT_ERROR);
  HandshakeTimeoutCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback);

  // Set up SSL client
  EventBase eventBase;
  auto client =
      std::make_shared<SSLClient>(&eventBase, server.getAddress(), 1, 10);
  client->setSSLOptions(SSL_OP_NO_TICKET);

  client->connect(true /* write before connect completes */);
  EventBaseAborter eba(&eventBase, 3000);
  eventBase.loop();

  usleep(100000);
  // This is checking that the connectError callback precedes any queued
  // writeError callbacks.  This matches AsyncSocket's behavior
  EXPECT_EQ(client->getWriteAfterConnectErrors(), 1);
  EXPECT_EQ(client->getErrors(), 1);
  EXPECT_EQ(client->getMiss(), 0);
  EXPECT_EQ(client->getHit(), 0);

  cerr << "SSLClientTimeoutTest test completed" << endl;
}

class PerLoopReadCallback : public AsyncTransport::ReadCallback {
 public:
  void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
    *bufReturn = buf_.data();
    *lenReturn = buf_.size();
  }

  void readDataAvailable(size_t len) noexcept override {
    VLOG(3) << "Read of size: " << len;
    s_->setReadCB(nullptr);
    s_->getEventBase()->runInLoop([this]() { s_->setReadCB(this); });
  }

  void readErr(const AsyncSocketException&) noexcept override {}

  void readEOF() noexcept override {}

  void setSocket(AsyncSocket* s) { s_ = s; }

 private:
  AsyncSocket* s_;
  std::array<uint8_t, 1000> buf_;
};

class CloseNotifyConnector : public AsyncSocket::ConnectCallback {
 public:
  CloseNotifyConnector(EventBase* evb, const SocketAddress& addr) {
    evb_ = evb;
    ssl_ = AsyncSSLSocket::newSocket(std::make_shared<SSLContext>(), evb_);
    ssl_->connect(this, addr);
  }

  void connectSuccess() noexcept override {
    ssl_->writeChain(nullptr, IOBuf::copyBuffer("hi"));
    auto ssl = const_cast<SSL*>(ssl_->getSSL());
    SSL_shutdown(ssl);
    auto fd = ssl_->detachNetworkSocket();
    tcp_.reset(new AsyncSocket(evb_, fd), AsyncSocket::Destructor());
    evb_->runAfterDelay(
        [this]() {
          perLoopReads_.setSocket(tcp_.get());
          tcp_->setReadCB(&perLoopReads_);
          evb_->runAfterDelay([this]() { tcp_->closeNow(); }, 10);
        },
        100);
  }

  void connectErr(const AsyncSocketException& ex) noexcept override {
    FAIL() << ex.what();
  }

 private:
  EventBase* evb_;
  std::shared_ptr<AsyncSSLSocket> ssl_;
  std::shared_ptr<AsyncSocket> tcp_;
  PerLoopReadCallback perLoopReads_;
};

class ErrorCheckingWriteCallback : public AsyncSocket::WriteCallback {
 public:
  void writeSuccess() noexcept override {}

  void writeErr(size_t, const AsyncSocketException& ex) noexcept override {
    LOG(ERROR) << "write error: " << ex.what();
    EXPECT_NE(
        ex.getType(),
        AsyncSocketException::AsyncSocketExceptionType::SSL_ERROR);
  }
};

class WriteOnEofReadCallback : public ReadCallback {
 public:
  using ReadCallback::ReadCallback;

  void readEOF() noexcept override {
    LOG(INFO) << "Got EOF";
    auto chain = IOBuf::create(0);
    for (size_t i = 0; i < 1000 * 1000; i++) {
      auto buf = IOBuf::create(10);
      buf->append(10);
      memset(buf->writableData(), 'x', 10);
      chain->prependChain(std::move(buf));
    }
    socket_->writeChain(&writeCallback_, std::move(chain));
  }

  void readErr(const AsyncSocketException& ex) noexcept override {
    LOG(ERROR) << ex.what();
  }

 private:
  ErrorCheckingWriteCallback writeCallback_;
};

TEST(AsyncSSLSocketTest, EarlyCloseNotify) {
  WriteOnEofReadCallback readCallback(nullptr);
  HandshakeCallback handshakeCallback(&readCallback);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback);

  EventBase eventBase;
  CloseNotifyConnector cnc(&eventBase, server.getAddress());

  eventBase.loop();
}

/**
 * Verify Client Ciphers obtained using SSL MSG Callback.
 */
TEST(AsyncSSLSocketTest, SSLParseClientHelloSuccess) {
  EventBase eventBase;
  auto serverCtx = std::make_shared<SSLContext>();
  serverCtx->setVerificationOption(SSLContext::VerifyClientCertificate::ALWAYS);
  serverCtx->setCiphersuitesOrThrow(
      "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256");
  serverCtx->loadPrivateKey(find_resource(kTestKey).c_str());
  serverCtx->loadCertificate(find_resource(kTestCert).c_str());
  serverCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());
  serverCtx->setSupportedClientCertificateAuthorityNamesFromFile(
      find_resource(kTestCA).c_str());

  auto clientCtx = std::make_shared<SSLContext>();
  clientCtx->setVerificationOption(
      SSLContext::VerifyServerCertificate::IF_PRESENTED);
  clientCtx->setCiphersuitesOrThrow(
      "TLS_CHACHA20_POLY1305_SHA256:TLS_AES_256_GCM_SHA384");
  // With SSLContext min version set to TLS 1.2, TLS 1.2 ciphers are appended to
  // the list that's sent to the server, and those would appear in the
  // clientCiphers_ captured and verified below. Remove all of them by setting
  // eNULL.
  clientCtx->setCiphersOrThrow("eNULL");
  clientCtx->loadPrivateKey(find_resource(kTestKey).c_str());
  clientCtx->loadCertificate(find_resource(kTestCert).c_str());
  clientCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());

  NetworkSocket fds[2];
  getfds(fds);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));

  SSLHandshakeClient client(std::move(clientSock), true, true);
  SSLHandshakeServerParseClientHello server(std::move(serverSock), true, true);

  eventBase.loop();

#if defined(OPENSSL_IS_BORINGSSL)
  EXPECT_EQ(
      server.clientCiphers_,
      "TLS_CHACHA20_POLY1305_SHA256:TLS_AES_256_GCM_SHA384");
#else
  EXPECT_EQ(
      server.clientCiphers_,
      "TLS_CHACHA20_POLY1305_SHA256:TLS_AES_256_GCM_SHA384:00ff");
#endif
  EXPECT_EQ(server.chosenCipher_, "TLS_CHACHA20_POLY1305_SHA256");
  EXPECT_EQ(server.chosenCipherId_, 0x1303);
  EXPECT_TRUE(client.handshakeVerify_);
  EXPECT_TRUE(client.handshakeSuccess_);
  EXPECT_TRUE(!client.handshakeError_);
  EXPECT_TRUE(server.handshakeVerify_);
  EXPECT_TRUE(server.handshakeSuccess_);
  EXPECT_TRUE(!server.handshakeError_);
}

TEST(AsyncSSLSocketTest, SSLGetEKM) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto serverCtx = std::make_shared<SSLContext>();

  NetworkSocket fds[2];
  getfds(fds);
  getctx(clientCtx, serverCtx);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));

  EXPECT_EQ(
      nullptr, clientSock->getExportedKeyingMaterial("test", nullptr, 12));

  serverSock->sslAccept(nullptr, std::chrono::milliseconds::zero());
  clientSock->sslConn(nullptr, std::chrono::milliseconds::zero());
  eventBase.loop();
  auto ekm = clientSock->getExportedKeyingMaterial("test", nullptr, 32);

  EXPECT_NE(nullptr, ekm);
  EXPECT_EQ(ekm->computeChainDataLength(), 32);
  // non null context
  EXPECT_NE(
      nullptr,
      clientSock->getExportedKeyingMaterial(
          "test", IOBuf::copyBuffer("haha"), 32));
  // empty label
  EXPECT_NE(
      nullptr,
      clientSock->getExportedKeyingMaterial("", IOBuf::copyBuffer("haha"), 32));

  auto serverEkm = serverSock->getExportedKeyingMaterial("test", nullptr, 32);
  EXPECT_TRUE(folly::IOBufEqualTo{}(ekm, serverEkm));
}

TEST(AsyncSSLSocketTest, SSLGetEKMFailsOnTLS10) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto serverCtx = std::make_shared<SSLContext>();
  SSL_CTX_set_max_proto_version(serverCtx->getSSLCtx(), TLS1_VERSION);

  NetworkSocket fds[2];
  getfds(fds);
  getctx(clientCtx, serverCtx);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));

  serverSock->sslAccept(nullptr, std::chrono::milliseconds::zero());
  clientSock->sslConn(nullptr, std::chrono::milliseconds::zero());
  eventBase.loop();
  auto ekm = clientSock->getExportedKeyingMaterial("test", nullptr, 32);

  EXPECT_EQ(nullptr, ekm);
}

/**
 * Verify that server is able to get client cert by getPeerCert() API.
 */
TEST(AsyncSSLSocketTest, GetClientCertificate) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto serverCtx = std::make_shared<SSLContext>();
  serverCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  serverCtx->ciphers("ECDHE-RSA-AES128-SHA:AES128-SHA:AES256-SHA");
  serverCtx->loadPrivateKey(find_resource(kTestKey).c_str());
  serverCtx->loadCertificate(find_resource(kTestCert).c_str());
  serverCtx->loadTrustedCertificates(find_resource(kClientTestCA).c_str());
  serverCtx->setSupportedClientCertificateAuthorityNamesFromFile(
      find_resource(kClientTestCA).c_str());

  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  clientCtx->ciphers("AES256-SHA:AES128-SHA");
  clientCtx->loadPrivateKey(find_resource(kClientTestKey).c_str());
  clientCtx->loadCertificate(find_resource(kClientTestCert).c_str());
  clientCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());

  std::array<NetworkSocket, 2> fds;
  getfds(fds.data());

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));

  SSLHandshakeClient client(std::move(clientSock), true, true);
  SSLHandshakeServerParseClientHello server(std::move(serverSock), true, true);

  eventBase.loop();

  // Handshake should succeed.
  EXPECT_TRUE(client.handshakeSuccess_);
  EXPECT_TRUE(server.handshakeSuccess_);

  // Reclaim the sockets from SSLHandshakeBase.
  auto cliSocket = std::move(client).moveSocket();
  auto srvSocket = std::move(server).moveSocket();

  // Client cert retrieved from server side.
  auto serverPeerCert = srvSocket->getPeerCertificate();
  CHECK(serverPeerCert);

  // Client cert retrieved from client side.
  auto clientSelfCert = cliSocket->getSelfCertificate();
  CHECK(clientSelfCert);

  auto serverX509 =
      folly::OpenSSLTransportCertificate::tryExtractX509(serverPeerCert);
  CHECK(serverX509);

  auto clientX509 =
      folly::OpenSSLTransportCertificate::tryExtractX509(clientSelfCert);
  CHECK(clientX509);

  // The two certs should be the same.
  EXPECT_EQ(0, X509_cmp(clientX509.get(), serverX509.get()));
}

TEST(AsyncSSLSocketTest, SSLParseClientHelloOnePacket) {
  EventBase eventBase;
  auto ctx = std::make_shared<SSLContext>();

  NetworkSocket fds[2];
  getfds(fds);

  int bufLen = 42;
  uint8_t majorVersion = 18;
  uint8_t minorVersion = 25;

  // Create callback buf
  auto buf = IOBuf::create(bufLen);
  buf->append(bufLen);
  folly::io::RWPrivateCursor cursor(buf.get());
  cursor.write<uint8_t>(uint8_t(SSL3_MT_CLIENT_HELLO));
  cursor.write<uint16_t>(uint16_t(0));
  cursor.write<uint8_t>(uint8_t(38));
  cursor.write<uint8_t>(majorVersion);
  cursor.write<uint8_t>(minorVersion);
  cursor.skip(32);
  cursor.write<uint32_t>(uint32_t(0));

  SSL* ssl = ctx->createSSL();
  SCOPE_EXIT {
    SSL_free(ssl);
  };
  AsyncSSLSocket::UniquePtr sock(
      new AsyncSSLSocket(ctx, &eventBase, fds[0], true));
  sock->enableClientHelloParsing();

  // Test client hello parsing in one packet
  AsyncSSLSocket::clientHelloParsingCallback(
      0, 0, SSL3_RT_HANDSHAKE, buf->data(), buf->length(), ssl, sock.get());
  buf.reset();

  auto parsedClientHello = sock->getClientHelloInfo();
  EXPECT_TRUE(parsedClientHello != nullptr);
  EXPECT_EQ(parsedClientHello->clientHelloMajorVersion_, majorVersion);
  EXPECT_EQ(parsedClientHello->clientHelloMinorVersion_, minorVersion);
}

TEST(AsyncSSLSocketTest, SSLParseClientHelloTwoPackets) {
  EventBase eventBase;
  auto ctx = std::make_shared<SSLContext>();

  NetworkSocket fds[2];
  getfds(fds);

  int bufLen = 42;
  uint8_t majorVersion = 18;
  uint8_t minorVersion = 25;

  // Create callback buf
  auto buf = IOBuf::create(bufLen);
  buf->append(bufLen);
  folly::io::RWPrivateCursor cursor(buf.get());
  cursor.write<uint8_t>(uint8_t(SSL3_MT_CLIENT_HELLO));
  cursor.write<uint16_t>(uint16_t(0));
  cursor.write<uint8_t>(uint8_t(38));
  cursor.write<uint8_t>(majorVersion);
  cursor.write<uint8_t>(minorVersion);
  cursor.skip(32);
  cursor.write<uint32_t>(uint32_t(0));

  SSL* ssl = ctx->createSSL();
  SCOPE_EXIT {
    SSL_free(ssl);
  };
  AsyncSSLSocket::UniquePtr sock(
      new AsyncSSLSocket(ctx, &eventBase, fds[0], true));
  sock->enableClientHelloParsing();

  // Test parsing with two packets with first packet size < 3
  auto bufCopy = folly::IOBuf::copyBuffer(buf->data(), 2);
  AsyncSSLSocket::clientHelloParsingCallback(
      0,
      0,
      SSL3_RT_HANDSHAKE,
      bufCopy->data(),
      bufCopy->length(),
      ssl,
      sock.get());
  bufCopy.reset();
  bufCopy = folly::IOBuf::copyBuffer(buf->data() + 2, buf->length() - 2);
  AsyncSSLSocket::clientHelloParsingCallback(
      0,
      0,
      SSL3_RT_HANDSHAKE,
      bufCopy->data(),
      bufCopy->length(),
      ssl,
      sock.get());
  bufCopy.reset();

  auto parsedClientHello = sock->getClientHelloInfo();
  EXPECT_TRUE(parsedClientHello != nullptr);
  EXPECT_EQ(parsedClientHello->clientHelloMajorVersion_, majorVersion);
  EXPECT_EQ(parsedClientHello->clientHelloMinorVersion_, minorVersion);
}

TEST(AsyncSSLSocketTest, SSLParseClientHelloMultiplePackets) {
  EventBase eventBase;
  auto ctx = std::make_shared<SSLContext>();
  ctx->setSupportedGroups(std::vector<std::string>({"P-256"}));

  NetworkSocket fds[2];
  getfds(fds);

  int bufLen = 42;
  uint8_t majorVersion = 18;
  uint8_t minorVersion = 25;

  // Create callback buf
  auto buf = IOBuf::create(bufLen);
  buf->append(bufLen);
  folly::io::RWPrivateCursor cursor(buf.get());
  cursor.write<uint8_t>(uint8_t(SSL3_MT_CLIENT_HELLO));
  cursor.write<uint16_t>(uint16_t(0));
  cursor.write<uint8_t>(uint8_t(38));
  cursor.write<uint8_t>(majorVersion);
  cursor.write<uint8_t>(minorVersion);
  cursor.skip(32);
  cursor.write<uint32_t>(uint32_t(0));

  SSL* ssl = ctx->createSSL();
  SCOPE_EXIT {
    SSL_free(ssl);
  };
  AsyncSSLSocket::UniquePtr sock(
      new AsyncSSLSocket(ctx, &eventBase, fds[0], true));
  sock->enableClientHelloParsing();

  // Test parsing with multiple small packets
  for (std::size_t i = 0; i < buf->length(); i += 3) {
    auto bufCopy = folly::IOBuf::copyBuffer(
        buf->data() + i, std::min((std::size_t)3, buf->length() - i));
    AsyncSSLSocket::clientHelloParsingCallback(
        0,
        0,
        SSL3_RT_HANDSHAKE,
        bufCopy->data(),
        bufCopy->length(),
        ssl,
        sock.get());
    bufCopy.reset();
  }

  auto parsedClientHello = sock->getClientHelloInfo();
  EXPECT_TRUE(parsedClientHello != nullptr);
  EXPECT_EQ(parsedClientHello->clientHelloMajorVersion_, majorVersion);
  EXPECT_EQ(parsedClientHello->clientHelloMinorVersion_, minorVersion);
}

/**
 * Verify successful behavior of SSL certificate validation.
 */
TEST(AsyncSSLSocketTest, SSLHandshakeValidationSuccess) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto dfServerCtx = std::make_shared<SSLContext>();

  NetworkSocket fds[2];
  getfds(fds);
  getctx(clientCtx, dfServerCtx);

  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  dfServerCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(dfServerCtx, &eventBase, fds[1], true));

  SSLHandshakeClient client(std::move(clientSock), true, true);
  clientCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());

  SSLHandshakeServer server(std::move(serverSock), true, true);

  eventBase.loop();

  EXPECT_TRUE(client.handshakeVerify_);
  EXPECT_TRUE(client.handshakeSuccess_);
  EXPECT_TRUE(!client.handshakeError_);
  EXPECT_LE(0, client.handshakeTime.count());
  EXPECT_TRUE(!server.handshakeVerify_);
  EXPECT_TRUE(server.handshakeSuccess_);
  EXPECT_TRUE(!server.handshakeError_);
  EXPECT_LE(0, server.handshakeTime.count());
}

/**
 * Verify that the client's verification callback is able to fail SSL
 * connection establishment.
 */
TEST(AsyncSSLSocketTest, SSLHandshakeValidationFailure) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto dfServerCtx = std::make_shared<SSLContext>();

  NetworkSocket fds[2];
  getfds(fds);
  getctx(clientCtx, dfServerCtx);

  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  dfServerCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(dfServerCtx, &eventBase, fds[1], true));

  SSLHandshakeClient client(std::move(clientSock), true, false);
  clientCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());

  SSLHandshakeServer server(std::move(serverSock), true, true);

  eventBase.loop();

  EXPECT_TRUE(client.handshakeVerify_);
  EXPECT_TRUE(!client.handshakeSuccess_);
  EXPECT_TRUE(client.handshakeError_);
  EXPECT_LE(0, client.handshakeTime.count());
  EXPECT_TRUE(!server.handshakeVerify_);
  EXPECT_TRUE(!server.handshakeSuccess_);
  EXPECT_TRUE(server.handshakeError_);
  EXPECT_LE(0, server.handshakeTime.count());
}

/**
 * Verify that the client successfully handshakes when
 * CertificateIdentityVerifier is set and returns with no exception.
 */
TEST(AsyncSSLSocketTest, SSLCertificateIdentityVerifierReturns) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<folly::SSLContext>();
  auto serverCtx = std::make_shared<folly::SSLContext>();
  getctx(clientCtx, serverCtx);
  // the client socket will default to USE_CTX, so set VERIFY here
  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  // load root certificate
  clientCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());

  // prepare a basic server (callbacks have a few EXPECTS to fullfil)
  ReadCallback readCallback(nullptr);
  // expects successful handshake
  HandshakeCallback handshakeCallback(&readCallback);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback, serverCtx);

  std::shared_ptr<MockCertificateIdentityVerifier> verifier =
      std::make_shared<MockCertificateIdentityVerifier>();

  // expecting context verification to be called first
  auto&& verifyContext =
      EXPECT_CALL(*verifier, verifyContext(true, _))
          .Times(AtLeast(1))
          .WillRepeatedly(Return(true));
  // expecting to only verify once, with the leaf certificate
  // (kTestCert)
  EXPECT_CALL(
      *verifier,
      verifyLeaf(Property(
          &AsyncTransportCertificate::getIdentity, StrEq("Asox Company"))))
      .After(verifyContext)
      .WillOnce(Return(ByMove(
          std::make_unique<folly::ssl::BasicTransportCertificate>(
              "Asox Company", readCertFromFile(kTestCert)))));

  AsyncSSLSocket::Options opts;
  opts.verifier = std::move(verifier);

  // connect to server and handshake
  AsyncSSLSocket::UniquePtr socket(
      new AsyncSSLSocket(clientCtx, &eventBase, std::move(opts)));
  socket->connect(nullptr, server.getAddress(), 0);

  // write to satisfy server ReadCallback EXPECTs
  std::array<uint8_t, 128> buf;
  memset(buf.data(), 'a', buf.size());
  socket->write(nullptr, buf.data(), buf.size());

  eventBase.loop();

  socket->close();
}

/**
 * Verify that the client fails to connect during handshake because
 * CertificateIdentityVerifier returns a failure while verifying the server's
 * leaf certificate.
 */
TEST(AsyncSSLSocketTest, SSLCertificateIdentityVerifierFailsToConnect) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<folly::SSLContext>();
  auto serverCtx = std::make_shared<folly::SSLContext>();
  getctx(clientCtx, serverCtx);
  // the client socket will default to USE_CTX, so set VERIFY here
  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  // load root certificate
  clientCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());

  // prepare a basic server (callbacks have a few EXPECTS to fullfil)
  ReadCallback readCallback(nullptr);
  // expects a failed handshake
  HandshakeCallback handshakeCallback(
      &readCallback, HandshakeCallback::ExpectType::EXPECT_ERROR);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback, serverCtx);

  std::shared_ptr<MockCertificateIdentityVerifier> verifier =
      std::make_shared<MockCertificateIdentityVerifier>();

  // Throw an exception on verification failure
  CertificateIdentityVerifierException failed{"a failed test reason"};

  // expecting context verification to be called first
  auto&& verifyContext =
      EXPECT_CALL(*verifier, verifyContext(true, _))
          .Times(AtLeast(1))
          .WillRepeatedly(Return(true));

  // expecting to only verify once, with the leaf certificate (kTestCert)
  EXPECT_CALL(
      *verifier,
      verifyLeaf(Property(
          &AsyncTransportCertificate::getIdentity, StrEq("Asox Company"))))
      .After(verifyContext)
      .WillOnce(Throw(failed));

  AsyncSSLSocket::Options opts;
  opts.verifier = std::move(verifier);

  // connect to server and handshake
  AsyncSSLSocket::UniquePtr socket(
      new AsyncSSLSocket(clientCtx, &eventBase, std::move(opts)));
  socket->connect(nullptr, server.getAddress(), 0);

  eventBase.loop();

  socket->close();
}

/**
 * Verify that the client fails to connect during handshake when
 * CertificateIdentityVerifier::verifyContext returns false,
 * and that verifyLeaf is not invoked when verifyContext fails.
 */
TEST(AsyncSSLSocketTest, SSLCertificateIdentityVerifierContextFailure) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<folly::SSLContext>();
  auto serverCtx = std::make_shared<folly::SSLContext>();
  getctx(clientCtx, serverCtx);
  // the client socket will default to USE_CTX, so set VERIFY here
  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  // load root certificate
  clientCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());

  // prepare a basic server (callbacks have a few EXPECTS to fulfill)
  ReadCallback readCallback(nullptr);
  // expects a failed handshake
  HandshakeCallback handshakeCallback(
      &readCallback, HandshakeCallback::ExpectType::EXPECT_ERROR);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback, serverCtx);

  std::shared_ptr<StrictMock<MockCertificateIdentityVerifier>> verifier =
      std::make_shared<StrictMock<MockCertificateIdentityVerifier>>();

  // verifyContext should be called and return false to fail verification
  EXPECT_CALL(*verifier, verifyContext(true, _)).WillOnce(Return(false));

  // verifyLeaf should NOT be called since verifyContext failed
  EXPECT_CALL(*verifier, verifyLeaf).Times(0);

  AsyncSSLSocket::Options opts;
  opts.verifier = std::move(verifier);

  // connect to server and handshake
  AsyncSSLSocket::UniquePtr socket(
      new AsyncSSLSocket(clientCtx, &eventBase, std::move(opts)));
  socket->connect(nullptr, server.getAddress(), 0);

  eventBase.loop();

  socket->close();
}

/**
 * Verify that verifyContext has access to the X509_STORE_CTX
 * and can inspect the certificate chain.
 */
TEST(AsyncSSLSocketTest, SSLCertificateIdentityVerifierContextInspectsChain) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<folly::SSLContext>();
  auto serverCtx = std::make_shared<folly::SSLContext>();
  getctx(clientCtx, serverCtx);
  // the client socket will default to USE_CTX, so set VERIFY here
  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  // load root certificate
  clientCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());

  // prepare a basic server (callbacks have a few EXPECTS to fulfill)
  ReadCallback readCallback(nullptr);
  // expects successful handshake
  HandshakeCallback handshakeCallback(&readCallback);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback, serverCtx);

  std::shared_ptr<MockCertificateIdentityVerifier> verifier =
      std::make_shared<MockCertificateIdentityVerifier>();

  // verifyContext should be called and have access to X509_STORE_CTX
  auto&& verifyContext =
      EXPECT_CALL(*verifier, verifyContext(true, _))
          .Times(AtLeast(1))
          .WillRepeatedly(WithArg<1>([](X509_STORE_CTX* ctx) {
            // Verify we can access the cert chain
            X509* cert = X509_STORE_CTX_get_current_cert(ctx);
            EXPECT_NE(cert, nullptr);

            // Verify we can get the chain depth
            int depth = X509_STORE_CTX_get_error_depth(ctx);
            EXPECT_GE(depth, 0);

            return true;
          }));

  // expecting to verify leaf certificate after context verification
  EXPECT_CALL(
      *verifier,
      verifyLeaf(Property(
          &AsyncTransportCertificate::getIdentity, StrEq("Asox Company"))))
      .After(verifyContext)
      .WillOnce(Return(ByMove(
          std::make_unique<folly::ssl::BasicTransportCertificate>(
              "Asox Company", readCertFromFile(kTestCert)))));

  AsyncSSLSocket::Options opts;
  opts.verifier = std::move(verifier);

  // connect to server and handshake
  AsyncSSLSocket::UniquePtr socket(
      new AsyncSSLSocket(clientCtx, &eventBase, std::move(opts)));
  socket->connect(nullptr, server.getAddress(), 0);

  // write to satisfy server ReadCallback EXPECTs
  std::array<uint8_t, 128> buf;
  memset(buf.data(), 'a', buf.size());
  socket->write(nullptr, buf.data(), buf.size());

  eventBase.loop();

  socket->close();
}

/**
 * Verify that the client's CertificateIdentityVerifier is not invoked if
 * OpenSSL's verification fails. (With no HandshakeCB.)
 */
TEST(AsyncSSLSocketTest, SSLCertificateIdentityVerifierNotInvokedX509Failure) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<folly::SSLContext>();
  auto serverCtx = std::make_shared<folly::SSLContext>();
  getctx(clientCtx, serverCtx);
  // the client socket will default to USE_CTX, so set VERIFY here
  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  // DO NOT load root certificate, so that server certificate is rejected

  // prepare a basic server (callbacks have a few EXPECTS to fullfil)
  ReadCallback readCallback(nullptr);
  // expects successful handshake
  HandshakeCallback handshakeCallback(
      &readCallback, HandshakeCallback::ExpectType::EXPECT_ERROR);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback, serverCtx);

  // do not override verification result
  std::shared_ptr<StrictMock<MockCertificateIdentityVerifier>> verifier =
      std::make_shared<StrictMock<MockCertificateIdentityVerifier>>();
  EXPECT_CALL(*verifier, verifyContext(false, _)).WillOnce(Return(false));

  AsyncSSLSocket::Options opts;
  opts.verifier = std::move(verifier);

  // connect to server and handshake
  AsyncSSLSocket::UniquePtr socket(
      new AsyncSSLSocket(clientCtx, &eventBase, std::move(opts)));
  socket->connect(nullptr, server.getAddress(), 0);

  eventBase.loop();

  socket->close();
}

/**
 * Verify that the client CertificateIdentityVerifier is not invoked if
 * HandshakeCB::handshakeVer verification fails.
 */
TEST(
    AsyncSSLSocketTest,
    SSLCertificateIdentityVerifierNotInvokedHandshakeCBFailure) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<folly::SSLContext>();
  auto serverCtx = std::make_shared<folly::SSLContext>();
  getctx(clientCtx, serverCtx);
  // the client socket will default to USE_CTX, so set VERIFY here
  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  // load root certificate
  clientCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());

  NetworkSocket fds[2];
  getfds(fds);

  AsyncSocket::UniquePtr rawClient(new AsyncSocket(&eventBase, fds[0]));
  AsyncSocket::UniquePtr rawServer(new AsyncSocket(&eventBase, fds[1]));

  // should be invoked to only verify context
  std::shared_ptr<StrictMock<MockCertificateIdentityVerifier>> verifier =
      std::make_shared<StrictMock<MockCertificateIdentityVerifier>>();
  EXPECT_CALL(*verifier, verifyContext)
      .Times(AtLeast(1))
      .WillRepeatedly(Return(true));

  AsyncSSLSocket::Options clientOpts;
  clientOpts.verifier = verifier;

  AsyncSSLSocket::Options serverOpts;
  serverOpts.isServer = true;

  AsyncSSLSocket::UniquePtr clientSock(new AsyncSSLSocket(
      clientCtx, std::move(rawClient), std::move(clientOpts)));
  AsyncSSLSocket::UniquePtr serverSock(new AsyncSSLSocket(
      serverCtx, std::move(rawServer), std::move(serverOpts)));

  serverSock->sslAccept(nullptr, std::chrono::milliseconds::zero());

  StrictMock<MockHandshakeCB> clientHandshakeCB;

  // Force the end entity certificate, which normally is successfully verified,
  // to be considered as unsuccessful
  EXPECT_CALL(clientHandshakeCB, handshakeVerImpl(clientSock.get(), true, _))
      .Times(AtLeast(1))
      .WillRepeatedly([&](auto&&, bool preverifyOk, auto&& ctx) {
        auto currentDepth = X509_STORE_CTX_get_error_depth(ctx);
        if (currentDepth == 0) {
          EXPECT_TRUE(preverifyOk);
          return false;
        }
        return preverifyOk;
      });

  // failure callback to verify handshake failed
  EXPECT_CALL(clientHandshakeCB, handshakeErrImpl(clientSock.get(), _));

  clientSock->sslConn(&clientHandshakeCB);

  eventBase.loop();

  clientSock->close();
  serverSock->close();
}

/**
 * Verify that the client CertificateIdentityVerifier is invoked on a server
 * socket when peer verification is requested.
 */
TEST(AsyncSSLSocketTest, SSLCertificateIdentityVerifierSucceedsOnServer) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<folly::SSLContext>();
  auto serverCtx = std::make_shared<folly::SSLContext>();
  getctx(clientCtx, serverCtx);
  // the client socket will default to USE_CTX, so set VERIFY here
  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  // load root certificate
  clientCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());
  // load identity and key on client, it's the same identity as server just for
  // convenience
  clientCtx->loadCertificate(find_resource(kTestCert).c_str());
  clientCtx->loadPrivateKey(find_resource(kTestKey).c_str());
  // instruct server to verify client
  serverCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  serverCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());

  NetworkSocket fds[2];
  getfds(fds);

  AsyncSocket::UniquePtr rawClient(new AsyncSocket(&eventBase, fds[0]));
  AsyncSocket::UniquePtr rawServer(new AsyncSocket(&eventBase, fds[1]));

  // client and server verifiers should verify only once each
  std::shared_ptr<MockCertificateIdentityVerifier> clientVerifier =
      std::make_shared<MockCertificateIdentityVerifier>();
  auto&& clientVerifyContext =
      EXPECT_CALL(*clientVerifier, verifyContext(true, _))
          .Times(AtLeast(1))
          .WillRepeatedly(Return(true));
  EXPECT_CALL(
      *clientVerifier,
      verifyLeaf(Property(
          &AsyncTransportCertificate::getIdentity, StrEq("Asox Company"))))
      .After(clientVerifyContext)
      .WillOnce(Return(ByMove(
          std::make_unique<folly::ssl::BasicTransportCertificate>(
              "Asox Company", readCertFromFile(kTestCert)))));
  std::shared_ptr<StrictMock<MockCertificateIdentityVerifier>> serverVerifier =
      std::make_shared<StrictMock<MockCertificateIdentityVerifier>>();
  auto&& serverVerifyContext =
      EXPECT_CALL(*serverVerifier, verifyContext(true, _))
          .Times(AtLeast(1))
          .WillRepeatedly(Return(true));
  EXPECT_CALL(
      *serverVerifier,
      verifyLeaf(Property(
          &AsyncTransportCertificate::getIdentity, StrEq("Asox Company"))))
      .After(serverVerifyContext)
      .WillOnce(Return(ByMove(
          std::make_unique<folly::ssl::BasicTransportCertificate>(
              "Asox Company", readCertFromFile(kTestCert)))));

  AsyncSSLSocket::Options clientOpts;
  clientOpts.verifier = clientVerifier;

  AsyncSSLSocket::Options serverOpts;
  serverOpts.isServer = true;
  serverOpts.verifier = serverVerifier;

  AsyncSSLSocket::UniquePtr clientSock(new AsyncSSLSocket(
      clientCtx, std::move(rawClient), std::move(clientOpts)));
  AsyncSSLSocket::UniquePtr serverSock(new AsyncSSLSocket(
      serverCtx, std::move(rawServer), std::move(serverOpts)));

  // no HandshakeCBs anywhere
  serverSock->sslAccept(nullptr, std::chrono::milliseconds::zero());
  clientSock->sslConn(nullptr);

  eventBase.loop();

  clientSock->close();
  serverSock->close();
}

/**
 * Verify that the options in SSLContext can be overridden in
 * sslConnect/Accept.i.e specifying that no validation should be performed
 * allows an otherwise-invalid certificate to be accepted and doesn't fire
 * the validation callback.
 */
TEST(AsyncSSLSocketTest, OverrideSSLCtxDisableVerify) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto dfServerCtx = std::make_shared<SSLContext>();

  NetworkSocket fds[2];
  getfds(fds);
  getctx(clientCtx, dfServerCtx);

  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  dfServerCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(dfServerCtx, &eventBase, fds[1], true));

  SSLHandshakeClientNoVerify client(std::move(clientSock), false, false);
  clientCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());

  SSLHandshakeServerNoVerify server(std::move(serverSock), false, false);

  eventBase.loop();

  EXPECT_TRUE(!client.handshakeVerify_);
  EXPECT_TRUE(client.handshakeSuccess_);
  EXPECT_TRUE(!client.handshakeError_);
  EXPECT_LE(0, client.handshakeTime.count());
  EXPECT_TRUE(!server.handshakeVerify_);
  EXPECT_TRUE(server.handshakeSuccess_);
  EXPECT_TRUE(!server.handshakeError_);
  EXPECT_LE(0, server.handshakeTime.count());
}

/**
 * Verify that the options in SSLContext can be overridden in
 * sslConnect/Accept. Enable verification even if context says otherwise.
 * Test requireClientCert with client cert
 */
TEST(AsyncSSLSocketTest, OverrideSSLCtxEnableVerify) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto serverCtx = std::make_shared<SSLContext>();
  serverCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::NO_VERIFY);
  serverCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  serverCtx->loadPrivateKey(find_resource(kTestKey).c_str());
  serverCtx->loadCertificate(find_resource(kTestCert).c_str());
  serverCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());
  serverCtx->setSupportedClientCertificateAuthorityNamesFromFile(
      find_resource(kTestCA).c_str());

  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::NO_VERIFY);
  clientCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  clientCtx->loadPrivateKey(find_resource(kTestKey).c_str());
  clientCtx->loadCertificate(find_resource(kTestCert).c_str());
  clientCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());

  NetworkSocket fds[2];
  getfds(fds);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));

  SSLHandshakeClientDoVerify client(std::move(clientSock), true, true);
  SSLHandshakeServerDoVerify server(std::move(serverSock), true, true);

  eventBase.loop();

  EXPECT_TRUE(client.handshakeVerify_);
  EXPECT_TRUE(client.handshakeSuccess_);
  EXPECT_FALSE(client.handshakeError_);
  EXPECT_LE(0, client.handshakeTime.count());
  EXPECT_TRUE(server.handshakeVerify_);
  EXPECT_TRUE(server.handshakeSuccess_);
  EXPECT_FALSE(server.handshakeError_);
  EXPECT_LE(0, server.handshakeTime.count());
}

/**
 * Verify that the client's verification callback is able to override
 * the preverification failure and allow a successful connection.
 */
TEST(AsyncSSLSocketTest, SSLHandshakeValidationOverride) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto dfServerCtx = std::make_shared<SSLContext>();

  NetworkSocket fds[2];
  getfds(fds);
  getctx(clientCtx, dfServerCtx);

  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  dfServerCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(dfServerCtx, &eventBase, fds[1], true));

  SSLHandshakeClient client(std::move(clientSock), false, true);
  SSLHandshakeServer server(std::move(serverSock), true, true);

  eventBase.loop();

  EXPECT_TRUE(client.handshakeVerify_);
  EXPECT_TRUE(client.handshakeSuccess_);
  EXPECT_TRUE(!client.handshakeError_);
  EXPECT_LE(0, client.handshakeTime.count());
  EXPECT_TRUE(!server.handshakeVerify_);
  EXPECT_TRUE(server.handshakeSuccess_);
  EXPECT_TRUE(!server.handshakeError_);
  EXPECT_LE(0, server.handshakeTime.count());
}

/**
 * Verify that specifying that no validation should be performed allows an
 * otherwise-invalid certificate to be accepted and doesn't fire the validation
 * callback.
 */
TEST(AsyncSSLSocketTest, SSLHandshakeValidationSkip) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto dfServerCtx = std::make_shared<SSLContext>();

  NetworkSocket fds[2];
  getfds(fds);
  getctx(clientCtx, dfServerCtx);

  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::NO_VERIFY);
  dfServerCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::NO_VERIFY);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(dfServerCtx, &eventBase, fds[1], true));

  SSLHandshakeClient client(std::move(clientSock), false, false);
  SSLHandshakeServer server(std::move(serverSock), false, false);

  eventBase.loop();

  EXPECT_TRUE(!client.handshakeVerify_);
  EXPECT_TRUE(client.handshakeSuccess_);
  EXPECT_TRUE(!client.handshakeError_);
  EXPECT_LE(0, client.handshakeTime.count());
  EXPECT_TRUE(!server.handshakeVerify_);
  EXPECT_TRUE(server.handshakeSuccess_);
  EXPECT_TRUE(!server.handshakeError_);
  EXPECT_LE(0, server.handshakeTime.count());
}

/**
 * Test requireClientCert with client cert
 */
TEST(AsyncSSLSocketTest, ClientCertHandshakeSuccess) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto serverCtx = std::make_shared<SSLContext>();
  serverCtx->setVerificationOption(
      SSLContext::SSLVerifyPeerEnum::VERIFY_REQ_CLIENT_CERT);
  serverCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  serverCtx->loadPrivateKey(find_resource(kTestKey).c_str());
  serverCtx->loadCertificate(find_resource(kTestCert).c_str());
  serverCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());
  serverCtx->setSupportedClientCertificateAuthorityNamesFromFile(
      find_resource(kTestCA).c_str());

  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  clientCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  clientCtx->loadPrivateKey(find_resource(kTestKey).c_str());
  clientCtx->loadCertificate(find_resource(kTestCert).c_str());
  clientCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());

  NetworkSocket fds[2];
  getfds(fds);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));

  SSLHandshakeClient client(std::move(clientSock), true, true);
  SSLHandshakeServer server(std::move(serverSock), true, true);

  eventBase.loop();

  EXPECT_TRUE(client.handshakeVerify_);
  EXPECT_TRUE(client.handshakeSuccess_);
  EXPECT_FALSE(client.handshakeError_);
  EXPECT_LE(0, client.handshakeTime.count());
  EXPECT_TRUE(server.handshakeVerify_);
  EXPECT_TRUE(server.handshakeSuccess_);
  EXPECT_FALSE(server.handshakeError_);
  EXPECT_LE(0, server.handshakeTime.count());

  // check certificates
  auto clientSsl = std::move(client).moveSocket();
  auto serverSsl = std::move(server).moveSocket();

  auto clientPeer = clientSsl->getPeerCertificate();
  auto clientSelf = clientSsl->getSelfCertificate();
  auto serverPeer = serverSsl->getPeerCertificate();
  auto serverSelf = serverSsl->getSelfCertificate();

  EXPECT_NE(clientPeer, nullptr);
  EXPECT_NE(clientSelf, nullptr);
  EXPECT_NE(serverPeer, nullptr);
  EXPECT_NE(serverSelf, nullptr);

  EXPECT_EQ(clientPeer->getIdentity(), serverSelf->getIdentity());
  EXPECT_EQ(clientSelf->getIdentity(), serverPeer->getIdentity());
}

/**
 * Test requireClientCert with no client cert
 */
TEST(AsyncSSLSocketTest, NoClientCertHandshakeError) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto serverCtx = std::make_shared<SSLContext>();
  serverCtx->setVerificationOption(
      SSLContext::SSLVerifyPeerEnum::VERIFY_REQ_CLIENT_CERT);
  serverCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  serverCtx->loadPrivateKey(find_resource(kTestKey).c_str());
  serverCtx->loadCertificate(find_resource(kTestCert).c_str());
  serverCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());
  serverCtx->setSupportedClientCertificateAuthorityNamesFromFile(
      find_resource(kTestCA).c_str());
  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::NO_VERIFY);
  clientCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");

  NetworkSocket fds[2];
  getfds(fds);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));

  SSLHandshakeClient client(std::move(clientSock), false, false);
  SSLHandshakeServer server(std::move(serverSock), false, false);

  eventBase.loop();

  EXPECT_FALSE(server.handshakeVerify_);
  EXPECT_FALSE(server.handshakeSuccess_);
  EXPECT_TRUE(server.handshakeError_);
  EXPECT_LE(0, client.handshakeTime.count());
  EXPECT_LE(0, server.handshakeTime.count());
}

static void makeNonBlockingPipe(int pipefds[2]) {
  if (fileops::pipe(pipefds) != 0) {
    throw std::runtime_error("Cannot create pipe");
  }
  if (::fcntl(pipefds[0], F_SETFL, O_NONBLOCK) != 0) {
    throw std::runtime_error("Cannot set pipe to nonblocking");
  }
  if (::fcntl(pipefds[1], F_SETFL, O_NONBLOCK) != 0) {
    throw std::runtime_error("Cannot set pipe to nonblocking");
  }
}

// Custom RSA private key encryption method
static int kRSAExIndex = -1;
static int kRSAEvbExIndex = -1;
static int kRSASocketExIndex = -1;
static constexpr StringPiece kEngineId = "AsyncSSLSocketTest";

static int customRsaPrivEnc(
    int flen,
    const unsigned char* from,
    unsigned char* to,
    RSA* rsa,
    int padding) {
  LOG(INFO) << "rsa_priv_enc";
  EventBase* asyncJobEvb =
      reinterpret_cast<EventBase*>(RSA_get_ex_data(rsa, kRSAEvbExIndex));
  CHECK(asyncJobEvb);

  RSA* actualRSA = reinterpret_cast<RSA*>(RSA_get_ex_data(rsa, kRSAExIndex));
  CHECK(actualRSA);

  AsyncSSLSocket* socket = reinterpret_cast<AsyncSSLSocket*>(
      RSA_get_ex_data(rsa, kRSASocketExIndex));

  ASYNC_JOB* job = ASYNC_get_current_job();
  if (job == nullptr) {
    throw std::runtime_error("Expected call in job context");
  }
  ASYNC_WAIT_CTX* waitctx = ASYNC_get_wait_ctx(job);
  OSSL_ASYNC_FD pipefds[2] = {0, 0};
  makeNonBlockingPipe(pipefds);
  if (!ASYNC_WAIT_CTX_set_wait_fd(
          waitctx, kEngineId.data(), pipefds[0], nullptr, nullptr)) {
    throw std::runtime_error("Cannot set wait fd");
  }
  int ret = 0;
  int* retptr = &ret;

  auto hand = folly::NetworkSocket::native_handle_type(pipefds[1]);
  auto asyncPipeWriter = folly::AsyncPipeWriter::newWriter(
      asyncJobEvb, folly::NetworkSocket(hand));

  if (socket) {
    LOG(INFO) << "Got a socket passed in, closing it...";
    socket->closeNow();
  }
  asyncJobEvb->runInEventBaseThread(
      [retptr = retptr,
       flen = flen,
       from = from,
       to = to,
       padding = padding,
       actualRSA = actualRSA,
       writer = std::move(asyncPipeWriter)]() {
        LOG(INFO) << "Running job";
        *retptr = RSA_meth_get_priv_enc(RSA_PKCS1_OpenSSL())(
            flen, from, to, actualRSA, padding);
        LOG(INFO) << "Finished job, writing to pipe";
        uint8_t byte = *retptr > 0 ? 1 : 0;
        writer->write(nullptr, &byte, 1);
      });

  LOG(INFO) << "About to pause job";

  ASYNC_pause_job();
  LOG(INFO) << "Resumed job with ret: " << ret;
  return ret;
}

void rsaFree(void*, void* ptr, CRYPTO_EX_DATA*, int, long, void*) {
  LOG(INFO) << "RSA_free is called with ptr " << std::hex << ptr;
  if (ptr == nullptr) {
    LOG(INFO) << "Returning early from rsaFree because ptr is null";
    return;
  }
  RSA* rsa = (RSA*)ptr;
  auto meth = RSA_get_method(rsa);
  if (meth != RSA_get_default_method()) {
    auto nonconst = const_cast<RSA_METHOD*>(meth);
    RSA_meth_free(nonconst);
    RSA_set_method(rsa, RSA_get_default_method());
  }
  RSA_free(rsa);
}

struct RSAPointers {
  RSA* actualrsa{nullptr};
  RSA* dummyrsa{nullptr};
  RSA_METHOD* meth{nullptr};
};

inline void RSAPointersFree(RSAPointers* p) {
  if (p->meth && p->dummyrsa && RSA_get_method(p->dummyrsa) == p->meth) {
    RSA_set_method(p->dummyrsa, RSA_get_default_method());
  }

  if (p->meth) {
    LOG(INFO) << "Freeing meth";
    RSA_meth_free(p->meth);
  }

  if (p->actualrsa) {
    LOG(INFO) << "Freeing actualrsa";
    RSA_free(p->actualrsa);
  }

  if (p->dummyrsa) {
    LOG(INFO) << "Freeing dummyrsa";
    RSA_free(p->dummyrsa);
  }

  delete p;
}

using RSAPointersDeleter =
    folly::static_function_deleter<RSAPointers, RSAPointersFree>;

std::unique_ptr<RSAPointers, RSAPointersDeleter> setupCustomRSA(
    const char* certPath, const char* keyPath, EventBase* jobEvb) {
  auto certPEM = getFileAsBuf(certPath);
  auto keyPEM = getFileAsBuf(keyPath);

  ssl::BioUniquePtr certBio(
      BIO_new_mem_buf((void*)certPEM.data(), certPEM.size()));
  ssl::BioUniquePtr keyBio(
      BIO_new_mem_buf((void*)keyPEM.data(), keyPEM.size()));

  ssl::X509UniquePtr cert(
      PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr));
  ssl::EvpPkeyUniquePtr evpPkey(
      PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr));
  ssl::EvpPkeyUniquePtr publicEvpPkey(X509_get_pubkey(cert.get()));

  std::unique_ptr<RSAPointers, RSAPointersDeleter> ret(new RSAPointers());

  RSA* actualrsa = EVP_PKEY_get1_RSA(evpPkey.get());
  LOG(INFO) << "actualrsa ptr " << std::hex << (void*)actualrsa;
  RSA* dummyrsa = EVP_PKEY_get1_RSA(publicEvpPkey.get());
  if (dummyrsa == nullptr) {
    throw std::runtime_error("Couldn't get RSA cert public factors");
  }
  RSA_METHOD* meth = RSA_meth_dup(RSA_get_default_method());
  if (meth == nullptr || RSA_meth_set1_name(meth, "Async RSA method") == 0 ||
      RSA_meth_set_priv_enc(meth, customRsaPrivEnc) == 0 ||
      RSA_meth_set_flags(meth, RSA_METHOD_FLAG_NO_CHECK) == 0) {
    throw std::runtime_error("Cannot create async RSA_METHOD");
  }
  RSA_set_method(dummyrsa, meth);
  RSA_set_flags(dummyrsa, RSA_FLAG_EXT_PKEY);

  kRSAExIndex = RSA_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
  kRSAEvbExIndex = RSA_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
  kRSASocketExIndex =
      RSA_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
  CHECK_NE(kRSAExIndex, -1);
  CHECK_NE(kRSAEvbExIndex, -1);
  CHECK_NE(kRSASocketExIndex, -1);
  RSA_set_ex_data(dummyrsa, kRSAExIndex, actualrsa);
  RSA_set_ex_data(dummyrsa, kRSAEvbExIndex, jobEvb);

  ret->actualrsa = actualrsa;
  ret->dummyrsa = dummyrsa;
  ret->meth = meth;

  return ret;
}

// TODO: disabled with ASAN doesn't play nice with ASYNC for some reason
#ifndef FOLLY_SANITIZE_ADDRESS
TEST(AsyncSSLSocketTest, OpenSSL110AsyncTest) {
  ASYNC_init_thread(1, 1);
  EventBase eventBase;
  ScopedEventBaseThread jobEvbThread;
  auto clientCtx = std::make_shared<SSLContext>();
  auto serverCtx = std::make_shared<SSLContext>();
  serverCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  serverCtx->loadCertificate(find_resource(kTestCert).c_str());
  serverCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());
  serverCtx->setSupportedClientCertificateAuthorityNamesFromFile(
      find_resource(kTestCA).c_str());

  auto rsaPointers =
      setupCustomRSA(kTestCert, kTestKey, jobEvbThread.getEventBase());
  CHECK(rsaPointers->dummyrsa);
  // up-refs dummyrsa
  SSL_CTX_use_RSAPrivateKey(serverCtx->getSSLCtx(), rsaPointers->dummyrsa);
  SSL_CTX_set_mode(serverCtx->getSSLCtx(), SSL_MODE_ASYNC);

  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::NO_VERIFY);
  clientCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");

  NetworkSocket fds[2];
  getfds(fds);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));

  SSLHandshakeClient client(std::move(clientSock), false, false);
  SSLHandshakeServer server(std::move(serverSock), false, false);

  eventBase.loop();

  EXPECT_TRUE(server.handshakeSuccess_);
  EXPECT_TRUE(client.handshakeSuccess_);
  ASYNC_cleanup_thread();
}

TEST(AsyncSSLSocketTest, OpenSSL110AsyncTestFailure) {
  ASYNC_init_thread(1, 1);
  EventBase eventBase;
  ScopedEventBaseThread jobEvbThread;
  auto clientCtx = std::make_shared<SSLContext>();
  auto serverCtx = std::make_shared<SSLContext>();
  serverCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  serverCtx->loadCertificate(find_resource(kTestCert).c_str());
  serverCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());
  serverCtx->setSupportedClientCertificateAuthorityNamesFromFile(
      find_resource(kTestCA).c_str());
  // Set the wrong key for the cert
  auto rsaPointers =
      setupCustomRSA(kTestCert, kClientTestKey, jobEvbThread.getEventBase());
  CHECK(rsaPointers->dummyrsa);
  SSL_CTX_use_RSAPrivateKey(serverCtx->getSSLCtx(), rsaPointers->dummyrsa);
  SSL_CTX_set_mode(serverCtx->getSSLCtx(), SSL_MODE_ASYNC);

  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::NO_VERIFY);
  clientCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");

  NetworkSocket fds[2];
  getfds(fds);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));

  SSLHandshakeClient client(std::move(clientSock), false, false);
  SSLHandshakeServer server(std::move(serverSock), false, false);

  eventBase.loop();

  EXPECT_TRUE(server.handshakeError_);
  EXPECT_TRUE(client.handshakeError_);
  ASYNC_cleanup_thread();
}

TEST(AsyncSSLSocketTest, OpenSSL110AsyncTestClosedWithCallbackPending) {
  ASYNC_init_thread(1, 1);
  EventBase eventBase;
  std::optional<EventBaseThread> jobEvbThread;
  jobEvbThread.emplace();
  auto clientCtx = std::make_shared<SSLContext>();
  auto serverCtx = std::make_shared<SSLContext>();
  serverCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  serverCtx->loadCertificate(find_resource(kTestCert).c_str());
  serverCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());
  serverCtx->setSupportedClientCertificateAuthorityNamesFromFile(
      find_resource(kTestCA).c_str());

  auto rsaPointers =
      setupCustomRSA(kTestCert, kTestKey, jobEvbThread->getEventBase());
  CHECK(rsaPointers->dummyrsa);
  // up-refs dummyrsa
  SSL_CTX_use_RSAPrivateKey(serverCtx->getSSLCtx(), rsaPointers->dummyrsa);
  SSL_CTX_set_mode(serverCtx->getSSLCtx(), SSL_MODE_ASYNC);

  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::NO_VERIFY);
  clientCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");

  NetworkSocket fds[2];
  getfds(fds);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));

  RSA_set_ex_data(rsaPointers->dummyrsa, kRSASocketExIndex, serverSock.get());

  SSLHandshakeClient client(std::move(clientSock), false, false);
  SSLHandshakeServer server(std::move(serverSock), false, false);

  eventBase.loop();

  EXPECT_TRUE(server.handshakeError_);
  EXPECT_TRUE(client.handshakeError_);
  ASYNC_cleanup_thread();
  jobEvbThread.reset();
}
#endif // FOLLY_SANITIZE_ADDRESS

TEST(AsyncSSLSocketTest, LoadCertFromMemory) {
  using folly::ssl::OpenSSLUtils;
  auto cert = getFileAsBuf(kTestCert);
  auto key = getFileAsBuf(kTestKey);

  ssl::BioUniquePtr certBio(BIO_new(BIO_s_mem()));
  BIO_write(certBio.get(), cert.data(), cert.size());
  ssl::BioUniquePtr keyBio(BIO_new(BIO_s_mem()));
  BIO_write(keyBio.get(), key.data(), key.size());

  // Create SSL structs from buffers to get properties
  ssl::X509UniquePtr certStruct(
      PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr));
  ssl::EvpPkeyUniquePtr keyStruct(
      PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr));
  certBio = nullptr;
  keyBio = nullptr;

  auto origCommonName = OpenSSLUtils::getCommonName(certStruct.get());
  auto origKeySize = EVP_PKEY_bits(keyStruct.get());
  certStruct = nullptr;
  keyStruct = nullptr;

  auto ctx = std::make_shared<SSLContext>();
  ctx->loadPrivateKeyFromBufferPEM(key);
  ctx->loadCertificateFromBufferPEM(cert);
  ctx->loadTrustedCertificates(find_resource(kTestCA).c_str());

  ssl::SSLUniquePtr ssl(ctx->createSSL());

  auto newCert = SSL_get_certificate(ssl.get());
  auto newKey = SSL_get_privatekey(ssl.get());

  // Get properties from SSL struct
  auto newCommonName = OpenSSLUtils::getCommonName(newCert);
  auto newKeySize = EVP_PKEY_bits(newKey);

  // Check that the key and cert have the expected properties
  EXPECT_EQ(origCommonName, newCommonName);
  EXPECT_EQ(origKeySize, newKeySize);
}

TEST(AsyncSSLSocketTest, MinWriteSizeTest) {
  EventBase eb;

  // Set up SSL context.
  auto sslContext = std::make_shared<SSLContext>();
  sslContext->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");

  // create SSL socket
  AsyncSSLSocket::UniquePtr socket(new AsyncSSLSocket(sslContext, &eb));

  EXPECT_EQ(1500, socket->getMinWriteSize());

  socket->setMinWriteSize(0);
  EXPECT_EQ(0, socket->getMinWriteSize());
  socket->setMinWriteSize(50000);
  EXPECT_EQ(50000, socket->getMinWriteSize());
}

class ReadCallbackTerminator : public ReadCallback {
 public:
  ReadCallbackTerminator(EventBase* base, WriteCallbackBase* wcb)
      : ReadCallback(wcb), base_(base) {}

  // Do not write data back, terminate the loop.
  void readDataAvailable(size_t len) noexcept override {
    std::cerr << "readDataAvailable, len " << len << std::endl;

    currentBuffer.length = len;

    buffers.push_back(currentBuffer);
    currentBuffer.reset();
    state = STATE_SUCCEEDED;

    socket_->setReadCB(nullptr);
    base_->terminateLoopSoon();
  }

 private:
  EventBase* base_;
};

/**
 * Test a full unencrypted codepath
 */
TEST(AsyncSSLSocketTest, UnencryptedTest) {
  EventBase base;

  auto clientCtx = std::make_shared<folly::SSLContext>();
  auto serverCtx = std::make_shared<folly::SSLContext>();
  NetworkSocket fds[2];
  getfds(fds);
  getctx(clientCtx, serverCtx);
  auto client =
      AsyncSSLSocket::newSocket(clientCtx, &base, fds[0], false, true);
  std::shared_ptr<AsyncSSLSocket> server =
      AsyncSSLSocket::newSocket(serverCtx, &base, fds[1], true, true);

  ReadCallbackTerminator readCallback(&base, nullptr);
  server->setReadCB(&readCallback);
  readCallback.setSocket(server);

  uint8_t buf[128];
  memset(buf, 'a', sizeof(buf));
  client->write(nullptr, buf, sizeof(buf));

  // Check that bytes are unencrypted
  char c;
  EXPECT_EQ(1, netops::recv(fds[1], &c, 1, MSG_PEEK));
  EXPECT_EQ('a', c);

  EventBaseAborter eba(&base, 3000);
  base.loop();

  EXPECT_EQ(1, readCallback.buffers.size());
  EXPECT_EQ(AsyncSSLSocket::STATE_UNENCRYPTED, client->getSSLState());

  server->setReadCB(&readCallback);

  // Unencrypted
  server->sslAccept(nullptr);
  client->sslConn(nullptr);

  // Do NOT wait for handshake, writing should be queued and happen after

  client->write(nullptr, buf, sizeof(buf));

  // Check that bytes are *not* unencrypted
  char c2;
  EXPECT_EQ(1, netops::recv(fds[1], &c2, 1, MSG_PEEK));
  EXPECT_NE('a', c2);

  base.loop();

  EXPECT_EQ(2, readCallback.buffers.size());
  EXPECT_EQ(AsyncSSLSocket::STATE_ESTABLISHED, client->getSSLState());
}

TEST(AsyncSSLSocketTest, ConnectUnencryptedTest) {
  auto clientCtx = std::make_shared<folly::SSLContext>();
  auto serverCtx = std::make_shared<folly::SSLContext>();
  getctx(clientCtx, serverCtx);

  WriteCallbackBase writeCallback;
  ReadCallback readCallback(&writeCallback);
  HandshakeCallback handshakeCallback(&readCallback);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback);

  EventBase evb;
  std::shared_ptr<AsyncSSLSocket> socket =
      AsyncSSLSocket::newSocket(clientCtx, &evb, true);
  socket->connect(nullptr, server.getAddress(), 0);

  evb.loop();

  EXPECT_EQ(AsyncSSLSocket::STATE_UNENCRYPTED, socket->getSSLState());
  socket->sslConn(nullptr);
  evb.loop();
  EXPECT_EQ(AsyncSSLSocket::STATE_ESTABLISHED, socket->getSSLState());

  // write()
  std::array<uint8_t, 128> buf;
  memset(buf.data(), 'a', buf.size());
  socket->write(nullptr, buf.data(), buf.size());

  // wait until the server has reflected all data written by the client
  SemiFuture<StateEnum> future = writeCallback.getSemiFuture();
  StateEnum state = std::move(future).get(std::chrono::seconds(3));
  EXPECT_EQ(state, STATE_SUCCEEDED);
  socket->close();
}

/**
 * Test acceptrunner in various situations
 */
TEST(AsyncSSLSocketTest, SSLAcceptRunnerBasic) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto serverCtx = std::make_shared<SSLContext>();
  serverCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  serverCtx->loadPrivateKey(find_resource(kTestKey).c_str());
  serverCtx->loadCertificate(find_resource(kTestCert).c_str());

  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  clientCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  clientCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());

  NetworkSocket fds[2];
  getfds(fds);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));

  serverCtx->sslAcceptRunner(std::make_unique<SSLAcceptEvbRunner>(&eventBase));

  SSLHandshakeClient client(std::move(clientSock), true, true);
  SSLHandshakeServer server(std::move(serverSock), true, true);

  eventBase.loop();

  EXPECT_TRUE(client.handshakeSuccess_);
  EXPECT_FALSE(client.handshakeError_);
  EXPECT_LE(0, client.handshakeTime.count());
  EXPECT_TRUE(server.handshakeSuccess_);
  EXPECT_FALSE(server.handshakeError_);
  EXPECT_LE(0, server.handshakeTime.count());
}

TEST(AsyncSSLSocketTest, SSLAcceptRunnerAcceptError) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto serverCtx = std::make_shared<SSLContext>();
  serverCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  serverCtx->loadPrivateKey(find_resource(kTestKey).c_str());
  serverCtx->loadCertificate(find_resource(kTestCert).c_str());

  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  clientCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  clientCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());

  NetworkSocket fds[2];
  getfds(fds);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));

  serverCtx->sslAcceptRunner(
      std::make_unique<SSLAcceptErrorRunner>(&eventBase));

  SSLHandshakeClient client(std::move(clientSock), true, true);
  SSLHandshakeServer server(std::move(serverSock), true, true);

  eventBase.loop();

  EXPECT_FALSE(client.handshakeSuccess_);
  EXPECT_TRUE(client.handshakeError_);
  EXPECT_FALSE(server.handshakeSuccess_);
  EXPECT_TRUE(server.handshakeError_);
}

TEST(AsyncSSLSocketTest, SSLAcceptRunnerAcceptClose) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto serverCtx = std::make_shared<SSLContext>();
  serverCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  serverCtx->loadPrivateKey(find_resource(kTestKey).c_str());
  serverCtx->loadCertificate(find_resource(kTestCert).c_str());

  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  clientCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  clientCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());

  NetworkSocket fds[2];
  getfds(fds);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));

  serverCtx->sslAcceptRunner(
      std::make_unique<SSLAcceptCloseRunner>(&eventBase, serverSock.get()));

  SSLHandshakeClient client(std::move(clientSock), true, true);
  SSLHandshakeServer server(std::move(serverSock), true, true);

  eventBase.loop();

  EXPECT_FALSE(client.handshakeSuccess_);
  EXPECT_TRUE(client.handshakeError_);
  EXPECT_FALSE(server.handshakeSuccess_);
  EXPECT_TRUE(server.handshakeError_);
}

TEST(AsyncSSLSocketTest, SSLAcceptRunnerAcceptDestroy) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto serverCtx = std::make_shared<SSLContext>();
  serverCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  serverCtx->loadPrivateKey(find_resource(kTestKey).c_str());
  serverCtx->loadCertificate(find_resource(kTestCert).c_str());

  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  clientCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  clientCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());

  NetworkSocket fds[2];
  getfds(fds);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));

  SSLHandshakeClient client(std::move(clientSock), true, true);
  SSLHandshakeServer server(std::move(serverSock), true, true);

  serverCtx->sslAcceptRunner(
      std::make_unique<SSLAcceptDestroyRunner>(&eventBase, &server));

  eventBase.loop();

  EXPECT_FALSE(client.handshakeSuccess_);
  EXPECT_TRUE(client.handshakeError_);
  EXPECT_FALSE(server.handshakeSuccess_);
  EXPECT_TRUE(server.handshakeError_);
}

TEST(AsyncSSLSocketTest, SSLAcceptRunnerFiber) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto serverCtx = std::make_shared<SSLContext>();
  serverCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  serverCtx->loadPrivateKey(find_resource(kTestKey).c_str());
  serverCtx->loadCertificate(find_resource(kTestCert).c_str());

  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  clientCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  clientCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());

  NetworkSocket fds[2];
  getfds(fds);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));

  SSLHandshakeClient client(std::move(clientSock), true, true);
  SSLHandshakeServer server(std::move(serverSock), true, true);

  serverCtx->sslAcceptRunner(
      std::make_unique<SSLAcceptFiberRunner>(&eventBase));

  eventBase.loop();

  EXPECT_TRUE(client.handshakeSuccess_);
  EXPECT_FALSE(client.handshakeError_);
  EXPECT_TRUE(server.handshakeSuccess_);
  EXPECT_FALSE(server.handshakeError_);
}

#ifdef SSL_ERROR_WANT_CLIENT_HELLO_CB
TEST(AsyncSSLSocketTest, SSLClientHelloRetryCallback) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto serverCtx = std::make_shared<SSLContext>();
  serverCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  serverCtx->loadPrivateKey(find_resource(kTestKey).c_str());
  serverCtx->loadCertificate(find_resource(kTestCert).c_str());

  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  clientCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  clientCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());

  NetworkSocket fds[2];
  getfds(fds);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));
  auto* serverSockPtr = serverSock.get();

  // State passed through the void* arg to the C callback
  struct CbState {
    bool firstCall = true;
    EventBase* evb = nullptr;
    AsyncSSLSocket* sock = nullptr;
  };
  CbState cbState;
  cbState.evb = &eventBase;
  cbState.sock = serverSockPtr;

  // Non-capturing lambda convertible to C function pointer.
  // State is accessed through the void* arg.
  SSL_CTX_set_client_hello_cb(
      serverCtx->getSSLCtx(),
      [](SSL* /*ssl*/, int* /*al*/, void* arg) -> int {
        auto* state = static_cast<CbState*>(arg);
        if (state->firstCall) {
          state->firstCall = false;
          // Schedule restart — this runs AFTER handleReturnFromSSLAccept
          // has set STATE_ASYNC_PENDING, because we're currently inside
          // SSL_accept() which is inside the SSLAcceptEvbRunner's runInLoop.
          auto* sock = state->sock;
          state->evb->runInLoop([sock]() { sock->restartSSLAccept(); });
          return SSL_CLIENT_HELLO_RETRY;
        }
        return SSL_CLIENT_HELLO_SUCCESS;
      },
      &cbState);

  serverCtx->sslAcceptRunner(std::make_unique<SSLAcceptEvbRunner>(&eventBase));

  SSLHandshakeClient client(std::move(clientSock), true, true);
  SSLHandshakeServer server(std::move(serverSock), true, true);

  eventBase.loop();

  EXPECT_TRUE(client.handshakeSuccess_);
  EXPECT_FALSE(client.handshakeError_);
  EXPECT_TRUE(server.handshakeSuccess_);
  EXPECT_FALSE(server.handshakeError_);
}
#endif

static int newCloseCb(SSL* ssl, SSL_SESSION*) {
  AsyncSSLSocket::getFromSSL(ssl)->closeNow();

  // We do not want ownership over the supplied SSL_SESSION*, we return 0 to
  // tell OpenSSL to free it on our behalf.
  return 0;
}

static SSL_SESSION* getCloseCb(SSL* ssl, const unsigned char*, int, int*) {
  AsyncSSLSocket::getFromSSL(ssl)->closeNow();
  return nullptr;
} // namespace

TEST(AsyncSSLSocketTest, SSLAcceptRunnerFiberCloseSessionCb) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto serverCtx = std::make_shared<SSLContext>();
  serverCtx->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  serverCtx->loadPrivateKey(find_resource(kTestKey).c_str());
  serverCtx->loadCertificate(find_resource(kTestCert).c_str());
  SSL_CTX_set_session_cache_mode(
      serverCtx->getSSLCtx(),
      SSL_SESS_CACHE_NO_INTERNAL | SSL_SESS_CACHE_SERVER);
  SSL_CTX_sess_set_new_cb(serverCtx->getSSLCtx(), &newCloseCb);
  SSL_CTX_sess_set_get_cb(serverCtx->getSSLCtx(), &getCloseCb);
  serverCtx->sslAcceptRunner(
      std::make_unique<SSLAcceptFiberRunner>(&eventBase));

  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::VERIFY);
  clientCtx->ciphers("AES128-SHA256");
  clientCtx->loadTrustedCertificates(find_resource(kTestCA).c_str());
  clientCtx->setOptions(SSL_OP_NO_TICKET);

  NetworkSocket fds[2];
  getfds(fds);

  AsyncSSLSocket::UniquePtr clientSock(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSock(
      new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));

  SSLHandshakeClient client(std::move(clientSock), true, true);
  SSLHandshakeServer server(std::move(serverSock), true, true);

  eventBase.loop();

  // As close() is called during session callbacks, client sees it as a
  // successful connection
  EXPECT_TRUE(client.handshakeSuccess_);
  EXPECT_FALSE(client.handshakeError_);
  EXPECT_FALSE(server.handshakeSuccess_);
  EXPECT_TRUE(server.handshakeError_);
}

TEST(AsyncSSLSocketTest, ConnResetErrorString) {
  // Start listening on a local port
  WriteCallbackBase writeCallback;
  WriteErrorCallback readCallback(&writeCallback);
  HandshakeCallback handshakeCallback(
      &readCallback, HandshakeCallback::EXPECT_ERROR);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback);

  auto socket = std::make_shared<BlockingSocket>(server.getAddress(), nullptr);
  socket->open();
  uint8_t buf[3] = {0x16, 0x03, 0x01};
  socket->write(buf, sizeof(buf));
  socket->closeWithReset();

  handshakeCallback.waitForHandshake();
  EXPECT_NE(
      handshakeCallback.errorString_.find("Network error"), std::string::npos);
  EXPECT_NE(handshakeCallback.errorString_.find("104"), std::string::npos);
}

TEST(AsyncSSLSocketTest, ConnEOFErrorString) {
  // Start listening on a local port
  WriteCallbackBase writeCallback;
  WriteErrorCallback readCallback(&writeCallback);
  HandshakeCallback handshakeCallback(
      &readCallback, HandshakeCallback::EXPECT_ERROR);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback);

  auto socket = std::make_shared<BlockingSocket>(server.getAddress(), nullptr);
  socket->open();
  uint8_t buf[3] = {0x16, 0x03, 0x01};
  socket->write(buf, sizeof(buf));
  socket->close();

  handshakeCallback.waitForHandshake();
  EXPECT_NE(
      handshakeCallback.errorString_.find("Network error"), std::string::npos);
}

TEST(AsyncSSLSocketTest, ConnOpenSSLErrorString) {
  // Start listening on a local port
  WriteCallbackBase writeCallback;
  WriteErrorCallback readCallback(&writeCallback);
  HandshakeCallback handshakeCallback(
      &readCallback, HandshakeCallback::EXPECT_ERROR);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback);

  auto socket = std::make_shared<BlockingSocket>(server.getAddress(), nullptr);
  socket->open();
  uint8_t buf[256] = {0x16, 0x03};
  memset(buf + 2, 'a', sizeof(buf) - 2);
  socket->write(buf, sizeof(buf));
  socket->close();

  handshakeCallback.waitForHandshake();
  EXPECT_NE(
      handshakeCallback.errorString_.find("SSL routines"), std::string::npos);
#if defined(OPENSSL_IS_BORINGSSL)
  EXPECT_NE(
      handshakeCallback.errorString_.find("ENCRYPTED_LENGTH_TOO_LONG"),
      std::string::npos);
#else
  EXPECT_NE(
      handshakeCallback.errorString_.find("packet length too long"),
      std::string::npos);
#endif
}

TEST(AsyncSSLSocketTest, TestSSLCipherCodeToNameMap) {
  using folly::ssl::OpenSSLUtils;
  EXPECT_EQ(
      OpenSSLUtils::getCipherName(0xc02c), "ECDHE-ECDSA-AES256-GCM-SHA384");
  // TLS_DHE_RSA_WITH_DES_CBC_SHA - We shouldn't be building with this
  EXPECT_EQ(OpenSSLUtils::getCipherName(0x0015), "");
  // This indicates TLS_EMPTY_RENEGOTIATION_INFO_SCSV, no name expected
  EXPECT_EQ(OpenSSLUtils::getCipherName(0x00ff), "");
}

#if defined __linux__
/**
 * Ensure TransparentTLS flag is disabled with AsyncSSLSocket
 */
TEST(AsyncSSLSocketTest, TTLSDisabled) {
  // clear all setsockopt tracking history
  globalStatic.reset();

  // Start listening on a local port
  WriteCallbackBase writeCallback;
  ReadCallback readCallback(&writeCallback);
  HandshakeCallback handshakeCallback(&readCallback);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback);

  // Set up SSL context.
  auto sslContext = std::make_shared<SSLContext>();

  // connect
  auto socket =
      std::make_shared<BlockingSocket>(server.getAddress(), sslContext);
  socket->open();

  EXPECT_TRUE(
      globalStatic.ttlsDisabledSet.contains(socket->getNetworkSocket()));

  // write()
  std::array<uint8_t, 128> buf;
  memset(buf.data(), 'a', buf.size());
  socket->write(buf.data(), buf.size());

  // wait until the server has reflected all data written by the client
  SemiFuture<StateEnum> future = writeCallback.getSemiFuture();
  StateEnum state = std::move(future).get(std::chrono::seconds(3));
  EXPECT_EQ(state, STATE_SUCCEEDED);

  // close()
  socket->close();
}
#endif

#if FOLLY_ALLOW_TFO

class MockAsyncTFOSSLSocket : public AsyncSSLSocket {
 public:
  using UniquePtr = std::unique_ptr<MockAsyncTFOSSLSocket, Destructor>;

  explicit MockAsyncTFOSSLSocket(
      std::shared_ptr<folly::SSLContext> sslCtx, EventBase* evb)
      : AsyncSSLSocket(sslCtx, evb) {}

  MOCK_METHOD(
      ssize_t,
      tfoSendMsg,
      (NetworkSocket fd, struct msghdr* msg, int msg_flags));
};

#if defined __linux__
/**
 * Ensure TransparentTLS flag is disabled with AsyncSSLSocket + TFO
 */
TEST(AsyncSSLSocketTest, TTLSDisabledWithTFO) {
  // clear all setsockopt tracking history
  globalStatic.reset();

  // Start listening on a local port
  WriteCallbackBase writeCallback;
  ReadCallback readCallback(&writeCallback);
  HandshakeCallback handshakeCallback(&readCallback);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback, true);

  // Set up SSL context.
  auto sslContext = std::make_shared<SSLContext>();

  // connect
  auto socket =
      std::make_shared<BlockingSocket>(server.getAddress(), sslContext);
  socket->enableTFO();
  socket->open();

  EXPECT_TRUE(
      globalStatic.ttlsDisabledSet.contains(socket->getNetworkSocket()));

  // write()
  std::array<uint8_t, 128> buf;
  memset(buf.data(), 'a', buf.size());
  socket->write(buf.data(), buf.size());

  // wait until the server has reflected all data written by the client
  SemiFuture<StateEnum> future = writeCallback.getSemiFuture();
  StateEnum state = std::move(future).get(std::chrono::seconds(3));
  EXPECT_EQ(state, STATE_SUCCEEDED);

  // close()
  socket->close();
}
#endif

/**
 * Test connecting to, writing to, reading from, and closing the
 * connection to the SSL server with TFO.
 */
TEST(AsyncSSLSocketTest, ConnectWriteReadCloseTFO) {
  // Start listening on a local port
  WriteCallbackBase writeCallback;
  ReadCallback readCallback(&writeCallback);
  HandshakeCallback handshakeCallback(&readCallback);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback, true);

  // Set up SSL context.
  auto sslContext = std::make_shared<SSLContext>();

  // connect
  auto socket =
      std::make_shared<BlockingSocket>(server.getAddress(), sslContext);
  socket->enableTFO();
  socket->open();

  // write()
  std::array<uint8_t, 128> buf;
  memset(buf.data(), 'a', buf.size());
  socket->write(buf.data(), buf.size());

  // read()
  std::array<uint8_t, 128> readbuf;
  uint32_t bytesRead = socket->readAll(readbuf.data(), readbuf.size());
  EXPECT_EQ(bytesRead, 128);
  EXPECT_EQ(memcmp(buf.data(), readbuf.data(), bytesRead), 0);

  // close()
  socket->close();
}

/**
 * Test connecting to, writing to, reading from, and closing the
 * connection to the SSL server with TFO.
 */
TEST(AsyncSSLSocketTest, ConnectWriteReadCloseTFOWithTFOServerDisabled) {
  // Start listening on a local port
  WriteCallbackBase writeCallback;
  ReadCallback readCallback(&writeCallback);
  HandshakeCallback handshakeCallback(&readCallback);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback);

  // Set up SSL context.
  auto sslContext = std::make_shared<SSLContext>();

  // connect
  auto socket =
      std::make_shared<BlockingSocket>(server.getAddress(), sslContext);
  socket->enableTFO();
  socket->open();

  // write()
  std::array<uint8_t, 128> buf;
  memset(buf.data(), 'a', buf.size());
  socket->write(buf.data(), buf.size());

  // read()
  std::array<uint8_t, 128> readbuf;
  uint32_t bytesRead = socket->readAll(readbuf.data(), readbuf.size());
  EXPECT_EQ(bytesRead, 128);
  EXPECT_EQ(memcmp(buf.data(), readbuf.data(), bytesRead), 0);

  // close()
  socket->close();
}

class ConnCallback : public AsyncSocket::ConnectCallback {
 public:
  void connectSuccess() noexcept override { state = State::SUCCESS; }

  void connectErr(const AsyncSocketException& ex) noexcept override {
    state = State::ERROR;
    error = ex.what();
  }

  enum class State { WAITING, SUCCESS, ERROR };

  State state{State::WAITING};
  std::string error;
};

template <class Cardinality>
MockAsyncTFOSSLSocket::UniquePtr setupSocketWithFallback(
    EventBase* evb, const SocketAddress& address, Cardinality cardinality) {
  // Set up SSL context.
  auto sslContext = std::make_shared<SSLContext>();

  // connect
  auto socket = MockAsyncTFOSSLSocket::UniquePtr(
      new MockAsyncTFOSSLSocket(sslContext, evb));
  socket->enableTFO();

  EXPECT_CALL(*socket, tfoSendMsg(_, _, _))
      .Times(cardinality)
      .WillOnce(Invoke([&](NetworkSocket fd, struct msghdr*, int) {
        sockaddr_storage addr;
        auto len = address.getAddress(&addr);
        return netops::connect(fd, (const struct sockaddr*)&addr, len);
      }));
  return socket;
}

TEST(AsyncSSLSocketTest, ConnectWriteReadCloseTFOFallback) {
  if (!folly::test::isTFOAvailable()) {
    GTEST_SKIP() << "TFO not supported.";
  }

  // Start listening on a local port
  WriteCallbackBase writeCallback;
  ReadCallback readCallback(&writeCallback);
  HandshakeCallback handshakeCallback(&readCallback);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback, true);

  EventBase evb;

  auto socket = setupSocketWithFallback(&evb, server.getAddress(), 1);
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);

  evb.loop();
  EXPECT_EQ(ConnCallback::State::SUCCESS, ccb.state);

  evb.runInEventBaseThread([&] { socket->detachEventBase(); });
  evb.loop();

  BlockingSocket sock(std::move(socket));
  // write()
  std::array<uint8_t, 128> buf;
  memset(buf.data(), 'a', buf.size());
  sock.write(buf.data(), buf.size());

  // read()
  std::array<uint8_t, 128> readbuf;
  uint32_t bytesRead = sock.readAll(readbuf.data(), readbuf.size());
  EXPECT_EQ(bytesRead, 128);
  EXPECT_EQ(memcmp(buf.data(), readbuf.data(), bytesRead), 0);

  // close()
  sock.close();
}

#if !defined(OPENSSL_IS_BORINGSSL)
TEST(AsyncSSLSocketTest, ConnectTFOTimeout) {
  // Start listening on a local port
  ConnectTimeoutCallback acceptCallback;
  TestSSLServer server(&acceptCallback, true);

  // Set up SSL context.
  auto sslContext = std::make_shared<SSLContext>();

  // connect
  auto socket =
      std::make_shared<BlockingSocket>(server.getAddress(), sslContext);
  socket->enableTFO();
  EXPECT_THROW(
      socket->open(std::chrono::milliseconds(20)), AsyncSocketException);
}
#endif

#if !defined(OPENSSL_IS_BORINGSSL)
TEST(AsyncSSLSocketTest, ConnectTFOFallbackTimeout) {
  // Start listening on a local port
  ConnectTimeoutCallback acceptCallback;
  TestSSLServer server(&acceptCallback, true);

  EventBase evb;

  auto socket = setupSocketWithFallback(&evb, server.getAddress(), AtMost(1));
  ConnCallback ccb;
  // Set a short timeout
  socket->connect(&ccb, server.getAddress(), 1);

  evb.loop();
  EXPECT_EQ(ConnCallback::State::ERROR, ccb.state);
}
#endif

TEST(AsyncSSLSocketTest, HandshakeTFOFallbackTimeout) {
  // Start listening on a local port
  EmptyReadCallback readCallback;
  HandshakeCallback handshakeCallback(
      &readCallback, HandshakeCallback::EXPECT_ERROR);
  HandshakeTimeoutCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback, true);

  EventBase evb;

  auto socket = setupSocketWithFallback(&evb, server.getAddress(), AtMost(1));
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 100);

  evb.loop();
  EXPECT_EQ(ConnCallback::State::ERROR, ccb.state);
  EXPECT_THAT(ccb.error, testing::HasSubstr("SSL connect timed out"));
}

TEST(AsyncSSLSocketTest, HandshakeTFORefused) {
  // Start listening on a local port
  EventBase evb;

  // Hopefully nothing is listening on this address
  SocketAddress addr("127.0.0.1", 65535);
  auto socket = setupSocketWithFallback(&evb, addr, AtMost(1));
  ConnCallback ccb;
  socket->connect(&ccb, addr, 100);

  evb.loop();
  EXPECT_EQ(ConnCallback::State::ERROR, ccb.state);
  EXPECT_THAT(ccb.error, testing::HasSubstr("refused"));
}

TEST(AsyncSSLSocketTest, TestPreReceivedData) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto dfServerCtx = std::make_shared<SSLContext>();
  std::array<NetworkSocket, 2> fds;
  getfds(fds.data());
  getctx(clientCtx, dfServerCtx);

  AsyncSSLSocket::UniquePtr clientSockPtr(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSockPtr(
      new AsyncSSLSocket(dfServerCtx, &eventBase, fds[1], true));
  auto clientSock = clientSockPtr.get();
  auto serverSock = serverSockPtr.get();
  SSLHandshakeClient client(std::move(clientSockPtr), true, true);

  // Steal some data from the server.
  std::array<uint8_t, 10> buf;
  auto bytesReceived = netops::recv(fds[1], buf.data(), buf.size(), 0);
  checkUnixError(bytesReceived, "recv failed");

  serverSock->setPreReceivedData(
      IOBuf::wrapBuffer(ByteRange(buf.data(), bytesReceived)));
  SSLHandshakeServer server(std::move(serverSockPtr), true, true);
  // The order in which the client and server handshake completion callbacks
  // fire differs between TLS 1.2 and TLS 1.3. Loop until both are complete to
  // ensure this works regardless of TLS version.
  while ((!client.handshakeSuccess_ && !client.handshakeError_) ||
         (!server.handshakeSuccess_ && !server.handshakeError_)) {
    eventBase.loopOnce();
  }

  EXPECT_TRUE(client.handshakeSuccess_);
  EXPECT_TRUE(server.handshakeSuccess_);
  EXPECT_EQ(
      serverSock->getRawBytesReceived(), clientSock->getRawBytesWritten());
}

TEST(AsyncSSLSocketTest, TestMoveFromAsyncSocket) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto dfServerCtx = std::make_shared<SSLContext>();
  std::array<NetworkSocket, 2> fds;
  getfds(fds.data());
  getctx(clientCtx, dfServerCtx);

  AsyncSSLSocket::UniquePtr clientSockPtr(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSocket::UniquePtr serverSockPtr(new AsyncSocket(&eventBase, fds[1]));
  auto clientSock = clientSockPtr.get();
  auto serverSock = serverSockPtr.get();
  SSLHandshakeClient client(std::move(clientSockPtr), true, true);

  // Steal some data from the server.
  std::array<uint8_t, 10> buf;
  auto bytesReceived = netops::recv(fds[1], buf.data(), buf.size(), 0);
  checkUnixError(bytesReceived, "recv failed");

  serverSock->setPreReceivedData(
      IOBuf::wrapBuffer(ByteRange(buf.data(), bytesReceived)));
  AsyncSSLSocket::UniquePtr serverSSLSockPtr(
      new AsyncSSLSocket(dfServerCtx, std::move(serverSockPtr), true));
  auto serverSSLSock = serverSSLSockPtr.get();
  SSLHandshakeServer server(std::move(serverSSLSockPtr), true, true);
  // The order in which the client and server handshake completion callbacks
  // fire differs between TLS 1.2 and TLS 1.3. Loop until both are complete to
  // ensure this works regardless of TLS version.
  while ((!client.handshakeSuccess_ && !client.handshakeError_) ||
         (!server.handshakeSuccess_ && !server.handshakeError_)) {
    eventBase.loopOnce();
  }

  EXPECT_TRUE(client.handshakeSuccess_);
  EXPECT_TRUE(server.handshakeSuccess_);
  EXPECT_EQ(
      serverSSLSock->getRawBytesReceived(), clientSock->getRawBytesWritten());
}

TEST(AsyncSSLSocketTest, TestNullConnectCallbackError) {
  EventBase eventBase;
  auto clientCtx = std::make_shared<SSLContext>();
  auto serverCtx = std::make_shared<SSLContext>();
  std::array<NetworkSocket, 2> fds;
  getfds(fds.data());
  getctx(clientCtx, serverCtx);
  // Mismatched ciphers
  clientCtx->setCiphersuitesOrThrow("TLS_AES_256_GCM_SHA384");
  serverCtx->setCiphersuitesOrThrow("TLS_AES_128_GCM_SHA256");

  AsyncSSLSocket::UniquePtr clientSockPtr(
      new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
  AsyncSSLSocket::UniquePtr serverSockPtr(
      new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));
  auto clientSock = clientSockPtr.get();

  clientSock->sslConn(nullptr);

  ExpectSSLWriteErrorCallback wcb;
  wcb.setSocket(std::move(clientSockPtr));
  clientSock->write(&wcb, "abc", 3);

  SSLHandshakeServer server(std::move(serverSockPtr), true, true);
  eventBase.loop();

  EXPECT_FALSE(server.handshakeSuccess_);
}

TEST(AsyncSSLSocketTest, TestSSLSetClientOptionsP256) {
  EventBase evb;
  std::array<NetworkSocket, 2> fds;
  getfds(fds.data());
  auto clientCtx = std::make_shared<SSLContext>();
  auto serverCtx = std::make_shared<SSLContext>();
  serverCtx->setSupportedGroups(std::vector<std::string>({"P-256"}));
  serverCtx->loadCertificate(find_resource(kTestCert).c_str());
  serverCtx->loadPrivateKey(find_resource(kTestKey).c_str());
  ssl::SSLCommonOptions::setClientOptions(*clientCtx);

  auto clientSocket =
      AsyncSSLSocket::newSocket(clientCtx, &evb, fds[0], false, true);
  std::shared_ptr<AsyncSSLSocket> serverSocket =
      AsyncSSLSocket::newSocket(serverCtx, &evb, fds[1], true, true);

  ReadCallbackTerminator readCallback(&evb, nullptr);
  serverSocket->setReadCB(&readCallback);
  readCallback.setSocket(serverSocket);
  serverSocket->sslAccept(nullptr);
  clientSocket->sslConn(nullptr);

  uint8_t buf[128];
  memset(buf, 'a', sizeof(buf));
  clientSocket->write(nullptr, buf, sizeof(buf));

  EventBaseAborter eba(&evb, 3000);

  evb.loop();

  auto sharedGroupName = serverSocket->getNegotiatedGroup();
  EXPECT_THAT(sharedGroupName, testing::HasSubstr("prime256v1"));
}

TEST(AsyncSSLSocketTest, TestSSLSetClientOptionsX25519) {
  EventBase evb;
  std::array<NetworkSocket, 2> fds;
  getfds(fds.data());
  auto clientCtx = std::make_shared<SSLContext>();
  auto serverCtx = std::make_shared<SSLContext>();
  serverCtx->setSupportedGroups(std::vector<std::string>({"X25519", "P-256"}));
  serverCtx->loadCertificate(find_resource(kTestCert).c_str());
  serverCtx->loadPrivateKey(find_resource(kTestKey).c_str());
  ssl::SSLCommonOptions::setClientOptions(*clientCtx);

  auto clientSocket =
      AsyncSSLSocket::newSocket(clientCtx, &evb, fds[0], false, true);
  std::shared_ptr<AsyncSSLSocket> serverSocket =
      AsyncSSLSocket::newSocket(serverCtx, &evb, fds[1], true, true);

  ReadCallbackTerminator readCallback(&evb, nullptr);
  serverSocket->setReadCB(&readCallback);
  readCallback.setSocket(serverSocket);
  serverSocket->sslAccept(nullptr);
  clientSocket->sslConn(nullptr);

  uint8_t buf[128];
  memset(buf, 'a', sizeof(buf));
  clientSocket->write(nullptr, buf, sizeof(buf));

  EventBaseAborter eba(&evb, 3000);

  evb.loop();

  auto sharedGroupName = serverSocket->getNegotiatedGroup();
  EXPECT_THAT(sharedGroupName, testing::HasSubstr("X25519"));
}

/**
 * Test overriding the flags passed to "sendmsg()" system call,
 * and verifying that write requests fail properly.
 */
TEST(AsyncSSLSocketTest, SendMsgParamsCallback) {
  // Start listening on a local port
  SendMsgFlagsCallback msgCallback;
  ExpectWriteErrorCallback writeCallback(&msgCallback);
  ReadCallback readCallback(&writeCallback);
  HandshakeCallback handshakeCallback(&readCallback);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback);

  // Set up SSL context.
  auto sslContext = std::make_shared<SSLContext>();
  sslContext->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");

  // connect
  auto socket =
      std::make_shared<BlockingSocket>(server.getAddress(), sslContext);
  socket->open();

  // Setting flags to "-1" to trigger an error on attempt to use these
  // flags in the sendmsg() system call. The exact errno is
  // kernel-dependent (EINVAL or EISCONN); see ExpectWriteErrorCallback.
  msgCallback.resetFlags(-1);

  // write()
  std::vector<uint8_t> buf(128, 'a');
  ASSERT_EQ(socket->write(buf.data(), buf.size()), buf.size());

  // close()
  socket->close();

  cerr << "SendMsgParamsCallback test completed" << endl;
}

#if FOLLY_HAVE_SO_TIMESTAMPING

class AsyncSSLSocketByteEventTest : public ::testing::Test {
 protected:
  using MockDispatcher = ::testing::NiceMock<netops::test::MockDispatcher>;
  using TestObserver =
      test::MockAsyncSocketLegacyLifecycleObserverForByteEvents;
  using ByteEventType = AsyncSocket::ByteEvent::Type;

  /**
   * Components of a client connection to TestServer.
   *
   * Includes EventBase, client's AsyncSocket.
   */
  class ClientConn {
   public:
    explicit ClientConn(
        std::shared_ptr<TestSSLServer> server,
        std::shared_ptr<AsyncSSLSocket> socket = nullptr)
        : server_(std::move(server)), socket_(std::move(socket)) {
      if (!socket_) {
        socket_ = AsyncSSLSocket::newSocket(getSslContext(), &getEventBase());
      }
      socket_->setOverrideNetOpsDispatcher(netOpsDispatcher_);
      netOpsDispatcher_->forwardToDefaultImpl();
    }

    ~ClientConn() {
      if (socket_) {
        socket_->close();
      }
    }

    void connect() {
      CHECK_NOTNULL(socket_.get());
      CHECK_NOTNULL(socket_->getEventBase());
      socket_->connect(&connCb_, server_->getAddress(), 30);
      socket_->getEventBase()->loop();
      ASSERT_EQ(connCb_.state, ConnCallback::State::SUCCESS);
      setReadCb();
    }

    void waitForHandshake() {
      while (connCb_.state == ConnCallback::State::WAITING) {
        socket_->getEventBase()->loopOnce();
      }
    }

    void setReadCb() {
      // Due to how libevent works, we currently need to be subscribed to
      // EV_READ events in order to get error messages.
      //
      // TODO(bschlinker): Resolve this with libevent modification.
      // See https://github.com/libevent/libevent/issues/1038 for details.
      socket_->setReadCB(&readCb_);
      readCb_.setSocket(socket_);
    }

    std::shared_ptr<NiceMock<TestObserver>> attachObserver(
        bool enableByteEvents) {
      auto observer = AsyncSSLSocketByteEventTest::attachObserver(
          socket_.get(), enableByteEvents);
      observers.push_back(observer);
      return observer;
    }

    /**
     * Write to client socket, echo at server, and wait for echo at client.
     *
     * Waiting for echo at client ensures that we have given opportunity for
     * timestamps to be generated by the kernel.
     */
    void writeAndReflect(
        const std::vector<uint8_t>& wbuf, const WriteFlags writeFlags) {
      CHECK_NOTNULL(socket_.get());
      CHECK_NOTNULL(socket_->getEventBase());

      // write to the client socket
      WriteCallbackBase wcb;
      socket_->write(&wcb, wbuf.data(), wbuf.size(), writeFlags);
      while (wcb.state == STATE_WAITING) {
        socket_->getEventBase()->loopOnce();
      }
      ASSERT_EQ(wcb.state, STATE_SUCCEEDED);

      // TestSSLServer reads and reflects for us

      // read reflection at client
      while (wbuf.size() != readCb_.dataRead()) {
        socket_->getEventBase()->loopOnce();
      }
      readCb_.verifyData(wbuf.data(), wbuf.size());
      readCb_.clearData();
    }

    std::shared_ptr<AsyncSSLSocket> getRawSocket() { return socket_; }

    std::shared_ptr<SSLContext> getSslContext() {
      static std::shared_ptr<SSLContext> sslContext = initSslContext();
      return sslContext;
    }

    EventBase& getEventBase() {
      static EventBase evb; // use same EventBase for all client sockets
      return evb;
    }

    void netOpsExpectTimestampingSetSockOpt() {
      // must whitelist other calls
      EXPECT_CALL(*netOpsDispatcher_, setsockopt(_, _, _, _, _))
          .Times(AnyNumber());
      EXPECT_CALL(
          *netOpsDispatcher_, setsockopt(_, SOL_SOCKET, SO_TIMESTAMPING, _, _))
          .Times(1);
    }

    void netOpsExpectNoTimestampingSetSockOpt() {
      // must whitelist other calls
      EXPECT_CALL(*netOpsDispatcher_, setsockopt(_, _, _, _, _))
          .Times(AnyNumber());
      EXPECT_CALL(
          *netOpsDispatcher_, setsockopt(_, SOL_SOCKET, SO_TIMESTAMPING, _, _))
          .Times(0);
    }

    void netOpsExpectWriteWithFlags(WriteFlags writeFlags) {
      EXPECT_CALL(*netOpsDispatcher_, sendmsg(_, _, _))
          .WillOnce(Invoke(
              [this, writeFlags](
                  NetworkSocket socket, const msghdr* message, int flags) {
                EXPECT_EQ(writeFlags, getMsgWriteFlags(*message));
                return netOpsDispatcher_->netops::Dispatcher::sendmsg(
                    socket, message, flags);
              }));
    }

    void netOpsVerifyAndClearExpectations() {
      Mock::VerifyAndClearExpectations(netOpsDispatcher_.get());
    }

    /**
     * Static utilities.
     */
    static std::shared_ptr<SSLContext> initSslContext() {
      auto sslContext = std::make_shared<SSLContext>();
      sslContext->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
      return sslContext;
    }

   private:
    // server
    std::shared_ptr<TestSSLServer> server_;

    // managed observers
    std::vector<std::shared_ptr<TestObserver>> observers;

    // socket components
    ConnCallback connCb_;
    ReadCallback readCb_;
    std::shared_ptr<MockDispatcher> netOpsDispatcher_{
        std::make_shared<MockDispatcher>()};
    std::shared_ptr<AsyncSSLSocket> socket_;
  };

  ClientConn getClientConn() { return ClientConn(server_); }

  void SetUp() override {
    serverWriteCb_ = std::make_unique<WriteCallbackBase>();
    serverReadCb_ = std::make_unique<ReadCallback>(serverWriteCb_.get());
    serverHandshakeCb_ =
        std::make_unique<HandshakeCallback>(serverReadCb_.get());
    serverAcceptCb_ =
        std::make_unique<SSLServerAcceptCallback>(serverHandshakeCb_.get());
    server_ = std::make_shared<TestSSLServer>(serverAcceptCb_.get());
  }

  /**
   * Static utility functions.
   */

  static std::shared_ptr<NiceMock<TestObserver>> attachObserver(
      AsyncSocket* socket, bool enableByteEvents) {
    AsyncSocket::LegacyLifecycleObserver::Config config = {};
    config.byteEvents = enableByteEvents;
    return std::make_shared<NiceMock<TestObserver>>(socket, config);
  }

  static WriteFlags getMsgWriteFlags(const struct msghdr& msg) {
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

  static WriteFlags dropWriteFromFlags(WriteFlags writeFlags) {
    return writeFlags & ~WriteFlags::TIMESTAMP_WRITE;
  }

  // server components
  std::unique_ptr<WriteCallbackBase> serverWriteCb_;
  std::unique_ptr<ReadCallback> serverReadCb_;
  std::unique_ptr<HandshakeCallback> serverHandshakeCb_;
  std::unique_ptr<SSLServerAcceptCallback> serverAcceptCb_;
  std::shared_ptr<TestSSLServer> server_;
};

TEST_F(AsyncSSLSocketByteEventTest, ObserverAttachedBeforeConnect) {
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

  {
    clientConn.netOpsExpectWriteWithFlags(dropWriteFromFlags(flags));
    clientConn.writeAndReflect(wbuf, flags);
    clientConn.netOpsVerifyAndClearExpectations();

    // may have more than four new ByteEvents if write split further by kernel
    EXPECT_THAT(observer->byteEvents, SizeIs(Ge(4)));

    // due to SSL overhead, offset will not be 0
    auto offsetExpected = clientConn.getRawSocket()->getRawBytesWritten() - 1;
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::TX));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::ACK));
  }

  // write again to check offsets
  {
    const auto startNumByteEvents = observer->byteEvents.size();
    clientConn.netOpsExpectWriteWithFlags(dropWriteFromFlags(flags));
    clientConn.writeAndReflect(wbuf, flags);
    clientConn.netOpsVerifyAndClearExpectations();

    // may have more than four new ByteEvents if write split further by kernel
    EXPECT_THAT(observer->byteEvents, SizeIs(Ge(startNumByteEvents + 4)));

    // due to SSL overhead, offset will not be 1
    auto offsetExpected = clientConn.getRawSocket()->getRawBytesWritten() - 1;
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::TX));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::ACK));
  }
}

TEST_F(AsyncSSLSocketByteEventTest, ObserverAttachedAfterConnect) {
  const auto flags = WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
      WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK;
  const std::vector<uint8_t> wbuf(1, 'a');

  auto clientConn = getClientConn();
  clientConn.netOpsExpectNoTimestampingSetSockOpt();
  clientConn.connect();
  clientConn.netOpsVerifyAndClearExpectations();

  // We make sure the server writes at least one byte to the client before
  // enabling byte events. Otherwise, the test is flaky. We believe this happens
  // when the following sequence occurs:
  //
  // (1) The client socket enables byte events successfully;
  //
  // (2) The client socket writes data to the server with timestamping set;
  //
  // (3) The client socket receives byte events for WRITE, SCHED, and TX and
  //     processes them using `handleErrMessages` in `ioReady`. Once the error
  //     queue is empty, `ioReady` calls `handleRead`. During this time, the
  //     client also begins to read data sent by the server as part of the SSL
  //     handshake completion;
  //
  // (4) The server socket receives the data from the client and reflects it
  //     back;
  //
  // (5) The client socket receives the data reflected by the server. When a
  //     data packet contains an ACK for previous data, the kernel emits the ACK
  //     timestamp before handing off the received data to userspace. However,
  //     in this case, since the client socket is still inside the `handleRead`
  //     function (step 3), it will process the read, even though the ACK
  //     timestamp is already enqueued in the error queue. When it finishes
  //     reading the data, it does not go back to handle messages from the error
  //     queue until the next call to `ioReady`. This causes several `EXPECT`
  //     statements below to fail.

  serverHandshakeCb_->waitForHandshake();
  clientConn.waitForHandshake();
  clientConn.writeAndReflect(wbuf, flags);
  clientConn.netOpsVerifyAndClearExpectations();

  clientConn.netOpsExpectTimestampingSetSockOpt();
  auto observer = clientConn.attachObserver(true);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());
  clientConn.netOpsVerifyAndClearExpectations();

  {
    clientConn.netOpsExpectWriteWithFlags(dropWriteFromFlags(flags));
    clientConn.writeAndReflect(wbuf, flags);
    clientConn.netOpsVerifyAndClearExpectations();

    // may have more than four new ByteEvents if write split further by kernel
    EXPECT_THAT(observer->byteEvents, SizeIs(Ge(4)));

    // due to SSL overhead, offset will not be 0
    auto offsetExpected = clientConn.getRawSocket()->getRawBytesWritten() - 1;
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::TX));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::ACK));
  }

  // write again to check offsets
  {
    const auto startNumByteEvents = observer->byteEvents.size();
    clientConn.netOpsExpectWriteWithFlags(dropWriteFromFlags(flags));
    clientConn.writeAndReflect(wbuf, flags);
    clientConn.netOpsVerifyAndClearExpectations();

    // may have more than four new ByteEvents if write split further by kernel
    EXPECT_THAT(observer->byteEvents, SizeIs(Ge(startNumByteEvents + 4)));

    // due to SSL overhead, offset will not be 1
    auto offsetExpected = clientConn.getRawSocket()->getRawBytesWritten() - 1;
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::TX));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::ACK));
  }
}

TEST_F(AsyncSSLSocketByteEventTest, MultiByteWrites) {
  const auto flags = WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
      WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK;

  auto clientConn = getClientConn();
  auto observer = clientConn.attachObserver(true /* enableByteEvents */);
  clientConn.netOpsExpectTimestampingSetSockOpt();
  clientConn.connect();
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());
  clientConn.netOpsVerifyAndClearExpectations();

  // write 20 bytes
  {
    std::vector<uint8_t> wbuf(20, 'a'); // 20 bytes

    clientConn.netOpsExpectWriteWithFlags(dropWriteFromFlags(flags));
    clientConn.writeAndReflect(wbuf, flags);
    clientConn.netOpsVerifyAndClearExpectations();

    // may have more than four new ByteEvents if write split further by kernel
    EXPECT_THAT(observer->byteEvents, SizeIs(Ge(4)));

    // due to SSL overhead, offset will not be 0
    auto offsetExpected = clientConn.getRawSocket()->getRawBytesWritten() - 1;
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::TX));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::ACK));
  }

  // write 40 bytes
  {
    std::vector<uint8_t> wbuf(20, 'a'); // 20 bytes

    const auto startNumByteEvents = observer->byteEvents.size();
    clientConn.netOpsExpectWriteWithFlags(dropWriteFromFlags(flags));
    clientConn.writeAndReflect(wbuf, flags);
    clientConn.netOpsVerifyAndClearExpectations();

    // may have more than four new ByteEvents if write split further by kernel
    EXPECT_THAT(observer->byteEvents, SizeIs(Ge(startNumByteEvents + 4)));

    // due to SSL overhead, offset will not be 1
    auto offsetExpected = clientConn.getRawSocket()->getRawBytesWritten() - 1;
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::TX));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::ACK));
  }
}

TEST_F(AsyncSSLSocketByteEventTest, MultiByteWritesEnableSecondWrite) {
  const auto flags = WriteFlags::TIMESTAMP_WRITE | WriteFlags::TIMESTAMP_SCHED |
      WriteFlags::TIMESTAMP_TX | WriteFlags::TIMESTAMP_ACK;

  auto clientConn = getClientConn();
  clientConn.netOpsExpectNoTimestampingSetSockOpt();
  clientConn.connect();
  clientConn.netOpsVerifyAndClearExpectations();

  // write 20 bytes with no ByteEvents / observer
  {
    std::vector<uint8_t> wbuf(20, 'a'); // 20 bytes
    clientConn.netOpsExpectWriteWithFlags(WriteFlags::NONE);
    clientConn.writeAndReflect(wbuf, flags);
    clientConn.netOpsVerifyAndClearExpectations();
  }

  // enable observer
  clientConn.netOpsExpectTimestampingSetSockOpt();
  auto observer = clientConn.attachObserver(true /* enableByteEvents */);
  EXPECT_EQ(1, observer->byteEventsEnabledCalled);
  EXPECT_EQ(0, observer->byteEventsUnavailableCalled);
  EXPECT_FALSE(observer->byteEventsUnavailableCalledEx.has_value());
  clientConn.netOpsVerifyAndClearExpectations();

  // write 40 bytes
  {
    std::vector<uint8_t> wbuf(20, 'a'); // 20 bytes

    const auto startNumByteEvents = observer->byteEvents.size();
    clientConn.netOpsExpectWriteWithFlags(dropWriteFromFlags(flags));
    clientConn.writeAndReflect(wbuf, flags);
    clientConn.netOpsVerifyAndClearExpectations();

    // may have more than four new ByteEvents if write split further by kernel
    EXPECT_THAT(observer->byteEvents, SizeIs(Ge(startNumByteEvents + 4)));

    // due to SSL overhead, offset will not be 1
    auto offsetExpected = clientConn.getRawSocket()->getRawBytesWritten() - 1;
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::WRITE));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::SCHED));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::TX));
    EXPECT_EQ(
        offsetExpected,
        observer->maxOffsetForByteEventReceived(ByteEventType::ACK));
  }
}

#endif // FOLLY_HAVE_SO_TIMESTAMPING

#endif // __linux__

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
  folly::ssl::SSLSessionUniquePtr session_;
  // set after object construction
  folly::AsyncSSLSocket* socket_;
};

TEST(AsyncSSLSocketTest, TestSNIClientHelloBehavior) {
  EventBase eventBase;
  auto serverCtx = std::make_shared<SSLContext>();
  auto clientCtx = std::make_shared<SSLContext>();
  serverCtx->loadPrivateKey(find_resource(kTestKey).c_str());
  serverCtx->loadCertificate(find_resource(kTestCert).c_str());

  auto sessionCb = std::make_unique<SimpleSessionLifecycleCallback>();
  auto sessionCbPtr = sessionCb.get();
  clientCtx->setSessionLifecycleCallbacks(std::move(sessionCb));
  clientCtx->setSessionCacheContext("test context");
  clientCtx->setVerificationOption(SSLContext::SSLVerifyPeerEnum::NO_VERIFY);
  folly::ssl::SSLSessionUniquePtr resumptionSession = nullptr;

  {
    std::array<NetworkSocket, 2> fds;
    getfds(fds.data());

    AsyncSSLSocket::UniquePtr clientSock(
        new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
    AsyncSSLSocket::UniquePtr serverSock(
        new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));

    // Client sends SNI that doesn't match anything the server cert advertises
    clientSock->setServerName("Foobar");
    auto clientSockPtr = clientSock.get();
    SSLHandshakeServerParseClientHello server(
        std::move(serverSock), true, true);
    // capture client socket in session callback, so we can detach event base
    sessionCbPtr->socket_ = clientSockPtr;
    SSLHandshakeClient client(std::move(clientSock), true, true);

    // should stop when the session ticket is received
    ReadCallback readCallback;
    clientSockPtr->setReadCB(&readCallback);
    readCallback.state = STATE_SUCCEEDED;
    // loop until we receive session (and unregister read callback)
    eventBase.loop();
    resumptionSession = std::move(sessionCbPtr->session_);

    serverSock = std::move(server).moveSocket();
    auto chi = serverSock->getClientHelloInfo();
    ASSERT_NE(chi, nullptr);
    EXPECT_EQ(
        std::string("Foobar"), std::string(serverSock->getSSLServerName()));

    // create another client, resuming with the prior session, but under a
    // different common name.
    clientSock = std::move(client).moveSocket();
    ASSERT_TRUE(resumptionSession != nullptr);
  }

  {
    std::array<NetworkSocket, 2> fds;
    getfds(fds.data());

    AsyncSSLSocket::UniquePtr clientSock(
        new AsyncSSLSocket(clientCtx, &eventBase, fds[0], false));
    AsyncSSLSocket::UniquePtr serverSock(
        new AsyncSSLSocket(serverCtx, &eventBase, fds[1], true));

    clientSock->setRawSSLSession(std::move(resumptionSession));
    clientSock->setServerName("Baz");
    SSLHandshakeServerParseClientHello server(
        std::move(serverSock), true, true);
    SSLHandshakeClient client(std::move(clientSock), true, true);
    eventBase.loop();

    serverSock = std::move(server).moveSocket();
    clientSock = std::move(client).moveSocket();
    EXPECT_TRUE(clientSock->getSSLSessionReused());

    // OpenSSL 1.1.1 changes the semantics of SSL_get_servername
    // in
    // https://github.com/openssl/openssl/commit/1c4aa31d79821dee9be98e915159d52cc30d8403
    //
    // Previously, the SNI would be taken from the ClientHello.
    // Now, the SNI will be taken from the established session.
    //
    // But the session that was established with the client (prior handshake)
    // would not have set the server name field because the SNI that the client
    // requested ("Foobar") did not match any of the SANs that the server was
    // presenting ("127.0.0.1")
    //
    // To preserve this 1.1.0 behavior, getSSLServerName() should return the
    // parsed ClientHello servername. This test asserts this behavior.
    auto sni = serverSock->getSSLServerName();
    ASSERT_NE(sni, nullptr);

    std::string sniStr(sni);
    EXPECT_EQ(sniStr, std::string("Baz"));
  }
}

TEST(AsyncSSLSocketTest, BytesWrittenWithMove) {
  WriteCallbackBase writeCallback;
  ReadCallback readCallback(&writeCallback);
  HandshakeCallback handshakeCallback(&readCallback);
  SSLServerAcceptCallback acceptCallback(&handshakeCallback);
  TestSSLServer server(&acceptCallback);

  auto sslContext = std::make_shared<SSLContext>();
  sslContext->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  auto socket1 =
      std::make_shared<BlockingSocket>(server.getAddress(), sslContext);
  socket1->open(std::chrono::milliseconds(10000));

  // write
  std::vector<uint8_t> wbuf(128, 'a');
  socket1->write(wbuf.data(), wbuf.size());
  const auto socket1AppBytes = socket1->getSocket()->getAppBytesWritten();
  const auto socket1RawBytes = socket1->getSocket()->getRawBytesWritten();
  EXPECT_EQ(128, socket1AppBytes);
  EXPECT_LT(128, socket1RawBytes);

  // read reflection
  std::vector<uint8_t> readbuf(wbuf.size());
  uint32_t bytesRead = socket1->readAll(readbuf.data(), readbuf.size());
  EXPECT_EQ(bytesRead, wbuf.size());

  // additional sanity checks on virtuals
  EXPECT_EQ(
      socket1->getSSLSocket()->getRawBytesWritten(),
      socket1->getSocket()->getRawBytesWritten());
  EXPECT_EQ(128, socket1->getSocket()->getAppBytesWritten());
  EXPECT_EQ(128, socket1->getSSLSocket()->getAppBytesWritten());

  // move to another AsyncSSLSocket
  AsyncSSLSocket::UniquePtr socket2(
      new AsyncSSLSocket(sslContext, socket1->getSocket()));
  EXPECT_EQ(socket1AppBytes, socket2->getAppBytesWritten());
  EXPECT_EQ(socket1RawBytes, socket2->getRawBytesWritten());

  // move to an AsyncSocket
  AsyncSocket::UniquePtr socket3(new AsyncSocket(std::move(socket2)));
  EXPECT_EQ(socket1AppBytes, socket3->getAppBytesWritten());
  EXPECT_EQ(socket1RawBytes, socket3->getRawBytesWritten());
}

#ifdef SIGPIPE
///////////////////////////////////////////////////////////////////////////
// init_unit_test_suite
///////////////////////////////////////////////////////////////////////////
namespace {
struct Initializer {
  Initializer() { signal(SIGPIPE, SIG_IGN); }
};
Initializer initializer;
} // namespace
#endif
