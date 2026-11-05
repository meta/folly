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

#include <folly/rust/dynamic/dynamic.h>

#include <string_view>
#include <utility>

#include <folly/json/json.h>

namespace facebook::folly_rust::dynamic {
using folly::dynamic;

namespace {
std::string_view as_string_view(
    rust::Slice<const std::uint8_t> bytes) noexcept {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
} // namespace

std::unique_ptr<dynamic> new_dynamic_null() noexcept {
  return std::make_unique<dynamic>();
}

std::unique_ptr<dynamic> new_dynamic_bool(bool value) noexcept {
  return std::make_unique<dynamic>(value);
}

std::unique_ptr<dynamic> new_dynamic_int(int64_t value) noexcept {
  return std::make_unique<dynamic>(value);
}

std::unique_ptr<dynamic> new_dynamic_double(double value) noexcept {
  return std::make_unique<dynamic>(value);
}

std::unique_ptr<dynamic> new_dynamic_string(folly::StringPiece value) noexcept {
  return std::make_unique<dynamic>(std::string(value));
}

std::unique_ptr<dynamic> new_dynamic_array() noexcept {
  return std::make_unique<dynamic>(dynamic::array());
}

std::unique_ptr<dynamic> new_dynamic_object() noexcept {
  return std::make_unique<dynamic>(dynamic::object());
}

rust::String get_string(const dynamic& d) {
  return rust::String(d.getString());
}

void array_push_back(dynamic& d, std::unique_ptr<dynamic> value) {
  d.push_back(std::move(*value));
}

void array_pop_back(dynamic& d) {
  d.pop_back();
}

const dynamic& at(const dynamic& d, size_t index) {
  return d[index];
}

dynamic& at_mut(dynamic& d, size_t index) {
  return d[index];
}

void array_set(dynamic& d, size_t index, std::unique_ptr<dynamic> value) {
  d[index] = std::move(*value);
}

bool object_contains(const dynamic& d, folly::StringPiece key) {
  return d.count(key) > 0;
}

const dynamic& get(const dynamic& d, folly::StringPiece key) {
  return d[key];
}

dynamic& get_mut(dynamic& d, folly::StringPiece key) {
  return d[key];
}

void object_set(
    dynamic& d, folly::StringPiece key, std::unique_ptr<dynamic> value) {
  d[key] = std::move(*value);
}

bool object_erase(dynamic& d, folly::StringPiece key) {
  return d.erase(key) > 0;
}

rust::Vec<rust::String> object_keys(const dynamic& d) {
  rust::Vec<rust::String> keys;
  for (const auto& item : d.items()) {
    keys.push_back(rust::String(item.first.getString()));
  }
  return keys;
}

std::unique_ptr<dynamic> clone_dynamic(const dynamic& d) noexcept {
  return std::make_unique<dynamic>(d);
}

void set_null(dynamic& d) noexcept {
  d = nullptr;
}

void set_bool(dynamic& d, bool value) noexcept {
  d = value;
}

void set_int(dynamic& d, int64_t value) noexcept {
  d = value;
}

void set_double(dynamic& d, double value) noexcept {
  d = value;
}

void set_string(dynamic& d, folly::StringPiece value) noexcept {
  d = std::string(value);
}

void set_dynamic(dynamic& d, const dynamic& value) noexcept {
  d = value;
}

rust::String to_json(const dynamic& d) {
  return rust::String(folly::toJson(d));
}

rust::String to_pretty_json(const dynamic& d) {
  return rust::String(folly::toPrettyJson(d));
}

std::unique_ptr<dynamic> parse_json(folly::StringPiece json) {
  return std::make_unique<dynamic>(folly::parseJson(json));
}

std::unique_ptr<dynamic> new_dynamic_string_bytes(
    rust::Slice<const std::uint8_t> value) noexcept {
  return new_dynamic_string(as_string_view(value));
}

rust::Slice<const std::uint8_t> get_borrowed_bytes(const dynamic& d) {
  const auto& value = d.getString();
  return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

bool object_contains_bytes(
    const dynamic& d, rust::Slice<const std::uint8_t> key) {
  return object_contains(d, as_string_view(key));
}

const dynamic& get_bytes(
    const dynamic& d, rust::Slice<const std::uint8_t> key) {
  return get(d, as_string_view(key));
}

dynamic& get_mut_bytes(dynamic& d, rust::Slice<const std::uint8_t> key) {
  return get_mut(d, as_string_view(key));
}

void object_set_bytes(
    dynamic& d,
    rust::Slice<const std::uint8_t> key,
    std::unique_ptr<dynamic> value) {
  object_set(d, as_string_view(key), std::move(value));
}

bool object_erase_bytes(dynamic& d, rust::Slice<const std::uint8_t> key) {
  return object_erase(d, as_string_view(key));
}

void set_string_bytes(
    dynamic& d, rust::Slice<const std::uint8_t> value) noexcept {
  set_string(d, as_string_view(value));
}

std::unique_ptr<dynamic> parse_json_bytes(
    rust::Slice<const std::uint8_t> json) {
  return parse_json(as_string_view(json));
}

bool equals(const dynamic& lhs, const dynamic& rhs) noexcept {
  return lhs == rhs;
}

} // namespace facebook::folly_rust::dynamic
