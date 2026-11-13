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

#pragma once

#include <folly/Range.h>
#include <folly/debugging/symbolizer/DwarfUtil.h>
#include <folly/debugging/symbolizer/SymbolizedFrame.h>

namespace folly {
namespace symbolizer {

#if FOLLY_HAVE_DWARF && FOLLY_HAVE_ELF

class DwarfLineNumberVM {
 public:
  DwarfLineNumberVM(
      std::span<const uint8_t> data,
      folly::StringPiece compilationDirectory,
      const DebugSections& debugSections);

  bool findAddress(uintptr_t target, Path& file, uint64_t& line);

  /** Gets full file name at given index including directory. */
  DwarfParseResult<Path> getFullFileName(uint64_t index) const;

 private:
  DwarfParseResult<bool> init();
  void reset();

  /** Execute until we commit one new row to the line number matrix */
  DwarfParseResult<bool> next(DwarfCursor& program);
  enum StepResult {
    CONTINUE, // Continue feeding opcodes
    COMMIT, // Commit new <address, file, line> tuple
    END, // End of sequence
  };
  /** Execute one opcode */
  DwarfParseResult<StepResult> step(DwarfCursor& program);

  struct FileName {
    folly::StringPiece relativeName;
    // 0 = current compilation directory
    // otherwise, 1-based index in the list of include directories
    uint64_t directoryIndex = 0;
  };

  /** Read one FileName object, advance sp */
  static DwarfParseResult<folly::Optional<FileName>> readFileName(
      DwarfCursor& program);

  /**
   * Get file name at given index; may be in the initial table
   * (fileNames_) or defined using DW_LNE_define_file (and we reexecute
   * enough of the program to find it, if so)
   */
  DwarfParseResult<FileName> getFileName(uint64_t index) const;

  /** Get include directory at given index */
  DwarfParseResult<folly::StringPiece> getIncludeDirectory(
      uint64_t index) const;

  /**
   * Execute opcodes until finding a DW_LNE_define_file and return true;
   * return file at the end.
   */
  DwarfParseResult<folly::Optional<FileName>> nextDefineFile(
      DwarfCursor& program) const;

  // Initialization
  bool initializationSuccess_ = false;
  bool is64Bit_ = false;
  std::span<const uint8_t> data_;
  folly::StringPiece compilationDirectory_;
  const DebugSections& debugSections_;

  // Header
  uint16_t version_ = 0;
  uint8_t minLength_ = 0;
  bool defaultIsStmt_ = false;
  int8_t lineBase_ = 0;
  uint8_t lineRange_ = 0;
  uint8_t opcodeBase_ = 0;
  std::span<const uint8_t> standardOpcodeLengths_;

  // 6.2.4 The Line Number Program Header.
  struct {
    size_t includeDirectoryCount;
    std::span<const uint8_t> includeDirectories;
    size_t fileNameCount;
    std::span<const uint8_t> fileNames;
  } v4_;

  struct {
    uint8_t directoryEntryFormatCount;
    std::span<const uint8_t> directoryEntryFormat;
    uint64_t directoriesCount;
    std::span<const uint8_t> directories;

    uint8_t fileNameEntryFormatCount;
    std::span<const uint8_t> fileNameEntryFormat;
    uint64_t fileNamesCount;
    std::span<const uint8_t> fileNames;
  } v5_;

  // State machine registers
  uint64_t address_;
  uint64_t file_;
  uint64_t line_;
  uint64_t column_;
  bool isStmt_;
  bool basicBlock_;
  bool endSequence_;
  bool prologueEnd_;
  bool epilogueBegin_;
  uint64_t isa_;
  uint64_t discriminator_;
};

#endif

} // namespace symbolizer
} // namespace folly
