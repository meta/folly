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

#include <fmt/core.h>

#include <folly/Likely.h>
#include <folly/Portability.h>
#include <folly/python/ScopedGILRelease.h>
#include <folly/python/error.h>

#if defined(__APPLE__)
#include <TargetConditionals.h> // @manual
#endif

#if !defined(__APPLE__) || !(TARGET_OS_SIMULATOR || TARGET_OS_IPHONE)
#ifndef PY_VERSION_HEX
#error \
    "Python.h must be included before folly/python/import.h checks Py_GIL_DISABLED"
#endif
#endif

#ifdef Py_GIL_DISABLED
#include <chrono>
#include <condition_variable>

#include <folly/synchronization/AtomicNotification.h>
#endif

namespace folly {
namespace python {
#ifdef Py_GIL_DISABLED
namespace detail {

using ImportCacheThreadIdent = unsigned long;

// PyThread_get_thread_ident does not require an attached Python thread state.
// CPython thread identifiers are nonzero, leaving zero as the unlocked state.
inline constexpr ImportCacheThreadIdent kImportCacheUnlocked = 0;

class import_cache_call_guard {
 public:
  explicit import_cache_call_guard(std::atomic<ImportCacheThreadIdent>& owner)
      : owner_{owner} {
    auto const threadIdent = PyThread_get_thread_ident();

    auto expected = kImportCacheUnlocked;
    if (owner_.compare_exchange_strong(
            expected,
            threadIdent,
            std::memory_order_acquire,
            std::memory_order_relaxed)) {
      owns_ = true;
      return;
    }

    // On CAS failure, expected now holds the current owner. Matching the
    // current thread means same-thread recursion, which must not wait.
    if (expected == threadIdent) {
      return;
    }

    // Bounded: this guard is invisible to CPython's import deadlock detector,
    // so waiting forever on a cycle would hang with no diagnostic. Giving up
    // falls back to the at-least-once behavior this cache already allows for
    // reentrant imports.
    auto const deadline = std::chrono::steady_clock::now() + kMaxWait;
    // Detach while parked so free-threaded stop-the-world can reach a
    // safepoint.
    ScopedGILRelease release;
    do {
      if (folly::atomic_wait_until(&owner_, expected, deadline) ==
          std::cv_status::timeout) {
        return;
      }
      expected = kImportCacheUnlocked;
    } while (!owner_.compare_exchange_strong(
        expected,
        threadIdent,
        std::memory_order_acquire,
        std::memory_order_relaxed));

    owns_ = true;
  }

  ~import_cache_call_guard() {
    if (owns_) {
      owner_.store(kImportCacheUnlocked, std::memory_order_release);
      folly::atomic_notify_all(&owner_);
    }
  }

  import_cache_call_guard(import_cache_call_guard const&) = delete;
  import_cache_call_guard& operator=(import_cache_call_guard const&) = delete;

  bool owns() const { return owns_; }

 private:
  static constexpr std::chrono::seconds kMaxWait{60};

  std::atomic<ImportCacheThreadIdent>& owner_;
  bool owns_{false};
};

} // namespace detail
#endif

/**
 * Call-once like abstraction for cython-api module imports.
 *
 * On failure, returns false and keeps python error indicator set (caller must
 * handle it).
 */
class import_cache_nocapture {
 public:
  using sig = int();

  consteval import_cache_nocapture(sig& fun) noexcept : fun_{fun} {}

  bool operator()() const {
    // protecting this with the cxxabi mutex (the mutex that
    // guards static local variables) leads to deadlock with
    // cpython's gil; at-least-once semantics is fine here
    auto const val = fun_.load(std::memory_order_acquire);
    return FOLLY_LIKELY(!val) ? true : call_slow();
  }

 private:
  FOLLY_NOINLINE bool call_slow() const {
#ifdef Py_GIL_DISABLED
    detail::import_cache_call_guard guard{owner_};
    if (!guard.owns()) {
      // Re-entered, or gave up waiting: another import is still in flight, so
      // import again but leave publishing completion to the owner.
      auto const val = fun_.load(std::memory_order_acquire);
      return !val || 0 == val();
    }
#endif

    auto const val = fun_.load(std::memory_order_acquire);
    if (val && 0 != val()) {
      return false;
    } else {
      fun_.store(nullptr, std::memory_order_release);
      return true;
    }
  }

 private:
  mutable std::atomic<sig*> fun_; // if nullptr, already called
#ifdef Py_GIL_DISABLED
  mutable std::atomic<detail::ImportCacheThreadIdent> owner_{
      detail::kImportCacheUnlocked};
#endif
};

/**
 * On failure, captures python exception (clearing error indicator) and throws a
 * wrapper C++ exception
 */
class import_cache {
 public:
  consteval import_cache(
      import_cache_nocapture::sig& fun, char const* const name) noexcept
      : impl_{fun}, name_{name ? name : "<unknown>"} {}

  void operator()() const {
    if (!impl_()) {
      handlePythonError(fmt::format("import {} failed: ", name_));
    }
  }

 private:
  import_cache_nocapture impl_;
  char const* const name_; // for handling import errors
};

} // namespace python
} // namespace folly
