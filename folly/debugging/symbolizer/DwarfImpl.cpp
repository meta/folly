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

#include <folly/Range.h>
#include <folly/debugging/symbolizer/DwarfImpl.h>

#include <array>
#include <type_traits>

#include <folly/Optional.h>
#include <folly/debugging/symbolizer/DwarfUtil.h>
#include <folly/debugging/symbolizer/detail/DwarfConstants.h>
#include <folly/lang/SafeAssert.h>
#include <folly/portability/Config.h>

#if FOLLY_HAVE_DWARF && FOLLY_HAVE_ELF

namespace folly {
namespace symbolizer {

// Indicates inline function `name` is called  at `line@file`.
struct CallLocation {
  Path file = {};
  uint64_t line = 0;
  folly::StringPiece name;
};

DwarfImpl::DwarfImpl(
    ElfCacheBase* elfCache, CompilationUnits& cu, LocationInfoMode mode)
    : elfCache_(elfCache), cu_(cu), mode_(mode) {}

/**
 * Find the @locationInfo for @address in the compilation unit @cu_.
 *
 * Best effort:
 * - fills @inlineFrames if mode_ == FULL_WITH_INLINE,
 * - calls @eachParameterName on the function parameters.
 *
 * if @checkAddress is true, we verify that the address is mapped to
 * a range in this CU before running the line number VM
 */
bool DwarfImpl::findLocation(
    uintptr_t address,
    SymbolizedFrame& frame,
    folly::Range<SymbolizedFrame*> inlineFrames,
    folly::FunctionRef<void(folly::StringPiece)> eachParameterName,
    bool checkAddress) const {
  auto mainCu = cu_.mainCompilationUnit;
  Die die = getDieAtOffset(mainCu, mainCu.firstDie);
  // Partial compilation unit (DW_TAG_partial_unit) is not supported.
  if (die.abbr.tag != DW_TAG_compile_unit &&
      die.abbr.tag != DW_TAG_skeleton_unit) {
    FOLLY_SAFE_DFATAL("Unsupported die.abbr.tag: ", die.abbr.tag);
    return false;
  }

  // Offset in .debug_line for the line number VM program for this CU
  folly::Optional<uint64_t> lineOffset;
  folly::StringPiece compilationDirectory;
  folly::Optional<folly::StringPiece> mainFileName;
  folly::Optional<uint64_t> baseAddrCU;
  folly::Optional<uint64_t> rangesOffset;
  bool seenLowPC = false;
  bool seenHighPC = false;
  enum : unsigned {
    kStmtList = 1U << 0,
    kCompDir = 1U << 1,
    kName = 1U << 2,
    kLowPC = 1U << 3,
    kHighPCOrRanges = 1U << 4,
  };
  unsigned expectedAttributes = kStmtList | kCompDir | kName | kLowPC;
  bool foundAddress = !checkAddress;
  if (!foundAddress) {
    expectedAttributes |= kHighPCOrRanges;
  }

  forEachAttribute(mainCu, die, [&](const Attribute& attr) {
    switch (attr.spec.name) {
      case DW_AT_stmt_list:
        expectedAttributes &= ~kStmtList;
        // Offset in .debug_line for the line number VM program for this
        // compilation unit
        lineOffset = std::get<uint64_t>(attr.attrValue);
        break;
      case DW_AT_comp_dir:
        expectedAttributes &= ~kCompDir;
        // Compilation directory
        compilationDirectory = std::get<folly::StringPiece>(attr.attrValue);
        break;
      case DW_AT_name:
        expectedAttributes &= ~kName;
        // File name of main file being compiled
        mainFileName = std::get<folly::StringPiece>(attr.attrValue);
        break;
      case DW_AT_low_pc:
        expectedAttributes &= ~kLowPC;
        baseAddrCU = std::get<uint64_t>(attr.attrValue);
        if (!foundAddress) {
          if (address < *baseAddrCU) {
            return false;
          }
          seenLowPC = true;
          if (seenHighPC) {
            foundAddress = true;
          } else if (rangesOffset) {
            if (!isAddrInRangeList(
                    mainCu,
                    address,
                    baseAddrCU,
                    *rangesOffset,
                    mainCu.addrSize)) {
              return false;
            }
            foundAddress = true;
          }
        }
        break;
      case DW_AT_high_pc:
        expectedAttributes &= ~kHighPCOrRanges;
        if (!foundAddress) {
          if (address >= std::get<uint64_t>(attr.attrValue)) {
            return false;
          }
          seenHighPC = true;
          foundAddress = seenLowPC;
        }
        break;
      case DW_AT_ranges:
        // 3.1.1: CU entries have:
        // - either DW_AT_low_pc and DW_AT_high_pc
        // OR
        // - DW_AT_ranges and optional DW_AT_low_pc
        expectedAttributes &= ~kHighPCOrRanges;
        if (!foundAddress) {
          rangesOffset = std::get<uint64_t>(attr.attrValue);
          if (seenLowPC) {
            if (!isAddrInRangeList(
                    mainCu,
                    address,
                    baseAddrCU,
                    *rangesOffset,
                    mainCu.addrSize)) {
              return false;
            }
            foundAddress = true;
          }
        }
        break;
    }
    return (expectedAttributes != 0); // continue forEachAttribute
  });

  if (!foundAddress || !lineOffset) {
    return false;
  }

  if (mainFileName) {
    frame.location.hasMainFile = true;
    frame.location.mainFile = Path(compilationDirectory, "", *mainFileName);
  }

  if (*lineOffset > mainCu.debugSections.debugLine.size()) {
    return false;
  }
  auto lineSection = mainCu.debugSections.debugLine.subspan(*lineOffset);
  DwarfLineNumberVM lineVM(
      lineSection, compilationDirectory, mainCu.debugSections);

  // Execute line number VM program to find file and line
  frame.location.hasFileAndLine =
      lineVM.findAddress(address, frame.location.file, frame.location.line);
  if (!frame.location.hasFileAndLine) {
    return false;
  }

  // NOTE: locationInfo was found, so findLocation returns success bellow.
  // Missing inline function / parameter name is not a failure (best effort).
  bool checkInline =
      (mode_ == LocationInfoMode::FULL_WITH_INLINE && !inlineFrames.empty());
  if (!checkInline && !eachParameterName) {
    return true;
  }

  auto& cu = cu_.defaultCompilationUnit();
  if (cu_.splitCU.hasValue()) {
    die = getDieAtOffset(*cu_.splitCU, cu_.splitCU->firstDie);
  }

  // Cache abbreviation.
  std::array<DIEAbbreviation, kMaxAbbreviationEntries> abbrs;
  cu.abbrCache = folly::range(abbrs);
  folly::StringPiece abbrev = asStringPiece(cu.debugSections.debugAbbrev);
  abbrev.advance(cu.abbrevOffset.value_or(0));
  DIEAbbreviation abbr;
  while (readAbbreviation(abbrev, abbr)) {
    // Abbreviation code 0 is reserved for null debugging information entries.
    if (abbr.code != 0 && abbr.code <= kMaxAbbreviationEntries) {
      cu.abbrCache.data()[abbr.code - 1] = abbr;
    }
  }

  // Find the subprogram that matches the given address.
  Die subprogram;
  if (!findSubProgramDieForAddress(cu, die, address, baseAddrCU, subprogram)) {
    // Even though @cu contains @address, it's possible
    // that the corresponding DW_TAG_subprogram DIE is missing.
    return true;
  }

  if (auto name = getFunctionNameFromDie(cu, subprogram); !name.empty()) {
    // frame.name may already be filled from an earlier call to:
    //   frame.name = elf->getSymbolName(elf->getDefinitionByAddress(address))
    //
    // It's possible to have multiple symbols point to the same addresses.
    // - to disambiguate use the DW_AT_linkage_name/DW_AT_name as found in the
    //   subprogram DIE.
    //
    // It's possible that DW_AT_linkage_name is a prefix of the symbol name
    // - e.g. coroutines may add .resume/.destroy/.cleanup suffixes
    //   to the symbol name, but not to DW_AT_linkage_name.
    // - If names share a common prefix, prefer the more specific name.
    if (!folly::StringPiece(frame.name).startsWith(name)) {
      frame.name = name.data();
    }
  }

  if (eachParameterName) {
    forEachChild(cu, subprogram, [&](const Die& child) {
      if (child.abbr.tag == DW_TAG_formal_parameter) {
        if (auto name =
                getAttribute<folly::StringPiece>(cu, child, DW_AT_name)) {
          eachParameterName(*name);
        }
      }
      return true; // continue forEachChild
    });
  }

  if (!checkInline || !subprogram.abbr.hasChildren) {
    return true;
  }

  // NOTE: @subprogram is the DIE of caller function.
  // Use an extra location and get its call file and call line, so that
  // they can be used for the second last location when we don't have
  // enough inline frames for all inline functions call stack.
  size_t size =
      std::min<size_t>(kMaxInlineLocationInfoPerFrame, inlineFrames.size()) + 1;
  CallLocation callLocations[kMaxInlineLocationInfoPerFrame + 1];
  size_t numFound = 0;
  findInlinedSubroutineDieForAddress(
      cu,
      subprogram,
      lineVM,
      address,
      baseAddrCU,
      folly::Range<CallLocation*>(callLocations, size),
      numFound);
  folly::Range<CallLocation*> inlineLocations(callLocations, numFound);
  fillInlineFrames(address, frame, inlineLocations, inlineFrames);
  return true;
}

void DwarfImpl::fillInlineFrames(
    uintptr_t address,
    SymbolizedFrame& frame,
    folly::Range<CallLocation*> inlineLocations,
    folly::Range<SymbolizedFrame*> inlineFrames) const {
  if (inlineLocations.empty()) {
    return;
  }
  size_t numFound = inlineLocations.size();
  const auto innerMostFile = frame.location.file;
  const auto innerMostLine = frame.location.line;

  // Earlier we filled in locationInfo:
  // - mainFile: the path to the CU -- the file where the non-inlined
  //   call is made from.
  // - file + line: the location of the inner-most inlined call.
  // Here we already find inlined info so mainFile would be redundant.
  frame.location.hasMainFile = false;
  frame.location.mainFile = Path{};
  // @findInlinedSubroutineDieForAddress fills inlineLocations[0] with the
  // file+line of the non-inlined outer function making the call.
  // frame.location.name is already set by the caller by looking up the
  // non-inlined function @address belongs to.
  frame.location.hasFileAndLine = true;
  frame.location.file = inlineLocations[0].file;
  frame.location.line = inlineLocations[0].line;

  // The next inlined subroutine's call file and call line is the current
  // caller's location.
  for (size_t i = 0; i < numFound - 1; i++) {
    inlineLocations[i].file = inlineLocations[i + 1].file;
    inlineLocations[i].line = inlineLocations[i + 1].line;
  }
  // CallLocation for the inner-most inlined function:
  // - will be computed if enough space was available in the passed
  //   buffer.
  // - will have a .name, but no !.file && !.line
  // - its corresponding file+line is the one returned by LineVM based
  //   on @address.
  // Use the inner-most inlined file+line info we got from the LineVM.
  inlineLocations[numFound - 1].file = innerMostFile;
  inlineLocations[numFound - 1].line = innerMostLine;

  // Skip the extra location when actual inline function calls are more
  // than provided frames.
  inlineLocations =
      inlineLocations.subpiece(0, std::min(numFound, inlineFrames.size()));

  // Fill in inline frames in reverse order (as
  // expected by the caller).
  std::reverse(inlineLocations.begin(), inlineLocations.end());
  for (size_t i = 0; i < inlineLocations.size(); i++) {
    inlineFrames[i].found = true;
    inlineFrames[i].addr = address;
    inlineFrames[i].name = inlineLocations[i].name.data();
    inlineFrames[i].location.hasFileAndLine = true;
    inlineFrames[i].location.file = inlineLocations[i].file;
    inlineFrames[i].location.line = inlineLocations[i].line;
  }
}

// Finds the Compilation Unit starting at offset.
CompilationUnit DwarfImpl::findCompilationUnit(
    const CompilationUnit& cu, uint64_t targetOffset) const {
  FOLLY_SAFE_DCHECK(
      targetOffset < cu.debugSections.debugInfo.size(),
      "unexpected target address");
  uint64_t offset = 0;
  while (offset < cu.debugSections.debugInfo.size()) {
    folly::StringPiece chunk(asStringPiece(cu.debugSections.debugInfo));
    chunk.advance(offset);

    auto initialLength = read<uint32_t>(chunk);
    auto is64Bit = (initialLength == (uint32_t)-1);
    auto size = is64Bit ? read<uint64_t>(chunk) : initialLength;
    if (size > chunk.size()) {
      FOLLY_SAFE_DFATAL(
          "invalid chunk size: ", size, " chunk.size(): ", chunk.size());
      break;
    }
    size += is64Bit ? 12 : 4;
    if (offset + size > targetOffset) {
      break;
    }
    offset += size;
  }
  return getCompilationUnits(
             elfCache_, cu.debugSections, offset, /* requireSplitDwarf */ false)
      .mainCompilationUnit;
}

size_t DwarfImpl::forEachChild(
    const CompilationUnit& cu,
    const Die& die,
    folly::FunctionRef<bool(const Die& die)> f) const {
  size_t nextDieOffset = forEachAttribute(cu, die, [&](const Attribute&) {
    return true;
  });
  if (!die.abbr.hasChildren) {
    return nextDieOffset;
  }

  auto childDie = getDieAtOffset(cu, nextDieOffset);
  while (childDie.code != 0) {
    if (!f(childDie)) {
      return childDie.offset;
    }

    // NOTE: Don't run `f` over grandchildren, just skip over them.
    size_t siblingOffset = forEachChild(cu, childDie, [](const Die&) {
      return true;
    });
    childDie = getDieAtOffset(cu, siblingOffset);
  }

  // childDie is now a dummy die whose offset is to the code 0 marking the
  // end of the children. Need to add one to get the offset of the next die.
  return childDie.offset + 1;
}

bool DwarfImpl::isAddrInRangeList(
    const CompilationUnit& cu,
    uint64_t address,
    folly::Optional<uint64_t> baseAddr,
    size_t offset,
    uint8_t addrSize) const {
  if (addrSize != 4 && addrSize != 8) {
    FOLLY_SAFE_DFATAL("wrong address size: ", int(addrSize));
    return false;
  }
  if (cu.version <= 4 && !cu.debugSections.debugRanges.empty()) {
    const bool is64BitAddr = addrSize == 8;
    folly::StringPiece sp = asStringPiece(cu.debugSections.debugRanges);
    if (offset > sp.size()) {
      return false;
    }
    sp.advance(offset);
    const uint64_t maxAddr = is64BitAddr
        ? std::numeric_limits<uint64_t>::max()
        : std::numeric_limits<uint32_t>::max();
    while (sp.size() >= 2 * addrSize) {
      uint64_t begin = readOffset(sp, is64BitAddr);
      uint64_t end = readOffset(sp, is64BitAddr);
      // The range list entry is a base address selection entry.
      if (begin == maxAddr) {
        baseAddr = end;
        continue;
      }
      // The range list entry is an end of list entry.
      if (begin == 0 && end == 0) {
        break;
      }

      // Check if the given address falls in the range list entry.
      // 2.17.3 Non-Contiguous Address Ranges
      // The applicable base address of a range list entry is determined by
      // the closest preceding base address selection entry (see below) in the
      // same range list. If there is no such selection entry, then the
      // applicable base address defaults to the base address of the
      // compilation unit.
      if (baseAddr && address >= begin + *baseAddr &&
          address < end + *baseAddr) {
        return true;
      }
    }
  }

  if (cu.version == 5 && !cu.debugSections.debugRnglists.empty() &&
      cu.addrBase.has_value()) {
    auto debugRnglists = asStringPiece(cu.debugSections.debugRnglists);
    debugRnglists.advance(offset);

    while (!debugRnglists.empty()) {
      auto kind = read<uint8_t>(debugRnglists);
      switch (kind) {
        case DW_RLE_end_of_list:
          return false;
        case DW_RLE_base_addressx: {
          auto index = readULEB(debugRnglists);
          auto sp = asStringPiece(cu.debugSections.debugAddr)
                        .subpiece(*cu.addrBase + index * sizeof(uint64_t));
          baseAddr = read<uint64_t>(sp);
        } break;

        case DW_RLE_startx_endx: {
          auto indexStart = readULEB(debugRnglists);
          auto indexEnd = readULEB(debugRnglists);
          auto spStart =
              asStringPiece(cu.debugSections.debugAddr)
                  .subpiece(*cu.addrBase + indexStart * sizeof(uint64_t));
          auto start = read<uint64_t>(spStart);

          auto spEnd =
              asStringPiece(cu.debugSections.debugAddr)
                  .subpiece(*cu.addrBase + indexEnd * sizeof(uint64_t));
          auto end = read<uint64_t>(spEnd);
          if (address >= start && address < end) {
            return true;
          }
        } break;

        case DW_RLE_startx_length: {
          auto indexStart = readULEB(debugRnglists);
          auto length = readULEB(debugRnglists);
          auto spStart =
              asStringPiece(cu.debugSections.debugAddr)
                  .subpiece(*cu.addrBase + indexStart * sizeof(uint64_t));
          auto start = read<uint64_t>(spStart);
          auto end = start + length;
          if (start != end && address >= start && address < end) {
            return true;
          }
        } break;

        case DW_RLE_offset_pair: {
          auto offsetStart = readULEB(debugRnglists);
          auto offsetEnd = readULEB(debugRnglists);
          if (baseAddr && address >= (*baseAddr + offsetStart) &&
              address < (*baseAddr + offsetEnd)) {
            return true;
          }
        } break;

        case DW_RLE_base_address:
          baseAddr = read<uint64_t>(debugRnglists);
          break;

        case DW_RLE_start_end: {
          uint64_t start = read<uint64_t>(debugRnglists);
          uint64_t end = read<uint64_t>(debugRnglists);
          if (address >= start && address < end) {
            return true;
          }
        } break;

        case DW_RLE_start_length: {
          uint64_t start = read<uint64_t>(debugRnglists);
          uint64_t end = start + readULEB(debugRnglists);
          if (address >= start && address < end) {
            return true;
          }
        } break;

        default:
          FOLLY_SAFE_DFATAL(
              "Unexpected debug_rnglists entry kind: ", static_cast<int>(kind));
          return false;
      }
    }
  }
  return false;
}

bool DwarfImpl::findSubProgramDieForAddress(
    const CompilationUnit& cu,
    const Die& die,
    uint64_t address,
    folly::Optional<uint64_t> baseAddrCU,
    Die& subprogram) const {
  forEachChild(cu, die, [&](const Die& childDie) {
    if (childDie.abbr.tag == DW_TAG_subprogram) {
      folly::Optional<uint64_t> lowPc;
      folly::Optional<uint64_t> highPc;
      folly::Optional<bool> isHighPcAddr;
      folly::Optional<uint64_t> rangeOffset;
      forEachAttribute(cu, childDie, [&](const Attribute& attr) {
        switch (attr.spec.name) {
          case DW_AT_ranges:
            rangeOffset =
                std::get<uint64_t>(attr.attrValue) + cu.rangesBase.value_or(0);
            break;
          case DW_AT_low_pc:
            lowPc = std::get<uint64_t>(attr.attrValue);
            break;
          case DW_AT_high_pc:
            // The value of the DW_AT_high_pc attribute can be
            // an address (DW_FORM_addr*) or an offset (DW_FORM_data*).
            isHighPcAddr = attr.spec.form == DW_FORM_addr || //
                attr.spec.form == DW_FORM_addrx || //
                attr.spec.form == DW_FORM_addrx1 || //
                attr.spec.form == DW_FORM_addrx2 || //
                attr.spec.form == DW_FORM_addrx3 || //
                attr.spec.form == DW_FORM_addrx4;
            highPc = std::get<uint64_t>(attr.attrValue);
            break;
        }
        return true; // continue forEachAttribute
      });

      bool pcMatch = lowPc && highPc && isHighPcAddr && address >= *lowPc &&
          (address < (*isHighPcAddr ? *highPc : *lowPc + *highPc));
      if (pcMatch) {
        subprogram = childDie;
        return false; // stop forEachChild
      }

      bool rangeMatch = rangeOffset &&
          isAddrInRangeList(cu, address, baseAddrCU, *rangeOffset, cu.addrSize);
      if (rangeMatch) {
        subprogram = childDie;
        return false; // stop forEachChild
      }
    }

    // Continue forEachChild to next sibling DIE only if not already found.
    return !findSubProgramDieForAddress(
        cu, childDie, address, baseAddrCU, subprogram);
  });
  return subprogram.abbr.tag == DW_TAG_subprogram;
}

/**
 * Find DW_TAG_inlined_subroutine child DIEs that contain @address and
 * then extract:
 * - Where was it called from (DW_AT_call_file & DW_AT_call_line):
 *   the statement or expression that caused the inline expansion.
 * - The inlined function's name. As a function may be inlined multiple
 *   times, common attributes like DW_AT_linkage_name or DW_AT_name
 *   are only stored in its "concrete out-of-line instance" (a
 *   DW_TAG_subprogram) which we find using DW_AT_abstract_origin.
 */
void DwarfImpl::findInlinedSubroutineDieForAddress(
    const CompilationUnit& cu,
    const Die& die,
    const DwarfLineNumberVM& lineVM,
    uint64_t address,
    folly::Optional<uint64_t> baseAddrCU,
    folly::Range<CallLocation*> locations,
    size_t& numFound) const {
  if (numFound >= locations.size()) {
    return;
  }

  forEachChild(cu, die, [&](const Die& childDie) {
    // Between a DW_TAG_subprogram and DW_TAG_inlined_subroutine we might
    // have arbitrary intermediary "nodes", including DW_TAG_common_block,
    // DW_TAG_lexical_block, DW_TAG_try_block, DW_TAG_catch_block and
    // DW_TAG_with_stmt, etc.
    // We can't filter with location here since its range may be not specified.
    // See section 2.6.2: A location list containing only an end of list entry
    // describes an object that exists in the source code but not in the
    // executable program.
    if (childDie.abbr.tag == DW_TAG_try_block ||
        childDie.abbr.tag == DW_TAG_catch_block ||
        childDie.abbr.tag == DW_TAG_entry_point ||
        childDie.abbr.tag == DW_TAG_common_block ||
        childDie.abbr.tag == DW_TAG_lexical_block) {
      findInlinedSubroutineDieForAddress(
          cu, childDie, lineVM, address, baseAddrCU, locations, numFound);
      return true;
    }

    if (childDie.abbr.tag != DW_TAG_subprogram &&
        childDie.abbr.tag != DW_TAG_inlined_subroutine) {
      return true;
    }

    folly::Optional<uint64_t> lowPc;
    folly::Optional<uint64_t> highPc;
    folly::Optional<bool> isHighPcAddr;
    folly::Optional<uint64_t> abstractOrigin;
    folly::Optional<uint64_t> abstractOriginRefType;
    folly::Optional<uint64_t> callFile;
    folly::Optional<uint64_t> callLine;
    folly::Optional<uint64_t> rangeOffset;
    forEachAttribute(cu, childDie, [&](const Attribute& attr) {
      switch (attr.spec.name) {
        case DW_AT_ranges:
          rangeOffset =
              std::get<uint64_t>(attr.attrValue) + cu.rangesBase.value_or(0);
          break;
        case DW_AT_low_pc:
          lowPc = std::get<uint64_t>(attr.attrValue);
          break;
        case DW_AT_high_pc:
          // The value of the DW_AT_high_pc attribute can be
          // an address (DW_FORM_addr*) or an offset (DW_FORM_data*).
          isHighPcAddr = attr.spec.form == DW_FORM_addr || //
              attr.spec.form == DW_FORM_addrx || //
              attr.spec.form == DW_FORM_addrx1 || //
              attr.spec.form == DW_FORM_addrx2 || //
              attr.spec.form == DW_FORM_addrx3 || //
              attr.spec.form == DW_FORM_addrx4;
          highPc = std::get<uint64_t>(attr.attrValue);
          break;
        case DW_AT_abstract_origin:
          abstractOriginRefType = attr.spec.form;
          abstractOrigin = std::get<uint64_t>(attr.attrValue);
          break;
        case DW_AT_call_line:
          callLine = std::get<uint64_t>(attr.attrValue);
          break;
        case DW_AT_call_file:
          callFile = std::get<uint64_t>(attr.attrValue);
          break;
      }
      return true; // continue forEachAttribute
    });

    // 2.17 Code Addresses and Ranges
    // Any debugging information entry describing an entity that has a
    // machine code address or range of machine code addresses,
    // which includes compilation units, module initialization, subroutines,
    // ordinary blocks, try/catch blocks, labels and the like, may have
    //  - A DW_AT_low_pc attribute for a single address,
    //  - A DW_AT_low_pc and DW_AT_high_pc pair of attributes for a
    //    single contiguous range of addresses, or
    //  - A DW_AT_ranges attribute for a non-contiguous range of addresses.
    // TODO: Support DW_TAG_entry_point and DW_TAG_common_block that don't
    // have DW_AT_low_pc/DW_AT_high_pc pairs and DW_AT_ranges.
    // TODO: Support relocated address which requires lookup in relocation
    // map.
    bool pcMatch = lowPc && highPc && isHighPcAddr && address >= *lowPc &&
        (address < (*isHighPcAddr ? *highPc : (*lowPc + *highPc)));
    bool rangeMatch = rangeOffset &&
        isAddrInRangeList(cu, address, baseAddrCU, *rangeOffset, cu.addrSize);
    if (!pcMatch && !rangeMatch) {
      // Address doesn't match. Keep searching other children.
      return true;
    }

    if (!abstractOrigin || !abstractOriginRefType || !callLine || !callFile) {
      // We expect a single sibling DIE to match on addr, but it's missing
      // required fields. Stop searching for other DIEs.
      return false;
    }

    locations[numFound].file = lineVM.getFullFileName(*callFile);
    locations[numFound].line = *callLine;

    // DW_AT_abstract_origin is a reference. There a 3 types of references:
    // - the reference can identify any debugging information entry within the
    //   compilation unit (DW_FORM_ref1, DW_FORM_ref2, DW_FORM_ref4,
    //   DW_FORM_ref8, DW_FORM_ref_udata). This type of reference is an offset
    //   from the first byte of the compilation header for the compilation
    //   unit containing the reference.
    // - the reference can identify any debugging information entry within a
    //   .debug_info section; in particular, it may refer to an entry in a
    //   different compilation unit (DW_FORM_ref_addr)
    // - the reference can identify any debugging information type entry that
    //   has been placed in its own type unit.
    //   Not applicable for DW_AT_abstract_origin.
    locations[numFound].name = (*abstractOriginRefType != DW_FORM_ref_addr)
        ? getFunctionName(cu, cu.offset + *abstractOrigin)
        : getFunctionName(
              findCompilationUnit(cu, *abstractOrigin), *abstractOrigin);
    findInlinedSubroutineDieForAddress(
        cu, childDie, lineVM, address, baseAddrCU, locations, ++numFound);

    return false;
  });
}

} // namespace symbolizer
} // namespace folly

#endif // FOLLY_HAVE_DWARF
