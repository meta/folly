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

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <folly/CPortability.h>
#include <folly/lang/Bits.h>

namespace folly {

//  memeq
//
//  Returns std::memcmp(lhs, rhs, n) == 0. Ranges of up to 32 bytes are
//  compared with overlapping unaligned loads instead of a library call, which
//  lets callers in hot loops avoid saving and restoring registers around it.
FOLLY_ALWAYS_INLINE bool memeq(
    void const* lhs, void const* rhs, std::size_t n) noexcept {
  auto const l = static_cast<unsigned char const*>(lhs);
  auto const r = static_cast<unsigned char const*>(rhs);
  if (n > 32) {
    return std::memcmp(l, r, n) == 0;
  }
  if (n >= 8) {
    auto diff = [&](std::size_t i) {
      return loadUnaligned<uint64_t>(l + i) ^ loadUnaligned<uint64_t>(r + i);
    };
    if (n <= 16) {
      return (diff(0) | diff(n - 8)) == 0;
    }
    return (diff(0) | diff(8) | diff(n - 16) | diff(n - 8)) == 0;
  }
  if (n >= 4) {
    auto diff = [&](std::size_t i) {
      return loadUnaligned<uint32_t>(l + i) ^ loadUnaligned<uint32_t>(r + i);
    };
    return (diff(0) | diff(n - 4)) == 0;
  }
  if (n == 0) {
    return true;
  }
  return l[0] == r[0] && l[n / 2] == r[n / 2] && l[n - 1] == r[n - 1];
}

} // namespace folly
