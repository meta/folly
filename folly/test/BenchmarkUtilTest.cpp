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

#include <folly/BenchmarkUtil.h>

#include <array>
#include <vector>

#include <folly/portability/GTest.h>

namespace folly {

struct BenchmarkUtilTest : ::testing::Test {};

TEST_F(BenchmarkUtilTest, bm_llc_size_fallback) {
  EXPECT_NE(0, detail::bm_llc_size_fallback());
}

TEST_F(BenchmarkUtilTest, bm_llc_size) {
  EXPECT_NE(0, bm_llc_size());
}

TEST_F(BenchmarkUtilTest, bm_llc_evict) {
  bm_llc_evict(0xdeadbeef); // in santizer builds, should not abort
  SUCCEED();
}

TEST_F(BenchmarkUtilTest, bm_llc_evict_touch) {
  struct Case {
    size_t word_count;
    size_t words_per_line;
    std::vector<size_t> written_offsets;
  };
  const std::array cases{
      Case{0, 8, {}},
      Case{1, 8, {0}},
      Case{16, 8, {0, 8, 15}},
      Case{19, 8, {0, 8, 16, 18}},
      Case{33, 16, {0, 16, 32}},
      Case{3, 1, {0, 1, 2}},
  };
  constexpr size_t untouched = 0xdeadbeef;
  constexpr size_t written = 0x12345678;
  for (auto const& test : cases) {
    SCOPED_TRACE(test.word_count);
    SCOPED_TRACE(test.words_per_line);
    std::vector<size_t> actual(test.word_count + 2, untouched);
    auto expected = actual;
    for (auto offset : test.written_offsets) {
      expected[offset + 1] = written;
    }

    detail::bm_llc_evict_touch(
        {actual.data() + 1, test.word_count}, test.words_per_line, written);

    EXPECT_EQ(expected, actual);
  }
}

} // namespace folly
