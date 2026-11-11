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

#include <folly/debugging/symbolizer/DwarfUtil.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include <folly/portability/GTest.h>

#if FOLLY_HAVE_DWARF && FOLLY_HAVE_ELF

namespace folly::symbolizer {
namespace {

template <typename T>
void append(std::vector<uint8_t>& bytes, T value) {
  const auto offset = bytes.size();
  bytes.resize(offset + sizeof(value));
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template <typename T>
void overwrite(std::vector<uint8_t>& bytes, size_t offset, T value) {
  ASSERT_LE(offset + sizeof(value), bytes.size());
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::vector<uint8_t> dwarf4Header() {
  std::vector<uint8_t> bytes;
  append<uint32_t>(bytes, 7);
  append<uint16_t>(bytes, 4);
  append<uint32_t>(bytes, 0x1234);
  append<uint8_t>(bytes, sizeof(uintptr_t));
  return bytes;
}

std::vector<uint8_t> dwarf5SkeletonHeader() {
  std::vector<uint8_t> bytes;
  append<uint32_t>(bytes, std::numeric_limits<uint32_t>::max());
  append<uint64_t>(bytes, 20);
  append<uint16_t>(bytes, 5);
  append<uint8_t>(bytes, DW_UT_skeleton);
  append<uint8_t>(bytes, sizeof(uintptr_t));
  append<uint64_t>(bytes, 0x123456789abcdef0);
  append<uint64_t>(bytes, 0xfedcba9876543210);
  return bytes;
}

std::vector<uint8_t> dwpIndex(uint64_t dwoId, uint32_t unitIndex = 1) {
  std::vector<uint8_t> bytes;
  append<uint32_t>(bytes, 2); // version
  append<uint32_t>(bytes, 4); // columns
  append<uint32_t>(bytes, 1); // units
  append<uint32_t>(bytes, 1); // buckets
  append<uint64_t>(bytes, dwoId);
  append<uint32_t>(bytes, unitIndex);
  append<uint32_t>(bytes, DW_SECT_INFO);
  append<uint32_t>(bytes, DW_SECT_ABBREV);
  append<uint32_t>(bytes, DW_SECT_STR_OFFSETS);
  append<uint32_t>(bytes, DW_SECT_RNGLISTS);
  append<uint32_t>(bytes, 0x10);
  append<uint32_t>(bytes, 0x20);
  append<uint32_t>(bytes, 0x30);
  append<uint32_t>(bytes, 0x40);
  return bytes;
}

TEST(DwarfUtil, ParsesDwarf4CompilationUnitHeader) {
  const auto bytes = dwarf4Header();

  auto result = parseCompilationUnitHeader(bytes, 0);

  ASSERT_TRUE(result.hasValue());
  EXPECT_FALSE(result->is64Bit);
  EXPECT_EQ(bytes.size(), result->size);
  EXPECT_EQ(4, result->version);
  EXPECT_EQ(DW_UT_compile, result->unitType);
  EXPECT_EQ(sizeof(uintptr_t), result->addrSize);
  EXPECT_EQ(uint64_t{0x1234}, result->abbrevOffset);
  EXPECT_EQ(bytes.size(), result->firstDie);
  EXPECT_FALSE(result->dwoId.hasValue());
}

TEST(DwarfUtil, ParsesDwarf5ExtendedLengthSkeletonHeader) {
  const auto bytes = dwarf5SkeletonHeader();

  auto result = parseCompilationUnitHeader(bytes, 0);

  ASSERT_TRUE(result.hasValue());
  EXPECT_TRUE(result->is64Bit);
  EXPECT_EQ(bytes.size(), result->size);
  EXPECT_EQ(5, result->version);
  EXPECT_EQ(DW_UT_skeleton, result->unitType);
  EXPECT_EQ(0x123456789abcdef0, result->abbrevOffset);
  ASSERT_TRUE(result->dwoId.hasValue());
  EXPECT_EQ(0xfedcba9876543210, *result->dwoId);
}

TEST(DwarfUtil, ParsesUnsupportedDwarf5UnitTypeHeader) {
  // DW_UT_type is valid but unsupported: parsing stops after the unit type.
  auto bytes = dwarf5SkeletonHeader();
  overwrite<uint8_t>(bytes, 14, DW_UT_type);

  auto result = parseCompilationUnitHeader(bytes, 0);

  ASSERT_TRUE(result.hasValue());
  EXPECT_EQ(DW_UT_type, result->unitType);
  EXPECT_EQ(15, result->firstDie);
  EXPECT_FALSE(result->dwoId.hasValue());
}

TEST(DwarfUtil, ReportsAbsoluteHeaderOffsets) {
  auto header = dwarf4Header();
  std::vector<uint8_t> bytes{0xaa, 0xbb, 0xcc};
  bytes.insert(bytes.end(), header.begin(), header.end());

  auto result = parseCompilationUnitHeader(bytes, 3);

  ASSERT_TRUE(result.hasValue());
  EXPECT_EQ(bytes.size(), result->firstDie);
}

TEST(DwarfUtil, RejectsEveryTruncatedHeaderPrefix) {
  const auto bytes = dwarf5SkeletonHeader();
  for (size_t size = 0; size < bytes.size(); ++size) {
    SCOPED_TRACE(size);
    auto result = parseCompilationUnitHeader(
        std::span<const uint8_t>{bytes}.first(size), 0);
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(DwarfParseErrorCode::truncated, result.error().code);
  }
}

TEST(DwarfUtil, RejectsOutOfRangeCompilationUnitOffset) {
  const auto bytes = dwarf4Header();

  auto result = parseCompilationUnitHeader(bytes, bytes.size() + 1);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::invalid_offset, result.error().code);
}

TEST(DwarfUtil, GetCompilationUnitsRejectsTruncatedHeader) {
  const std::vector<uint8_t> bytes{0x01, 0x02, 0x03};
  DebugSections sections{};
  sections.debugInfo = bytes;

  auto result = getCompilationUnits(nullptr, sections, 0);

  EXPECT_EQ(uint32_t{0}, result.mainCompilationUnit.size);
  EXPECT_FALSE(result.splitCU.hasValue());
}

TEST(DwarfUtil, RejectsReservedInitialLength) {
  std::vector<uint8_t> bytes;
  append<uint32_t>(bytes, 0xfffffff0);

  auto result = parseCompilationUnitHeader(bytes, 0);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::invalid_value, result.error().code);
}

TEST(DwarfUtil, RejectsInvalidCompilationUnitFields) {
  {
    auto bytes = dwarf4Header();
    overwrite<uint16_t>(bytes, sizeof(uint32_t), 1);
    auto result = parseCompilationUnitHeader(bytes, 0);
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(DwarfParseErrorCode::invalid_value, result.error().code);
  }
  {
    auto bytes = dwarf4Header();
    overwrite<uint8_t>(bytes, bytes.size() - 1, 3);
    auto result = parseCompilationUnitHeader(bytes, 0);
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(DwarfParseErrorCode::invalid_value, result.error().code);
  }
  {
    auto bytes = dwarf5SkeletonHeader();
    overwrite<uint8_t>(bytes, 14, 0xff);
    auto result = parseCompilationUnitHeader(bytes, 0);
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(DwarfParseErrorCode::invalid_value, result.error().code);
  }
}

TEST(DwarfUtil, ParsesDwpCompilationUnitContribution) {
  constexpr uint64_t kDwoId = 0x123456789abcdef0;
  const auto bytes = dwpIndex(kDwoId);
  CompilationUnit cu{};

  auto result = findDwpCompilationUnit(bytes, kDwoId, cu);

  ASSERT_TRUE(result.hasValue());
  EXPECT_TRUE(*result);
  EXPECT_EQ(uint32_t{0x10}, cu.offset);
  ASSERT_TRUE(cu.abbrevOffset.hasValue());
  EXPECT_EQ(uint64_t{0x20}, *cu.abbrevOffset);
  ASSERT_TRUE(cu.strOffsetsBase.hasValue());
  EXPECT_EQ(uint64_t{0x30}, *cu.strOffsetsBase);
  ASSERT_TRUE(cu.rnglistsBase.hasValue());
  EXPECT_EQ(uint64_t{0x40}, *cu.rnglistsBase);
}

TEST(DwarfUtil, MissingDwpCompilationUnitDoesNotModifyOutput) {
  const auto bytes = dwpIndex(0x1234);
  CompilationUnit cu{};
  cu.offset = 0x55;

  auto result = findDwpCompilationUnit(bytes, 0x4321, cu);

  ASSERT_TRUE(result.hasValue());
  EXPECT_FALSE(*result);
  EXPECT_EQ(uint32_t{0x55}, cu.offset);
}

TEST(DwarfUtil, RejectsEveryTruncatedDwpIndexPrefix) {
  constexpr uint64_t kDwoId = 0x123456789abcdef0;
  const auto bytes = dwpIndex(kDwoId);
  for (size_t size = 1; size < bytes.size(); ++size) {
    SCOPED_TRACE(size);
    CompilationUnit cu{};
    auto result = findDwpCompilationUnit(
        std::span<const uint8_t>{bytes}.first(size), kDwoId, cu);
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(DwarfParseErrorCode::truncated, result.error().code);
  }
}

TEST(DwarfUtil, RejectsInvalidDwpUnitIndexWithoutModifyingOutput) {
  constexpr uint64_t kDwoId = 0x123456789abcdef0;
  const auto bytes = dwpIndex(kDwoId, 2);
  CompilationUnit cu{};
  cu.offset = 0x55;

  auto result = findDwpCompilationUnit(bytes, kDwoId, cu);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::invalid_value, result.error().code);
  EXPECT_EQ(uint32_t{0x55}, cu.offset);
}

TEST(DwarfUtil, RejectsOverflowingDwpDimensions) {
  std::vector<uint8_t> bytes;
  append<uint32_t>(bytes, 2);
  append<uint32_t>(bytes, std::numeric_limits<uint32_t>::max());
  append<uint32_t>(bytes, std::numeric_limits<uint32_t>::max());
  append<uint32_t>(bytes, 0);
  CompilationUnit cu{};

  auto result = findDwpCompilationUnit(bytes, 0x1234, cu);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::integer_overflow, result.error().code);
}

} // namespace
} // namespace folly::symbolizer

#endif
