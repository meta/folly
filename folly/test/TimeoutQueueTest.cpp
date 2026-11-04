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

#include <folly/TimeoutQueue.h>

#include <limits>
#include <stdexcept>
#include <vector>

#include <folly/portability/GTest.h>

using namespace folly;

namespace {
// A callback whose copy throws while *throwOnCopy is set.
struct ThrowingCopyCallback {
  bool* throwOnCopy;
  std::vector<TimeoutQueue::Id>* events;

  ThrowingCopyCallback(bool* t, std::vector<TimeoutQueue::Id>* e)
      : throwOnCopy(t), events(e) {}
  ThrowingCopyCallback(const ThrowingCopyCallback& other)
      : throwOnCopy(other.throwOnCopy), events(other.events) {
    if (*throwOnCopy) {
      throw std::runtime_error("copy");
    }
  }
  ThrowingCopyCallback(ThrowingCopyCallback&&) noexcept = default;
  ThrowingCopyCallback& operator=(const ThrowingCopyCallback&) = delete;
  ThrowingCopyCallback& operator=(ThrowingCopyCallback&&) = delete;

  void operator()(TimeoutQueue::Id id, int64_t /* now */) const {
    events->push_back(id);
  }
};
} // namespace

TEST(TimeoutQueue, Simple) {
  using EventVec = std::vector<TimeoutQueue::Id>;
  EventVec events;

  TimeoutQueue q;
  TimeoutQueue::Callback cb =
      [&events](TimeoutQueue::Id id, int64_t /* now */) {
        events.push_back(id);
      };

  EXPECT_EQ(1, q.add(0, 10, cb));
  EXPECT_EQ(2, q.add(0, 11, cb));
  EXPECT_EQ(3, q.addRepeating(0, 9, cb));

  EXPECT_TRUE(events.empty());
  EXPECT_EQ(21, q.runOnce(12)); // now+9

  bool r = (EventVec{3, 1, 2} == events);
  EXPECT_TRUE(r);

  events.clear();
  EXPECT_EQ(49, q.runOnce(40));
  r = (EventVec{3} == events);
  EXPECT_TRUE(r);
}

TEST(TimeoutQueue, Erase) {
  using EventVec = std::vector<TimeoutQueue::Id>;
  EventVec events;

  TimeoutQueue q;
  TimeoutQueue::Callback cb =
      [&events, &q](TimeoutQueue::Id id, int64_t /* now */) {
        events.push_back(id);
        if (id == 2) {
          q.erase(1);
        }
      };

  EXPECT_EQ(1, q.addRepeating(0, 10, cb));
  EXPECT_EQ(2, q.add(0, 35, cb));

  int64_t now = 0;
  while (now < std::numeric_limits<int64_t>::max()) {
    now = q.runOnce(now);
  }

  bool r = (EventVec{1, 1, 1, 2} == events);
  EXPECT_TRUE(r);
}

TEST(TimeoutQueue, RunOnceRepeating) {
  int count = 0;
  TimeoutQueue q;
  TimeoutQueue::Callback cb =
      [&count, &q](TimeoutQueue::Id id, int64_t /* now */) {
        if (++count == 100) {
          EXPECT_TRUE(q.erase(id));
        }
      };

  EXPECT_EQ(1, q.addRepeating(0, 0, cb));

  EXPECT_EQ(0, q.runOnce(0));
  EXPECT_EQ(1, count);
  EXPECT_EQ(0, q.runOnce(0));
  EXPECT_EQ(2, count);
  EXPECT_EQ(std::numeric_limits<int64_t>::max(), q.runLoop(0));
  EXPECT_EQ(100, count);
}

TEST(TimeoutQueue, RunOnceReschedule) {
  int count = 0;
  TimeoutQueue q;
  TimeoutQueue::Callback cb;
  cb = [&count, &q, &cb](TimeoutQueue::Id id, int64_t now) {
    if (++count < 100) {
      EXPECT_LT(id, q.add(now, 0, cb));
    }
  };

  EXPECT_EQ(1, q.add(0, 0, cb));

  EXPECT_EQ(0, q.runOnce(0));
  EXPECT_EQ(1, count);
  EXPECT_EQ(0, q.runOnce(0));
  EXPECT_EQ(2, count);
  EXPECT_EQ(std::numeric_limits<int64_t>::max(), q.runLoop(0));
  EXPECT_EQ(100, count);
}

TEST(TimeoutQueue, EqualExpirationsRunInInsertionOrder) {
  std::vector<TimeoutQueue::Id> events;
  TimeoutQueue q;
  TimeoutQueue::Callback cb = [&events](TimeoutQueue::Id id, int64_t) {
    events.push_back(id);
  };

  EXPECT_EQ(1, q.addRepeating(0, 10, cb));
  EXPECT_EQ(2, q.add(5, 5, cb));
  EXPECT_EQ(3, q.addRepeating(2, 8, cb));
  EXPECT_EQ(4, q.add(0, 10, cb));
  EXPECT_TRUE(q.erase(2));
  EXPECT_FALSE(q.erase(2));
  EXPECT_EQ(5, q.add(10, 0, cb));

  EXPECT_EQ(18, q.runOnce(10));
  EXPECT_EQ((std::vector<TimeoutQueue::Id>{1, 3, 4, 5}), events);

  // Event 3 is back at 18, ahead of event 6, which is added later.
  events.clear();
  EXPECT_EQ(6, q.addRepeating(10, 8, cb));
  EXPECT_EQ(28, q.runOnce(20));
  EXPECT_EQ((std::vector<TimeoutQueue::Id>{3, 6, 1}), events);
}

TEST(TimeoutQueue, FailedRunLeavesQueueUnchanged) {
  std::vector<TimeoutQueue::Id> events;
  bool throwOnCopy = false;
  TimeoutQueue q;
  TimeoutQueue::Callback cb = [&events](TimeoutQueue::Id id, int64_t) {
    events.push_back(id);
  };
  EXPECT_EQ(1, q.add(0, 10, cb));
  EXPECT_EQ(
      2, q.addRepeating(0, 10, ThrowingCopyCallback(&throwOnCopy, &events)));
  EXPECT_EQ(3, q.add(0, 30, cb));

  throwOnCopy = true;
  EXPECT_THROW(q.runOnce(10), std::runtime_error);
  throwOnCopy = false;

  EXPECT_TRUE(events.empty());
  EXPECT_EQ(10, q.nextExpiration());
  EXPECT_TRUE(q.erase(1));
  EXPECT_EQ(20, q.runOnce(10));
  EXPECT_EQ((std::vector<TimeoutQueue::Id>{2}), events);
  EXPECT_TRUE(q.erase(2));
  EXPECT_EQ(std::numeric_limits<int64_t>::max(), q.runOnce(30));
  EXPECT_EQ((std::vector<TimeoutQueue::Id>{2, 3}), events);
}
