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

#include <folly/lang/Memory.h>

#include <cstring>
#include <vector>

#include <folly/portability/GTest.h>

TEST(MemoryTest, memeqMatchesMemcmpAtEveryLengthOffsetAndPosition) {
  for (std::size_t offset = 0; offset < 8; ++offset) {
    for (std::size_t n = 0; n <= 80; ++n) {
      std::vector<unsigned char> a(offset + n, 'x');
      std::vector<unsigned char> b(n, 'x');
      unsigned char const* const lhs = a.data() + offset;
      EXPECT_TRUE(folly::memeq(lhs, b.data(), n));
      for (std::size_t i = 0; i < n; ++i) {
        b[i] = 'y';
        EXPECT_EQ(
            std::memcmp(lhs, b.data(), n) == 0, folly::memeq(lhs, b.data(), n))
            << "length " << n << ", offset " << offset << ", position " << i;
        EXPECT_FALSE(folly::memeq(b.data(), lhs, n))
            << "length " << n << ", offset " << offset << ", position " << i;
        b[i] = 'x';
      }
    }
  }
}
