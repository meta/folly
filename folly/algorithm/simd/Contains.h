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

#include <folly/CPortability.h>
#include <folly/algorithm/simd/detail/Traits.h>

#include <iterator>
#include <span>

namespace folly::simd {
namespace detail {

// no overloading for easier profiling.

bool containsU8(
    std::span<const std::uint8_t> haystack, std::uint8_t needle) noexcept;
bool containsU16(
    std::span<const std::uint16_t> haystack, std::uint16_t needle) noexcept;
bool containsU32(
    std::span<const std::uint32_t> haystack, std::uint32_t needle) noexcept;
bool containsU64(
    std::span<const std::uint64_t> haystack, std::uint64_t needle) noexcept;

template <typename R>
using std_range_value_t = typename std::iterator_traits<decltype(std::begin(
    std::declval<R&>()))>::value_type;

// Constexpr check that we can always safely cast from From to To.
// If we don't require this, we might silently get different semantics from
// standard algorithms.
template <typename From, typename To>
constexpr bool convertible_with_no_loss() {
  if (sizeof(From) > sizeof(To)) {
    return false;
  }
  if (std::is_signed_v<From>) {
    return std::is_signed_v<To>;
  }

  return std::is_unsigned_v<To> || sizeof(From) < sizeof(To);
}

// All the requirements to call contains(haystack, needle);
//  * both are simd friendly (contiguous range, primitive types)
//  * integrals only
//  * needle can be converted to the value_type of haystack and
//    the result of equality comparison will be the same.
template <typename R, typename T>
constexpr bool contains_haystack_needle_test() {
  if constexpr (!std::is_invocable_v<AsSimdFriendlyUintFn, R>) {
    return false;
  } else if constexpr (!has_integral_simd_friendly_equivalent_scalar_v<T>) {
    return false;
  } else {
    using simd_haystack_value =
        simd_friendly_equivalent_scalar_t<std_range_value_t<R>>;
    using simd_needle = simd_friendly_equivalent_scalar_t<T>;
    return convertible_with_no_loss<simd_needle, simd_haystack_value>();
  }
}

} // namespace detail

/**
 * folly::simd::contains
 * folly::simd::contains_fn
 *
 * A vectorized version of `std::ranges::find(r, x) != r.end()`.
 * Only works for "simd friendly cases" -
 * specifically the ones where the type can be reasonably cast
 * to `uint8/16/32/64_t`.
 *
 **/
struct contains_fn {
  template <typename R, typename T>
    requires(detail::contains_haystack_needle_test<R, T>())
  FOLLY_ERASE bool operator()(R&& r, T x) const {
    auto castR = detail::asSimdFriendlyUint(std::span(r));
    using value_type = detail::std_range_value_t<decltype(castR)>;

    auto castX = static_cast<value_type>(detail::asSimdFriendlyUint(x));

    if constexpr (std::is_same_v<value_type, std::uint8_t>) {
      return detail::containsU8(castR, castX);
    } else if constexpr (std::is_same_v<value_type, std::uint16_t>) {
      return detail::containsU16(castR, castX);
    } else if constexpr (std::is_same_v<value_type, std::uint32_t>) {
      return detail::containsU32(castR, castX);
    } else {
      static_assert(
          std::is_same_v<value_type, std::uint64_t>,
          "internal error, unknown type");
      return detail::containsU64(castR, castX);
    }
  }
};

inline constexpr contains_fn contains;

} // namespace folly::simd
