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

#include <folly/Function.h>
#include <folly/Range.h>
#include <folly/debugging/symbolizer/DwarfUtil.h>
#include <folly/debugging/symbolizer/Elf.h>
#include <folly/debugging/symbolizer/ElfCache.h>
#include <folly/debugging/symbolizer/SymbolizedFrame.h>

namespace folly {
namespace symbolizer {

#if FOLLY_HAVE_DWARF && FOLLY_HAVE_ELF

/**
 * DWARF record parser.
 *
 * We only implement enough DWARF functionality to convert from PC address
 * to file and line number information.
 *
 * This means (although they're not part of the public API of this class), we
 * can parse Debug Information Entries (DIEs), abbreviations, attributes (of
 * all forms), and we can interpret bytecode for the line number VM.
 *
 * We can interpret DWARF records of version 2, 3, or 4, although we don't
 * actually support many of the version 4 features (such as VLIW, multiple
 * operations per instruction)
 *
 * Note that the DWARF record parser does not allocate heap memory at all.
 * This is on purpose: you can use the parser from
 * memory-constrained situations (such as an exception handler for
 * std::out_of_memory)  If it weren't for this requirement, some things would
 * be much simpler: the Path class would be unnecessary and would be replaced
 * with a std::string; the list of file names in the line number VM would be
 * kept as a vector of strings instead of re-executing the program to look for
 * DW_LNE_define_file instructions, etc.
 */
class Dwarf {
  /**
   * Text returned as StringPiece points within sections in the ELF file and
   * remains live for as long as the passed-in ElfFile is live.
   */
 public:
  /** Create a DWARF parser around an ELF file. */
  Dwarf(ElfCacheBase* elfCache, const ElfFile* elf);

  /**
   * Find the file and line number information corresponding to address.
   * If `eachParameterName` is provided, the callback will be invoked once
   * for each parameter of the function.
   */
  bool findAddress(
      uintptr_t address,
      LocationInfoMode mode,
      SymbolizedFrame& frame,
      folly::Range<SymbolizedFrame*> inlineFrames = {},
      folly::FunctionRef<void(const folly::StringPiece name)>
          eachParameterName = {}) const;

 private:
  ElfCacheBase* elfCache_;
  DebugSections defaultDebugSections_;
};

#endif

} // namespace symbolizer
} // namespace folly
