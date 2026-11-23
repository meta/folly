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

// TODO(lume): remove this file in future diff once we replace LegacyObserver
// with AsyncSocket::ManagedObserver

#pragma once

#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/AsyncSocketException.h>
#include <folly/portability/GMock.h>

namespace folly {
namespace test {

class MockAsyncSocketLegacyLifecycleObserver
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
  MOCK_METHOD(
      void,
      prewriteMock,
      (AsyncSocket*, const PrewriteState&, PrewriteRequestContainer&));

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
      AsyncSocket* socket, const AsyncSocketException& ex) noexcept override {
    byteEventsUnavailableMock(socket, ex);
  }
  void prewrite(
      AsyncSocket* socket,
      const PrewriteState& state,
      PrewriteRequestContainer& container) noexcept override {
    prewriteMock(socket, state, container);
  }
};

/**
 * Extends mock class to simplify ByteEvents tests.
 */
class MockAsyncSocketLegacyLifecycleObserverForByteEvents
    : public MockAsyncSocketLegacyLifecycleObserver {
 public:
  MockAsyncSocketLegacyLifecycleObserverForByteEvents(
      AsyncSocket* socket,
      const MockAsyncSocketLegacyLifecycleObserverForByteEvents::Config&
          observerConfig)
      : MockAsyncSocketLegacyLifecycleObserver(observerConfig),
        socket_(socket) {
    ON_CALL(*this, byteEventMock(testing::_, testing::_))
        .WillByDefault(
            testing::Invoke([this](
                                AsyncSocket* socketport,
                                const AsyncSocket::ByteEvent& event) {
              CHECK_EQ(this->socket_, socketport);
              byteEvents_.emplace_back(event);
            }));
    ON_CALL(*this, byteEventsEnabledMock(testing::_))
        .WillByDefault(testing::Invoke([this](AsyncSocket* socketport) {
          CHECK_EQ(this->socket_, socketport);
          byteEventsEnabledCalled_++;
        }));

    ON_CALL(*this, byteEventsUnavailableMock(testing::_, testing::_))
        .WillByDefault(
            testing::Invoke(
                [this](
                    AsyncSocket* socketport, const AsyncSocketException& ex) {
                  CHECK_EQ(this->socket_, socketport);
                  byteEventsUnavailableCalled_++;
                  byteEventsUnavailableCalledEx_.emplace(ex);
                }));
    socket_->addLifecycleObserver(this);
  }

  const std::vector<AsyncSocket::ByteEvent>& getByteEvents() {
    return byteEvents_;
  }

  folly::Optional<AsyncSocket::ByteEvent> getByteEventReceivedWithOffset(
      const uint64_t offset, const AsyncSocket::ByteEvent::Type type) {
    for (const auto& byteEvent : byteEvents_) {
      if (type == byteEvent.type && offset == byteEvent.offset) {
        return byteEvent;
      }
    }
    return folly::none;
  }

  folly::Optional<uint64_t> maxOffsetForByteEventReceived(
      const AsyncSocket::ByteEvent::Type type) {
    folly::Optional<uint64_t> maybeMaxOffset;
    for (const auto& byteEvent : byteEvents_) {
      if (type == byteEvent.type &&
          (!maybeMaxOffset.has_value() ||
           maybeMaxOffset.value() <= byteEvent.offset)) {
        maybeMaxOffset = byteEvent.offset;
      }
    }
    return maybeMaxOffset;
  }

  bool checkIfByteEventReceived(
      const AsyncSocket::ByteEvent::Type type, const uint64_t offset) {
    for (const auto& byteEvent : byteEvents_) {
      if (type == byteEvent.type && offset == byteEvent.offset) {
        return true;
      }
    }
    return false;
  }

  void waitForByteEvent(
      const AsyncSocket::ByteEvent::Type type, const uint64_t offset) {
    while (!checkIfByteEventReceived(type, offset)) {
      socket_->getEventBase()->loopOnce();
    }
  }

  // Exposed ByteEvent helper fields with const
  const uint32_t& byteEventsEnabledCalled{byteEventsEnabledCalled_};
  const uint32_t& byteEventsUnavailableCalled{byteEventsUnavailableCalled_};
  const folly::Optional<AsyncSocketException>& byteEventsUnavailableCalledEx{
      byteEventsUnavailableCalledEx_};
  const std::vector<AsyncSocket::ByteEvent>& byteEvents{byteEvents_};

 private:
  AsyncSocket* socket_;

  // ByteEvents helpers
  uint32_t byteEventsEnabledCalled_{0};
  uint32_t byteEventsUnavailableCalled_{0};
  folly::Optional<AsyncSocketException> byteEventsUnavailableCalledEx_;
  std::vector<AsyncSocket::ByteEvent> byteEvents_;
};

} // namespace test
} // namespace folly
