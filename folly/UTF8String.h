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
#include <iterator>
#include <string_view>

#include <boost/regex/pending/unicode_iterator.hpp>

namespace folly {

class UTF8StringPiece {
 public:
  using iterator = boost::u8_to_u32_iterator<const char*>;
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
      : begin_{first, first, last}, end_{last, first, last} {}

  iterator begin_;
  iterator end_;
};

} // namespace folly
