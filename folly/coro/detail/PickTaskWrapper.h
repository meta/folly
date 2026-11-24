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

#include <folly/Portability.h>
#include <folly/lang/SafeAlias-fwd.h>

/// For functions-of-coros, like `timeout()` or `collectAll()`, we want the
/// outer coro to be able to pass through these attributes of the inner coro:
///  - `folly::ext::must_use_immediately_v`
///  - `value_only_awaitable_v`
///  - `strict_safe_alias_of_v` / `lenient_safe_alias_of_v`
///
/// Variation along these dimensions is currently implemented as a zoo of coro
/// templates and wrappers -- `Task` aka `UnsafeMovableTask`, `now_task`,
/// `safe_task`, `value_or_fatal<InnerTask>`.  The type function
/// `pick_task_wrapper` provides common logic for picking a task type with the
/// given attributes.

#if FOLLY_HAS_COROUTINES

namespace folly::coro {

template <typename T>
class Task;
template <typename T>
class TaskWithExecutor;

template <safe_alias, typename>
class safe_task;
template <safe_alias, typename>
class safe_task_with_executor;

template <typename T>
class now_task;
template <typename T>
class now_task_with_executor;

template <typename, auto>
class value_or_fatal;

namespace detail {

struct identity_metafunction {
  template <typename T>
  using apply = T;
};

template <safe_alias, bool /*must use immediately (now)*/>
struct pick_task_wrapper_impl;

template <>
struct pick_task_wrapper_impl<safe_alias::unsafe, /*await now*/ false> {
  template <typename T>
  using Task = Task<T>;
  template <typename T>
  using TaskWithExecutor = TaskWithExecutor<T>;
};

template <>
struct pick_task_wrapper_impl<safe_alias::unsafe, /*await now*/ true> {
  template <typename T>
  using Task = now_task<T>;
  template <typename T>
  using TaskWithExecutor = now_task_with_executor<T>;
};

// These `safe_task` types are immovable, so "await now" doesn't matter.
template <safe_alias Safety, bool AwaitNow>
  requires(Safety < safe_alias::closure_min_arg_safety)
struct pick_task_wrapper_impl<Safety, AwaitNow> {
  template <typename T>
  using Task = safe_task<Safety, T>;
  template <typename T>
  using TaskWithExecutor = safe_task_with_executor<Safety, T>;
};

template <safe_alias Safety>
  requires(Safety >= safe_alias::closure_min_arg_safety)
// Future: There is no principled reason we can't have must-use-immediately
// `safe_task`s with these higher safety levels, but supporting that cleanly
// would require reorganizing the `folly/coro` task-wrapper implementations. Two
// possible approaches are:
//  - `now_task<T> = await_now<Task<T>>`
//  - Roll up `now_task` and `safe_task` into `basic_task<T, Cfg>` or similar,
//    where `Cfg` captures both safety & immediate-awaitability.
struct pick_task_wrapper_impl<Safety, /*await now*/ false> {
  template <typename T>
  using Task = safe_task<Safety, T>;
  template <typename T>
  using TaskWithExecutor = safe_task_with_executor<Safety, T>;
};

// Pass this as `AddWrapperMetaFn` to `pick_task_wrapper` to add
// `value_or_fatal`.
template <auto CancelCfg>
struct value_or_fatal_with_cancel_cfg {
  template <typename T>
  using apply = value_or_fatal<T, CancelCfg>;
};

template <
    typename T,
    safe_alias Safety,
    bool MustUseImmediately,
    typename AddWrapperMetaFn = identity_metafunction>
using pick_task_wrapper = typename AddWrapperMetaFn::template apply<
    typename pick_task_wrapper_impl<Safety, MustUseImmediately>::template Task<
        T>>;

template <
    typename T,
    safe_alias Safety,
    bool MustUseImmediately,
    typename AddWrapperMetaFn = identity_metafunction>
using pick_task_with_executor_wrapper =
    typename AddWrapperMetaFn::template apply<typename pick_task_wrapper_impl<
        Safety,
        MustUseImmediately>::template TaskWithExecutor<T>>;

} // namespace detail
} // namespace folly::coro

#endif
