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

#pragma once

#include <folly/io/async/AsyncSSLSocket.h>
#include <folly/portability/GMock.h>

namespace folly {
namespace test {

class MockAsyncSSLSocket : public AsyncSSLSocket {
 public:
  MockAsyncSSLSocket(
      const std::shared_ptr<SSLContext>& ctx,
      EventBase* base,
      bool deferSecurityNegotiation = false)
      : AsyncSSLSocket(ctx, base, deferSecurityNegotiation) {}

  MOCK_METHOD(
      void,
      connect_,
      (AsyncSocket::ConnectCallback*,
       const folly::SocketAddress&,
       int,
       const folly::SocketOptionMap&,
       const BindOptions&,
       const std::string&));
  void connect(
      AsyncSocket::ConnectCallback* callback,
      const folly::SocketAddress& address,
      int timeout,
      const folly::SocketOptionMap& options,
      const BindOptions& bindOptions,
      const std::string& ifName) noexcept override {
    connect_(callback, address, timeout, options, bindOptions, ifName);
  }

  MOCK_METHOD(void, getLocalAddress, (folly::SocketAddress*), (const));
  MOCK_METHOD(void, getPeerAddress, (folly::SocketAddress*), (const));
  MOCK_METHOD(void, closeNow, ());
  MOCK_METHOD(bool, good, (), (const));
  MOCK_METHOD(bool, readable, (), (const));
  MOCK_METHOD(bool, hangup, (), (const));
  MOCK_METHOD(
      void,
      getSelectedNextProtocol,
      (const unsigned char**, unsigned*),
      (const));
  MOCK_METHOD(
      bool,
      getSelectedNextProtocolNoThrow,
      (const unsigned char**, unsigned*),
      (const));
  MOCK_METHOD(void, setReadCB, (ReadCallback*));

  void sslConn(
      AsyncSSLSocket::HandshakeCB* cb,
      std::chrono::milliseconds timeout,
      const SSLContext::SSLVerifyPeerEnum& verify) override {
    if (timeout > std::chrono::milliseconds::zero()) {
      handshakeTimeout_.scheduleTimeout(timeout);
    }

    state_ = StateEnum::ESTABLISHED;
    sslState_ = STATE_CONNECTING;
    handshakeCallback_ = cb;

    sslConnectMockable(cb, timeout, verify);
  }

  void sslAccept(
      AsyncSSLSocket::HandshakeCB* cb,
      std::chrono::milliseconds timeout,
      const SSLContext::SSLVerifyPeerEnum& verify) override {
    if (timeout > std::chrono::milliseconds::zero()) {
      handshakeTimeout_.scheduleTimeout(timeout);
    }

    state_ = StateEnum::ESTABLISHED;
    sslState_ = STATE_ACCEPTING;
    handshakeCallback_ = cb;

    sslAcceptMockable(cb, timeout, verify);
  }

  MOCK_METHOD(
      void,
      sslConnectMockable,
      (AsyncSSLSocket::HandshakeCB*,
       std::chrono::milliseconds,
       const SSLContext::SSLVerifyPeerEnum&));

  MOCK_METHOD(
      void,
      sslAcceptMockable,
      (AsyncSSLSocket::HandshakeCB*,
       std::chrono::milliseconds,
       const SSLContext::SSLVerifyPeerEnum&));
};

} // namespace test
} // namespace folly
