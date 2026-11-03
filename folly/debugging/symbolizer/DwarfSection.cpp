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

#include <folly/debugging/symbolizer/DwarfUtil.h>

#if FOLLY_HAVE_DWARF && FOLLY_HAVE_ELF

namespace folly {
namespace symbolizer {

DwarfSection::DwarfSection(std::span<const uint8_t> data)
    : is64Bit_(false), data_(data) {}

// Next chunk in section
bool DwarfSection::next(std::span<const uint8_t>& chunk) {
  auto cursor = data_;
  if (cursor.empty()) {
    return false;
  }

  // Initial length is a uint32_t value for a 32-bit section, and
  // a 96-bit value (0xffffffff followed by the 64-bit length) for a 64-bit
  // section. Either read below aborts on truncation, so check first.
  if (data_.size() < sizeof(uint32_t)) {
    return false;
  }
  auto initialLength = read<uint32_t>(cursor);
  is64Bit_ = (initialLength == uint32_t(-1));
  if (is64Bit_ && cursor.size() < sizeof(uint64_t)) {
    return false;
  }
  auto length = is64Bit_ ? read<uint64_t>(cursor) : initialLength;
  if (length > cursor.size()) {
    return false;
  }
  auto headerSize = data_.size() - cursor.size();
  chunk = data_.subspan(headerSize, length);
  data_ = data_.subspan(headerSize + length);
  return true;
}

} // namespace symbolizer
} // namespace folly

#endif // FOLLY_HAVE_DWARF
