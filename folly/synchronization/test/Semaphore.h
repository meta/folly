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

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <folly/Optional.h>
#include <folly/ScopeGuard.h>
#include <folly/container/IntrusiveList.h>
#include <folly/lang/Exception.h>

namespace folly {
namespace test {

//  Semaphore
//
//  A basic portable semaphore, primarily intended for testing scenarios. Likely
//  to be much less performant than better-optimized semaphore implementations.
//
//  In the interest of portability, uses only synchronization mechanisms shipped
//  with all implementations of C++: std::mutex and std::condition_variable.
class Semaphore {
 public:
  Semaphore() {}

  explicit Semaphore(std::size_t value) : value_(value) {}

  bool try_wait() {
    std::unique_lock l{m_};
    if (value_ > 0) {
      --value_;
      return true;
    } else {
      return false;
    }
  }

  template <typename PreWait, typename PostWait>
  void wait(PreWait pre_wait, PostWait post_wait) {
    std::unique_lock l{m_};
    pre_wait();
    if (value_ > 0) {
      --value_;
      post_wait();
    } else {
      ++waiting_;
      cv_.wait(l, [&] { return signaled_ > 0; });
      --signaled_;
      post_wait();
    }
  }

  void wait() {
    wait([] {}, [] {});
  }

  template <typename PrePost>
  void post(PrePost pre_post) {
    std::unique_lock l{m_};
    if (value_ == -size_t(1)) {
      throw_exception<std::logic_error>("overflow");
    }
    pre_post();
    if (!waiting_) {
      ++value_;
    } else {
      --waiting_;
      ++signaled_;
      cv_.notify_one();
    }
  }

  void post() {
    post([] {});
  }

 private:
  std::size_t value_ = 0;
  std::size_t waiting_ = 0;
  std::size_t signaled_ = 0;
  std::mutex m_;
  std::condition_variable cv_;
};

enum class SemaphoreWakePolicy { Fifo, Lifo };

//  PolicySemaphore
//
//  A basic portable semaphore, primarily intended for testing scenarios. Likely
//  to be much less performant than better-optimized semaphore implementations.
//
//  Like Semaphore above, but with controlled wake order.
//
//  In the interest of portability, uses only synchronization mechanisms shipped
//  with all implementations of C++: std::mutex and std::condition_variable.
template <SemaphoreWakePolicy WakePolicy>
class PolicySemaphore {
 public:
  PolicySemaphore() {}

  explicit PolicySemaphore(std::size_t value) : value_(value) {}

  bool try_wait() {
    std::unique_lock lock{mutex_};
    if (value_) {
      --value_;
      return true;
    } else {
      return false;
    }
  }

  template <typename PreWait, typename PostWait>
  void wait(PreWait pre_wait, PostWait post_wait) {
    std::unique_lock lock{mutex_};
    pre_wait();
    if (value_) {
      --value_;
      post_wait();
    } else {
      auto const protect_post_wait = !is_empty_callable(post_wait);
      Waiter w;
      waiters_.push_back(w);
      w.protect_waiter_post_wait = protect_post_wait;
      w.wake_waiter.wait(lock);
      auto guard = makeGuard([&] {
        if (protect_post_wait) {
          w.wake_poster->post();
          w.wake_waiter.wait(lock);
        }
      });
      post_wait();
    }
  }

  void wait() { wait(EmptyCallable{}, EmptyCallable{}); }

  template <typename PrePost>
  void post(PrePost pre_post) {
    std::unique_lock lock{mutex_};
    if (value_ == -size_t(1)) {
      throw_exception<std::logic_error>("overflow");
    }
    pre_post();
    if (waiters_.empty()) {
      ++value_;
    } else {
      auto& w = pull();
      waiters_.erase(waiters_.iterator_to(w));
      auto const protect_waiter_post_wait = w.protect_waiter_post_wait;
      Optional<Event> wake_poster;
      if (protect_waiter_post_wait) {
        wake_poster.emplace();
        w.wake_poster = &*wake_poster;
      }
      w.wake_waiter.post();
      if (protect_waiter_post_wait) {
        wake_poster->wait(lock);
        w.wake_waiter.post();
      }
    }
  }

  void post() {
    post([] {});
  }

 private:
  class Event {
   public:
    void post() {
      signaled = true;
      cv.notify_one();
    }
    void wait(std::unique_lock<std::mutex>& lock) {
      cv.wait(lock, [&] { return signaled; });
      signaled = false;
    }

   private:
    bool signaled = false;
    std::condition_variable cv;
  };

  struct Waiter {
    SafeIntrusiveListHook hook;
    bool protect_waiter_post_wait = false;
    Event wake_waiter;
    Event* wake_poster = nullptr;
  };
  using WaiterList = SafeIntrusiveList<Waiter, &Waiter::hook>;

  class EmptyCallable {
   public:
    constexpr void operator()() const noexcept {}
  };

  template <typename Callable>
  std::false_type is_empty_callable(Callable const&) {
    return {};
  }
  std::true_type is_empty_callable(EmptyCallable const&) { return {}; }

  Waiter& pull() {
    switch (WakePolicy) {
      case SemaphoreWakePolicy::Fifo:
        return waiters_.front();
      case SemaphoreWakePolicy::Lifo:
        return waiters_.back();
    }
    terminate_with<std::invalid_argument>("wake-policy");
  }

  std::size_t value_ = 0;
  std::mutex mutex_;
  WaiterList waiters_;
};

using FifoSemaphore = PolicySemaphore<SemaphoreWakePolicy::Fifo>;
using LifoSemaphore = PolicySemaphore<SemaphoreWakePolicy::Lifo>;

} // namespace test
} // namespace folly
