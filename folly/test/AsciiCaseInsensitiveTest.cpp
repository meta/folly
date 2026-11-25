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

#include <algorithm>
#include <string_view>

#include <folly/Range.h>
#include <folly/portability/GFlags.h>
#include <folly/portability/GTest.h>

using namespace std;
using namespace folly;

TEST(CaseInsensitiveMatch, CompareWithLegacy) {
  AsciiCaseInsensitive cmp;
  for (int i = 0; i < (1 << 8); i++) {
    EXPECT_TRUE(cmp(tolower(i), toupper(i)));
    EXPECT_TRUE(cmp(toupper(i), tolower(i)));
  }
}

TEST(AsciiCaseInsensitiveEqual, Basic) {
  EXPECT_TRUE(asciiCaseInsensitiveEqual("", ""));
  EXPECT_TRUE(asciiCaseInsensitiveEqual("hello", "hello"));
  EXPECT_TRUE(asciiCaseInsensitiveEqual("Hello", "hELLO"));
  EXPECT_TRUE(asciiCaseInsensitiveEqual("a1-B2_c3", "A1-b2_C3"));
  EXPECT_FALSE(asciiCaseInsensitiveEqual("hello", "hell"));
  EXPECT_FALSE(asciiCaseInsensitiveEqual("hell", "hello"));
  EXPECT_FALSE(asciiCaseInsensitiveEqual("hello", "world"));
  EXPECT_FALSE(asciiCaseInsensitiveEqual("", "a"));
  // These pairs differ only in the case bit but are not letters.
  EXPECT_FALSE(asciiCaseInsensitiveEqual("@", "`"));
  EXPECT_FALSE(asciiCaseInsensitiveEqual("[", "{"));
}

TEST(AsciiCaseInsensitiveEqual, EmbeddedNul) {
  using namespace std::string_view_literals;
  EXPECT_TRUE(asciiCaseInsensitiveEqual("a\0B"sv, "A\0b"sv));
  EXPECT_FALSE(asciiCaseInsensitiveEqual("a\0b"sv, "a\0c"sv));
  EXPECT_FALSE(asciiCaseInsensitiveEqual("a\0b"sv, "a"sv));
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  folly::gflags::ParseCommandLineFlags(&argc, &argv, true);
  return RUN_ALL_TESTS();
}
