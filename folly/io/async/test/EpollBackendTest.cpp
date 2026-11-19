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

#include <folly/io/async/EpollBackend.h>

#if FOLLY_HAS_EPOLL

#include <cerrno>

#include <folly/io/async/EventHandler.h>
#include <folly/io/async/test/AsyncSignalHandlerTestLib.h>
#include <folly/io/async/test/EventBaseTestLib.h>
#include <folly/portability/Sockets.h>
#include <folly/portability/Unistd.h>

namespace folly {
namespace test {

struct EpollBackendProvider : BackendProviderBase {
  static std::unique_ptr<folly::EventBaseBackendBase> getBackend() {
    folly::EpollBackend::Options options;
    return std::make_unique<folly::EpollBackend>(options);
  }
};
INSTANTIATE_TYPED_TEST_SUITE_P(
    EventBaseTest, EventBaseTest, EpollBackendProvider);
INSTANTIATE_TYPED_TEST_SUITE_P(
    AsyncSignalHandlerTest, AsyncSignalHandlerTest, EpollBackendProvider);

namespace {

EventBase::Options epollOptions() {
  return EventBase::Options().setBackendFactory([] {
    return EpollBackendProvider::getBackend();
  });
}

struct SocketPair {
  SocketPair() {
    PCHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds) == 0);
  }
  ~SocketPair() {
    for (int& fd : fds) {
      closeFd(fd);
    }
  }
  static void closeFd(int& fd) {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
  void sendByte() const {
    char byte = 'x';
    PCHECK(::write(fds[1], &byte, 1) == 1);
  }
  int fds[2] = {-1, -1};
};

struct Recorder : EventHandler {
  Recorder(EventBase* evb, int fd)
      : EventHandler(evb, NetworkSocket::fromFd(fd)) {}
  void handlerReady(uint16_t events) noexcept override {
    seen |= events;
    ++calls;
  }
  uint16_t seen{0};
  int calls{0};
};

void noopCallback(libevent_fd_t, short, void*) {}

} // namespace

TEST(EpollBackendTest, ARefusedRegistrationLeavesTheExistingOneWorking) {
  EventBase evb(epollOptions());
  SocketPair sp;
  Recorder first(&evb, sp.fds[0]);
  Recorder second(&evb, sp.fds[0]);
  ASSERT_TRUE(
      first.registerHandler(EventHandler::READ | EventHandler::PERSIST));
  EXPECT_FALSE(
      second.registerHandler(EventHandler::READ | EventHandler::PERSIST));

  sp.sendByte();
  evb.loopOnce(EVLOOP_NONBLOCK);
  EXPECT_EQ(1, first.calls);
}

TEST(EpollBackendTest, ARefusedAddLeavesTheEventUnregistered) {
  EventBase evb(epollOptions());
  SocketPair sp;
  int closed = sp.fds[0];
  SocketPair::closeFd(sp.fds[0]);
  EventBaseEvent event;
  event.eb_event_set(closed, EV_READ, &noopCallback, nullptr);
  event.eb_event_base_set(&evb);

  errno = 0;
  EXPECT_EQ(-1, event.eb_event_add(nullptr));
  EXPECT_EQ(EBADF, errno);
  EXPECT_FALSE(event.isEventRegistered());
}

} // namespace test
} // namespace folly

#endif
