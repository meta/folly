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

// DWARF record parser

#pragma once

#include <variant>

#include <folly/Function.h>
#include <folly/Range.h>
#include <folly/debugging/symbolizer/Elf.h>
#include <folly/debugging/symbolizer/ElfCache.h>
#include <folly/debugging/symbolizer/detail/DwarfConstants.h>

#if FOLLY_HAVE_DWARF && FOLLY_HAVE_ELF

namespace folly {
namespace symbolizer {

/**
 * More than one location info may exist if current frame is an inline
 * function call.
 */
const uint32_t kMaxInlineLocationInfoPerFrame = 20;

// Maximum number of DIEAbbreviation to cache in a compilation unit. Used to
// speed up inline function lookup.
const uint32_t kMaxAbbreviationEntries = 1000;

// A struct contains an Elf object and different debug sections.
struct DebugSections {
  const ElfFile* elf;
  std::span<const uint8_t> debugCuIndex; // .debug_cu_index
  std::span<const uint8_t> debugAbbrev; // .debug_abbrev
  std::span<const uint8_t> debugAddr; // .debug_addr (DWARF 5)
  std::span<const uint8_t> debugAranges; // .debug_aranges
  std::span<const uint8_t> debugInfo; // .debug_info
  std::span<const uint8_t> debugLine; // .debug_line
  std::span<const uint8_t> debugLineStr; // .debug_line_str (DWARF 5)
  std::span<const uint8_t> debugLoclists; // .debug_loclists (DWARF 5)
  std::span<const uint8_t> debugRanges; // .debug_ranges
  std::span<const uint8_t> debugRnglists; // .debug_rnglists (DWARF 5)
  std::span<const uint8_t> debugStr; // .debug_str
  std::span<const uint8_t> debugStrOffsets; // .debug_str_offsets (DWARF 5)
};

// Abbreviation for a Debugging Information Entry.
struct DIEAbbreviation {
  uint64_t code = 0;
  uint64_t tag = 0;
  bool hasChildren = false;
  folly::StringPiece attributes;
};

// A struct that contains the metadata of a compilation unit.
struct CompilationUnit {
  DebugSections debugSections;

  bool is64Bit = false;
  uint8_t version = 0;
  uint8_t unitType = DW_UT_compile; // DW_UT_compile or DW_UT_skeleton
  uint8_t addrSize = 0;
  // Offset in .debug_info of this compilation unit.
  uint32_t offset = 0;
  uint32_t size = 0;
  // Offset in .debug_info for the first DIE in this compilation unit.
  uint32_t firstDie = 0;
  folly::Optional<uint64_t> abbrevOffset;

  // Compilation directory.
  folly::StringPiece compDir = ".";
  // The beginning of the CU's contribution to .debug_addr
  // DW_AT_addr_base (DWARF 5, DebugFission)
  folly::Optional<uint64_t> addrBase;
  // The beginning of the CU's contribution to .debug_ranges
  // DW_AT_ranges_base (DebugFission)
  folly::Optional<uint64_t> rangesBase;
  // The beginning of the offsets table (immediately following the
  // header) of the CU's contribution to .debug_loclists
  folly::Optional<uint64_t> loclistsBase; // DW_AT_loclists_base (DWARF 5)
  // The beginning of the offsets table (immediately following the
  // header) of the CU's contribution to .debug_rnglists
  folly::Optional<uint64_t> rnglistsBase; // DW_AT_rnglists_base (DWARF 5)
  // Points to the first string offset of the compilation unit’s
  // contribution to the .debug_str_offsets (or .debug_str_offsets.dwo) section.
  folly::Optional<uint64_t> strOffsetsBase; // DW_AT_str_offsets_base (DWARF 5)

  // The actual dwo file and id contains the debug sections for this
  // compilation unit.
  folly::Optional<folly::StringPiece> dwoName;
  folly::Optional<uint64_t> dwoId;

  // Only the CompilationUnit that contains the caller functions needs this.
  // Indexed by (abbr.code - 1) if (abbr.code - 1) < abbrCache.size();
  folly::Range<DIEAbbreviation*> abbrCache;
};

// Contains the main compilation unit in the binary file and an optional
// compilation unit in dwo/dwp file if the main one is a skeleton.
struct CompilationUnits {
  CompilationUnit mainCompilationUnit;
  folly::Optional<CompilationUnit> splitCU;

  CompilationUnit& defaultCompilationUnit() {
    if (splitCU.hasValue()) {
      return *splitCU;
    }
    return mainCompilationUnit;
  }
};

// Debugging information entry to define a low-level representation of a
// source program. Each debugging information entry consists of an identifying
// tag and a series of attributes. An entry, or group of entries together,
// provide a description of a corresponding entity in the source program.
struct Die {
  bool is64Bit = false;
  // Offset from start to first attribute
  uint8_t attrOffset = 0;
  // Offset within debug info.
  uint32_t offset = 0;
  uint64_t code = 0;
  DIEAbbreviation abbr;
};

struct AttributeSpec {
  uint64_t name = 0;
  uint64_t form = 0;
  int64_t implicitConst = 0; // only set when form=DW_FORM_implicit_const

  explicit operator bool() const { return name != 0 || form != 0; }
};

struct Attribute {
  AttributeSpec spec;
  const Die& die;
  std::variant<uint64_t, folly::StringPiece> attrValue;
};

folly::StringPiece asStringPiece(std::span<const uint8_t> bytes) noexcept;

// Get an ELF section by name.
std::span<const uint8_t> getElfSection(const ElfFile* elf, const char* name);

// All following read* functions read from a StringPiece, advancing the
// StringPiece, and aborting if there's not enough room.

// Read (bitwise) one object of type T
template <class T>
typename std::enable_if<
    std::is_standard_layout<T>::value && std::is_trivial<T>::value,
    T>::type
read(folly::StringPiece& sp) {
  FOLLY_SAFE_CHECK(sp.size() >= sizeof(T), "underflow");
  T x;
  memcpy(&x, sp.data(), sizeof(T));
  sp.advance(sizeof(T));
  return x;
}

// Read (bitwise) an unsigned number of N bytes (N in 1, 2, 3, 4).
template <size_t N>
uint64_t readU64(folly::StringPiece& sp);

// Read ULEB (unsigned) varint value; algorithm from the DWARF spec
uint64_t readULEB(folly::StringPiece& sp, uint8_t& shift, uint8_t& val);
uint64_t readULEB(folly::StringPiece& sp);

// Read SLEB (signed) varint value; algorithm from the DWARF spec
int64_t readSLEB(folly::StringPiece& sp);

// Read a value of "section offset" type, which may be 4 or 8 bytes
uint64_t readOffset(folly::StringPiece& sp, bool is64Bit);

// Read "len" bytes
folly::StringPiece readBytes(folly::StringPiece& sp, uint64_t len);

AttributeSpec readAttributeSpec(folly::StringPiece& sp);

// Reads an abbreviation from a StringPiece, return true if at end; advance sp
bool readAbbreviation(folly::StringPiece& section, DIEAbbreviation& abbr);

// Read a null-terminated string
folly::StringPiece readNullTerminated(folly::StringPiece& sp);

folly::StringPiece getStringFromStringSection(
    folly::StringPiece str, uint64_t offset);

CompilationUnits getCompilationUnits(
    ElfCacheBase* elfCache,
    const DebugSections& debugSections,
    uint64_t offset,
    bool requireSplitDwarf = false);

/** cu must exist during the life cycle of created Die. */
Die getDieAtOffset(const CompilationUnit& cu, uint64_t offset);

// Read attribute value.
Attribute readAttribute(
    const CompilationUnit& cu,
    const Die& die,
    AttributeSpec spec,
    folly::StringPiece& info);

/*
 * Iterate over all attributes of the given DIE, calling the given callable
 * for each. Iteration is stopped early if any of the calls return false.
 */
size_t forEachAttribute(
    const CompilationUnit& cu,
    const Die& die,
    folly::FunctionRef<bool(const Attribute&)> f);

template <class T>
folly::Optional<T> getAttribute(
    const CompilationUnit& cu, const Die& die, uint64_t attrName) {
  folly::Optional<T> result;
  forEachAttribute(cu, die, [&](const Attribute& attr) {
    if (attr.spec.name == attrName) {
      result = std::get<T>(attr.attrValue);
      return false;
    }
    return true;
  });
  return result;
}

folly::StringPiece getFunctionNameFromDie(
    const CompilationUnit& srcu, const Die& die);

folly::StringPiece getFunctionName(
    const CompilationUnit& srcu, uint64_t dieOffset);

Die findDefinitionDie(const CompilationUnit& cu, const Die& die);

} // namespace symbolizer
} // namespace folly

#endif
