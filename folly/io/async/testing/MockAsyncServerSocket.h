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

#include <folly/io/async/AsyncServerSocket.h>
#include <folly/io/async/EventBase.h>
#include <folly/portability/GMock.h>

namespace folly {

namespace test {

class MockAsyncServerSocket : public AsyncServerSocket {
 public:
  using UniquePtr = std::unique_ptr<MockAsyncServerSocket, Destructor>;

  // We explicitly do not mock destroy(), since the base class implementation
  // in DelayedDestruction is what actually deletes the object.
  // MOCK_METHOD(void, destroy, ());
  MOCK_METHOD(void, bind, (const folly::SocketAddress& address));
  MOCK_METHOD(
      void,
      bind,
      (const std::vector<folly::IPAddress>& ipAddresses, uint16_t port));
  MOCK_METHOD(void, bind, (uint16_t port));
  MOCK_METHOD(void, listen, (int backlog));
  MOCK_METHOD(void, startAccepting, ());
  MOCK_METHOD(
      void,
      addAcceptCallback,
      (AcceptCallback * callback, EventBase* eventBase, uint32_t maxAtOnce));
};

} // namespace test
} // namespace folly
