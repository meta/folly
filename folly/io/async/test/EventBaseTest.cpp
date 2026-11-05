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

#include <vector>

#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/test/AsyncSignalHandlerTestLib.h>
#include <folly/io/async/test/EventBaseTestLib.h>
#include <folly/portability/GTest.h>

namespace folly {
namespace test {

struct DefaultBackendProvider : BackendProviderBase {
  static std::unique_ptr<folly::EventBaseBackendBase> getBackend() {
    return folly::EventBase::getDefaultBackend();
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(
    EventBaseTest, EventBaseTest, DefaultBackendProvider);
INSTANTIATE_TYPED_TEST_SUITE_P(
    AsyncSignalHandlerTest, AsyncSignalHandlerTest, DefaultBackendProvider);

namespace {

class RecordingLoopCallback : public EventBase::LoopCallback {
 public:
  RecordingLoopCallback(std::vector<int>& log, int id) : log_(log), id_(id) {}

  void runLoopCallback() noexcept override { log_.push_back(id_); }

 private:
  std::vector<int>& log_;
  int id_;
};

} // namespace

// Drains a list the way Thrift and mcrouter flush lists do.
TEST(LoopCallbackListTest, Drain) {
  std::vector<int> log;
  RecordingLoopCallback first(log, 1);
  RecordingLoopCallback second(log, 2);
  EventBase::LoopCallbackList list;
  EXPECT_TRUE(list.empty());

  list.push_back(first);
  list.push_back(second);
  EXPECT_FALSE(list.empty());
  EXPECT_EQ(2u, list.size());
  EXPECT_TRUE(first.isLoopCallbackScheduled());

  EventBase::LoopCallbackList moved;
  // A cast instead of std::move keeps lint from flagging the moved-from checks.
  moved = static_cast<EventBase::LoopCallbackList&&>(list);
  EXPECT_TRUE(list.empty());
  auto callbacks = static_cast<EventBase::LoopCallbackList&&>(moved);
  EXPECT_TRUE(moved.empty());
  while (!callbacks.empty()) {
    auto& callback = callbacks.front();
    callbacks.pop_front();
    EXPECT_FALSE(callback.isLoopCallbackScheduled());
    callback.runLoopCallback();
  }
  EXPECT_EQ((std::vector<int>{1, 2}), log);
}

TEST(LoopCallbackListTest, DestroyedCallbackUnlinks) {
  std::vector<int> log;
  RecordingLoopCallback kept(log, 1);
  EventBase::LoopCallbackList list;
  {
    RecordingLoopCallback destroyed(log, 2);
    list.push_back(destroyed);
    list.push_back(kept);
    EXPECT_EQ(2u, list.size());
  }
  EXPECT_EQ(1u, list.size());
  EXPECT_EQ(&kept, &list.front());

  kept.cancelLoopCallback();
  EXPECT_TRUE(list.empty());
  EXPECT_FALSE(kept.isLoopCallbackScheduled());
  EXPECT_TRUE(log.empty());
}

} // namespace test
} // namespace folly
