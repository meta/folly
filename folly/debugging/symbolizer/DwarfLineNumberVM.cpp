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

#include <limits>

#include <folly/Optional.h>
#include <folly/debugging/symbolizer/DwarfSection.h>
#include <folly/debugging/symbolizer/detail/DwarfConstants.h>
#include <folly/lang/CheckedMath.h>

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
  auto initialized = init();
  initializationSuccess_ = initialized.hasValue() && *initialized;
  reset();
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
  DwarfAttributeValue attrValue;
};

template <typename T>
DwarfParseResult<T> parseError(
    DwarfParseErrorCode code, size_t offset) noexcept {
  return makeUnexpected(DwarfParseError{code, offset});
}

template <typename Size = size_t>
DwarfParseResult<Size> toSize(uint64_t value, size_t errorOffset) noexcept {
  if constexpr (sizeof(Size) < sizeof(uint64_t)) {
    if (value > std::numeric_limits<Size>::max()) {
      return parseError<Size>(
          DwarfParseErrorCode::integer_overflow, errorOffset);
    }
  }
  return static_cast<Size>(value);
}

DwarfParseResult<folly::StringPiece> readString(
    std::span<const uint8_t> section, uint64_t offset, size_t errorOffset) {
  auto size = toSize(offset, errorOffset);
  if (size.hasError()) {
    return makeUnexpected(size.error());
  }
  if (*size >= section.size()) {
    return parseError<folly::StringPiece>(
        DwarfParseErrorCode::invalid_offset, errorOffset);
  }
  DwarfCursor cursor(section.subspan(*size), *size);
  auto value = cursor.readCString();
  if (value.hasError()) {
    return makeUnexpected(value.error());
  }
  return folly::StringPiece(value->data(), value->size());
}

DwarfParseResult<DwarfAttributeValue> readLineNumberValue(
    bool is64Bit,
    uint64_t formCode,
    DwarfCursor& entries,
    std::span<const uint8_t> debugStr,
    std::span<const uint8_t> debugLineStr) {
  const auto valueOffset = entries.offset();
  switch (formCode) {
    case DW_FORM_string: {
      auto value = entries.readCString();
      if (value.hasError()) {
        return makeUnexpected(value.error());
      }
      return DwarfAttributeValue{
          folly::StringPiece(value->data(), value->size())};
    }
    case DW_FORM_line_strp:
    case DW_FORM_strp: {
      auto offset = entries.readOffset(is64Bit);
      if (offset.hasError()) {
        return makeUnexpected(offset.error());
      }
      auto value = readString(
          formCode == DW_FORM_line_strp ? debugLineStr : debugStr,
          *offset,
          valueOffset);
      if (value.hasError()) {
        return makeUnexpected(value.error());
      }
      return DwarfAttributeValue{*value};
    }
    case DW_FORM_data1: {
      auto value = entries.read<uint8_t>();
      if (value.hasError()) {
        return makeUnexpected(value.error());
      }
      return DwarfAttributeValue{static_cast<uint64_t>(*value)};
    }
    case DW_FORM_data2: {
      auto value = entries.read<uint16_t>();
      if (value.hasError()) {
        return makeUnexpected(value.error());
      }
      return DwarfAttributeValue{static_cast<uint64_t>(*value)};
    }
    case DW_FORM_data4: {
      auto value = entries.read<uint32_t>();
      if (value.hasError()) {
        return makeUnexpected(value.error());
      }
      return DwarfAttributeValue{static_cast<uint64_t>(*value)};
    }
    case DW_FORM_data8: {
      auto value = entries.read<uint64_t>();
      if (value.hasError()) {
        return makeUnexpected(value.error());
      }
      return DwarfAttributeValue{*value};
    }
    case DW_FORM_udata: {
      auto value = entries.readULEB();
      if (value.hasError()) {
        return makeUnexpected(value.error());
      }
      return DwarfAttributeValue{*value};
    }
    case DW_FORM_block: {
      auto length = entries.readULEB();
      if (length.hasError()) {
        return makeUnexpected(length.error());
      }
      auto size = toSize(*length, valueOffset);
      if (size.hasError()) {
        return makeUnexpected(size.error());
      }
      auto value = entries.readBytes(*size);
      if (value.hasError()) {
        return makeUnexpected(value.error());
      }
      return DwarfAttributeValue{*value};
    }
    case DW_FORM_data16: {
      auto value = entries.readBytes(16);
      if (value.hasError()) {
        return makeUnexpected(value.error());
      }
      return DwarfAttributeValue{*value};
    }
    default:
      return parseError<DwarfAttributeValue>(
          DwarfParseErrorCode::invalid_value, valueOffset);
  }
}

DwarfParseResult<LineNumberAttribute> readLineNumberAttribute(
    bool is64Bit,
    DwarfCursor& format,
    DwarfCursor& entries,
    std::span<const uint8_t> debugStr,
    std::span<const uint8_t> debugLineStr) {
  auto contentTypeCode = format.readULEB();
  if (contentTypeCode.hasError()) {
    return makeUnexpected(contentTypeCode.error());
  }
  auto formCode = format.readULEB();
  if (formCode.hasError()) {
    return makeUnexpected(formCode.error());
  }
  auto attrValue =
      readLineNumberValue(is64Bit, *formCode, entries, debugStr, debugLineStr);
  if (attrValue.hasError()) {
    return makeUnexpected(attrValue.error());
  }
  return LineNumberAttribute{
      .contentTypeCode = *contentTypeCode,
      .formCode = *formCode,
      .attrValue = *attrValue,
  };
}

std::span<const uint8_t> consumedBytes(
    std::span<const uint8_t> bytes, const DwarfCursor& cursor) {
  return bytes.first(bytes.size() - cursor.remaining().size());
}

DwarfParseResult<uint64_t> addUnsigned(
    uint64_t value, uint64_t increment, size_t errorOffset) {
  uint64_t result;
  if (!folly::checked_add(&result, value, increment)) {
    return parseError<uint64_t>(
        DwarfParseErrorCode::integer_overflow, errorOffset);
  }
  return result;
}

DwarfParseResult<uint64_t> multiplyAndAdd(
    uint64_t value,
    uint64_t multiplier,
    uint64_t increment,
    size_t errorOffset) {
  uint64_t result;
  if (!folly::checked_muladd(&result, multiplier, increment, value)) {
    return parseError<uint64_t>(
        DwarfParseErrorCode::integer_overflow, errorOffset);
  }
  return result;
}

DwarfParseResult<uint64_t> addSigned(
    uint64_t value, int64_t increment, size_t errorOffset) {
  if (increment >= 0) {
    return addUnsigned(value, static_cast<uint64_t>(increment), errorOffset);
  }
  const auto magnitude = static_cast<uint64_t>(-(increment + 1)) + 1;
  if (magnitude > value) {
    return parseError<uint64_t>(
        DwarfParseErrorCode::integer_overflow, errorOffset);
  }
  return value - magnitude;
}

} // namespace

DwarfParseResult<bool> DwarfLineNumberVM::init() {
  DwarfCursor data(data_);
  auto version = data.read<uint16_t>();
  if (version.hasError()) {
    return makeUnexpected(version.error());
  }
  version_ = *version;
  if (version_ < 2 || version_ > 5) {
    return parseError<bool>(DwarfParseErrorCode::invalid_value, 0);
  }
  if (version_ == 5) {
    auto addressSize = data.read<uint8_t>();
    if (addressSize.hasError()) {
      return makeUnexpected(addressSize.error());
    }
    if (*addressSize != sizeof(uintptr_t)) {
      return parseError<bool>(
          DwarfParseErrorCode::invalid_value, data.offset() - 1);
    }
    auto segmentSelectorSize = data.read<uint8_t>();
    if (segmentSelectorSize.hasError()) {
      return makeUnexpected(segmentSelectorSize.error());
    }
    if (*segmentSelectorSize != 0) {
      return parseError<bool>(
          DwarfParseErrorCode::invalid_value, data.offset() - 1);
    }
  }
  auto headerLength = data.readOffset(is64Bit_);
  if (headerLength.hasError()) {
    return makeUnexpected(headerLength.error());
  }
  auto headerSize = toSize(*headerLength, data.offset());
  if (headerSize.hasError()) {
    return makeUnexpected(headerSize.error());
  }
  const auto headerOffset = data.offset();
  auto headerBytes = data.readBytes(*headerSize);
  if (headerBytes.hasError()) {
    return makeUnexpected(headerBytes.error());
  }
  data_ = data.remaining();
  DwarfCursor header(*headerBytes, headerOffset);

  auto minLength = header.read<uint8_t>();
  if (minLength.hasError()) {
    return makeUnexpected(minLength.error());
  }
  minLength_ = *minLength;
  if (minLength_ == 0) {
    return parseError<bool>(
        DwarfParseErrorCode::invalid_value, header.offset() - 1);
  }
  if (version_ >= 4) { // Version 2 and 3 records don't have this
    auto maxOpsPerInstruction = header.read<uint8_t>();
    if (maxOpsPerInstruction.hasError()) {
      return makeUnexpected(maxOpsPerInstruction.error());
    }
    if (*maxOpsPerInstruction != 1) {
      return parseError<bool>(
          DwarfParseErrorCode::invalid_value, header.offset() - 1);
    }
  }
  auto defaultIsStmt = header.read<uint8_t>();
  auto lineBase = header.read<int8_t>();
  auto lineRange = header.read<uint8_t>();
  auto opcodeBase = header.read<uint8_t>();
  if (defaultIsStmt.hasError()) {
    return makeUnexpected(defaultIsStmt.error());
  }
  if (lineBase.hasError()) {
    return makeUnexpected(lineBase.error());
  }
  if (lineRange.hasError()) {
    return makeUnexpected(lineRange.error());
  }
  if (opcodeBase.hasError()) {
    return makeUnexpected(opcodeBase.error());
  }
  defaultIsStmt_ = *defaultIsStmt != 0;
  lineBase_ = *lineBase;
  lineRange_ = *lineRange;
  opcodeBase_ = *opcodeBase;
  if (lineRange_ == 0 || opcodeBase_ == 0) {
    return parseError<bool>(
        DwarfParseErrorCode::invalid_value, header.offset() - 1);
  }
  auto standardOpcodeLengths = header.readBytes(opcodeBase_ - 1);
  if (standardOpcodeLengths.hasError()) {
    return makeUnexpected(standardOpcodeLengths.error());
  }
  standardOpcodeLengths_ = *standardOpcodeLengths;

  if (version_ <= 4) {
    // We don't want to use heap, so we don't keep an unbounded amount of state.
    // We'll just skip over include directories and file names here, and
    // we'll loop again when we actually need to retrieve one.
    const auto includeDirectories = header.remaining();
    v4_.includeDirectoryCount = 0;
    for (;;) {
      auto directory = header.readCString();
      if (directory.hasError()) {
        return makeUnexpected(directory.error());
      }
      if (directory->empty()) {
        break;
      }
      ++v4_.includeDirectoryCount;
    }
    v4_.includeDirectories = consumedBytes(includeDirectories, header);

    const auto fileNames = header.remaining();
    v4_.fileNameCount = 0;
    for (;;) {
      auto fileName = readFileName(header);
      if (fileName.hasError()) {
        return makeUnexpected(fileName.error());
      }
      if (!fileName->hasValue()) {
        break;
      }
      ++v4_.fileNameCount;
    }
    v4_.fileNames = consumedBytes(fileNames, header);
  } else if (version_ == 5) {
    auto directoryEntryFormatCount = header.read<uint8_t>();
    if (directoryEntryFormatCount.hasError()) {
      return makeUnexpected(directoryEntryFormatCount.error());
    }
    v5_.directoryEntryFormatCount = *directoryEntryFormatCount;
    const auto directoryEntryFormat = header.remaining();
    for (uint8_t i = 0; i < v5_.directoryEntryFormatCount; i++) {
      // A sequence of directory entry format descriptions. Each description
      // consists of a pair of ULEB128 values:
      auto contentType = header.readULEB();
      auto form = header.readULEB();
      if (contentType.hasError()) {
        return makeUnexpected(contentType.error());
      }
      if (form.hasError()) {
        return makeUnexpected(form.error());
      }
    }
    v5_.directoryEntryFormat = consumedBytes(directoryEntryFormat, header);
    auto directoriesCount = header.readULEB();
    if (directoriesCount.hasError()) {
      return makeUnexpected(directoriesCount.error());
    }
    v5_.directoriesCount = *directoriesCount;
    if (v5_.directoriesCount != 0 &&
        (v5_.directoryEntryFormatCount == 0 ||
         v5_.directoriesCount > header.remaining().size())) {
      return parseError<bool>(
          DwarfParseErrorCode::invalid_value, header.offset());
    }
    const auto directories = header.remaining();
    for (uint64_t i = 0; i < v5_.directoriesCount; i++) {
      DwarfCursor format(v5_.directoryEntryFormat);
      for (uint8_t f = 0; f < v5_.directoryEntryFormatCount; f++) {
        auto attribute = readLineNumberAttribute(
            is64Bit_,
            format,
            header,
            debugSections_.debugStr,
            debugSections_.debugLineStr);
        if (attribute.hasError()) {
          return makeUnexpected(attribute.error());
        }
      }
    }
    v5_.directories = consumedBytes(directories, header);

    auto fileNameEntryFormatCount = header.read<uint8_t>();
    if (fileNameEntryFormatCount.hasError()) {
      return makeUnexpected(fileNameEntryFormatCount.error());
    }
    v5_.fileNameEntryFormatCount = *fileNameEntryFormatCount;
    const auto fileNameEntryFormat = header.remaining();
    for (uint8_t i = 0; i < v5_.fileNameEntryFormatCount; i++) {
      // A sequence of file entry format descriptions. Each description
      // consists of a pair of ULEB128 values:
      auto contentType = header.readULEB();
      auto form = header.readULEB();
      if (contentType.hasError()) {
        return makeUnexpected(contentType.error());
      }
      if (form.hasError()) {
        return makeUnexpected(form.error());
      }
    }
    v5_.fileNameEntryFormat = consumedBytes(fileNameEntryFormat, header);
    auto fileNamesCount = header.readULEB();
    if (fileNamesCount.hasError()) {
      return makeUnexpected(fileNamesCount.error());
    }
    v5_.fileNamesCount = *fileNamesCount;
    if (v5_.fileNamesCount != 0 &&
        (v5_.fileNameEntryFormatCount == 0 ||
         v5_.fileNamesCount > header.remaining().size())) {
      return parseError<bool>(
          DwarfParseErrorCode::invalid_value, header.offset());
    }
    const auto fileNames = header.remaining();
    for (uint64_t i = 0; i < v5_.fileNamesCount; i++) {
      DwarfCursor format(v5_.fileNameEntryFormat);
      for (uint8_t f = 0; f < v5_.fileNameEntryFormatCount; f++) {
        auto attribute = readLineNumberAttribute(
            is64Bit_,
            format,
            header,
            debugSections_.debugStr,
            debugSections_.debugLineStr);
        if (attribute.hasError()) {
          return makeUnexpected(attribute.error());
        }
      }
    }
    v5_.fileNames = consumedBytes(fileNames, header);
  }
  if (!header.empty()) {
    return parseError<bool>(
        DwarfParseErrorCode::invalid_value, header.offset());
  }
  return true;
}

DwarfParseResult<bool> DwarfLineNumberVM::next(DwarfCursor& program) {
  DwarfParseResult<StepResult> ret = CONTINUE;
  do {
    ret = step(program);
    if (ret.hasError()) {
      return makeUnexpected(ret.error());
    }
  } while (*ret == CONTINUE);

  return *ret == COMMIT;
}

DwarfParseResult<DwarfLineNumberVM::FileName> DwarfLineNumberVM::getFileName(
    uint64_t index) const {
  if (!initializationSuccess_) {
    return parseError<FileName>(DwarfParseErrorCode::invalid_value, 0);
  }
  if (version_ <= 4) {
    if (index == 0) {
      return parseError<FileName>(DwarfParseErrorCode::invalid_value, 0);
    }
    if (index <= v4_.fileNameCount) {
      DwarfCursor fileNames(v4_.fileNames);
      folly::Optional<FileName> fileName;
      for (; index; --index) {
        auto nextFileName = readFileName(fileNames);
        if (nextFileName.hasError()) {
          return makeUnexpected(nextFileName.error());
        }
        if (!nextFileName->hasValue()) {
          return parseError<FileName>(
              DwarfParseErrorCode::invalid_value, fileNames.offset());
        }
        fileName = **nextFileName;
      }
      return *fileName;
    }

    index -= v4_.fileNameCount;

    DwarfCursor program(data_);
    folly::Optional<FileName> fileName;
    for (; index; --index) {
      auto nextFileName = nextDefineFile(program);
      if (nextFileName.hasError()) {
        return makeUnexpected(nextFileName.error());
      }
      if (!nextFileName->hasValue()) {
        return parseError<FileName>(
            DwarfParseErrorCode::invalid_value, program.offset());
      }
      fileName = **nextFileName;
    }

    return *fileName;
  } else {
    if (index >= v5_.fileNamesCount) {
      return parseError<FileName>(DwarfParseErrorCode::invalid_value, 0);
    }

    DwarfCursor fileNames(v5_.fileNames);
    FileName fileName;
    bool hasPath = false;
    for (uint64_t i = 0; i < v5_.fileNamesCount; i++) {
      DwarfCursor format(v5_.fileNameEntryFormat);
      for (uint8_t f = 0; f < v5_.fileNameEntryFormatCount; f++) {
        auto attr = readLineNumberAttribute(
            is64Bit_,
            format,
            fileNames,
            debugSections_.debugStr,
            debugSections_.debugLineStr);
        if (attr.hasError()) {
          return makeUnexpected(attr.error());
        }
        if (i == index) {
          switch (attr->contentTypeCode) {
            case DW_LNCT_path: {
              const auto* path =
                  std::get_if<folly::StringPiece>(&attr->attrValue);
              if (path == nullptr || path->empty()) {
                return parseError<FileName>(
                    DwarfParseErrorCode::invalid_value, fileNames.offset());
              }
              fileName.relativeName = *path;
              hasPath = true;
              break;
            }
            case DW_LNCT_directory_index: {
              const auto* directoryIndex =
                  std::get_if<uint64_t>(&attr->attrValue);
              if (directoryIndex == nullptr) {
                return parseError<FileName>(
                    DwarfParseErrorCode::invalid_value, fileNames.offset());
              }
              fileName.directoryIndex = *directoryIndex;
              break;
            }
          }
        }
      }
    }
    if (!hasPath) {
      return parseError<FileName>(DwarfParseErrorCode::invalid_value, 0);
    }
    return fileName;
  }
}

DwarfParseResult<folly::StringPiece> DwarfLineNumberVM::getIncludeDirectory(
    uint64_t index) const {
  if (version_ <= 4) {
    if (index == 0) {
      // In DWARF <= 4 the current directory is not represented in the
      // directories field and a directory index of 0 implicitly referred to
      // that directory as found in the DW_AT_comp_dir attribute of the
      // compilation unit debugging information entry.
      return folly::StringPiece{};
    }

    if (index > v4_.includeDirectoryCount) {
      return parseError<folly::StringPiece>(
          DwarfParseErrorCode::invalid_value, 0);
    }

    DwarfCursor includeDirectories(v4_.includeDirectories);
    folly::StringPiece dir;
    for (; index; --index) {
      auto nextDirectory = includeDirectories.readCString();
      if (nextDirectory.hasError()) {
        return makeUnexpected(nextDirectory.error());
      }
      if (nextDirectory->empty()) {
        return parseError<folly::StringPiece>(
            DwarfParseErrorCode::invalid_value, includeDirectories.offset());
      }
      dir = folly::StringPiece(nextDirectory->data(), nextDirectory->size());
    }

    return dir;
  } else {
    if (index >= v5_.directoriesCount) {
      return parseError<folly::StringPiece>(
          DwarfParseErrorCode::invalid_value, 0);
    }
    DwarfCursor directories(v5_.directories);
    for (uint64_t i = 0; i < v5_.directoriesCount; i++) {
      DwarfCursor format(v5_.directoryEntryFormat);
      for (uint8_t f = 0; f < v5_.directoryEntryFormatCount; f++) {
        auto attr = readLineNumberAttribute(
            is64Bit_,
            format,
            directories,
            debugSections_.debugStr,
            debugSections_.debugLineStr);
        if (attr.hasError()) {
          return makeUnexpected(attr.error());
        }
        if (i == index && attr->contentTypeCode == DW_LNCT_path) {
          const auto* path = std::get_if<folly::StringPiece>(&attr->attrValue);
          if (path == nullptr || path->empty()) {
            return parseError<folly::StringPiece>(
                DwarfParseErrorCode::invalid_value, directories.offset());
          }
          return *path;
        }
      }
    }
    return parseError<folly::StringPiece>(
        DwarfParseErrorCode::invalid_value, directories.offset());
  }
}

DwarfParseResult<folly::Optional<DwarfLineNumberVM::FileName>>
DwarfLineNumberVM::readFileName(DwarfCursor& program) {
  auto relativeName = program.readCString();
  if (relativeName.hasError()) {
    return makeUnexpected(relativeName.error());
  }
  if (relativeName->empty()) {
    return folly::Optional<FileName>{};
  }
  auto directoryIndex = program.readULEB();
  auto modifiedTime = program.readULEB();
  auto fileSize = program.readULEB();
  if (directoryIndex.hasError()) {
    return makeUnexpected(directoryIndex.error());
  }
  if (modifiedTime.hasError()) {
    return makeUnexpected(modifiedTime.error());
  }
  if (fileSize.hasError()) {
    return makeUnexpected(fileSize.error());
  }
  return folly::Optional<FileName>{FileName{
      .relativeName =
          folly::StringPiece(relativeName->data(), relativeName->size()),
      .directoryIndex = *directoryIndex,
  }};
}

DwarfParseResult<folly::Optional<DwarfLineNumberVM::FileName>>
DwarfLineNumberVM::nextDefineFile(DwarfCursor& program) const {
  while (!program.empty()) {
    auto opcode = program.read<uint8_t>();
    if (opcode.hasError()) {
      return makeUnexpected(opcode.error());
    }

    if (*opcode >= opcodeBase_) { // special opcode
      continue;
    }

    if (*opcode != 0) { // standard opcode
      // Skip, slurp the appropriate number of LEB arguments
      uint8_t argCount = standardOpcodeLengths_[*opcode - 1];
      while (argCount--) {
        auto argument = program.readULEB();
        if (argument.hasError()) {
          return makeUnexpected(argument.error());
        }
      }
      continue;
    }

    // Extended opcode
    auto length = program.readULEB();
    if (length.hasError()) {
      return makeUnexpected(length.error());
    }
    // the opcode itself should be included in the length, so length >= 1
    if (*length == 0) {
      return parseError<folly::Optional<FileName>>(
          DwarfParseErrorCode::invalid_value, program.offset());
    }
    auto payloadSize = toSize(*length, program.offset());
    if (payloadSize.hasError()) {
      return makeUnexpected(payloadSize.error());
    }
    auto payloadBytes = program.readBytes(*payloadSize);
    if (payloadBytes.hasError()) {
      return makeUnexpected(payloadBytes.error());
    }
    DwarfCursor payload(*payloadBytes);
    auto extendedOpcode = payload.read<uint8_t>();
    if (extendedOpcode.hasError()) {
      return makeUnexpected(extendedOpcode.error());
    }

    if (*extendedOpcode == DW_LNE_define_file) {
      if (version_ == 5) {
        return parseError<folly::Optional<FileName>>(
            DwarfParseErrorCode::invalid_value, program.offset());
      }
      auto fileName = readFileName(payload);
      if (fileName.hasError()) {
        return makeUnexpected(fileName.error());
      }
      if (!fileName->hasValue() || !payload.empty()) {
        return parseError<folly::Optional<FileName>>(
            DwarfParseErrorCode::invalid_value, program.offset());
      }
      return *fileName;
    }
  }

  return folly::Optional<FileName>{};
}

DwarfParseResult<DwarfLineNumberVM::StepResult> DwarfLineNumberVM::step(
    DwarfCursor& program) {
  const auto opcodeOffset = program.offset();
  auto opcode = program.read<uint8_t>();
  if (opcode.hasError()) {
    return makeUnexpected(opcode.error());
  }

  if (*opcode >= opcodeBase_) { // special opcode
    uint8_t adjustedOpcode = *opcode - opcodeBase_;
    uint8_t opAdvance = adjustedOpcode / lineRange_;

    auto address =
        multiplyAndAdd(address_, minLength_, opAdvance, opcodeOffset);
    auto line =
        addSigned(line_, lineBase_ + adjustedOpcode % lineRange_, opcodeOffset);
    if (address.hasError()) {
      return makeUnexpected(address.error());
    }
    if (line.hasError()) {
      return makeUnexpected(line.error());
    }
    address_ = *address;
    line_ = *line;

    basicBlock_ = false;
    prologueEnd_ = false;
    epilogueBegin_ = false;
    discriminator_ = 0;
    return COMMIT;
  }

  if (*opcode != 0) { // standard opcode
    // Only interpret opcodes that are recognized by the version we're parsing;
    // the others are vendor extensions and we should ignore them.
    switch (*opcode) {
      case DW_LNS_copy:
        basicBlock_ = false;
        prologueEnd_ = false;
        epilogueBegin_ = false;
        discriminator_ = 0;
        return COMMIT;
      case DW_LNS_advance_pc:
        if (auto increment = program.readULEB(); increment.hasValue()) {
          auto address =
              multiplyAndAdd(address_, minLength_, *increment, opcodeOffset);
          if (address.hasError()) {
            return makeUnexpected(address.error());
          }
          address_ = *address;
        } else {
          return makeUnexpected(increment.error());
        }
        return CONTINUE;
      case DW_LNS_advance_line: {
        auto increment = program.readSLEB();
        if (increment.hasError()) {
          return makeUnexpected(increment.error());
        }
        auto line = addSigned(line_, *increment, opcodeOffset);
        if (line.hasError()) {
          return makeUnexpected(line.error());
        }
        line_ = *line;
        return CONTINUE;
      }
      case DW_LNS_set_file: {
        auto file = program.readULEB();
        if (file.hasError()) {
          return makeUnexpected(file.error());
        }
        file_ = *file;
        return CONTINUE;
      }
      case DW_LNS_set_column: {
        auto column = program.readULEB();
        if (column.hasError()) {
          return makeUnexpected(column.error());
        }
        column_ = *column;
        return CONTINUE;
      }
      case DW_LNS_negate_stmt:
        isStmt_ = !isStmt_;
        return CONTINUE;
      case DW_LNS_set_basic_block:
        basicBlock_ = true;
        return CONTINUE;
      case DW_LNS_const_add_pc:
        if (auto address = multiplyAndAdd(
                address_,
                minLength_,
                (255 - opcodeBase_) / lineRange_,
                opcodeOffset);
            address.hasValue()) {
          address_ = *address;
        } else {
          return makeUnexpected(address.error());
        }
        return CONTINUE;
      case DW_LNS_fixed_advance_pc: {
        auto increment = program.read<uint16_t>();
        if (increment.hasError()) {
          return makeUnexpected(increment.error());
        }
        auto address = addUnsigned(address_, *increment, opcodeOffset);
        if (address.hasError()) {
          return makeUnexpected(address.error());
        }
        address_ = *address;
        return CONTINUE;
      }
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
      case DW_LNS_set_isa: {
        if (version_ == 2) {
          break; // not supported in version 2
        }
        auto isa = program.readULEB();
        if (isa.hasError()) {
          return makeUnexpected(isa.error());
        }
        isa_ = *isa;
        return CONTINUE;
      }
    }

    // Unrecognized standard opcode, slurp the appropriate number of LEB
    // arguments.
    uint8_t argCount = standardOpcodeLengths_[*opcode - 1];
    while (argCount--) {
      auto argument = program.readULEB();
      if (argument.hasError()) {
        return makeUnexpected(argument.error());
      }
    }
    return CONTINUE;
  }

  // Extended opcode
  auto length = program.readULEB();
  if (length.hasError()) {
    return makeUnexpected(length.error());
  }
  // the opcode itself should be included in the length, so length >= 1
  if (*length == 0) {
    return parseError<StepResult>(
        DwarfParseErrorCode::invalid_value, opcodeOffset);
  }
  auto payloadSize = toSize(*length, opcodeOffset);
  if (payloadSize.hasError()) {
    return makeUnexpected(payloadSize.error());
  }
  auto payloadBytes = program.readBytes(*payloadSize);
  if (payloadBytes.hasError()) {
    return makeUnexpected(payloadBytes.error());
  }
  DwarfCursor payload(*payloadBytes, opcodeOffset);
  auto extendedOpcode = payload.read<uint8_t>();
  if (extendedOpcode.hasError()) {
    return makeUnexpected(extendedOpcode.error());
  }

  switch (*extendedOpcode) {
    case DW_LNE_end_sequence:
      if (!payload.empty()) {
        return parseError<StepResult>(
            DwarfParseErrorCode::invalid_value, opcodeOffset);
      }
      return END;
    case DW_LNE_set_address: {
      auto address = payload.read<uintptr_t>();
      if (address.hasError()) {
        return makeUnexpected(address.error());
      }
      if (!payload.empty()) {
        return parseError<StepResult>(
            DwarfParseErrorCode::invalid_value, opcodeOffset);
      }
      address_ = *address;
      return CONTINUE;
    }
    case DW_LNE_define_file:
      if (version_ == 5) {
        return parseError<StepResult>(
            DwarfParseErrorCode::invalid_value, opcodeOffset);
      }
      // We can't process DW_LNE_define_file here, as it would require us to
      // use unbounded amounts of state (ie. use the heap).  We'll do a second
      // pass (using nextDefineFile()) if necessary.
      break;
#if !defined(__FreeBSD__)
    case DW_LNE_set_discriminator:
      if (auto discriminator = payload.readULEB();
          discriminator.hasValue() && payload.empty()) {
        discriminator_ = *discriminator;
      } else if (discriminator.hasError()) {
        return makeUnexpected(discriminator.error());
      } else {
        return parseError<StepResult>(
            DwarfParseErrorCode::invalid_value, opcodeOffset);
      }
      return CONTINUE;
#endif
  }

  // Unrecognized extended opcode. The payload has already been consumed.
  return CONTINUE;
}

DwarfParseResult<Path> DwarfLineNumberVM::getFullFileName(
    uint64_t index) const {
  auto fn = getFileName(index);
  if (fn.hasError()) {
    return makeUnexpected(fn.error());
  }
  auto directory = getIncludeDirectory(fn->directoryIndex);
  if (directory.hasError()) {
    return makeUnexpected(directory.error());
  }
  return Path(
      // DWARF <= 4: the current dir is not represented in the CU's Line Number
      // Program Header and relies on the CU's DW_AT_comp_dir.
      // DWARF 5: the current directory is explicitly present.
      version_ == 5 ? "" : compilationDirectory_,
      *directory,
      fn->relativeName);
}

bool DwarfLineNumberVM::findAddress(
    uintptr_t target, Path& file, uint64_t& line) {
  if (!initializationSuccess_) {
    return false;
  }
  DwarfCursor program(data_);

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
    auto committed = next(program);
    if (committed.hasError()) {
      return false;
    }
    bool seqEnd = !*committed;

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
        auto fullFileName = getFullFileName(prevFile);
        if (fullFileName.hasError()) {
          return false;
        }
        file = *fullFileName;
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
