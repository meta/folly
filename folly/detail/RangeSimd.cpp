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

#include <folly/detail/RangeSimd.h>

#include <string_view>

#include <folly/Portability.h>

#include <folly/detail/RangeSse42.h>
#include <folly/external/nvidia/detail/RangeSve2.h>

namespace folly {
namespace detail {

size_t qfind_first_byte_of_simd(
    const std::string_view haystack, const std::string_view needles) {
#if FOLLY_ARM_FEATURE_SVE2
  return qfind_first_byte_of_sve2(haystack, needles);
#elif FOLLY_SSE_PREREQ(4, 2)
  return qfind_first_byte_of_sse42(haystack, needles);
#else
  return qfind_first_byte_of_nosimd(haystack, needles);
#endif
}

} // namespace detail
} // namespace folly
