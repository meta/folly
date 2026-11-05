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

#include <cstdint>
#include <cstdio>
#include <vector>

#include <folly/CpuId.h>
#include <folly/Portability.h>
#include <folly/detail/TrapOnAvx512.h>
#include <folly/hash/detail/ChecksumDetail.h>
#include <folly/portability/GTest.h>

namespace {

// Says which path a kernel test covered. A host without the kernel's features
// passes by checking the generic entry point instead, and this line is how a
// reader of a green run tells the two apart.
void recordPath(const char* kernel, bool ran, const char* fallback) {
  const folly::CpuId id;
  char line[192];
  std::snprintf(
      line,
      sizeof(line),
      "%s %s (avx2=%d avx512f=%d avx512vl=%d vpclmulqdq=%d pclmuldq=%d)",
      ran ? "ran" : "fell back to",
      ran ? kernel : fallback,
      id.avx2(),
      id.avx512f(),
      id.avx512vl(),
      id.vpclmulqdq(),
      id.pclmuldq());
  testing::Test::RecordProperty(kernel, line);
  std::printf("%s: %s\n", kernel, line);
}

const uint8_t* testBuffer() {
  static const std::vector<uint8_t> buf = [] {
    std::vector<uint8_t> v((1u << 20) + 64);
    for (size_t i = 0; i < v.size(); ++i) {
      v[i] = static_cast<uint8_t>((i * 1103515245ull) >> 16);
    }
    return v;
  }();
  return buf.data();
}

struct Fold {
  const char* name;
  uint32_t (*fn)(const uint8_t*, size_t, uint32_t);
};

uint32_t genericCrc32c(const uint8_t* data, size_t nbytes, uint32_t crc) {
  return folly::crc32c(data, nbytes, crc);
}

// Each fold where this host can run it. Where it cannot, the same inputs go
// through folly::crc32c instead, so every host checks something.
std::vector<Fold> runnableFolds() {
  struct Candidate {
    const char* name;
    bool usable;
    uint32_t (*fn)(const uint8_t*, size_t, uint32_t);
  };
#if FOLLY_X64 && FOLLY_HAS_CRC32_VPCLMUL
  const Candidate candidates[] = {
      {"crc32c_wide512",
       folly::detail::crc32c_wide512_usable(),
       folly::detail::crc32c_wide512},
      {"crc32c_wide256_avx512",
       folly::detail::crc32c_wide256_avx512_usable(),
       folly::detail::crc32c_wide256_avx512},
      {"crc32c_wide256_avx2",
       folly::detail::crc32c_wide256_avx2_usable(),
       folly::detail::crc32c_wide256_avx2},
  };
#else
  const Candidate candidates[] = {
      {"crc32c_wide512", false, nullptr},
      {"crc32c_wide256_avx512", false, nullptr},
      {"crc32c_wide256_avx2", false, nullptr},
  };
#endif
  std::vector<Fold> folds;
  for (const auto& c : candidates) {
    recordPath(c.name, c.usable, "folly::crc32c");
    folds.push_back({c.name, c.usable ? c.fn : genericCrc32c});
  }
  return folds;
}

#if FOLLY_X64 && FOLLY_HAS_CRC32_VPCLMUL
void crc32WideFoldMatchesNarrowerFolds(const uint8_t* aligned) {
  const auto* p = reinterpret_cast<const __m128i*>(aligned);
  for (uint32_t remainder : {0U, ~0U, 0x12345678U}) {
    static_assert(
        folly::detail::kCrc32Vpclmul512MinVectorsAmd < 72,
        "the sweep below must cross the fold's minimum vector count");
    for (size_t vecs = 0; vecs <= 72; ++vecs) {
      SCOPED_TRACE(
          testing::Message() << "remainder=" << remainder << " vecs=" << vecs);
      const uint32_t ref = folly::detail::crc32_hw_aligned(remainder, p, vecs);
      ASSERT_EQ(
          ref, folly::detail::crc32_hw_aligned_vpclmul(remainder, p, vecs));
      ASSERT_EQ(
          ref, folly::detail::crc32_hw_aligned_vpclmul512(remainder, p, vecs));
    }
    for (size_t vecs : {size_t(256), size_t(512), size_t(4096)}) {
      const uint32_t ref = folly::detail::crc32_hw_aligned(remainder, p, vecs);
      ASSERT_EQ(
          ref, folly::detail::crc32_hw_aligned_vpclmul512(remainder, p, vecs))
          << vecs;
    }
  }
}
#endif

} // namespace

// What the two capability predicates require. A part that fails one of
// them runs a narrower kernel and returns the same checksum, so the flag
// list is invisible in any output comparison.
#if FOLLY_X64 && FOLLY_HAS_CRC32_VPCLMUL
TEST(Checksum, crc32cWideUsableChecksTheRightFlags) {
  const folly::CpuId id;

  constexpr bool kAvx512Allowed = !FOLLY_DISABLE_AVX512;
  EXPECT_EQ(
      kAvx512Allowed && id.avx512f() && id.avx512vl() && id.vpclmulqdq() &&
          id.pclmuldq() && id.sse42() && !folly::detail::hasTrapOnAvx512(),
      folly::detail::crc32c_wide_avx512_usable());
  EXPECT_EQ(
      id.avx2() && id.vpclmulqdq() && id.pclmuldq() && id.sse42(),
      folly::detail::crc32c_wide256_avx2_usable());

  // These hold whatever the flag lists are, so they survive a rewrite of
  // the predicates that the equalities above would not. Both folds carry a
  // 128-bit tail and a scalar remainder, so neither runs without the legacy
  // PCLMULQDQ and SSE4.2.
  if (folly::detail::crc32c_wide_avx512_usable() ||
      folly::detail::crc32c_wide256_avx2_usable()) {
    EXPECT_TRUE(id.vpclmulqdq());
    EXPECT_TRUE(id.pclmuldq());
    EXPECT_TRUE(id.sse42());
  }
  if (folly::detail::hasTrapOnAvx512()) {
    EXPECT_FALSE(folly::detail::crc32c_wide_avx512_usable());
  }

  // Each AVX-512 fold adds a check of its own entry to the shared test. This
  // binary is not BOLTed, so neither entry is trapped.
  EXPECT_EQ(
      folly::detail::crc32c_wide_avx512_usable(),
      folly::detail::crc32c_wide512_usable());
  EXPECT_EQ(
      folly::detail::crc32c_wide_avx512_usable(),
      folly::detail::crc32c_wide256_avx512_usable());
}
#endif

#if FOLLY_X64 && FOLLY_HAS_CRC32_VPCLMUL
TEST(Checksum, crc32VpclmulUsableChecksTheRightFlags) {
  const folly::CpuId id;
  const bool entryTrapped = folly::detail::hasTrapOnEntry(
      reinterpret_cast<const void*>(
          &folly::detail::crc32_hw_aligned_vpclmul512));

  EXPECT_EQ(
      id.avx2() && id.vpclmulqdq() && id.pclmuldq(),
      folly::detail::crc32_vpclmul_usable());
#if FOLLY_DISABLE_AVX512
  EXPECT_FALSE(folly::detail::crc32_vpclmul512_usable());
#else
  EXPECT_EQ(
      id.avx512f() && id.vpclmulqdq() && id.pclmuldq() &&
          !folly::detail::hasTrapOnAvx512() && !entryTrapped,
      folly::detail::crc32_vpclmul512_usable());
#endif

  // These hold whatever the flag lists are. The 128-bit tail of both folds
  // needs the legacy PCLMULQDQ.
  if (folly::detail::crc32_vpclmul512_usable()) {
    EXPECT_TRUE(id.avx512f());
    EXPECT_TRUE(id.vpclmulqdq());
    EXPECT_TRUE(id.pclmuldq());
  }
  if (entryTrapped || folly::detail::hasTrapOnAvx512()) {
    EXPECT_FALSE(folly::detail::crc32_vpclmul512_usable());
  }
}
#endif

// The wider folds must agree with the kernel they were derived from, not just
// with crc32c_sw: the whole claim is that pairing the accumulators into wider
// registers changes nothing. A fold that is wrong in one lane still gets the
// other seven right, so the sweep runs every length across a block boundary
// rather than one convenient size.
TEST(Checksum, crc32cWideFoldsMatchAvx512) {
  const uint8_t* const buffer = testBuffer();
  const auto folds = runnableFolds();
  // Every alignment, because the prologue peels to an 8-byte boundary and
  // then conditionally consumes one more 8-byte word. 224 bytes is the block
  // the derived loop consumes per iteration, so the sweep covers no blocks,
  // one block, the boundary and a partial tail.
  for (uint32_t startingChecksum : {0U, ~0U}) {
    for (size_t offset = 0; offset < 16; ++offset) {
      for (size_t length = 0; length <= 224 * 3 + 32; ++length) {
        const uint8_t* data = buffer + offset;
        const uint32_t sw =
            folly::detail::crc32c_sw(data, length, startingChecksum);
        for (const auto& fold : folds) {
          ASSERT_EQ(sw, fold.fn(data, length, startingChecksum))
              << fold.name << " startingChecksum=" << startingChecksum
              << " offset=" << offset << " length=" << length;
        }
      }
    }
  }
  // The sweep above stops at 704 bytes, so walk a spread of larger lengths
  // too. A bug that needs a particular number of loop iterations, or a
  // particular tail, would otherwise sit in the gap.
  for (size_t length = 705; length < (1u << 20); length += 997) {
    const uint32_t sw = folly::detail::crc32c_sw(buffer + 3, length, ~0U);
    for (const auto& fold : folds) {
      ASSERT_EQ(sw, fold.fn(buffer + 3, length, ~0U))
          << fold.name << " length=" << length;
    }
  }
  // Large inputs, where the derived loop actually runs many iterations, and
  // both sides of every size where crc32c changes its mind: 4096 and 6144
  // pick between scalar and a generated kernel.
  for (size_t length :
       {size_t(4095),
        size_t(4096),
        size_t(6144),
        size_t(6145),
        size_t(8192),
        size_t(65536),
        size_t(1) << 20}) {
    const uint32_t sw = folly::detail::crc32c_sw(buffer, length, 0);
    for (const auto& fold : folds) {
      ASSERT_EQ(sw, fold.fn(buffer, length, 0)) << fold.name << " " << length;
    }
  }
}

// The 512-bit fold must agree with the 256-bit one it was derived from and
// with the 128-bit scalar fold, at every vector count across the 8-vector
// boundary its loop keys on, and on both sides of the 4-vector tail. The
// sweep also has to cross the kernel's own guard, where the fold hands
// short calls to a narrower kernel, so both sides of that are checked.
TEST(Checksum, crc32WideFoldMatches) {
  alignas(16) static uint8_t aligned[64 * 1024];
  for (size_t i = 0; i < sizeof(aligned); ++i) {
    aligned[i] = static_cast<uint8_t>((i * 1103515245ull) >> 16);
  }
#if FOLLY_X64 && FOLLY_HAS_CRC32_VPCLMUL
  if (folly::detail::crc32_vpclmul512_usable()) {
    recordPath("crc32_hw_aligned_vpclmul512", true, "folly::crc32");
    crc32WideFoldMatchesNarrowerFolds(aligned);
    return;
  }
#endif
  recordPath("crc32_hw_aligned_vpclmul512", false, "folly::crc32");
  for (uint32_t startingChecksum : {0U, ~0U, 0x12345678U}) {
    for (size_t vecs :
         {size_t(0),
          size_t(1),
          size_t(7),
          size_t(8),
          size_t(63),
          size_t(64),
          size_t(72),
          size_t(256),
          size_t(4096)}) {
      const size_t nbytes = vecs * 16;
      ASSERT_EQ(
          folly::detail::crc32_sw(aligned, nbytes, startingChecksum),
          folly::crc32(aligned, nbytes, startingChecksum))
          << "startingChecksum=" << startingChecksum << " vecs=" << vecs;
    }
  }
}
