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

#include <functional>
#include <string_view>
#include <type_traits>

#include <folly/Conv.h>
#include <folly/Expected.h>
#include <folly/Unit.h>
#include <folly/Utility.h>

namespace folly {
namespace settings {

enum class SetErrorCode {
  NotFound,
  Rejected,
  FrozenImmutable,
};

using SetResult = Expected<Unit, SetErrorCode>;

std::string_view toString(SetErrorCode code);

enum class Mutability {
  Mutable,
  Immutable,
};
enum class CommandLine {
  AcceptOverrides,
  RejectOverrides,
};

/**
 * Static information about the setting definition
 */
struct SettingMetadata {
  /**
   * Project string.
   */
  std::string_view project;

  /**
   * Setting name within the project.
   */
  std::string_view name;

  /**
   * String representation of the type.
   */
  std::string_view typeStr;

  /**
   * typeid() of the type.
   */
  const std::type_info& typeId;

  /**
   * String representation of the default value.
   * (note: string literal default values will be stringified with quotes)
   */
  std::string_view defaultStr;

  /**
   * Determines if the setting can change after initialization.
   */
  Mutability mutability;

  /**
   * Determines if the setting can be set from the command line.
   */
  CommandLine commandLine;

  /**
   * Setting description field.
   */
  std::string_view description;
};

/**
 * Type containing the string representation of a setting as well as the static
 * metadata associated with it. Used as the "source" in setting conversion.
 */
struct SettingValueAndMetadata {
  SettingValueAndMetadata(
      std::string_view valueStr, const SettingMetadata& metadata)
      : value(valueStr), meta(metadata) {}

  std::string_view value;
  const SettingMetadata& meta;
};

/**
 * Like parseTo in folly/Conv.h, but the "source" is SettingValueAndMetadata
 * instead of a StringPiece. Defaults to folly::tryTo<T>(StringPiece) but can be
 * overridden using ADL for user defined types.
 */
template <typename T>
Expected<Unit, ExpectedErrorType<decltype(tryTo<T>(StringPiece{}))>> convertTo(
    const SettingValueAndMetadata& src, T& out) {
  auto result = tryTo<T>(StringPiece(src.value));
  if (result.hasError()) {
    return makeUnexpected(std::move(result).error());
  }
  out = std::move(result).value();
  return unit;
}

/**
 * Conversion functions for converting a SettingValueAndMetadata to a setting
 * type T. Implementation is similar to folly/Conv and allows for customization
 * using the convertTo function above.
 */
template <typename T>
Expected<
    T,
    ExpectedErrorType<decltype(convertTo(
        std::declval<const SettingValueAndMetadata&>(), std::declval<T&>()))>>
tryTo(const SettingValueAndMetadata& src) {
  T result;
  auto convResult = convertTo(src, result);
  if (convResult.hasError()) {
    return makeUnexpected(std::move(convResult).error());
  }
  return result;
}
template <typename T>
T to(const SettingValueAndMetadata& src) {
  using ErrorCode = ExpectedErrorType<decltype(tryTo<T>(
      std::declval<const SettingValueAndMetadata&>()))>;
  return tryTo<T>(src).thenOrThrow(std::identity{}, [&](const ErrorCode& e) {
    throw_exception(makeConversionError(e, src.value));
  });
}
} // namespace settings
} // namespace folly
