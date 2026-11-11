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

#include <folly/Optional.h>
#include <folly/debugging/symbolizer/DwarfSection.h>
#include <folly/debugging/symbolizer/detail/DwarfConstants.h>

#if FOLLY_HAVE_DWARF && FOLLY_HAVE_ELF

namespace folly {
namespace symbolizer {

DwarfLineNumberVM::DwarfLineNumberVM(
    std::span<const uint8_t> data,
    folly::StringPiece compilationDirectory,
    const DebugSections& debugSections)
    : compilationDirectory_(compilationDirectory),
      debugSections_(debugSections) {
  DwarfSection section(data);
  auto chunk = section.next();
  if (chunk.hasError() || !chunk->hasValue()) {
    reset();
    return;
  }
  data_ = **chunk;
  is64Bit_ = section.is64Bit();
  initializationSuccess_ = init();
}

void DwarfLineNumberVM::reset() {
  address_ = 0;
  file_ = 1;
  line_ = 1;
  column_ = 0;
  isStmt_ = defaultIsStmt_;
  basicBlock_ = false;
  endSequence_ = false;
  prologueEnd_ = false;
  epilogueBegin_ = false;
  isa_ = 0;
  discriminator_ = 0;
}

namespace {

struct LineNumberAttribute {
  uint64_t contentTypeCode;
  uint64_t formCode;
  std::variant<uint64_t, folly::StringPiece, std::span<const uint8_t>>
      attrValue;
};

folly::Optional<LineNumberAttribute> readLineNumberAttribute(
    bool is64Bit,
    std::span<const uint8_t>& format,
    std::span<const uint8_t>& entries,
    std::span<const uint8_t> debugStr,
    std::span<const uint8_t> debugLineStr) {
  uint64_t contentTypeCode = readULEB(format);
  uint64_t formCode = readULEB(format);
  std::variant<uint64_t, folly::StringPiece, std::span<const uint8_t>>
      attrValue;

  switch (contentTypeCode) {
    case DW_LNCT_path: {
      switch (formCode) {
        case DW_FORM_string:
          attrValue = readNullTerminated(entries);
          break;
        case DW_FORM_line_strp: {
          auto off = readOffset(entries, is64Bit);
          attrValue = getStringFromStringSection(debugLineStr, off);
        } break;
        case DW_FORM_strp:
          attrValue = getStringFromStringSection(
              debugStr, readOffset(entries, is64Bit));
          break;
        case DW_FORM_strp_sup:
          FOLLY_SAFE_DFATAL("Unexpected DW_FORM_strp_sup");
          return folly::none;
        default:
          FOLLY_SAFE_DFATAL("Unexpected form for DW_LNCT_path: ", formCode);
          return folly::none;
      }
    } break;

    case DW_LNCT_directory_index: {
      switch (formCode) {
        case DW_FORM_data1:
          attrValue = read<uint8_t>(entries);
          break;
        case DW_FORM_data2:
          attrValue = read<uint16_t>(entries);
          break;
        case DW_FORM_udata:
          attrValue = readULEB(entries);
          break;
        default:
          FOLLY_SAFE_DFATAL(
              "Unexpected form for DW_LNCT_directory_index: ", formCode);
          return folly::none;
      }
    } break;

    case DW_LNCT_timestamp: {
      switch (formCode) {
        case DW_FORM_udata:
          attrValue = readULEB(entries);
          break;
        case DW_FORM_data4:
          attrValue = read<uint32_t>(entries);
          break;
        case DW_FORM_data8:
          attrValue = read<uint64_t>(entries);
          break;
        case DW_FORM_block:
          attrValue = readBytes(entries, readULEB(entries));
          break;
        default:
          FOLLY_SAFE_DFATAL(
              "Unexpected form for DW_LNCT_timestamp: ", formCode);
          return folly::none;
      }
    } break;

    case DW_LNCT_size: {
      switch (formCode) {
        case DW_FORM_udata:
          attrValue = readULEB(entries);
          break;
        case DW_FORM_data1:
          attrValue = read<uint8_t>(entries);
          break;
        case DW_FORM_data2:
          attrValue = read<uint16_t>(entries);
          break;
        case DW_FORM_data4:
          attrValue = read<uint32_t>(entries);
          break;
        case DW_FORM_data8:
          attrValue = read<uint64_t>(entries);
          break;
        default:
          FOLLY_SAFE_DFATAL("Unexpected form for DW_LNCT_size: ", formCode);
          return folly::none;
      }
    } break;

    case DW_LNCT_MD5: {
      switch (formCode) {
        case DW_FORM_data16:
          attrValue = readBytes(entries, 16);
          break;
        default:
          FOLLY_SAFE_DFATAL("Unexpected form for DW_LNCT_MD5: ", formCode);
          return folly::none;
      }
    } break;

    default:
      // TODO: skip over vendor data as specified by the form instead.
      FOLLY_SAFE_DFATAL(
          "Unexpected vendor content type code: ", contentTypeCode);
      return folly::none;
  }
  return LineNumberAttribute{
      .contentTypeCode = contentTypeCode,
      .formCode = formCode,
      .attrValue = attrValue,
  };
}

} // namespace

bool DwarfLineNumberVM::init() {
  version_ = read<uint16_t>(data_);
  if (version_ < 2 || version_ > 5) {
    FOLLY_SAFE_DFATAL("invalid version in line number VM: ", version_);
    return false;
  }
  if (version_ == 5) {
    auto addressSize = read<uint8_t>(data_);
    if (addressSize != sizeof(uintptr_t)) {
      FOLLY_SAFE_DFATAL(
          "Unexpected Line Number Table address_size: ", addressSize);
      return false;
    }
    auto segment_selector_size = read<uint8_t>(data_);
    if (segment_selector_size != 0) {
      FOLLY_SAFE_DFATAL("Segments not supported");
      return false;
    }
  }
  uint64_t headerLength = readOffset(data_, is64Bit_);
  if (headerLength > data_.size()) {
    FOLLY_SAFE_DFATAL(
        "invalid line number VM header length: headerLength: ",
        headerLength,
        " data_.size(): ",
        data_.size());
    return false;
  }

  auto header = data_.first(headerLength);
  data_ = data_.subspan(headerLength);

  minLength_ = read<uint8_t>(header);
  if (version_ >= 4) { // Version 2 and 3 records don't have this
    uint8_t maxOpsPerInstruction = read<uint8_t>(header);
    if (maxOpsPerInstruction != 1) {
      FOLLY_SAFE_DFATAL("VLIW not supported");
      return false;
    }
  }
  defaultIsStmt_ = read<uint8_t>(header);
  lineBase_ = read<int8_t>(header); // yes, signed
  lineRange_ = read<uint8_t>(header);
  opcodeBase_ = read<uint8_t>(header);
  // The standard opcode lengths table occupies opcodeBase_ - 1 bytes, which
  // must be present in the header.
  if (opcodeBase_ == 0 || opcodeBase_ - 1 > header.size()) {
    FOLLY_SAFE_DFATAL("invalid opcode base");
    return false;
  }
  standardOpcodeLengths_ = header.data();
  header = header.subspan(opcodeBase_ - 1);

  if (version_ <= 4) {
    // We don't want to use heap, so we don't keep an unbounded amount of state.
    // We'll just skip over include directories and file names here, and
    // we'll loop again when we actually need to retrieve one.
    folly::StringPiece sp;
    const uint8_t* tmp = header.data();
    v4_.includeDirectoryCount = 0;
    while (!(sp = readNullTerminated(header)).empty()) {
      ++v4_.includeDirectoryCount;
    }
    v4_.includeDirectories = {tmp, header.data()};

    tmp = header.data();
    FileName fn;
    v4_.fileNameCount = 0;
    while (readFileName(header, fn)) {
      ++v4_.fileNameCount;
    }
    v4_.fileNames = {tmp, header.data()};
  } else if (version_ == 5) {
    v5_.directoryEntryFormatCount = read<uint8_t>(header);
    const uint8_t* tmp = header.data();
    for (uint8_t i = 0; i < v5_.directoryEntryFormatCount; i++) {
      // A sequence of directory entry format descriptions. Each description
      // consists of a pair of ULEB128 values:
      readULEB(header); // A content type code
      readULEB(header); // A form code using the attribute form codes
    }
    v5_.directoryEntryFormat = {tmp, header.data()};
    v5_.directoriesCount = readULEB(header);
    tmp = header.data();
    for (uint64_t i = 0; i < v5_.directoriesCount; i++) {
      auto format = v5_.directoryEntryFormat;
      for (uint8_t f = 0; f < v5_.directoryEntryFormatCount; f++) {
        if (!readLineNumberAttribute(
                is64Bit_,
                format,
                header,
                debugSections_.debugStr,
                debugSections_.debugLineStr)) {
          return false;
        }
      }
    }
    v5_.directories = {tmp, header.data()};

    v5_.fileNameEntryFormatCount = read<uint8_t>(header);
    tmp = header.data();
    for (uint8_t i = 0; i < v5_.fileNameEntryFormatCount; i++) {
      // A sequence of file entry format descriptions. Each description
      // consists of a pair of ULEB128 values:
      readULEB(header); // A content type code
      readULEB(header); // A form code using the attribute form codes
    }
    v5_.fileNameEntryFormat = {tmp, header.data()};
    v5_.fileNamesCount = readULEB(header);
    tmp = header.data();
    for (uint64_t i = 0; i < v5_.fileNamesCount; i++) {
      auto format = v5_.fileNameEntryFormat;
      for (uint8_t f = 0; f < v5_.fileNameEntryFormatCount; f++) {
        if (!readLineNumberAttribute(
                is64Bit_,
                format,
                header,
                debugSections_.debugStr,
                debugSections_.debugLineStr)) {
          return false;
        }
      }
    }
    v5_.fileNames = {tmp, header.data()};
  }
  return true;
}

bool DwarfLineNumberVM::next(std::span<const uint8_t>& program) {
  DwarfLineNumberVM::StepResult ret;
  do {
    ret = step(program);
  } while (ret == CONTINUE);

  return (ret == COMMIT);
}

DwarfLineNumberVM::FileName DwarfLineNumberVM::getFileName(
    uint64_t index) const {
  FileName fn;
  if (!initializationSuccess_) {
    return fn;
  }
  if (version_ <= 4) {
    if (index == 0) {
      FOLLY_SAFE_DFATAL("invalid file index 0");
      return fn;
    }
    if (index <= v4_.fileNameCount) {
      auto fileNames = v4_.fileNames;
      for (; index; --index) {
        if (!readFileName(fileNames, fn)) {
          abort();
        }
      }
      return fn;
    }

    index -= v4_.fileNameCount;

    auto program = data_;
    for (; index; --index) {
      if (!nextDefineFile(program, fn)) {
        FOLLY_SAFE_DFATAL("invalid file index: ", index);
        return fn;
      }
    }

    return fn;
  } else {
    if (index >= v5_.fileNamesCount) {
      FOLLY_SAFE_DFATAL(
          "invalid file index: ",
          index,
          " v5_.fileNamesCount: ",
          v5_.fileNamesCount);
      return fn;
    }

    auto fileNames = v5_.fileNames;
    for (uint64_t i = 0; i < v5_.fileNamesCount; i++) {
      auto format = v5_.fileNameEntryFormat;
      for (uint8_t f = 0; f < v5_.fileNameEntryFormatCount; f++) {
        auto attr = readLineNumberAttribute(
            is64Bit_,
            format,
            fileNames,
            debugSections_.debugStr,
            debugSections_.debugLineStr);
        if (!attr) {
          return fn;
        }
        if (i == index) {
          switch (attr->contentTypeCode) {
            case DW_LNCT_path:
              fn.relativeName = std::get<folly::StringPiece>(attr->attrValue);
              break;
            case DW_LNCT_directory_index:
              fn.directoryIndex = std::get<uint64_t>(attr->attrValue);
              break;
          }
        }
      }
    }
    return fn;
  }
}

folly::StringPiece DwarfLineNumberVM::getIncludeDirectory(
    uint64_t index) const {
  if (version_ <= 4) {
    if (index == 0) {
      // In DWARF <= 4 the current directory is not represented in the
      // directories field and a directory index of 0 implicitly referred to
      // that directory as found in the DW_AT_comp_dir attribute of the
      // compilation unit debugging information entry.
      return {};
    }

    if (index > v4_.includeDirectoryCount) {
      FOLLY_SAFE_DFATAL(
          "invalid include directory: index: ",
          index,
          " v4_.includeDirectoryCount: ",
          v4_.includeDirectoryCount);
      return {};
    }

    auto includeDirectories = v4_.includeDirectories;
    folly::StringPiece dir;
    for (; index; --index) {
      dir = readNullTerminated(includeDirectories);
      if (dir.empty()) {
        FOLLY_SAFE_DFATAL(
            "Unexpected empty null-terminated directory name: index: ", index);
        return {};
      }
    }

    return dir;
  } else {
    if (index >= v5_.directoriesCount) {
      FOLLY_SAFE_DFATAL(
          "invalid file index: index: ",
          index,
          " v5_.directoriesCount: ",
          v5_.directoriesCount);
      return {};
    }
    auto directories = v5_.directories;
    for (uint64_t i = 0; i < v5_.directoriesCount; i++) {
      auto format = v5_.directoryEntryFormat;
      for (uint8_t f = 0; f < v5_.directoryEntryFormatCount; f++) {
        auto attr = readLineNumberAttribute(
            is64Bit_,
            format,
            directories,
            debugSections_.debugStr,
            debugSections_.debugLineStr);
        if (!attr) {
          return {};
        }
        if (i == index && attr->contentTypeCode == DW_LNCT_path) {
          return std::get<folly::StringPiece>(attr->attrValue);
        }
      }
    }
    // This could only happen if DWARF5's directory_entry_format doesn't contain
    // a DW_LNCT_path. Highly unlikely, but we shouldn't crash.
    return folly::StringPiece("<directory not found>");
  }
}

bool DwarfLineNumberVM::readFileName(
    std::span<const uint8_t>& program, FileName& fn) {
  fn.relativeName = readNullTerminated(program);
  if (fn.relativeName.empty()) {
    return false;
  }
  fn.directoryIndex = readULEB(program);
  // Skip over file size and last modified time
  readULEB(program);
  readULEB(program);
  return true;
}

bool DwarfLineNumberVM::nextDefineFile(
    std::span<const uint8_t>& program, FileName& fn) const {
  while (!program.empty()) {
    auto opcode = read<uint8_t>(program);

    if (opcode >= opcodeBase_) { // special opcode
      continue;
    }

    if (opcode != 0) { // standard opcode
      // Skip, slurp the appropriate number of LEB arguments
      uint8_t argCount = standardOpcodeLengths_[opcode - 1];
      while (argCount--) {
        readULEB(program);
      }
      continue;
    }

    // Extended opcode
    auto length = readULEB(program);
    // the opcode itself should be included in the length, so length >= 1
    if (length == 0) {
      FOLLY_SAFE_DFATAL("unexpected extended opcode length = 0");
      return false;
    }
    read<uint8_t>(program); // extended opcode
    --length;

    if (opcode == DW_LNE_define_file) {
      if (version_ == 5) {
        FOLLY_SAFE_DFATAL("DW_LNE_define_file deprecated in DWARF5");
        return false;
      }
      if (!readFileName(program, fn)) {
        FOLLY_SAFE_DFATAL("invalid empty file in DW_LNE_define_file");
        return false;
      }
      return true;
    }

    program = program.subspan(length);
    continue;
  }

  return false;
}

DwarfLineNumberVM::StepResult DwarfLineNumberVM::step(
    std::span<const uint8_t>& program) {
  auto opcode = read<uint8_t>(program);

  if (opcode >= opcodeBase_) { // special opcode
    uint8_t adjustedOpcode = opcode - opcodeBase_;
    uint8_t opAdvance = adjustedOpcode / lineRange_;

    address_ += minLength_ * opAdvance;
    line_ += lineBase_ + adjustedOpcode % lineRange_;

    basicBlock_ = false;
    prologueEnd_ = false;
    epilogueBegin_ = false;
    discriminator_ = 0;
    return COMMIT;
  }

  if (opcode != 0) { // standard opcode
    // Only interpret opcodes that are recognized by the version we're parsing;
    // the others are vendor extensions and we should ignore them.
    switch (opcode) {
      case DW_LNS_copy:
        basicBlock_ = false;
        prologueEnd_ = false;
        epilogueBegin_ = false;
        discriminator_ = 0;
        return COMMIT;
      case DW_LNS_advance_pc:
        address_ += minLength_ * readULEB(program);
        return CONTINUE;
      case DW_LNS_advance_line:
        line_ += readSLEB(program);
        return CONTINUE;
      case DW_LNS_set_file:
        file_ = readULEB(program);
        return CONTINUE;
      case DW_LNS_set_column:
        column_ = readULEB(program);
        return CONTINUE;
      case DW_LNS_negate_stmt:
        isStmt_ = !isStmt_;
        return CONTINUE;
      case DW_LNS_set_basic_block:
        basicBlock_ = true;
        return CONTINUE;
      case DW_LNS_const_add_pc:
        address_ += minLength_ * ((255 - opcodeBase_) / lineRange_);
        return CONTINUE;
      case DW_LNS_fixed_advance_pc:
        address_ += read<uint16_t>(program);
        return CONTINUE;
      case DW_LNS_set_prologue_end:
        if (version_ == 2) {
          break; // not supported in version 2
        }
        prologueEnd_ = true;
        return CONTINUE;
      case DW_LNS_set_epilogue_begin:
        if (version_ == 2) {
          break; // not supported in version 2
        }
        epilogueBegin_ = true;
        return CONTINUE;
      case DW_LNS_set_isa:
        if (version_ == 2) {
          break; // not supported in version 2
        }
        isa_ = readULEB(program);
        return CONTINUE;
    }

    // Unrecognized standard opcode, slurp the appropriate number of LEB
    // arguments.
    uint8_t argCount = standardOpcodeLengths_[opcode - 1];
    while (argCount--) {
      readULEB(program);
    }
    return CONTINUE;
  }

  // Extended opcode
  auto length = readULEB(program);
  // the opcode itself should be included in the length, so length >= 1
  if (length == 0) {
    FOLLY_SAFE_DFATAL("unexpected extended opcode length = 0");
    return END;
  }
  auto extendedOpcode = read<uint8_t>(program);
  --length;

  switch (extendedOpcode) {
    case DW_LNE_end_sequence:
      return END;
    case DW_LNE_set_address:
      address_ = read<uintptr_t>(program);
      return CONTINUE;
    case DW_LNE_define_file:
      if (version_ == 5) {
        FOLLY_SAFE_DFATAL("DW_LNE_define_file deprecated in DWARF5");
        return END;
      }
      // We can't process DW_LNE_define_file here, as it would require us to
      // use unbounded amounts of state (ie. use the heap).  We'll do a second
      // pass (using nextDefineFile()) if necessary.
      break;
#if !defined(__FreeBSD__)
    case DW_LNE_set_discriminator:
      discriminator_ = readULEB(program);
      return CONTINUE;
#endif
  }

  // Unrecognized extended opcode
  program = program.subspan(length);
  return CONTINUE;
}

Path DwarfLineNumberVM::getFullFileName(uint64_t index) const {
  auto fn = getFileName(index);
  return Path(
      // DWARF <= 4: the current dir is not represented in the CU's Line Number
      // Program Header and relies on the CU's DW_AT_comp_dir.
      // DWARF 5: the current directory is explicitly present.
      version_ == 5 ? "" : compilationDirectory_,
      getIncludeDirectory(fn.directoryIndex),
      fn.relativeName);
}

bool DwarfLineNumberVM::findAddress(
    uintptr_t target, Path& file, uint64_t& line) {
  if (!initializationSuccess_) {
    return false;
  }
  auto program = data_;

  // Within each sequence of instructions, the address may only increase.
  // Unfortunately, within the same compilation unit, sequences may appear
  // in any order.  So any sequence is a candidate if it starts at an address
  // <= the target address, and we know we've found the target address if
  // a candidate crosses the target address.
  enum State {
    START,
    LOW_SEQ, // candidate
    HIGH_SEQ
  };
  State state = START;
  reset();

  uint64_t prevFile = 0;
  uint64_t prevLine = 0;
  while (!program.empty()) {
    bool seqEnd = !next(program);

    if (state == START) {
      if (!seqEnd) {
        state = address_ <= target ? LOW_SEQ : HIGH_SEQ;
      }
    }

    if (state == LOW_SEQ) {
      if (address_ > target) {
        // Found it!  Note that ">" is indeed correct (not ">="), as each
        // sequence is guaranteed to have one entry past-the-end (emitted by
        // DW_LNE_end_sequence)

        // NOTE: In DWARF <= 4 the file register is non-zero.
        //   See DWARF 4: 6.2.4 The Line Number Program Header
        //   "The line number program assigns numbers to each of the file
        //   entries in order, beginning with 1, and uses those numbers instead
        //   of file names in the file register."
        // DWARF 5 has a different include directory/file header and 0 is valid.
        if (version_ <= 4 && prevFile == 0) {
          return false;
        }
        file = getFullFileName(prevFile);
        line = prevLine;
        return true;
      }
      prevFile = file_;
      prevLine = line_;
    }

    if (seqEnd) {
      state = START;
      reset();
    }
  }

  return false;
}

} // namespace symbolizer
} // namespace folly

#endif // FOLLY_HAVE_DWARF
