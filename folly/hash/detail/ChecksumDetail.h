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

#include <folly/Portability.h>

#if FOLLY_X64 && FOLLY_SSE_PREREQ(4, 2)
#include <immintrin.h>
#endif

#include <stdint.h>

#include <cstddef>

namespace folly {
namespace detail {

/**
 * Compute a CRC-32C checksum of a buffer using a hardware-accelerated
 * implementation.
 *
 * @note This function is exposed to support special cases where the
 *       calling code is absolutely certain it ought to invoke a hardware-
 *       accelerated CRC-32C implementation - unit tests, for example.  For
 *       all other scenarios, please call crc32c() and let it pick an
 *       implementation based on the capabilities of the underlying CPU.
 */
uint32_t crc32c_hw(
    const uint8_t* data, size_t nbytes, uint32_t startingChecksum = ~0U);

/**
 * Check whether a SSE4.2 hardware-accelerated CRC-32C implementation is
 * supported on the current CPU.
 */
bool crc32c_hw_supported_sse42();

/**
 * Check whether a hardware-accelerated CRC-32C implementation is
 * supported on the current CPU.
 */
bool crc32c_hw_supported();

/**
 * Check whether an AVX512VL hardware-accelerated CRC-32C implementation is
 * supported on the current CPU.
 */
bool crc32c_hw_supported_avx512();

/**
 * Check whether a NEON hardware-accelerated CRC-32C implementation is
 * supported on the current CPU.
 */
bool crc32c_hw_supported_neon();

/**
 * Check whether a NEON+EOR3+SHA3 hardware-accelerated CRC-32C implementation
 * is supported on the current CPU.
 */
bool crc32c_hw_supported_neon_eor3_sha3();

/**
 * Compute a CRC-32C checksum of a buffer using a portable,
 * software-only implementation.
 *
 * @note This function is exposed to support special cases where the
 *       calling code is absolutely certain it wants to use the software
 *       implementation instead of the hardware-accelerated code - unit
 *       tests, for example.  For all other scenarios, please call crc32c()
 *       and let it pick an implementation based on the capabilities of
 *       the underlying CPU.
 */
uint32_t crc32c_sw(
    const uint8_t* data, size_t nbytes, uint32_t startingChecksum = ~0U);

/**
 * Compute a CRC-32 checksum of a buffer using a hardware-accelerated
 * implementation.
 *
 * @note This function is exposed to support special cases where the
 *       calling code is absolutely certain it ought to invoke a hardware-
 *       accelerated CRC-32 implementation - unit tests, for example.  For
 *       all other scenarios, please call crc32() and let it pick an
 *       implementation based on the capabilities of the underlying CPU.
 */
uint32_t crc32_hw(
    const uint8_t* data, size_t nbytes, uint32_t startingChecksum = ~0U);

/**
 * Whether a call of this size takes a generated kernel instead of scalar
 * crc32c_hw. The generated kernels are tuned for Intel and lose to crc32c_hw
 * on AMD, so only Intel takes them, and only above 4096 bytes.
 *
 * Split out from crc32c() and kept free of any CPU query so the choice can be
 * tested on any host, which a runtime dispatch cannot be: every kernel returns
 * the same checksum, so comparing output says nothing about which one ran.
 */
inline constexpr size_t kCrc32cGeneratedMinBytes = 4096;

constexpr bool crc32c_generated_for(size_t nbytes, bool vendorIntel) {
  return vendorIntel && nbytes > kCrc32cGeneratedMinBytes;
}

/**
 * Smallest call that takes one of the wider folds. The two numbers differ
 * because the kernel being replaced differs. On AMD a call this size runs
 * scalar crc32c_hw, which the wider fold beats from 4096 up. On Intel it runs
 * avx512_crc32c_v8s3x4, which is still ahead on a buffer that has fallen out
 * of cache until about 6144.
 */
inline constexpr size_t kCrc32cWideMinBytesAmd = 4096;
inline constexpr size_t kCrc32cWideMinBytesIntel = 6144;

enum class Crc32cWideFold {
  kNone,
  kWide512,
  kWide256Avx512,
  kWide256Avx2,
};

/**
 * Which wider fold a call should use. Split out from crc32c() and kept free
 * of any CPU query so the choice can be tested on any host, which a runtime
 * dispatch cannot be: every fold returns the same checksum, so comparing
 * output says nothing about which one ran.
 */
constexpr Crc32cWideFold crc32c_wide_fold_for(
    size_t nbytes,
    bool wide512Usable,
    bool wide256Avx512Usable,
    bool avx2Usable,
    bool vendorIntel) {
  const size_t least =
      vendorIntel ? kCrc32cWideMinBytesIntel : kCrc32cWideMinBytesAmd;
  if (nbytes < least) {
    return Crc32cWideFold::kNone;
  }
  // A part whose own AVX-512 fold is refused takes the AVX2 fold, not the
  // other AVX-512 one.
  if (vendorIntel ? wide256Avx512Usable : wide512Usable) {
    return vendorIntel
        ? Crc32cWideFold::kWide256Avx512
        : Crc32cWideFold::kWide512;
  }
  return avx2Usable ? Crc32cWideFold::kWide256Avx2 : Crc32cWideFold::kNone;
}

/**
 * Shortest call the 512-bit fold takes, in 16-byte vectors. The loop needs
 * eight. Both floors are higher, and set where the fold is measured to win
 * rather than where it merely runs.
 *
 * On AMD that is 64 vectors: below 32 the two parts measured disagree about
 * the sign and one loses about 4%. Intel gains nothing until 256 vectors,
 * so it waits.
 */
inline constexpr size_t kCrc32Vpclmul512MinVectorsAmd = 64;
inline constexpr size_t kCrc32Vpclmul512MinVectorsIntel = 256;
inline constexpr size_t kCrc32Vpclmul512MinVectors =
    kCrc32Vpclmul512MinVectorsAmd < kCrc32Vpclmul512MinVectorsIntel
    ? kCrc32Vpclmul512MinVectorsAmd
    : kCrc32Vpclmul512MinVectorsIntel;

enum class Crc32Fold {
  kScalar,
  kVpclmul256,
  kVpclmul512,
};

/**
 * Which fold a call of this many 16-byte vectors gets. Split out from
 * crc32_hw() and kept free of any CPU query so the choice can be tested on
 * any host, which a runtime dispatch cannot be: every fold returns the same
 * checksum, so comparing output says nothing about which one ran.
 *
 * A call below the floor still takes the widest fold the part can run. The
 * floor chooses between the two widest folds, and never drops a call two
 * widths at once.
 */
constexpr Crc32Fold crc32_fold_for(
    size_t vec_count,
    bool vpclmul512Usable,
    bool vpclmulUsable,
    bool vendorIntel) {
  const Crc32Fold narrower =
      vpclmulUsable ? Crc32Fold::kVpclmul256 : Crc32Fold::kScalar;
  if (vec_count < kCrc32Vpclmul512MinVectors) {
    return narrower;
  }
  const size_t least = vendorIntel
      ? kCrc32Vpclmul512MinVectorsIntel
      : kCrc32Vpclmul512MinVectorsAmd;
  return vpclmul512Usable && vec_count >= least
      ? Crc32Fold::kVpclmul512
      : narrower;
}

/** CpuId().vendor_intel(), read once. */
bool vendor_intel();

#if FOLLY_X64 && FOLLY_SSE_PREREQ(4, 2)
uint32_t crc32_hw_aligned(
    uint32_t remainder, const __m128i* p, size_t vec_count);

// MSVC cannot apply a target attribute to a function, so
// FOLLY_TARGET_ATTRIBUTE expands to nothing and the VPCLMULQDQ intrinsics fail
// to compile rather than being gated. Compile the fold out there.
#if defined(_MSC_VER)
#define FOLLY_HAS_CRC32_VPCLMUL 0
#else
#define FOLLY_HAS_CRC32_VPCLMUL 1
#endif

#if FOLLY_HAS_CRC32_VPCLMUL
/**
 * Whether the 256-bit VPCLMULQDQ fold below can be used on this CPU.
 *
 * Deliberately a function rather than a preprocessor test: the kernel carries
 * its own target attribute, so whether it is usable depends on the CPU, never
 * on the ISA flags of whichever translation unit happens to include this.
 */
bool crc32_vpclmul_usable();

/** crc32_hw_aligned, folding 256 bits at a time. Bit-identical output. */
uint32_t crc32_hw_aligned_vpclmul(
    uint32_t remainder, const __m128i* p, size_t vec_count);

/** Whether the 512-bit fold below can be used on this CPU. */
bool crc32_vpclmul512_usable();

/** crc32_hw_aligned, folding 512 bits at a time. Bit-identical output. */
uint32_t crc32_hw_aligned_vpclmul512(
    uint32_t remainder, const __m128i* p, size_t vec_count);

/**
 * Whether the wider CRC-32C folds below can be used on this CPU. The two
 * AVX-512 folds have the same CPU precondition, so one test covers both. The
 * `_avx2` fold is the one a part with VPCLMULQDQ and no AVX-512 can still run.
 */
bool crc32c_wide_avx512_usable();
bool crc32c_wide256_avx2_usable();

/**
 * crc32c_wide_avx512_usable(), and the fold's own entry is not trapped. BOLT
 * can trap one AVX-512 fold and leave the other, so each is checked alone.
 */
bool crc32c_wide512_usable();
bool crc32c_wide256_avx512_usable();

/**
 * avx512_crc32c_v8s3x4, folding 512 or 256 bits at a time. Bit-identical
 * output.
 */
uint32_t crc32c_wide512(const uint8_t* buf, size_t len, uint32_t crc0);
uint32_t crc32c_wide256_avx512(const uint8_t* buf, size_t len, uint32_t crc0);
uint32_t crc32c_wide256_avx2(const uint8_t* buf, size_t len, uint32_t crc0);
#endif
#else
#define FOLLY_HAS_CRC32_VPCLMUL 0
#endif

/**
 * Check whether a hardware-accelerated CRC-32 implementation is
 * supported on the current CPU.
 */
bool crc32_hw_supported();

/**
 * Compute a CRC-32 checksum of a buffer using a portable,
 * software-only implementation.
 *
 * @note This function is exposed to support special cases where the
 *       calling code is absolutely certain it wants to use the software
 *       implementation instead of the hardware-accelerated code - unit
 *       tests, for example.  For all other scenarios, please call crc32()
 *       and let it pick an implementation based on the capabilities of
 *       the underlying CPU.
 */
uint32_t crc32_sw(
    const uint8_t* data, size_t nbytes, uint32_t startingChecksum = ~0U);

/* See Checksum.h for details.
 *
 * crc2len *must* be a power of two >= 4.
 */
uint32_t crc32_combine_sw(uint32_t crc1, uint32_t crc2, size_t crc2len);
uint32_t crc32_combine_hw(uint32_t crc1, uint32_t crc2, size_t crc2len);
uint32_t crc32c_combine_sw(uint32_t crc1, uint32_t crc2, size_t crc2len);
uint32_t crc32c_combine_hw(uint32_t crc1, uint32_t crc2, size_t crc2len);

} // namespace detail
} // namespace folly
