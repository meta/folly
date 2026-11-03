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

#include <bitset>
#include <memory_resource>
#include <vector>

#include <folly/lang/VectorTraits.h>
#include <folly/portability/GTest.h>

bool returnTrueIfBitReference(bool&) {
  return false;
}

bool returnTrueIfBitReference(folly::vector_bool_reference auto) {
  return true;
}

namespace folly {

TEST(VectorTraitsTest, vector_bool_reference) {
  static_assert(
      folly::is_vector_bool_reference_v<std::vector<bool>::reference>);
  static_assert(
      folly::is_vector_bool_reference_v<
          std::vector<bool, std::pmr::polymorphic_allocator<>>::reference>);

  static_assert(!folly::is_vector_bool_reference_v<bool>);

  // Don't confuse with bitset reference.
  static_assert(!folly::is_vector_bool_reference_v<std::bitset<8>::reference>);

#if defined(_LIBCPP_VERSION) && _LIBCPP_ABI_VERSION == 1
  // In ABI version 1, libc++'s std::vector<bool>::const_reference was
  // implemented as a const bit reference, non-conformant to the standard.
  static_assert(
      folly::is_vector_bool_reference_v<std::vector<bool>::const_reference>);
#else
  // [vector.bool.pspc]: using const_reference = bool
  static_assert(
      !folly::is_vector_bool_reference_v<std::vector<bool>::const_reference>);
#endif

  bool b = true;
  EXPECT_FALSE(returnTrueIfBitReference(b));
  std::vector<bool> vb1{1};
  EXPECT_TRUE(returnTrueIfBitReference(vb1[0]));
  std::vector<bool, std::pmr::polymorphic_allocator<>> vb2{1};
  EXPECT_TRUE(returnTrueIfBitReference(vb2[0]));
}

} // namespace folly
