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

#include <folly/debugging/symbolizer/DwarfCursor.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

#include <folly/portability/GTest.h>

namespace folly::symbolizer {
namespace {

template <typename T>
void expectError(
    const DwarfParseResult<T>& result,
    DwarfParseErrorCode code,
    size_t offset) {
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ((DwarfParseError{code, offset}), result.error());
}

TEST(DwarfCursor, TotalSizeIncludesConsumedBytes) {
  constexpr std::array<uint8_t, 8> kInput{};
  DwarfCursor cursor(kInput);

  EXPECT_EQ(8, cursor.totalSize());
  EXPECT_EQ(8, cursor.remaining().size());
  EXPECT_FALSE(cursor.empty());

  auto prefix = cursor.readBytes(4);
  ASSERT_TRUE(prefix.hasValue());
  EXPECT_EQ(8, cursor.totalSize());
  EXPECT_EQ(4, cursor.remaining().size());
  EXPECT_FALSE(cursor.empty());

  auto suffix = cursor.readBytes(4);
  ASSERT_TRUE(suffix.hasValue());
  EXPECT_EQ(8, cursor.totalSize());
  EXPECT_EQ(0, cursor.remaining().size());
  EXPECT_TRUE(cursor.empty());
}

TEST(DwarfCursor, ReadsFixedWidthValues) {
  const uint32_t input = 0x12345678;
  DwarfCursor cursor(
      std::span{reinterpret_cast<const uint8_t*>(&input), sizeof(input)});

  auto value = cursor.read<uint32_t>();

  ASSERT_TRUE(value.hasValue());
  EXPECT_EQ(input, *value);
  EXPECT_EQ(sizeof(input), cursor.offset());
  EXPECT_TRUE(cursor.empty());
}

TEST(DwarfCursor, FixedWidthTruncationDoesNotAdvance) {
  const std::array<uint8_t, 2> input{0x12, 0x34};
  DwarfCursor cursor(input);

  auto value = cursor.read<uint32_t>();

  expectError(value, DwarfParseErrorCode::truncated, 0);
  EXPECT_EQ(0, cursor.offset());
  EXPECT_EQ(input.data(), cursor.remaining().data());
  EXPECT_EQ(input.size(), cursor.remaining().size());
}

TEST(DwarfCursor, ReportsOffsetsRelativeToContainingSection) {
  const std::array<uint8_t, 1> input{0x12};
  DwarfCursor cursor(input, 0x40);

  auto byte = cursor.read<uint8_t>();
  auto truncated = cursor.read<uint8_t>();

  ASSERT_TRUE(byte.hasValue());
  EXPECT_EQ(0x12, *byte);
  EXPECT_EQ(0x41, cursor.offset());
  expectError(truncated, DwarfParseErrorCode::truncated, 0x41);
}

TEST(DwarfCursor, ReadsPartialWidthUnsignedValue) {
  const std::array<uint8_t, 3> input{0x12, 0x34, 0x56};
  uint64_t expected = 0;
  std::memcpy(&expected, input.data(), input.size());
  DwarfCursor cursor(input);

  auto value = cursor.readU64<3>();

  ASSERT_TRUE(value.hasValue());
  EXPECT_EQ(expected, *value);
  EXPECT_TRUE(cursor.empty());
}

TEST(DwarfCursor, ReadsBytesAndTracksErrorOffset) {
  const std::array<uint8_t, 3> input{0x12, 0x34, 0x56};
  DwarfCursor cursor(input);

  auto prefix = cursor.readBytes(2);
  ASSERT_TRUE(prefix.hasValue());
  EXPECT_EQ(input.data(), prefix->data());
  EXPECT_EQ(2, prefix->size());

  auto value = cursor.read<uint16_t>();
  expectError(value, DwarfParseErrorCode::truncated, 2);
  EXPECT_EQ(2, cursor.offset());
}

TEST(DwarfCursor, ReadsSectionOffsets) {
  const uint32_t dwarf32Offset = 0x12345678;
  const uint64_t dwarf64Offset = 0x123456789abcdef0;
  std::array<uint8_t, sizeof(dwarf32Offset) + sizeof(dwarf64Offset)> input{};
  std::memcpy(input.data(), &dwarf32Offset, sizeof(dwarf32Offset));
  std::memcpy(
      input.data() + sizeof(dwarf32Offset),
      &dwarf64Offset,
      sizeof(dwarf64Offset));
  DwarfCursor cursor(input);

  auto value32 = cursor.readOffset(false);
  auto value64 = cursor.readOffset(true);

  ASSERT_TRUE(value32.hasValue());
  EXPECT_EQ(dwarf32Offset, *value32);
  ASSERT_TRUE(value64.hasValue());
  EXPECT_EQ(dwarf64Offset, *value64);
}

TEST(DwarfCursor, TruncatedSectionOffsetDoesNotAdvance) {
  constexpr std::array<uint8_t, 3> kInput{0xff, 0x12, 0x34};
  DwarfCursor cursor(kInput);
  ASSERT_TRUE(cursor.read<uint8_t>().hasValue());

  auto value = cursor.readOffset(false);

  expectError(value, DwarfParseErrorCode::truncated, 1);
  EXPECT_EQ(1, cursor.offset());
  EXPECT_EQ(kInput.data() + 1, cursor.remaining().data());
  EXPECT_EQ(2, cursor.remaining().size());
}

TEST(DwarfCursor, ReadsULEB) {
  const std::array<uint8_t, 3> input{0xe5, 0x8e, 0x26};
  DwarfCursor cursor(input);

  auto value = cursor.readULEB();

  ASSERT_TRUE(value.hasValue());
  EXPECT_EQ(624485, *value);
  EXPECT_TRUE(cursor.empty());
}

TEST(DwarfCursor, RejectsTruncatedULEBWithoutAdvancing) {
  const std::array<uint8_t, 1> input{0x80};
  DwarfCursor cursor(input);

  auto value = cursor.readULEB();

  expectError(value, DwarfParseErrorCode::truncated, 0);
  EXPECT_EQ(0, cursor.offset());
}

TEST(DwarfCursor, ReadsMaximumULEB) {
  const std::array<uint8_t, 10> input{
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01};
  DwarfCursor cursor(input);

  auto value = cursor.readULEB();

  ASSERT_TRUE(value.hasValue());
  EXPECT_EQ(std::numeric_limits<uint64_t>::max(), *value);
}

TEST(DwarfCursor, RejectsOverflowingULEBWithoutAdvancing) {
  const std::array<uint8_t, 10> input{
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02};
  DwarfCursor cursor(input);

  auto value = cursor.readULEB();

  expectError(value, DwarfParseErrorCode::integer_overflow, 0);
  EXPECT_EQ(0, cursor.offset());
}

TEST(DwarfCursor, ReadsSLEB) {
  const std::array<uint8_t, 3> input{0x9b, 0xf1, 0x59};
  DwarfCursor cursor(input);

  auto value = cursor.readSLEB();

  ASSERT_TRUE(value.hasValue());
  EXPECT_EQ(-624485, *value);
  EXPECT_TRUE(cursor.empty());
}

TEST(DwarfCursor, RejectsOverflowingSLEBWithoutAdvancing) {
  const std::array<uint8_t, 10> input{
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
  DwarfCursor cursor(input);

  auto value = cursor.readSLEB();

  expectError(value, DwarfParseErrorCode::integer_overflow, 0);
  EXPECT_EQ(0, cursor.offset());
}

TEST(DwarfCursor, ReadsNullTerminatedString) {
  const std::array<uint8_t, 5> input{'f', 'o', 'o', 0, 0x42};
  DwarfCursor cursor(input);

  auto value = cursor.readCString();

  ASSERT_TRUE(value.hasValue());
  EXPECT_EQ("foo", std::string_view(*value));
  EXPECT_EQ(4, cursor.offset());
  EXPECT_EQ(0x42, cursor.remaining().front());
}

TEST(DwarfCursor, RejectsUnterminatedStringWithoutAdvancing) {
  const std::array<uint8_t, 3> input{'f', 'o', 'o'};
  DwarfCursor cursor(input);

  auto value = cursor.readCString();

  expectError(value, DwarfParseErrorCode::unterminated_string, 0);
  EXPECT_EQ(0, cursor.offset());
}

TEST(DwarfCursor, RejectsContinuedTenthULEBByteWithoutAdvancing) {
  const std::array<uint8_t, 10> input{
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
  DwarfCursor cursor(input);

  auto value = cursor.readULEB();

  expectError(value, DwarfParseErrorCode::integer_overflow, 0);
  EXPECT_EQ(0, cursor.offset());
}

TEST(DwarfCursor, RejectsTruncatedSLEBWithoutAdvancing) {
  const std::array<uint8_t, 1> input{0x80};
  DwarfCursor cursor(input);

  auto value = cursor.readSLEB();

  expectError(value, DwarfParseErrorCode::truncated, 0);
  EXPECT_EQ(0, cursor.offset());
}

TEST(DwarfCursor, ReadsMinimumSLEB) {
  const std::array<uint8_t, 10> input{
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x7f};
  DwarfCursor cursor(input);

  auto value = cursor.readSLEB();

  ASSERT_TRUE(value.hasValue());
  EXPECT_EQ(std::numeric_limits<int64_t>::min(), *value);
  EXPECT_TRUE(cursor.empty());
}

} // namespace
} // namespace folly::symbolizer
