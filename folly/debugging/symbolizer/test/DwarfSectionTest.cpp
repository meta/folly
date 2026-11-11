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

#include <cstdint>
#include <span>
#include <vector>

#include <folly/debugging/symbolizer/DwarfSection.h>

#include <folly/portability/Config.h>
#include <folly/portability/GTest.h>

#if FOLLY_HAVE_DWARF && FOLLY_HAVE_ELF

using folly::symbolizer::DwarfSection;

TEST(DwarfSection, RejectsTruncatedInitialLength) {
  const std::vector<uint8_t> bytes{0x01, 0x02};
  DwarfSection section(bytes);

  auto result = section.next();

  ASSERT_TRUE(result.hasError());
}

TEST(DwarfSection, RejectsTruncatedDwarf64Length) {
  const std::vector<uint8_t> bytes{0xff, 0xff, 0xff, 0xff, 0x01, 0x02};
  DwarfSection section(bytes);

  auto result = section.next();

  ASSERT_TRUE(result.hasError());
}

TEST(DwarfSection, RejectsOverlongLength) {
  const std::vector<uint8_t> bytes{0x10, 0x00, 0x00, 0x00, 0xaa, 0xbb};
  DwarfSection section(bytes);

  auto result = section.next();

  ASSERT_TRUE(result.hasError());
}

TEST(DwarfSection, ParsesDwarf32Chunk) {
  const std::vector<uint8_t> bytes{0x02, 0x00, 0x00, 0x00, 0xaa, 0xbb};
  DwarfSection section(bytes);

  auto result = section.next();

  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result->hasValue());
  EXPECT_FALSE(section.is64Bit());
  const std::vector<uint8_t> expected{0xaa, 0xbb};
  EXPECT_EQ(
      expected, std::vector<uint8_t>((**result).begin(), (**result).end()));
  auto end = section.next();
  ASSERT_TRUE(end.hasValue());
  EXPECT_FALSE(end->hasValue());
}

TEST(DwarfSection, ParsesDwarf64Chunk) {
  const std::vector<uint8_t> bytes{
      0xff,
      0xff,
      0xff,
      0xff,
      0x01,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0xcc,
  };
  DwarfSection section(bytes);

  auto result = section.next();

  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result->hasValue());
  EXPECT_TRUE(section.is64Bit());
  const std::vector<uint8_t> expected{0xcc};
  EXPECT_EQ(
      expected, std::vector<uint8_t>((**result).begin(), (**result).end()));
  auto end = section.next();
  ASSERT_TRUE(end.hasValue());
  EXPECT_FALSE(end->hasValue());
}

#endif
