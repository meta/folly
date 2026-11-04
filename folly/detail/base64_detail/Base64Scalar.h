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

#include <array>
#include <cstdint>
#include <folly/Utility.h>
#include <folly/detail/base64_detail/Base64Common.h>
#include <folly/detail/base64_detail/Base64Constants.h>
#include <folly/memory/UninitializedMemoryHacks.h>

namespace folly::detail::base64_detail {

constexpr std::uint8_t atAsU8(const char* f, int offset) {
  return static_cast<std::uint8_t>(f[offset]);
}

constexpr std::array<std::uint8_t, 3> base64DecodePack4To3(
    std::uint8_t aaa, std::uint8_t bbb, std::uint8_t ccc, std::uint8_t ddd) {
  std::uint8_t aaab = static_cast<std::uint8_t>((aaa << 2) | (bbb >> 4));
  std::uint8_t bbcc = static_cast<std::uint8_t>((bbb << 4) | (ccc >> 2));
  std::uint8_t cddd = static_cast<std::uint8_t>((ccc << 6) | ddd);

  return {{aaab, bbcc, cddd}};
}

template <bool isURL>
struct Base64ScalarImpl {
  static constexpr const char* kCharset =
      isURL ? constants::kBase64URLCharset : constants::kBase64Charset;

  static constexpr const char* kDecodeTable = isURL
      ? constants::kBase64URLDecodeTable.data()
      : constants::kBase64DecodeTable.data();

  // 0, 1 or 2 bytes
  static constexpr char* encodeTail(const char* f, const char* l, char* o) {
    if (f == l) {
      return o;
    }

    std::uint8_t aaab = f[0];
    std::uint8_t aaa = aaab >> 2;
    *o++ = kCharset[aaa];

    // duplicating some tail handling to try to do less jumps
    if (l - f == 1) {
      std::uint8_t b00 = aaab << 4 & 0x3f;
      *o++ = kCharset[b00];
      if constexpr (!isURL) {
        *o++ = '=';
        *o++ = '=';
      }
      return o;
    }

    // l - f == 2
    std::uint8_t bbcc = f[1];
    std::uint8_t bbb = ((aaab << 4) | (bbcc >> 4)) & 0x3f;
    std::uint8_t cc0 = (bbcc << 2) & 0x3f;
    *o++ = kCharset[bbb];
    *o++ = kCharset[cc0];
    if constexpr (!isURL) {
      *o++ = '=';
    }
    return o;
  }

  static constexpr char* encode(const char* f, const char* l, char* o) {
    // clang-21+ over-vectorizes this small scalar loop, spilling ~768B onto the
    // stack (frame 168B -> 936B at -O3) with no throughput benefit -- the bulk
    // path uses explicit SIMD intrinsics and is unaffected. Disable
    // auto-vectorization for just this loop; revert once the underlying LLVM
    // codegen bug is fixed and backported to llvm-fb:21.
    // FIXME(T284058114)
#if defined(__clang__)
#pragma clang loop vectorize(disable) interleave(disable)
#endif
    while ((l - f) >= 3) {
      std::uint8_t aaab = f[0];
      std::uint8_t bbcc = f[1];
      std::uint8_t cddd = f[2];

      std::uint8_t aaa = aaab >> 2;
      std::uint8_t bbb = ((aaab << 4) | (bbcc >> 4)) & 0x3f;
      std::uint8_t ccc = ((bbcc << 2) | (cddd >> 6)) & 0x3f;
      std::uint8_t ddd = cddd & 0x3f;

      o[0] = kCharset[aaa];
      o[1] = kCharset[bbb];
      o[2] = kCharset[ccc];
      o[3] = kCharset[ddd];

      f += 3;
      o += 4;
    }

    return encodeTail(f, l, o);
  }

  static constexpr std::uint8_t decodeMainLoop(
      const char*& f, std::size_t fullSteps, char*& o) {
    std::uint8_t errorAccumulator = 0;
    while (fullSteps--) {
      std::uint8_t aaa = kDecodeTable[atAsU8(f, 0)];
      std::uint8_t bbb = kDecodeTable[atAsU8(f, 1)];
      std::uint8_t ccc = kDecodeTable[atAsU8(f, 2)];
      std::uint8_t ddd = kDecodeTable[atAsU8(f, 3)];

      errorAccumulator |= aaa | bbb | ccc | ddd;

      auto packed = base64DecodePack4To3(aaa, bbb, ccc, ddd);

      o[0] = static_cast<char>(packed[0]);
      o[1] = static_cast<char>(packed[1]);
      o[2] = static_cast<char>(packed[2]);

      f += 4;
      o += 3;
    }
    return errorAccumulator;
  }
};

constexpr char* base64EncodeScalar(
    const char* f, const char* l, char* o) noexcept {
  return Base64ScalarImpl</*isURL*/ false>::encode(f, l, o);
}

constexpr char* base64URLEncodeScalar(
    const char* f, const char* l, char* o) noexcept {
  return Base64ScalarImpl</*isURL*/ true>::encode(f, l, o);
}

constexpr char* base64DecodeTailScalar(
    const char* f, char* o, std::uint8_t& errorAccumulator) {
  std::uint8_t aaa = constants::kBase64DecodeTable[atAsU8(f, 0)];
  std::uint8_t bbb = constants::kBase64DecodeTable[atAsU8(f, 1)];

  *o++ = static_cast<std::uint8_t>((aaa << 2) | (bbb >> 4));
  errorAccumulator |= aaa | bbb;

  if (f[2] == '=' && f[3] == '=') {
    if (bbb & 0xf) {
      errorAccumulator = constants::kDecodeErrorMarker;
    }
    return o;
  }

  std::uint8_t ccc = constants::kBase64DecodeTable[atAsU8(f, 2)];
  *o++ = static_cast<char>((bbb << 4) | (ccc >> 2));
  errorAccumulator |= ccc;

  if (f[3] == '=') {
    if (ccc & 0x3) {
      errorAccumulator = constants::kDecodeErrorMarker;
    }
    return o;
  }

  std::uint8_t ddd = constants::kBase64DecodeTable[atAsU8(f, 3)];
  *o++ = static_cast<char>((ccc << 6) | ddd);
  errorAccumulator |= ddd;

  return o;
}

constexpr Base64DecodeResult base64DecodeScalar(
    const char* f, const char* l, char* o) noexcept {
  if (f == l) {
    return {true, o};
  }
  if ((l - f) % 4) {
    return {false, o};
  }

  std::size_t fullSteps = (l - f) / 4 - 1; // last step may contain padding
                                           // and needs special care.

  std::uint8_t errorAccumulator =
      Base64ScalarImpl</*isURL*/ false>::decodeMainLoop(f, fullSteps, o);

  o = base64DecodeTailScalar(f, o, errorAccumulator);

  return {
      errorAccumulator !=
          static_cast<std::uint8_t>(constants::kDecodeErrorMarker),
      o};
}

constexpr void base64URLDecodeStripValidPadding(const char* f, const char*& l) {
  // Valid paddings:
  // 00==, 000=
  // Invalid paddings:
  // 00=, =, ==, 00=0
  if ((l - f) != 4) {
    return;
  }
  l -= *(l - 1) == '=';
  l -= *(l - 1) == '=';
}

constexpr char* base64URLDecodeScalarLast4Bytes(
    const char* f, const char* l, char* o, std::uint8_t& errorAccumulator) {
  base64URLDecodeStripValidPadding(f, l);

  std::uint8_t aaa = constants::kBase64URLDecodeTable[atAsU8(f, 0)];
  std::uint8_t bbb = constants::kBase64URLDecodeTable[atAsU8(f, 1)];

  *o++ = static_cast<std::uint8_t>((aaa << 2) | (bbb >> 4));
  errorAccumulator |= aaa | bbb; // This will detect incorrect padding as well

  f += 2;
  if (f == l) {
    return o;
  }

  std::uint8_t ccc = constants::kBase64URLDecodeTable[atAsU8(f, 0)];
  *o++ = static_cast<char>((bbb << 4) | (ccc >> 2));
  errorAccumulator |= ccc;
  ++f;

  if (f == l) {
    return o;
  }

  std::uint8_t ddd = constants::kBase64URLDecodeTable[atAsU8(f, 0)];
  *o++ = static_cast<char>((ccc << 6) | ddd);
  errorAccumulator |= ddd;

  return o;
}

constexpr Base64DecodeResult base64URLDecodeScalar(
    const char* f, const char* l, char* o) noexcept {
  if (f == l) {
    return {true, o};
  }
  std::size_t rem = (l - f) % 4;

  std::size_t fullSteps =
      (l - f) / 4 - (rem == 0); // last 4 bytes may contain padding
                                // and need special care.

  std::uint8_t errorAccumulator =
      Base64ScalarImpl</*isURL*/ true>::decodeMainLoop(f, fullSteps, o);

  if (l - f < 2) {
    return {false, o};
  }
  o = base64URLDecodeScalarLast4Bytes(f, l, o, errorAccumulator);

  return {
      errorAccumulator !=
          static_cast<std::uint8_t>(constants::kDecodeErrorMarker),
      o};
}

constexpr inline const char* base64PHPStrictRemoveWhitespacesScalar(
    const char* f, const char* l, char* o) noexcept {
  constexpr auto& skips =
      detail::base64_detail::constants::kBase64PHPStrictDecodeSkipTable;
  std::size_t pos = 0;

  while (f != l) {
    auto v = *f++;
    o[pos] = v;
    pos += skips[to_unsigned(v)];
  }

  return o + pos;
}

} // namespace folly::detail::base64_detail
