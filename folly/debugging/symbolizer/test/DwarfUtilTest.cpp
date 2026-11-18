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

#include <folly/debugging/symbolizer/DwarfSection.h>
#include <folly/debugging/symbolizer/DwarfUtil.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include <folly/Utility.h>
#include <folly/Varint.h>
#include <folly/portability/GTest.h>
#include <folly/portability/SysMman.h>

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

void appendULEB(std::vector<uint8_t>& bytes, uint64_t value) {
  const auto offset = bytes.size();
  bytes.resize(offset + folly::kMaxVarintLength64);
  const auto encodedSize = folly::encodeVarint(value, bytes.data() + offset);
  bytes.resize(offset + encodedSize);
}

CompilationUnit rangeUnit(
    uint8_t version,
    std::span<const uint8_t> debugRanges,
    std::span<const uint8_t> debugRnglists,
    std::span<const uint8_t> debugAddr = {}) {
  CompilationUnit cu{};
  cu.version = version;
  cu.addrSize = sizeof(uint64_t);
  cu.debugSections.debugRanges = debugRanges;
  cu.debugSections.debugRnglists = debugRnglists;
  cu.debugSections.debugAddr = debugAddr;
  return cu;
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

std::vector<uint8_t> abbreviation() {
  return {
      1,
      DW_TAG_compile_unit,
      DW_CHILDREN_yes,
      DW_AT_name,
      DW_FORM_string,
      0,
      0,
      0,
  };
}

// DWARF 4 split units carry their DWO ID in DW_AT_GNU_dwo_id, DWARF 5 in the
// unit header.
std::vector<uint8_t> splitUnitAbbreviation(uint16_t version) {
  if (version == 5) {
    return abbreviation();
  }
  std::vector<uint8_t> bytes{1, DW_TAG_compile_unit, DW_CHILDREN_yes};
  appendULEB(bytes, DW_AT_GNU_dwo_id);
  appendULEB(bytes, DW_FORM_data8);
  bytes.insert(bytes.end(), {0, 0, 0});
  return bytes;
}

std::vector<uint8_t> splitUnit(uint16_t version, uint64_t dwoId) {
  std::vector<uint8_t> bytes;
  append<uint32_t>(bytes, 0);
  append<uint16_t>(bytes, version);
  if (version == 5) {
    append<uint8_t>(bytes, DW_UT_split_compile);
    append<uint8_t>(bytes, sizeof(uintptr_t));
    append<uint32_t>(bytes, 0);
    append<uint64_t>(bytes, dwoId);
    appendULEB(bytes, 1);
    bytes.insert(bytes.end(), {'x', 0});
  } else {
    append<uint32_t>(bytes, 0);
    append<uint8_t>(bytes, sizeof(uintptr_t));
    appendULEB(bytes, 1);
    append<uint64_t>(bytes, dwoId);
  }
  bytes.push_back(0);
  overwrite<uint32_t>(bytes, 0, folly::to_narrow(bytes.size() - 4));
  return bytes;
}

class SparseSection {
 public:
  explicit SparseSection(size_t size)
      : size_(size),
        data_(mmap(
            nullptr,
            size,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
            -1,
            0)) {}
  SparseSection(const SparseSection&) = delete;
  SparseSection& operator=(const SparseSection&) = delete;
  ~SparseSection() {
    if (valid()) {
      munmap(data_, size_);
    }
  }

  bool valid() const { return data_ != MAP_FAILED; }
  void write(size_t offset, std::span<const uint8_t> bytes) {
    std::memcpy(
        static_cast<uint8_t*>(data_) + offset, bytes.data(), bytes.size());
  }
  std::span<const uint8_t> bytes() const {
    return {static_cast<const uint8_t*>(data_), size_};
  }

 private:
  size_t size_;
  void* data_;
};

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

TEST(DwarfUtil, FindsDwpUnitPastTruncatedInfoOffset) {
  if (sizeof(size_t) < sizeof(uint64_t)) {
    GTEST_SKIP() << "requires a 64-bit address space";
  }
  constexpr uint64_t kTruncatedOffset = 0x10;
  constexpr uint64_t kOffset = (uint64_t{1} << 32) + kTruncatedOffset;
  constexpr uint64_t kDwoId = 0x123456789abcdef0;
  SparseSection info(folly::to_narrow(kOffset + 0x1000));
  if (!info.valid()) {
    GTEST_SKIP() << "cannot map a section larger than 4 GiB";
  }
  for (uint16_t version : {4, 5}) {
    SCOPED_TRACE(version);
    const auto abbrev = splitUnitAbbreviation(version);
    info.write(kTruncatedOffset, splitUnit(version, kDwoId + 1));
    info.write(folly::to_narrow(kOffset), splitUnit(version, kDwoId));
    CompilationUnit cu{};
    cu.debugSections.debugInfo = info.bytes();
    cu.debugSections.debugAbbrev = abbrev;
    cu.offset = kTruncatedOffset;

    EXPECT_TRUE(parseDwpCompilationUnit(cu, kDwoId));

    EXPECT_EQ(kOffset, cu.offset);
    ASSERT_TRUE(cu.dwoId.hasValue());
    EXPECT_EQ(kDwoId, *cu.dwoId);
  }
}

TEST(DwarfUtil, RejectsDwpUnitWithMismatchedDwoIdWithoutModifyingOutput) {
  const auto info = splitUnit(5, 0x1234);
  const auto abbrev = splitUnitAbbreviation(5);
  CompilationUnit cu{};
  cu.debugSections.debugInfo = info;
  cu.debugSections.debugAbbrev = abbrev;

  EXPECT_FALSE(parseDwpCompilationUnit(cu, 0x4321));

  EXPECT_EQ(uint32_t{0}, cu.size);
  EXPECT_FALSE(cu.dwoId.hasValue());
}

TEST(DwarfUtil, ResolvesStrxThroughDwarf64DwpContributionOfDwarf32Unit) {
  const std::vector<uint8_t> strings{'a', 0, 'b', 0};
  std::vector<uint8_t> stringOffsets(4);
  append<uint32_t>(stringOffsets, std::numeric_limits<uint32_t>::max());
  append<uint64_t>(stringOffsets, 20);
  append<uint16_t>(stringOffsets, 5);
  append<uint16_t>(stringOffsets, 0);
  append<uint64_t>(stringOffsets, 0);
  append<uint64_t>(stringOffsets, 2);
  CompilationUnit cu{};
  cu.is64Bit = false;
  cu.strOffsetsIs64Bit = false;
  cu.strOffsetsBase = 4;
  cu.debugSections.debugStr = strings;
  cu.debugSections.debugStrOffsets = stringOffsets;
  const std::vector<uint8_t> info{1};
  DwarfCursor cursor(info);
  Die die{};

  setDwpStrOffsetsBase(cu);
  auto result = readAttribute(
      cu,
      die,
      AttributeSpec{.name = DW_AT_name, .form = DW_FORM_strx1},
      cursor);

  ASSERT_TRUE(result.hasValue());
  EXPECT_EQ("b", std::get<folly::StringPiece>(result->attrValue));
}

TEST(DwarfUtil, ParsesAbbreviationDeclaration) {
  const auto bytes = abbreviation();
  DwarfCursor cursor(bytes, 0x20);

  auto result = readAbbreviation(cursor);

  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result->hasValue());
  const auto& abbr = **result;
  EXPECT_EQ(uint64_t{1}, abbr.code);
  EXPECT_EQ(uint64_t{DW_TAG_compile_unit}, abbr.tag);
  EXPECT_TRUE(abbr.hasChildren);
  EXPECT_EQ(bytes.data() + 3, abbr.attributes.data());
  EXPECT_EQ(size_t{4}, abbr.attributes.size());
  EXPECT_EQ(size_t{0x27}, cursor.offset());
}

TEST(DwarfUtil, ReadsAbbreviationTableTerminator) {
  const std::vector<uint8_t> bytes{0};
  DwarfCursor cursor(bytes);

  auto result = readAbbreviation(cursor);

  ASSERT_TRUE(result.hasValue());
  EXPECT_FALSE(result->hasValue());
  EXPECT_TRUE(cursor.empty());
}

TEST(DwarfUtil, RejectsEveryTruncatedAbbreviationPrefix) {
  const auto bytes = abbreviation();
  for (size_t size = 0; size < bytes.size() - 1; ++size) {
    SCOPED_TRACE(size);
    DwarfCursor cursor(std::span<const uint8_t>{bytes}.first(size));
    auto result = readAbbreviation(cursor);
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(DwarfParseErrorCode::truncated, result.error().code);
  }
}

TEST(DwarfUtil, RejectsInvalidAbbreviationChildrenMarker) {
  auto bytes = abbreviation();
  bytes[2] = 2;
  DwarfCursor cursor(bytes);

  auto result = readAbbreviation(cursor);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::invalid_value, result.error().code);
  EXPECT_EQ(size_t{2}, result.error().offset);
}

TEST(DwarfUtil, RejectsZeroAbbreviationTag) {
  const std::vector<uint8_t> bytes{1, 0, DW_CHILDREN_no, 0, 0};
  DwarfCursor cursor(bytes);

  auto result = readAbbreviation(cursor);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::invalid_value, result.error().code);
  EXPECT_EQ(size_t{1}, result.error().offset);
}

TEST(DwarfUtil, RejectsMismatchedAttributeSpecTerminator) {
  const std::vector<uint8_t> bytes{DW_AT_name, 0};
  DwarfCursor cursor(bytes);

  auto result = readAttributeSpec(cursor);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::invalid_value, result.error().code);
  EXPECT_EQ(size_t{0}, result.error().offset);
}

TEST(DwarfUtil, ParsesImplicitConstantAttributeSpec) {
  const std::vector<uint8_t> bytes{
      DW_AT_decl_line, DW_FORM_implicit_const, 0x7f};
  DwarfCursor cursor(bytes);

  auto result = readAttributeSpec(cursor);

  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result->hasValue());
  EXPECT_EQ(uint64_t{DW_AT_decl_line}, (**result).name);
  EXPECT_EQ(uint64_t{DW_FORM_implicit_const}, (**result).form);
  EXPECT_EQ(int64_t{-1}, (**result).implicitConst);
}

TEST(DwarfUtil, ParsesDieAndResolvesAbbreviation) {
  const std::vector<uint8_t> info{1};
  const auto abbrev = abbreviation();
  CompilationUnit cu{};
  cu.debugSections.debugInfo = info;
  cu.debugSections.debugAbbrev = abbrev;
  cu.size = static_cast<uint32_t>(info.size());
  cu.abbrevOffset = 0;

  auto result = getDieAtOffset(cu, 0);

  ASSERT_TRUE(result.hasValue());
  EXPECT_EQ(uint64_t{1}, result->code);
  EXPECT_EQ(uint8_t{1}, result->attrOffset);
  EXPECT_EQ(uint64_t{DW_TAG_compile_unit}, result->abbr.tag);
}

TEST(DwarfUtil, RejectsTruncatedDieCode) {
  const std::vector<uint8_t> info{0x80};
  CompilationUnit cu{};
  cu.debugSections.debugInfo = info;
  cu.size = static_cast<uint32_t>(info.size());

  auto result = getDieAtOffset(cu, 0);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::truncated, result.error().code);
}

TEST(DwarfUtil, RejectsDieOutsideCompilationUnit) {
  const std::vector<uint8_t> info{0};
  CompilationUnit cu{};
  cu.debugSections.debugInfo = info;
  cu.size = static_cast<uint32_t>(info.size());

  auto result = getDieAtOffset(cu, 2);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::invalid_offset, result.error().code);
}

TEST(DwarfUtil, RejectsMissingAbbreviationCode) {
  const std::vector<uint8_t> info{2};
  const auto abbrev = abbreviation();
  CompilationUnit cu{};
  cu.debugSections.debugInfo = info;
  cu.debugSections.debugAbbrev = abbrev;
  cu.size = static_cast<uint32_t>(info.size());
  cu.abbrevOffset = 0;

  auto result = getDieAtOffset(cu, 0);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::invalid_value, result.error().code);
}

TEST(DwarfUtil, RejectsOutOfRangeAbbreviationOffset) {
  const std::vector<uint8_t> info{1};
  const auto abbrev = abbreviation();
  CompilationUnit cu{};
  cu.debugSections.debugInfo = info;
  cu.debugSections.debugAbbrev = abbrev;
  cu.size = static_cast<uint32_t>(info.size());
  cu.abbrevOffset = 99;

  auto result = getDieAtOffset(cu, 0);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::invalid_offset, result.error().code);
}

TEST(DwarfUtil, GetCompilationUnitsRejectsMalformedAbbreviation) {
  auto info = dwarf4Header();
  overwrite<uint32_t>(info, 0, 8);
  info.push_back(1);
  const std::vector<uint8_t> abbrev{0x80};
  DebugSections sections{};
  sections.debugInfo = info;
  sections.debugAbbrev = abbrev;

  auto result = getCompilationUnits(nullptr, sections, 0);

  EXPECT_EQ(info.size(), result.mainCompilationUnit.size);
  EXPECT_FALSE(result.splitCU.hasValue());
}

TEST(DwarfUtil, FindDefinitionDiePropagatesInvalidReference) {
  const uint32_t invalidOffset = 0x100;
  std::vector<uint8_t> info(sizeof(invalidOffset));
  std::memcpy(info.data(), &invalidOffset, sizeof(invalidOffset));
  const std::vector<uint8_t> attributes{
      DW_AT_specification, DW_FORM_ref4, 0, 0};
  CompilationUnit cu{};
  cu.debugSections.debugInfo = info;
  cu.size = static_cast<uint32_t>(info.size());
  Die die{
      .is64Bit = false,
      .attrOffset = 0,
      .offset = 0,
      .code = 1,
      .abbr =
          {
              .code = 1,
              .tag = DW_TAG_subprogram,
              .hasChildren = false,
              .attributes = attributes,
          },
  };

  auto result = findDefinitionDie(cu, die);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::invalid_offset, result.error().code);
}

TEST(DwarfUtil, ReadAttributeRejectsTruncatedFixedValue) {
  const std::vector<uint8_t> bytes{0x01, 0x02, 0x03};
  DwarfCursor cursor(bytes);
  CompilationUnit cu{};
  Die die{};

  auto result = readAttribute(
      cu,
      die,
      AttributeSpec{.name = DW_AT_decl_line, .form = DW_FORM_data4},
      cursor);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::truncated, result.error().code);
  EXPECT_EQ(size_t{0}, cursor.offset());
}

TEST(DwarfUtil, ReadAttributeRejectsTruncatedBlock) {
  const std::vector<uint8_t> bytes{4, 0x01, 0x02};
  DwarfCursor cursor(bytes);
  CompilationUnit cu{};
  Die die{};

  auto result = readAttribute(
      cu,
      die,
      AttributeSpec{.name = DW_AT_location, .form = DW_FORM_block1},
      cursor);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::truncated, result.error().code);
}

TEST(DwarfUtil, ReadAttributeRejectsUnterminatedString) {
  const std::vector<uint8_t> bytes{'f', 'o', 'o'};
  DwarfCursor cursor(bytes);
  CompilationUnit cu{};
  Die die{};

  auto result = readAttribute(
      cu,
      die,
      AttributeSpec{.name = DW_AT_name, .form = DW_FORM_string},
      cursor);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::unterminated_string, result.error().code);
}

TEST(DwarfUtil, ReadAttributeRejectsUnknownForm) {
  const std::vector<uint8_t> bytes;
  DwarfCursor cursor(bytes);
  CompilationUnit cu{};
  Die die{};

  auto result = readAttribute(
      cu,
      die,
      AttributeSpec{.name = DW_AT_name, .form = uint64_t{0xffff}},
      cursor);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::invalid_value, result.error().code);
}

TEST(DwarfUtil, ReadAttributeResolvesIndirectForm) {
  const std::vector<uint8_t> bytes{DW_FORM_data1, 0x2a};
  DwarfCursor cursor(bytes);
  CompilationUnit cu{};
  Die die{};

  auto result = readAttribute(
      cu,
      die,
      AttributeSpec{.name = DW_AT_decl_line, .form = DW_FORM_indirect},
      cursor);

  ASSERT_TRUE(result.hasValue());
  EXPECT_EQ(uint64_t{DW_FORM_data1}, result->spec.form);
  EXPECT_EQ(uint64_t{0x2a}, std::get<uint64_t>(result->attrValue));
}

TEST(DwarfUtil, ReadAttributeReadsEmbeddedValueForms) {
  // DW_FORM_flag_present and DW_FORM_implicit_const carry their value in the
  // form itself and consume no bytes; DW_FORM_sdata reads a single SLEB.
  {
    const std::vector<uint8_t> bytes;
    DwarfCursor cursor(bytes);
    CompilationUnit cu{};
    Die die{};

    auto result = readAttribute(
        cu,
        die,
        AttributeSpec{.name = DW_AT_external, .form = DW_FORM_flag_present},
        cursor);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(uint64_t{1}, std::get<uint64_t>(result->attrValue));
    EXPECT_EQ(0, cursor.offset());
  }
  {
    const std::vector<uint8_t> bytes;
    DwarfCursor cursor(bytes);
    CompilationUnit cu{};
    Die die{};

    auto result = readAttribute(
        cu,
        die,
        AttributeSpec{
            .name = DW_AT_decl_line,
            .form = DW_FORM_implicit_const,
            .implicitConst = -1},
        cursor);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(
        std::numeric_limits<uint64_t>::max(),
        std::get<uint64_t>(result->attrValue));
    EXPECT_EQ(0, cursor.offset());
  }
  {
    const std::vector<uint8_t> bytes{0x7f};
    DwarfCursor cursor(bytes);
    CompilationUnit cu{};
    Die die{};

    auto result = readAttribute(
        cu,
        die,
        AttributeSpec{.name = DW_AT_decl_line, .form = DW_FORM_sdata},
        cursor);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(
        std::numeric_limits<uint64_t>::max(),
        std::get<uint64_t>(result->attrValue));
  }
}

TEST(DwarfUtil, ForEachAttributeReturnsNextDieOffset) {
  const std::vector<uint8_t> info{'f', 'o', 'o', 0};
  const std::vector<uint8_t> attributes{DW_AT_name, DW_FORM_string, 0, 0};
  CompilationUnit cu{};
  cu.debugSections.debugInfo = info;
  cu.size = static_cast<uint32_t>(info.size());
  Die die{
      .abbr =
          {
              .attributes = attributes,
          },
  };
  folly::StringPiece name;

  auto result = forEachAttribute(cu, die, [&](const Attribute& attr) {
    name = std::get<folly::StringPiece>(attr.attrValue);
    return true;
  });

  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result->hasValue());
  EXPECT_EQ(info.size(), **result);
  EXPECT_EQ("foo", name);
}

TEST(DwarfUtil, ForEachAttributeReportsEarlyStop) {
  const std::vector<uint8_t> info{0x2a};
  const std::vector<uint8_t> attributes{DW_AT_decl_line, DW_FORM_data1, 0, 0};
  CompilationUnit cu{};
  cu.debugSections.debugInfo = info;
  cu.size = static_cast<uint32_t>(info.size());
  Die die{
      .abbr =
          {
              .attributes = attributes,
          },
  };

  auto result = forEachAttribute(cu, die, [](const Attribute&) {
    return false;
  });

  ASSERT_TRUE(result.hasValue());
  EXPECT_FALSE(result->hasValue());
}

TEST(DwarfUtil, GetAttributeRejectsTypeMismatch) {
  const std::vector<uint8_t> info{0x2a};
  const std::vector<uint8_t> attributes{DW_AT_name, DW_FORM_data1, 0, 0};
  CompilationUnit cu{};
  cu.debugSections.debugInfo = info;
  cu.size = static_cast<uint32_t>(info.size());
  Die die{
      .abbr =
          {
              .attributes = attributes,
          },
  };

  auto result = getAttribute<folly::StringPiece>(cu, die, DW_AT_name);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::invalid_value, result.error().code);
}

TEST(DwarfUtil, ReadAttributeRejectsInvalidStringIndex) {
  const uint32_t invalidStringOffset = 0x100;
  std::vector<uint8_t> stringOffsets(sizeof(invalidStringOffset));
  std::memcpy(
      stringOffsets.data(), &invalidStringOffset, sizeof(invalidStringOffset));
  const std::vector<uint8_t> strings{'x', 0};
  const std::vector<uint8_t> info{0};
  DwarfCursor cursor(info);
  CompilationUnit cu{};
  cu.debugSections.debugStrOffsets = stringOffsets;
  cu.debugSections.debugStr = strings;
  cu.strOffsetsBase = 0;
  Die die{};

  auto result = readAttribute(
      cu,
      die,
      AttributeSpec{.name = DW_AT_name, .form = DW_FORM_strx1},
      cursor);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::invalid_offset, result.error().code);
}

TEST(DwarfUtil, ReadAttributeUsesCompilationUnitAddressSize) {
  std::vector<uint8_t> addresses;
  append<uint32_t>(addresses, 0x12345678);
  const std::vector<uint8_t> info{0};
  DwarfCursor cursor(info);
  CompilationUnit cu{};
  cu.addrSize = sizeof(uint32_t);
  cu.addrBase = 0;
  cu.debugSections.debugAddr = addresses;
  Die die{};

  auto result = readAttribute(
      cu,
      die,
      AttributeSpec{.name = DW_AT_low_pc, .form = DW_FORM_addrx1},
      cursor);

  ASSERT_TRUE(result.hasValue());
  const auto* value = std::get_if<uint64_t>(&result->attrValue);
  ASSERT_NE(nullptr, value);
  EXPECT_EQ(uint64_t{0x12345678}, *value);
}

TEST(DwarfUtil, CollectsAddressBaseAfterIndexedLowPc) {
  struct AddressForm {
    uint64_t form;
    size_t width;
  };
  constexpr AddressForm kAddressForms[] = {
      {DW_FORM_addrx, 1},
      {DW_FORM_addrx1, 1},
      {DW_FORM_addrx2, 2},
      {DW_FORM_addrx3, 3},
      {DW_FORM_addrx4, 4},
      {DW_FORM_GNU_addr_index, 1},
  };
  constexpr uint32_t kAddressBase = 8;
  constexpr uintptr_t kLowPc = 0x2000;
  constexpr char kDirectory[] = "/build";

  std::vector<uint8_t> addresses;
  append<uint32_t>(addresses, 4 + 2 * sizeof(uintptr_t));
  append<uint16_t>(addresses, 5);
  append<uint8_t>(addresses, sizeof(uintptr_t));
  append<uint8_t>(addresses, 0);
  append<uintptr_t>(addresses, 0x1000);
  append<uintptr_t>(addresses, kLowPc);

  for (const auto& addressForm : kAddressForms) {
    SCOPED_TRACE(addressForm.form);
    std::vector<uint8_t> abbrev;
    appendULEB(abbrev, 1);
    appendULEB(abbrev, DW_TAG_compile_unit);
    append<uint8_t>(abbrev, DW_CHILDREN_no);
    appendULEB(abbrev, DW_AT_low_pc);
    appendULEB(abbrev, addressForm.form);
    appendULEB(abbrev, DW_AT_addr_base);
    appendULEB(abbrev, DW_FORM_sec_offset);
    appendULEB(abbrev, DW_AT_comp_dir);
    appendULEB(abbrev, DW_FORM_string);
    abbrev.insert(abbrev.end(), {0, 0, 0});

    std::vector<uint8_t> info;
    append<uint32_t>(info, 0);
    append<uint16_t>(info, 5);
    append<uint8_t>(info, DW_UT_compile);
    append<uint8_t>(info, sizeof(uintptr_t));
    append<uint32_t>(info, 0);
    appendULEB(info, 1);
    append<uint8_t>(info, 1); // index of kLowPc
    info.insert(info.end(), addressForm.width - 1, 0);
    append<uint32_t>(info, kAddressBase);
    info.insert(info.end(), std::begin(kDirectory), std::end(kDirectory));
    overwrite<uint32_t>(
        info, 0, folly::to_narrow(info.size() - sizeof(uint32_t)));

    DebugSections sections{};
    sections.debugInfo = info;
    sections.debugAbbrev = abbrev;
    sections.debugAddr = addresses;
    const auto units = getCompilationUnits(nullptr, sections, 0);
    const auto& cu = units.mainCompilationUnit;

    ASSERT_TRUE(cu.addrBase.hasValue());
    EXPECT_EQ(kAddressBase, *cu.addrBase);
    EXPECT_EQ(kDirectory, cu.compDir);
    auto die = getDieAtOffset(cu, cu.firstDie);
    ASSERT_TRUE(die.hasValue());
    auto lowPc = getAttribute<uint64_t>(cu, *die, DW_AT_low_pc);
    ASSERT_TRUE(lowPc.hasValue());
    ASSERT_TRUE(lowPc->hasValue());
    EXPECT_EQ(kLowPc, **lowPc);

    overwrite<uint8_t>(info, die->offset + die->attrOffset, 2);
    auto invalidLowPc = getAttribute<uint64_t>(cu, *die, DW_AT_low_pc);
    ASSERT_TRUE(invalidLowPc.hasError());
    EXPECT_EQ(DwarfParseErrorCode::invalid_offset, invalidLowPc.error().code);
  }
}

TEST(DwarfUtil, ReadAttributeRejectsMissingAddressBase) {
  const std::vector<uint8_t> addresses(sizeof(uint64_t));
  const std::vector<uint8_t> info{0};
  DwarfCursor cursor(info);
  CompilationUnit cu{};
  cu.addrSize = sizeof(uint64_t);
  cu.debugSections.debugAddr = addresses;
  Die die{};

  auto result = readAttribute(
      cu,
      die,
      AttributeSpec{.name = DW_AT_low_pc, .form = DW_FORM_addrx1},
      cursor);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::invalid_value, result.error().code);
}

TEST(DwarfUtil, FindsAddressInLegacyRangeList) {
  std::vector<uint8_t> ranges;
  append<uint64_t>(ranges, std::numeric_limits<uint64_t>::max());
  append<uint64_t>(ranges, 0x1000);
  append<uint64_t>(ranges, 0x10);
  append<uint64_t>(ranges, 0x20);
  append<uint64_t>(ranges, 0);
  append<uint64_t>(ranges, 0);
  const auto cu = rangeUnit(4, ranges, {});

  auto match = isAddressInRangeList(cu, 0x1015, folly::none, 0);
  auto miss = isAddressInRangeList(cu, 0x1020, folly::none, 0);

  ASSERT_TRUE(match.hasValue());
  EXPECT_TRUE(*match);
  ASSERT_TRUE(miss.hasValue());
  EXPECT_FALSE(*miss);
}

TEST(DwarfUtil, LegacyRangeDefaultsToZeroBase) {
  std::vector<uint8_t> ranges;
  append<uint64_t>(ranges, 0x10);
  append<uint64_t>(ranges, 0x20);
  append<uint64_t>(ranges, 0);
  append<uint64_t>(ranges, 0);
  const auto cu = rangeUnit(4, ranges, {});

  auto result = isAddressInRangeList(cu, 0x15, folly::none, 0);

  ASSERT_TRUE(result.hasValue());
  EXPECT_TRUE(*result);
}

TEST(DwarfUtil, RecoversWhenLegacyRangePrecedesBaseSelection) {
  std::vector<uint8_t> ranges;
  append<uint64_t>(ranges, 0x30);
  append<uint64_t>(ranges, 0x40);
  append<uint64_t>(ranges, std::numeric_limits<uint64_t>::max());
  append<uint64_t>(ranges, 0x1000);
  append<uint64_t>(ranges, 0x10);
  append<uint64_t>(ranges, 0x20);
  append<uint64_t>(ranges, 0);
  append<uint64_t>(ranges, 0);
  const auto cu = rangeUnit(4, ranges, {});

  auto result = isAddressInRangeList(cu, 0x1015, folly::none, 0);

  ASSERT_TRUE(result.hasValue());
  EXPECT_TRUE(*result);
}

TEST(DwarfUtil, RejectsEveryTruncatedLegacyRangeListPrefix) {
  std::vector<uint8_t> ranges;
  append<uint64_t>(ranges, 0x10);
  append<uint64_t>(ranges, 0x20);
  append<uint64_t>(ranges, 0);
  append<uint64_t>(ranges, 0);
  for (size_t size = 1; size < ranges.size(); ++size) {
    SCOPED_TRACE(size);
    auto cu = rangeUnit(4, std::span{ranges}.first(size), {});
    auto result = isAddressInRangeList(cu, 0x2000, uint64_t{0x1000}, 0);
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(DwarfParseErrorCode::truncated, result.error().code);
  }
}

TEST(DwarfUtil, RejectsLegacyRangeEndpointOverflow) {
  std::vector<uint8_t> ranges;
  append<uint64_t>(ranges, 0x20);
  append<uint64_t>(ranges, 0x30);
  append<uint64_t>(ranges, 0);
  append<uint64_t>(ranges, 0);
  const auto cu = rangeUnit(4, ranges, {});

  auto result = isAddressInRangeList(
      cu, 0, std::numeric_limits<uint64_t>::max() - 0x10, 0);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::integer_overflow, result.error().code);
}

TEST(DwarfUtil, FindsAddressInDirectDwarf5RangeEntries) {
  {
    std::vector<uint8_t> ranges{DW_RLE_start_end};
    append<uint64_t>(ranges, 0x1000);
    append<uint64_t>(ranges, 0x1020);
    ranges.push_back(DW_RLE_end_of_list);
    const auto cu = rangeUnit(5, {}, ranges);
    auto result = isAddressInRangeList(cu, 0x1010, folly::none, 0);
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(*result);
  }
  {
    std::vector<uint8_t> ranges{DW_RLE_start_length};
    append<uint64_t>(ranges, 0x1000);
    appendULEB(ranges, 0x20);
    ranges.push_back(DW_RLE_end_of_list);
    const auto cu = rangeUnit(5, {}, ranges);
    auto result = isAddressInRangeList(cu, 0x1010, folly::none, 0);
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(*result);
  }
  {
    std::vector<uint8_t> ranges{DW_RLE_base_address};
    append<uint64_t>(ranges, 0x1000);
    ranges.push_back(DW_RLE_offset_pair);
    appendULEB(ranges, 0x10);
    appendULEB(ranges, 0x20);
    ranges.push_back(DW_RLE_end_of_list);
    const auto cu = rangeUnit(5, {}, ranges);
    auto result = isAddressInRangeList(cu, 0x1015, folly::none, 0);
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(*result);
  }
  {
    std::vector<uint8_t> ranges{DW_RLE_start_end};
    append<uint32_t>(ranges, 0x1000);
    append<uint32_t>(ranges, 0x1020);
    ranges.push_back(DW_RLE_end_of_list);
    auto cu = rangeUnit(5, {}, ranges);
    cu.addrSize = sizeof(uint32_t);
    auto result = isAddressInRangeList(cu, 0x1010, folly::none, 0);
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(*result);
  }
}

TEST(DwarfUtil, FindsAddressInIndexedDwarf5RangeEntries) {
  std::vector<uint8_t> addresses;
  append<uint64_t>(addresses, 0x1000);
  append<uint64_t>(addresses, 0x1020);
  {
    std::vector<uint8_t> ranges{DW_RLE_startx_endx};
    appendULEB(ranges, 0);
    appendULEB(ranges, 1);
    ranges.push_back(DW_RLE_end_of_list);
    auto cu = rangeUnit(5, {}, ranges, addresses);
    cu.addrBase = 0;
    auto result = isAddressInRangeList(cu, 0x1010, folly::none, 0);
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(*result);
  }
  {
    std::vector<uint8_t> ranges{DW_RLE_startx_length};
    appendULEB(ranges, 0);
    appendULEB(ranges, 0x20);
    ranges.push_back(DW_RLE_end_of_list);
    auto cu = rangeUnit(5, {}, ranges, addresses);
    cu.addrBase = 0;
    auto result = isAddressInRangeList(cu, 0x1010, folly::none, 0);
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(*result);
  }
  {
    std::vector<uint8_t> ranges{DW_RLE_base_addressx};
    appendULEB(ranges, 0);
    ranges.push_back(DW_RLE_offset_pair);
    appendULEB(ranges, 0x10);
    appendULEB(ranges, 0x20);
    ranges.push_back(DW_RLE_end_of_list);
    auto cu = rangeUnit(5, {}, ranges, addresses);
    cu.addrBase = 0;
    auto result = isAddressInRangeList(cu, 0x1015, folly::none, 0);
    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(*result);
  }
}

TEST(DwarfUtil, RejectsEveryTruncatedDwarf5RangeListPrefix) {
  std::vector<uint8_t> addresses;
  append<uint64_t>(addresses, 0x1000);
  append<uint64_t>(addresses, 0x1020);
  std::vector<uint8_t> ranges{DW_RLE_base_addressx};
  appendULEB(ranges, 0);
  ranges.push_back(DW_RLE_startx_endx);
  appendULEB(ranges, 0);
  appendULEB(ranges, 1);
  ranges.push_back(DW_RLE_startx_length);
  appendULEB(ranges, 0);
  appendULEB(ranges, 1);
  ranges.push_back(DW_RLE_offset_pair);
  appendULEB(ranges, 1);
  appendULEB(ranges, 2);
  ranges.push_back(DW_RLE_base_address);
  append<uint64_t>(ranges, 0x2000);
  ranges.push_back(DW_RLE_start_end);
  append<uint64_t>(ranges, 0x3000);
  append<uint64_t>(ranges, 0x3001);
  ranges.push_back(DW_RLE_start_length);
  append<uint64_t>(ranges, 0x4000);
  appendULEB(ranges, 1);
  ranges.push_back(DW_RLE_end_of_list);

  for (size_t size = 1; size < ranges.size(); ++size) {
    SCOPED_TRACE(size);
    auto cu = rangeUnit(5, {}, std::span{ranges}.first(size), addresses);
    cu.addrBase = 0;
    auto result = isAddressInRangeList(cu, 0x5000, folly::none, 0);
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(DwarfParseErrorCode::truncated, result.error().code);
  }
}

TEST(DwarfUtil, RejectsInvalidDwarf5AddressIndex) {
  std::vector<uint8_t> addresses;
  append<uint64_t>(addresses, 0x1000);
  std::vector<uint8_t> ranges{DW_RLE_startx_endx};
  appendULEB(ranges, 0);
  appendULEB(ranges, 1);
  ranges.push_back(DW_RLE_end_of_list);
  auto cu = rangeUnit(5, {}, ranges, addresses);
  cu.addrBase = 0;

  auto result = isAddressInRangeList(cu, 0x1000, folly::none, 0);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::invalid_offset, result.error().code);
}

TEST(DwarfUtil, RejectsDwarf5OffsetPairWithoutBaseAddress) {
  std::vector<uint8_t> ranges{DW_RLE_offset_pair};
  appendULEB(ranges, 0x10);
  appendULEB(ranges, 0x20);
  ranges.push_back(DW_RLE_end_of_list);
  const auto cu = rangeUnit(5, {}, ranges);

  auto result = isAddressInRangeList(cu, 0x1015, folly::none, 0);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::invalid_value, result.error().code);
}

TEST(DwarfUtil, RejectsDwarf5RangeEndpointOverflow) {
  std::vector<uint8_t> ranges{DW_RLE_start_length};
  append<uint64_t>(ranges, std::numeric_limits<uint64_t>::max() - 1);
  appendULEB(ranges, 4);
  ranges.push_back(DW_RLE_end_of_list);
  const auto cu = rangeUnit(5, {}, ranges);

  auto result = isAddressInRangeList(cu, 0, folly::none, 0);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::integer_overflow, result.error().code);
}

TEST(DwarfUtil, RejectsIndexedDwarf5RangeEndpointOverflow) {
  std::vector<uint8_t> addresses;
  append<uint64_t>(addresses, std::numeric_limits<uint64_t>::max() - 1);
  std::vector<uint8_t> ranges{DW_RLE_startx_length};
  appendULEB(ranges, 0);
  appendULEB(ranges, 4);
  ranges.push_back(DW_RLE_end_of_list);
  auto cu = rangeUnit(5, {}, ranges, addresses);
  cu.addrBase = 0;

  auto result = isAddressInRangeList(cu, 0, folly::none, 0);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::integer_overflow, result.error().code);
}

TEST(DwarfUtil, RejectsDwarf5OffsetPairBaseOverflow) {
  std::vector<uint8_t> ranges{DW_RLE_base_address};
  append<uint64_t>(ranges, std::numeric_limits<uint64_t>::max() - 0x10);
  ranges.push_back(DW_RLE_offset_pair);
  appendULEB(ranges, 0x10);
  appendULEB(ranges, 0x20);
  ranges.push_back(DW_RLE_end_of_list);
  const auto cu = rangeUnit(5, {}, ranges);

  auto result = isAddressInRangeList(cu, 0, folly::none, 0);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::integer_overflow, result.error().code);
}

TEST(DwarfUtil, RejectsUnknownDwarf5RangeEntry) {
  const std::vector<uint8_t> ranges{0xff};
  const auto cu = rangeUnit(5, {}, ranges);

  auto result = isAddressInRangeList(cu, 0, folly::none, 0);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::invalid_value, result.error().code);
}

TEST(DwarfSection, Reads32And64BitChunks) {
  std::vector<uint8_t> bytes;
  append<uint32_t>(bytes, 2);
  bytes.push_back(0x11);
  bytes.push_back(0x22);
  append<uint32_t>(bytes, std::numeric_limits<uint32_t>::max());
  append<uint64_t>(bytes, 3);
  bytes.push_back(0x33);
  bytes.push_back(0x44);
  bytes.push_back(0x55);
  DwarfSection section(bytes);

  auto dwarf32 = section.next();
  ASSERT_TRUE(dwarf32.hasValue());
  ASSERT_TRUE(dwarf32->hasValue());
  EXPECT_FALSE(section.is64Bit());
  EXPECT_EQ(size_t{2}, (**dwarf32).size());
  EXPECT_EQ(uint8_t{0x11}, (**dwarf32)[0]);
  EXPECT_EQ(uint8_t{0x22}, (**dwarf32)[1]);

  auto dwarf64 = section.next();
  ASSERT_TRUE(dwarf64.hasValue());
  ASSERT_TRUE(dwarf64->hasValue());
  EXPECT_TRUE(section.is64Bit());
  EXPECT_EQ(size_t{3}, (**dwarf64).size());
  EXPECT_EQ(uint8_t{0x33}, (**dwarf64)[0]);
  EXPECT_EQ(uint8_t{0x55}, (**dwarf64)[2]);

  auto end = section.next();
  ASSERT_TRUE(end.hasValue());
  EXPECT_FALSE(end->hasValue());
}

TEST(DwarfSection, ReadsEmptySectionAsEnd) {
  const std::vector<uint8_t> bytes;
  DwarfSection section(bytes);

  auto end = section.next();

  ASSERT_TRUE(end.hasValue());
  EXPECT_FALSE(end->hasValue());
}

TEST(DwarfSection, RejectsTruncatedLengthHeader) {
  const std::vector<uint8_t> bytes{0x01, 0x02, 0x03};
  DwarfSection section(bytes);

  auto result = section.next();

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::truncated, result.error().code);
  EXPECT_EQ(size_t{0}, result.error().offset);
}

TEST(DwarfSection, RejectsTruncatedChunk) {
  std::vector<uint8_t> bytes;
  append<uint32_t>(bytes, 4);
  bytes.push_back(0x01);
  bytes.push_back(0x02);
  DwarfSection section(bytes);

  auto result = section.next();

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::truncated, result.error().code);
  EXPECT_EQ(size_t{4}, result.error().offset);
}

TEST(DwarfSection, RejectsReservedInitialLength) {
  std::vector<uint8_t> bytes;
  append<uint32_t>(bytes, 0xfffffff0);
  DwarfSection section(bytes);

  auto result = section.next();

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(DwarfParseErrorCode::invalid_value, result.error().code);
}

} // namespace
} // namespace folly::symbolizer

#endif
