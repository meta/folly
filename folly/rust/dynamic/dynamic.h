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

#include <cstdint>
#include <memory>

#include <folly/json/dynamic.h>
#include <folly/json/json.h>
#include "rust/cxx.h"

namespace facebook::folly_rust::dynamic {

std::unique_ptr<folly::dynamic> new_dynamic_null() noexcept;
std::unique_ptr<folly::dynamic> new_dynamic_bool(bool value) noexcept;
std::unique_ptr<folly::dynamic> new_dynamic_int(int64_t value) noexcept;
std::unique_ptr<folly::dynamic> new_dynamic_double(double value) noexcept;
std::unique_ptr<folly::dynamic> new_dynamic_string(
    folly::StringPiece value) noexcept;
std::unique_ptr<folly::dynamic> new_dynamic_array() noexcept;
std::unique_ptr<folly::dynamic> new_dynamic_object() noexcept;

rust::String get_string(const folly::dynamic& d);

void array_push_back(folly::dynamic& d, std::unique_ptr<folly::dynamic> value);
void array_pop_back(folly::dynamic& d);
const folly::dynamic& at(const folly::dynamic& d, size_t index);
folly::dynamic& at_mut(folly::dynamic& d, size_t index);
void array_set(
    folly::dynamic& d, size_t index, std::unique_ptr<folly::dynamic> value);

bool object_contains(const folly::dynamic& d, folly::StringPiece key);
const folly::dynamic& get(const folly::dynamic& d, folly::StringPiece key);
folly::dynamic& get_mut(folly::dynamic& d, folly::StringPiece key);
void object_set(
    folly::dynamic& d,
    folly::StringPiece key,
    std::unique_ptr<folly::dynamic> value);
bool object_erase(folly::dynamic& d, folly::StringPiece key);
rust::Vec<rust::String> object_keys(const folly::dynamic& d);

std::unique_ptr<folly::dynamic> clone_dynamic(const folly::dynamic& d) noexcept;

void set_null(folly::dynamic& d) noexcept;
void set_bool(folly::dynamic& d, bool value) noexcept;
void set_int(folly::dynamic& d, int64_t value) noexcept;
void set_double(folly::dynamic& d, double value) noexcept;
void set_string(folly::dynamic& d, folly::StringPiece value) noexcept;
void set_dynamic(folly::dynamic& d, const folly::dynamic& value) noexcept;

rust::String to_json(const folly::dynamic& d);
rust::String to_pretty_json(const folly::dynamic& d);
std::unique_ptr<folly::dynamic> parse_json(folly::StringPiece json);

std::unique_ptr<folly::dynamic> new_dynamic_string_bytes(
    rust::Slice<const std::uint8_t> value) noexcept;
rust::Slice<const std::uint8_t> get_borrowed_bytes(const folly::dynamic& d);
bool object_contains_bytes(
    const folly::dynamic& d, rust::Slice<const std::uint8_t> key);
const folly::dynamic& get_bytes(
    const folly::dynamic& d, rust::Slice<const std::uint8_t> key);
folly::dynamic& get_mut_bytes(
    folly::dynamic& d, rust::Slice<const std::uint8_t> key);
void object_set_bytes(
    folly::dynamic& d,
    rust::Slice<const std::uint8_t> key,
    std::unique_ptr<folly::dynamic> value);
bool object_erase_bytes(folly::dynamic& d, rust::Slice<const std::uint8_t> key);
void set_string_bytes(
    folly::dynamic& d, rust::Slice<const std::uint8_t> value) noexcept;
std::unique_ptr<folly::dynamic> parse_json_bytes(
    rust::Slice<const std::uint8_t> json);

bool equals(const folly::dynamic& lhs, const folly::dynamic& rhs) noexcept;

} // namespace facebook::folly_rust::dynamic
