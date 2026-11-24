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
#include <folly/coro/safe/NowTask.h>
#include <folly/lang/SafeAlias.h>

#if FOLLY_HAS_COROUTINES

namespace folly::coro {

/// Why is `SafeTask.h` useful?
///
/// `now_task` from `NowTask.h` should be your default.  This immovable task
/// should always be awaited in the full-expression that created it,
/// eliminating most classes of lifetime-safety bugs.
///
/// If you need a MOVABLE task with compile-time safety checks, read on.
///
/// Typically, you will not use `safe_task` directly.  Instead, choose one of
/// the type-aliases below, following `APIBestPractices.md` guidance.  Briefly:
///
///   `value_task`:
///   Use if your coro only takes value-semantic args.
///
///   `member_task`:
///   Use for non-static member functions.  Can be awaited immediately (like
///   `now_task`), or wrapped in an `async_closure` to support less-structured
///   concurrency -- including scheduling on a background scope belonging to
///   the object.  IMPORTANT: adding a `member_task` to a class requires it to
///   have an **explicit** `folly/lang/SafeAlias.h` annotation.
///
///   `closure_task`:
///   Use if your coro is called via `async_closure`.  Outside of closures,
///   this behaves like `now_task`.
///
///   `co_cleanup_safe_task`:
///   Use for tasks that can be directly scheduled on a `safe_async_scope`.
///
///   `auto_safe_task`:
///   Generic coros where you want the argument & return types to automatically
///   choose between a `now_task` and a `safe_task`.
///
/// `safe_task` is a thin wrapper around `folly::coro::Task` that uses
/// `safe_alias_of` to enforce some compile-time guarantees:
///   - The `safe_task` has `safe_alias_of` memory safety at least as high as
///     the coro's arguments.  In particular, no args are taken by reference.
///   - Regardless of the task's declared safety, the coro's return must
///     have safety `maybe_value` (explained in `safe_task_ret_and_args`).
///   - The coroutine is NOT a stateful callable -- this prohibits lambda
///     captures, since those are a very common cause of coro memory bugs.
template <safe_alias, typename = void>
class safe_task;
template <safe_alias, typename = void>
class safe_task_with_executor;

// A `safe_task` whose args and return type follow value semantics.
template <typename T = void>
using value_task = safe_task<safe_alias::maybe_value, T>;

// A `safe_task` that can be added to `safe_async_scope`, and may run during
// closure cleanup.  Its content must therefore be `co_cleanup_safe_ref`-safe.
template <typename T = void>
using co_cleanup_safe_task = safe_task<safe_alias::co_cleanup_safe_ref, T>;

// Use `closure_task` as the inner coro type for tasks meant to be wrapped in
// an `async_closure` or `async_now_closure`.  The closure code will measure
// the safety of its args, and return a correct `safe_task`.
//
// Without a surrounding closure call, a `closure_task` is immovable, and works
// like `now_task`, but with some restrictions on its arg & return types.
//
// Immovability rationale: `closure_task` is implemented as a `safe_task` for
// reasons explained in the next paragraph.  But, its safety contract is weaker
// than that of the usual closure (it can take `capture<Val>`, which should
// never be moved) -- immovability is meant to reduce the odds of misuse.
// Making it truly opaque / not semi-awaitable would be a stronger safeguard,
// but that requires extra complexity even just so that
// `value_or_fatal<closure_task<>> foo()` would compile.
//
// "closure_task is a safe_task" rationale: `async_closure` cannot emit a
// `safe_task` without the inner coro being a `safe_task` -- otherwise it could
// not guarantee that none of the args are taken by reference.  Conveniently,
// `safe_task` also checks the return type is safe, and the coro's callable is
// stateless, so `async_closure` can rely on those checks.
//
// This `closure_task` implementation uses a `safe_alias` level safer than
// `unsafe` to get all of the above `SafeAlias` checks.  The level also has
// to be less safe than `shared_cleanup` so we can treat these differently:
//  - `capture<Value>` (safety `unsafe_closure_internal`) should stay in the
//    original closure.  Users can move the content, but shouldn't move the
//    wrapper, since that messes with the safety system.
//  - `co_cleanup_capture<Value&>` refs (safety `shared_cleanup`) can safely
//    be moved or copied into other closures.
//  - By the way, `co_cleanup_capture<Value>` should never be moved from the
//    owning closure that's responsible for its cleanup.
template <typename T = void>
using closure_task = safe_task<safe_alias::unsafe_closure_internal, T>;

// A `member_task` is nearly identical to `closure_task`, except that it's
// usable with non-static member coroutines on **stateful** classes.
//   - In `async_closure`, it behaves like `closure_task`, with one
//     extra constraint -- its implicit object parameter must refer to a class
//     with an explicitly annotated `safe_alias_of` (see `SafeAlias.h`).
//   - Without `async_closure`, it is immediately-awaitable, like `now_task`.
//   - Since it is also a `safe_task`, it forbids `safe_alias::unsafe`
//     arguments, and unsafe return types.
//
// To construct `async_closure`s with `member_task`s (for background or scope
// tasks), use this special calling convention:
//
//   async_closure(bind::args{obj, args...}, FOLLY_INVOKE_MEMBER(memberFnName))
//
// This closure will safety-check the now-explicit object param, and produce a
// movable `safe_task` of the safety level determined by the arguments.  This
// integration lets us safely schedule member coros on `safe_async_scope`, pass
// `co_cleanup` args into such coros, etc.
//
// Design note: Fundamentally, the reason that we have `member_task`, and
// cannot just use `closure_task` for class members, is that C++20 does not let
// a coroutine function distinguish (i) an "implicit object param" from (ii) a
// regular lvalue reference.  For (i), `member_task` requires `async_closure`
// to invoke it with `FOLLY_INVOKE_MEMBER`, so in this case we just need the
// first arg to be a reference-to-a-safe-object.  In case (ii), such as
// `closure_task`, a reference-to-a-safe-object is definitely unsafe, whereas a
// reference to a stateless/empty object is safe enough.
template <typename T>
using member_task = safe_task<safe_alias::unsafe_member_internal, T>;

// NB: There are some `async_closure`-specific values of `safe_alias` that
// do not yet have a `safe_task` alias.  That's because they haven't come up
// in user-facing type signatures.

namespace detail {
template <typename T, safe_alias Safety>
using auto_safe_task_impl = std::conditional_t<
    // This checks both args & the return value because we want to avoid this
    // resolving to a `safe_task` that won't actually compile.
    (Safety >= safe_alias::closure_min_arg_safety &&
     lenient_safe_alias_of_v<T> >= safe_alias::maybe_value),
    safe_task<Safety, T>,
    now_task<T>>;
}

/// Coros declared as `safe_task<Safety, T>` will satisfy the strong
/// constraints above, or fail with a compile error.
///
/// The safety of a coroutine template may vary depending on the args or
/// return type, meaning that the user can't actually pick a fixed
/// safety level for their generic coro.
///
/// Instead, the generic coro can return `auto_safe_task<ReturnT,
/// SafetyArgs...>`, where `SafetyArgs` is (typically) the subset of the
/// coroutine's argument types that may affect safety.
///
/// `auto_safe_task` has a Significant Caveat -- you can't use it with
/// non-`static` member functions -- the implicit object parameter is unsafe
/// (as it should be).  And if you do use it, you will get a compile-time
/// error instead of a `now_task`, simply because this type-function has no
/// access to the callable.  See `APIBestPractices.md` for workarounds.
template <typename T, typename... SafetyArgs>
using auto_safe_task = detail::auto_safe_task_impl<
    T,
    // Same logic as `lenient_safe_alias_of_v`
    safe_alias_of_pack<safe_alias::maybe_value, SafetyArgs...>::value>;

namespace detail {

struct SafeTaskTest;

template <safe_alias ArgSafety, typename RetT, typename... Args>
concept safe_task_ret_and_args =
    ((lenient_safe_alias_of_v<Args> >= ArgSafety) && ...) &&
    // In the event that you need a child scope to return a reference to
    // something owned by a still-valid ancestor scope, we don't have a good
    // way to detect this automatically.  To work around, use a `manual_safe_*`
    // wrapper in `SafeAlias.h`, and comment why it is safe.
    (lenient_safe_alias_of_v<RetT> >= safe_alias::maybe_value);

template <typename T>
concept is_stateless_class_or_func =
    // `require_sizeof` avoids `is_empty` UB on incomplete types
    (require_sizeof<T> >= 0 && std::is_empty_v<T>) ||
    (std::is_pointer_v<T> && std::is_function_v<std::remove_pointer_t<T>>);

template <safe_alias, typename...>
inline constexpr bool is_safe_task_valid = false;
// Coros taking 0 args can't be methods (no implicit object parameter),
// so their safety is determined by the return type.
template <safe_alias ArgSafety, typename RetT>
inline constexpr bool is_safe_task_valid<ArgSafety, RetT> =
    safe_task_ret_and_args<ArgSafety, RetT>;
// Inspect the first argument, which can be an implicit object parameter, to
// allow stateless callables (like lambdas), but to prohibit stateful callables
// (these can contain unsafe aliasing in their state, which we can't inspect
// automatically).  If you need to make `safe_task`s from a stateful object,
// ideas include:
//   - Pass a `capture<YourClass&>` to a static member `YourClass::func`
//   - Explicitly mark `safe_alias_of<YourClass>` and use `member_task`.
//   - Future: Check out `AsyncObject.h`.
//
// How this works: With >= 1 args in the pack, the `First` argument
// **could** be an implicit object parameter.  We don't know if it is, but
// we do know that any such parameter has type lvalue reference, which means
// that it would fail `safe_task_ret_and_args<RetT, First, Args...>`.
//
// This test accepts any `First` that is an lref to a stateless class
// or function -- that is, it returns `true` if the first arg is either:
//   - not an lref, and has no unsafe aliasing (not an implicit object param)
//   - an lref to something stateless (MAY be an implicit object param)
// As a side effect, this allows coros without an implicit object param to
// pass a stateless class by reference, if it's the first param.  This
// should be harmless in practice.
//
// For `member_task`, `First` is assumed to be the implicit object parameter.
// We cannot deduce its safety, so we use `strict_safe_alias_of_v` to force the
// user to explicitly mark this class -- incidentally, this precludes
// `member_task` lambdas.  It is safe for `member_task` to treat
// a ref-to-a-safe-object as safe, since it is usable only:
//   - As a free coro, where it acts like a restricted `now_task`
//   - Via `async_closure`, which enforces a `FOLLY_INVOKE_MEMBER` calling
//     convention that either takes a safe capture-ref, or makes the closure
//     take ownership of the closure.
// We MUST NOT apply the same treatment to `closure_task`, since it cannot
// ensure that the ref itself is safe (even if the referenced type is).
template <safe_alias ArgSafety, typename RetT, typename First, typename... Args>
inline constexpr bool is_safe_task_valid<ArgSafety, RetT, First, Args...> =
    (std::is_lvalue_reference_v<First> &&
     ((ArgSafety == safe_alias::unsafe_member_internal &&
       strict_safe_alias_of_v<std::remove_reference_t<First>> >=
           // This `std::max` requires the implicit object parameter of
           // `member_task`s to have safety `> shared_cleanup`.  In effect,
           // this prohibits `member_task`s from being added to classes that
           // store `co_cleanup_capture` refs, or (equivalently) to
           // `async_object`s.  This tradeoff is required so that
           // `async_now_closure()` of `member_task`s is not ALWAYS forced to
           // apply shared-cleanup downgrades to its arguments.  The details
           // are in `async_closure_safeties_and_bindings`.  If you find
           // yourself blocked by this restriction, my best idea is:
           // (1) Add another `co_cleanup_member_task` (better name TBD), with
           //     a new safety level `< unsafe_closure_internal`.
           // (2) Scan all mentions of `unsafe_closure_internal` to make sure
           //     nothing assumes it is the lowest "safe" level. Importantly,
           //     the `bind_captures_to_closure` logic related to
           //     shared-cleanup in `async_now_closure` would continue to
           //     test for `unsafe_closure_internal`, NOT the new level.
           // (3) Special-case `co_cleanup_member_task` in this check to
           //     let its implicit object param be `<= shared_cleanup`.
           std::max(
               ArgSafety,
               safe_alias{
                   to_underlying(safe_alias::closure_min_arg_safety) + 1})) ||
      is_stateless_class_or_func<std::remove_reference_t<First>>))
    ? safe_task_ret_and_args<ArgSafety, RetT, Args...>
    : safe_task_ret_and_args<ArgSafety, RetT, First, Args...>;

template <safe_alias ArgSafety, typename T, typename... Args>
class safe_task_promise final
    : public TaskPromiseWrapper<
          T,
          safe_task<ArgSafety, T>,
          detail::TaskPromise<T>> {
  // "Unsafe" is not a "safe" task any more.  In the future, we could have
  // `safe_task<unsafe, T>` act as `now_task<T>`, but there's no present use
  // for this uniformity, but there are benefits to explicitness.
  static_assert(
      ArgSafety > safe_alias::unsafe,
      "Instead of making an unsafe `safe_task`, use a `now_task`, or "
      "`async_now_closure()`");

 public:
  // IMPORTANT: If you alter this arrangement, do the "Manual test" inside
  // `returnsVoid` in `SafeTaskTest.cpp`.
  //
  // This is a no-op wrapper.  It needs to exist because `is_safe_task_valid`
  // requires the coroutine function to be a complete type before checking if
  // it's a stateless callable in `is_safe_task_valid`.  This is a good place
  // to do this, since this function is guaranteed to be instantiated.
  safe_task<ArgSafety, T> get_return_object() noexcept {
    // If your build failed here, your `safe_task<>` coro declaration is
    // invalid.  Specific causes for this failure:
    //   - One of the arguments, or the return value, contains "unsafe
    //     aliasing" -- see `SafeAlias.h` for the details.  Typical
    //     causes include raw pointers, references, reference wrappers, etc.
    //   - A stateful callable: lambda with captures, class with members, etc.
    static_assert(
        detail::is_safe_task_valid<ArgSafety, T, Args...>,
        "Bad safe_task: check for unsafe aliasing in arguments or return "
        "type; also ensure your callable is stateless (or, in the case of "
        "`member_task` -- explicitly specifies `safe_alias_of`).");
    return TaskPromiseWrapper<
        T,
        safe_task<ArgSafety, T>,
        detail::TaskPromise<T>>::get_return_object();
  }
};

template <auto>
auto bind_captures_to_closure(auto&&, auto);

template <safe_alias ArgSafety, typename T>
struct safe_task_with_executor_cfg : DoesNotWrapAwaitable {
  using InnerTaskWithExecutorT = TaskWithExecutor<T>;
  using WrapperTaskT = safe_task<ArgSafety, T>;
};

template <safe_alias, typename>
struct safe_task_with_executor_base_traits;

template <safe_alias ArgSafety, typename T>
  requires(ArgSafety >= safe_alias::closure_min_arg_safety)
struct safe_task_with_executor_base_traits<ArgSafety, T> {
  using type = TaskWithExecutorWrapperCrtp<
      safe_task_with_executor<ArgSafety, T>,
      safe_task_with_executor_cfg<ArgSafety, T>>;
};

// `member_task` and `closure_task` are immovable.
template <safe_alias ArgSafety, typename T>
  requires(ArgSafety < safe_alias::closure_min_arg_safety)
struct safe_task_with_executor_base_traits<ArgSafety, T> {
  using type = ext::wrap_must_use_immediately_t<TaskWithExecutorWrapperCrtp<
      safe_task_with_executor<ArgSafety, T>,
      safe_task_with_executor_cfg<ArgSafety, T>>>;
};

template <safe_alias ArgSafety, typename T>
struct safe_task_cfg : DoesNotWrapAwaitable {
  using ValueT = T;
  using InnerTaskT = Task<T>;
  using TaskWithExecutorT = safe_task_with_executor<ArgSafety, T>;
  // There is no `promise_type` here because it's added by `coroutine_traits`
  // below.  This is the mechanism that enables `safe_task_promise` to inspect
  // the specific arguments of the coroutine (including the implicit object
  // parameter), and fail the compilation if anything looks unsafe.
  using PromiseT = void;
};

template <safe_alias ArgSafety, typename T>
struct safe_task_base_traits {
  using type =
      TaskWrapperCrtp<safe_task<ArgSafety, T>, safe_task_cfg<ArgSafety, T>>;
};

// `member_task` and `closure_task` are immovable.
template <safe_alias ArgSafety, typename T>
  requires(ArgSafety < safe_alias::closure_min_arg_safety)
struct safe_task_base_traits<ArgSafety, T> {
  using type = ext::wrap_must_use_immediately_t<
      TaskWrapperCrtp<safe_task<ArgSafety, T>, safe_task_cfg<ArgSafety, T>>>;
};

} // namespace detail

template <safe_alias, typename>
class BackgroundTask;

// IMPORTANT: This omits `start()` because backgrounded tasks can easily
// outlive the references they took, defeating the purpose of `safe_task`.
// See `BackgroundTask` instead.
template <safe_alias ArgSafety, typename T>
class [[nodiscard]] safe_task_with_executor final
    : public detail::safe_task_with_executor_base_traits<ArgSafety, T>::type {
 protected:
  using detail::safe_task_with_executor_base_traits<ArgSafety, T>::type::type;

  template <safe_alias, typename>
  friend class BackgroundTask; // for `unwrapTaskWithExecutor()`, remove later

 public:
  template <safe_alias>
  using folly_private_safe_alias_t = safe_alias_constant<ArgSafety>;

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

template <safe_alias ArgSafety, typename T>
class FOLLY_CORO_TASK_ATTRS safe_task final
    : public detail::safe_task_base_traits<ArgSafety, T>::type {
 protected:
  friend struct folly::coro::detail::SafeTaskTest; // to test `withNewSafety`
  template <safe_alias, typename>
  friend class safe_task; // `withNewSafety` makes a different `safe_task`
  template <auto> // uses `withNewSafety`
  friend auto detail::bind_captures_to_closure(auto&&, auto);
  template <safe_alias Safety, typename U>
  friend auto to_now_task(safe_task<Safety, U>);

  // The `async_closure` implementation is allowed to override the
  // argument-deduced `lenient_safe_alias_of_v` for a `safe_task` because
  // `capture_safety` marks some coro-stored `*capture*`s as `unsafe` even
  // though they're safe -- to discourage users from moving them.
  template <safe_alias NewSafety>
  safe_task<NewSafety, T> withNewSafety() && {
    return safe_task<NewSafety, T>{std::move(*this).unwrapTask()};
  }

 public:
  using detail::safe_task_base_traits<ArgSafety, T>::type::type;
  template <safe_alias>
  using folly_private_safe_alias_t = safe_alias_constant<ArgSafety>;

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

template <safe_alias Safety, typename T>
auto to_now_task(safe_task<Safety, T> t) {
  return now_task<T>{std::move(t).unwrapTask()};
}

namespace detail {

template <typename>
struct safe_task_traits;

template <typename T>
struct safe_task_traits<Task<T>> {
  static constexpr safe_alias arg_safety = safe_alias::unsafe;
  using return_type = T;
};
template <typename T>
struct safe_task_traits<TaskWithExecutor<T>> : safe_task_traits<Task<T>> {};
template <typename T>
struct safe_task_traits<now_task<T>> : safe_task_traits<Task<T>> {};
template <typename T>
struct safe_task_traits<now_task_with_executor<T>> : safe_task_traits<Task<T>> {
};

template <safe_alias ArgSafety, typename T>
struct safe_task_traits<safe_task<ArgSafety, T>> {
  static constexpr safe_alias arg_safety = ArgSafety;
  using return_type = T;
};
template <safe_alias ArgSafety, typename T>
struct safe_task_traits<safe_task_with_executor<ArgSafety, T>>
    : safe_task_traits<safe_task<ArgSafety, T>> {};

} // namespace detail

} // namespace folly::coro

template <folly::safe_alias ArgSafety, typename T, typename... Args>
struct folly::coro::
    coroutine_traits<folly::coro::safe_task<ArgSafety, T>, Args...> {
  // UGH: Pass `Args...` into `safe_task_promise` because at this point, the
  // coroutine function is still an incomplete type, and can't be validated.
  using promise_type =
      folly::coro::detail::safe_task_promise<ArgSafety, T, Args...>;
};

#endif
