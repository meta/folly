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

#include <folly/codec/hex.h>

#include <folly/lang/Keep.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

using namespace std::literals;

extern "C" FOLLY_KEEP bool check_folly_hex_is_digit_table(char h) {
  return folly::hex_is_digit_table(h);
}

extern "C" FOLLY_KEEP bool check_folly_hex_is_digit_flavor_aarch64(char h) {
  return folly::hex_is_digit_flavor_aarch64(h);
}

extern "C" FOLLY_KEEP bool check_folly_hex_is_digit_flavor_x86_64(char h) {
  return folly::hex_is_digit_flavor_x86_64(h);
}

extern "C" FOLLY_KEEP uint8_t
check_folly_hex_decode_digit_raw_flavor_aarch64(char h) {
  return folly::hex_decode_digit_raw_flavor_aarch64(h);
}

extern "C" FOLLY_KEEP uint8_t
check_folly_hex_decode_digit_raw_flavor_x86_64(char h) {
  return folly::hex_decode_digit_raw_flavor_x86_64(h);
}

extern "C" FOLLY_KEEP uint8_t check_folly_hex_decode_digit_table(char h) {
  return folly::hex_decode_digit_table(h);
}

extern "C" FOLLY_KEEP uint8_t
check_folly_hex_decode_digit_flavor_aarch64(char h) {
  return folly::hex_decode_digit_flavor_aarch64(h);
}

extern "C" FOLLY_KEEP uint8_t
check_folly_hex_decode_digit_flavor_x86_64(char h) {
  return folly::hex_decode_digit_flavor_x86_64(h);
}

extern "C" FOLLY_KEEP uint8_t
check_folly_hex_decode_digit_table_or_sink(char h) {
  auto const v = folly::hex_decode_digit_table(h);
  if (!folly::hex_decoded_digit_is_valid(v)) [[unlikely]] {
    folly::detail::keep_sink_nx();
  }
  return v;
}

extern "C" FOLLY_KEEP uint8_t
check_folly_hex_decode_digit_flavor_aarch64_or_sink(char h) {
  auto const v = folly::hex_decode_digit_flavor_aarch64(h);
  if (!folly::hex_decoded_digit_is_valid(v)) [[unlikely]] {
    folly::detail::keep_sink_nx();
  }
  return v;
}

extern "C" FOLLY_KEEP uint8_t
check_folly_hex_decode_digit_flavor_x86_64_or_sink(char h) {
  auto const v = folly::hex_decode_digit_flavor_x86_64(h);
  if (!folly::hex_decoded_digit_is_valid(v)) [[unlikely]] {
    folly::detail::keep_sink_nx();
  }
  return v;
}

extern "C" FOLLY_KEEP uint8_t
check_folly_hex_decode_digit_table_or_sink_split(char h) {
  if (!folly::hex_is_digit_table(h)) [[unlikely]] {
    folly::detail::keep_sink_nx();
    return ~0;
  }
  return folly::hex_decode_digit_table(h);
}

extern "C" FOLLY_KEEP uint8_t
check_folly_hex_decode_digit_flavor_aarch64_or_sink_split(char h) {
  if (!folly::hex_is_digit_flavor_aarch64(h)) [[unlikely]] {
    folly::detail::keep_sink_nx();
    return ~0;
  }
  return folly::hex_decode_digit_raw_flavor_aarch64(h);
}

extern "C" FOLLY_KEEP uint8_t
check_folly_hex_decode_digit_flavor_x86_64_or_sink_split(char h) {
  if (!folly::hex_is_digit_flavor_x86_64(h)) [[unlikely]] {
    folly::detail::keep_sink_nx();
    return ~0;
  }
  return folly::hex_decode_digit_raw_flavor_x86_64(h);
}

struct HexTest : testing::Test {};

TEST_F(HexTest, hex_decode_digit_lower_all) {
  constexpr auto alpha = folly::hex_alphabet_lower;
  for (size_t i = 0; i < alpha.size(); ++i) {
    auto const h = alpha[i];
    EXPECT_EQ(i, folly::hex_decode_digit(h));
    EXPECT_EQ(i, folly::hex_decode_digit_table(h));
    EXPECT_EQ(i, folly::hex_decode_digit_flavor_aarch64(h));
    EXPECT_EQ(i, folly::hex_decode_digit_flavor_x86_64(h));
  }
}

TEST_F(HexTest, hex_decode_digit_upper_all) {
  constexpr auto alpha = folly::hex_alphabet_upper;
  for (size_t i = 0; i < alpha.size(); ++i) {
    auto const h = alpha[i];
    EXPECT_EQ(i, folly::hex_decode_digit(h));
    EXPECT_EQ(i, folly::hex_decode_digit_table(h));
    EXPECT_EQ(i, folly::hex_decode_digit_flavor_aarch64(h));
    EXPECT_EQ(i, folly::hex_decode_digit_flavor_x86_64(h));
  }
}

TEST_F(HexTest, hex_decode_digit_full) {
  for (size_t i = 0; i < 256; ++i) {
    auto const c = static_cast<char>(i);
    auto const d = folly::hex_is_digit_table(c);
    EXPECT_EQ(d, folly::hex_is_digit(c));
    EXPECT_EQ(d, folly::hex_is_digit_flavor_aarch64(c));
    EXPECT_EQ(d, folly::hex_is_digit_flavor_x86_64(c));
    auto const h = folly::hex_alphabet_table[i];
    EXPECT_EQ(h, folly::hex_decode_digit(c));
    EXPECT_EQ(h, folly::hex_decode_digit_flavor_aarch64(c));
    EXPECT_EQ(h, folly::hex_decode_digit_flavor_x86_64(c));
  }
}
