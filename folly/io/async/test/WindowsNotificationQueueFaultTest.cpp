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
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseAtomicNotificationQueue.h>
#include <folly/io/async/EventBaseBackendBase.h>
#include <folly/portability/Event.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

#ifdef _WIN32

namespace folly {
namespace {

using namespace std::chrono_literals;
using ::testing::ElementsAreArray;

class ReadRegistrationFailureBackend : public EventBaseBackendBase {
 public:
  ReadRegistrationFailureBackend()
      : delegate_(EventBase::getDefaultBackend()) {}

  bool failReadRegistration{true};
  bool suppressReadDispatch{false};
  size_t failures{0};
  size_t readRegistrations{0};

  event_base* getEventBase() override { return delegate_->getEventBase(); }
  int eb_event_base_loop(int flags) override {
    return delegate_->eb_event_base_loop(flags);
  }
  int eb_event_base_loopbreak() override {
    return delegate_->eb_event_base_loopbreak();
  }
  void eb_yield_after_internal_event() override {
    delegate_->eb_yield_after_internal_event();
  }
  int eb_event_add(Event& event, const timeval* timeout) override {
    if (!timeout && (event.eb_ev_events() & EV_READ)) {
      if (failReadRegistration) {
        ++failures;
        return -1;
      }
      ++readRegistrations;
      if (suppressReadDispatch) {
        suppressed_.push_back(&event);
        event.eb_set_registration(Event::Registration::kInserted);
        return 0;
      }
    }
    return delegate_->eb_event_add(event, timeout);
  }
  int eb_event_del(Event& event) override {
    const auto found =
        std::find(suppressed_.begin(), suppressed_.end(), &event);
    if (found != suppressed_.end()) {
      suppressed_.erase(found);
      event.eb_clear_registration();
      return 0;
    }
    return delegate_->eb_event_del(event);
  }
  bool eb_event_active(Event& event, int result) override {
    if (std::find(suppressed_.begin(), suppressed_.end(), &event) !=
        suppressed_.end()) {
      return true;
    }
    return delegate_->eb_event_active(event, result);
  }

 private:
  std::vector<Event*> suppressed_;
  std::unique_ptr<EventBaseBackendBase> delegate_;
};

TEST(
    WindowsNotificationQueueFaultTest,
    TimerServicesTasksWithoutReadRegistration) {
  ReadRegistrationFailureBackend* backend = nullptr;
  EventBase::Options options;
  options.setBackendFactory([&] {
    auto result = std::make_unique<ReadRegistrationFailureBackend>();
    backend = result.get();
    return result;
  });
  EventBase loop(std::move(options));
  std::vector<int> delivered;
  auto consumer = [&delivered](int&& task) noexcept {
    delivered.push_back(task);
  };
  EventBaseAtomicNotificationQueue<int, decltype(consumer)> queue(
      std::move(consumer));
  queue.startConsuming(&loop);
  queue.putMessage(42);

  auto deadline = std::chrono::steady_clock::now() + 3s;
  while (delivered.empty() && std::chrono::steady_clock::now() < deadline) {
    ASSERT_TRUE(loop.loopOnce());
  }
  ASSERT_THAT(delivered, ElementsAreArray({42}));
  EXPECT_GT(backend->failures, 0);
  EXPECT_EQ(0, backend->readRegistrations);

  backend->failReadRegistration = false;
  queue.putMessage(43);
  deadline = std::chrono::steady_clock::now() + 3s;
  while ((delivered.size() < 2 || backend->readRegistrations == 0) &&
         std::chrono::steady_clock::now() < deadline) {
    ASSERT_TRUE(loop.loopOnce());
  }
  EXPECT_THAT(delivered, ElementsAreArray({42, 43}));
  EXPECT_GT(backend->readRegistrations, 0);
  queue.stopConsuming();
}

TEST(WindowsNotificationQueueFaultTest, HealthyTimerServicesMissedReadiness) {
  EventBase::Options options;
  options.setBackendFactory([] {
    auto result = std::make_unique<ReadRegistrationFailureBackend>();
    result->failReadRegistration = false;
    result->suppressReadDispatch = true;
    return result;
  });
  EventBase loop(std::move(options));
  std::vector<int> delivered;
  auto consumer = [&delivered](int&& task) noexcept {
    delivered.push_back(task);
  };
  EventBaseAtomicNotificationQueue<int, decltype(consumer)> queue(
      std::move(consumer));
  queue.startConsuming(&loop);
  for (int task = 0; task < 2; ++task) {
    queue.putMessage(task);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (delivered.size() < static_cast<size_t>(task + 1) &&
           std::chrono::steady_clock::now() < deadline) {
      ASSERT_TRUE(loop.loopOnce());
    }
    ASSERT_EQ(static_cast<size_t>(task + 1), delivered.size());
  }
  EXPECT_THAT(delivered, ElementsAreArray({0, 1}));
  queue.stopConsuming();
}

TEST(WindowsNotificationQueueFaultTest, QueueMayOutliveAttachedEventBase) {
  std::vector<int> delivered;
  auto consumer = [&delivered](int&& task) noexcept {
    delivered.push_back(task);
  };
  EventBaseAtomicNotificationQueue<int, decltype(consumer)> queue(
      std::move(consumer));
  {
    EventBase loop;
    queue.startConsuming(&loop);
    queue.putMessage(1);
    ASSERT_TRUE(loop.loopOnce());
  }
  queue.putMessage(2);
  {
    EventBase loop;
    queue.startConsuming(&loop);
    ASSERT_TRUE(loop.loopOnce());
  }
  EXPECT_THAT(delivered, ElementsAreArray({1, 2}));
}

} // namespace
} // namespace folly

#endif
