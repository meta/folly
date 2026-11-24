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

#include <folly/lang/bind/Bind.h>

#if FOLLY_HAS_COROUTINES

///
/// Please read the user- and developer-facing docs in `Capture.md`.
///
/// Callers typically only include `BindCaptures.h`, while callees need to
/// include `Captures.h`.  Both are provided by `AsyncClosure.h`.
///
/// NOTE:
///   - The binding helpers in this file are all in `folly::bind::` for
///     uniformity with the other related verbs -- `bind::args`,
///     `bind::in_place` `bind::constant`, etc.
///   - The `bind_to_storage_policy` specialization for this is only used
///     in one file, so it's inlined in `BindAsyncClosure.h`.
///

namespace folly::bind {

namespace detail {

// Any binding with this key is meant to be owned by the async closure
enum class capture_kind {
  plain = 0,
  // Syntax sugar: Passing `bind::capture_indirect()` with a pointer-like (e.g.
  // `unique_ptr<T>`), this emits a `capture_indirect<>`, giving access to
  // the underlying `T` with just one dereference `*` / `->`, instead of 2.
  indirect,
};

struct capture_bind_info_t : folly::bind::ext::bind_info_t {
  capture_kind captureKind_;

  constexpr explicit capture_bind_info_t(
      // Using a constraint prevents object slicing
      std::same_as<folly::bind::ext::bind_info_t> auto bi,
      capture_kind ap)
      : folly::bind::ext::bind_info_t(std::move(bi)), captureKind_(ap) {}
};

template <capture_kind Kind, typename UpdateBI = std::identity>
struct capture_bind_info {
  // Using `auto` prevents object slicing
  constexpr auto operator()(auto bi) {
    return capture_bind_info_t{UpdateBI{}(std::move(bi)), Kind};
  }
};

} // namespace detail

///
/// `bind::capture()` and `bind::capture_indirect()` work much like other
/// `folly::bind` modifiers.  However, since they're primarily intended
/// for `async_closure` arguments, you will practically only use them:
///   - alone, for non-`co_cleanup` arguments;
///   - with `bind::in_place()` or `bind::in_place_with()`, for `co_cleanup`
///     arguments;
///   - with `constant()`, for either.
///
/// `capture_in_place<T>()` is short for `bind::capture(bind::in_place<T>())`.
///
/// See `Captures.md` and `folly/lang/Bindings.md`.
///

template <typename... Ts>
struct capture
    : ::folly::bind::ext::merge_update_args<
          detail::capture_bind_info<detail::capture_kind::plain>,
          Ts...> {
  using ::folly::bind::ext::merge_update_args<
      detail::capture_bind_info<detail::capture_kind::plain>,
      Ts...>::merge_update_args;
};
template <typename... Ts>
capture(Ts&&...) -> capture<folly::bind::ext::deduce_args_t<Ts>...>;

template <typename... Ts>
struct capture_indirect
    : ::folly::bind::ext::merge_update_args<
          detail::capture_bind_info<detail::capture_kind::indirect>,
          Ts...> {
  using ::folly::bind::ext::merge_update_args<
      detail::capture_bind_info<detail::capture_kind::indirect>,
      Ts...>::merge_update_args;
};
template <typename... Ts>
capture_indirect(Ts&&...)
    -> capture_indirect<folly::bind::ext::deduce_args_t<Ts>...>;

// Sugar for `capture{const_ref{...}}`
template <typename... Ts>
struct capture_const_ref
    : ::folly::bind::ext::merge_update_args<
          detail::capture_bind_info<
              detail::capture_kind::plain,
              ::folly::bind::detail::const_ref_bind_info>,
          Ts...> {
  using ::folly::bind::ext::merge_update_args<
      detail::capture_bind_info<
          detail::capture_kind::plain,
          ::folly::bind::detail::const_ref_bind_info>,
      Ts...>::merge_update_args;
};
template <typename... Ts>
capture_const_ref(Ts&&...)
    -> capture_const_ref<folly::bind::ext::deduce_args_t<Ts>...>;
// Sugar for `capture{mut_ref{...}}`
template <typename... Ts>
struct capture_mut_ref
    : ::folly::bind::ext::merge_update_args<
          detail::capture_bind_info<
              detail::capture_kind::plain,
              ::folly::bind::detail::mut_ref_bind_info>,
          Ts...> {
  using ::folly::bind::ext::merge_update_args<
      detail::capture_bind_info<
          detail::capture_kind::plain,
          ::folly::bind::detail::mut_ref_bind_info>,
      Ts...>::merge_update_args;
};
template <typename... Ts>
capture_mut_ref(Ts&&...)
    -> capture_mut_ref<folly::bind::ext::deduce_args_t<Ts>...>;

// Sugar for `capture{in_place<T>(...)}`
template <typename T>
auto capture_in_place(auto&&... as [[clang::lifetimebound]]) {
  return capture(bind::in_place<T>(static_cast<decltype(as)>(as)...));
}
// Sugar for `capture{in_place_with(fn, ...)}`
auto capture_in_place_with(
    auto make_fn, auto&&... as [[clang::lifetimebound]]) {
  return capture(
      bind::in_place_with(
          std::move(make_fn), static_cast<decltype(as)>(as)...));
}

} // namespace folly::bind

#endif
