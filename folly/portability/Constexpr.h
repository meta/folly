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
#include <folly/Portability.h>

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace folly {

namespace detail {

#if FOLLY_HAS_FEATURE(cxx_constexpr_string_builtins) || \
    FOLLY_HAS_BUILTIN(__builtin_strlen) || defined(_MSC_VER)
#define FOLLY_DETAIL_STRLEN __builtin_strlen
#else
#define FOLLY_DETAIL_STRLEN ::std::strlen
#endif

#if FOLLY_HAS_FEATURE(cxx_constexpr_string_builtins) || \
    FOLLY_HAS_BUILTIN(__builtin_strcmp)
#define FOLLY_DETAIL_STRCMP __builtin_strcmp
#else
#define FOLLY_DETAIL_STRCMP ::std::strcmp
#endif

// This overload is preferred if Char is char and if FOLLY_DETAIL_STRLEN
// yields a compile-time constant.
template <
    typename Char,
    std::size_t = FOLLY_DETAIL_STRLEN(static_cast<const Char*>(""))>
constexpr std::size_t constexpr_strlen_internal(const Char* s, int) noexcept {
  return FOLLY_DETAIL_STRLEN(s);
}
template <typename Char>
constexpr std::size_t constexpr_strlen_internal(
    const Char* s, unsigned) noexcept {
  std::size_t ret = 0;
  while (*s++) {
    ++ret;
  }
  return ret;
}

template <typename Char>
constexpr std::size_t constexpr_strlen_fallback(const Char* s) noexcept {
  return constexpr_strlen_internal(s, 0u);
}

static_assert(
    constexpr_strlen_fallback("123456789") == 9,
    "Someone appears to have broken constexpr_strlen...");

// This overload is preferred if Char is char and if FOLLY_DETAIL_STRCMP
// yields a compile-time constant.
template <
    typename Char,
    int = FOLLY_DETAIL_STRCMP(static_cast<const Char*>(""), "")>
constexpr int constexpr_strcmp_internal(
    const Char* s1, const Char* s2, int) noexcept {
  return FOLLY_DETAIL_STRCMP(s1, s2);
}
template <typename Char>
constexpr int constexpr_strcmp_internal(
    const Char* s1, const Char* s2, unsigned) noexcept {
  while (*s1 && *s1 == *s2) {
    ++s1, ++s2;
  }
  // NOTE: `int(*s1 - *s2)` may cause signed arithmetics overflow which is UB.
  return int(*s2 < *s1) - int(*s1 < *s2);
}

template <typename Char>
constexpr int constexpr_strcmp_fallback(
    const Char* s1, const Char* s2) noexcept {
  return constexpr_strcmp_internal(s1, s2, 0u);
}

#undef FOLLY_DETAIL_STRCMP
#undef FOLLY_DETAIL_STRLEN

} // namespace detail

template <typename Char>
constexpr std::size_t constexpr_strlen(const Char* s) noexcept {
#if __GNUC_PREREQ(11, 0)
  return detail::constexpr_strlen_internal(s, 0u);
#else
  return detail::constexpr_strlen_internal(s, 0);
#endif
}

template <typename Char>
constexpr int constexpr_strcmp(const Char* s1, const Char* s2) noexcept {
  return detail::constexpr_strcmp_internal(s1, s2, 0);
}

} // namespace folly
