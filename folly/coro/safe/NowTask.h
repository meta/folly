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

#include <folly/coro/TaskWrapper.h>
#include <folly/lang/SafeAlias-fwd.h>

#if FOLLY_HAS_COROUTINES

/// `now_task<T>` quacks like `Task<T>` but is immovable, and must be
/// `co_await`ed in the same expression that created it.
///
/// Using `now_task` by default brings considerable safety benefits.  With
/// `Task`, the following would be anti-patterns that cause dangling reference
/// bugs, but with `now_task`, C++ lifetime extension rules ensure that they
/// simply work.
///   - Pass-by-reference into coroutines.
///   - Ephemeral coro lambdas with captures.
///   - Coro lambdas with capture-by-reference.
///
/// Q: Is it `now_task` or `NowTask`?
///
/// A: In order to ease adoption, both forms will compile, but `now_task` is
///    primary.  Both are intended to eventually be renamed to simply
///    `folly::coro::task`, while the current movable, delayed-awaitable task
///    will be renamed to `unsafe_task`, and largely superseded by
///    `async_closure` and various `safe_task` flavors.
///
/// Notes:
///   - (subject to change) Unlike `safe_task`, `now_task` does NOT check
///     `safe_alias_of` for the return type `T`.  `now_task` is essentially an
///     immediate async function -- it satisfies the structured concurrency
///     maxim of "lexical scope drives both control flow & lifetime".  That
///     lowers the odds that returned pointers/references are unexpectedly
///     invalid.  The one failure mode I can think of is that the
///     pointed-to-data gets invalidated by a concurrent thread of execution,
///     but in that case the program almost certainly has a data race --
///     regardless of the lifetime bug -- and that requires runtime
///     instrumentation (like TSAN) to detect in present-day C++.

namespace folly::coro {

template <safe_alias, typename>
class BackgroundTask;

template <typename T = void>
class now_task;

template <typename T = void>
class now_task_with_executor;

// Backwards-compatibility shims
template <typename T = void>
using NowTask = now_task<T>;
template <typename T = void>
using NowTaskWithExecutor = now_task_with_executor<T>;

namespace detail {
template <typename T>
struct now_task_with_executor_cfg : DoesNotWrapAwaitable {
  using InnerTaskWithExecutorT = TaskWithExecutor<T>;
  using WrapperTaskT = now_task<T>;
};
template <typename T>
using now_task_with_executor_base =
    ext::wrap_must_use_immediately_t<TaskWithExecutorWrapperCrtp<
        now_task_with_executor<T>,
        detail::now_task_with_executor_cfg<T>>>;
} // namespace detail

template <typename T>
class [[nodiscard]]
now_task_with_executor final : public detail::now_task_with_executor_base<T> {
 protected:
  using detail::now_task_with_executor_base<T>::now_task_with_executor_base;

  template <safe_alias, typename>
  friend class BackgroundTask; // for `unwrapTaskWithExecutor`, remove later
 public:
  [[deprecated(
      "`as_unsafe()` is provided as an escape hatch for interoperating with "
      "older futures-based code, or other places not yet compatible with "
      "true structured concurrency patterns. Beware, the full `Task` API "
      "abounds with footguns like `start()` and `semi()` -- including UB, "
      "leaks, and lost errors. See `folly/coro/safe/docs/AsUnsafe.md` for "
      " safe migration patterns.")]]
  TaskWithExecutor<T> as_unsafe() && {
    return std::move(*this).unwrapTaskWithExecutor();
  }
};

namespace detail {
template <typename T>
class now_task_promise final
    : public TaskPromiseWrapper<T, now_task<T>, TaskPromise<T>> {};
template <typename T>
struct now_task_cfg : DoesNotWrapAwaitable {
  using ValueT = T;
  using InnerTaskT = Task<T>;
  using TaskWithExecutorT = now_task_with_executor<T>;
  using PromiseT = now_task_promise<T>;
};
template <typename T>
using now_task_base = ext::wrap_must_use_immediately_t<
    TaskWrapperCrtp<now_task<T>, detail::now_task_cfg<T>>>;
} // namespace detail

template <safe_alias, typename>
class safe_task;

template <safe_alias S, typename U>
auto to_now_task(safe_task<S, U> t);

template <typename T>
class FOLLY_CORO_TASK_ATTRS now_task final : public detail::now_task_base<T> {
 protected:
  using detail::now_task_base<T>::now_task_base;

  template <typename U> // can construct
  friend auto to_now_task(Task<U>);
  template <safe_alias S, typename U> // can construct
  friend auto to_now_task(safe_task<S, U>);
  template <typename U> // can construct & `unwrapTask`
  friend auto to_now_task(now_task<U>);

 public:
  [[deprecated(
      "`as_unsafe()` is provided as an escape hatch for interoperating with "
      "older futures-based code, or other places not yet compatible with "
      "true structured concurrency patterns. Beware, the full `Task` API "
      "abounds with footguns like `start()` and `semi()` -- including UB, "
      "leaks, and lost errors. See `folly/coro/safe/docs/AsUnsafe.md` for "
      " safe migration patterns.")]]
  Task<T> as_unsafe() && {
    return std::move(*this).unwrapTask();
  }
};

// NB: `to_now_task(safe_task)` is in `SafeTask.h` to avoid circular deps.
template <typename T>
auto to_now_task(Task<T> t) {
  return now_task<T>{std::move(t)};
}
template <typename T>
auto to_now_task(now_task<T> t) {
  return now_task<T>{std::move(t).unwrapTask()};
}

// Apparently, Clang 15 has a bug in prvalue semantics support, so it cannot
// return immovable coroutines.
#if !defined(__clang__) || __clang_major__ > 15

/// Make a `now_task` that trivially returns a value.
template <class T>
now_task<T> make_now_task(T t) {
  co_return t;
}

/// Make a `now_task` that trivially returns no value
inline now_task<> make_now_task() {
  co_return;
}
/// Same as make_now_task(). See Unit
inline now_task<> make_now_task(Unit /*unused*/) {
  co_return;
}

/// Make a `now_task` that will trivially yield an exception.
template <class T>
now_task<T> make_error_now_task(exception_wrapper ew) {
  co_yield co_error(std::move(ew));
}

#endif // no `make_now_task` on old/buggy clang

} // namespace folly::coro

#endif
