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

#include <folly/coro/ViaIfAsync.h>
#include <folly/result/try.h>

#if FOLLY_HAS_RESULT

namespace folly::coro {

/// `value_or_error_or_stopped` is the `result<T>` analog of the older
/// `co_awaitTry`.
///
/// In a `folly::coro` async coroutine, use `value_or_error_or_stopped` like so:
///
///   result<int> res = co_await value_or_error_or_stopped(taskReturningInt());
///   if (auto ex = get_exception<MyError>(res)) {
///     /* handle ex */
///   } else {
///     sum += co_await co_ready(res); // efficiently propagate unhandled error
///   }
///
/// Contrast that with related async coro vocabulary:
///  - `co_yield co_result(r)` from `Result.h` -- propagate `result<T>` or
///    `Try<T>` to the awaiter of the current coro.
///  - `auto& v = co_await co_ready(r)` from `Ready.h` -- given a `result<T>`,
///    unpack the value, or propagate any error to our awaiter.
///
/// The purpose of `value_or_error_or_stopped` is to handle errors from a child
/// task via `result<T>`, rather than through `try {} catch {}`.  Some reasons
/// to do so:
///   - Your error-handling APIs (logging, retry, etc) use `result<T>`.
///   - You wish to avoid the ~microsecond cost of thrown exceptions,
///     applicable only when your error path is hot, and the child uses
///     `co_yield` instead of `throw` to propagate exceptions.

namespace detail {

template <typename Awaiter>
using detect_await_resume_result =
    decltype(FOLLY_DECLVAL(Awaiter).await_resume_result());

template <typename Awaiter>
constexpr bool is_awaiter_result =
    is_detected_v<detect_await_resume_result, Awaiter>;

template <typename Awaitable>
constexpr bool is_awaitable_result =
    is_awaiter_result<awaiter_type_t<Awaitable>>;

// On the happy path, this uses the dedicated `await_resume_result()` protocol,
// if it's supported by the awaiter.
//
// As fallback, this reuses the `await_resume_try()` machinery in the hope that
// the compiler will be able to optimize away the `Try` -> `result` conversion.
//
// The reasons to support the dedicated protocol are (1) better semantics, and
// (2) a data flow that's easier for the compiler to optimize.  Specifically:
//
//   - `await_resume_result()` cleanly handles `Task<V&>`, whereas `Try`
//     doesn't support storing references, and the caller of `co_awaitTry` has
//     to deal with `Try<std::reference_wrapper<V>>`.  See the test in
//     `TaskOfLvalueReferenceAsTry`.
//
//   - `await_resume_result()` implementations can explicitly avoid the "empty
//     `Try`" pitfall, which is something that gets converted to a
//     `UsingUninitializedTry` error by the `try_to_result()` fallback.
//
//   - Falling back to `await_resume_try()` can incur an extra move-copy, which
//     may not always optimize away.
template <typename Awaitable>
class ResultAwaiter : public ValueOnlyAwaiterBase<Awaitable> {
 private:
  static_assert(is_awaitable_try<Awaitable> || is_awaitable_result<Awaitable>);

  using Base = ValueOnlyAwaiterBase<Awaitable>;

 public:
  explicit ResultAwaiter(Awaitable&& awaitable)
      : Base(static_cast<Awaitable&&>(awaitable)) {}

  // Preferred `await_resume()`: use `await_resume_result()` when available
  template <
      typename Awaiter2 = typename Base::Awaiter,
      typename Result =
          decltype(FOLLY_DECLVAL(Awaiter2&).await_resume_result())>
  Result await_resume() noexcept(
      noexcept(this->awaiter_.await_resume_result())) {
    return this->awaiter_.await_resume_result();
  }

  // Fallback `await_resume()`: use `await_resume_try()` otherwise
  template <
      typename Awaiter2 = typename Base::Awaiter,
      typename Result =
          decltype(try_to_result(FOLLY_DECLVAL(Awaiter2&).await_resume_try()))>
  Result await_resume() noexcept(
      noexcept(try_to_result(this->awaiter_.await_resume_try())))
    requires(!is_awaitable_result<Awaitable>) // Fallback logic
  {
    return try_to_result(this->awaiter_.await_resume_try());
  }
};

template <typename T>
class [[FOLLY_ATTR_CLANG_CORO_AWAIT_ELIDABLE]] ValueOrErrorOrStopped
    : public CommutativeWrapperAwaitable<ValueOrErrorOrStopped, T> {
 public:
  using CommutativeWrapperAwaitable<ValueOrErrorOrStopped, T>::
      CommutativeWrapperAwaitable;

  template <
      typename Self,
      std::enable_if_t<
          std::is_same_v<std::remove_cvref_t<Self>, ValueOrErrorOrStopped>,
          int> = 0,
      typename T2 = like_t<Self, T>,
      std::enable_if_t<is_awaitable_v<T2>, int> = 0>
  friend ResultAwaiter<T2> operator co_await(Self&& self) {
    return ResultAwaiter<T2>{static_cast<Self&&>(self).inner_};
  }

  using folly_private_value_only_awaitable_t = std::true_type;
};

// The awaitable backing `ValueOrError`, see that doc for why it's separate.
//
// NB: Besides `folly_private_value_only_awaitable_t`, this is identical to
// `ValueOrErrorOrStopped`, but the way the `CommutativeWrapperAwaitable`
// template is set up, it's a lot of code to parameterize it.
//
// IMPORTANT: this will never return "stopped" since `BasePromise` sets up
// `OperationCancelled` to unwind directly to the parent.
template <typename T>
class [[FOLLY_ATTR_CLANG_CORO_AWAIT_ELIDABLE]]
ValueOrErrorImpl : public CommutativeWrapperAwaitable<ValueOrErrorImpl, T> {
 public:
  using CommutativeWrapperAwaitable<ValueOrErrorImpl, T>::
      CommutativeWrapperAwaitable;

  template <
      typename Self,
      std::enable_if_t<
          std::is_same_v<std::remove_cvref_t<Self>, ValueOrErrorImpl>,
          int> = 0,
      typename T2 = like_t<Self, T>,
      std::enable_if_t<is_awaitable_v<T2>, int> = 0>
  friend ResultAwaiter<T2> operator co_await(Self&& self) {
    return ResultAwaiter<T2>{static_cast<Self&&>(self).inner_};
  }

  // Future: Cannot mark `folly_private_value_only_awaitable_t` since it
  // completes with "stopped" which may throw in contexts like `blocking_wait`.
  // Once the `awaitable_completions_v` refactor is complete, this should
  // remove the "error" completion from the underlying awaitable.
};

// Similarly to NothrowAwaitable, this deliberately omits `co_await` and relies
// on `BasePromise::await_transform`.  The goal is to prevent its use in
// contexts that cannot provide nothrow-propagation semantics for stopped
// coros.  Gating on `BasePromise` provides a tightly controlled allowlist of
// where this can be awaited, and how.
//
// For a specific example, consider `blocking_wait(value_or_error(...))` --
// this should either not compile, or throw on `OperationCancelled`.  The
// latter "sometimes throwing" semantics would be a surprising footgun, so we
// instead require the obvious contract of `value_or_error_or_stopped()` here.
template <typename T>
class [[FOLLY_ATTR_CLANG_CORO_AWAIT_ELIDABLE]]
ValueOrError : public CommutativeWrapperAwaitable<ValueOrError, T> {
 public:
  using CommutativeWrapperAwaitable<ValueOrError, T>::
      CommutativeWrapperAwaitable;

 protected:
  template <typename>
  friend class BasePromise;
  ValueOrErrorImpl<T> toValueOrErrorImpl() && {
    return ValueOrErrorImpl<T>{folly::ext::must_use_immediately_unsafe_mover(
        std::move(this->inner_))()};
  }

  // Future: See `ValueOrErrorImpl` regarding
  // `folly_private_value_only_awaitable_t` and `awaitable_completions_v`.
};

} // namespace detail

// BEFORE CHANGING: If you need an `Awaitable&&` overload, you must bifurcate
// these APIs on `must_use_immediately_v`, see `co_awaitTry` for an example.

/// When awaited, returns a `result<T>` in a value or an error state, but never
/// `has_stopped()` (aka `OperationCancelled`).  If `awaitable` reports as
/// "stopped", the awaiting coroutine will be promptly torn down, **without
/// throwing**, and its parent will in turn receive a "stopped" completion.
///
/// BE CAREFUL -- This is `co_nothrow` semantics, but **only** for
/// cancellation.  -- if your coro requires async cleanup, such as awaiting an
/// async scope or other background work, then you **must** use a safe async
/// RAII pattern, such `async_closure` if applicable (or `co_scope_exit`, but
/// beware of its lifetime risks).
template <typename Awaitable>
detail::ValueOrError<Awaitable> value_or_error(
    [[FOLLY_ATTR_CLANG_CORO_AWAIT_ELIDABLE_ARGUMENT]] Awaitable awaitable) {
  return detail::ValueOrError<Awaitable>{
      std::in_place, [&] {
        return folly::ext::must_use_immediately_unsafe_mover(
            std::move(awaitable))();
      }};
}

/// When awaited, returns `result<T>` just like `value_or_error()`, but also
/// captures "stopped" completions, so you **must** test for `has_stopped()`.
template <typename Awaitable>
detail::ValueOrErrorOrStopped<Awaitable> value_or_error_or_stopped(
    [[FOLLY_ATTR_CLANG_CORO_AWAIT_ELIDABLE_ARGUMENT]] Awaitable awaitable) {
  return detail::ValueOrErrorOrStopped<Awaitable>{
      std::in_place, [&] {
        return folly::ext::must_use_immediately_unsafe_mover(
            std::move(awaitable))();
      }};
}

} // namespace folly::coro

#endif
