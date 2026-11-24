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

#include <array>
#include <cstring>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <folly/debugging/symbolizer/Elf.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <algorithm>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <folly/Conv.h>
#include <folly/CppAttributes.h>
#include <folly/File.h>
#include <folly/FileUtil.h>
#include <folly/String.h>
#include <folly/Subprocess.h>
#include <folly/debugging/symbolizer/detail/Debug.h>
#include <folly/portability/GTest.h>
#include <folly/testing/TestUtil.h>

#if FOLLY_HAVE_ELF

using folly::symbolizer::ElfEhdr;
using folly::symbolizer::ElfFile;
using folly::symbolizer::ElfNhdr;
using folly::symbolizer::ElfPhdr;
using folly::symbolizer::ElfShdr;
using folly::symbolizer::ElfSym;

// Add some symbols for testing. Note that we have to be careful with type
// signatures here to prevent name mangling
extern "C" {
[[gnu::used, FOLLY_ATTR_GNU_RETAIN]] uint64_t kIntegerValue = 1234567890ULL;
[[gnu::used, FOLLY_ATTR_GNU_RETAIN]] const char* kStringValue = "coconuts";
[[gnu::noinline, gnu::used, FOLLY_ATTR_GNU_RETAIN]] int sum_func(
    int lhs, int rhs) {
  return lhs + rhs;
}
[[gnu::noinline, gnu::used, FOLLY_ATTR_GNU_RETAIN]] int sub_func(
    int lhs, int rhs) {
  return lhs - rhs;
}
}

const char* const kDefaultElf = "/proc/self/exe";
class ElfTest : public ::testing::Test {
 protected:
  ElfFile elfFile_{kDefaultElf};
};

std::vector<uint8_t> relocatableElf(size_t sectionCount = 1) {
  ElfFile source(kDefaultElf);
  auto header = source.elfHeader();
  header.e_type = ET_REL;
  header.e_phoff = 0;
  header.e_phnum = 0;
  header.e_phentsize = 0;
  header.e_shoff = sizeof(ElfEhdr);
  header.e_shentsize = sizeof(ElfShdr);
  header.e_shnum = static_cast<decltype(header.e_shnum)>(sectionCount);
  header.e_shstrndx = SHN_UNDEF;

  std::vector<uint8_t> bytes(sizeof(header) + sectionCount * sizeof(ElfShdr));
  std::memcpy(bytes.data(), &header, sizeof(header));
  return bytes;
}

void overwriteHeader(std::vector<uint8_t>& bytes, const ElfEhdr& header) {
  ASSERT_GE(bytes.size(), sizeof(header));
  std::memcpy(bytes.data(), &header, sizeof(header));
}

ElfEhdr readHeader(const std::vector<uint8_t>& bytes) {
  EXPECT_GE(bytes.size(), sizeof(ElfEhdr));
  ElfEhdr header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  return header;
}

TEST_F(ElfTest, IntegerValue) {
  auto sym = elfFile_.getSymbolByName("kIntegerValue");
  EXPECT_NE(nullptr, sym.first) << "Failed to look up symbol kIntegerValue";
  EXPECT_EQ(kIntegerValue, *elfFile_.getSymbolValue<uint64_t>(sym.second));
}

TEST_F(ElfTest, UnalignedAddressValue) {
  auto sym = elfFile_.getSymbolByName("kIntegerValue");
  ASSERT_NE(nullptr, sym.first) << "Failed to look up symbol kIntegerValue";
  const auto* value =
      elfFile_.getAddressValue<uint32_t>(sym.second->st_value + 1);
  ASSERT_NE(nullptr, value);
  uint32_t expected;
  std::memcpy(
      &expected,
      reinterpret_cast<const char*>(&kIntegerValue) + 1,
      sizeof(expected));
  uint32_t actual;
  std::memcpy(&actual, value, sizeof(actual));
  EXPECT_EQ(expected, actual);
}

TEST_F(ElfTest, PointerValue) {
  auto sym = elfFile_.getSymbolByName("kStringValue");
  EXPECT_NE(nullptr, sym.first) << "Failed to look up symbol kStringValue";
  ElfW(Addr) addr = *elfFile_.getSymbolValue<ElfW(Addr)>(sym.second);
  // Let's check the address for the symbol against our own copy of
  // kStringValue.
  // For PIE binaries we need to adjust the address due to relocation.
  auto binaryOffset = folly::symbolizer::detail::get_r_debug()->r_map->l_addr;
  EXPECT_EQ(
      static_cast<const void*>(&kStringValue),
      reinterpret_cast<const void*>(binaryOffset + sym.second->st_value));
  if (binaryOffset == 0) { // non-PIE
    // Only do this check if we have a non-PIE. For the PIE case, the compiler
    // could put a 0 in the .data section for kStringValue, and then rely on
    // the dynamic linker to fill in the actual pointer to the .rodata section
    // via relocation; the actual offset of the string is encoded in the
    // .rela.dyn section, which isn't parsed in the current implementation of
    // ElfFile.
    const char* str = elfFile_.getAddressValue<const char>(addr);
    EXPECT_STREQ(kStringValue, str);
  }
}

TEST_F(ElfTest, SymbolByName) {
  auto sym = elfFile_.getSymbolByName("sum_func");
  EXPECT_NE(nullptr, sym.first) << "Failed to look up symbol sum_func";

  sym = elfFile_.getSymbolByName("sum_func", {STT_OBJECT});
  EXPECT_EQ(nullptr, sym.first) << "Unexpectedly look up of non-object symbol.";

  sym = elfFile_.getSymbolByName("sum_func", {STT_FUNC});
  EXPECT_NE(nullptr, sym.first)
      << "Failed to look up symbol sum_func when specifying type";

  sym = elfFile_.getSymbolByName("kIntegerValue", {STT_FUNC});
  EXPECT_EQ(nullptr, sym.first) << "Unexpected look up of non-func symbol.";

  sym = elfFile_.getSymbolByName("kIntegerValue", {STT_OBJECT});
  EXPECT_NE(nullptr, sym.first)
      << "Failed to look up symbol kIntegerValue when specifying type";
}

TEST_F(ElfTest, NullNamesReturnNoResults) {
  EXPECT_EQ(nullptr, elfFile_.getSectionByName(nullptr));
  const auto symbol = elfFile_.getSymbolByName(nullptr);
  EXPECT_EQ(nullptr, symbol.first);
  EXPECT_EQ(nullptr, symbol.second);
}

TEST_F(ElfTest, SymbolsByNameSuccess) {
  std::vector<std::string> names = {"sum_func", "sub_func"};
  auto result = elfFile_.getSymbolsByName(names, {STT_FUNC});

  EXPECT_EQ(names.size(), result.size());
  EXPECT_TRUE(result.contains("sum_func"));
  EXPECT_TRUE(result.contains("sub_func"));

  const auto& sumFuncSymbol = result.at("sum_func");
  EXPECT_NE(nullptr, sumFuncSymbol.first);
  EXPECT_NE(nullptr, sumFuncSymbol.second);

  const auto& subFuncSymbol = result.at("sub_func");
  EXPECT_NE(nullptr, subFuncSymbol.first);
  EXPECT_NE(nullptr, subFuncSymbol.second);
}

TEST_F(ElfTest, SymbolsByNamePartial) {
  std::vector<std::string> names = {"sum_func", "sub_func", "foo_func"};
  auto result = elfFile_.getSymbolsByName(names, {STT_FUNC});

  EXPECT_EQ(names.size(), result.size());
  EXPECT_TRUE(result.contains("sum_func"));
  EXPECT_TRUE(result.contains("sub_func"));
  EXPECT_TRUE(result.contains("foo_func"));

  const auto& fooFuncSymbol = result.at("foo_func");
  EXPECT_EQ(nullptr, fooFuncSymbol.first);
  EXPECT_EQ(nullptr, fooFuncSymbol.second);
}

TEST_F(ElfTest, SymbolsByNamePropagatesAllocationFailure) {
  struct FailingSymbolName {
    [[noreturn]] operator std::string() const { throw std::bad_alloc(); }
  };
  const std::array<FailingSymbolName, 1> names{};

  EXPECT_THROW(elfFile_.getSymbolsByName(names), std::bad_alloc);
}

TEST_F(ElfTest, SymbolsByNameEmpty) {
  std::vector<std::string> names;
  auto result = elfFile_.getSymbolsByName(names, {STT_FUNC});

  EXPECT_EQ(0, result.size());
  EXPECT_EQ(names.size(), result.size());
}

TEST_F(ElfTest, iterateProgramHeaders) {
  auto phdr = elfFile_.iterateProgramHeaders([](auto& h) {
    return h.p_type == PT_LOAD;
  });
  EXPECT_NE(nullptr, phdr);
  EXPECT_GE(phdr->p_filesz, 0);
  auto body = elfFile_.getSegmentBody(*phdr);
  EXPECT_EQ(body.size(), phdr->p_filesz);
}

TEST_F(ElfTest, NoProgramHeadersReturnsNull) {
  auto bytes = relocatableElf();
  auto header = readHeader(bytes);
  header.e_phnum = 0;
  overwriteHeader(bytes, header);
  folly::test::TemporaryFile tmpFile;
  folly::writeFull(tmpFile.fd(), bytes.data(), bytes.size());
  ElfFile elfFile;
  ASSERT_EQ(ElfFile::kSuccess, elfFile.openNoThrow(tmpFile.path().c_str()));

  EXPECT_EQ(nullptr, elfFile.iterateProgramHeaders([](const ElfPhdr&) {
    return true;
  }));
}

TEST_F(ElfTest, TinyNonElfFile) {
  folly::test::TemporaryFile tmpFile;
  const static folly::StringPiece contents = "!";
  folly::writeFull(tmpFile.fd(), contents.data(), contents.size());

  ElfFile elfFile;
  auto res = elfFile.openNoThrow(tmpFile.path().c_str());
  EXPECT_EQ(ElfFile::kInvalidElfFile, res.code);
  EXPECT_STREQ("not an ELF file (too short)", res.msg);
}

TEST_F(ElfTest, TruncatedElfHeaderIsRejected) {
  folly::test::TemporaryFile tmpFile;
  const std::array<uint8_t, SELFMAG> contents{
      ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3};
  folly::writeFull(tmpFile.fd(), contents.data(), contents.size());

  ElfFile elfFile;
  const auto result = elfFile.openNoThrow(tmpFile.path().c_str());

  EXPECT_EQ(ElfFile::kInvalidElfFile, result.code);
  EXPECT_STREQ("not an ELF file (too short)", result.msg);
}

TEST_F(ElfTest, ThrowingOpenReportsMalformedElf) {
  folly::test::TemporaryFile tmpFile;
  const std::array<uint8_t, SELFMAG> contents{
      ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3};
  folly::writeFull(tmpFile.fd(), contents.data(), contents.size());

  EXPECT_THROW(ElfFile(tmpFile.path().c_str()), std::runtime_error);
}

TEST_F(ElfTest, InvalidExtendedSectionMetadataReturnsNoSections) {
  auto bytes = relocatableElf();
  auto header = readHeader(bytes);
  header.e_shnum = 0;
  header.e_shoff = bytes.size() - 1;
  header.e_shstrndx = SHN_XINDEX;
  overwriteHeader(bytes, header);
  folly::test::TemporaryFile tmpFile;
  folly::writeFull(tmpFile.fd(), bytes.data(), bytes.size());
  ElfFile elfFile;
  ASSERT_EQ(ElfFile::kSuccess, elfFile.openNoThrow(tmpFile.path().c_str()));

  EXPECT_EQ(nullptr, elfFile.getSectionByIndex(0));
  EXPECT_EQ(nullptr, elfFile.getSectionByName(".text"));
  EXPECT_EQ(nullptr, elfFile.iterateSections([](const ElfShdr&) {
    return true;
  }));
}

TEST_F(ElfTest, InvalidSectionIndexReturnsNull) {
  const auto bytes = relocatableElf();
  folly::test::TemporaryFile tmpFile;
  folly::writeFull(tmpFile.fd(), bytes.data(), bytes.size());
  ElfFile elfFile;
  ASSERT_EQ(ElfFile::kSuccess, elfFile.openNoThrow(tmpFile.path().c_str()));

  EXPECT_EQ(nullptr, elfFile.getSectionByIndex(1));
  EXPECT_EQ(
      nullptr, elfFile.getSectionByIndex(std::numeric_limits<size_t>::max()));
}

TEST_F(ElfTest, OutOfBoundsSectionAndSegmentBodiesAreEmpty) {
  const auto bytes = relocatableElf();
  folly::test::TemporaryFile tmpFile;
  folly::writeFull(tmpFile.fd(), bytes.data(), bytes.size());
  ElfFile elfFile;
  ASSERT_EQ(ElfFile::kSuccess, elfFile.openNoThrow(tmpFile.path().c_str()));
  ElfShdr section{};
  section.sh_type = SHT_PROGBITS;
  section.sh_offset = bytes.size();
  section.sh_size = 1;
  ElfPhdr segment{};
  segment.p_offset = bytes.size();
  segment.p_filesz = 1;

  EXPECT_TRUE(elfFile.getSectionBody(section).empty());
  EXPECT_TRUE(elfFile.getSegmentBody(segment).empty());
}

TEST_F(ElfTest, MalformedStringTablesReturnNull) {
  const auto bytes = relocatableElf();
  folly::test::TemporaryFile tmpFile;
  folly::writeFull(tmpFile.fd(), bytes.data(), bytes.size());
  ElfFile elfFile;
  ASSERT_EQ(ElfFile::kSuccess, elfFile.openNoThrow(tmpFile.path().c_str()));
  ElfShdr stringTable{};
  stringTable.sh_type = SHT_STRTAB;
  stringTable.sh_offset = bytes.size();
  stringTable.sh_size = 1;

  EXPECT_EQ(nullptr, elfFile.getString(stringTable, 0));
  stringTable.sh_offset = sizeof(ElfEhdr);
  EXPECT_EQ(nullptr, elfFile.getString(stringTable, 1));
  stringTable.sh_type = SHT_PROGBITS;
  EXPECT_EQ(nullptr, elfFile.getString(stringTable, 0));
}

TEST_F(ElfTest, InvalidSectionEntrySizeReturnsNull) {
  const auto bytes = relocatableElf();
  folly::test::TemporaryFile tmpFile;
  folly::writeFull(tmpFile.fd(), bytes.data(), bytes.size());
  ElfFile elfFile;
  ASSERT_EQ(ElfFile::kSuccess, elfFile.openNoThrow(tmpFile.path().c_str()));
  ElfShdr section{};
  section.sh_entsize = 1;
  section.sh_size = 1;

  EXPECT_EQ(
      nullptr,
      elfFile.iterateSectionEntries<ElfSym>(section, [](const ElfSym&) {
        return false;
      }));
}

TEST_F(ElfTest, EmptySectionEntriesReturnNull) {
  const auto bytes = relocatableElf();
  folly::test::TemporaryFile tmpFile;
  folly::writeFull(tmpFile.fd(), bytes.data(), bytes.size());
  ElfFile elfFile;
  ASSERT_EQ(ElfFile::kSuccess, elfFile.openNoThrow(tmpFile.path().c_str()));
  ElfShdr section{};
  section.sh_entsize = sizeof(ElfSym);
  section.sh_size = 0;

  EXPECT_EQ(
      nullptr,
      elfFile.iterateSectionEntries<ElfSym>(section, [](const ElfSym&) {
        return false;
      }));
}

TEST_F(ElfTest, SectionEntriesIgnoreTrailingPartialEntry) {
  auto bytes = relocatableElf();
  const auto entriesOffset = bytes.size();
  bytes.resize(entriesOffset + sizeof(ElfSym) + 1);
  folly::test::TemporaryFile tmpFile;
  folly::writeFull(tmpFile.fd(), bytes.data(), bytes.size());
  ElfFile elfFile;
  ASSERT_EQ(ElfFile::kSuccess, elfFile.openNoThrow(tmpFile.path().c_str()));
  ElfShdr section{};
  section.sh_offset = entriesOffset;
  section.sh_size = sizeof(ElfSym) + 1;
  section.sh_entsize = sizeof(ElfSym);
  size_t entries = 0;

  elfFile.iterateSectionEntries<ElfSym>(section, [&](const ElfSym&) {
    ++entries;
    return false;
  });

  EXPECT_EQ(1, entries);
}

TEST_F(ElfTest, SectionAddressRangeDoesNotOverflow) {
  auto bytes = relocatableElf();
  ElfShdr section{};
  section.sh_addr = std::numeric_limits<ElfW(Addr)>::max() - 1;
  section.sh_size = 4;
  std::memcpy(bytes.data() + sizeof(ElfEhdr), &section, sizeof(section));
  folly::test::TemporaryFile tmpFile;
  folly::writeFull(tmpFile.fd(), bytes.data(), bytes.size());
  ElfFile elfFile;
  ASSERT_EQ(ElfFile::kSuccess, elfFile.openNoThrow(tmpFile.path().c_str()));

  EXPECT_NE(
      nullptr,
      elfFile.getSectionContainingAddress(
          std::numeric_limits<ElfW(Addr)>::max()));
}

TEST_F(ElfTest, NonElfScript) {
  folly::test::TemporaryFile tmpFile;
  const static folly::StringPiece contents =
      "#!/bin/sh\necho I'm small non-ELF executable\n";
  folly::writeFull(tmpFile.fd(), contents.data(), contents.size());

  ElfFile elfFile;
  auto res = elfFile.openNoThrow(tmpFile.path().c_str());
  EXPECT_EQ(ElfFile::kInvalidElfFile, res.code);
  EXPECT_STREQ("invalid ELF magic", res.msg);
}

TEST_F(ElfTest, FailToOpenLargeFilename) {
  // ElfFile used to segfault if it failed to open large filenames
  folly::test::TemporaryDirectory tmpDir;
  auto elfFile = std::make_unique<ElfFile>(); // on the heap so asan can see it
  auto const largeNonExistingName = (tmpDir.path() / std::string(1000, 'x'));
  ASSERT_EQ(
      ElfFile::kSystemError,
      elfFile->openNoThrow(largeNonExistingName.c_str()));
  ASSERT_EQ(
      ElfFile::kSystemError,
      elfFile->openNoThrow(largeNonExistingName.c_str()));
  EXPECT_EQ(ElfFile::kSuccess, elfFile->openNoThrow(kDefaultElf));
}

TEST(TestGetNoteGnuBuildId, SimpleElf) {
  auto const file =
      folly::test::find_resource("folly/debugging/symbolizer/test/simple_elf");
  ASSERT_TRUE(std::filesystem::exists(file.c_str())) << file.c_str();
  ElfFile elfFile = ElfFile(file.c_str());
  auto noteMaybe = elfFile.getNoteGnuBuildId();
  ASSERT_TRUE(([&]() {
    return noteMaybe.hasError()
        ? testing::AssertionFailure()
            << ElfFile::FindNoteError::getErrorMessage(noteMaybe.error())
        : testing::AssertionSuccess();
  })());

  auto note = noteMaybe.value();
  EXPECT_EQ(4, note.size());
  EXPECT_THAT(note, ::testing::ElementsAreArray({0xDE, 0xAD, 0xBE, 0xEF}));
}

TEST(TestNoteParsing, UnpaddedFinalDescriptor) {
  constexpr std::array<uint8_t, 6> kBuildId{0xba, 0xdf, 0xac, 0xed, 0xec, 0xaf};
  alignas(ElfNhdr) std::array<uint8_t, sizeof(ElfNhdr) + 4 + kBuildId.size()>
      bytes{};
  const ElfNhdr header{
      .n_namesz = 4,
      .n_descsz = kBuildId.size(),
      .n_type = NT_GNU_BUILD_ID,
  };
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + sizeof(header), "GNU", 4);
  std::memcpy(bytes.data() + sizeof(header) + 4, kBuildId.data(), 6);

  auto result = ElfFile::Note::parse(bytes);

  ASSERT_TRUE(result.hasValue());
  EXPECT_EQ(bytes.size(), result->size());
  EXPECT_EQ("GNU", std::string_view(result->getName()));
  EXPECT_THAT(result->getDesc(), ::testing::ElementsAreArray(kBuildId));
}

TEST(TestNoteParsing, DescriptorExcludesPadding) {
  constexpr std::array<uint8_t, 6> kBuildId{0xba, 0xdf, 0xac, 0xed, 0xec, 0xaf};
  alignas(ElfNhdr) std::array<uint8_t, sizeof(ElfNhdr) + 4 + 8> bytes{};
  const ElfNhdr header{
      .n_namesz = 4,
      .n_descsz = kBuildId.size(),
      .n_type = NT_GNU_BUILD_ID,
  };
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + sizeof(header), "GNU", 4);
  std::memcpy(bytes.data() + sizeof(header) + 4, kBuildId.data(), 6);

  auto result = ElfFile::Note::parse(bytes);

  ASSERT_TRUE(result.hasValue());
  EXPECT_EQ(bytes.size(), result->size());
  EXPECT_THAT(result->getDesc(), ::testing::ElementsAreArray(kBuildId));
}

TEST(TestNoteParsing, RejectsUnterminatedName) {
  alignas(ElfNhdr) std::array<uint8_t, sizeof(ElfNhdr) + 4> bytes{};
  const ElfNhdr header{
      .n_namesz = 4,
      .n_descsz = 0,
      .n_type = NT_GNU_BUILD_ID,
  };
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + sizeof(header), "GNU!", 4);

  auto result = ElfFile::Note::parse(bytes);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(
      ElfFile::FindNoteFailureCode::NoteUndersized, result.error().failureCode);
}

TEST(TestNoteParsing, RejectsTruncatedDescriptor) {
  // n_descsz claims 16 bytes, but only 6 follow the name.
  constexpr std::array<uint8_t, 6> kBuildId{0xba, 0xdf, 0xac, 0xed, 0xec, 0xaf};
  alignas(ElfNhdr) std::array<uint8_t, sizeof(ElfNhdr) + 4 + kBuildId.size()>
      bytes{};
  const ElfNhdr header{
      .n_namesz = 4,
      .n_descsz = 16,
      .n_type = NT_GNU_BUILD_ID,
  };
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + sizeof(header), "GNU", 4);
  std::memcpy(
      bytes.data() + sizeof(header) + 4, kBuildId.data(), kBuildId.size());

  auto result = ElfFile::Note::parse(bytes);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(
      ElfFile::FindNoteFailureCode::NoteUndersized, result.error().failureCode);
}

TEST(TestNoteParsing, RejectsPartialDescriptorPadding) {
  // A 6-byte descriptor needs 2 padding bytes; only 1 trailing byte follows.
  constexpr std::array<uint8_t, 6> kBuildId{0xba, 0xdf, 0xac, 0xed, 0xec, 0xaf};
  alignas(ElfNhdr)
      std::array<uint8_t, sizeof(ElfNhdr) + 4 + kBuildId.size() + 1>
          bytes{};
  const ElfNhdr header{
      .n_namesz = 4,
      .n_descsz = kBuildId.size(),
      .n_type = NT_GNU_BUILD_ID,
  };
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + sizeof(header), "GNU", 4);
  std::memcpy(
      bytes.data() + sizeof(header) + 4, kBuildId.data(), kBuildId.size());

  auto result = ElfFile::Note::parse(bytes);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(
      ElfFile::FindNoteFailureCode::NoteUndersized, result.error().failureCode);
}

TEST(TestNoteParsing, RejectsUnalignedHeader) {
  alignas(ElfNhdr) std::array<uint8_t, sizeof(ElfNhdr) + 1> bytes{};
  const auto unaligned = std::span<const uint8_t>(bytes).subspan(1);

  const auto result = ElfFile::Note::parse(unaligned);

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(
      ElfFile::FindNoteFailureCode::NoteUnaligned, result.error().failureCode);
}

TEST(TestNoteParsing, AcceptsNullPaddedName) {
  constexpr std::array<uint8_t, 4> kBuildId{0xde, 0xad, 0xbe, 0xef};
  alignas(ElfNhdr) std::array<uint8_t, sizeof(ElfNhdr) + 8 + kBuildId.size()>
      bytes{};
  const ElfNhdr header{
      .n_namesz = 8,
      .n_descsz = kBuildId.size(),
      .n_type = NT_GNU_BUILD_ID,
  };
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + sizeof(header), "GNU", 3);
  std::memcpy(
      bytes.data() + sizeof(header) + 8, kBuildId.data(), kBuildId.size());

  auto result = ElfFile::Note::parse(bytes);

  ASSERT_TRUE(result.hasValue());
  EXPECT_EQ("GNU", std::string_view(result->getName()));
  EXPECT_THAT(result->getDesc(), ::testing::ElementsAreArray(kBuildId));
}

TEST(TestNoteSectionIteration, SimpleElf) {
  auto const file =
      folly::test::find_resource("folly/debugging/symbolizer/test/simple_elf");
  EXPECT_TRUE(std::filesystem::exists(file.c_str())) << file.c_str();
  ElfFile elfFile = ElfFile(file.c_str());
  const auto* shdr = elfFile.getSectionByName(".note.gnu.build-id");
  EXPECT_NE(nullptr, shdr);

  std::vector<folly::Expected<ElfFile::Note, ElfFile::FindNoteError>>
      noteMaybes;
  noteMaybes.emplace_back(
      elfFile.iterateNotesInSections(shdr, [](const ElfFile::Note& note) {
        if (note.header()->n_type != NT_GNU_BUILD_ID) {
          return false;
        }

        auto expectedSize = folly::align_ceil(note.header()->n_namesz, 4) +
            folly::align_ceil(note.header()->n_descsz, 4);
        return note.body().size() == expectedSize;
      }));

  noteMaybes.emplace_back(
      elfFile.iterateNotesInSections(nullptr, [](const ElfFile::Note& note) {
        if (note.header()->n_type != NT_GNU_BUILD_ID) {
          return false;
        }

        auto expectedSize = folly::align_ceil(note.header()->n_namesz, 4) +
            folly::align_ceil(note.header()->n_descsz, 4);
        return note.body().size() == expectedSize;
      }));

  for (auto& noteMaybe : noteMaybes) {
    ASSERT_TRUE(([&]() {
      return noteMaybe.hasError()
          ? testing::AssertionFailure()
              << ElfFile::FindNoteError::getErrorMessage(noteMaybe.error())
          : testing::AssertionSuccess();
    })());

    ElfFile::Note note = *noteMaybe;
    ASSERT_NE(nullptr, note.header());
    EXPECT_EQ(note.getName(), "GNU");
    EXPECT_THAT(
        note.getDesc(), ::testing::ElementsAreArray({0xDE, 0xAD, 0xBE, 0xEF}));
  }
}

TEST(TestNoteSegmentIteration, SimpleElf) {
  auto const file =
      folly::test::find_resource("folly/debugging/symbolizer/test/simple_elf");
  EXPECT_TRUE(std::filesystem::exists(file.c_str())) << file.c_str();
  ElfFile elfFile = ElfFile(file.c_str());

  // PRStatus should always be in the segments
  auto noteMaybe =
      elfFile.iterateNotesInSegments(nullptr, [](const ElfFile::Note& note) {
        return note.header()->n_type == NT_PRSTATUS;
      });

  ASSERT_TRUE(([&]() {
    return noteMaybe.hasError()
        ? testing::AssertionFailure()
            << ElfFile::FindNoteError::getErrorMessage(noteMaybe.error())
        : testing::AssertionSuccess();
  })());

  auto note = *noteMaybe;
  ASSERT_NE(nullptr, note.header());
  EXPECT_EQ(note.getName(), "GNU");
  EXPECT_NE(0, note.getDesc().size());
}

TEST(TestNoteMultipleNoteSections, SimpleElf) {
  auto const file =
      folly::test::find_resource("folly/debugging/symbolizer/test/simple_elf");
  EXPECT_TRUE(std::filesystem::exists(file.c_str())) << file.c_str();
  ElfFile elfFile = ElfFile(file.c_str());
  std::vector<ElfFile::Note> notes;
  elfFile.iterateNotesInSections(nullptr, [&](const ElfFile::Note& note) {
    notes.push_back(note);
    return false;
  });

  EXPECT_NE(0, notes.size());
  for (const auto& noteEntry : notes) {
    EXPECT_NE(nullptr, noteEntry.header());
    EXPECT_NE(0, noteEntry.body().size());
    EXPECT_NE("", noteEntry.getName());
    EXPECT_NE(0, noteEntry.getDesc().size());
  }
}

TEST(TestShnXindex, LargeSectionCount) {
  // Synthetic ELF with 65536 sections, triggering SHN_XINDEX encoding:
  //   e_shnum = 0     (real count in section 0's sh_size)
  //   e_shstrndx = 0xFFFF  (real index in section 0's sh_link)
  // Verifies getSectionByName works when the section header string table
  // index requires SHN_XINDEX indirection.
  auto const file = folly::test::find_resource(
      "folly/debugging/symbolizer/test/test-xindex.o");
  ASSERT_TRUE(std::filesystem::exists(file.c_str())) << file.c_str();

  ElfFile elfFile;
  auto res = elfFile.openNoThrow(file.c_str());
  ASSERT_EQ(ElfFile::kSuccess, res.code) << res.msg;

  // .debug_str is at section index 65534 in the synthetic ELF.
  // If SHN_XINDEX is not handled, getSectionByName will fail because
  // e_shstrndx=0xFFFF is used as a literal index (out of bounds) and
  // e_shnum=0 makes iterateSections a no-op.
  const auto* debugStr = elfFile.getSectionByName(".debug_str");
  ASSERT_NE(nullptr, debugStr)
      << ".debug_str not found (SHN_XINDEX handling broken)";
  EXPECT_EQ(debugStr->sh_type, SHT_PROGBITS);
  EXPECT_GT(debugStr->sh_size, 0);

  const auto* shstrtab = elfFile.getSectionByName(".shstrtab");
  ASSERT_NE(nullptr, shstrtab)
      << ".shstrtab not found (SHN_XINDEX handling broken)";
  EXPECT_EQ(shstrtab->sh_type, SHT_STRTAB);
}

TEST(TestNoteParsing, SimpleElf) {
  uint8_t headerOnly[sizeof(ElfNhdr)];
  ElfNhdr header;
  header.n_namesz = 8;
  header.n_descsz = 4;
  memcpy(&headerOnly, &header, sizeof(ElfNhdr));
  auto noteMaybe = ElfFile::Note::parse(
      std::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(&headerOnly), sizeof(ElfNhdr)));
  EXPECT_TRUE(([&]() {
    return noteMaybe.hasError()
        ? testing::AssertionSuccess()
        : testing::AssertionFailure();
  })());
  auto& err = noteMaybe.error();
  EXPECT_TRUE(err.isDataCorruptionError());
  EXPECT_EQ(err.failureCode, ElfFile::FindNoteFailureCode::NoteUndersized);

  uint8_t smallerThanHeader[6];
  noteMaybe = ElfFile::Note::parse(
      std::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(&smallerThanHeader), 6));
  EXPECT_TRUE(([&]() {
    return noteMaybe.hasError()
        ? testing::AssertionSuccess()
        : testing::AssertionFailure();
  })());
  err = noteMaybe.error();
  EXPECT_TRUE(err.isDataCorruptionError());
  EXPECT_EQ(err.failureCode, ElfFile::FindNoteFailureCode::NoteUndersized);
}

TEST(TestFindNote, SimpleElf) {
  auto const file =
      folly::test::find_resource("folly/debugging/symbolizer/test/simple_elf");

  EXPECT_TRUE(std::filesystem::exists(file.c_str())) << file.c_str();
  ElfFile elfFile = ElfFile(file.c_str());
  const auto* shdr = elfFile.getSectionByName(".note.gnu.build-id");
  EXPECT_NE(nullptr, shdr);

  // There are multiple notes whose names are "GNU"
  // to avoid flakiness we just check the name. But below when we iterate
  // by type, we check the type and the name.
  std::string noteName = "GNU";
  {
    auto noteMaybe = elfFile.findNoteByName(noteName);
    ASSERT_TRUE(([&]() {
      return noteMaybe.hasError()
          ? testing::AssertionFailure()
              << ElfFile::FindNoteError::getErrorMessage(noteMaybe.error())
          : testing::AssertionSuccess();
    })());

    EXPECT_EQ(noteName, noteMaybe->getName());
  }
  {
    auto noteMaybe = elfFile.findNoteByType(NT_GNU_BUILD_ID);
    ASSERT_TRUE(([&]() {
      return noteMaybe.hasError()
          ? testing::AssertionFailure()
              << ElfFile::FindNoteError::getErrorMessage(noteMaybe.error())
          : testing::AssertionSuccess();
    })());

    EXPECT_EQ(noteName, noteMaybe->getName());
    EXPECT_EQ(NT_GNU_BUILD_ID, noteMaybe->header()->n_type);
  }
  {
    auto noteMaybe = elfFile.findNoteByType(NT_PRSTATUS);
    ASSERT_TRUE(([&]() {
      return noteMaybe.hasError()
          ? testing::AssertionFailure()
              << ElfFile::FindNoteError::getErrorMessage(noteMaybe.error())
          : testing::AssertionSuccess();
    })());

    EXPECT_EQ(noteName, noteMaybe->getName());
    EXPECT_EQ(NT_PRSTATUS, noteMaybe->header()->n_type);
  }
  {
    auto noteMaybe =
        elfFile.findNoteByType(std::numeric_limits<uint32_t>::max());
    EXPECT_TRUE(noteMaybe.hasError());
    EXPECT_FALSE(noteMaybe.error().isDataCorruptionError());
  }
  {
    auto noteMaybe = elfFile.findNoteByName("NOT_A_NOTE");
    EXPECT_TRUE(noteMaybe.hasError());
    EXPECT_FALSE(noteMaybe.error().isDataCorruptionError());
  }
}

namespace {

// This process's fds that are open on the file at `path`, in ascending order.
std::vector<int> fdsOpenOn(const char* path) {
  struct stat target{};
  std::vector<int> fds;
  if (::stat(path, &target) != 0) {
    return fds;
  }
  for (const auto& entry :
       std::filesystem::directory_iterator("/proc/self/fd")) {
    const int fd = std::stoi(entry.path().filename().string());
    struct stat st{};
    if (::fstat(fd, &st) == 0 && st.st_dev == target.st_dev &&
        st.st_ino == target.st_ino) {
      fds.push_back(fd);
    }
  }
  std::sort(fds.begin(), fds.end());
  return fds;
}

// What `fd` refers to in a child spawned (fork and exec) now: empty if the
// child did not inherit it.
std::string fdTargetInChild(int fd) {
  folly::Subprocess child(
      {"/bin/readlink", folly::to<std::string>("/proc/self/fd/", fd)},
      folly::Subprocess::Options().pipeStdout().stderrFd(
          folly::Subprocess::DEV_NULL));
  const auto target = child.communicate().first;
  child.wait();
  return folly::rtrimWhitespace(target).str();
}

} // namespace

// The symbolizer's ELF cache keeps its files open for the life of the
// process, so a helper the process later forks and execs must not inherit
// them.
TEST(ElfFileFd, NotInheritedByChildProcesses) {
  const auto binary = std::filesystem::read_symlink(kDefaultElf).string();
  {
    // The probe sees a descriptor a child inherits, and only that one.
    const folly::File inherited(kDefaultElf, O_RDONLY);
    EXPECT_EQ(binary, fdTargetInChild(inherited.fd()));
    const folly::File closeOnExec(kDefaultElf, O_RDONLY | O_CLOEXEC);
    EXPECT_EQ("", fdTargetInChild(closeOnExec.fd()));
  }

  const auto before = fdsOpenOn(kDefaultElf);
  ElfFile elfFile(kDefaultElf);
  const auto after = fdsOpenOn(kDefaultElf);
  std::vector<int> opened;
  std::set_difference(
      after.begin(),
      after.end(),
      before.begin(),
      before.end(),
      std::back_inserter(opened));
  ASSERT_EQ(1, opened.size());

  EXPECT_EQ("", fdTargetInChild(opened[0]));
}

#endif
