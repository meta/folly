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

#include <folly/debugging/symbolizer/DwarfSection.h>

#include <limits>

#include <folly/debugging/symbolizer/detail/DwarfConstants.h>

#if FOLLY_HAVE_DWARF && FOLLY_HAVE_ELF

namespace folly {
namespace symbolizer {

DwarfSection::DwarfSection(std::span<const uint8_t> data)
    : is64Bit_(false), data_(data) {}

DwarfParseResult<folly::Optional<std::span<const uint8_t>>>
DwarfSection::next() {
  if (data_.empty()) {
    return folly::Optional<std::span<const uint8_t>>{};
  }

  DwarfCursor cursor(data_, offset_);
  // Initial length is a uint32_t value for a 32-bit section, and
  // a 96-bit value (0xffffffff followed by the 64-bit length) for a 64-bit
  // section.
  auto initialLength = cursor.read<uint32_t>();
  if (initialLength.hasError()) {
    return makeUnexpected(initialLength.error());
  }
  const bool is64Bit = *initialLength == std::numeric_limits<uint32_t>::max();
  uint64_t length = *initialLength;
  if (is64Bit) {
    auto extendedLength = cursor.read<uint64_t>();
    if (extendedLength.hasError()) {
      return makeUnexpected(extendedLength.error());
    }
    length = *extendedLength;
  } else if (*initialLength >= kDwarfReservedInitialLengthStart) {
    return makeUnexpected(
        DwarfParseError{DwarfParseErrorCode::invalid_value, offset_});
  }
  if (length > cursor.remaining().size()) {
    return makeUnexpected(
        DwarfParseError{DwarfParseErrorCode::truncated, cursor.offset()});
  }

  const size_t headerSize = cursor.offset() - offset_;
  const size_t chunkSize = static_cast<size_t>(length);
  auto chunk = data_.subspan(headerSize, chunkSize);
  const size_t consumedSize = headerSize + chunkSize;
  data_ = data_.subspan(consumedSize);
  offset_ += consumedSize;
  is64Bit_ = is64Bit;
  return folly::Optional<std::span<const uint8_t>>{chunk};
}

} // namespace symbolizer
} // namespace folly

#endif // FOLLY_HAVE_DWARF
