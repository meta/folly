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

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include <folly/CPortability.h>
#include <folly/Likely.h>
#include <folly/lang/Exception.h>
#include <folly/synchronization/SaturatingSemaphore.h>

namespace folly {

/// Similar to std::latch (C++20) but with timed waits.
///
/// The latch class is a downward counter which can be used to synchronize
/// threads. The value of the counter is initialized on creation. Threads may
/// block on the latch until the counter is decremented to zero. There is no
/// possibility to increase or reset the counter, which makes the latch a
/// single-use barrier.
//
/// Example:
///
///     const int N = 32;
///     folly::Latch latch(N);
///     std::vector<std::thread> threads;
///     for (int i = 0; i < N; i++) {
///       threads.emplace_back([&] {
///         do_some_work();
///         latch.count_down();
///       });
///     }
///     latch.wait();
///
/// A latch can be used to easily wait for mocked async methods in tests:
///
///     ACTION_P(DecrementLatchImpl, latch) {
///       latch.count_down();
///     }
///     constexpr auto DecrementLatch = DecrementLatchImpl<folly::Latch&>;
///
///     class MockableObject {
///      public:
///       MOCK_METHOD(void, someAsyncEvent, ());
///     };
///
///     TEST(TestSuite, TestFeature) {
///       MockableObject mockObjA;
///       MockableObject mockObjB;
///
///       folly::Latch latch(5);
///
///       EXPECT_CALL(mockObjA, someAsyncEvent())
///           .Times(2)
///           .WillRepeatedly(DecrementLatch(latch)); // called 2 times
///
///       EXPECT_CALL(mockObjB, someAsyncEvent())
///           .Times(3)
///           .WillRepeatedly(DecrementLatch(latch)); // called 3 times
///
///       // trigger async events
///       // ...
///
///       EXPECT_TRUE(latch.try_wait_for(std::chrono::seconds(60)));
///     }

class Latch {
 public:
  /// The maximum value of counter supported by this implementation
  static constexpr ptrdiff_t max() noexcept {
    return std::numeric_limits<int32_t>::max();
  }

  /// Constructs a latch and initializes its internal counter.
  constexpr explicit Latch(ptrdiff_t expected) : count_(expected) {
    terminate_if(expected < 0 || expected > max());
    if (expected == 0) {
      semaphore_.post();
    }
  }

  /// Atomically decrements the internal counter by `n` without blocking the
  /// caller.
  FOLLY_ALWAYS_INLINE void count_down(ptrdiff_t n = 1) noexcept {
    terminate_if(n < 0 || n > max());
    if (n) [[likely]] {
      const auto count = count_.fetch_sub(n, std::memory_order_acq_rel);
      terminate_if(count < n);
      if (count == n) [[unlikely]] {
        semaphore_.post();
      }
    }
  }

  /// Returns true only if the internal counter has reached zero. The function
  /// does not block.
  FOLLY_ALWAYS_INLINE bool try_wait() noexcept { return semaphore_.try_wait(); }

  /// Wait until the internal counter reaches zero, or until the given `timeout`
  /// expires. Returns true if the internal counter reached zero before the
  /// period expired, otherwise false.
  template <typename Rep, typename Period>
  FOLLY_ALWAYS_INLINE bool try_wait_for(
      const std::chrono::duration<Rep, Period>& timeout) noexcept {
    return semaphore_.try_wait_for(timeout);
  }

  /// Wait until the internal counter reaches zero, or until the given
  /// `deadline` expires. Returns true if the internal counter reached zero
  /// before the deadline expired, otherwise false.
  template <typename Clock, typename Duration>
  FOLLY_ALWAYS_INLINE bool try_wait_until(
      const std::chrono::time_point<Clock, Duration>& deadline) noexcept {
    return semaphore_.try_wait_until(deadline);
  }

  /// Equivalent to try_wait(), but available on const receivers.
  FOLLY_ALWAYS_INLINE bool ready() const noexcept { return semaphore_.ready(); }

  /// Wait until the internal counter reaches zero, indefinitely.
  FOLLY_ALWAYS_INLINE void wait() noexcept { semaphore_.wait(); }

  /// Atomically decrement the internal counter by `n` and wait until the
  /// internal counter reaches zero, indefinitely. Equivalent to `count_down()`
  /// followed by a `wait()`.
  FOLLY_ALWAYS_INLINE void arrive_and_wait(ptrdiff_t n = 1) noexcept {
    count_down(n);
    wait();
  }

 private:
  FOLLY_ALWAYS_INLINE constexpr void terminate_if(bool cond) noexcept {
    if (cond) {
      folly::terminate_with<std::invalid_argument>(
          "argument outside expected range");
    }
  }

  std::atomic<int32_t> count_;
  SaturatingSemaphore<> semaphore_;
};

} // namespace folly
