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
#include <limits>
#include <type_traits>

#include <folly/Optional.h>
#include <folly/Range.h>
#include <folly/debugging/symbolizer/Elf.h>
#include <folly/debugging/symbolizer/detail/DwarfConstants.h>
#include <folly/lang/CheckedMath.h>
#include <folly/portability/Config.h>
#include <folly/portability/Unistd.h>

#if FOLLY_HAVE_DWARF && FOLLY_HAVE_ELF

namespace folly {
namespace symbolizer {

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

// Read ULEB (unsigned) varint value; algorithm from the DWARF spec
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

// Read SLEB (signed) varint value; algorithm from the DWARF spec
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
uint64_t readOffset(std::span<const uint8_t>& bytes, bool is64Bit) {
  return is64Bit ? read<uint64_t>(bytes) : read<uint32_t>(bytes);
}

// Read "len" bytes
std::span<const uint8_t> readBytes(
    std::span<const uint8_t>& bytes, uint64_t len) {
  FOLLY_SAFE_CHECK(len <= bytes.size(), "invalid byte length");
  auto ret = bytes.first(len);
  bytes = bytes.subspan(len);
  return ret;
}

// Read a null-terminated string
folly::StringPiece readNullTerminated(std::span<const uint8_t>& bytes) {
  const auto* p =
      static_cast<const uint8_t*>(memchr(bytes.data(), 0, bytes.size()));
  FOLLY_SAFE_CHECK(p, "invalid null-terminated string");
  folly::StringPiece ret(
      reinterpret_cast<const char*>(bytes.data()), p - bytes.data());
  bytes = bytes.subspan(p - bytes.data() + 1);
  return ret;
}

folly::StringPiece getStringFromStringSection(
    std::span<const uint8_t> bytes, uint64_t offset) {
  FOLLY_SAFE_CHECK(offset < bytes.size(), "invalid string offset");
  bytes = bytes.subspan(offset);
  return readNullTerminated(bytes);
}

AttributeSpec readAttributeSpec(std::span<const uint8_t>& bytes) {
  AttributeSpec spec;
  spec.name = readULEB(bytes);
  spec.form = readULEB(bytes);
  if (spec.form == DW_FORM_implicit_const) {
    spec.implicitConst = readSLEB(bytes);
  }
  return spec;
}

DwarfParseResult<folly::Optional<AttributeSpec>> readAttributeSpec(
    DwarfCursor& cursor) {
  const auto specOffset = cursor.offset();
  auto name = cursor.readULEB();
  if (name.hasError()) {
    return makeUnexpected(name.error());
  }
  auto form = cursor.readULEB();
  if (form.hasError()) {
    return makeUnexpected(form.error());
  }
  if (*name == 0 && *form == 0) {
    return folly::Optional<AttributeSpec>{};
  }
  if (*name == 0 || *form == 0) {
    return makeUnexpected(
        DwarfParseError{DwarfParseErrorCode::invalid_value, specOffset});
  }

  AttributeSpec spec{.name = *name, .form = *form};
  if (spec.form == DW_FORM_implicit_const) {
    auto implicitConst = cursor.readSLEB();
    if (implicitConst.hasError()) {
      return makeUnexpected(implicitConst.error());
    }
    spec.implicitConst = *implicitConst;
  }
  return folly::Optional<AttributeSpec>{spec};
}

DwarfParseResult<folly::Optional<DIEAbbreviation>> readAbbreviation(
    DwarfCursor& cursor) {
  auto code = cursor.readULEB();
  if (code.hasError()) {
    return makeUnexpected(code.error());
  }
  if (*code == 0) {
    return folly::Optional<DIEAbbreviation>{};
  }

  const auto tagOffset = cursor.offset();
  auto tag = cursor.readULEB();
  if (tag.hasError()) {
    return makeUnexpected(tag.error());
  }
  if (*tag == 0) {
    return makeUnexpected(
        DwarfParseError{DwarfParseErrorCode::invalid_value, tagOffset});
  }
  const auto childrenOffset = cursor.offset();
  auto children = cursor.read<uint8_t>();
  if (children.hasError()) {
    return makeUnexpected(children.error());
  }
  if (*children != DW_CHILDREN_no && *children != DW_CHILDREN_yes) {
    return makeUnexpected(
        DwarfParseError{DwarfParseErrorCode::invalid_value, childrenOffset});
  }

  DIEAbbreviation abbr{
      .code = *code,
      .tag = *tag,
      .hasChildren = *children == DW_CHILDREN_yes,
      .attributes = {},
  };
  const auto* attributeBegin = cursor.remaining().data();
  for (;;) {
    auto spec = readAttributeSpec(cursor);
    if (spec.hasError()) {
      return makeUnexpected(spec.error());
    }
    if (!spec->hasValue()) {
      break;
    }
  }
  abbr.attributes = {
      attributeBegin,
      static_cast<size_t>(cursor.remaining().data() - attributeBegin)};
  return folly::Optional<DIEAbbreviation>{abbr};
}

namespace {

template <typename Size = size_t>
constexpr Size toErrorOffset(uint64_t offset) noexcept {
  if constexpr (sizeof(Size) < sizeof(uint64_t)) {
    if (offset > std::numeric_limits<Size>::max()) {
      return std::numeric_limits<Size>::max();
    }
  }
  return static_cast<Size>(offset);
}

DwarfParseResult<DIEAbbreviation> getAbbreviation(
    std::span<const uint8_t> debugAbbrev, uint64_t code, uint64_t offset) {
  if (offset > debugAbbrev.size()) {
    return makeUnexpected(
        DwarfParseError{
            DwarfParseErrorCode::invalid_offset, toErrorOffset(offset)});
  }

  DwarfCursor cursor(
      debugAbbrev.subspan(static_cast<size_t>(offset)),
      static_cast<size_t>(offset));
  for (;;) {
    auto abbr = readAbbreviation(cursor);
    if (abbr.hasError()) {
      return makeUnexpected(abbr.error());
    }
    if (!abbr->hasValue()) {
      break;
    }
    if ((**abbr).code == code) {
      return **abbr;
    }
  }

  return makeUnexpected(
      DwarfParseError{
          DwarfParseErrorCode::invalid_value, toErrorOffset(offset)});
}

template <typename T>
DwarfParseResult<T> parseError(DwarfParseErrorCode code, size_t offset) {
  return makeUnexpected(DwarfParseError{code, offset});
}

DwarfParseResult<size_t> checkedMultiply(
    size_t lhs, size_t rhs, size_t offset) {
  size_t result;
  if (!folly::checked_mul(&result, lhs, rhs)) {
    return parseError<size_t>(DwarfParseErrorCode::integer_overflow, offset);
  }
  return result;
}

bool isSupportedCompilationUnitType(uint8_t unitType) {
  return unitType == DW_UT_compile || unitType == DW_UT_skeleton ||
      unitType == DW_UT_split_compile;
}

} // namespace

DwarfParseResult<CompilationUnitHeader> parseCompilationUnitHeader(
    std::span<const uint8_t> debugInfo, size_t offset) {
  if (offset > debugInfo.size()) {
    return parseError<CompilationUnitHeader>(
        DwarfParseErrorCode::invalid_offset, offset);
  }
  if (offset > std::numeric_limits<uint32_t>::max()) {
    return parseError<CompilationUnitHeader>(
        DwarfParseErrorCode::integer_overflow, offset);
  }

  DwarfCursor lengthCursor(debugInfo.subspan(offset), offset);
  auto initialLength = lengthCursor.read<uint32_t>();
  if (initialLength.hasError()) {
    return makeUnexpected(initialLength.error());
  }

  CompilationUnitHeader header;
  uint64_t unitLength = *initialLength;
  if (*initialLength == std::numeric_limits<uint32_t>::max()) {
    header.is64Bit = true;
    auto extendedLength = lengthCursor.read<uint64_t>();
    if (extendedLength.hasError()) {
      return makeUnexpected(extendedLength.error());
    }
    unitLength = *extendedLength;
  } else if (*initialLength >= kDwarfReservedInitialLengthStart) {
    return parseError<CompilationUnitHeader>(
        DwarfParseErrorCode::invalid_value, offset);
  }

  const auto lengthFieldSize = lengthCursor.offset() - offset;
  if (unitLength > lengthCursor.remaining().size()) {
    return parseError<CompilationUnitHeader>(
        DwarfParseErrorCode::truncated, lengthCursor.offset());
  }
  const auto unitSize = lengthFieldSize + static_cast<size_t>(unitLength);
  if (unitSize > std::numeric_limits<uint32_t>::max() ||
      offset > std::numeric_limits<uint32_t>::max() - unitSize) {
    return parseError<CompilationUnitHeader>(
        DwarfParseErrorCode::integer_overflow, offset);
  }
  header.size = static_cast<uint32_t>(unitSize);

  DwarfCursor cursor(debugInfo.subspan(offset, unitSize), offset);
  auto lengthField = cursor.readBytes(lengthFieldSize);
  if (lengthField.hasError()) {
    return makeUnexpected(lengthField.error());
  }

  auto version = cursor.read<uint16_t>();
  if (version.hasError()) {
    return makeUnexpected(version.error());
  }
  if (*version < 2 || *version > 5) {
    return parseError<CompilationUnitHeader>(
        DwarfParseErrorCode::invalid_value, cursor.offset() - sizeof(*version));
  }
  header.version = *version;

  if (header.version == 5) {
    auto unitType = cursor.read<uint8_t>();
    if (unitType.hasError()) {
      return makeUnexpected(unitType.error());
    }
    if (*unitType < 1 || *unitType > 6) {
      return parseError<CompilationUnitHeader>(
          DwarfParseErrorCode::invalid_value,
          cursor.offset() - sizeof(*unitType));
    }
    header.unitType = *unitType;
    if (!isSupportedCompilationUnitType(header.unitType)) {
      header.firstDie = static_cast<uint32_t>(cursor.offset());
      return header;
    }

    auto addrSize = cursor.read<uint8_t>();
    if (addrSize.hasError()) {
      return makeUnexpected(addrSize.error());
    }
    if (*addrSize != sizeof(uintptr_t)) {
      return parseError<CompilationUnitHeader>(
          DwarfParseErrorCode::invalid_value,
          cursor.offset() - sizeof(*addrSize));
    }
    header.addrSize = *addrSize;

    auto abbrevOffset = cursor.readOffset(header.is64Bit);
    if (abbrevOffset.hasError()) {
      return makeUnexpected(abbrevOffset.error());
    }
    header.abbrevOffset = *abbrevOffset;

    if (header.unitType == DW_UT_skeleton ||
        header.unitType == DW_UT_split_compile) {
      auto dwoId = cursor.read<uint64_t>();
      if (dwoId.hasError()) {
        return makeUnexpected(dwoId.error());
      }
      header.dwoId = *dwoId;
    }
  } else {
    auto abbrevOffset = cursor.readOffset(header.is64Bit);
    if (abbrevOffset.hasError()) {
      return makeUnexpected(abbrevOffset.error());
    }
    header.abbrevOffset = *abbrevOffset;

    auto addrSize = cursor.read<uint8_t>();
    if (addrSize.hasError()) {
      return makeUnexpected(addrSize.error());
    }
    if (*addrSize != sizeof(uintptr_t)) {
      return parseError<CompilationUnitHeader>(
          DwarfParseErrorCode::invalid_value,
          cursor.offset() - sizeof(*addrSize));
    }
    header.addrSize = *addrSize;
  }

  header.firstDie = static_cast<uint32_t>(cursor.offset());
  return header;
}

DwarfParseResult<bool> findDwpCompilationUnit(
    std::span<const uint8_t> debugCuIndex,
    uint64_t dwoId,
    CompilationUnit& cu) {
  if (debugCuIndex.empty()) {
    return false;
  }

  DwarfCursor cursor(debugCuIndex);
  auto version = cursor.read<uint32_t>();
  auto numColumns = cursor.read<uint32_t>();
  auto numUnits = cursor.read<uint32_t>();
  auto numBuckets = cursor.read<uint32_t>();
  if (version.hasError()) {
    return makeUnexpected(version.error());
  }
  if (numColumns.hasError()) {
    return makeUnexpected(numColumns.error());
  }
  if (numUnits.hasError()) {
    return makeUnexpected(numUnits.error());
  }
  if (numBuckets.hasError()) {
    return makeUnexpected(numBuckets.error());
  }
  if (*version != 2 && *version != (5 << (kIsLittleEndian ? 0 : 16))) {
    return false;
  }

  const auto tableEntries =
      checkedMultiply(*numUnits, *numColumns, cursor.offset());
  if (tableEntries.hasError()) {
    return makeUnexpected(tableEntries.error());
  }
  const auto offsetsSize =
      checkedMultiply(*tableEntries, sizeof(uint32_t), cursor.offset());
  if (offsetsSize.hasError()) {
    return makeUnexpected(offsetsSize.error());
  }
  const auto signaturesSize =
      checkedMultiply(*numBuckets, sizeof(uint64_t), cursor.offset());
  if (signaturesSize.hasError()) {
    return makeUnexpected(signaturesSize.error());
  }
  const auto indexesSize =
      checkedMultiply(*numBuckets, sizeof(uint32_t), cursor.offset());
  if (indexesSize.hasError()) {
    return makeUnexpected(indexesSize.error());
  }
  const auto columnsSize =
      checkedMultiply(*numColumns, sizeof(uint32_t), cursor.offset());
  if (columnsSize.hasError()) {
    return makeUnexpected(columnsSize.error());
  }

  const auto signaturesOffset = cursor.offset();
  auto signatures = cursor.readBytes(*signaturesSize);
  if (signatures.hasError()) {
    return makeUnexpected(signatures.error());
  }
  const auto indexesOffset = cursor.offset();
  auto indexes = cursor.readBytes(*indexesSize);
  if (indexes.hasError()) {
    return makeUnexpected(indexes.error());
  }
  const auto columnsOffset = cursor.offset();
  auto columns = cursor.readBytes(*columnsSize);
  if (columns.hasError()) {
    return makeUnexpected(columns.error());
  }
  const auto offsetsOffset = cursor.offset();
  auto offsets = cursor.readBytes(*offsetsSize);
  if (offsets.hasError()) {
    return makeUnexpected(offsets.error());
  }

  DwarfCursor signaturesCursor(*signatures, signaturesOffset);
  DwarfCursor indexesCursor(*indexes, indexesOffset);
  uint32_t unitIndex = 0;
  for (uint32_t i = 0; i < *numBuckets; ++i) {
    auto signature = signaturesCursor.read<uint64_t>();
    auto index = indexesCursor.read<uint32_t>();
    if (signature.hasError()) {
      return makeUnexpected(signature.error());
    }
    if (index.hasError()) {
      return makeUnexpected(index.error());
    }
    if (*signature == dwoId) {
      unitIndex = *index;
      break;
    }
  }
  if (unitIndex == 0) {
    return false;
  }
  if (unitIndex > *numUnits) {
    return parseError<bool>(DwarfParseErrorCode::invalid_value, indexesOffset);
  }

  folly::Optional<size_t> infoSectionIndex;
  folly::Optional<size_t> abbrevSectionIndex;
  folly::Optional<size_t> strOffsetsSectionIndex;
  folly::Optional<size_t> rnglistsSectionIndex;
  DwarfCursor columnsCursor(*columns, columnsOffset);
  for (uint32_t i = 0; i < *numColumns; ++i) {
    auto section = columnsCursor.read<uint32_t>();
    if (section.hasError()) {
      return makeUnexpected(section.error());
    }
    switch (*section) {
      case DW_SECT_INFO:
        infoSectionIndex = i;
        break;
      case DW_SECT_ABBREV:
        abbrevSectionIndex = i;
        break;
      case DW_SECT_STR_OFFSETS:
        strOffsetsSectionIndex = i;
        break;
      case DW_SECT_RNGLISTS:
        rnglistsSectionIndex = i;
        break;
    }
  }
  if (!infoSectionIndex || !abbrevSectionIndex || !strOffsetsSectionIndex) {
    return false;
  }

  const auto rowSize = *columnsSize;
  const auto rowOffset = static_cast<size_t>(unitIndex - 1) * rowSize;
  DwarfCursor rowCursor(
      offsets->subspan(rowOffset, rowSize), offsetsOffset + rowOffset);
  uint32_t infoOffset = 0;
  uint32_t abbrevOffset = 0;
  uint32_t strOffsetsBase = 0;
  folly::Optional<uint32_t> rnglistsBase;
  for (uint32_t i = 0; i < *numColumns; ++i) {
    auto value = rowCursor.read<uint32_t>();
    if (value.hasError()) {
      return makeUnexpected(value.error());
    }
    if (i == *infoSectionIndex) {
      infoOffset = *value;
    } else if (i == *abbrevSectionIndex) {
      abbrevOffset = *value;
    } else if (i == *strOffsetsSectionIndex) {
      strOffsetsBase = *value;
    } else if (rnglistsSectionIndex && i == *rnglistsSectionIndex) {
      rnglistsBase = *value;
    }
  }

  cu.offset = infoOffset;
  cu.abbrevOffset = abbrevOffset;
  cu.strOffsetsBase = strOffsetsBase;
  if (rnglistsBase) {
    cu.rnglistsBase = *rnglistsBase;
  }
  return true;
}

namespace {

DwarfParseResult<bool> parseCompilationUnitMetadata(
    CompilationUnit& cu, size_t offset, bool preserveIndexedOffsets = false) {
  auto header = parseCompilationUnitHeader(cu.debugSections.debugInfo, offset);
  if (header.hasError()) {
    return makeUnexpected(header.error());
  }

  cu.offset = static_cast<uint32_t>(offset);
  cu.is64Bit = header->is64Bit;
  cu.size = header->size;
  cu.version = header->version;
  cu.unitType = header->unitType;
  cu.addrSize = header->addrSize;
  cu.firstDie = header->firstDie;
  cu.dwoId = header->dwoId;
  if (!preserveIndexedOffsets) {
    cu.abbrevOffset = header->abbrevOffset;
  }
  if (!isSupportedCompilationUnitType(cu.unitType)) {
    return false;
  }

  auto dieResult = getDieAtOffset(cu, cu.firstDie);
  if (dieResult.hasError()) {
    return makeUnexpected(dieResult.error());
  }
  const auto& die = *dieResult;
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
  CompilationUnits cu;
  cu.mainCompilationUnit.debugSections = debugSections;

  if (offset > debugSections.debugInfo.size()) {
    return cu;
  }
  auto mainResult = parseCompilationUnitMetadata(
      cu.mainCompilationUnit, static_cast<size_t>(offset));
  if (mainResult.hasError() || !*mainResult) {
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
    auto indexResult = findDwpCompilationUnit(
        splitCU.debugSections.debugCuIndex,
        *cu.mainCompilationUnit.dwoId,
        splitCU);
    if (indexResult.hasError() || !*indexResult) {
      return cu;
    }
    auto splitResult =
        parseCompilationUnitMetadata(splitCU, splitCU.offset, true);
    if (splitResult.hasError() || !*splitResult) {
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
    bool found = false;
    for (size_t dwoOffset = 0;
         dwoOffset < splitCU.debugSections.debugInfo.size();) {
      auto splitResult = parseCompilationUnitMetadata(splitCU, dwoOffset);
      if (splitResult.hasError()) {
        return cu;
      }
      if (*splitResult) {
        found = true;
        break;
      }
      if (splitCU.size == 0 ||
          splitCU.size > splitCU.debugSections.debugInfo.size() - dwoOffset) {
        return cu;
      }
      dwoOffset += splitCU.size;
    }
    if (!found) {
      return cu;
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

namespace {

std::span<const uint8_t> getCompilationUnitBytes(
    const CompilationUnit& cu, uint64_t offset) {
  const auto debugInfo = cu.debugSections.debugInfo;
  FOLLY_SAFE_CHECK(cu.offset <= debugInfo.size(), "invalid CU offset");
  FOLLY_SAFE_CHECK(cu.size <= debugInfo.size() - cu.offset, "invalid CU size");
  const auto end = cu.offset + cu.size;
  FOLLY_SAFE_CHECK(offset <= end, "offset outside CU");
  return debugInfo.subspan(offset, end - offset);
}

std::span<const uint8_t> getIndexedSectionBytes(
    std::span<const uint8_t> section,
    uint64_t base,
    uint64_t index,
    uint64_t entrySize) {
  // Entry sizes come from address and offset widths, which are nonzero, but
  // check anyway: the index bound below divides by entrySize.
  FOLLY_SAFE_CHECK(entrySize != 0, "zero section entry size");
  FOLLY_SAFE_CHECK(base <= section.size(), "invalid section base");
  const auto remaining = section.size() - base;
  FOLLY_SAFE_CHECK(entrySize <= remaining, "section entry out of bounds");
  FOLLY_SAFE_CHECK(
      index <= (remaining - entrySize) / entrySize,
      "section index out of bounds");
  return section.subspan(base + index * entrySize);
}

} // namespace

DwarfParseResult<Die> getDieAtOffset(
    const CompilationUnit& cu, uint64_t offset) {
  const auto debugInfo = cu.debugSections.debugInfo;
  if (cu.offset > debugInfo.size() || cu.size > debugInfo.size() - cu.offset) {
    return makeUnexpected(
        DwarfParseError{DwarfParseErrorCode::invalid_offset, cu.offset});
  }
  const auto unitEnd = static_cast<uint64_t>(cu.offset) + cu.size;
  if (offset < cu.offset || offset > unitEnd || offset > debugInfo.size()) {
    return makeUnexpected(
        DwarfParseError{
            DwarfParseErrorCode::invalid_offset, toErrorOffset(offset)});
  }
  if (offset > std::numeric_limits<uint32_t>::max()) {
    return makeUnexpected(
        DwarfParseError{
            DwarfParseErrorCode::integer_overflow,
            static_cast<size_t>(offset)});
  }

  Die die;
  DwarfCursor cursor(
      debugInfo.subspan(
          static_cast<size_t>(offset), static_cast<size_t>(unitEnd - offset)),
      static_cast<size_t>(offset));
  die.offset = static_cast<uint32_t>(offset);
  die.is64Bit = cu.is64Bit;
  auto code = cursor.readULEB();
  if (code.hasError()) {
    return makeUnexpected(code.error());
  }
  die.code = *code;
  if (die.code == 0) {
    return die;
  }
  const size_t attrOffset = cursor.offset() - static_cast<size_t>(offset);
  if (attrOffset > std::numeric_limits<uint8_t>::max()) {
    return makeUnexpected(
        DwarfParseError{
            DwarfParseErrorCode::integer_overflow, cursor.offset()});
  }
  die.attrOffset = static_cast<uint8_t>(attrOffset);

  if (!cu.abbrCache.empty() && die.code <= cu.abbrCache.size() &&
      cu.abbrCache[die.code - 1].code == die.code) {
    die.abbr = cu.abbrCache[die.code - 1];
    return die;
  }
  auto abbr = getAbbreviation(
      cu.debugSections.debugAbbrev, die.code, cu.abbrevOffset.value_or(0));
  if (abbr.hasError()) {
    return makeUnexpected(abbr.error());
  }
  die.abbr = *abbr;

  return die;
}

Attribute readAttribute(
    const CompilationUnit& cu,
    const Die& die,
    AttributeSpec spec,
    std::span<const uint8_t>& info) {
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
    const auto entrySize = cu.is64Bit ? sizeof(uint64_t) : sizeof(uint32_t);
    auto sp = getIndexedSectionBytes(
        cu.debugSections.debugStrOffsets,
        cu.strOffsetsBase.value_or(0),
        index,
        entrySize);
    uint64_t strOffset = readOffset(sp, cu.is64Bit);
    return getStringFromStringSection(cu.debugSections.debugStr, strOffset);
  };

  auto readDebugAddr = [&](uint64_t index) {
    if (!cu.addrBase.has_value()) {
      return uint64_t(0);
    }
    // DWARF 5: 7.27 Address Table
    // The DW_AT_addr_base attribute points to the first entry following
    // the header. The entries are indexed sequentially from this base
    // entry, starting from 0.
    auto sp = getIndexedSectionBytes(
        cu.debugSections.debugAddr, *cu.addrBase, index, sizeof(uint64_t));
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
              cu.debugSections.debugStr, readOffset(info, die.is64Bit))};
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
              cu.debugSections.debugLineStr, readOffset(info, die.is64Bit))};

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
      auto sp = getIndexedSectionBytes(
          cu.debugSections.debugRnglists, *cu.rnglistsBase, index, offsetSize);
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
      auto sp = getIndexedSectionBytes(
          cu.debugSections.debugLoclists, *cu.loclistsBase, index, offsetSize);
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
  const auto debugInfo = cu.debugSections.debugInfo;
  auto values = getCompilationUnitBytes(cu, die.offset);
  FOLLY_SAFE_CHECK(die.attrOffset <= values.size(), "attributes outside CU");
  values = values.subspan(die.attrOffset);
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
  if (declDie.hasError()) {
    return {};
  }
  auto name = getFunctionNameFromDie(srcu, *declDie);
  if (!name.empty()) {
    return name;
  }
  auto definition = findDefinitionDie(srcu, *declDie);
  return definition.hasValue()
      ? getFunctionNameFromDie(srcu, *definition)
      : folly::StringPiece{};
}

DwarfParseResult<Die> findDefinitionDie(
    const CompilationUnit& cu, const Die& die) {
  // Find the real definition instead of declaration.
  // DW_AT_specification: Incomplete, non-defining, or separate declaration
  // corresponding to a declaration
  auto offset = getAttribute<uint64_t>(cu, die, DW_AT_specification);
  if (!offset) {
    return die;
  }
  if (*offset > cu.size) {
    return makeUnexpected(
        DwarfParseError{DwarfParseErrorCode::invalid_offset, die.offset});
  }
  return getDieAtOffset(cu, cu.offset + offset.value());
}

} // namespace symbolizer
} // namespace folly

#endif // FOLLY_HAVE_DWARF
