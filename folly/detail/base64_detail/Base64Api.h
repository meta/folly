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

#include <type_traits>
#include <folly/Portability.h>
#include <folly/detail/base64_detail/Base64Common.h>
#include <folly/detail/base64_detail/Base64Scalar.h>

namespace folly::detail::base64_detail {

struct Base64RuntimeImpl {
  using Encode = char* (*)(const char*, const char*, char*) noexcept;
  using Decode =
      Base64DecodeResult (*)(const char*, const char*, char*) noexcept;

  Encode encode;
  Encode encodeURL;
  Decode decode;
  Decode decodeURL;
};

Base64RuntimeImpl base64EncodeSelectImplementation();

inline const auto& base64RuntimeImpl() {
  static const auto r = base64EncodeSelectImplementation();
  return r;
}

inline char* base64EncodeRuntime(
    const char* f, const char* l, char* o) noexcept {
  return base64RuntimeImpl().encode(f, l, o);
}

inline constexpr char* base64Encode(
    const char* f, const char* l, char* o) noexcept {
  if (std::is_constant_evaluated()) {
    return base64EncodeScalar(f, l, o);
  } else {
    return base64EncodeRuntime(f, l, o);
  }
}

inline char* base64URLEncodeRuntime(
    const char* f, const char* l, char* o) noexcept {
  return base64RuntimeImpl().encodeURL(f, l, o);
}

inline constexpr char* base64URLEncode(
    const char* f, const char* l, char* o) noexcept {
  if (std::is_constant_evaluated()) {
    return base64URLEncodeScalar(f, l, o);
  } else {
    return base64URLEncodeRuntime(f, l, o);
  }
}

inline Base64DecodeResult base64DecodeRuntime(
    const char* f, const char* l, char* o) noexcept {
  return base64RuntimeImpl().decode(f, l, o);
}

inline constexpr Base64DecodeResult base64Decode(
    const char* f, const char* l, char* o) noexcept {
  if (std::is_constant_evaluated()) {
    return base64DecodeScalar(f, l, o);
  } else {
    return base64DecodeRuntime(f, l, o);
  }
}

inline Base64DecodeResult base64URLDecodeRuntime(
    const char* f, const char* l, char* o) noexcept {
  return base64RuntimeImpl().decodeURL(f, l, o);
}

inline constexpr Base64DecodeResult base64URLDecode(
    const char* f, const char* l, char* o) noexcept {
  if (std::is_constant_evaluated()) {
    return base64URLDecodeScalar(f, l, o);
  } else {
    return base64URLDecodeRuntime(f, l, o);
  }
}

inline constexpr const char* base64PHPStrictRemoveWhitespaces(
    const char* f, const char* l, char* o) noexcept {
  return base64PHPStrictRemoveWhitespacesScalar(f, l, o);
}

inline constexpr Base64DecodeResult base64PHPStrictDecode(
    const char* f, const char* l, char* o) noexcept {
  const char* o_l = base64PHPStrictRemoveWhitespaces(f, l, o);

  // Encoded string is always longer so reuse output to avoid extra allocation
  Base64DecodeResult result = base64Decode(o, o_l, o);
  return {result.isSuccess, result.o};
}

} // namespace folly::detail::base64_detail
