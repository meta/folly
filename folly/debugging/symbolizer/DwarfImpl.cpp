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
#include <limits>
#include <optional>
#include <type_traits>

#include <folly/Optional.h>
#include <folly/ScopeGuard.h>
#include <folly/debugging/symbolizer/DwarfUtil.h>
#include <folly/debugging/symbolizer/detail/DwarfConstants.h>
#include <folly/lang/CheckedMath.h>
#include <folly/portability/Config.h>

#if FOLLY_HAVE_DWARF && FOLLY_HAVE_ELF

namespace folly {
namespace symbolizer {

namespace {

template <typename Size = size_t>
DwarfParseResult<Size> incrementDieOffset(uint64_t offset) {
  if (offset >= std::numeric_limits<Size>::max()) {
    return makeUnexpected(
        DwarfParseError{DwarfParseErrorCode::integer_overflow, offset});
  }
  return static_cast<Size>(offset) + 1;
}

// Between a DW_TAG_subprogram and DW_TAG_inlined_subroutine we might have
// arbitrary intermediary "nodes", including DW_TAG_common_block,
// DW_TAG_lexical_block, DW_TAG_try_block, DW_TAG_catch_block and
// DW_TAG_with_stmt, etc.
bool isNestedBlockTag(uint64_t tag) {
  return //
      tag == DW_TAG_try_block || //
      tag == DW_TAG_catch_block || //
      tag == DW_TAG_entry_point || //
      tag == DW_TAG_common_block || //
      tag == DW_TAG_lexical_block;
}

// The value of the DW_AT_high_pc attribute can be an address (DW_FORM_addr*)
// or an offset (DW_FORM_data*).
bool isHighPcAddressForm(uint64_t form) {
  return //
      form == DW_FORM_addr || //
      form == DW_FORM_addrx || //
      form == DW_FORM_addrx1 || //
      form == DW_FORM_addrx2 || //
      form == DW_FORM_addrx3 || //
      form == DW_FORM_addrx4;
}

// Offset just past the attributes of @die: the offset of its first child when
// it has children, of its next sibling when it does not.
DwarfParseResult<size_t> endOfDieAttributes(
    const CompilationUnit& cu, const Die& die) {
  auto offset = forEachAttribute(cu, die, [](const Attribute&) {
    return true;
  });
  if (offset.hasError()) {
    return makeUnexpected(offset.error());
  }
  if (!offset->hasValue()) {
    return makeUnexpected(
        DwarfParseError{DwarfParseErrorCode::invalid_value, die.offset});
  }
  return **offset;
}

// Offset of the DIE that follows @die and all of its descendants.
//
// DIEs are stored depth first, so the subtree is skipped by counting the
// sibling chains that are still open rather than by descending into each
// child: nesting depth is controlled by the file being parsed, and
// symbolization may run on a small sigaltstack.
DwarfParseResult<size_t> skipDie(const CompilationUnit& cu, const Die& die) {
  auto offset = endOfDieAttributes(cu, die);
  if (offset.hasError()) {
    return makeUnexpected(offset.error());
  }
  size_t next = *offset;
  for (size_t openChains = die.abbr.hasChildren ? 1 : 0; openChains != 0;) {
    auto sibling = getDieAtOffset(cu, next);
    if (sibling.hasError()) {
      return makeUnexpected(sibling.error());
    }
    if (sibling->code == 0) {
      auto afterTerminator = incrementDieOffset(sibling->offset);
      if (afterTerminator.hasError()) {
        return makeUnexpected(afterTerminator.error());
      }
      next = *afterTerminator;
      --openChains;
      continue;
    }
    auto end = endOfDieAttributes(cu, *sibling);
    if (end.hasError()) {
      return makeUnexpected(end.error());
    }
    next = *end;
    if (sibling->abbr.hasChildren) {
      ++openChains;
    }
  }
  return next;
}

// Offset just past the terminator of the sibling chain starting at @offset,
// skipping every remaining sibling and its descendants.
DwarfParseResult<size_t> skipRemainingSiblings(
    const CompilationUnit& cu, size_t offset) {
  for (;;) {
    auto die = getDieAtOffset(cu, offset);
    if (die.hasError()) {
      return makeUnexpected(die.error());
    }
    if (die->code == 0) {
      return incrementDieOffset(die->offset);
    }
    auto next = skipDie(cu, *die);
    if (next.hasError()) {
      return makeUnexpected(next.error());
    }
    offset = *next;
  }
}

// DW_AT_abstract_origin and the call site of an inlined subroutine.
struct InlinedCallInfo {
  std::optional<uint64_t> abstractOrigin;
  std::optional<uint64_t> abstractOriginRefType;
  std::optional<uint64_t> callFile;
  std::optional<uint64_t> callLine;
};

// The machine code addresses of a DIE: 2.17 Code Addresses and Ranges.
struct DieAddressRange {
  std::optional<uint64_t> lowPc;
  std::optional<uint64_t> highPc;
  std::optional<bool> isHighPcAddr;
  std::optional<uint64_t> rangeOffset;
};

// @callInfo is read only when non-null, so that callers which never look at
// the call site do not reject DIEs whose call attributes are malformed.
DwarfParseResult<DieAddressRange> readDieAddressRange(
    const CompilationUnit& cu,
    const Die& die,
    InlinedCallInfo* callInfo = nullptr) {
  DieAddressRange range;
  bool typeMismatch = false;
  std::optional<DwarfParseError> overflow;
  auto getUnsigned = [&](const Attribute& attr) {
    const auto* value = std::get_if<uint64_t>(&attr.attrValue);
    typeMismatch |= value == nullptr;
    return value;
  };
  auto attributes = forEachAttribute(cu, die, [&](const Attribute& attr) {
    switch (attr.spec.name) {
      case DW_AT_ranges: {
        const auto* value = getUnsigned(attr);
        if (value == nullptr) {
          return false;
        }
        const auto base = cu.rangesBase.value_or(0);
        uint64_t rangeOffset;
        if (!folly::checked_add(&rangeOffset, *value, base)) {
          overflow = DwarfParseError{
              DwarfParseErrorCode::integer_overflow,
              die.offset,
          };
          return false;
        }
        range.rangeOffset = rangeOffset;
        break;
      }
      case DW_AT_low_pc: {
        const auto* value = getUnsigned(attr);
        if (value == nullptr) {
          return false;
        }
        range.lowPc = *value;
        break;
      }
      case DW_AT_high_pc: {
        const auto* value = getUnsigned(attr);
        if (value == nullptr) {
          return false;
        }
        range.isHighPcAddr = isHighPcAddressForm(attr.spec.form);
        range.highPc = *value;
        break;
      }
      case DW_AT_abstract_origin: {
        if (callInfo == nullptr) {
          break;
        }
        const auto* value = getUnsigned(attr);
        if (value == nullptr) {
          return false;
        }
        callInfo->abstractOriginRefType = attr.spec.form;
        callInfo->abstractOrigin = *value;
        break;
      }
      case DW_AT_call_line: {
        if (callInfo == nullptr) {
          break;
        }
        const auto* value = getUnsigned(attr);
        if (value == nullptr) {
          return false;
        }
        callInfo->callLine = *value;
        break;
      }
      case DW_AT_call_file: {
        if (callInfo == nullptr) {
          break;
        }
        const auto* value = getUnsigned(attr);
        if (value == nullptr) {
          return false;
        }
        callInfo->callFile = *value;
        break;
      }
    }
    return true; // continue forEachAttribute
  });
  if (attributes.hasError()) {
    return makeUnexpected(attributes.error());
  }
  if (overflow) {
    return makeUnexpected(*overflow);
  }
  if (typeMismatch) {
    return makeUnexpected(
        DwarfParseError{DwarfParseErrorCode::invalid_value, die.offset});
  }
  return range;
}

// Whether the DW_AT_low_pc / DW_AT_high_pc pair of a DIE covers @address.
DwarfParseResult<bool> rangeContainsAddress(
    const DieAddressRange& range, uint64_t address, size_t errorOffset) {
  if (!range.lowPc || !range.highPc || !range.isHighPcAddr) {
    return false;
  }
  uint64_t end = *range.highPc;
  if (!*range.isHighPcAddr &&
      !folly::checked_add(&end, *range.lowPc, *range.highPc)) {
    return makeUnexpected(
        DwarfParseError{DwarfParseErrorCode::integer_overflow, errorOffset});
  }
  return address >= *range.lowPc && address < end;
}

} // namespace

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
  auto dieResult = getDieAtOffset(mainCu, mainCu.firstDie);
  if (dieResult.hasError()) {
    return false;
  }
  Die die = *dieResult;
  // Partial compilation unit (DW_TAG_partial_unit) is not supported.
  if (die.abbr.tag != DW_TAG_compile_unit &&
      die.abbr.tag != DW_TAG_skeleton_unit) {
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

  bool typeMismatch = false;
  folly::Optional<DwarfParseError> rangeParseError;
  auto getUnsigned = [&](const Attribute& attr) {
    const auto* value = std::get_if<uint64_t>(&attr.attrValue);
    typeMismatch |= value == nullptr;
    return value;
  };
  auto getString = [&](const Attribute& attr) {
    const auto* value = std::get_if<folly::StringPiece>(&attr.attrValue);
    typeMismatch |= value == nullptr;
    return value;
  };
  auto attributes = forEachAttribute(mainCu, die, [&](const Attribute& attr) {
    switch (attr.spec.name) {
      case DW_AT_stmt_list: {
        const auto* value = getUnsigned(attr);
        if (value == nullptr) {
          return false;
        }
        expectedAttributes &= ~kStmtList;
        // Offset in .debug_line for the line number VM program for this
        // compilation unit
        lineOffset = *value;
        break;
      }
      case DW_AT_comp_dir: {
        const auto* value = getString(attr);
        if (value == nullptr) {
          return false;
        }
        expectedAttributes &= ~kCompDir;
        // Compilation directory
        compilationDirectory = *value;
        break;
      }
      case DW_AT_name: {
        const auto* value = getString(attr);
        if (value == nullptr) {
          return false;
        }
        expectedAttributes &= ~kName;
        // File name of main file being compiled
        mainFileName = *value;
        break;
      }
      case DW_AT_low_pc: {
        const auto* value = getUnsigned(attr);
        if (value == nullptr) {
          return false;
        }
        expectedAttributes &= ~kLowPC;
        baseAddrCU = *value;
        if (!foundAddress) {
          if (address < *baseAddrCU) {
            return false;
          }
          seenLowPC = true;
          if (seenHighPC) {
            foundAddress = true;
          } else if (rangesOffset) {
            auto rangeMatch = isAddressInRangeList(
                mainCu, address, baseAddrCU, *rangesOffset);
            if (rangeMatch.hasError()) {
              rangeParseError = rangeMatch.error();
              return false;
            }
            if (!*rangeMatch) {
              return false;
            }
            foundAddress = true;
          }
        }
        break;
      }
      case DW_AT_high_pc: {
        const auto* value = getUnsigned(attr);
        if (value == nullptr) {
          return false;
        }
        expectedAttributes &= ~kHighPCOrRanges;
        if (!foundAddress) {
          if (address >= *value) {
            return false;
          }
          seenHighPC = true;
          foundAddress = seenLowPC;
        }
        break;
      }
      case DW_AT_ranges: {
        const auto* value = getUnsigned(attr);
        if (value == nullptr) {
          return false;
        }
        // 3.1.1: CU entries have:
        // - either DW_AT_low_pc and DW_AT_high_pc
        // OR
        // - DW_AT_ranges and optional DW_AT_low_pc
        expectedAttributes &= ~kHighPCOrRanges;
        if (!foundAddress) {
          rangesOffset = *value;
          if (seenLowPC) {
            auto rangeMatch = isAddressInRangeList(
                mainCu, address, baseAddrCU, *rangesOffset);
            if (rangeMatch.hasError()) {
              rangeParseError = rangeMatch.error();
              return false;
            }
            if (!*rangeMatch) {
              return false;
            }
            foundAddress = true;
          }
        }
        break;
      }
    }
    return (expectedAttributes != 0); // continue forEachAttribute
  });
  if (attributes.hasError() || typeMismatch || rangeParseError.has_value()) {
    return false;
  }

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
    auto splitDie = getDieAtOffset(*cu_.splitCU, cu_.splitCU->firstDie);
    if (splitDie.hasError()) {
      return true;
    }
    die = *splitDie;
  }

  // Cache abbreviation.
  std::array<DIEAbbreviation, kMaxAbbreviationEntries> abbrs;
  cu.abbrCache = folly::range(abbrs);
  auto resetAbbrCache = folly::makeGuard([&] { cu.abbrCache = {}; });
  const auto abbrevOffset = cu.abbrevOffset.value_or(0);
  if (abbrevOffset > cu.debugSections.debugAbbrev.size()) {
    return false;
  }
  DwarfCursor abbrev(
      cu.debugSections.debugAbbrev.subspan(abbrevOffset), abbrevOffset);
  for (;;) {
    auto abbr = readAbbreviation(abbrev);
    if (abbr.hasError()) {
      return true;
    }
    if (!abbr->hasValue()) {
      break;
    }
    // Abbreviation code 0 is reserved for null debugging information entries.
    if ((**abbr).code <= kMaxAbbreviationEntries) {
      abbrs[(**abbr).code - 1] = **abbr;
    }
  }

  // Find the subprogram that matches the given address.
  Die subprogram;
  auto foundSubprogram =
      findSubProgramDieForAddress(cu, die, address, baseAddrCU, subprogram);
  if (foundSubprogram.hasError() || !*foundSubprogram) {
    // Even though @cu contains @address, it's possible
    // that the corresponding DW_TAG_subprogram DIE is missing.
    return true;
  }

  auto functionName = getFunctionNameFromDie(cu, subprogram);
  if (functionName.hasValue() && !functionName->empty()) {
    const auto name = *functionName;
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
    // frame.name is null when the ELF symbol table has no name for the
    // address, which a malformed symbol table also reports.
    if (frame.name == nullptr ||
        !folly::StringPiece(frame.name).startsWith(name)) {
      frame.name = name.data();
    }
  }

  if (eachParameterName) {
    bool parameterError = false;
    auto parameters = forEachChild(cu, subprogram, [&](const Die& child) {
      if (child.abbr.tag == DW_TAG_formal_parameter) {
        auto name = getAttribute<folly::StringPiece>(cu, child, DW_AT_name);
        if (name.hasError()) {
          parameterError = true;
          return false;
        }
        if (name->hasValue()) {
          eachParameterName(**name);
        }
      }
      return true; // continue forEachChild
    });
    if (parameters.hasError() || parameterError) {
      return true;
    }
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
  auto inlineResult = findInlinedSubroutineDieForAddress(
      cu,
      subprogram,
      lineVM,
      address,
      baseAddrCU,
      folly::Range<CallLocation*>(callLocations, size),
      numFound);
  if (inlineResult.hasError()) {
    return true;
  }
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
  if (targetOffset >= cu.debugSections.debugInfo.size()) {
    return {};
  }
  size_t offset = 0;
  while (offset < cu.debugSections.debugInfo.size()) {
    auto header =
        parseCompilationUnitHeader(cu.debugSections.debugInfo, offset);
    if (header.hasError() || header->size == 0) {
      return {};
    }
    if (offset + header->size > targetOffset) {
      break;
    }
    offset += header->size;
  }
  return getCompilationUnits(
             elfCache_, cu.debugSections, offset, /* requireSplitDwarf */ false)
      .mainCompilationUnit;
}

DwarfParseResult<size_t> DwarfImpl::forEachChild(
    const CompilationUnit& cu,
    const Die& die,
    folly::FunctionRef<bool(const Die& die)> f) const {
  auto nextDieOffset = endOfDieAttributes(cu, die);
  if (nextDieOffset.hasError()) {
    return makeUnexpected(nextDieOffset.error());
  }
  if (!die.abbr.hasChildren) {
    return *nextDieOffset;
  }

  auto childDie = getDieAtOffset(cu, *nextDieOffset);
  if (childDie.hasError()) {
    return makeUnexpected(childDie.error());
  }
  while (childDie->code != 0) {
    if (!f(*childDie)) {
      return childDie->offset;
    }

    // NOTE: Don't run `f` over grandchildren, just skip over them.
    auto siblingOffset = skipDie(cu, *childDie);
    if (siblingOffset.hasError()) {
      return makeUnexpected(siblingOffset.error());
    }
    childDie = getDieAtOffset(cu, *siblingOffset);
    if (childDie.hasError()) {
      return makeUnexpected(childDie.error());
    }
  }

  // childDie is now a dummy die whose offset is to the code 0 marking the
  // end of the children. Need to add one to get the offset of the next die.
  return incrementDieOffset(childDie->offset);
}

DwarfParseResult<bool> DwarfImpl::findSubProgramDieForAddress(
    const CompilationUnit& cu,
    const Die& die,
    uint64_t address,
    folly::Optional<uint64_t> baseAddrCU,
    Die& subprogram) const {
  // Depth first search over the DIEs below @die, walked iteratively like
  // skipDie above: nesting depth is controlled by the file being parsed.
  auto offset = endOfDieAttributes(cu, die);
  if (offset.hasError()) {
    return makeUnexpected(offset.error());
  }
  size_t next = *offset;
  for (size_t openChains = die.abbr.hasChildren ? 1 : 0; openChains != 0;) {
    auto childDie = getDieAtOffset(cu, next);
    if (childDie.hasError()) {
      return makeUnexpected(childDie.error());
    }
    if (childDie->code == 0) {
      auto afterTerminator = incrementDieOffset(childDie->offset);
      if (afterTerminator.hasError()) {
        return makeUnexpected(afterTerminator.error());
      }
      next = *afterTerminator;
      --openChains;
      continue;
    }

    if (childDie->abbr.tag == DW_TAG_subprogram) {
      auto range = readDieAddressRange(cu, *childDie);
      if (range.hasError()) {
        return makeUnexpected(range.error());
      }
      auto pcMatch = rangeContainsAddress(*range, address, childDie->offset);
      if (pcMatch.hasError()) {
        return makeUnexpected(pcMatch.error());
      }
      bool found = *pcMatch;
      if (!found && range->rangeOffset) {
        auto rangeMatch =
            isAddressInRangeList(cu, address, baseAddrCU, *range->rangeOffset);
        if (rangeMatch.hasError()) {
          return makeUnexpected(rangeMatch.error());
        }
        found = *rangeMatch;
      }
      if (found) {
        subprogram = *childDie;
        return true;
      }
    }

    auto end = endOfDieAttributes(cu, *childDie);
    if (end.hasError()) {
      return makeUnexpected(end.error());
    }
    next = *end;
    if (childDie->abbr.hasChildren) {
      ++openChains;
    }
  }
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
DwarfParseResult<bool> DwarfImpl::findInlinedSubroutineDieForAddress(
    const CompilationUnit& cu,
    const Die& die,
    const DwarfLineNumberVM& lineVM,
    uint64_t address,
    folly::Optional<uint64_t> baseAddrCU,
    folly::Range<CallLocation*> locations,
    size_t& numFound) const {
  if (numFound >= locations.size()) {
    return true;
  }
  auto dieEnd = endOfDieAttributes(cu, die);
  if (dieEnd.hasError()) {
    return makeUnexpected(dieEnd.error());
  }
  if (!die.abbr.hasChildren) {
    return true;
  }

  // Levels whose remaining siblings are skipped once the walk comes back to
  // them: a DIE that contains @address is expected to be the only match among
  // its siblings. A level is pushed only together with a recorded call
  // location, so this holds at most as many entries as @locations has slots,
  // which lets the depth first walk below keep its state here instead of on
  // the call stack (see skipDie above for why recursion is avoided).
  std::array<size_t, kMaxInlineLocationInfoPerFrame + 1> stoppedLevels{};
  size_t stoppedCount = 0;

  size_t offset = *dieEnd;
  size_t level = 0; // the sibling chain of @die's children
  while (numFound < locations.size()) {
    if (stoppedCount != 0 && stoppedLevels[stoppedCount - 1] == level) {
      if (level == 0) {
        break;
      }
      --stoppedCount;
      auto afterChain = skipRemainingSiblings(cu, offset);
      if (afterChain.hasError()) {
        return makeUnexpected(afterChain.error());
      }
      offset = *afterChain;
      --level;
      continue;
    }

    auto childDieResult = getDieAtOffset(cu, offset);
    if (childDieResult.hasError()) {
      return makeUnexpected(childDieResult.error());
    }
    const Die& childDie = *childDieResult;
    if (childDie.code == 0) {
      auto afterTerminator = incrementDieOffset(childDie.offset);
      if (afterTerminator.hasError()) {
        return makeUnexpected(afterTerminator.error());
      }
      offset = *afterTerminator;
      if (level == 0) {
        break;
      }
      --level;
      continue;
    }

    // We can't filter with location here since its range may be not
    // specified. See section 2.6.2: A location list containing only an end of
    // list entry describes an object that exists in the source code but not in
    // the executable program.
    bool descend = isNestedBlockTag(childDie.abbr.tag);
    bool stopLevel = false;
    if (!descend &&
        (childDie.abbr.tag == DW_TAG_subprogram ||
         childDie.abbr.tag == DW_TAG_inlined_subroutine)) {
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
      InlinedCallInfo call;
      auto range = readDieAddressRange(cu, childDie, &call);
      if (range.hasError()) {
        return makeUnexpected(range.error());
      }
      auto pcMatch = rangeContainsAddress(*range, address, childDie.offset);
      if (pcMatch.hasError()) {
        return makeUnexpected(pcMatch.error());
      }
      bool rangeMatch = false;
      if (range->rangeOffset) {
        auto match =
            isAddressInRangeList(cu, address, baseAddrCU, *range->rangeOffset);
        if (match.hasError()) {
          return makeUnexpected(match.error());
        }
        rangeMatch = *match;
      }
      if (*pcMatch || rangeMatch) {
        if (!call.abstractOrigin || !call.abstractOriginRefType ||
            !call.callLine || !call.callFile) {
          // We expect a single sibling DIE to match on addr, but it's missing
          // required fields. Stop searching for other DIEs.
          stopLevel = true;
        } else {
          auto callFileName = lineVM.getFullFileName(*call.callFile);
          if (callFileName.hasError()) {
            return makeUnexpected(callFileName.error());
          }
          // Checked immediately before the write. The iterative walk cannot
          // fill @locations between the loop condition and this check; the
          // guard also protects @stoppedLevels if a future caller ever passes
          // a larger @locations buffer.
          if (numFound >= locations.size() ||
              stoppedCount == stoppedLevels.size()) {
            break;
          }
          locations[numFound].file = *callFileName;
          locations[numFound].line = *call.callLine;

          // DW_AT_abstract_origin is a reference. There a 3 types of
          // references:
          // - the reference can identify any debugging information entry
          //   within the compilation unit (DW_FORM_ref1, DW_FORM_ref2,
          //   DW_FORM_ref4, DW_FORM_ref8, DW_FORM_ref_udata). This type of
          //   reference is an offset from the first byte of the compilation
          //   header for the compilation unit containing the reference.
          // - the reference can identify any debugging information entry
          //   within a .debug_info section; in particular, it may refer to an
          //   entry in a different compilation unit (DW_FORM_ref_addr)
          // - the reference can identify any debugging information type entry
          //   that has been placed in its own type unit.
          //   Not applicable for DW_AT_abstract_origin.
          locations[numFound].name =
              (*call.abstractOriginRefType != DW_FORM_ref_addr)
              ? getFunctionName(cu, cu.offset + *call.abstractOrigin)
              : getFunctionName(
                    findCompilationUnit(cu, *call.abstractOrigin),
                    *call.abstractOrigin);
          ++numFound;
          // Nested inlined subroutines are searched next; the siblings of
          // this DIE are skipped once that search is done.
          stoppedLevels[stoppedCount++] = level;
          descend = true;
        }
      }
    }

    if (stopLevel) {
      if (level == 0) {
        break;
      }
      auto afterChain = skipRemainingSiblings(cu, offset);
      if (afterChain.hasError()) {
        return makeUnexpected(afterChain.error());
      }
      offset = *afterChain;
      --level;
      continue;
    }

    if (descend && childDie.abbr.hasChildren) {
      auto childrenOffset = endOfDieAttributes(cu, childDie);
      if (childrenOffset.hasError()) {
        return makeUnexpected(childrenOffset.error());
      }
      offset = *childrenOffset;
      ++level;
      continue;
    }

    auto sibling = skipDie(cu, childDie);
    if (sibling.hasError()) {
      return makeUnexpected(sibling.error());
    }
    offset = *sibling;
  }
  return true;
}

} // namespace symbolizer
} // namespace folly

#endif // FOLLY_HAVE_DWARF
