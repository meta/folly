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

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <string_view>

#include <folly/lang/Exception.h>

namespace folly {

/**
 * A view of UTF-8 text as a bidirectional range of code points.
 *
 * Invalid UTF-8 throws std::out_of_range: construction checks that the text
 * doesn't start with a continuation byte or end with a truncated sequence,
 * incrementing and decrementing check the sequence structure, and
 * dereferencing also rejects overlong encodings, surrogates and values above
 * U+10FFFF.
 */
class UTF8StringPiece {
 public:
  class iterator {
   public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = std::uint32_t;
    using difference_type = std::ptrdiff_t;
    using pointer = const value_type*;
    using reference = value_type;

    iterator() = default;

    value_type operator*() const {
      size_t const size = sequenceSize(byte(0));
      static constexpr value_type kLeadMask[] = {0, 0x7F, 0x1F, 0x0F, 0x07};
      static constexpr value_type kMin[] = {0, 0, 0x80, 0x800, 0x10000};
      value_type cp = byte(0) & kLeadMask[size];
      for (size_t i = 1; i < size; ++i) {
        if (!isContinuation(byte(i))) {
          invalid();
        }
        cp = (cp << 6) | (byte(i) & 0x3F);
      }
      if (cp < kMin[size] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        invalid();
      }
      return cp;
    }

    iterator& operator++() {
      size_t const size = sequenceSize(byte(0));
      for (size_t i = 1; i < size; ++i) {
        if (!isContinuation(byte(i))) {
          invalid();
        }
      }
      pos_ += size;
      return *this;
    }

    iterator operator++(int) {
      iterator it = *this;
      ++*this;
      return it;
    }

    iterator& operator--() {
      size_t size = 1;
      // Can't pass begin_: the constructor rejects a leading continuation byte.
      while (isContinuation(static_cast<unsigned char>(*--pos_))) {
        ++size;
      }
      if (sequenceSize(byte(0)) != size) {
        invalid();
      }
      return *this;
    }

    iterator operator--(int) {
      iterator it = *this;
      --*this;
      return it;
    }

    // Returns the position of the current code point in the UTF-8 text.
    const char* base() const noexcept { return pos_; }

    friend bool operator==(const iterator& a, const iterator& b) noexcept {
      return a.pos_ == b.pos_;
    }

   private:
    friend class UTF8StringPiece;

    explicit iterator(const char* pos) noexcept : pos_(pos) {}

    unsigned char byte(size_t i) const noexcept {
      return static_cast<unsigned char>(pos_[i]);
    }

    const char* pos_ = nullptr;
  };

  using size_type = std::size_t;

  /* implicit */ UTF8StringPiece(const std::string_view piece)
      : UTF8StringPiece(
            piece.data(),
            piece.empty() ? piece.data() : piece.data() + piece.size()) {}
  /* implicit */ UTF8StringPiece(std::nullptr_t) = delete;
  template <typename T>
    requires std::convertible_to<T, std::string_view>
  /* implicit */ UTF8StringPiece(const T& t)
      : UTF8StringPiece(std::string_view(t)) {}

  iterator begin() const noexcept { return begin_; }
  iterator cbegin() const noexcept { return begin_; }
  iterator end() const noexcept { return end_; }
  iterator cend() const noexcept { return end_; }

  bool empty() const noexcept { return begin_ == end_; }
  size_type walk_size() const {
    return static_cast<size_type>(std::distance(begin_, end_));
  }

 private:
  UTF8StringPiece(const char* first, const char* last)
      : begin_{first}, end_{last} {
    if (first == last) {
      return;
    }
    if (isContinuation(static_cast<unsigned char>(*first))) {
      invalid();
    }
    // Make sure the last sequence is complete so iteration stays in bounds.
    const char* lead = last - 1;
    while (lead != first && isContinuation(static_cast<unsigned char>(*lead))) {
      --lead;
    }
    if (static_cast<size_t>(last - lead) <
        sequenceSize(static_cast<unsigned char>(*lead))) {
      invalid();
    }
  }

  static bool isContinuation(unsigned char c) noexcept {
    return (c & 0xC0) == 0x80;
  }

  // Returns the length of a sequence from its lead byte.
  static size_t sequenceSize(unsigned char lead) {
    if (lead < 0x80) {
      return 1;
    }
    if (lead < 0xC0) {
      invalid(); // continuation byte
    }
    if (lead < 0xE0) {
      return 2;
    }
    if (lead < 0xF0) {
      return 3;
    }
    if (lead < 0xF8) {
      return 4;
    }
    invalid();
  }

  [[noreturn]] static void invalid() {
    throw_exception<std::out_of_range>("Invalid UTF-8 sequence");
  }

  iterator begin_;
  iterator end_;
};

} // namespace folly
