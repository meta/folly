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
#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#ifdef _MSC_VER
#include <intrin.h>
#endif

#include <folly/CPortability.h>
#include <folly/Likely.h>
#include <folly/Traits.h>

namespace folly {
namespace detail {

template <typename T>
  requires std::is_integral_v<T>
bool generic_checked_add(T* result, T a, T b) {
  if constexpr (std::is_signed_v<T>) {
    if (a >= 0) {
      if (std::numeric_limits<T>::max() - a < b) [[unlikely]] {
        *result = {};
        return false;
      }
    } else if (b < std::numeric_limits<T>::min() - a) [[unlikely]] {
      *result = {};
      return false;
    }
    *result = a + b;
    return true;
  } else {
    if (a <= std::numeric_limits<T>::max() - b) [[likely]] {
      *result = a + b;
      return true;
    } else {
      *result = {};
      return false;
    }
  }
}

template <typename T>
  requires std::is_unsigned_v<T>
bool generic_checked_small_mul(T* result, T a, T b) {
  static_assert(sizeof(T) < sizeof(uint64_t), "Too large");
  uint64_t res = static_cast<uint64_t>(a) * static_cast<uint64_t>(b);
  constexpr uint64_t overflowMask = ~((1ULL << (sizeof(T) * 8)) - 1);
  if ((res & overflowMask) != 0) [[unlikely]] {
    *result = {};
    return false;
  }
  *result = static_cast<T>(res);
  return true;
}

template <typename T>
  requires(std::is_unsigned_v<T> && sizeof(T) < sizeof(uint64_t))
bool generic_checked_mul(T* result, T a, T b) {
  return generic_checked_small_mul(result, a, b);
}

template <typename T>
  requires(std::is_unsigned_v<T> && sizeof(T) == sizeof(uint64_t))
bool generic_checked_mul(T* result, T a, T b) {
  constexpr uint64_t halfBits = 32;
  constexpr uint64_t halfMask = (1ULL << halfBits) - 1ULL;
  uint64_t lhs_high = a >> halfBits;
  uint64_t lhs_low = a & halfMask;
  uint64_t rhs_high = b >> halfBits;
  uint64_t rhs_low = b & halfMask;

  if (lhs_high == 0 && rhs_high == 0) [[likely]] {
    *result = lhs_low * rhs_low;
    return true;
  }

  if (lhs_high != 0 && rhs_high != 0) [[unlikely]] {
    *result = {};
    return false;
  }

  uint64_t mid_bits1 = lhs_low * rhs_high;
  if (mid_bits1 >> halfBits != 0) [[unlikely]] {
    *result = {};
    return false;
  }

  uint64_t mid_bits2 = lhs_high * rhs_low;
  if (mid_bits2 >> halfBits != 0) [[unlikely]] {
    *result = {};
    return false;
  }

  uint64_t mid_bits = mid_bits1 + mid_bits2;
  if (mid_bits >> halfBits != 0) [[unlikely]] {
    *result = {};
    return false;
  }

  uint64_t bot_bits = lhs_low * rhs_low;
  if (!generic_checked_add(result, bot_bits, mid_bits << halfBits))
      [[unlikely]] {
    *result = {};
    return false;
  }
  return true;
}
} // namespace detail

template <typename T>
  requires std::is_integral_v<T>
bool checked_add(T* result, T a, T b) {
#if FOLLY_HAS_BUILTIN(__builtin_add_overflow)
  if (!__builtin_add_overflow(a, b, result)) [[likely]] {
    return true;
  }
  *result = {};
  return false;
#else
  return detail::generic_checked_add(result, a, b);
#endif
}

template <typename T>
  requires std::is_unsigned_v<T>
bool checked_add(T* result, T a, T b, T c) {
  T tmp{};
  if (!checked_add(&tmp, a, b)) [[unlikely]] {
    *result = {};
    return false;
  }
  if (!checked_add(&tmp, tmp, c)) [[unlikely]] {
    *result = {};
    return false;
  }
  *result = tmp;
  return true;
}

template <typename T>
  requires std::is_unsigned_v<T>
bool checked_add(T* result, T a, T b, T c, T d) {
  T tmp{};
  if (!checked_add(&tmp, a, b)) [[unlikely]] {
    *result = {};
    return false;
  }
  if (!checked_add(&tmp, tmp, c)) [[unlikely]] {
    *result = {};
    return false;
  }
  if (!checked_add(&tmp, tmp, d)) [[unlikely]] {
    *result = {};
    return false;
  }
  *result = tmp;
  return true;
}

template <typename T>
bool checked_div(T* result, T dividend, T divisor) {
  if (divisor == 0) [[unlikely]] {
    *result = {};
    return false;
  }
  if constexpr (folly::is_integral_v<T> && folly::is_signed_v<T>) {
    constexpr auto min =
        T(folly::make_unsigned_t<T>(1) << (sizeof(T) * CHAR_BIT - 1));
    if (dividend == min && divisor == T(-1)) [[unlikely]] {
      *result = {};
      return false;
    }
  }
  *result = dividend / divisor;
  return true;
}

template <typename T>
bool checked_mod(T* result, T dividend, T divisor) {
  if (divisor == 0) [[unlikely]] {
    *result = {};
    return false;
  }
  using R = decltype(dividend % divisor);
  if constexpr (
      folly::is_integral_v<T> && folly::is_integral_v<R> &&
      folly::is_signed_v<R>) {
    constexpr auto min =
        R(folly::make_unsigned_t<R>(1) << (sizeof(R) * CHAR_BIT - 1));
    if (dividend == min && divisor == T(-1)) [[unlikely]] {
      *result = {};
      return false;
    }
  }
  *result = dividend % divisor;
  return true;
}

template <typename T>
  requires std::is_unsigned_v<T>
bool checked_mul(T* result, T a, T b) {
  assert(result != nullptr);
#if FOLLY_HAS_BUILTIN(__builtin_mul_overflow)
  if (!__builtin_mul_overflow(a, b, result)) [[likely]] {
    return true;
  }
  *result = {};
  return false;
#elif _MSC_VER && FOLLY_X64
  static_assert(sizeof(T) <= sizeof(unsigned __int64), "Too large");
  if (sizeof(T) < sizeof(uint64_t)) {
    return detail::generic_checked_mul(result, a, b);
  } else {
    unsigned __int64 high;
    unsigned __int64 low = _umul128(a, b, &high);
    if (high == 0) [[likely]] {
      *result = static_cast<T>(low);
      return true;
    }
    *result = {};
    return false;
  }
#else
  return detail::generic_checked_mul(result, a, b);
#endif
}

template <typename T>
  requires std::is_unsigned_v<T>
bool checked_muladd(T* result, T base, T mul, T add) {
  T tmp{};
  if (!checked_mul(&tmp, base, mul)) [[unlikely]] {
    *result = {};
    return false;
  }
  if (!checked_add(&tmp, tmp, add)) [[unlikely]] {
    *result = {};
    return false;
  }
  *result = tmp;
  return true;
}

template <typename T, typename T2>
  requires(std::is_pointer_v<T> && std::is_unsigned_v<T2>)
bool checked_add(T* result, T a, T2 b) {
  size_t out = 0;
  bool ret = checked_muladd(
      &out,
      static_cast<size_t>(b),
      sizeof(std::remove_pointer_t<T>),
      reinterpret_cast<size_t>(a));

  *result = reinterpret_cast<T>(out);
  return ret;
}
} // namespace folly
