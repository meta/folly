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

//
// Docs: https://fburl.com/fbcref_callonce
//

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>

#include <folly/Likely.h>
#include <folly/Portability.h>
#include <folly/ScopeGuard.h>
#include <folly/functional/Invoke.h>
#include <folly/synchronization/AtomicNotification.h>

namespace folly {

/**
 * The flag template to be used with call_once. Parameterizable by the mutex
 * type and atomic template. The mutex type is required to mimic std::mutex and
 * the atomic type is required to mimic std::atomic.
 */
template <typename Mutex, template <typename> class Atom = std::atomic>
class basic_once_flag;

/**
 * An alternative flag that can be used with call_once that uses only 1 byte.
 * Uses a 3-state std::atomic<uint8_t> with wait/notify for synchronization.
 */
class compact_once_flag;

/**
 * Drop-in replacement for std::call_once.
 *
 * The libstdc++ implementation has two flaws:
 * * it lacks a fast path, and
 * * it deadlocks (in explicit violation of the standard) when invoked twice
 *   with a given flag, and the callable passed to the first invocation throws.
 *
 * This implementation corrects both flaws, and is smaller at 1 byte vs 4 bytes
 * with libstdc++ on Linux/x64.
 *
 * Does not work with std::once_flag.
 *
 * mimic: std::call_once
 */
template <typename OnceFlag, typename F, typename... Args>
FOLLY_ALWAYS_INLINE void call_once(OnceFlag& flag, F&& f, Args&&... args) {
  if (flag.test_once()) [[likely]] {
    return;
  }
  flag.call_once_slow(std::forward<F>(f), std::forward<Args>(args)...);
}

/**
 * Like call_once, but using a boolean return type to signal pass/fail rather
 * than throwing exceptions.
 *
 * Returns true if any previous call to try_call_once with the same once_flag
 * has returned true or if any previous call to call_once with the same
 * once_flag has completed without throwing an exception or if the function
 * passed as an argument returns true; otherwise returns false.
 *
 * Note: This has no parallel in the std::once_flag interface.
 */
template <typename OnceFlag, typename F, typename... Args>
[[nodiscard]] FOLLY_ALWAYS_INLINE bool try_call_once(
    OnceFlag& flag, F&& f, Args&&... args) noexcept {
  static_assert(is_nothrow_invocable_v<F, Args...>, "must be noexcept");
  if (flag.test_once()) [[likely]] {
    return true;
  }
  return flag.try_call_once_slow(
      std::forward<F>(f), std::forward<Args>(args)...);
}

/**
 * Tests whether any invocation to call_once with the given flag has succeeded.
 *
 * May help with space usage in certain esoteric scenarios compared with caller
 * code tracking a separate and possibly-padded bool.
 *
 * Note: This has no parallel in the std::once_flag interface.
 */
template <typename OnceFlag>
FOLLY_ALWAYS_INLINE bool test_once(OnceFlag const& flag) noexcept {
  return flag.test_once();
}

/**
 * Resets a flag.
 *
 * Warning: unsafe to call concurrently with any other flag operations.
 */
template <typename OnceFlag>
FOLLY_ALWAYS_INLINE void reset_once(OnceFlag& flag) noexcept {
  return flag.reset_once();
}

/**
 * Sets a flag, as if call_once(flag, [] {}) was called.
 *
 * Warning: unsafe to call concurrently with any other flag operations.
 */
template <typename OnceFlag>
FOLLY_ALWAYS_INLINE void set_once(OnceFlag& flag) noexcept {
  return flag.set_once();
}

template <typename Mutex, template <typename> class Atom>
class basic_once_flag {
 public:
  constexpr basic_once_flag() noexcept = default;
  basic_once_flag(const basic_once_flag&) = delete;
  basic_once_flag& operator=(const basic_once_flag&) = delete;

 private:
  template <typename OnceFlag, typename F, typename... Args>
  friend void call_once(OnceFlag&, F&&, Args&&...);

  template <typename OnceFlag>
  friend bool test_once(OnceFlag const& flag) noexcept;

  template <typename OnceFlag>
  friend void reset_once(OnceFlag&) noexcept;

  template <typename OnceFlag>
  friend void set_once(OnceFlag&) noexcept;

  template <typename F, typename... Args>
  FOLLY_NOINLINE void call_once_slow(F&& f, Args&&... args) {
    std::lock_guard lock(mutex_);
    if (called_.load(std::memory_order_relaxed)) {
      return;
    }
    std::invoke(std::forward<F>(f), std::forward<Args>(args)...);
    called_.store(true, std::memory_order_release);
  }

  template <typename OnceFlag, typename F, typename... Args>
  friend bool try_call_once(OnceFlag&, F&&, Args&&...) noexcept;

  template <typename F, typename... Args>
  FOLLY_NOINLINE bool try_call_once_slow(F&& f, Args&&... args) noexcept {
    std::lock_guard lock(mutex_);
    if (called_.load(std::memory_order_relaxed)) {
      return true;
    }
    auto const pass =
        std::invoke(std::forward<F>(f), std::forward<Args>(args)...);
    called_.store(pass, std::memory_order_release);
    return pass;
  }

  FOLLY_ALWAYS_INLINE bool test_once() const noexcept {
    return called_.load(std::memory_order_acquire);
  }

  FOLLY_ALWAYS_INLINE void reset_once() noexcept {
    called_.store(false, std::memory_order_relaxed);
  }

  FOLLY_ALWAYS_INLINE void set_once() noexcept {
    called_.store(true, std::memory_order_relaxed);
  }

  Atom<bool> called_{false};
  Mutex mutex_;
};

class compact_once_flag {
 public:
  constexpr compact_once_flag() noexcept = default;
  compact_once_flag(const compact_once_flag&) = delete;
  compact_once_flag& operator=(const compact_once_flag&) = delete;

 private:
  // kDone is 0 so test_once() compiles to test+jnz on x86.
  static constexpr uint8_t kDone = 0;
  static constexpr uint8_t kInit = 1;
  static constexpr uint8_t kActive = 2;

  template <typename OnceFlag, typename F, typename... Args>
  friend void call_once(OnceFlag&, F&&, Args&&...);

  template <typename OnceFlag>
  friend bool test_once(OnceFlag const& flag) noexcept;

  template <typename OnceFlag>
  friend void reset_once(OnceFlag&) noexcept;

  template <typename OnceFlag>
  friend void set_once(OnceFlag&) noexcept;

  template <typename F>
  FOLLY_ALWAYS_INLINE bool call_once_impl(F&& f) {
    while (true) {
      uint8_t expected = kInit;
      if (state_.compare_exchange_weak(
              expected, kActive, std::memory_order_acquire)) {
        bool pass = false;
        SCOPE_EXIT {
          state_.store(pass ? kDone : kInit, std::memory_order_release);
          folly::atomic_notify_all(&state_);
        };
        pass = f();
        return pass;
      }
      if (expected == kDone) {
        return true;
      }
      folly::atomic_wait(&state_, kActive);
    }
  }

  template <typename F, typename... Args>
  FOLLY_NOINLINE void call_once_slow(F&& f, Args&&... args) {
    call_once_impl([&]() -> bool {
      std::invoke(std::forward<F>(f), std::forward<Args>(args)...);
      return true;
    });
  }

  template <typename OnceFlag, typename F, typename... Args>
  friend bool try_call_once(OnceFlag&, F&&, Args&&...) noexcept;

  template <typename F, typename... Args>
  FOLLY_NOINLINE bool try_call_once_slow(F&& f, Args&&... args) noexcept {
    return call_once_impl([&]() noexcept -> bool {
      return static_cast<bool>(
          std::invoke(std::forward<F>(f), std::forward<Args>(args)...));
    });
  }

  FOLLY_ALWAYS_INLINE bool test_once() const noexcept {
    return state_.load(std::memory_order_acquire) == kDone;
  }

  FOLLY_ALWAYS_INLINE void reset_once() noexcept {
    state_.store(kInit, std::memory_order_relaxed);
  }

  FOLLY_ALWAYS_INLINE void set_once() noexcept {
    state_.store(kDone, std::memory_order_relaxed);
  }

  std::atomic<uint8_t> state_{kInit};
};

static_assert(
    sizeof(compact_once_flag) == 1, "compact_once_flag should be 1 byte");

/**
 * The flag type to be used with call_once.
 *
 * Does not work with std::call_once.
 *
 * mimic: std::once_flag
 */
using once_flag = compact_once_flag;

} // namespace folly
