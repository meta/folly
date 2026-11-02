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

#include <folly/Unit.h>
#include <folly/coro/TaskWrapper.h>
#include <folly/coro/ViaIfAsync.h>
#include <folly/lang/Assume.h>
#include <folly/result/result.h>
#include <folly/result/try.h>
#include <folly/result/value_only_result.h>

#if FOLLY_HAS_COROUTINES

/// `value_or_fatal<Task<T>, Policy>` wraps a task to guarantee value-only
/// completion.  Use this only with APIs that require it -- like `co_cleanup()`
/// async RAII, or async scopes.  If your code compiles without it, skip it!
///
/// You must specify a policy at least for "stopped" -- from C++26 onward,
/// cancellation is not an error: https://wg21.link/p1677
///
///   Policy                            | Stopped | Error
///   ----------------------------------|---------|------
///   on_stopped_void                   | void    | fatal
///   on_stopped<V>                     | V       | fatal
///   on_stopped_and_error<V>           | V       | V
///   on_stopped_and_error<V, W>        | V       | W
///   on_stopped_and_error<will_fatal>  | fatal   | fatal
///
/// ## Why not just `noexcept`?
///
/// A `noexcept` coroutine only fatals on errors during frame construction --
/// awaiting it can still throw.  `value_or_fatal<>` makes the await not throw.
///
/// ## This is probably not for you
///
///   - Not related to `co_nothrow`, which efficiently propagates exceptions to
///     the parent, without letting the awaiting task handle them.
///   - Not a general "avoid exceptions" tool -- the wrapper adds build cost,
///     frame allocation can still throw, and `std::terminate` offramps can
///     pessimize vs normal exception propagation.
///
/// ## Custom policies
///
///   struct my_policy_t {
///     static int value_only_default(non_value_result&& nv) noexcept {
///       LOG(ERROR) << (nv.has_stopped() ? "stopped" : "error");
///       return 0;
///     }
///   };
///   inline constexpr my_policy_t my_policy{};
///   value_or_fatal<Task<int>, my_policy> foo() { ... }

namespace folly::coro {

// Tag for "terminate on this completion"
struct will_fatal_t {};
inline constexpr will_fatal_t will_fatal{};

/// Primary policy struct: specify behavior for stopped (cancellation) and
/// error. Use via the variable templates below, not directly.
template <auto Stopped, auto Error>
class on_stopped_and_error_t {
 private:
  template <auto V, typename RetT>
  static constexpr RetT value_or_terminate(non_value_result&& nv) noexcept {
    if constexpr (std::is_same_v<decltype(V), will_fatal_t>) {
      nv.throw_exception(); // Better error than `std::terminate()`
      folly::assume_unreachable();
    } else {
      return V;
    }
  }

 public:
  template <
      typename RetT = conditional_t<
          std::is_same_v<decltype(Stopped), will_fatal_t>,
          decltype(Error),
          decltype(Stopped)>>
  static constexpr RetT value_only_default(non_value_result&& nv) noexcept {
    return nv.has_stopped()
        ? value_or_terminate<Stopped, RetT>(std::move(nv))
        : value_or_terminate<Error, RetT>(std::move(nv));
  }
};

template <auto Stopped, auto Error = Stopped>
inline constexpr auto on_stopped_and_error =
    on_stopped_and_error_t<Stopped, Error>{};

/// Convenience: stopped returns V, error terminates (fatal).
template <auto V>
inline constexpr auto on_stopped = on_stopped_and_error<V, will_fatal>;

/// Convenience for void tasks: stopped returns unit, error terminates.
inline constexpr auto on_stopped_void = on_stopped<unit>;

namespace detail {

// Detect if a type is a TaskWithExecutor-like type (has an attached executor).
template <typename T>
inline constexpr bool is_task_with_executor_v = requires {
  typename T::folly_private_task_without_executor_t;
};

template <typename Awaitable, auto Policy>
class ValueOrFatalAwaiter : public ValueOnlyAwaiterBase<Awaitable> {
 private:
  using Base = ValueOnlyAwaiterBase<Awaitable>;
  using RetT = decltype(FOLLY_DECLVAL(typename Base::Awaiter).await_resume());

 public:
  explicit ValueOrFatalAwaiter(Awaitable&& awaitable)
      : Base(static_cast<Awaitable&&>(awaitable)) {}

  // Returns `value_only_result<T>` for composition with `await_resume_result()`
  // protocol. Implicitly converts to `result<T>` for callers expecting that.
  value_only_result<RetT> await_resume_result() noexcept {
    // Get result from awaiter using the best available protocol
    auto res = [&]() -> result<RetT> {
      if constexpr (is_awaiter_result<typename Base::Awaiter>) {
        // Catch missed optimization: `awaiter_` returning `value_only_result`
        // would needlessly convert `value_only_result` to `result`, then back.
        static_assert(
            !is_instantiation_of_v<
                value_only_result,
                std::remove_cvref_t<
                    decltype(this->awaiter_.await_resume_result())>>);
        return this->awaiter_.await_resume_result();
      } else if constexpr (is_awaiter_try<typename Base::Awaiter>) {
        return try_to_result(this->awaiter_.await_resume_try());
      } else {
        try {
          if constexpr (std::is_void_v<RetT>) {
            this->awaiter_.await_resume();
            return {};
          } else {
            return this->awaiter_.await_resume();
          }
        } catch (...) { // Policy may handle both "stopped" and "error" below
          return non_value_result::from_current_exception();
        }
      }
    }();

    // Handle result uniformly
    if (res.has_value()) {
      if constexpr (std::is_void_v<RetT>) {
        return {};
      } else {
        return std::move(res).value_or_throw();
      }
    } else { // Apply policy to non-value result
      auto nv = std::move(res).non_value();
      using PolicyRetT = decltype(Policy.value_only_default(std::move(nv)));
      if constexpr (std::is_same_v<PolicyRetT, will_fatal_t>) {
        // Policy terminates (will_fatal), won't return
        Policy.value_only_default(std::move(nv));
        folly::assume_unreachable();
      } else if constexpr (std::is_void_v<RetT>) {
        Policy.value_only_default(std::move(nv));
        return {};
      } else {
        return Policy.value_only_default(std::move(nv));
      }
    }
  }

  RetT await_resume() noexcept { return await_resume_result().value_only(); }

  // IMPORTANT: There is deliberately NO `await_resume_try()` here. This causes
  // `co_awaitTry(value_or_fatal(...))` to INTENTIONALLY fail at compile time:
  //   - `value_or_fatal` guarantees value-only completion
  //   - `Try`'s error state would be useless (never populated)
  //   - We want new code to adopt `result`, not `Try`
  void await_resume_try(auto&&...) = delete;
};

template <typename, auto>
class ValueOrFatalAwaitable;

template <auto Policy>
struct ValueOrFatalAwaitableWithPolicy {
  template <typename T>
  using apply = ValueOrFatalAwaitable<T, Policy>;
};

/// `value_or_fatal<>` / `ValueOrFatalAwaitable<>` compose properly with other
/// coro- and awaitable-wrappers.  But, not all combinations make sense -- see
/// the test, and/or extend it if needed.  For example, the outer wrapper is
/// useless in `ValueOrFatalAwaitable<...>(co_awaitTry(...))`, since exceptions
/// would already have been routed into a `Try`.
template <typename T, auto Policy>
class [[FOLLY_ATTR_CLANG_CORO_AWAIT_ELIDABLE]] ValueOrFatalAwaitable
    : public CommutativeWrapperAwaitable<
          ValueOrFatalAwaitableWithPolicy<Policy>::template apply,
          T> {
 public:
  using CommutativeWrapperAwaitable<
      ValueOrFatalAwaitableWithPolicy<Policy>::template apply,
      T>::CommutativeWrapperAwaitable;

  template <typename T2 = T, std::enable_if_t<is_awaitable_v<T2>, int> = 0>
  ValueOrFatalAwaiter<T, Policy> operator co_await() && {
    return ValueOrFatalAwaiter<T, Policy>{std::move(this->inner_)};
  }

  using folly_private_value_only_awaitable_t = std::true_type;
};

} // namespace detail

#if FOLLY_HAS_IMMOVABLE_COROUTINES

template <typename Inner, auto Policy>
class value_or_fatal;

namespace detail {

// Configuration for wrapping TaskWithExecutor-like types
template <typename Inner, auto Policy>
struct value_or_fatal_with_executor_cfg {
  using InnerTaskWithExecutorT = Inner;
  using WrapperTaskT = value_or_fatal<
      typename Inner::folly_private_task_without_executor_t,
      Policy>;
  template <typename Awaitable>
  static inline auto wrapAwaitable(Awaitable&& awaitable) noexcept {
    static_assert(!value_only_awaitable_v<Awaitable>); // Don't double-wrap
    return detail::ValueOrFatalAwaitable<Awaitable, Policy>{
        std::in_place, [&] {
          return folly::ext::must_use_immediately_unsafe_mover(
              static_cast<Awaitable&&>(awaitable))();
        }};
  }
};

template <typename Inner, auto Policy>
using value_or_fatal_with_executor_base = TaskWithExecutorWrapperCrtp<
    value_or_fatal<Inner, Policy>,
    value_or_fatal_with_executor_cfg<Inner, Policy>>;

template <typename... BaseArgs>
class value_or_fatal_task_promise_wrapper final
    : public TaskPromiseWrapper<BaseArgs...> {};

// Configuration for wrapping Task-like types
template <typename Inner, auto Policy>
struct value_or_fatal_cfg {
  using ValueT = semi_await_result_t<Inner>;
  using InnerTaskT = Inner;
  using TaskWithExecutorT = value_or_fatal<
      decltype(co_withExecutor(
          FOLLY_DECLVAL(Executor::KeepAlive<>), FOLLY_DECLVAL(Inner))),
      Policy>;
  using PromiseT = value_or_fatal_task_promise_wrapper<
      ValueT,
      value_or_fatal<Inner, Policy>,
      typename folly::coro::coroutine_traits<Inner>::promise_type>;
  template <typename Awaitable>
  static inline auto wrapAwaitable(Awaitable&& awaitable) noexcept {
    static_assert(!value_only_awaitable_v<Awaitable>); // Don't double-wrap
    return detail::ValueOrFatalAwaitable<Awaitable, Policy>{
        static_cast<Awaitable&&>(awaitable)};
  }
};

template <typename Inner, auto Policy>
using value_or_fatal_base = TaskWrapperCrtp<
    value_or_fatal<Inner, Policy>,
    value_or_fatal_cfg<Inner, Policy>>;

// Selects base class based on whether Inner has an attached executor
template <typename Inner, auto Policy>
using value_or_fatal_auto_base = conditional_t<
    is_task_with_executor_v<Inner>,
    value_or_fatal_with_executor_base<Inner, Policy>,
    value_or_fatal_base<Inner, Policy>>;

// CAUTION: `value_or_fatal_rewrapper` gives you the power to wrap and unwrap
// `value_or_fatal`, so you must be extremely careful to preserve behavior:
//   - The unwrapped task must be rewrapped before awaiting.
//   - You must not wrap any other task.

template <typename>
struct value_or_fatal_rewrapper {
  static inline constexpr bool value_or_fatal_wrapped = false;
  static auto wrap_with(auto fn) { return fn(); }
};

template <typename Inner, auto Policy>
struct value_or_fatal_rewrapper<value_or_fatal<Inner, Policy>> {
  static inline constexpr bool value_or_fatal_wrapped = true;
  static Inner unwrapTask(value_or_fatal<Inner, Policy>&& t) {
    return std::move(t).unwrapTask();
  }
  static auto wrap_with(auto fn) {
    return value_or_fatal<decltype(fn()), Policy>{fn()};
  }
};

} // namespace detail

/// `value_or_fatal<Task<T>, Policy>` wraps a task to guarantee value-only
/// completion.
template <typename Inner, auto Policy>
class FOLLY_CORO_TASK_ATTRS value_or_fatal final
    : public detail::value_or_fatal_auto_base<Inner, Policy> {
 protected:
  using detail::value_or_fatal_auto_base<Inner, Policy>::
      value_or_fatal_auto_base;

  template <typename>
  friend struct detail::value_or_fatal_rewrapper;

 public:
  using folly_private_value_only_awaitable_t = std::true_type;
};

#endif // FOLLY_HAS_IMMOVABLE_COROUTINES

} // namespace folly::coro

#endif // FOLLY_HAS_COROUTINES
