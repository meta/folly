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
#include <folly/io/async/AsyncSocketException.h>
#include <folly/portability/GMock.h>

namespace folly {
namespace test {

class MockAsyncSocketObserver : public AsyncSocket::ManagedObserver {
 public:
  using AsyncSocket::ManagedObserver::ManagedObserver;
  MOCK_METHOD((void), destroyed, (AsyncSocket*, DestroyContext*), (noexcept));
  MOCK_METHOD(
      (void), moved, (AsyncSocket*, AsyncSocket*, MoveContext*), (noexcept));
  MOCK_METHOD((void), close, (AsyncSocket*), (noexcept));
  MOCK_METHOD((void), evbAttach, (AsyncSocket*, EventBase*), (noexcept));
  MOCK_METHOD((void), evbDetach, (AsyncSocket*, EventBase*), (noexcept));
  MOCK_METHOD((void), connectAttempt, (AsyncSocket*), (noexcept));
  MOCK_METHOD((void), connectSuccess, (AsyncSocket*), (noexcept));
  MOCK_METHOD(
      (void),
      connectError,
      (AsyncSocket*, const AsyncSocketException&),
      (noexcept));
  MOCK_METHOD((void), fdAttach, (AsyncSocket*), (noexcept));
  MOCK_METHOD((void), fdDetach, (AsyncSocket*), (noexcept));
};

/*
 * Mock class for AsyncSocketLifecycleObserver.
 */
class MockAsyncSocketLifecycleObserver
    : public AsyncSocket::LegacyLifecycleObserver {
 public:
  using AsyncSocket::LegacyLifecycleObserver::LegacyLifecycleObserver;
  MOCK_METHOD(void, observerAttachMock, (AsyncSocket*));
  MOCK_METHOD(void, observerDetachMock, (AsyncSocket*));
  MOCK_METHOD(void, destroyMock, (AsyncSocket*));
  MOCK_METHOD(void, closeMock, (AsyncSocket*));
  MOCK_METHOD(void, connectAttemptMock, (AsyncSocket*));
  MOCK_METHOD(void, connectSuccessMock, (AsyncSocket*));
  MOCK_METHOD(
      void, connectErrorMock, (AsyncSocket*, const AsyncSocketException&));
  MOCK_METHOD(void, evbAttachMock, (AsyncSocket*, EventBase*));
  MOCK_METHOD(void, evbDetachMock, (AsyncSocket*, EventBase*));
  MOCK_METHOD(
      void, byteEventMock, (AsyncSocket*, const AsyncSocket::ByteEvent&));
  MOCK_METHOD(void, byteEventsEnabledMock, (AsyncSocket*));
  MOCK_METHOD(
      void,
      byteEventsUnavailableMock,
      (AsyncSocket*, const AsyncSocketException&));

  // additional handlers specific to AsyncSocket::LegacyLifecycleObserver
  MOCK_METHOD(void, fdDetachMock, (AsyncSocket*));
  MOCK_METHOD(void, fdAttachMock, (AsyncSocket*));
  MOCK_METHOD(void, moveMock, (AsyncSocket*, AsyncSocket*));

 private:
  void observerAttach(AsyncSocket* socket) noexcept override {
    observerAttachMock(socket);
  }
  void observerDetach(AsyncSocket* socket) noexcept override {
    observerDetachMock(socket);
  }
  void destroy(AsyncSocket* socket) noexcept override { destroyMock(socket); }
  void close(AsyncSocket* socket) noexcept override { closeMock(socket); }
  void connectAttempt(AsyncSocket* socket) noexcept override {
    connectAttemptMock(socket);
  }
  void connectSuccess(AsyncSocket* socket) noexcept override {
    connectSuccessMock(socket);
  }
  void connectError(
      AsyncSocket* socket, const AsyncSocketException& ex) noexcept override {
    connectErrorMock(socket, ex);
  }
  void evbAttach(AsyncSocket* socket, EventBase* eb) noexcept override {
    evbAttachMock(socket, eb);
  }
  void evbDetach(AsyncSocket* socket, EventBase* eb) noexcept override {
    evbDetachMock(socket, eb);
  }
  void byteEvent(
      AsyncSocket* socket, const AsyncSocket::ByteEvent& ev) noexcept override {
    byteEventMock(socket, ev);
  }
  void byteEventsEnabled(AsyncSocket* socket) noexcept override {
    byteEventsEnabledMock(socket);
  }
  void byteEventsUnavailable(
      AsyncSocket* trans, const AsyncSocketException& ex) noexcept override {
    byteEventsUnavailableMock(trans, ex);
  }
  void fdDetach(AsyncSocket* sock) noexcept override { fdDetachMock(sock); }
  void fdAttach(AsyncSocket* sock) noexcept override { fdAttachMock(sock); }
  void move(AsyncSocket* olds, AsyncSocket* news) noexcept override {
    moveMock(olds, news);
  }
};

} // namespace test
} // namespace folly
