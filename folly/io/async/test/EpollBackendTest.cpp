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
#include <chrono>
#include <string>
#include <thread>

#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventHandler.h>
#include <folly/io/async/test/AsyncSignalHandlerTestLib.h>
#include <folly/io/async/test/EventBaseTestLib.h>
#include <folly/portability/Sockets.h>
#include <folly/portability/Unistd.h>
#include <folly/synchronization/Baton.h>

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

TEST(EpollBackendTest, DeletingAfterTheFdClosedSucceeds) {
  EventBase evb(epollOptions());
  SocketPair sp;
  EventBaseEvent event;
  event.eb_event_set(sp.fds[0], EV_READ | EV_PERSIST, &noopCallback, nullptr);
  event.eb_event_base_set(&evb);
  ASSERT_EQ(0, event.eb_event_add(nullptr));
  SocketPair::closeFd(sp.fds[0]);

  EXPECT_EQ(0, event.eb_event_del());
  EXPECT_FALSE(event.isEventRegistered());
}

TEST(EpollBackendTest, AClosedFdsRegistrationGivesWay) {
  EventBase evb(epollOptions());
  SocketPair before;
  SocketPair after;
  int fd = before.fds[0];
  Recorder stale(&evb, fd);
  ASSERT_TRUE(
      stale.registerHandler(EventHandler::READ | EventHandler::PERSIST));
  SocketPair::closeFd(before.fds[0]);
  ASSERT_EQ(fd, ::dup2(after.fds[0], fd));

  Recorder current(&evb, fd);
  ASSERT_TRUE(
      current.registerHandler(EventHandler::READ | EventHandler::PERSIST));
  after.sendByte();
  evb.loopOnce(EVLOOP_NONBLOCK);
  EXPECT_EQ(0, stale.calls);
  EXPECT_EQ(1, current.calls);

  // Leaves the registration of the Event that took the number over.
  stale.unregisterHandler();
  evb.loopOnce(EVLOOP_NONBLOCK);
  EXPECT_EQ(2, current.calls);
  current.unregisterHandler();
  ::close(fd);
}

// A dup keeps the closed fd's file, and so its epoll entry, alive.
TEST(EpollBackendTest, AnEntryLeftByAClosedFdIsIgnored) {
  EventBase evb(epollOptions());
  SocketPair old;
  int fd = old.fds[0];
  int dup = ::dup(fd);
  Recorder first(&evb, fd);
  ASSERT_TRUE(
      first.registerHandler(EventHandler::READ | EventHandler::PERSIST));
  SocketPair::closeFd(old.fds[0]);
  first.unregisterHandler();

  SocketPair fresh;
  ASSERT_EQ(fd, ::dup2(fresh.fds[0], fd));
  Recorder second(&evb, fd);
  ASSERT_TRUE(
      second.registerHandler(EventHandler::READ | EventHandler::PERSIST));
  // Readies the old file only.
  old.sendByte();
  evb.loopOnce(EVLOOP_NONBLOCK);
  EXPECT_EQ(0, second.calls);

  second.unregisterHandler();
  ::close(fd);
  ::close(dup);
}

TEST(EpollBackendTest, ReadAndWriteEventsOnOneFdAreIndependent) {
  EventBase evb(epollOptions());
  SocketPair sp;
  Recorder reader(&evb, sp.fds[0]);
  Recorder writer(&evb, sp.fds[0]);
  ASSERT_TRUE(
      reader.registerHandler(EventHandler::READ | EventHandler::PERSIST));
  ASSERT_TRUE(
      writer.registerHandler(EventHandler::WRITE | EventHandler::PERSIST));
  sp.sendByte();

  evb.loopOnce(EVLOOP_NONBLOCK);
  EXPECT_EQ(EventHandler::READ, reader.seen);
  EXPECT_EQ(EventHandler::WRITE, writer.seen);

  writer.unregisterHandler();
  evb.loopOnce(EVLOOP_NONBLOCK);
  EXPECT_EQ(2, reader.calls);
  EXPECT_EQ(1, writer.calls);
}

TEST(EpollBackendTest, OneShotEventsPerDirectionOnOneFdEachFireOnce) {
  EventBase evb(epollOptions());
  SocketPair sp;
  Recorder reader(&evb, sp.fds[0]);
  Recorder writer(&evb, sp.fds[0]);
  ASSERT_TRUE(reader.registerHandler(EventHandler::READ));
  ASSERT_TRUE(writer.registerHandler(EventHandler::WRITE));
  // Keeps the loop polling once the Events under test are gone.
  evb.runAfterDelay([] {}, 60'000);

  evb.loopOnce(EVLOOP_NONBLOCK);
  EXPECT_EQ(1, writer.calls);
  EXPECT_EQ(0, reader.calls);
  EXPECT_TRUE(reader.isHandlerRegistered());

  sp.sendByte();
  evb.loopOnce(EVLOOP_NONBLOCK);
  evb.loopOnce(EVLOOP_NONBLOCK);
  EXPECT_EQ(1, reader.calls);
  EXPECT_EQ(1, writer.calls);
}

TEST(EpollBackendTest, AHangUpWakesTheEventsOfBothDirections) {
  EventBase evb(epollOptions());
  SocketPair sp;
  Recorder reader(&evb, sp.fds[0]);
  Recorder writer(&evb, sp.fds[0]);
  ASSERT_TRUE(reader.registerHandler(EventHandler::READ));
  // Leaves the writer nothing to wake for but the hang-up.
  std::string chunk(4096, 'x');
  while (::write(sp.fds[0], chunk.data(), chunk.size()) > 0) {
  }
  ASSERT_TRUE(writer.registerHandler(EventHandler::WRITE));
  SocketPair::closeFd(sp.fds[1]);

  evb.loopOnce(EVLOOP_NONBLOCK);
  EXPECT_EQ(EventHandler::READ, reader.seen);
  EXPECT_EQ(EventHandler::WRITE, writer.seen);
}

TEST(EpollBackendTest, EdgeTriggeringIsRefusedWhileAnotherEventSharesTheFd) {
  EventBase evb(epollOptions());
  SocketPair sp;
  Recorder reader(&evb, sp.fds[0]);
  Recorder writer(&evb, sp.fds[0]);
  ASSERT_TRUE(
      reader.registerHandler(EventHandler::READ | EventHandler::PERSIST));
  ASSERT_TRUE(
      writer.registerHandler(EventHandler::WRITE | EventHandler::PERSIST));

  EXPECT_FALSE(reader.setEdgeTriggered());
}

TEST(EpollBackendTest, AnEventCannotJoinAnFdHeldEdgeTriggered) {
  EventBase evb(epollOptions());
  SocketPair sp;
  Recorder reader(&evb, sp.fds[0]);
  ASSERT_TRUE(
      reader.registerHandler(EventHandler::READ | EventHandler::PERSIST));
  ASSERT_TRUE(reader.setEdgeTriggered());

  EventBaseEvent writer;
  writer.eb_event_set(sp.fds[0], EV_WRITE, &noopCallback, nullptr);
  writer.eb_event_base_set(&evb);
  errno = 0;
  EXPECT_EQ(-1, writer.eb_event_add(nullptr));
  EXPECT_EQ(EBUSY, errno);
}

TEST(EpollBackendTest, EdgeTriggeringAgainMakesNoEpollCall) {
  EventBase evb(epollOptions());
  SocketPair sp;
  Recorder reader(&evb, sp.fds[0]);
  ASSERT_TRUE(
      reader.registerHandler(EventHandler::READ | EventHandler::PERSIST));
  ASSERT_TRUE(reader.setEdgeTriggered());
  // Every epoll_ctl on the fd fails from here on.
  SocketPair::closeFd(sp.fds[0]);

  EXPECT_TRUE(reader.setEdgeTriggered());
}

TEST(EpollBackendTest, AddingARegisteredEventAgainChangesNothing) {
  EventBase evb(epollOptions());
  SocketPair sp;
  Recorder writer(&evb, sp.fds[0]);
  ASSERT_TRUE(
      writer.registerHandler(EventHandler::WRITE | EventHandler::PERSIST));
  EventBaseEvent reader;
  reader.eb_event_set(sp.fds[0], EV_READ | EV_PERSIST, &noopCallback, nullptr);
  reader.eb_event_base_set(&evb);
  ASSERT_EQ(0, reader.eb_event_add(nullptr));
  EXPECT_EQ(0, reader.eb_event_add(nullptr));

  reader.eb_event_del();
  writer.unregisterHandler();
  // Only the EventBase's internal Events are left.
  EXPECT_EQ(1, evb.getBackend()->eb_event_base_loop(EVLOOP_NONBLOCK));
}

TEST(EpollBackendTest, AnEventWithoutDirectionsTakesNoFd) {
  EventBase evb(epollOptions());
  SocketPair sp;
  EventBaseEvent bare;
  bare.eb_event_set(sp.fds[0], EV_PERSIST, &noopCallback, nullptr);
  bare.eb_event_base_set(&evb);
  ASSERT_EQ(0, bare.eb_event_add(nullptr));

  Recorder reader(&evb, sp.fds[0]);
  ASSERT_TRUE(
      reader.registerHandler(EventHandler::READ | EventHandler::PERSIST));
  sp.sendByte();
  evb.loopOnce(EVLOOP_NONBLOCK);
  EXPECT_EQ(1, reader.calls);
}

// What MuxIOThreadPoolExecutor waits on while a loop is suspended.
TEST(EpollBackendTest, ADueTimerTurnsThePollableFdReadable) {
  EventBase evb(epollOptions());
  bool fired = false;
  evb.runAfterDelay([&] { fired = true; }, 20);
  int fd = evb.getBackend()->getPollableFd();

  for (int round = 0; round < 10 && !fired; ++round) {
    if (evb.loopWithSuspension() != EventBase::LoopStatus::kSuspended) {
      break;
    }
    pollfd pfd{fd, POLLIN, 0};
    ASSERT_EQ(1, ::poll(&pfd, 1, 5000)) << "suspended with a timer pending";
  }
  EXPECT_TRUE(fired);
}

// A caller that loops without blocking, then waits on the pollable fd itself.
TEST(EpollBackendTest, ATimerScheduledDuringAPassTurnsThePollableFdReadable) {
  EventBase evb(epollOptions());
  SocketPair sp;
  bool fired = false;
  auto timeout = AsyncTimeout::make(evb, [&]() noexcept { fired = true; });
  struct Scheduler : Recorder {
    using Recorder::Recorder;
    void handlerReady(uint16_t events) noexcept override {
      Recorder::handlerReady(events);
      timeout->scheduleTimeout(20);
    }
    AsyncTimeout* timeout{nullptr};
  } scheduler(&evb, sp.fds[0]);
  scheduler.timeout = timeout.get();
  ASSERT_TRUE(scheduler.registerHandler(EventHandler::READ));
  sp.sendByte();
  evb.loopOnce(EVLOOP_NONBLOCK);
  ASSERT_EQ(1, scheduler.calls);

  pollfd pfd{evb.getBackend()->getPollableFd(), POLLIN, 0};
  ASSERT_EQ(1, ::poll(&pfd, 1, 5000));
  evb.loopOnce(EVLOOP_NONBLOCK);
  EXPECT_TRUE(fired);
}

TEST(EpollBackendTest, ATimeoutScheduledAfterACancelFiresOnTime) {
  EventBase evb(epollOptions());
  auto cancelled = AsyncTimeout::make(evb, []() noexcept {});
  bool fired = false;
  auto later = AsyncTimeout::make(evb, [&]() noexcept { fired = true; });
  // Leaves the timerfd armed for 10 ms, earlier than anything pending.
  cancelled->scheduleTimeout(10);
  cancelled->cancelTimeout();
  auto start = std::chrono::steady_clock::now();
  later->scheduleTimeout(40);

  Baton<> looped;
  std::thread guard([&] {
    if (!looped.try_wait_for(std::chrono::seconds(5))) {
      evb.terminateLoopSoon();
    }
  });
  evb.loop();
  looped.post();
  guard.join();
  EXPECT_TRUE(fired);
  EXPECT_GE(
      std::chrono::steady_clock::now() - start, std::chrono::milliseconds(40));
}

} // namespace test
} // namespace folly

#endif
