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

#include <folly/json/json_pointer.h>

#include <algorithm>
#include <iterator>
#include <string_view>
#include <type_traits>
#include <utility>

#include <folly/String.h>

namespace folly {

// Movability relies on there being no user-declared destructor; keep this
// pinned so `parse()`'s `std::move(res).value()` stays a real move.
static_assert(std::is_nothrow_move_constructible_v<json_pointer>);

// static, public
Expected<json_pointer, json_pointer::parse_error> json_pointer::try_parse(
    std::string_view const str) {
  // pointer describes complete document
  if (str.empty()) {
    return json_pointer{};
  }

  if (str.at(0) != '/') {
    return makeUnexpected(parse_error::invalid_first_character);
  }

  std::vector<std::string> tokens;
  splitTo<std::string>('/', str, std::inserter(tokens, tokens.begin()));
  tokens.erase(tokens.begin());

  for (auto& token : tokens) {
    if (!unescape(token)) {
      return makeUnexpected(parse_error::invalid_escape_sequence);
    }
  }

  return json_pointer(std::move(tokens));
}

// static, public
json_pointer json_pointer::parse(std::string_view const str) {
  auto res = try_parse(str);
  if (res.hasValue()) {
    return std::move(res).value();
  }
  switch (res.error()) {
    case parse_error::invalid_first_character:
      throw json_pointer::parse_exception(
          "non-empty JSON pointer string does not start with '/'");
    case parse_error::invalid_escape_sequence:
      throw json_pointer::parse_exception(
          "Invalid escape sequence in JSON pointer string");
    default:
      FOLLY_ASSUME_UNREACHABLE();
  }
}

bool json_pointer::is_prefix_of(json_pointer const& other) const noexcept {
  auto const& other_tokens = other.tokens();
  if (tokens_.size() > other_tokens.size()) {
    return false;
  }
  auto const other_begin = other_tokens.cbegin();
  auto const other_end = other_tokens.cbegin() + tokens_.size();
  return std::equal(tokens_.cbegin(), tokens_.cend(), other_begin, other_end);
}

std::vector<std::string> const& json_pointer::tokens() const {
  return tokens_;
}

// private
json_pointer::json_pointer(std::vector<std::string> tokens) noexcept
    : tokens_{std::move(tokens)} {}

// private, static
bool json_pointer::unescape(std::string& str) {
  char* out = &str[0];
  char const* begin = out;
  char const* end = begin + str.size();
  char const* decode = begin;
  while (decode < end) {
    if (*decode != '~') {
      *out++ = *decode++;
      continue;
    }
    if (decode + 1 == end) {
      return false;
    }
    switch (decode[1]) {
      case '1':
        *out++ = '/';
        break;
      case '0':
        *out++ = '~';
        break;
      default:
        return false;
    }
    decode += 2;
  }
  str.resize(out - begin);
  return true;
}

} // namespace folly
