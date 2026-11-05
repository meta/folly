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

#include <folly/Benchmark.h>
#include <folly/CpuId.h>
#include <folly/Portability.h>
#include <folly/Random.h>
#include <folly/external/fast-crc32/avx512_crc32c_v8s3x4.h>
#include <folly/external/fast-crc32/neon_crc32c_v3s4x2e_v2.h>
#include <folly/external/fast-crc32/neon_eor3_crc32c_v8s2x4e_s2x1.h>
#include <folly/external/fast-crc32/sse_crc32c_v8s3x3.h>
#include <folly/hash/Hash.h>
#include <folly/hash/detail/ChecksumDetail.h>
#include <folly/portability/GFlags.h>
#include <folly/portability/GTest.h>

namespace {
const unsigned int BUFFER_SIZE = 512 * 1024 * sizeof(uint64_t);
uint8_t buffer[BUFFER_SIZE];

struct ExpectedResult {
  size_t offset;
  size_t length;
  uint32_t crc32c;
};

ExpectedResult expectedResults[] = {
    // Zero-byte input
    {0, 0, ~0U},
    // Small aligned inputs to test special cases in SIMD implementations
    {8, 1, 1543413366},
    {8, 2, 523493126},
    {8, 3, 1560427360},
    {8, 4, 3422504776},
    {8, 5, 447841138},
    {8, 6, 3910050499},
    {8, 7, 3346241981},
    // Small unaligned inputs
    {9, 1, 3855826643},
    {10, 2, 560880875},
    {11, 3, 1479707779},
    {12, 4, 2237687071},
    {13, 5, 4063855784},
    {14, 6, 2553454047},
    {15, 7, 1349220140},
    // Larger inputs to test leftover chunks at the end of aligned blocks
    {8, 8, 627613930},
    {8, 9, 2105929409},
    {8, 10, 2447068514},
    {8, 11, 863807079},
    {8, 12, 292050879},
    {8, 13, 1411837737},
    {8, 14, 2614515001},
    {8, 15, 3579076296},
    {8, 16, 2897079161},
    {8, 17, 675168386},
    // Much larger inputs
    {0, BUFFER_SIZE, 2096790750},
    {1, BUFFER_SIZE / 2, 3854797577},
};

void testCRC32C(
    std::function<uint32_t(const uint8_t*, size_t, uint32_t)> impl) {
  for (auto expected : expectedResults) {
    uint32_t result = impl(buffer + expected.offset, expected.length, ~0U);
    EXPECT_EQ(expected.crc32c, result);
  }
}

void testCRC32CContinuation(
    std::function<uint32_t(const uint8_t*, size_t, uint32_t)> impl) {
  for (auto expected : expectedResults) {
    size_t partialLength = expected.length / 2;
    uint32_t partialChecksum =
        impl(buffer + expected.offset, partialLength, ~0U);
    uint32_t result = impl(
        buffer + expected.offset + partialLength,
        expected.length - partialLength,
        partialChecksum);
    EXPECT_EQ(expected.crc32c, result);
  }
}

// crc32c_hw_supported_avx512() asks about the CPU. Whether this build linked
// the real kernel or the abort() stub is a separate question, and only the
// kernel's own library can answer it.
bool avx512CrcUsable() {
  return folly::detail::crc32c_hw_supported_avx512() &&
      folly::detail::avx512_crc32c_v8s3x4_available();
}

// Bitwise CRC-32 (the zlib and boost::crc_32_type parameters).
uint32_t referenceCrc32(const uint8_t* data, size_t length) {
  uint32_t crc = ~0U;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1)));
    }
  }
  return ~crc;
}

void testMatchesReference32Type() {
  const auto check = reinterpret_cast<const uint8_t*>("123456789");
  EXPECT_EQ(0xCBF43926U, referenceCrc32(check, 9));
  EXPECT_EQ(0xCBF43926U, folly::crc32_type(check, 9));
  for (auto expected : expectedResults) {
    const uint32_t referenceResult =
        referenceCrc32(buffer + expected.offset, expected.length);
    const uint32_t follyResult =
        folly::crc32_type(buffer + expected.offset, expected.length);
    EXPECT_EQ(follyResult, referenceResult);
  }
}

} // namespace

TEST(Checksum, crc32cSoftware) {
  testCRC32C(folly::detail::crc32c_sw);
}

TEST(Checksum, crc32cContinuationSoftware) {
  testCRC32CContinuation(folly::detail::crc32c_sw);
}

TEST(Checksum, crc32cHardware) {
  if (folly::detail::crc32c_hw_supported()) {
    testCRC32C(folly::detail::crc32c_hw);
  } else {
#if FOLLY_X64
    LOG(WARNING) << "skipping hardware-accelerated CRC-32C tests"
                 << " (not supported on this CPU)";
#endif
  }
}

TEST(Checksum, crc32cHardwareEq) {
  if (folly::detail::crc32c_hw_supported()) {
    for (int i = 0; i < 1000; i++) {
      auto sw = folly::detail::crc32c_sw(buffer, i, 0);
      auto hw = folly::detail::crc32c_hw(buffer, i, 0);
      EXPECT_EQ(sw, hw);
    }
  } else {
#if FOLLY_X64
    LOG(WARNING) << "skipping hardware-accelerated CRC-32C tests"
                 << " (not supported on this CPU)";
#endif
  }
}

// crc32c_hw dispatches on the number of 24-byte triplets in the first block
// (1 to 128) and on the number of trailing 8-byte words (0 to 27), each
// through a switch with one case per count. Cover every case at every
// alignment.
TEST(Checksum, crc32cHardwareEqAllResidues) {
  if (!folly::detail::crc32c_hw_supported()) {
    return;
  }
  constexpr size_t kMaxLength = 216 + 24 * 128 + 8 * 28 + 8;
  for (size_t offset = 0; offset < 8; ++offset) {
    for (size_t len = 0; len <= kMaxLength; ++len) {
      auto sw = folly::detail::crc32c_sw(buffer + offset, len, 0);
      auto hw = folly::detail::crc32c_hw(buffer + offset, len, 0);
      ASSERT_EQ(sw, hw) << "offset " << offset << ", length " << len;
    }
  }
}

TEST(Checksum, crc32cContinuationHardware) {
  if (folly::detail::crc32c_hw_supported()) {
    testCRC32CContinuation(folly::detail::crc32c_hw);
  } else {
#if FOLLY_X64
    LOG(WARNING) << "skipping hardware-accelerated CRC-32C tests"
                 << " (not supported on this CPU)";
#endif
  }
}

TEST(Checksum, crc32cHardwareSse42) {
  if (folly::detail::crc32c_hw_supported_sse42()) {
    testCRC32C(folly::detail::sse_crc32c_v8s3x3);
  } else {
#if FOLLY_X64
    LOG(WARNING) << "skipping SSE4.2 hardware-accelerated CRC-32C tests"
                 << " (not supported on this CPU)";
#endif
  }
}

TEST(Checksum, crc32cHardwareEqSse42) {
  if (folly::detail::crc32c_hw_supported_sse42()) {
    for (size_t i = 0; i < 1000; i++) {
      auto sw = folly::detail::crc32c_sw(buffer, i, 0);
      auto hw = folly::detail::sse_crc32c_v8s3x3(buffer, i, 0);
      ASSERT_EQ(sw, hw);
    }
  } else {
#if FOLLY_X64
    LOG(WARNING) << "skipping SSE4.2 hardware-accelerated CRC-32C tests"
                 << " (not supported on this CPU)";
#endif
  }
}

TEST(Checksum, crc32cContinuationHardwareSse42) {
  if (folly::detail::crc32c_hw_supported_sse42()) {
    testCRC32CContinuation(folly::detail::sse_crc32c_v8s3x3);
  } else {
#if FOLLY_X64
    LOG(WARNING) << "skipping SSE4.2 hardware-accelerated CRC-32C tests"
                 << " (not supported on this CPU)";
#endif
  }
}

// This binary is not BOLTed, so neither the probe nor the kernel's entry is
// trapped. A build with FOLLY_DISABLE_AVX512, such as
// -c fbcode.hhvm_tuning_flags=true, never picks the kernel. Otherwise the CPU
// alone decides.
TEST(Checksum, crc32cHardwareAvx512Guard) {
#if FOLLY_DISABLE_AVX512
  EXPECT_FALSE(folly::detail::crc32c_hw_supported_avx512());
#else
  EXPECT_EQ(
      folly::kIsArchAmd64 && folly::CpuId().avx512vl(),
      folly::detail::crc32c_hw_supported_avx512());
#endif
}

TEST(Checksum, crc32cHardwareAvx512) {
  if (avx512CrcUsable()) {
    testCRC32C(folly::detail::avx512_crc32c_v8s3x4);
  } else {
#if FOLLY_X64
    LOG(WARNING) << "skipping AVX512 hardware-accelerated CRC-32C tests"
                 << " (not supported on this CPU)";
#endif
  }
}

TEST(Checksum, crc32cHardwareEqAvx512) {
  if (avx512CrcUsable()) {
    for (size_t i = 0; i < 1000; i++) {
      auto sw = folly::detail::crc32c_sw(buffer, i, 0);
      auto hw = folly::detail::avx512_crc32c_v8s3x4(buffer, i, 0);
      ASSERT_EQ(sw, hw);
    }
  } else {
#if FOLLY_X64
    LOG(WARNING) << "skipping AVX512 hardware-accelerated CRC-32C tests"
                 << " (not supported on this CPU)";
#endif
  }
}

TEST(Checksum, crc32cContinuationHardwareAvx512) {
  if (avx512CrcUsable()) {
    testCRC32CContinuation(folly::detail::avx512_crc32c_v8s3x4);
  } else {
#if FOLLY_X64
    LOG(WARNING) << "skipping AVX512 hardware-accelerated CRC-32C tests"
                 << " (not supported on this CPU)";
#endif
  }
}

TEST(Checksum, crc32cHardwareNeon) {
  if (folly::detail::crc32c_hw_supported_neon()) {
    testCRC32C(folly::detail::neon_crc32c_v3s4x2e_v2);
  } else {
#if FOLLY_AARCH64
    LOG(WARNING) << "skipping NEON hardware-accelerated CRC-32C tests"
                 << " (not supported on this CPU)";
#endif
  }
}

TEST(Checksum, crc32cHardwareEqNeon) {
  if (folly::detail::crc32c_hw_supported_neon()) {
    for (size_t i = 0; i < 1000; i++) {
      auto sw = folly::detail::crc32c_sw(buffer, i, 0);
      auto hw = folly::detail::neon_crc32c_v3s4x2e_v2(buffer, i, 0);
      ASSERT_EQ(sw, hw);
    }
  } else {
#if FOLLY_AARCH64
    LOG(WARNING) << "skipping NEON hardware-accelerated CRC-32C tests"
                 << " (not supported on this CPU)";
#endif
  }
}

TEST(Checksum, crc32cContinuationHardwareNeon) {
  if (folly::detail::crc32c_hw_supported_neon()) {
    testCRC32CContinuation(folly::detail::neon_crc32c_v3s4x2e_v2);
  } else {
#if FOLLY_AARCH64
    LOG(WARNING) << "skipping NEON hardware-accelerated CRC-32C tests"
                 << " (not supported on this CPU)";
#endif
  }
}

TEST(Checksum, crc32cHardwareNeonEor3Sha3) {
  if (folly::detail::crc32c_hw_supported_neon_eor3_sha3()) {
    testCRC32C(folly::detail::neon_eor3_crc32c_v8s2x4e_s2x1);
  } else {
#if FOLLY_AARCH64
    LOG(WARNING) << "skipping NEON+EOR3+SHA3 hardware-accelerated CRC-32C tests"
                 << " (not supported on this CPU)";
#endif
  }
}

TEST(Checksum, crc32cHardwareEqNeonEor3Sha3) {
  if (folly::detail::crc32c_hw_supported_neon_eor3_sha3()) {
    for (size_t i = 0; i < 1000; i++) {
      auto sw = folly::detail::crc32c_sw(buffer, i, 0);
      auto hw = folly::detail::neon_eor3_crc32c_v8s2x4e_s2x1(buffer, i, 0);
      ASSERT_EQ(sw, hw);
    }
  } else {
#if FOLLY_AARCH64
    LOG(WARNING) << "skipping NEON+EOR3+SHA3 hardware-accelerated CRC-32C tests"
                 << " (not supported on this CPU)";
#endif
  }
}

TEST(Checksum, crc32cContinuationHardwareNeonEor3Sha3) {
  if (folly::detail::crc32c_hw_supported_neon_eor3_sha3()) {
    testCRC32CContinuation(folly::detail::neon_eor3_crc32c_v8s2x4e_s2x1);
  } else {
#if FOLLY_AARCH64
    LOG(WARNING) << "skipping NEON+EOR3+SHA3 hardware-accelerated CRC-32C tests"
                 << " (not supported on this CPU)";
#endif
  }
}

// Test on very large buffer inputs to attempt to sanity check 32-bit
// overflow problems on 64-bit platforms.
#ifdef __LP64__
TEST(Checksum, crc32clargeBuffers) {
  constexpr size_t kLargeBufSz = 5ull * 1024 * 1024 * 1024; // 5GiB
  auto buf = std::make_unique<uint8_t[]>(kLargeBufSz); // 5GiB
  auto* bufp = buf.get();
  // Fill with non-zero pattern.
  memset(bufp, 0x2e, kLargeBufSz);

  constexpr uint32_t kCrc = 2860399007;

  if (folly::detail::crc32c_hw_supported_sse42()) {
    auto crcSse42 = folly::detail::sse_crc32c_v8s3x3(bufp, kLargeBufSz, ~0);
    ASSERT_EQ(kCrc, crcSse42);
    auto crcHw = folly::detail::crc32c_hw(bufp, kLargeBufSz, ~0);
    ASSERT_EQ(kCrc, crcHw);
  }
  if (avx512CrcUsable()) {
    auto crcAvx = folly::detail::avx512_crc32c_v8s3x4(bufp, kLargeBufSz, ~0);
    ASSERT_EQ(kCrc, crcAvx);
  }
  if (folly::detail::crc32c_hw_supported_neon()) {
    auto crcHw = folly::detail::neon_crc32c_v3s4x2e_v2(bufp, kLargeBufSz, ~0);
    ASSERT_EQ(kCrc, crcHw);
  }
  if (folly::detail::crc32c_hw_supported_neon_eor3_sha3()) {
    auto crcHw =
        folly::detail::neon_eor3_crc32c_v8s2x4e_s2x1(bufp, kLargeBufSz, ~0);
    ASSERT_EQ(kCrc, crcHw);
  }
#if FOLLY_HAS_CRC32_VPCLMUL
  // The generic CRC-32 folds take a vector count rather than a byte count, so
  // they overflow somewhere else again; this buffer is 2^38 bits.
  if (folly::detail::crc32_vpclmul_usable()) {
    const size_t head = -reinterpret_cast<uintptr_t>(bufp) & 15;
    const size_t vecCount = (kLargeBufSz - head) / 16;
    const auto* p = reinterpret_cast<const __m128i*>(bufp + head);
    ASSERT_EQ(
        folly::detail::crc32_hw_aligned(~0U, p, vecCount),
        folly::detail::crc32_hw_aligned_vpclmul(~0U, p, vecCount));
  }
#endif
}
#endif

TEST(Checksum, crc32cAutodetect) {
  testCRC32C(folly::crc32c);
}

TEST(Checksum, crc32cContinuationAutodetect) {
  testCRC32CContinuation(folly::crc32c);
}

// crc32c() selects between crc32c_hw and the generated kernels on a 4096-byte
// threshold, so the two sides of it run different code. expectedResults jumps
// from 17 bytes straight to 2MiB, and which kernel the threshold selects is
// vendor-dependent, so the generated ones are also tested directly rather than
// only through whichever crc32c() happens to reach on the host running this.
TEST(Checksum, crc32cEqAcrossDispatchThreshold) {
  constexpr size_t kOffsets[] = {0, 1, 8, 15};
  for (uint32_t startingChecksum : {0U, ~0U}) {
    for (size_t offset : kOffsets) {
      for (size_t length = 4080; length <= 4200; length++) {
        SCOPED_TRACE(
            testing::Message()
            << "startingChecksum=" << startingChecksum << " offset=" << offset
            << " length=" << length);
        const uint8_t* data = buffer + offset;
        const uint32_t sw =
            folly::detail::crc32c_sw(data, length, startingChecksum);
        ASSERT_EQ(sw, folly::crc32c(data, length, startingChecksum));
        if (folly::detail::crc32c_hw_supported_sse42()) {
          ASSERT_EQ(
              sw,
              folly::detail::sse_crc32c_v8s3x3(data, length, startingChecksum));
        }
        if (avx512CrcUsable()) {
          ASSERT_EQ(
              sw,
              folly::detail::avx512_crc32c_v8s3x4(
                  data, length, startingChecksum));
        }
      }
    }
  }
}

TEST(Checksum, crc32) {
  if (folly::detail::crc32_hw_supported()) {
    // Just check that sw and hw match
    for (auto expected : expectedResults) {
      uint32_t sw_res =
          folly::detail::crc32_sw(buffer + expected.offset, expected.length, 0);
      uint32_t hw_res =
          folly::detail::crc32_hw(buffer + expected.offset, expected.length, 0);
      EXPECT_EQ(sw_res, hw_res);
    }
  } else {
    LOG(WARNING) << "skipping hardware-accelerated CRC-32 tests"
                 << " (not supported on this CPU)";
  }
}

TEST(Checksum, crc32Continuation) {
  if (folly::detail::crc32_hw_supported()) {
    // Just check that sw and hw match
    for (auto expected : expectedResults) {
      auto halflen = expected.length / 2;
      uint32_t sw_res =
          folly::detail::crc32_sw(buffer + expected.offset, halflen, 0);
      sw_res = folly::detail::crc32_sw(
          buffer + expected.offset + halflen, halflen, sw_res);
      uint32_t hw_res =
          folly::detail::crc32_hw(buffer + expected.offset, halflen, 0);
      hw_res = folly::detail::crc32_hw(
          buffer + expected.offset + halflen, halflen, hw_res);
      EXPECT_EQ(sw_res, hw_res);
      uint32_t sw_res2 =
          folly::detail::crc32_sw(buffer + expected.offset, halflen * 2, 0);
      EXPECT_EQ(sw_res, sw_res2);
      uint32_t hw_res2 =
          folly::detail::crc32_hw(buffer + expected.offset, halflen * 2, 0);
      EXPECT_EQ(hw_res, hw_res2);
    }
  } else {
    LOG(WARNING) << "skipping hardware-accelerated CRC-32 tests"
                 << " (not supported on this CPU)";
  }
}

// crc32_hw() checksums the unaligned head in software, then hands the rest to
// whichever fold the runtime guard selects. Neither the head nor the guard is
// reached by calling a fold directly, and expectedResults has only two entries
// long enough to reach the 128-byte-at-a-time loop at all.
TEST(Checksum, crc32HardwareEq) {
  if (!folly::detail::crc32_hw_supported()) {
    LOG(WARNING) << "skipping hardware-accelerated CRC-32 tests"
                 << " (not supported on this CPU)";
    return;
  }
  constexpr size_t kLargeLengths[] = {1024, 65536, BUFFER_SIZE / 2};
  for (uint32_t startingChecksum : {0U, ~0U, 0x89abcdefU}) {
    // Past 16 so the aligned body starts on a 32-byte boundary for half of
    // these and straddles one for the other half: the 256-bit fold loads
    // 32 bytes at a time from a pointer only guaranteed 16-byte aligned.
    for (size_t offset = 0; offset < 32; offset++) {
      auto check = [&](size_t length) {
        SCOPED_TRACE(
            testing::Message()
            << "startingChecksum=" << startingChecksum << " offset=" << offset
            << " length=" << length);
        ASSERT_EQ(
            folly::detail::crc32_sw(buffer + offset, length, startingChecksum),
            folly::detail::crc32_hw(buffer + offset, length, startingChecksum));
      };
      for (size_t length = 0; length <= 400; length++) {
        check(length);
      }
      for (size_t length : kLargeLengths) {
        check(length);
      }
    }
  }
}

#if FOLLY_HAS_CRC32_VPCLMUL
// The 256-bit fold has to be bit-identical to the 128-bit one it replaces.
// vec_count picks between delegating back to it, the 1024-bit loop, and the
// 512- and 128-bit tails; the remainder is folded into the first vector, so a
// non-zero one is what distinguishes that from dropping it.
TEST(Checksum, crc32HardwareAlignedVpclmulEq) {
  if (!folly::detail::crc32_vpclmul_usable()) {
    LOG(WARNING) << "skipping VPCLMULQDQ CRC-32 tests"
                 << " (not supported on this CPU)";
    return;
  }
  // The fold's callers promise 16-byte alignment and nothing more, so run it
  // both on and off a 32-byte boundary.
  const auto* aligned32 = reinterpret_cast<const __m128i*>(
      buffer + (-reinterpret_cast<uintptr_t>(buffer) & 31));
  // Straddling a multiple of 8 so the tails run at scale as well.
  constexpr size_t kLongVecCounts[] = {
      BUFFER_SIZE / 32 - 1, BUFFER_SIZE / 32, BUFFER_SIZE / 32 + 1};
  for (const __m128i* p : {aligned32, aligned32 + 1}) {
    for (uint32_t remainder : {0U, ~0U, 0x89abcdefU}) {
      auto check = [&](size_t vecCount) {
        SCOPED_TRACE(
            testing::Message()
            << "aligned32=" << (p == aligned32) << " remainder=" << remainder
            << " vecCount=" << vecCount);
        ASSERT_EQ(
            folly::detail::crc32_hw_aligned(remainder, p, vecCount),
            folly::detail::crc32_hw_aligned_vpclmul(remainder, p, vecCount));
      };
      for (size_t vecCount = 0; vecCount <= 40; vecCount++) {
        check(vecCount);
      }
      for (size_t vecCount : kLongVecCounts) {
        check(vecCount);
      }
    }
  }
}
#endif

// The static buffer above is far too large for a read past the end of the
// input to land anywhere it would be noticed. These are sized exactly, so
// under ASAN a fold that over-reads faults instead of returning the right
// answer by accident. Lengths straddle the 16-byte vector boundary and the
// 128-byte point where the 256-bit fold takes over from the 128-bit one.
TEST(Checksum, crc32HardwareExactSizedBuffer) {
  if (!folly::detail::crc32_hw_supported()) {
    LOG(WARNING) << "skipping hardware-accelerated CRC-32 tests"
                 << " (not supported on this CPU)";
    return;
  }
  constexpr size_t kLengths[] = {
      1, 15, 16, 17, 31, 32, 127, 128, 129, 143, 144, 145, 255, 256, 257, 4097};
  for (size_t length : kLengths) {
    SCOPED_TRACE(testing::Message() << "length=" << length);
    auto data = std::make_unique<uint8_t[]>(length);
    memcpy(data.get(), buffer, length);
    EXPECT_EQ(
        folly::detail::crc32_sw(data.get(), length, ~0U),
        folly::detail::crc32_hw(data.get(), length, ~0U));
  }
}

TEST(Checksum, crc32Type) {
  testMatchesReference32Type();
}

TEST(Checksum, crc32Combine) {
  for (size_t totlen = 1024; totlen < BUFFER_SIZE; totlen += BUFFER_SIZE / 8) {
    auto mid = folly::Random::rand64(0, totlen);
    auto crc1 = folly::crc32(&buffer[0], mid, 0);
    auto crc2 = folly::crc32(&buffer[mid], totlen - mid, 0);
    auto crcfull = folly::crc32(&buffer[0], totlen, 0);
    auto combined = folly::crc32_combine(crc1, crc2, totlen - mid);
    EXPECT_EQ(combined, crcfull);
  }
}

TEST(Checksum, crc32cCombine) {
  for (size_t totlen = 1024; totlen < BUFFER_SIZE; totlen += BUFFER_SIZE / 8) {
    auto mid = folly::Random::rand64(0, totlen);
    auto crc1 = folly::crc32c(&buffer[0], mid, 0);
    auto crc2 = folly::crc32c(&buffer[mid], totlen - mid, 0);
    auto crcfull = folly::crc32c(&buffer[0], totlen, 0);
    auto combined = folly::crc32c_combine(crc1, crc2, totlen - mid);
    EXPECT_EQ(combined, crcfull);
  }
}

TEST(Checksum, crc32cCombineSeed_explicit_starting_checksum) {
  auto random32 = folly::Random::rand32();
  std::vector<uint32_t> startingChecksums = {0U, ~0U, random32};

  for (auto startingChecksum : startingChecksums) {
    for (size_t totlen = 1024; totlen < BUFFER_SIZE;
         totlen += BUFFER_SIZE / 8) {
      auto mid = folly::Random::rand64(0, totlen);
      auto crc1 = folly::crc32c(&buffer[0], mid, startingChecksum);
      auto crc2 = folly::crc32c(&buffer[mid], totlen - mid, startingChecksum);
      auto crcfull = folly::crc32c(&buffer[0], totlen, startingChecksum);
      auto combined = folly::crc32c_combine_seed(
          crc1, crc2, totlen - mid, startingChecksum);
      EXPECT_EQ(combined, crcfull)
          << "Failed for startingChecksum=" << startingChecksum
          << " totlen=" << totlen << " mid=" << mid;
    }
  }
}

void benchmarkHardwareCRC32C(unsigned long iters, size_t blockSize) {
  if (folly::detail::crc32c_hw_supported()) {
    uint32_t checksum;
    for (unsigned long i = 0; i < iters; i++) {
      checksum = folly::detail::crc32c_hw(buffer, blockSize);
      folly::doNotOptimizeAway(checksum);
    }
  } else {
    LOG(WARNING) << "skipping hardware-accelerated CRC-32C benchmarks"
                 << " (not supported on this CPU)";
  }
}

void benchmarkSoftwareCRC32C(unsigned long iters, size_t blockSize) {
  uint32_t checksum;
  for (unsigned long i = 0; i < iters; i++) {
    checksum = folly::detail::crc32c_sw(buffer, blockSize);
    folly::doNotOptimizeAway(checksum);
  }
}

void benchmarkHardwareCRC32(unsigned long iters, size_t blockSize) {
  if (folly::detail::crc32_hw_supported()) {
    uint32_t checksum;
    for (unsigned long i = 0; i < iters; i++) {
      checksum = folly::detail::crc32_hw(buffer, blockSize);
      folly::doNotOptimizeAway(checksum);
    }
  } else {
    LOG(WARNING) << "skipping hardware-accelerated CRC-32 benchmarks"
                 << " (not supported on this CPU)";
  }
}

void benchmarkSoftwareCRC32(unsigned long iters, size_t blockSize) {
  uint32_t checksum;
  for (unsigned long i = 0; i < iters; i++) {
    checksum = folly::detail::crc32_sw(buffer, blockSize);
    folly::doNotOptimizeAway(checksum);
  }
}

void benchmarkCombineHardwareCrc32(unsigned long iters, size_t blockSize) {
  // Arbitrarily chosen checksums
  uint32_t checksum1 = 0xEDB88320;
  uint32_t checksum2 = 0x82F63B78;
  uint32_t result;
  for (unsigned long i = 0; i < iters; i++) {
    result = folly::crc32_combine(checksum1, checksum2, blockSize);
    folly::doNotOptimizeAway(result);
  }
}

void benchmarkCombineSoftwareLinear(unsigned long iters, size_t blockSize) {
  // Arbitrarily chosen checksums
  std::vector<uint8_t> zbuffer;
  zbuffer.reserve(blockSize);
  memset(zbuffer.data(), 0, blockSize);
  uint32_t checksum1 = 0xEDB88320;
  uint32_t checksum2 = 0x82F63B78;
  uint32_t result;
  for (unsigned long i = 0; i < iters; i++) {
    result = folly::crc32c(zbuffer.data(), blockSize, checksum1);
    result ^= checksum2;
    folly::doNotOptimizeAway(result);
  }
}

void benchmarkCombineHardwareCrc32c(unsigned long iters, size_t blockSize) {
  // Arbitrarily chosen checksums
  uint32_t checksum1 = 0xEDB88320;
  uint32_t checksum2 = 0x82F63B78;
  uint32_t result;
  for (unsigned long i = 0; i < iters; i++) {
    result = folly::crc32c_combine(checksum1, checksum2, blockSize);
    folly::doNotOptimizeAway(result);
  }
}

// Small blocks where per-call overhead dominates over per-byte work.
BENCHMARK(crc32c_hardware_8B_block, iters) {
  benchmarkHardwareCRC32C(iters, 8);
}

BENCHMARK(crc32c_software_8B_block, iters) {
  benchmarkSoftwareCRC32C(iters, 8);
}

BENCHMARK(crc32_hardware_8B_block, iters) {
  benchmarkHardwareCRC32(iters, 8);
}

BENCHMARK(crc32_software_8B_block, iters) {
  benchmarkSoftwareCRC32(iters, 8);
}

BENCHMARK_DRAW_LINE();

BENCHMARK(crc32c_hardware_32B_block, iters) {
  benchmarkHardwareCRC32C(iters, 32);
}

BENCHMARK(crc32c_software_32B_block, iters) {
  benchmarkSoftwareCRC32C(iters, 32);
}

BENCHMARK(crc32_hardware_32B_block, iters) {
  benchmarkHardwareCRC32(iters, 32);
}

BENCHMARK(crc32_software_32B_block, iters) {
  benchmarkSoftwareCRC32(iters, 32);
}

BENCHMARK_DRAW_LINE();

BENCHMARK(crc32c_hardware_128B_block, iters) {
  benchmarkHardwareCRC32C(iters, 128);
}

BENCHMARK(crc32c_software_128B_block, iters) {
  benchmarkSoftwareCRC32C(iters, 128);
}

BENCHMARK(crc32_hardware_128B_block, iters) {
  benchmarkHardwareCRC32(iters, 128);
}

BENCHMARK(crc32_software_128B_block, iters) {
  benchmarkSoftwareCRC32(iters, 128);
}

BENCHMARK_DRAW_LINE();

// This test fits easily in the L1 cache on modern server processors,
// and thus it mainly measures the speed of the checksum computation.
BENCHMARK(crc32c_hardware_1KB_block, iters) {
  benchmarkHardwareCRC32C(iters, 1024);
}

BENCHMARK(crc32c_software_1KB_block, iters) {
  benchmarkSoftwareCRC32C(iters, 1024);
}

BENCHMARK(crc32_hardware_1KB_block, iters) {
  benchmarkHardwareCRC32(iters, 1024);
}

BENCHMARK(crc32_software_1KB_block, iters) {
  benchmarkSoftwareCRC32(iters, 1024);
}

BENCHMARK_DRAW_LINE();

// This test is too big for the L1 cache but fits in L2
BENCHMARK(crc32c_hardware_64KB_block, iters) {
  benchmarkHardwareCRC32C(iters, 64 * 1024);
}

BENCHMARK(crc32c_software_64KB_block, iters) {
  benchmarkSoftwareCRC32C(iters, 64 * 1024);
}

BENCHMARK(crc32_hardware_64KB_block, iters) {
  benchmarkHardwareCRC32(iters, 64 * 1024);
}

BENCHMARK(crc32_software_64KB_block, iters) {
  benchmarkSoftwareCRC32(iters, 64 * 1024);
}

BENCHMARK_DRAW_LINE();

// This test is too big for the L2 cache but fits in L3
BENCHMARK(crc32c_hardware_512KB_block, iters) {
  benchmarkHardwareCRC32C(iters, 512 * 1024);
}

BENCHMARK(crc32c_software_512KB_block, iters) {
  benchmarkSoftwareCRC32C(iters, 512 * 1024);
}

BENCHMARK(crc32_hardware_512KB_block, iters) {
  benchmarkHardwareCRC32(iters, 512 * 1024);
}

BENCHMARK(crc32_software_512KB_block, iters) {
  benchmarkSoftwareCRC32(iters, 512 * 1024);
}

BENCHMARK_DRAW_LINE();

BENCHMARK(crc32_combine_linear_512KB_block, iters) {
  benchmarkCombineSoftwareLinear(iters, 512 * 1024);
}

BENCHMARK(crc32_combine_512KB_block, iters) {
  benchmarkCombineHardwareCrc32(iters, 512 * 1024);
}

BENCHMARK(crc32c_combine_512KB_block, iters) {
  benchmarkCombineHardwareCrc32c(iters, 512 * 1024);
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  folly::gflags::ParseCommandLineFlags(&argc, &argv, true);

  // Populate a buffer with a deterministic pattern
  // on which to compute checksums
  const uint8_t* src = buffer;
  uint64_t* dst = (uint64_t*)buffer;
  const uint64_t* end = (const uint64_t*)(buffer + sizeof(buffer));
  *dst++ = 0;
  while (dst < end) {
    *dst++ = folly::hash::fnv64_buf_BROKEN((const char*)src, sizeof(uint64_t));
    src += sizeof(uint64_t);
  }

  auto ret = RUN_ALL_TESTS();
  if (!ret && FLAGS_benchmark) {
    folly::runBenchmarks();
  }
  return ret;
}
