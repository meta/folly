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

#include <cassert>
#include <cstddef>
#include <memory>
#include <span>
#include <type_traits>

#include <folly/Portability.h>
#include <folly/Traits.h>
#include <folly/lang/SafeAssert.h>
#include <folly/portability/Constexpr.h>

namespace folly {

//  down_cast
//
//  Unchecked polymorphic down-cast using static_cast. Only works for pairs of
//  types where the cvref-unqualified source type is polymorphic and a base of
//  the target type. The target type, which is passed as an explicit template
//  param, must be cvref-unqualified. The return type is the target type
//  following C++23 `std::forward_like` semantics, i.e.
//  - Same reference category as `S`.
//  - If the `S` is const-qualified, then `const` is added to the
//    underlying type of the result.
//
//  Checked with an assertion in debug builds.
template <typename T, typename S>
FOLLY_ERASE like_t<S, T>* down_cast(S* ptr) noexcept {
  using Q = std::remove_cv_t<S>;
  static_assert(std::is_polymorphic<Q>::value, "not polymorphic");
  static_assert(std::is_base_of<Q, T>::value, "not down-castable");
  using R = like_t<S, T>;
#if FOLLY_HAS_RTTI
  FOLLY_SAFE_DCHECK(dynamic_cast<R*>(ptr), "not a runtime down-cast");
#endif
  return static_cast<R*>(ptr);
}
template <typename T, typename S>
FOLLY_ERASE like_t<S&&, T> down_cast(S&& ref) noexcept {
  return static_cast<like_t<S&&, T>>(*down_cast<T>(std::addressof(ref)));
}

template <typename Dst, typename Src>
  requires std::is_function_v<Src>
FOLLY_ERASE Dst* reinterpret_function_cast(Src* src) noexcept {
  FOLLY_PUSH_WARNING
  //  Not every clang that reaches this header knows the warning group, and an
  //  unknown group is itself an error under -Werror.
#if FOLLY_HAS_WARNING("-Wcast-function-type-mismatch")
  FOLLY_CLANG_DISABLE_WARNING("-Wcast-function-type-mismatch")
#endif
  return reinterpret_cast<Dst*>(src);
  FOLLY_POP_WARNING
}
template <typename Dst, typename Src>
  requires std::is_function_v<Src>
FOLLY_ERASE Dst& reinterpret_function_cast(Src& src) noexcept {
  return *reinterpret_function_cast<Dst>(&src);
}

namespace detail {

struct span_cast_impl_fn {
  template <
      template <typename, std::size_t> class Span,
      typename U,
      typename T,
      std::size_t Extent>
  constexpr auto operator()(Span<T, Extent> in, U* castData) const {
    assert(
        static_cast<void const*>(in.data()) ==
        static_cast<void const*>(castData));

    // check alignment
    if (!std::is_constant_evaluated()) {
      assert(reinterpret_cast<std::uintptr_t>(in.data()) % sizeof(U) == 0);
    }

    if constexpr (Extent == std::dynamic_extent) {
      assert(in.size() * sizeof(T) % sizeof(U) == 0);
      return Span<U, std::dynamic_extent>(
          castData, in.size() * sizeof(T) / sizeof(U));
    } else {
      static_assert(Extent * sizeof(T) % sizeof(U) == 0);
      constexpr std::size_t kResSize = Extent * sizeof(T) / sizeof(U);
      return Span<U, kResSize>(castData, kResSize);
    }
  }
};

inline constexpr span_cast_impl_fn span_cast_impl;

} // namespace detail

/// static_span_cast
/// static_span_cast_fn
/// reinterpret_span_cast
/// reinterpret_span_cast_fn
/// const_span_cast
/// const_span_cast_fn
///
/// Casts a span to a different span. The result is a span referring to the same
/// region in memory but as a different type.
///
/// Example:
///
///   std::span<std::byte> bytes = ...
///   std::span<int> ints = folly::reinterpret_span_cast<int>(bytes);

template <typename U>
struct static_span_cast_fn {
  template <typename T, std::size_t Extent>
  constexpr auto operator()(std::span<T, Extent> in) const {
    return detail::span_cast_impl(in, static_cast<U*>(in.data()));
  }
};
template <typename U>
inline constexpr static_span_cast_fn<U> static_span_cast;

template <typename U>
struct reinterpret_span_cast_fn {
  template <typename T, std::size_t Extent>
  constexpr auto operator()(std::span<T, Extent> in) const {
    return detail::span_cast_impl(in, reinterpret_cast<U*>(in.data()));
  }
};
template <typename U>
inline constexpr reinterpret_span_cast_fn<U> reinterpret_span_cast;

template <typename U>
struct const_span_cast_fn {
  template <typename T, std::size_t Extent>
  constexpr auto operator()(std::span<T, Extent> in) const {
    return detail::span_cast_impl(in, const_cast<U*>(in.data()));
  }
};
template <typename U>
inline constexpr const_span_cast_fn<U> const_span_cast;

} // namespace folly
