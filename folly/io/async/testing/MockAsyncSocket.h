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

#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/EventBase.h>
#include <folly/portability/GMock.h>

namespace folly {

namespace test {

class MockAsyncSocket : public AsyncSocket {
 public:
  using UniquePtr = std::unique_ptr<MockAsyncSocket, Destructor>;

  explicit MockAsyncSocket(EventBase* base) : AsyncSocket(base) {}

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

  MOCK_METHOD(void, getPeerAddress, (folly::SocketAddress*), (const));
  MOCK_METHOD(NetworkSocket, detachNetworkSocket, ());
  MOCK_METHOD(NetworkSocket, getNetworkSocket, (), (const));
  MOCK_METHOD(void, closeNow, ());
  MOCK_METHOD(bool, good, (), (const));
  MOCK_METHOD(bool, readable, (), (const));
  MOCK_METHOD(bool, writable, (), (const));
  MOCK_METHOD(bool, hangup, (), (const));
  MOCK_METHOD(void, getLocalAddress, (SocketAddress*), (const));
  MOCK_METHOD(void, setReadCB, (ReadCallback*));
  MOCK_METHOD(void, _setPreReceivedData, (std::unique_ptr<IOBuf>&));
  MOCK_METHOD(size_t, getRawBytesWritten, (), (const));
  MOCK_METHOD(int, setSockOptVirtual, (int, int, void const*, socklen_t));
  MOCK_METHOD(void, setErrMessageCB, (AsyncSocket::ErrMessageCallback*));
  MOCK_METHOD(void, setSendMsgParamCB, (AsyncSocket::SendMsgParamsCallback*));
  MOCK_METHOD(std::string, getSecurityProtocol, (), (const));
  void setPreReceivedData(std::unique_ptr<IOBuf> data) override {
    return _setPreReceivedData(data);
  }

  MOCK_METHOD(
      void,
      addLifecycleObserver,
      (folly::AsyncSocket::LegacyLifecycleObserver * observer));
  MOCK_METHOD(
      bool,
      removeLifecycleObserver,
      (folly::AsyncSocket::LegacyLifecycleObserver * observer));
  MOCK_METHOD(
      std::vector<AsyncSocket::LegacyLifecycleObserver*>,
      getLifecycleObservers,
      (),
      (const));
};

} // namespace test
} // namespace folly
