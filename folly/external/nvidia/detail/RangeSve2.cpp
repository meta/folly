/*
 * Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#include <folly/external/nvidia/detail/RangeSve2.h>

#include <string_view>

#include <folly/CPortability.h>
#include <folly/Portability.h>

#if !FOLLY_ARM_FEATURE_SVE2
namespace folly {
namespace detail {
size_t qfind_first_byte_of_sve2(
    const std::string_view haystack, const std::string_view needles) {
  return qfind_first_byte_of_nosimd(haystack, needles);
}
} // namespace detail
} // namespace folly
#else
#include <cassert>

#include <arm_sve.h>

FOLLY_UNSAFE_BUFFER_USAGE_BEGIN

namespace folly {
namespace detail {

// helper method for case where needles.size() <= 16
size_t qfind_first_byte_of_needles16(
    const std::string_view haystack, const std::string_view needles) {
  assert(haystack.size() > 0u);
  assert(needles.size() > 0u);
  assert(needles.size() <= 16u);

  svuint8_t arr1, arr2;
  svbool_t pc, pn, pg = svwhilelt_b8(0, 16);  // pg = ptrue VL16

  // Only read the needles that are valid, otherwise `match' may report
  // incorrect matches. Also, copy a valid needle to inactive elements to avoid
  // incorrectly matching with zero.
  pn = svwhilelt_b8_u64(0, needles.size());
  arr2 = svld1_u8(pn, reinterpret_cast<uint8_t const*>(needles.data()));
  arr2 = svsel(pn, arr2, svdup_lane(arr2, 0));

  size_t index = 0;

  // Read valid chunks of 16 bytes.
  for (; index+16 <= haystack.size(); index += 16) {
    arr1 = svld1_u8(pg, reinterpret_cast<uint8_t const*>(haystack.data()+index));
    pc = svmatch_u8(pg, arr1, arr2);

    if (svptest_any(pg, pc)) {
      pc = svbrkb_z(pg, pc);
      return index+svcntp_b8(pg, pc);
    }
  }

  // Handle remainder.
  if (index < haystack.size()) {
    // Get a predicate just for the valid elements, otherwise same as above.
    pg = svwhilelt_b8(index, haystack.size());

    arr1 = svld1_u8(pg, reinterpret_cast<uint8_t const*>(haystack.data()+index));
    pc = svmatch_u8(pg, arr1, arr2);

    if (svptest_any(pg, pc)) {
      pc = svbrkb_z(pg, pc);
      return index+svcntp_b8(pg, pc);
    }
  }

  // No matches found.
  return std::string::npos;
}

size_t qfind_first_byte_of_sve2(
    const std::string_view haystack, const std::string_view needles) {
  if (FOLLY_UNLIKELY(needles.empty() || haystack.empty())) {
    return std::string::npos;
  } else if (needles.size() <= 16) {
    return qfind_first_byte_of_needles16(haystack, needles);
  }

  // This is pretty much the same as in `qfind_first_byte_of_needles16', just
  // looping through the needles.

  svuint8_t arr1, arr2;
  svbool_t pc, pn, pg;

  // Loop through haystacks of 16 (or potentially less) bytes.
  // Note: We could have an epilogue to avoid the min and `whilelt' below (see
  // qfind_first_byte_of_needles16).
  for (size_t index = 0; index < haystack.size(); index += 16) {
    // Load the haystack.
    pg = svwhilelt_b8(index, std::min(index+16, haystack.size()));
    arr1 = svld1_u8(pg, reinterpret_cast<uint8_t const*>(haystack.data()+index));

    // Loop through the needles in groups of 16 at a time.
    size_t j = 0;
    pn = svwhilelt_b8(0, 16);  // pn = ptrue VL16
    for (; j+16 <= needles.size(); j += 16) {
      // Load them.
      arr2 = svld1_u8(pn, reinterpret_cast<uint8_t const*>(needles.data()+j));

      // Carry the match.
      pc = svmatch_u8(pg, arr1, arr2);

      // If any match is found count and return the index of the first match
      // via a sequence of brkb+cntp.
      if (svptest_any(pg, pc)) {
        pc = svbrkb_z(pg, pc);
        return index+svcntp_b8(pg, pc);
      }
    }

    // Handle remainder needles.
    if (j < needles.size()) {
      // Get a predicate just for the valid elements, otherwise same as above.
      pn = svwhilelt_b8(j, needles.size());

      arr2 = svld1_u8(pn, reinterpret_cast<uint8_t const*>(needles.data()+j));
      arr2 = svsel(pn, arr2, svdup_lane(arr2, 0));

      pc = svmatch_u8(pg, arr1, arr2);
      if (svptest_any(pg, pc)) {
        pc = svbrkb_z(pg, pc);
        return index+svcntp_b8(pg, pc);
      }
    }
  }

  return std::string::npos;
}
} // namespace detail
} // namespace folly

FOLLY_UNSAFE_BUFFER_USAGE_END
#endif
