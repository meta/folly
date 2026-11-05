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

#include <folly/hash/Checksum.h>

#include <algorithm>
#include <stdexcept> // IWYU pragma: keep

#include <folly/CpuId.h>
#include <folly/detail/TrapOnAvx512.h>
#include <folly/external/fast-crc32/avx512_crc32c_v8s3x4.h> // @manual
#include <folly/external/fast-crc32/neon_crc32c_v3s4x2e_v2.h> // @manual
#include <folly/external/fast-crc32/neon_eor3_crc32_v8s2x4e_s1x2.h> // @manual
#include <folly/external/fast-crc32/neon_eor3_crc32c_v8s2x4e_s2x1.h> // @manual
#include <folly/external/fast-crc32/sse_crc32c_v8s3x3.h> // @manual
#include <folly/hash/detail/ChecksumDetail.h>

#if FOLLY_X64 && FOLLY_SSE_PREREQ(4, 2)
#include <emmintrin.h>
#endif

namespace folly {

namespace detail {

uint32_t crc32c_sw(
    const uint8_t* data, size_t nbytes, uint32_t startingChecksum);
#if FOLLY_X64 && FOLLY_SSE_PREREQ(4, 2)

uint32_t crc32_sw(
    const uint8_t* data, size_t nbytes, uint32_t startingChecksum);

// Fast SIMD implementation of CRC-32 for x86 with pclmul
uint32_t crc32_hw(
    const uint8_t* data, size_t nbytes, uint32_t startingChecksum) {
  uint32_t sum = startingChecksum;
  size_t offset = 0;

  // Process unaligned bytes
  if ((uintptr_t)data & 15) {
    size_t limit = std::min(nbytes, -(uintptr_t)data & 15);
    sum = crc32_sw(data, limit, sum);
    offset += limit;
    nbytes -= limit;
  }

  if (nbytes >= 16) {
    const __m128i* vec = (const __m128i*)(data + offset);
#if FOLLY_HAS_CRC32_VPCLMUL
    sum = crc32_vpclmul_usable()
        ? crc32_hw_aligned_vpclmul(sum, vec, nbytes / 16)
        : crc32_hw_aligned(sum, vec, nbytes / 16);
#else
    sum = crc32_hw_aligned(sum, vec, nbytes / 16);
#endif
    offset += nbytes & ~15;
    nbytes &= 15;
  }

  // Remaining unaligned bytes
  if (nbytes == 0) {
    return sum;
  }
  return crc32_sw(data + offset, nbytes, sum);
}

bool crc32c_hw_supported() {
  return crc32c_hw_supported_sse42();
}

bool crc32c_hw_supported_sse42() {
  static folly::CpuId id;
  return id.sse42();
}

// Under FOLLY_DISABLE_AVX512 the kernel stays compiled so that tests can
// still call it.
bool crc32c_hw_supported_avx512() {
#if FOLLY_DISABLE_AVX512
  return false;
#else
  static folly::CpuId id;
  static bool supported = id.avx512vl() && !detail::hasTrapOnAvx512() &&
      !detail::hasTrapOnEntry(reinterpret_cast<const void*>(
          &detail::avx512_crc32c_v8s3x4));
  return supported;
#endif
}

bool crc32_hw_supported() {
  static folly::CpuId id;
  return id.sse42();
}

bool crc32c_hw_supported_neon() {
  return false;
}

bool crc32c_hw_supported_neon_eor3_sha3() {
  return false;
}

bool crc32_hw_supported_neon_eor3_sha3() {
  return false;
}

#elif FOLLY_ARM_FEATURE_CRC32

// crc32_hw is defined in folly/external/nvidia/hash/Checksum.cpp

bool crc32c_hw_supported() {
  return true;
}

bool crc32c_hw_supported_sse42() {
  return false;
}

bool crc32c_hw_supported_avx512() {
  return false;
}

bool crc32c_hw_supported_neon() {
  static bool has_neon = has_neon_crc32c_v3s4x2e_v2();
  return has_neon;
}

bool crc32_hw_supported_neon_eor3_sha3() {
  static bool has_neon_eor3 = has_neon_eor3_crc32_v8s2x4e_s1x2();
  return has_neon_eor3;
}

bool crc32c_hw_supported_neon_eor3_sha3() {
  static bool has_neon_eor3 = has_neon_eor3_crc32c_v8s2x4e_s2x1();
  return has_neon_eor3;
}

bool crc32_hw_supported() {
  return true;
}

#else // FOLLY_ARM_FEATURE_CRC32

uint32_t crc32_hw(
    const uint8_t* /* data */,
    size_t /* nbytes */,
    uint32_t /* startingChecksum */) {
  throw std::runtime_error("crc32_hw is not implemented on this platform");
}

bool crc32c_hw_supported() {
  return false;
}

bool crc32c_hw_supported_sse42() {
  return false;
}

bool crc32c_hw_supported_avx512() {
  return false;
}

bool crc32_hw_supported() {
  return false;
}

bool crc32c_hw_supported_neon() {
  return false;
}

bool crc32_hw_supported_neon_eor3_sha3() {
  return false;
}

bool crc32c_hw_supported_neon_eor3_sha3() {
  return false;
}
#endif

namespace {

// Slice-by-4 CRC tables. entries[0] is the standard byte-at-a-time table:
// entries[0][i] is the CRC of byte i starting from CRC value 0.
// For k > 0, entries[k][i] is the CRC of byte i followed by k zero bytes,
// starting from CRC value 0. These are used to consume 4 input bytes per
// loop iteration, exploiting the linearity of CRC over GF(2).
template <uint32_t CRC_POLYNOMIAL_BIT_REVERSED>
struct CrcSwTable {
  uint32_t entries[4][256];
  constexpr CrcSwTable() : entries{} {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t crc = i;
      for (int j = 0; j < 8; j++) {
        crc = (crc & 1) ? (crc >> 1) ^ CRC_POLYNOMIAL_BIT_REVERSED : (crc >> 1);
      }
      entries[0][i] = crc;
    }
    for (uint32_t k = 0; k < 3; k++) {
      for (uint32_t i = 0; i < 256; i++) {
        uint32_t v = entries[k][i];
        entries[k + 1][i] = (v >> 8) ^ entries[0][v & 0xFF];
      }
    }
  }
};

} // namespace

template <uint32_t CRC_POLYNOMIAL_BIT_REVERSED>
uint32_t crc_sw(const uint8_t* data, size_t nbytes, uint32_t startingChecksum) {
  static constexpr CrcSwTable<CRC_POLYNOMIAL_BIT_REVERSED> table{};
  uint32_t crc = startingChecksum;
  // Process 4 bytes per iteration using the slice-by-4 algorithm. This is
  // mathematically equivalent to four byte-at-a-time CRC steps, by linearity
  // of CRC over GF(2).
  while (nbytes >= 4) {
    uint8_t b0 = data[0] ^ static_cast<uint8_t>(crc);
    uint8_t b1 = data[1] ^ static_cast<uint8_t>(crc >> 8);
    uint8_t b2 = data[2] ^ static_cast<uint8_t>(crc >> 16);
    uint8_t b3 = data[3] ^ static_cast<uint8_t>(crc >> 24);
    crc = table.entries[3][b0] ^ table.entries[2][b1] ^ table.entries[1][b2] ^
        table.entries[0][b3];
    data += 4;
    nbytes -= 4;
  }
  // Process any remaining bytes (0 to 3) one at a time.
  for (size_t i = 0; i < nbytes; i++) {
    crc = table.entries[0][(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc;
}

uint32_t crc32c_sw(
    const uint8_t* data, size_t nbytes, uint32_t startingChecksum) {
  // Bit-reversed from CRC-32C polynomial 0x1EDC6F41
  constexpr uint32_t CRC32C_POLYNOMIAL_BIT_REVERSED = 0x82F63B78;
  return crc_sw<CRC32C_POLYNOMIAL_BIT_REVERSED>(data, nbytes, startingChecksum);
}

uint32_t crc32_sw(
    const uint8_t* data, size_t nbytes, uint32_t startingChecksum) {
  // Bit-reversed from CRC-32 polynomial 0x04C11DB7
  constexpr uint32_t CRC32_POLYNOMIAL_BIT_REVERSED = 0xEDB88320;
  return crc_sw<CRC32_POLYNOMIAL_BIT_REVERSED>(data, nbytes, startingChecksum);
}

} // namespace detail

namespace {
#if FOLLY_X64
// The fast-crc32 generated kernels are tuned for Intel. Above 4096 bytes they
// beat crc32c_hw on Xeon (1.10x at 8KiB, 1.63x at 1MiB) and lose to it on every
// AMD part measured, at every size from 4KiB to 4MiB (0.70x-0.85x on Zen 3).
// Vendor and size are one decision, so they are answered in one place.
bool crc32c_prefer_generated(size_t nbytes) {
  // Size first: most callers are below the threshold and this way they never
  // touch the guard variable.
  if (nbytes <= 4096) {
    return false;
  }
  static const bool isIntel = folly::CpuId().vendor_intel();
  return isIntel;
}
#endif
} // namespace

uint32_t crc32c(const uint8_t* data, size_t nbytes, uint32_t startingChecksum) {
#if FOLLY_X64
  // available() is a cross-library call that does not inline, so it goes last:
  // a caller below the threshold must not pay for it.
  if (crc32c_prefer_generated(nbytes) && detail::crc32c_hw_supported_avx512() &&
      detail::avx512_crc32c_v8s3x4_available()) {
    return detail::avx512_crc32c_v8s3x4(data, nbytes, startingChecksum);
  }
#endif

#if FOLLY_AARCH64
  if (detail::crc32c_hw_supported_neon_eor3_sha3()) {
    if (nbytes < 1536) {
      return detail::neon_eor3_crc32c_small(data, nbytes, startingChecksum);
    } else {
      return detail::neon_eor3_crc32c_v8s2x4e_s2x1(
          data, nbytes, startingChecksum);
    }
  }

  if (nbytes >= 4096 && detail::crc32c_hw_supported_neon()) {
    return detail::neon_crc32c_v3s4x2e_v2(data, nbytes, startingChecksum);
  }
#endif

  if (detail::crc32c_hw_supported()) {
#if FOLLY_X64 && defined(FOLLY_ENABLE_SSE42_CRC32C_V8S3X3)
    if (crc32c_prefer_generated(nbytes)) {
      return detail::sse_crc32c_v8s3x3(data, nbytes, startingChecksum);
    }
#endif
    return detail::crc32c_hw(data, nbytes, startingChecksum);
  } else {
    return detail::crc32c_sw(data, nbytes, startingChecksum);
  }
}

uint32_t crc32(const uint8_t* data, size_t nbytes, uint32_t startingChecksum) {
#if FOLLY_AARCH64
  if (detail::crc32_hw_supported_neon_eor3_sha3()) {
    if (nbytes < 1536) {
      return detail::neon_eor3_crc32_small(data, nbytes, startingChecksum);
    } else {
      return detail::neon_eor3_crc32_v8s2x4e_s1x2(
          data, nbytes, startingChecksum);
    }
  }
#endif

  if (detail::crc32_hw_supported()) {
    return detail::crc32_hw(data, nbytes, startingChecksum);
  } else {
    return detail::crc32_sw(data, nbytes, startingChecksum);
  }
}

uint32_t crc32_type(
    const uint8_t* data, size_t nbytes, uint32_t startingChecksum) {
  return ~crc32(data, nbytes, startingChecksum);
}

uint32_t crc32_combine(uint32_t crc1, uint32_t crc2, size_t crc2len) {
  // Append up to 32 bits of zeroes in the normal way
  uint8_t data[4] = {0, 0, 0, 0};
  auto len = crc2len & 3;
  if (len) {
    crc1 = crc32(data, len, crc1);
  }

  if (detail::crc32_hw_supported()) {
    return detail::crc32_combine_hw(crc1, crc2, crc2len);
  } else {
    return detail::crc32_combine_sw(crc1, crc2, crc2len);
  }
}

uint32_t crc32c_combine(uint32_t crc1, uint32_t crc2, size_t crc2len) {
  // Append up to 32 bits of zeroes in the normal way
  uint8_t data[4] = {0, 0, 0, 0};
  auto len = crc2len & 3;
  if (len) {
    crc1 = crc32c(data, len, crc1);
  }

  if (detail::crc32c_hw_supported()) {
    return detail::crc32c_combine_hw(crc1, crc2, crc2len - len);
  } else {
    return detail::crc32c_combine_sw(crc1, crc2, crc2len - len);
  }
}

uint32_t crc32c_combine_seed(
    uint32_t crc1, uint32_t crc2, size_t crc2len, uint32_t startingChecksum) {
  if (startingChecksum == 0U) {
    return crc32c_combine(crc1, crc2, crc2len);
  }

  crc1 ^= startingChecksum;
  crc2 ^= startingChecksum;

  // Append up to 32 bits of zeroes in the normal way
  uint8_t data[4] = {0, 0, 0, 0};
  auto len = crc2len & 3;
  if (len) {
    crc1 = crc32c(data, len, crc1);
  }

  auto result = detail::crc32c_hw_supported()
      ? detail::crc32c_combine_hw(crc1, crc2, crc2len - len)
      : detail::crc32c_combine_sw(crc1, crc2, crc2len - len);
  result ^= startingChecksum;
  return result;
}

} // namespace folly
