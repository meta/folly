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

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#include <folly/Expected.h>
#include <folly/lang/cstring_view.h>

namespace folly::symbolizer {

enum class DwarfParseErrorCode : uint8_t {
  truncated,
  integer_overflow,
  invalid_offset,
  invalid_value,
  unterminated_string,
};

struct DwarfParseError {
  DwarfParseErrorCode code;
  size_t offset;

  friend bool operator==(const DwarfParseError&, const DwarfParseError&) =
      default;
};

template <typename T>
using DwarfParseResult = folly::Expected<T, DwarfParseError>;

/**
 * An allocation-free cursor for parsing untrusted DWARF data.
 *
 * Failed reads leave the cursor unchanged. The error offset identifies the
 * beginning of the value that could not be decoded.
 */
class DwarfCursor {
 public:
  explicit constexpr DwarfCursor(
      std::span<const uint8_t> bytes, size_t baseOffset = 0) noexcept
      : bytes_(bytes), baseOffset_(baseOffset) {}

  constexpr size_t offset() const noexcept { return baseOffset_ + position_; }
  constexpr size_t totalSize() const noexcept { return bytes_.size(); }
  constexpr bool empty() const noexcept { return position_ == bytes_.size(); }
  constexpr std::span<const uint8_t> remaining() const noexcept {
    return bytes_.subspan(position_);
  }

  template <typename T>
  DwarfParseResult<T> read() noexcept {
    static_assert(std::is_standard_layout_v<T> && std::is_trivial_v<T>);
    if (sizeof(T) > bytes_.size() - position_) {
      return fail<T>(DwarfParseErrorCode::truncated);
    }
    T value;
    std::memcpy(&value, bytes_.data() + position_, sizeof(T));
    position_ += sizeof(T);
    return value;
  }

  template <size_t N>
  DwarfParseResult<uint64_t> readU64() noexcept {
    static_assert(N >= 1 && N <= sizeof(uint64_t));
    if (N > bytes_.size() - position_) {
      return fail<uint64_t>(DwarfParseErrorCode::truncated);
    }
    uint64_t value = 0;
    std::memcpy(&value, bytes_.data() + position_, N);
    position_ += N;
    return value;
  }

  DwarfParseResult<uint64_t> readULEB() noexcept {
    const auto start = position_;
    const auto available = bytes_.size() - start;
    uint64_t value = 0;
    for (size_t i = 0; i < 10; ++i) {
      if (i >= available) {
        return fail<uint64_t>(DwarfParseErrorCode::truncated);
      }
      const auto byte = bytes_[start + i];
      const auto payload = static_cast<uint8_t>(byte & 0x7f);
      if (i == 9 && ((byte & 0x80) != 0 || payload > 1)) {
        return fail<uint64_t>(DwarfParseErrorCode::integer_overflow);
      }
      value |= static_cast<uint64_t>(payload) << (i * 7);
      if ((byte & 0x80) == 0) {
        position_ = start + i + 1;
        return value;
      }
    }
    return fail<uint64_t>(DwarfParseErrorCode::integer_overflow);
  }

  DwarfParseResult<int64_t> readSLEB() noexcept {
    const auto start = position_;
    const auto available = bytes_.size() - start;
    uint64_t value = 0;
    for (size_t i = 0; i < 10; ++i) {
      if (i >= available) {
        return fail<int64_t>(DwarfParseErrorCode::truncated);
      }
      const auto byte = bytes_[start + i];
      const auto payload = static_cast<uint8_t>(byte & 0x7f);
      if (i == 9 && ((byte & 0x80) != 0 || (payload != 0 && payload != 0x7f))) {
        return fail<int64_t>(DwarfParseErrorCode::integer_overflow);
      }
      value |= static_cast<uint64_t>(payload) << (i * 7);
      if ((byte & 0x80) == 0) {
        const auto shift = (i + 1) * 7;
        if (shift < 64 && (byte & 0x40) != 0) {
          value |= ~uint64_t{0} << shift;
        }
        position_ = start + i + 1;
        return std::bit_cast<int64_t>(value);
      }
    }
    return fail<int64_t>(DwarfParseErrorCode::integer_overflow);
  }

  DwarfParseResult<uint64_t> readOffset(bool is64Bit) noexcept {
    if (is64Bit) {
      return read<uint64_t>();
    }
    return read<uint32_t>();
  }

  DwarfParseResult<std::span<const uint8_t>> readBytes(size_t size) noexcept {
    if (size > bytes_.size() - position_) {
      return fail<std::span<const uint8_t>>(DwarfParseErrorCode::truncated);
    }
    auto value = bytes_.subspan(position_, size);
    position_ += size;
    return value;
  }

  DwarfParseResult<folly::cstring_view> readCString() noexcept {
    const auto bytes = remaining();
    if (bytes.empty()) {
      return fail<folly::cstring_view>(
          DwarfParseErrorCode::unterminated_string);
    }
    const auto* terminator =
        static_cast<const uint8_t*>(std::memchr(bytes.data(), 0, bytes.size()));
    if (terminator == nullptr) {
      return fail<folly::cstring_view>(
          DwarfParseErrorCode::unterminated_string);
    }
    const auto size = static_cast<size_t>(terminator - bytes.data());
    folly::cstring_view value(
        reinterpret_cast<const char*>(bytes.data()), size);
    position_ += size + 1;
    return value;
  }

 private:
  template <typename T>
  DwarfParseResult<T> fail(DwarfParseErrorCode code) const noexcept {
    return folly::makeUnexpected(DwarfParseError{code, offset()});
  }

  std::span<const uint8_t> bytes_;
  size_t baseOffset_ = 0;
  size_t position_ = 0;
};

} // namespace folly::symbolizer
