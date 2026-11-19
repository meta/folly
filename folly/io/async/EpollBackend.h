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

  // The Event that holds an fd's epoll entry, and the generation the entry
  // carries.
  struct FdEntry {
    EventInfo* owner{nullptr};
    uint32_t generation{0};
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

  // Ends `info`'s hold on `fd` and drops the fd's epoll entry, unless another
  // Event took the number over. Returns 0, or -1 with errno.
  int releaseFd(int fd, EventInfo* info);

  void updateTimerFd();
  void addTimerEvent(Event& event, const struct timeval* timeout);
  int removeTimerEvent(Event& event);
  void processTimers();
  void setProcessTimers();

  void addSignalEvent(Event& event);
  int removeSignalEvent(Event& event);
  void processSignals();

  const Options options_;

  int epollFd_{-1};

  F14FastMap<int, FdEntry> fds_;
  uint32_t nextGeneration_{0};

  size_t numInsertedEvents_{0};
  size_t numInternalEvents_{0};

  bool loopBreak_{false};
  std::vector<struct epoll_event> events_; // Cache allocation.

  int timerFd_{-1};
  std::optional<std::chrono::steady_clock::time_point> timerFdExpiration_;
  IntrusiveHeap<TimerInfo> timers_;

  SocketPair signalFds_;
  std::map<int, std::set<Event*>> signals_;
};
} // namespace folly
#endif
