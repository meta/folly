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

#include <folly/UTF8String.h>

#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include <folly/portability/GTest.h>
#include <folly/test/TestUtils.h>

using namespace folly;

static_assert(!std::is_constructible_v<UTF8StringPiece, std::nullptr_t>);
static_assert(std::bidirectional_iterator<UTF8StringPiece::iterator>);
static_assert(
    std::is_same_v<UTF8StringPiece::iterator::value_type, std::uint32_t>);

const std::string_view kTestUTF8 =
    reinterpret_cast<const char*>(u8"This is \U0001F602 stuff!");

TEST(UTF8StringPiece, validUtf8) {
  std::string_view sp = kTestUTF8;
  UTF8StringPiece utf8 = sp;
  // utf8.size() not available since it's not a random-access range
  EXPECT_EQ(16, utf8.walk_size());
}

TEST(UTF8StringPiece, validSuffix) {
  UTF8StringPiece utf8 = kTestUTF8.substr(8);
  EXPECT_EQ(8, utf8.walk_size());
}

TEST(UTF8StringPiece, emptyMidCodepoint) {
  UTF8StringPiece utf8 = kTestUTF8.substr(9, 0); // okay since it's empty
  EXPECT_EQ(0, utf8.walk_size());
}

TEST(UTF8StringPiece, invalidMidCodepoint) {
  EXPECT_THROW(UTF8StringPiece(kTestUTF8.substr(9, 1)), std::out_of_range);
}

TEST(UTF8StringPiece, validImplicitConversion) {
  std::string input =
      reinterpret_cast<const char*>(u8"\U0001F602\U0001F602\U0001F602");
  auto checkImplicitCtor = [](UTF8StringPiece implicitCtor) {
    return implicitCtor.walk_size();
  };
  EXPECT_EQ(3, checkImplicitCtor(input));
}

TEST(UTF8StringPiece, defaultEmptyView) {
  const std::string_view piece;
  UTF8StringPiece utf8 = piece;
  EXPECT_TRUE(utf8.empty());
  EXPECT_EQ(0, utf8.walk_size());
  EXPECT_EQ(piece.data(), utf8.begin().base());
  EXPECT_EQ(piece.data(), utf8.end().base());
}

TEST(UTF8StringPiece, boundedViewContainsNul) {
  const char input[] = {'x', 'a', '\0', '\xF0', '\x9F', '\x98', '\x82', 'y'};
  const std::string_view piece(input + 1, 6);
  UTF8StringPiece utf8 = piece;
  const std::u32string expected(U"a\0\U0001F602", 3);
  EXPECT_EQ(expected, std::u32string(utf8.begin(), utf8.end()));
  EXPECT_EQ(piece.data(), utf8.begin().base());
  EXPECT_EQ(piece.data() + piece.size(), utf8.end().base());
}

TEST(UTF8StringPiece, invalidTruncatedCodepoint) {
  EXPECT_THROW(UTF8StringPiece(kTestUTF8.substr(8, 3)), std::out_of_range);
}

TEST(UTF8StringPiece, implicitViewOnlyConversion) {
  struct ViewOnly {
    operator std::string_view() const { return "hello"; }
  };
  UTF8StringPiece utf8 = ViewOnly{};
  EXPECT_EQ(U"hello", std::u32string(utf8.begin(), utf8.end()));
}

TEST(UTF8StringPiece, decrement) {
  UTF8StringPiece utf8 = kTestUTF8;
  auto it = utf8.end();
  std::advance(it, -8);
  EXPECT_EQ(U'\U0001F602', *it);
  EXPECT_EQ(U' ', *std::prev(it));
}

TEST(UTF8StringPiece, invalidSequences) {
  // Overlong encoding, surrogate, above U+10FFFF.
  for (std::string_view s : {"\xC0\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80"}) {
    EXPECT_THROW(*UTF8StringPiece(s).begin(), std::out_of_range) << s;
  }
  // 5-byte lead byte.
  EXPECT_THROW(UTF8StringPiece("\xF8\x88\x80\x80\x80"), std::out_of_range);
  // Missing continuation byte.
  EXPECT_THROW(UTF8StringPiece("\xC3x").walk_size(), std::out_of_range);
}
