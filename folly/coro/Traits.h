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

#include <folly/Traits.h>
#include <folly/coro/Coroutine.h>

#include <concepts>
#include <type_traits>

#if FOLLY_HAS_COROUTINES

namespace folly {
namespace coro {

/**
 * A type trait to unwrap a std::reference_wrapper<T> to a type T
 */
template <typename T>
struct remove_reference_wrapper {
  using type = T;
};
template <typename T>
struct remove_reference_wrapper<std::reference_wrapper<T>> {
  using type = T;
};
template <typename T>
using remove_reference_wrapper_t = typename remove_reference_wrapper<T>::type;

namespace detail {

template <typename T>
inline constexpr bool is_coroutine_handle_v = folly::is_instantiation_of_v< //
    coroutine_handle,
    T>;

template <typename T>
concept awaiter_ =
    !std::is_void_v<T> && require_sizeof<T> != 0 && requires(T& t) {
      { t.await_ready() } -> std::same_as<bool>;
      t.await_resume();
    };

template <typename Awaitable>
concept has_member_operator_co_await_ = !std::is_void_v<Awaitable> &&
    require_sizeof<Awaitable> != 0 && requires(Awaitable&& awaitable) {
      { static_cast<Awaitable&&>(awaitable).operator co_await() } -> awaiter_;
    };

template <typename Awaitable>
concept has_free_operator_co_await_ = !std::is_void_v<Awaitable> &&
    require_sizeof<Awaitable> != 0 && requires(Awaitable&& awaitable) {
      { operator co_await(static_cast<Awaitable&&>(awaitable)) } -> awaiter_;
    };

} // namespace detail

/// is_awaiter<T>::value
/// is_awaiter_v<T>
///
/// Template metafunction for querying whether the specified type implements
/// the 'Awaiter' concept.
///
/// An 'Awaiter' must have the following three methods.
/// - awaiter.await_ready() -> bool
/// - awaiter.await_suspend(coroutine_handle<void>()) ->
///     void OR
///     bool OR
///     coroutine_handle<T> for some T
/// - awaiter.await_resume()
///
/// Note that we don't check for a valid await_suspend() method here since
/// we don't yet know the promise type to use and some await_suspend()
/// implementations have particular requirements on the promise (eg. the
/// stack-aware awaiters may require the .getAsyncFrame() method)
template <typename T>
inline constexpr bool is_awaiter_v = detail::awaiter_<T>;

template <typename T>
struct is_awaiter : std::bool_constant<is_awaiter_v<T>> {};

/// is_awaitable<T>::value
/// is_awaitable_v<T>
///
/// Query if a type, T, is awaitable within the context of any coroutine whose
/// promise_type does not have an await_transform() that modifies what is
/// normally awaitable.
///
/// A type, T, is awaitable if it is an Awaiter, or if it has either a
/// member operator co_await() or a free-function operator co_await() that
/// returns an Awaiter.
template <typename T>
inline constexpr bool is_awaitable_v =
    detail::has_member_operator_co_await_<T> ||
    detail::has_free_operator_co_await_<T> || detail::awaiter_<T>;

template <typename T>
struct is_awaitable : std::bool_constant<is_awaitable_v<T>> {};

/// get_awaiter(Awaitable&&) -> awaiter_type_t<Awaitable>
///
/// The get_awaiter() function takes an Awaitable type and returns a value
/// that contains the await_ready(), await_suspend() and await_resume() methods
/// for that type.
///
/// This encapsulates calling 'operator co_await()' if it exists.
struct get_awaiter_fn {
  template <typename Awaitable>
    requires is_awaitable_v<Awaitable>
  decltype(auto) operator()(Awaitable&& awaitable) const {
    if constexpr (detail::has_member_operator_co_await_<Awaitable>) {
      return static_cast<Awaitable&&>(awaitable).operator co_await();
    } else if constexpr (detail::has_free_operator_co_await_<Awaitable>) {
      return operator co_await(static_cast<Awaitable&&>(awaitable));
    } else {
      return static_cast<Awaitable&>(awaitable);
    }
  }
};
constexpr inline get_awaiter_fn get_awaiter{};

/// awaiter_type<Awaitable>
///
/// A template-metafunction that lets you query the type that will be used
/// as the Awaiter object when you co_await a value of type Awaitable.
/// This is the return-type of get_awaiter() when passed a value of type
/// Awaitable.
template <typename Awaitable, typename = void>
struct awaiter_type {};

template <typename Awaitable>
struct awaiter_type<Awaitable, std::enable_if_t<is_awaitable_v<Awaitable>>> {
  using type = decltype(get_awaiter(std::declval<Awaitable>()));
};

/// await_result<Awaitable>
///
/// A template metafunction that allows you to query the type that will result
/// from co_awaiting a value of that type in the context of a coroutine that
/// does not modify the normal behaviour with promise_type::await_transform().
template <typename Awaitable>
using awaiter_type_t = typename awaiter_type<Awaitable>::type;

template <typename Awaitable, typename = void>
struct await_result {};

template <typename Awaitable>
struct await_result<Awaitable, std::enable_if_t<is_awaitable_v<Awaitable>>> {
  using type = decltype(get_awaiter(std::declval<Awaitable>()).await_resume());
};

template <typename Awaitable>
using await_result_t = typename await_result<Awaitable>::type;

namespace detail {

template <typename Promise>
inline constexpr bool promiseHasAsyncFrame_v = require_sizeof<Promise> != 0 &&
    requires(Promise& promise) { promise.getAsyncFrame(); };

} // namespace detail

} // namespace coro
} // namespace folly

#endif // FOLLY_HAS_COROUTINES
