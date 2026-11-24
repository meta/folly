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

#include <folly/coro/safe/detail/AsyncClosure.h>

#if FOLLY_HAS_COROUTINES

namespace folly::coro {

/// Learn more about `coro/safe` tools by browsing `docs/`.  Start with
/// `README.md` and `AsyncClosure.md`.  Here's a tl;dr for `AsyncClosure.h`.
///
/// Use `async_closure()` / `async_now_closure()` only when `now_task` is not
/// enough.  For your effort, you get (1) guaranteed, exception-safe async
/// RAII, and (2) the resulting coro is an automatically-measured movable
/// `safe_task`, which improves lifetime safety (`LifetimeSafetyBenefits.md`).
///
/// Control flow matches a regular `now_task` lazy-start coro:
///   - All "argument-binding" and "coro creation" work is eager.
///   - Your inner task & subsequent cleanup can only run when awaited.
///
/// `async_closure(bind::args{...}, taskFn)` wraps an outer task around yours,
/// unless elided via an automatic optimization.  The outer task owns special
/// "capture" args passed to the closure, ensuring they outlive the inner task.
///
/// Lifecycle contract:
///   - `bind::args{}` evaluate left-to-right (L2R) due to `{}`.
///   - Construction of `bind::capture_in_place` / `bind::in_place` args is also
///   L2R.
///   - When args are passed to the inner coro, copy/move order is unspecified.
///   - Upon awaiting the inner coro, `setParentCancelToken()` is called on the
///     capture args in L2R order.
///   - After the inner coro exits, arg `co_cleanup()` is executed in R2L order.
///   - When the outer coro exits, captures are destroyed in R2L order.
///
/// The `co_cleanup()` and `setParentCancelToken()` protocols support capture
/// types like `safe_async_scope` and `BackgroundTask`, which give the user
/// guaranteed, exception-safe async cleanup. Before building custom async
/// RAII, carefully read `CoCleanupAsyncRAII.md`.
///
/// The `async_closure_make_outer_coro` machinery is reused by `AsyncObject`,
/// which implements a similar "async RAII" contract for object scopes.
///
/// The difference between `async_closure()` and `async_now_closure()` is that
/// the former measures argument & inner coro safety, and makes a `safe_task`,
/// while the latter has no safety checks, and makes a `now_task`.  Both make it
/// easy to write lifetime-safe code.

struct async_closure_config {
  /// POWER USERS ONLY: For efficiency, `async_closure` will elide the outer
  /// coro if there are no `co_cleanup` captures.  In particular,
  /// `setParentCancelToken` isn't currently part of this detection, since we
  /// don't expect it to be used without `co_cleanup`.
  ///
  /// This optimization has some observable effects on the types seen by the
  /// closure, e.g.  `capture<int&>` becomes `capture<int>` since the inner
  /// coro now owns the `int` instead of just holding a reference.  However, to
  /// the extent possible, the before/after types "quack" the same.
  ///
  /// There are some edge-case scenarios where you may want to disable this
  /// optimization. An incomplete list:
  ///   - If you're passing many in-place, non-movable captures, the current
  ///     implementation will allocate each one on the heap, separately.
  ///     Setting `.force_outer_coro = true` will consolidate them into one
  ///     `unique_ptr<tuple<>>` owned by the outer coro.  If this scenario
  ///     proves perf-sensitive, we may add an automatic heuristic.
  ///   - This can be required to make a member function coro own its object.
  ///
  /// NB: Currently, if you set `.force_outer_coro = true`, but there are no
  /// captures to store, the outer coro will still be elided.
  bool force_outer_coro = false;
};

// Implementation note: None of the below functions can take `make_inner_coro`
// by-value, because stateful callables (lambdas with captures) are allowed
// here -- even in `async_closure()` if it's a coroutine wrapper.  A callable
// passed by-value would be destroyed before it can be awaited, causing a
// stack-use-after-return error.

namespace detail {
template <bool ForceOuterCoro, bool EmitNowTask>
// OK to take `bind::args` by-ref since the porcelain functions take it by-value
auto async_closure_impl(auto&& bargs, auto&& make_inner_coro) {
  constexpr detail::async_closure_bindings_cfg Cfg{
      .force_outer_coro = ForceOuterCoro,
      .emit_now_task = EmitNowTask,
      .is_invoke_member = is_instantiation_of_v<
          invoke_member_wrapper_fn,
          std::remove_reference_t<decltype(make_inner_coro)>>};
  return detail::bind_captures_to_closure<Cfg>(
      static_cast<decltype(make_inner_coro)>(make_inner_coro),
      detail::async_closure_safeties_and_bindings<Cfg>(
          static_cast<decltype(bargs)>(bargs)));
}
} // namespace detail

// Makes a `safe_task` whose safety is determined by the supplied arguments.
// `safe_task` requires that (1) the inner coroutine must not take arguments
// by-reference, and (2) must have a `maybe_value`-safe return type.
//
// Caveat: When `make_inner_coro` is a coroutine wrapper, that part is
// evaluated synchronously, and is not subject to either (1) or (2).
//
// Coro creation, argument storage, and in-place construction are also
// synchronous, as is the movement of the args into the task coroutine.
//
// The first argument should be `bind::args{...}`.  For single-argument
// closures, you can omit the `bind::args` if you're passing `bind::capture()`,
// `bind::capture_in_place<>()`, or another `bind::ext::like_args` item.
//
// Async RAII: Awaiting the task ensures `co_cleanup(async_closure_private_t)`
// is awaited for each of the `capture` arguments that defines it.
//
// Awaiting the task also forwards its ambient cancellation token to the
// captures that have a `setParentCancelToken()` member.  WARNING: If you want
// a type to define that, WITHOUT implementing `co_cleanup()`, then read the
// `force_outer_coro` doc above -- you'll have to add a bit of logic to
// `capture_needs_outer_coro()`.
template <async_closure_config Cfg = async_closure_config{}>
auto async_closure(auto bargs, auto&& make_inner_coro) {
  return folly::coro::detail::
      async_closure_impl<Cfg.force_outer_coro, /*EmitNowTask*/ false>(
             std::move(bargs),
             static_cast<decltype(make_inner_coro)>(make_inner_coro))
          .release_outer_coro();
}

// Like `async_closure` -- same argument binding semantics, same `co_cleanup`
// async RAII, and cancellation support, but returns a non-movable `now_task`
// without the lifetime safety enforcement:
//   - `make_inner_coro` may return a `now_task`, plain `Task`, or `safe_task`.
//   - It can take arguments by ref, you can pass raw pointers, etc.
//   - There are no checks on the `co_return` type.
//
// Requiring the task to be immediately awaited prevents a lot of common
// lifetime bugs.  If you cannot immediately await the task, then you should
// review `LifetimSafetyBenefits.md` and use the `safe_task`-enabled
// `async_closure()`, which is movable and schedulable on `safe_async_scope`.
//
// BEWARE: Returning `now_task` doesn't prevent egregious bugs like returning
// a pointer to a local.  Instead, make sure to configure your compiler to
// error on simple, non-async lifetime bugs (e.g. `-Wdangling -Werror`).
template <async_closure_config Cfg = async_closure_config{}>
auto async_now_closure(auto bargs, auto&& make_inner_coro) {
  return folly::coro::detail::
      async_closure_impl<Cfg.force_outer_coro, /*EmitNowTask*/ true>(
          std::move(bargs),
          static_cast<decltype(make_inner_coro)>(make_inner_coro));
}

} // namespace folly::coro

#endif
