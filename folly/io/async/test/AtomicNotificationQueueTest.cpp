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

#include <algorithm>
#include <functional>
#include <iterator>
#include <set>
#include <utility>
#include <vector>

#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseAtomicNotificationQueue.h>
#include <folly/portability/Fcntl.h>
#include <folly/portability/GTest.h>

using namespace folly;
using namespace std;

#ifndef _WIN32
namespace {
// The descriptors this process has open, by number. Descriptors are handed
// out lowest-free, so anything a test opens lands well under the bound.
std::set<int> openFds() {
  std::set<int> fds;
  for (int fd = 0; fd < 4096; ++fd) {
    if (fcntl(fd, F_GETFD) != -1) {
      fds.insert(fd);
    }
  }
  return fds;
}
} // namespace
#endif

template <typename Task>
struct AtomicNotificationQueueConsumer {
  explicit AtomicNotificationQueueConsumer(vector<Task>& tasks)
      : tasks(tasks) {}

  void operator()(Task&& value) noexcept {
    tasks.push_back(value);
    if (fn) {
      fn(std::move(value));
    }
  }

  function<void(Task&&)> fn;
  vector<Task>& tasks;
};

TEST(AtomicNotificationQueueTest, TryPutMessage) {
  vector<int> data;
  AtomicNotificationQueueConsumer<int> consumer{data};
  EventBaseAtomicNotificationQueue<int, decltype(consumer)> queue{
      std::move(consumer)};

  constexpr uint32_t kMaxSize = 10;

  for (auto i = 1; i <= 9; ++i) {
    queue.putMessage(std::move(i));
  }

  EXPECT_TRUE(queue.tryPutMessage(10, kMaxSize));
  EXPECT_EQ(queue.size(), 10);
  EXPECT_FALSE(queue.tryPutMessage(11, kMaxSize));
  EXPECT_EQ(queue.size(), 10);
  queue.putMessage(11);
  EXPECT_EQ(queue.size(), 11);
  EXPECT_FALSE(queue.tryPutMessage(12, kMaxSize));

  queue.drain();
  EXPECT_TRUE(queue.tryPutMessage(0, kMaxSize));
  EXPECT_EQ(queue.size(), 1);
}

TEST(AtomicNotificationQueueTest, DiscardDequeuedTasks) {
  struct TaskWithExpiry {
    int datum;
    bool isExpired;
  };

  struct Consumer {
    explicit Consumer(std::vector<int>& data) : data(data) {}
    AtomicNotificationQueueTaskStatus operator()(
        TaskWithExpiry&& task) noexcept {
      if (task.isExpired) {
        return AtomicNotificationQueueTaskStatus::DISCARD;
      }
      data.push_back(task.datum);
      return AtomicNotificationQueueTaskStatus::CONSUMED;
    }
    vector<int>& data;
  };
  vector<int> data;
  Consumer consumer{data};

  EventBaseAtomicNotificationQueue<TaskWithExpiry, Consumer> queue{
      std::move(consumer)};
  queue.setMaxReadAtOnce(10);

  vector<TaskWithExpiry> tasks = {
      {0, false},
      {1, true},
      {2, true},
      {3, false},
      {4, false},
      {5, false},
      {6, false},
      {7, true},
      {8, false},
      {9, false},
      {10, false},
      {11, false},
      {12, true},
      {13, false},
      {14, true},
      {15, false},
  };

  EventBase eventBase;
  queue.startConsuming(&eventBase);

  for (auto& t : tasks) {
    queue.putMessage(t);
  }

  eventBase.loopOnce();

  vector<int> expectedMessages = {0, 3, 4, 5, 6, 8, 9, 10, 11, 13};
  EXPECT_EQ(data.size(), expectedMessages.size());
  for (unsigned i = 0; i < expectedMessages.size(); ++i) {
    EXPECT_EQ(data.at(i), expectedMessages[i]);
  }

  data.clear();
  eventBase.loopOnce();

  EXPECT_EQ(data.size(), 1);
  EXPECT_EQ(data.at(0), 15);
}

TEST(AtomicNotificationQueueTest, PutMessage) {
  struct Data {
    int datum;
    bool isExpired;

    explicit Data(int datum, bool isExpired)
        : datum(datum), isExpired(isExpired) {}

    bool operator==(const Data& data) const {
      return datum == data.datum && isExpired == data.isExpired;
    }
  };

  struct Consumer {
    explicit Consumer(vector<Data>& data) : data(data) {}
    void operator()(Data&& task) noexcept { data.push_back(task); }
    vector<Data>& data;
  };

  vector<Data>
      expected =
          {Data(10, false), Data(20, true), Data(-8, true), Data(0, false)},
      actual;
  Consumer consumer{actual};

  EventBaseAtomicNotificationQueue<Data, decltype(consumer)> queue{
      std::move(consumer)};
  queue.setMaxReadAtOnce(0);

  EventBase eventBase;
  queue.startConsuming(&eventBase);

  for (auto& t : expected) {
    queue.putMessage(t.datum, t.isExpired);
  }

  eventBase.loopOnce();

  EXPECT_EQ(expected.size(), actual.size());
  for (unsigned i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(expected[i], actual[i]);
  }
}

TEST(AtomicNotificationQueueTest, ConsumeStop) {
  struct Consumer {
    size_t* consumed;
    auto operator()(bool&& stop) noexcept {
      ++*consumed;
      return stop
          ? AtomicNotificationQueueTaskStatus::CONSUMED_STOP
          : AtomicNotificationQueueTaskStatus::CONSUMED;
    }
  };

  size_t consumed = 0;
  Consumer consumer{&consumed};

  EventBaseAtomicNotificationQueue<bool, decltype(consumer)> queue{
      std::move(consumer)};
  queue.setMaxReadAtOnce(3);

  EventBase eventBase;
  queue.startConsuming(&eventBase);

  for (auto t : {false, true, false, false, false, false}) {
    queue.putMessage(t);
  }

  ASSERT_EQ(consumed, 0);
  eventBase.loopOnce(EVLOOP_NONBLOCK);
  // Second message stopped consuming.
  ASSERT_EQ(std::exchange(consumed, 0), 2);
  eventBase.loopOnce(EVLOOP_NONBLOCK);
  // setMaxReadAtOnce() still honored.
  ASSERT_EQ(std::exchange(consumed, 0), 3);
  eventBase.loopOnce(EVLOOP_NONBLOCK);
  ASSERT_EQ(std::exchange(consumed, 0), 1);

  for (auto t : {true, true, true}) {
    queue.putMessage(t);
  }
  eventBase.loopOnce(EVLOOP_NONBLOCK);
  ASSERT_EQ(std::exchange(consumed, 0), 1);
  // Local queue is still non-empty, add a message to the atomic queue.
  queue.putMessage(true);

  // All messages, including the one just added, should be consumed.
  for (size_t i = 0; i < 3; ++i) {
    eventBase.loopOnce(EVLOOP_NONBLOCK);
    ASSERT_EQ(std::exchange(consumed, 0), 1);
  }
}

// drive() should not process messages that were put during its execution.
TEST(AtomicNotificationQueueTest, DriveSnapshot) {
  struct Consumer {
    size_t* consumed;
    auto operator()(folly::Func f) noexcept {
      f();
      ++*consumed;
      return AtomicNotificationQueueTaskStatus::CONSUMED;
    }
  };

  size_t consumed = 0;
  Consumer consumer{&consumed};
  EventBaseAtomicNotificationQueue<folly::Func, decltype(consumer)> queue{
      std::move(consumer)};

  EventBase eventBase;
  queue.startConsuming(&eventBase);

  queue.setMaxReadAtOnce(0);
  queue.putMessage([&] { queue.putMessage([] {}); });
  ASSERT_EQ(consumed, 0);
  // Atomic queue was empty when the second putMessage happened(), so the second
  // message is not processed.
  eventBase.loopOnce(EVLOOP_NONBLOCK);
  ASSERT_EQ(std::exchange(consumed, 0), 1);
  eventBase.loopOnce(EVLOOP_NONBLOCK);
  ASSERT_EQ(std::exchange(consumed, 0), 1);

  bool lastRan = false;
  queue.putMessage([&] {});
  queue.putMessage([&] { queue.putMessage([&] { lastRan = true; }); });

  // Leave the second message in the local queue.
  queue.setMaxReadAtOnce(1);
  eventBase.loopOnce(EVLOOP_NONBLOCK);
  ASSERT_EQ(std::exchange(consumed, 0), 1);

  // This will be in the atomic queue.
  queue.putMessage([] {});
  queue.setMaxReadAtOnce(0);
  // We'll consume the local queue and part of the atomic queue, but stop at the
  // message that was put in the callback.
  eventBase.loopOnce(EVLOOP_NONBLOCK);
  ASSERT_EQ(std::exchange(consumed, 0), 2);

  EXPECT_FALSE(lastRan);
  eventBase.loopOnce(EVLOOP_NONBLOCK);
  ASSERT_EQ(std::exchange(consumed, 0), 1);
  EXPECT_TRUE(lastRan);
}

#ifndef _WIN32
// Every descriptor the queue opens is close-on-exec: the eventfd by
// construction, and the pipe it falls back to (macOS) on both ends. A child
// process has no use for a wakeup descriptor, and an inherited one is leaked
// for the child's lifetime (and across an in-place exec, forever).
TEST(AtomicNotificationQueueTest, FdsAreCloseOnExec) {
  const auto before = openFds();
  vector<int> data;
  AtomicNotificationQueueConsumer<int> consumer{data};
  EventBaseAtomicNotificationQueue<int, decltype(consumer)> queue{
      std::move(consumer)};
  const auto after = openFds();

  std::vector<int> opened;
  std::set_difference(
      after.begin(),
      after.end(),
      before.begin(),
      before.end(),
      std::back_inserter(opened));
  ASSERT_FALSE(opened.empty());
  for (int fd : opened) {
    EXPECT_TRUE(fcntl(fd, F_GETFD) & FD_CLOEXEC) << "fd " << fd;
  }
}
#endif
