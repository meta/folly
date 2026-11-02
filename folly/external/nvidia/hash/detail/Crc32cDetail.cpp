/*
 * Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#if defined(__aarch64__) || defined(__arm__)

#include <cstring>

#include <folly/CPortability.h>
#include <folly/Portability.h>

#if FOLLY_ARM_FEATURE_CRC32

#include <arm_acle.h>

FOLLY_UNSAFE_BUFFER_USAGE_BEGIN

namespace folly::detail {

uint32_t crc32c_hw(const uint8_t* buf, size_t len, uint32_t crc) {
#if defined(__aarch64__)
  // AArch64 has __crc32cd for 8-byte chunks; 
  // AArch32 does not (no 64-bit GPRs), so it falls through to the 4-byte loop below.
  while (len >= 8) {
    uint64_t val = 0;
    std::memcpy(&val, buf, 8);
    crc = __crc32cd(crc, val);
    len -= 8;
    buf += 8;
  }
#endif

  while (len >= 4) {
    uint32_t val = 0;
    std::memcpy(&val, buf, 4);
    crc = __crc32cw(crc, val);
    len -= 4;
    buf += 4;
  }

  if (len >= 2) {
    uint16_t val = 0;
    std::memcpy(&val, buf, 2);
    crc = __crc32ch(crc, val);
    len -= 2;
    buf += 2;
  }

  if (len >= 1) {
    crc = __crc32cb(crc, *buf);
  }

  return crc;
}

} // namespace folly::detail

FOLLY_UNSAFE_BUFFER_USAGE_END

#endif // FOLLY_ARM_FEATURE_CRC32

#endif // __aarch64__ || __arm__
