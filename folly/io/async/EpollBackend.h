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

#include <folly/io/async/Epoll.h>

#if FOLLY_HAS_EPOLL

#include <sys/types.h>

#include <array>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include <folly/container/F14Map.h>
#include <folly/container/IntrusiveHeap.h>
#include <folly/io/async/EventBaseBackendBase.h>

namespace folly {

class EpollBackend : public EventBaseBackendBase {
 public:
  struct Options {
    size_t numLoopEvents{128};

    Options& setNumLoopEvents(size_t val) {
      numLoopEvents = val;
      return *this;
    }
  };

  // The epoll fd and the timerfd are shared with a forked child. Adding an
  // Event or looping in the child terminates with "pid mismatch"; deleting one
  // there, as destructors do, leaves the kernel alone.
  explicit EpollBackend(Options options);
  ~EpollBackend() override;

  int getEpollFd() const { return epollFd_; }

  int getPollableFd() const override { return epollFd_; }

  // Returns a non-standard value 2 when called with EVLOOP_NONBLOCK and the
  // loop would block if called in a blocking fashion.
  int eb_event_base_loop(int flags) override;
  int eb_event_base_loopbreak() override;

  int eb_event_add(Event& event, const struct timeval* timeout) override;
  int eb_event_del(Event& event) override;

  bool eb_event_active(Event&, int) override { return false; }

  bool setEdgeTriggered(Event& event) override;

 private:
  struct EventInfo;
  struct TimerInfo;

  // The Event that holds each direction of an fd's epoll entry, and the
  // generation the entry carries.
  struct FdEntry {
    std::array<EventInfo*, 2> owners{};
    uint32_t generation{0};
    bool edge{false};
  };

  class SocketPair {
   public:
    SocketPair();

    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;

    ~SocketPair();

    int readFd() const { return fds_[1]; }

    int writeFd() const { return fds_[0]; }

   private:
    std::array<int, 2> fds_{{-1, -1}};
  };

  bool inOtherProcess() const;
  void checkPid() const;
  [[noreturn]] void checkPidFail() const;

  static uint32_t interest(const FdEntry& entry);
  // These return 0, or -1 with errno.
  // Gives `info` a new entry for `fd`: EEXIST while another Event's
  // registration on the fd is live.
  int holdFdAlone(int fd, EventInfo* info, short dirs);
  // Adds `dirs`, which no Event holds, to the entry of `fd`.
  int joinFd(int fd, EventInfo* info, short dirs);
  // Ends `info`'s hold on `fd`, unless another Event took the number over.
  int releaseFd(int fd, EventInfo* info);

  // Arms the timerfd for the earliest timer unless it is armed no later than
  // that. Cancelling a timer leaves it armed early, which costs one wake.
  void armTimerFd();
  void addTimerEvent(Event& event, const struct timeval* timeout);
  int removeTimerEvent(Event& event);
  void processTimers();
  void setProcessTimers();

  void addSignalEvent(Event& event);
  int removeSignalEvent(Event& event);
  void processSignals();

  const Options options_;
  const pid_t pid_;

  int epollFd_{-1};

  F14FastMap<int, FdEntry> fds_;
  uint32_t nextGeneration_{0};

  size_t numInsertedEvents_{0};
  size_t numInternalEvents_{0};

  bool loopBreak_{false};
  std::vector<struct epoll_event> events_; // Cache allocation.

  int timerFd_{-1};
  // The deadline the timerfd is armed for, never later than the earliest
  // timer's. The timerfd fires then, or 1 ms after arming if that is later.
  std::optional<std::chrono::steady_clock::time_point> timerFdExpiration_;
  IntrusiveHeap<TimerInfo> timers_;

  SocketPair signalFds_;
  std::map<int, std::set<Event*>> signals_;
};
} // namespace folly
#endif
