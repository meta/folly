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

#include <algorithm>
#include <cstdint>
#include <string_view>

#include <folly/Portability.h>
#include <folly/algorithm/simd/detail/SimdAnyOf.h>
#include <folly/algorithm/simd/detail/SimdPlatform.h>

namespace folly {
namespace detail {

// Implementations of SimpleSimdStringUtils

template <typename Platform>
struct SimpleSimdStringUtilsImpl {
  using reg_t = typename Platform::reg_t;
  using logical_t = typename Platform::logical_t;

  struct HasSpaceOrCntrlSymbolsLambda {
    FOLLY_ALWAYS_INLINE
    logical_t operator()(reg_t reg) {
      // This happens to be equivalent to std::isspace(c) || std::iscntrl(c)
      return Platform::logical_or(
          Platform::less_equal(reg, 0x20), Platform::equal(reg, 0x7F));
    }
  };

  FOLLY_ALWAYS_INLINE
  static bool hasSpaceOrCntrlSymbols(std::string_view s) {
    if (s.empty()) {
      return false;
    }
    return simd::detail::simdAnyOf<Platform, /*unrolling*/ 4>(
        reinterpret_cast<const std::uint8_t*>(s.data()),
        reinterpret_cast<const std::uint8_t*>(s.data() + s.size()),
        HasSpaceOrCntrlSymbolsLambda{});
  }
};

template <>
struct SimpleSimdStringUtilsImpl<void> {
  FOLLY_ALWAYS_INLINE
  static bool hasSpaceOrCntrlSymbols(std::string_view s) {
    if (s.empty()) {
      return false;
    }
    return std::any_of(s.begin(), s.end(), [](char c) {
      std::uint32_t uc = static_cast<std::uint8_t>(c);
      return uc <= 0x20 || c == 0x7F;
    });
  }
};

} // namespace detail
} // namespace folly
