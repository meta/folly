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
#include <folly/debugging/symbolizer/DwarfLineNumberVM.h>
#include <folly/debugging/symbolizer/DwarfSection.h>
#include <folly/debugging/symbolizer/DwarfUtil.h>
#include <folly/debugging/symbolizer/Elf.h>
#include <folly/debugging/symbolizer/ElfCache.h>
#include <folly/debugging/symbolizer/SymbolizedFrame.h>

namespace folly {
namespace symbolizer {

#if FOLLY_HAVE_DWARF && FOLLY_HAVE_ELF

struct CallLocation;

class DwarfImpl {
 public:
  explicit DwarfImpl(
      ElfCacheBase* elfCache, CompilationUnits& cu, LocationInfoMode mode);

  /**
   * Find the @locationInfo for @address in the compilation unit @cu.
   *
   * Best effort:
   * - fills @inlineFrames if mode == FULL_WITH_INLINE,
   * - calls @eachParameterName on the function parameters.
   *
   * if @checkAddress is true, we verify that the address is mapped to
   * a range in this CU before running the line number VM
   */
  bool findLocation(
      uintptr_t address,
      SymbolizedFrame& frame,
      folly::Range<SymbolizedFrame*> inlineFrames,
      folly::FunctionRef<void(folly::StringPiece)> eachParameterName,
      bool checkAddress = true) const;

 private:
  using AttributeValue =
      std::variant<uint64_t, folly::StringPiece, std::span<const uint8_t>>;

  /**
   * Finds a subprogram debugging info entry that contains a given address among
   * children of given die. Depth first search.
   */
  bool findSubProgramDieForAddress(
      const CompilationUnit& cu,
      const Die& die,
      uint64_t address,
      folly::Optional<uint64_t> baseAddrCU,
      Die& subprogram) const;

  /**
   * Finds inlined subroutine DIEs and their caller lines that contains a given
   * address among children of given die. Depth first search.
   */
  void findInlinedSubroutineDieForAddress(
      const CompilationUnit& cu,
      const Die& die,
      const DwarfLineNumberVM& lineVM,
      uint64_t address,
      folly::Optional<uint64_t> baseAddrCU,
      folly::Range<CallLocation*> locations,
      size_t& numFound) const;

  CompilationUnit findCompilationUnit(
      const CompilationUnit& cu, uint64_t targetOffset) const;

  /**
   * Find the actual definition DIE instead of declaration for the given die.
   */
  Die findDefinitionDie(const CompilationUnit& cu, const Die& die) const;

  /**
   * Iterates over all children of a debugging info entry, calling the given
   * callable for each. Iteration is stopped early if any of the calls return
   * false. Returns the offset of next DIE after iterations.
   */
  size_t forEachChild(
      const CompilationUnit& cu,
      const Die& die,
      folly::FunctionRef<bool(const Die& die)> f) const;

  /**
   * Check if the given address is in the range list at the given offset in
   * .debug_ranges.
   */
  bool isAddrInRangeList(
      const CompilationUnit& cu,
      uint64_t address,
      folly::Optional<uint64_t> baseAddr,
      size_t offset,
      uint8_t addrSize) const;

  void fillInlineFrames(
      uintptr_t address,
      SymbolizedFrame& frame,
      folly::Range<CallLocation*> inlineLocations,
      folly::Range<SymbolizedFrame*> inlineFrames) const;

  ElfCacheBase* elfCache_;
  CompilationUnits& cu_;
  const LocationInfoMode mode_;
};

#endif

} // namespace symbolizer
} // namespace folly
