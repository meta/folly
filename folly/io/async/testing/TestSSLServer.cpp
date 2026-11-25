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

#include <folly/io/async/testing/TestSSLServer.h>

#include <folly/portability/OpenSSL.h>
#include <folly/testing/TestUtil.h>

namespace folly::test {

#if !defined(FOLLY_CERTS_DIR)
#define FOLLY_CERTS_DIR "folly/io/async/test/certs"
#endif

const char* kTestCert = FOLLY_CERTS_DIR "/tests-cert.pem";
const char* kTestKey = FOLLY_CERTS_DIR "/tests-key.pem";
const char* kTestCA = FOLLY_CERTS_DIR "/ca-cert.pem";
const char* kTestCertCN = "Asox Company";

const char* kClientTestCert = FOLLY_CERTS_DIR "/client_cert.pem";
const char* kClientTestKey = FOLLY_CERTS_DIR "/client_key.pem";
const char* kClientTestCA = FOLLY_CERTS_DIR "/client_ca_cert.pem";
const char* kClientTestChain = FOLLY_CERTS_DIR "/client_chain.pem";

TestSSLServer::~TestSSLServer() {
  if (thread_.joinable()) {
    evb_.runInEventBaseThread([&]() { socket_->stopAccepting(); });
    LOG(INFO) << "Waiting for server thread to exit";
    thread_.join();
  }
}

/* static */ std::unique_ptr<SSLContext> TestSSLServer::getDefaultSSLContext() {
  // Set up a default SSL context
  std::unique_ptr<SSLContext> sslContext = std::make_unique<SSLContext>();
  sslContext->loadCertificate(find_resource(kTestCert).string().c_str());
  sslContext->loadPrivateKey(find_resource(kTestKey).string().c_str());
  sslContext->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  // By default, SSLContext disables OpenSSL's internal session cache.
  // Enable it here on the server for testing session reuse.
  SSL_CTX_set_session_cache_mode(
      sslContext->getSSLCtx(), SSL_SESS_CACHE_SERVER);

  return sslContext;
}

TestSSLServer::TestSSLServer(SSLServerAcceptCallbackBase* acb, bool enableTFO)
    : acb_(acb) {
  ctx_ = getDefaultSSLContext();
  init(enableTFO);
}

void TestSSLServer::loadTestCerts() {
  ctx_->loadCertificate(find_resource(kTestCert).string().c_str());
  ctx_->loadPrivateKey(find_resource(kTestKey).string().c_str());
}

TestSSLServer::TestSSLServer(
    SSLServerAcceptCallbackBase* acb,
    std::shared_ptr<SSLContext> ctx,
    bool enableTFO)
    : ctx_(ctx), acb_(acb) {
  init(enableTFO);
}

void TestSSLServer::init(bool enableTFO) {
  socket_ = AsyncServerSocket::newSocket(&evb_);

  acb_->ctx_ = ctx_;
  acb_->base_ = &evb_;

  // Enable TFO
  if (enableTFO) {
    LOG(INFO) << "server TFO enabled";
    socket_->setTFOEnabled(true, 1000);
  }

  // set up the listening socket
  socket_->bind(0);
  socket_->getAddress(&address_);
  socket_->listen(100);
  socket_->addAcceptCallback(acb_, &evb_);
  socket_->startAccepting();

  thread_ = std::thread([&] {
    evb_.loop();
    acb_->detach();
    LOG(INFO) << "Server thread exited event loop";
  });
  LOG(INFO) << "Accepting connections on " << address_;
}

} // namespace folly::test
