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
#include <string>
#include <string_view>

#include <folly/Likely.h>

namespace folly {

namespace detail {

inline size_t qfind_first_byte_of_std(
    const std::string_view haystack, const std::string_view needles) {
  auto ret = std::find_first_of(
      haystack.begin(),
      haystack.end(),
      needles.begin(),
      needles.end(),
      [](char a, char b) { return a == b; });
  return ret == haystack.end() ? std::string::npos : ret - haystack.begin();
}

size_t qfind_first_byte_of_bitset(
    const std::string_view haystack, const std::string_view needles);

size_t qfind_first_byte_of_byteset(
    const std::string_view haystack, const std::string_view needles);

inline size_t qfind_first_byte_of_nosimd(
    const std::string_view haystack, const std::string_view needles) {
  if (needles.empty() || haystack.empty()) [[unlikely]] {
    return std::string::npos;
  }
  // The thresholds below were empirically determined by benchmarking.
  // This is not an exact science since it depends on the CPU, the size of
  // needles, and the size of haystack.
  if ((needles.size() >= 4 && haystack.size() <= 10) ||
      (needles.size() >= 16 && haystack.size() <= 64) || needles.size() >= 32) {
    return qfind_first_byte_of_byteset(haystack, needles);
  }
  return qfind_first_byte_of_std(haystack, needles);
}
} // namespace detail
} // namespace folly
