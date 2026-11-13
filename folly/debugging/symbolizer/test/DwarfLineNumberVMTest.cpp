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

#include <folly/debugging/symbolizer/DwarfLineNumberVM.h>

#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include <folly/Varint.h>
#include <folly/lang/cstring_view.h>
#include <folly/portability/GTest.h>

#if FOLLY_HAVE_DWARF && FOLLY_HAVE_ELF

namespace folly::symbolizer {
namespace {

constexpr uintptr_t kStartAddress = 0x1000;

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

void appendCString(std::vector<uint8_t>& bytes, folly::cstring_view value) {
  if (!value.empty()) {
    bytes.insert(bytes.end(), value.begin(), value.end());
  }
  bytes.push_back(0);
}

void appendStandardOpcodeLengths(std::vector<uint8_t>& header) {
  constexpr uint8_t kLengths[] = {0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1};
  header.insert(header.end(), std::begin(kLengths), std::end(kLengths));
}

void appendAddressMapping(std::vector<uint8_t>& program, uint64_t fileIndex) {
  program.push_back(0);
  appendULEB(program, sizeof(uintptr_t) + 1);
  program.push_back(DW_LNE_set_address);
  append<uintptr_t>(program, kStartAddress);
  program.push_back(DW_LNS_set_file);
  appendULEB(program, fileIndex);
  program.push_back(DW_LNS_copy);
  program.push_back(DW_LNS_advance_pc);
  appendULEB(program, 0x10);
  program.push_back(0);
  appendULEB(program, 1);
  program.push_back(DW_LNE_end_sequence);
}

std::vector<uint8_t> lineProgram(uint64_t fileIndex) {
  std::vector<uint8_t> program;
  appendAddressMapping(program, fileIndex);
  return program;
}

std::vector<uint8_t> lineProgramWithDefinedFile() {
  std::vector<uint8_t> program;
  std::vector<uint8_t> definition{DW_LNE_define_file};
  appendCString(definition, "generated.cpp");
  appendULEB(definition, 1); // directory index
  appendULEB(definition, 0); // modified time
  appendULEB(definition, 0); // file size
  program.push_back(0);
  appendULEB(program, definition.size());
  program.insert(program.end(), definition.begin(), definition.end());
  appendAddressMapping(program, 2);
  return program;
}

std::vector<uint8_t> frameLineTable(
    uint16_t version,
    std::vector<uint8_t> header,
    std::span<const uint8_t> program) {
  std::vector<uint8_t> payload;
  append<uint16_t>(payload, version);
  if (version == 5) {
    append<uint8_t>(payload, sizeof(uintptr_t));
    append<uint8_t>(payload, 0);
  }
  append<uint32_t>(payload, static_cast<uint32_t>(header.size()));
  payload.insert(payload.end(), header.begin(), header.end());
  payload.insert(payload.end(), program.begin(), program.end());

  std::vector<uint8_t> table;
  append<uint32_t>(table, static_cast<uint32_t>(payload.size()));
  table.insert(table.end(), payload.begin(), payload.end());
  return table;
}

std::vector<uint8_t> dwarf4LineTable(
    std::span<const uint8_t> program,
    uint8_t lineRange = 14,
    uint64_t fileDirectoryIndex = 1) {
  std::vector<uint8_t> header;
  append<uint8_t>(header, 1); // minimum instruction length
  append<uint8_t>(header, 1); // maximum operations per instruction
  append<uint8_t>(header, 1); // default is_stmt
  append<int8_t>(header, -5); // line base
  append<uint8_t>(header, lineRange);
  append<uint8_t>(header, 13); // opcode base
  appendStandardOpcodeLengths(header);
  appendCString(header, "src");
  appendCString(header, "");
  appendCString(header, "main.cpp");
  appendULEB(header, fileDirectoryIndex); // directory index
  appendULEB(header, 0); // modified time
  appendULEB(header, 0); // file size
  appendCString(header, "");
  return frameLineTable(4, std::move(header), program);
}

std::vector<uint8_t> dwarf5LineTable(
    std::span<const uint8_t> program, bool mismatchedFilePath = false) {
  std::vector<uint8_t> header;
  append<uint8_t>(header, 1); // minimum instruction length
  append<uint8_t>(header, 1); // maximum operations per instruction
  append<uint8_t>(header, 1); // default is_stmt
  append<int8_t>(header, -5); // line base
  append<uint8_t>(header, 14); // line range
  append<uint8_t>(header, 13); // opcode base
  appendStandardOpcodeLengths(header);

  append<uint8_t>(header, 1); // directory format count
  appendULEB(header, DW_LNCT_path);
  appendULEB(header, DW_FORM_string);
  appendULEB(header, 1); // directory count
  appendCString(header, "src");

  append<uint8_t>(header, 2); // file format count
  appendULEB(header, DW_LNCT_path);
  appendULEB(header, mismatchedFilePath ? DW_FORM_data1 : DW_FORM_string);
  appendULEB(header, DW_LNCT_directory_index);
  appendULEB(header, DW_FORM_data1);
  appendULEB(header, 1); // file count
  if (mismatchedFilePath) {
    append<uint8_t>(header, 7);
  } else {
    appendCString(header, "main.cpp");
  }
  append<uint8_t>(header, 0); // directory index
  return frameLineTable(5, std::move(header), program);
}

void expectNoLocation(std::span<const uint8_t> data) {
  DebugSections sections{};
  DwarfLineNumberVM vm(data, "/build", sections);
  Path file("keep", "this", "path");
  uint64_t line = 99;

  EXPECT_FALSE(vm.findAddress(kStartAddress, file, line));
  EXPECT_EQ("keep", file.baseDir());
  EXPECT_EQ("this", file.subDir());
  EXPECT_EQ("path", file.file());
  EXPECT_EQ(99, line);
}

TEST(DwarfLineNumberVM, FindsAddressInDwarf4LineTable) {
  const auto program = lineProgram(1);
  const auto table = dwarf4LineTable(program);
  DebugSections sections{};
  DwarfLineNumberVM vm(table, "/build", sections);
  Path file;
  uint64_t line = 0;

  ASSERT_TRUE(vm.findAddress(kStartAddress, file, line));
  EXPECT_EQ("/build", file.baseDir());
  EXPECT_EQ("src", file.subDir());
  EXPECT_EQ("main.cpp", file.file());
  EXPECT_EQ(1, line);
}

TEST(DwarfLineNumberVM, FindsAddressInDwarf4CompilationDirectory) {
  const auto program = lineProgram(1);
  const auto table = dwarf4LineTable(program, 14, 0);
  DebugSections sections{};
  DwarfLineNumberVM vm(table, "/build", sections);
  Path file;
  uint64_t line = 0;

  ASSERT_TRUE(vm.findAddress(kStartAddress, file, line));
  EXPECT_EQ("/build", file.baseDir());
  EXPECT_TRUE(file.subDir().empty());
  EXPECT_EQ("main.cpp", file.file());
  EXPECT_EQ(1, line);
}

TEST(DwarfLineNumberVM, FindsAddressInDwarf5LineTable) {
  const auto program = lineProgram(0);
  const auto table = dwarf5LineTable(program);
  DebugSections sections{};
  DwarfLineNumberVM vm(table, "/ignored", sections);
  Path file;
  uint64_t line = 0;

  ASSERT_TRUE(vm.findAddress(kStartAddress, file, line));
  EXPECT_EQ("src", file.baseDir());
  EXPECT_TRUE(file.subDir().empty());
  EXPECT_EQ("main.cpp", file.file());
  EXPECT_EQ(1, line);
}

TEST(DwarfLineNumberVM, FindsFileDefinedByDwarf4LineProgram) {
  const auto program = lineProgramWithDefinedFile();
  const auto table = dwarf4LineTable(program);
  DebugSections sections{};
  DwarfLineNumberVM vm(table, "/build", sections);
  Path file;
  uint64_t line = 0;

  ASSERT_TRUE(vm.findAddress(kStartAddress, file, line));
  EXPECT_EQ("/build", file.baseDir());
  EXPECT_EQ("src", file.subDir());
  EXPECT_EQ("generated.cpp", file.file());
  EXPECT_EQ(1, line);
}

TEST(DwarfLineNumberVM, RejectsEveryTruncatedPrefix) {
  const auto program = lineProgram(0);
  const auto complete = dwarf5LineTable(program);
  for (size_t payloadSize = 0; payloadSize + sizeof(uint32_t) < complete.size();
       ++payloadSize) {
    SCOPED_TRACE(payloadSize);
    std::vector<uint8_t> truncated(
        complete.begin(), complete.begin() + sizeof(uint32_t) + payloadSize);
    overwrite<uint32_t>(truncated, 0, static_cast<uint32_t>(payloadSize));
    expectNoLocation(truncated);
  }
}

TEST(DwarfLineNumberVM, RejectsZeroLineRange) {
  const auto program = lineProgram(1);
  expectNoLocation(dwarf4LineTable(program, 0));
}

TEST(DwarfLineNumberVM, RejectsTruncatedStandardOpcodeOperand) {
  const std::vector<uint8_t> program{DW_LNS_advance_pc, 0x80};
  expectNoLocation(dwarf4LineTable(program));
}

TEST(DwarfLineNumberVM, RejectsAddressOverflow) {
  std::vector<uint8_t> program{DW_LNS_advance_pc};
  appendULEB(program, std::numeric_limits<uint64_t>::max());
  program.push_back(DW_LNS_advance_pc);
  appendULEB(program, 1);
  expectNoLocation(dwarf4LineTable(program));
}

TEST(DwarfLineNumberVM, RejectsOversizedExtendedOpcode) {
  const std::vector<uint8_t> program{0, 2, DW_LNE_set_address};
  expectNoLocation(dwarf4LineTable(program));
}

TEST(DwarfLineNumberVM, RejectsInvalidFileIndex) {
  const auto program = lineProgram(2);
  expectNoLocation(dwarf4LineTable(program));
}

TEST(DwarfLineNumberVM, RejectsInvalidFileDirectoryIndex) {
  const auto program = lineProgram(1);
  expectNoLocation(dwarf4LineTable(program, 14, 5));
}

TEST(DwarfLineNumberVM, RejectsLineTableValueTypeMismatch) {
  const auto program = lineProgram(0);
  expectNoLocation(dwarf5LineTable(program, true));
}

TEST(DwarfLineNumberVM, RejectsDeprecatedDwarf5DefineFile) {
  const auto program = lineProgramWithDefinedFile();
  expectNoLocation(dwarf5LineTable(program));
}

} // namespace
} // namespace folly::symbolizer

#endif
