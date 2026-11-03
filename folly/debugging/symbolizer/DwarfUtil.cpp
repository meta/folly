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

#include <array>
#include <type_traits>

#include <folly/Optional.h>
#include <folly/Range.h>
#include <folly/debugging/symbolizer/Elf.h>
#include <folly/debugging/symbolizer/detail/DwarfConstants.h>
#include <folly/portability/Config.h>
#include <folly/portability/Unistd.h>

#if FOLLY_HAVE_DWARF && FOLLY_HAVE_ELF

namespace folly {
namespace symbolizer {

folly::StringPiece asStringPiece(std::span<const uint8_t> bytes) noexcept {
  return folly::StringPiece(
      reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::span<const uint8_t> getElfSection(const ElfFile* elf, const char* name) {
  const ElfShdr* elfSection = elf->getSectionByName(name);
  if (!elfSection) {
    return {};
  }
#ifdef SHF_COMPRESSED
  if (elfSection->sh_flags & SHF_COMPRESSED) {
    return {};
  }
#endif
  return elf->getSectionBody(*elfSection);
}

// Read (bitwise) an unsigned number of N bytes (N in 1, 2, 3, 4).
template <size_t N>
uint64_t readU64(std::span<const uint8_t>& bytes) {
  static_assert(N >= 1 && N <= sizeof(uint64_t), "unsupported width");
  FOLLY_SAFE_CHECK(bytes.size() >= N, "underflow");
  uint64_t x = 0;
  memcpy(&x, bytes.data(), N);
  bytes = bytes.subspan(N);
  return x;
}

template <size_t N>
uint64_t readU64(folly::StringPiece& sp) {
  FOLLY_SAFE_CHECK(sp.size() >= N, "underflow");
  uint64_t x = 0;
  memcpy(&x, sp.data(), N);
  sp.advance(N);
  return x;
}

uint64_t readULEB(
    std::span<const uint8_t>& bytes, uint8_t& shift, uint8_t& val) {
  uint64_t r = 0;
  shift = 0;
  // A 64-bit ULEB needs at most 10 bytes. More continuation bytes mean
  // malformed input, and shifting by 70 or more would be undefined.
  for (int i = 0; i < 10; ++i) {
    val = read<uint8_t>(bytes);
    r |= ((uint64_t)(val & 0x7f) << shift);
    shift = static_cast<uint8_t>(shift + 7);
    if (!(val & 0x80)) {
      return r;
    }
  }
  FOLLY_SAFE_CHECK(false, "ULEB overflow");
  return 0;
}

uint64_t readULEB(std::span<const uint8_t>& bytes) {
  uint8_t shift;
  uint8_t val;
  return readULEB(bytes, shift, val);
}

// Read ULEB (unsigned) varint value; algorithm from the DWARF spec
uint64_t readULEB(folly::StringPiece& sp, uint8_t& shift, uint8_t& val) {
  uint64_t r = 0;
  shift = 0;
  do {
    val = read<uint8_t>(sp);
    r |= ((uint64_t)(val & 0x7f) << shift);
    shift += 7;
  } while (val & 0x80);
  return r;
}

uint64_t readULEB(folly::StringPiece& sp) {
  uint8_t shift;
  uint8_t val;
  return readULEB(sp, shift, val);
}

// Read SLEB (signed) varint value; algorithm from the DWARF spec
int64_t readSLEB(folly::StringPiece& sp) {
  uint8_t shift;
  uint8_t val;
  uint64_t r = readULEB(sp, shift, val);

  if (shift < 64 && (val & 0x40)) {
    r |= -(1ULL << shift); // sign extend
  }

  return r;
}

int64_t readSLEB(std::span<const uint8_t>& bytes) {
  uint8_t shift;
  uint8_t val;
  uint64_t r = readULEB(bytes, shift, val);

  if (shift < 64 && (val & 0x40)) {
    r |= -(1ULL << shift); // sign extend
  }

  return r;
}

// Read a value of "section offset" type, which may be 4 or 8 bytes
uint64_t readOffset(folly::StringPiece& sp, bool is64Bit) {
  return is64Bit ? read<uint64_t>(sp) : read<uint32_t>(sp);
}

uint64_t readOffset(std::span<const uint8_t>& bytes, bool is64Bit) {
  return is64Bit ? read<uint64_t>(bytes) : read<uint32_t>(bytes);
}

// Read "len" bytes
folly::StringPiece readBytes(folly::StringPiece& sp, uint64_t len) {
  FOLLY_SAFE_CHECK(len <= sp.size(), "invalid string length");
  folly::StringPiece ret(sp.data(), len);
  sp.advance(len);
  return ret;
}

// Read a null-terminated string
folly::StringPiece readNullTerminated(folly::StringPiece& sp) {
  const char* p = static_cast<const char*>(memchr(sp.data(), 0, sp.size()));
  FOLLY_SAFE_CHECK(p, "invalid null-terminated string");
  folly::StringPiece ret(sp.data(), p);
  sp.assign(p + 1, sp.end());
  return ret;
}

folly::StringPiece getStringFromStringSection(
    folly::StringPiece str, uint64_t offset) {
  FOLLY_SAFE_CHECK(offset < str.size(), "invalid string offset");
  str.advance(offset);
  return readNullTerminated(str);
}

AttributeSpec readAttributeSpec(folly::StringPiece& sp) {
  AttributeSpec spec;
  spec.name = readULEB(sp);
  spec.form = readULEB(sp);
  if (spec.form == DW_FORM_implicit_const) {
    spec.implicitConst = readSLEB(sp);
  }
  return spec;
}

// Reads an abbreviation from a StringPiece, return true if at end; advance sp
bool readAbbreviation(folly::StringPiece& section, DIEAbbreviation& abbr) {
  // Abbreviation code
  abbr.code = readULEB(section);
  if (abbr.code == 0) {
    return false;
  }

  // Abbreviation tag
  abbr.tag = readULEB(section);

  // does this entry have children?
  abbr.hasChildren = (read<uint8_t>(section) != DW_CHILDREN_no);

  // attributes
  const char* attributeBegin = section.data();
  for (;;) {
    if (section.empty()) {
      FOLLY_SAFE_DFATAL("invalid attribute section");
      return false;
    }
    auto spec = readAttributeSpec(section);
    if (!spec) {
      break;
    }
  }
  abbr.attributes.assign(attributeBegin, section.data());
  return true;
}

namespace {

DIEAbbreviation getAbbreviation(
    folly::StringPiece debugAbbrev, uint64_t code, uint64_t offset) {
  // Linear search in the .debug_abbrev section, starting at offset
  debugAbbrev.advance(offset);

  DIEAbbreviation abbr;
  while (readAbbreviation(debugAbbrev, abbr)) {
    if (abbr.code == code) {
      return abbr;
    }
  }

  FOLLY_SAFE_DFATAL("could not find abbreviation code");
  return {};
}

// Parse compilation unit info, abbrev and str_offsets offset from
// .debug_cu_index section.
bool findCompiliationOffset(
    folly::StringPiece debugCuIndex, uint64_t dwoId, CompilationUnit& cu) {
  if (debugCuIndex.empty()) {
    return false;
  }

  // v4: GCC Debug Fission defines version as an unsigned 4B field with value
  // of 2.
  // https://gcc.gnu.org/wiki/DebugFissionDWP#Format_of_the_CU_and_TU_Index_Sections
  //
  // v5: DWARF5 7.3.5.3 Format of the CU and TU Index Sections
  // The first 2 header entries are version (2B, value=5) and padding (2B,
  // value=0)
  //
  // Both were read into version (4B) and on little endian will have value=5.
  auto version = read<uint32_t>(debugCuIndex);
  if (version != 2 && version != (5 << (kIsLittleEndian ? 0 : 16))) {
    return false;
  }
  auto numColumns = read<uint32_t>(debugCuIndex);
  read<uint32_t>(debugCuIndex);
  auto numBuckets = read<uint32_t>(debugCuIndex);

  // Get index of the CU matches the given dwo id.
  folly::StringPiece signatureSection = debugCuIndex;
  folly::StringPiece indexesSection = debugCuIndex;
  indexesSection.advance(numBuckets * sizeof(uint64_t));
  ssize_t idx = -1;
  for (unsigned i = 0; i < numBuckets; i++) {
    uint64_t hash = read<uint64_t>(signatureSection);
    uint32_t index = read<uint32_t>(indexesSection);
    if (hash == dwoId) {
      idx = index;
      break;
    }
  }
  if (idx <= 0) {
    return false;
  }

  // Skip signature and parallel table of indexes
  debugCuIndex.advance(numBuckets * sizeof(uint64_t));
  debugCuIndex.advance(numBuckets * sizeof(uint32_t));

  // column headers
  ssize_t infoSectionIndex = -1;
  ssize_t abbrevSectionIndex = -1;
  ssize_t strOffsetsSectionIndex = -1;
  ssize_t rnglistsSectionIndex = -1;
  for (unsigned i = 0; i != numColumns; ++i) {
    auto index = read<uint32_t>(debugCuIndex);
    if (index == DW_SECT_INFO) {
      infoSectionIndex = i;
    } else if (index == DW_SECT_ABBREV) {
      abbrevSectionIndex = i;
    } else if (index == DW_SECT_STR_OFFSETS) {
      strOffsetsSectionIndex = i;
    } else if (index == DW_SECT_RNGLISTS) {
      rnglistsSectionIndex = i;
    }
  }

  // DW_SECT_RNGLISTS/rnglistsSectionIndex only appears in DWARF5 .dwp, not in
  // DWARF4 .dwp
  if (infoSectionIndex == -1 || abbrevSectionIndex == -1 ||
      strOffsetsSectionIndex == -1) {
    return false;
  }

  // debugCuIndex offsets
  debugCuIndex.advance((idx - 1) * numColumns * sizeof(uint32_t));
  for (unsigned i = 0; i != numColumns; ++i) {
    auto offset = read<uint32_t>(debugCuIndex);
    if (i == infoSectionIndex) {
      cu.offset = offset;
    }
    if (i == abbrevSectionIndex) {
      cu.abbrevOffset = offset;
    }
    if (i == strOffsetsSectionIndex) {
      cu.strOffsetsBase = offset;
    }
    if (i == rnglistsSectionIndex) {
      cu.rnglistsBase = offset;
    }
  }
  return true;
}

bool parseCompilationUnitMetadata(CompilationUnit& cu, size_t offset) {
  auto debugInfo = asStringPiece(cu.debugSections.debugInfo);
  folly::StringPiece chunk(debugInfo);
  cu.offset = offset;
  chunk.advance(offset);
  // 1) unit_length
  auto initialLength = read<uint32_t>(chunk);
  cu.is64Bit = (initialLength == uint32_t(-1));
  cu.size = cu.is64Bit ? read<uint64_t>(chunk) : initialLength;
  if (cu.size > chunk.size()) {
    FOLLY_SAFE_DFATAL(
        "invalid size: ", cu.size, " chunk.size(): ", chunk.size());
    return false;
  }
  cu.size += cu.is64Bit ? 12 : 4;

  // 2) version
  cu.version = read<uint16_t>(chunk);
  if (cu.version < 2 || cu.version > 5) {
    FOLLY_SAFE_DFATAL("invalid info version: ", cu.version);
    return false;
  }

  if (cu.version == 5) {
    // DWARF5: 7.5.1.1 Full and Partial Compilation Unit Headers
    // 3) unit_type (new DWARF 5)
    cu.unitType = read<uint8_t>(chunk);
    if (cu.unitType != DW_UT_compile && cu.unitType != DW_UT_skeleton &&
        cu.unitType != DW_UT_split_compile) {
      return false;
    }
    // 4) address_size
    cu.addrSize = read<uint8_t>(chunk);
    if (cu.addrSize != sizeof(uintptr_t)) {
      FOLLY_SAFE_DFATAL("invalid address size: ", cu.addrSize);
      return false;
    }

    // 5) debug_abbrev_offset
    // This can be already set in from .debug_cu_index if dwp file is used.
    uint64_t abbrevOffset = readOffset(chunk, cu.is64Bit);
    if (!cu.abbrevOffset.hasValue()) {
      cu.abbrevOffset = abbrevOffset;
    }

    if (cu.unitType == DW_UT_skeleton || cu.unitType == DW_UT_split_compile) {
      // 6) dwo_id
      cu.dwoId = read<uint64_t>(chunk);
    }
  } else {
    // DWARF4 has a single type of unit in .debug_info
    cu.unitType = DW_UT_compile;
    // 3) debug_abbrev_offset
    // This can be already set in from .debug_cu_index if dwp file is used.
    uint64_t abbrevOffset = readOffset(chunk, cu.is64Bit);
    if (!cu.abbrevOffset.hasValue()) {
      cu.abbrevOffset = abbrevOffset;
    }
    // 4) address_size
    cu.addrSize = read<uint8_t>(chunk);
    if (cu.addrSize != sizeof(uintptr_t)) {
      FOLLY_SAFE_DFATAL("invalid address size: ", cu.addrSize);
      return false;
    }
  }
  cu.firstDie = chunk.data() - debugInfo.data();
  Die die = getDieAtOffset(cu, cu.firstDie);
  if (die.abbr.tag != DW_TAG_compile_unit &&
      die.abbr.tag != DW_TAG_skeleton_unit) {
    return false;
  }

  // Read the DW_AT_*_base attributes.
  // Attributes which use FORMs relative to these base attrs
  // will not have valid values during this first pass!
  forEachAttribute(cu, die, [&](const Attribute& attr) {
    switch (attr.spec.name) {
      case DW_AT_comp_dir:
        cu.compDir = std::get<folly::StringPiece>(attr.attrValue);
        break;
      case DW_AT_addr_base:
      case DW_AT_GNU_addr_base:
        cu.addrBase = std::get<uint64_t>(attr.attrValue);
        break;
      case DW_AT_GNU_ranges_base:
        cu.rangesBase = std::get<uint64_t>(attr.attrValue);
        break;
      case DW_AT_loclists_base:
        cu.loclistsBase = std::get<uint64_t>(attr.attrValue);
        break;
      case DW_AT_rnglists_base:
        cu.rnglistsBase = std::get<uint64_t>(attr.attrValue);
        break;
      case DW_AT_str_offsets_base:
        cu.strOffsetsBase = std::get<uint64_t>(attr.attrValue);
        break;
      case DW_AT_GNU_dwo_name:
      case DW_AT_dwo_name: // dwo id is set above in dwarf5.
        cu.dwoName = std::get<folly::StringPiece>(attr.attrValue);
        break;
      case DW_AT_GNU_dwo_id:
        cu.dwoId = std::get<uint64_t>(attr.attrValue);
        break;
    }
    return true; // continue forEachAttribute
  });

  return true;
}

} // namespace

CompilationUnits getCompilationUnits(
    ElfCacheBase* elfCache,
    const DebugSections& debugSections,
    uint64_t offset,
    bool requireSplitDwarf) {
  const folly::StringPiece debugInfo = asStringPiece(debugSections.debugInfo);
  FOLLY_SAFE_DCHECK(offset < debugInfo.size(), "unexpected offset");
  CompilationUnits cu;
  cu.mainCompilationUnit.debugSections = debugSections;

  if (!parseCompilationUnitMetadata(cu.mainCompilationUnit, offset)) {
    return cu;
  }

  if (!requireSplitDwarf || !cu.mainCompilationUnit.dwoId.hasValue() ||
      !cu.mainCompilationUnit.dwoName.hasValue()) {
    cu.mainCompilationUnit.rangesBase.reset();
    return cu;
  }

  CompilationUnit splitCU;
  // https://gcc.gnu.org/wiki/DebugFission
  // In order for the debugger to find the contribution to the .debug_addr
  // section corresponding to a particular compilation unit, the skeleton
  // DW_TAG_compile_unit entry in the .o file's .debug_info section will also
  // contain a DW_AT_addr_base attribute whose value points to the base of that
  // compilation unit's .debug_addr contribution.
  splitCU.addrBase = cu.mainCompilationUnit.addrBase;
  // The skeleton DW_TAG_compile_unit entry in the .o file’s .debug_info section
  // will contain a DW_AT_ranges_base attribute whose (relocated) value points
  // to the base of that compilation unit’s .debug_ranges contribution. All
  // values using DW_FORM_sec_offset for a DW_AT_ranges or DW_AT_start_scope
  // attribute (i.e., those attributes whose values are of class rangelistptr)
  // must be interpreted as offsets relative to the base address given by the
  // DW_AT_ranges_base attribute in the skeleton compilation unit DIE.
  splitCU.rangesBase = cu.mainCompilationUnit.rangesBase;

  // NOTE: For backwards compatibility reasons, DW_AT_GNU_ranges_base
  // applies only to DIE within dwo, but not within skeleton CU.
  if (cu.mainCompilationUnit.version <= 4) {
    cu.mainCompilationUnit.rangesBase.reset();
  }

  // Read from dwo file.
  static const size_t kPathLimit = 4 << 10; // 4KB
  ElfFile* elf = nullptr;
  {
    char path[kPathLimit];
    if (cu.mainCompilationUnit.compDir.size() + strlen("/") +
            cu.mainCompilationUnit.dwoName->size() + 1 >
        kPathLimit) {
      return cu;
    }
    path[0] = '\0';
    strncat(
        path,
        cu.mainCompilationUnit.compDir.data(),
        cu.mainCompilationUnit.compDir.size());
    strcat(path, "/");
    strncat(
        path,
        cu.mainCompilationUnit.dwoName->data(),
        cu.mainCompilationUnit.dwoName->size());
    elf = elfCache->getFile(path).get();
  }

  // Read from dwp file if dwo file doesn't exist.
  bool useDWP = false;
  if (elf == nullptr) {
    if (strlen(cu.mainCompilationUnit.debugSections.elf->filepath()) + 5 >
        kPathLimit) {
      return cu;
    }
    char dwpPath[kPathLimit];
    strcpy(dwpPath, cu.mainCompilationUnit.debugSections.elf->filepath());
    strcat(dwpPath, ".dwp");
    elf = elfCache->getFile(dwpPath).get();
    if (elf == nullptr) {
      return cu;
    }
    useDWP = true;
  }

  splitCU.debugSections = {
      .elf = elf,
      .debugCuIndex = getElfSection(elf, ".debug_cu_index"),
      .debugAbbrev = getElfSection(elf, ".debug_abbrev.dwo"),
      .debugAddr = cu.mainCompilationUnit.debugSections.debugAddr,
      .debugAranges = cu.mainCompilationUnit.debugSections.debugAranges,
      .debugInfo = getElfSection(elf, ".debug_info.dwo"),
      .debugLine = getElfSection(elf, ".debug_line.dwo"),
      .debugLineStr = cu.mainCompilationUnit.debugSections.debugLineStr,
      .debugLoclists = getElfSection(elf, ".debug_loclists.dwo"),
      .debugRanges = cu.mainCompilationUnit.debugSections.debugRanges,
      .debugRnglists = getElfSection(elf, ".debug_rnglists.dwo"),
      .debugStr = getElfSection(elf, ".debug_str.dwo"),
      .debugStrOffsets = getElfSection(elf, ".debug_str_offsets.dwo")};
  if (splitCU.debugSections.debugInfo.empty() ||
      splitCU.debugSections.debugAbbrev.empty() ||
      splitCU.debugSections.debugLine.empty() ||
      splitCU.debugSections.debugStr.empty()) {
    return cu;
  }

  if (useDWP) {
    if (!findCompiliationOffset(
            asStringPiece(splitCU.debugSections.debugCuIndex),
            *cu.mainCompilationUnit.dwoId,
            splitCU) ||
        !parseCompilationUnitMetadata(splitCU, splitCU.offset)) {
      return cu;
    }
    // 3.1.3 Split Full Compilation Unit Entries
    // The following attributes are not part of a split full compilation unit
    // entry but instead are inherited (if present) from the corresponding
    // skeleton compilation unit: DW_AT_low_pc, DW_AT_high_pc, DW_AT_ranges,
    // DW_AT_stmt_list, DW_AT_comp_dir, DW_AT_str_offsets_base, DW_AT_addr_base
    // and DW_AT_rnglists_base.
    if (cu.mainCompilationUnit.version == 5) {
      // 7.26 String Offsets Table
      // Each set of entries in the string offsets table contained in the
      // .debug_str_offsets
      //  or .debug_str_offsets.dwo section begins with a header containing:
      //  1. unit_length (initial length)
      //     A 4-byte or 12-byte length containing the length of the set of
      //     entries for this compilation unit, not including the length field
      //     itself.
      //  2. version (uhalf)
      //     A 2-byte version identifier containing the value 5.
      //  3. padding (uhalf)
      //     Reserved to DWARF (must be zero).
      splitCU.strOffsetsBase =
          splitCU.strOffsetsBase.value_or(0) + (splitCU.is64Bit ? 16 : 8);
      // 7.28 Range List Table
      // 1. unit_length (initial length)
      //    A 4-byte or 12-byte length containing the length of the set of
      //    entries for this compilation unit, not including the length field
      //    itself.
      // 2. version (uhalf)
      //    A 2-byte version identifier containing the value 5.
      // 3. address_size (ubyte)
      //    A 1-byte unsigned integer containing the size in bytes of an
      //    address.
      // 4. segment_selector_size (ubyte)
      //    A 1-byte unsigned integer containing the size in bytes of a segment
      //    selector on the target system.
      // 5. offset_entry_count (uword)
      //    A 4-byte count of the number of offsets that follow the header.
      splitCU.rnglistsBase =
          splitCU.rnglistsBase.value_or(0) + (splitCU.is64Bit ? 20 : 12);
    }
  } else {
    // Skip CUs like DW_TAG_type_unit
    for (size_t dwoOffset = 0;
         !parseCompilationUnitMetadata(splitCU, dwoOffset) &&
         dwoOffset < splitCU.debugSections.debugInfo.size();) {
      dwoOffset = dwoOffset + splitCU.size;
    }
    if (!splitCU.dwoId || *splitCU.dwoId != *cu.mainCompilationUnit.dwoId) {
      return cu;
    }
    if (cu.mainCompilationUnit.version == 5) {
      splitCU.strOffsetsBase = splitCU.is64Bit ? 16 : 8;
      splitCU.rnglistsBase = splitCU.is64Bit ? 20 : 12;
    }
  }

  cu.splitCU.emplace(std::move(splitCU));
  return cu;
}

Die getDieAtOffset(const CompilationUnit& cu, uint64_t offset) {
  const folly::StringPiece debugInfo =
      asStringPiece(cu.debugSections.debugInfo);
  FOLLY_SAFE_DCHECK(offset < debugInfo.size(), "unexpected offset");
  Die die;
  folly::StringPiece sp = folly::StringPiece{
      debugInfo.data() + offset, debugInfo.data() + cu.offset + cu.size};
  die.offset = offset;
  die.is64Bit = cu.is64Bit;
  auto code = readULEB(sp);
  die.code = code;
  if (code == 0) {
    return die;
  }
  die.attrOffset = sp.data() - debugInfo.data() - offset;
  die.abbr = !cu.abbrCache.empty() && die.code < kMaxAbbreviationEntries
      ? cu.abbrCache[die.code - 1]
      : getAbbreviation(
            asStringPiece(cu.debugSections.debugAbbrev),
            die.code,
            cu.abbrevOffset.value_or(0));

  return die;
}

Attribute readAttribute(
    const CompilationUnit& cu,
    const Die& die,
    AttributeSpec spec,
    folly::StringPiece& info) {
  // DWARF 5 introduces new FORMs whose values are relative to some base
  // attrs: DW_AT_str_offsets_base, DW_AT_rnglists_base, DW_AT_addr_base.
  // Debug Fission DWARF 4 uses GNU DW_AT_GNU_ranges_base &
  // DW_AT_GNU_addr_base.
  //
  // The order in which attributes appear in a CU is not defined.
  // The DW_AT_*_base attrs may appear after attributes that need them.
  // The DW_AT_*_base attrs are CU specific; so we read them just after
  // reading the CU header. During this first pass return empty values
  // when encountering a FORM that depends on DW_AT_*_base.
  auto getStringUsingOffsetTable = [&](uint64_t index) {
    if (!cu.strOffsetsBase.has_value() &&
        !(cu.version < 5 && cu.dwoId.has_value())) {
      return folly::StringPiece();
    }
    // DWARF 5: 7.26 String Offsets Table
    // The DW_AT_str_offsets_base attribute points to the first entry
    // following the header. The entries are indexed sequentially from
    // this base entry, starting from 0. For DWARF 4 version DWO files,
    // the value of DW_AT_str_offsets_base is implicitly zero.
    auto strOffsetsOffset = cu.strOffsetsBase.value_or(0) +
        index * (cu.is64Bit ? sizeof(uint64_t) : sizeof(uint32_t));
    auto sp = asStringPiece(cu.debugSections.debugStrOffsets)
                  .subpiece(strOffsetsOffset);
    uint64_t strOffset = readOffset(sp, cu.is64Bit);
    return getStringFromStringSection(
        asStringPiece(cu.debugSections.debugStr), strOffset);
  };

  auto readDebugAddr = [&](uint64_t index) {
    if (!cu.addrBase.has_value()) {
      return uint64_t(0);
    }
    // DWARF 5: 7.27 Address Table
    // The DW_AT_addr_base attribute points to the first entry following
    // the header. The entries are indexed sequentially from this base
    // entry, starting from 0.
    auto sp = asStringPiece(cu.debugSections.debugAddr)
                  .subpiece(*cu.addrBase + index * sizeof(uint64_t));
    return read<uint64_t>(sp);
  };

  switch (spec.form) {
    case DW_FORM_addr:
      return {spec, die, read<uintptr_t>(info)};
    case DW_FORM_block1:
      return {spec, die, readBytes(info, read<uint8_t>(info))};
    case DW_FORM_block2:
      return {spec, die, readBytes(info, read<uint16_t>(info))};
    case DW_FORM_block4:
      return {spec, die, readBytes(info, read<uint32_t>(info))};
    case DW_FORM_block:
      [[fallthrough]];
    case DW_FORM_exprloc:
      return {spec, die, readBytes(info, readULEB(info))};
    case DW_FORM_data1:
      [[fallthrough]];
    case DW_FORM_ref1:
      return {spec, die, read<uint8_t>(info)};
    case DW_FORM_data2:
      [[fallthrough]];
    case DW_FORM_ref2:
      return {spec, die, read<uint16_t>(info)};
    case DW_FORM_data4:
      [[fallthrough]];
    case DW_FORM_ref4:
      return {spec, die, read<uint32_t>(info)};
    case DW_FORM_data8:
      [[fallthrough]];
    case DW_FORM_ref8:
      [[fallthrough]];
    case DW_FORM_ref_sig8:
      return {spec, die, read<uint64_t>(info)};
    case DW_FORM_sdata:
      return {spec, die, to_unsigned(readSLEB(info))};
    case DW_FORM_udata:
      [[fallthrough]];
    case DW_FORM_ref_udata:
      return {spec, die, readULEB(info)};
    case DW_FORM_flag:
      return {spec, die, read<uint8_t>(info)};
    case DW_FORM_flag_present:
      return {spec, die, 1u};
    case DW_FORM_sec_offset:
      [[fallthrough]];
    case DW_FORM_ref_addr:
      return {spec, die, readOffset(info, die.is64Bit)};
    case DW_FORM_string:
      return {spec, die, readNullTerminated(info)};
    case DW_FORM_strp:
      return {
          spec,
          die,
          getStringFromStringSection(
              asStringPiece(cu.debugSections.debugStr),
              readOffset(info, die.is64Bit))};
    case DW_FORM_indirect: // form is explicitly specified
      // Update spec with the actual FORM.
      spec.form = readULEB(info);
      return readAttribute(cu, die, spec, info);

    // DWARF 5:
    case DW_FORM_implicit_const: // form is explicitly specified
      // For attributes with this form, the attribute specification
      // contains a third part, which is a signed LEB128 number. The value
      // of this number is used as the value of the attribute, and no
      // value is stored in the .debug_info section.
      return {spec, die, to_unsigned(spec.implicitConst)};

    case DW_FORM_addrx:
    case DW_FORM_GNU_addr_index:
      return {spec, die, readDebugAddr(readULEB(info))};
    case DW_FORM_addrx1:
      return {spec, die, readDebugAddr(readU64<1>(info))};
    case DW_FORM_addrx2:
      return {spec, die, readDebugAddr(readU64<2>(info))};
    case DW_FORM_addrx3:
      return {spec, die, readDebugAddr(readU64<3>(info))};
    case DW_FORM_addrx4:
      return {spec, die, readDebugAddr(readU64<4>(info))};

    case DW_FORM_line_strp:
      return {
          spec,
          die,
          getStringFromStringSection(
              asStringPiece(cu.debugSections.debugLineStr),
              readOffset(info, die.is64Bit))};

    case DW_FORM_strx:
      return {spec, die, getStringUsingOffsetTable(readULEB(info))};
    case DW_FORM_strx1:
      return {spec, die, getStringUsingOffsetTable(readU64<1>(info))};
    case DW_FORM_strx2:
      return {spec, die, getStringUsingOffsetTable(readU64<2>(info))};
    case DW_FORM_strx3:
      return {spec, die, getStringUsingOffsetTable(readU64<3>(info))};
    case DW_FORM_strx4:
      return {spec, die, getStringUsingOffsetTable(readU64<4>(info))};

    case DW_FORM_GNU_str_index:
      return {spec, die, getStringUsingOffsetTable(readULEB(info))};

    case DW_FORM_rnglistx: {
      auto index = readULEB(info);
      if (!cu.rnglistsBase.has_value()) {
        return {spec, die, 0u};
      }
      const uint64_t offsetSize =
          cu.is64Bit ? sizeof(uint64_t) : sizeof(uint32_t);
      auto sp = asStringPiece(cu.debugSections.debugRnglists)
                    .subpiece(*cu.rnglistsBase + index * offsetSize);
      auto offset = readOffset(sp, cu.is64Bit);
      return {spec, die, *cu.rnglistsBase + offset};
    }

    case DW_FORM_loclistx: {
      auto index = readULEB(info);
      if (!cu.loclistsBase.has_value()) {
        return {spec, die, 0u};
      }
      const uint64_t offsetSize =
          cu.is64Bit ? sizeof(uint64_t) : sizeof(uint32_t);
      auto sp = asStringPiece(cu.debugSections.debugLoclists)
                    .subpiece(*cu.loclistsBase + index * offsetSize);
      auto offset = readOffset(sp, cu.is64Bit);
      return {spec, die, *cu.loclistsBase + offset};
    }

    case DW_FORM_data16:
      return {spec, die, readBytes(info, 16)};

    case DW_FORM_ref_sup4:
    case DW_FORM_ref_sup8:
    case DW_FORM_strp_sup:
      FOLLY_SAFE_DFATAL(
          "Unexpected DWARF5 supplimentary object files: ", spec.form);
      return {spec, die, 0u};

    default:
      FOLLY_SAFE_DFATAL("invalid attribute form: ", spec.form);
      return {spec, die, 0u};
  }
}

/*
 * Iterate over all attributes of the given DIE, calling the given
 * callable for each. Iteration is stopped early if any of the calls
 * return false.
 */
size_t forEachAttribute(
    const CompilationUnit& cu,
    const Die& die,
    folly::FunctionRef<bool(const Attribute&)> f) {
  auto attrs = die.abbr.attributes;
  const auto debugInfo = asStringPiece(cu.debugSections.debugInfo);
  auto values = folly::StringPiece{
      debugInfo.data() + die.offset + die.attrOffset,
      debugInfo.data() + cu.offset + cu.size};
  while (auto spec = readAttributeSpec(attrs)) {
    auto attr = readAttribute(cu, die, spec, values);
    if (!f(attr)) {
      return static_cast<size_t>(-1);
    }
  }
  return values.data() - debugInfo.data();
}

folly::StringPiece getFunctionNameFromDie(
    const CompilationUnit& srcu, const Die& die) {
  folly::StringPiece name;
  forEachAttribute(srcu, die, [&](const Attribute& attr) {
    switch (attr.spec.name) {
      case DW_AT_linkage_name:
        name = std::get<folly::StringPiece>(attr.attrValue);
        break;
      case DW_AT_name:
        // NOTE: when DW_AT_linkage_name and DW_AT_name match, dwarf
        // emitters omit DW_AT_linkage_name (to save space). If present
        // DW_AT_linkage_name should always be preferred (mangled C++ name
        // vs just the function name).
        if (name.empty()) {
          name = std::get<folly::StringPiece>(attr.attrValue);
        }
        break;
    }
    return true; // continue forEachAttribute
  });
  return name;
}

folly::StringPiece getFunctionName(
    const CompilationUnit& srcu, uint64_t dieOffset) {
  auto declDie = getDieAtOffset(srcu, dieOffset);
  auto name = getFunctionNameFromDie(srcu, declDie);
  return name.empty()
      ? getFunctionNameFromDie(srcu, findDefinitionDie(srcu, declDie))
      : name;
}

Die findDefinitionDie(const CompilationUnit& cu, const Die& die) {
  // Find the real definition instead of declaration.
  // DW_AT_specification: Incomplete, non-defining, or separate declaration
  // corresponding to a declaration
  auto offset = getAttribute<uint64_t>(cu, die, DW_AT_specification);
  if (!offset) {
    return die;
  }
  return getDieAtOffset(cu, cu.offset + offset.value());
}

} // namespace symbolizer
} // namespace folly

#endif // FOLLY_HAVE_DWARF
