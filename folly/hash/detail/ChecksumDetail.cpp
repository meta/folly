/*
 * crc32_impl.h
 *
 * Copyright 2016 Eric Biggers
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * CRC-32 folding with PCLMULQDQ.
 *
 * The basic idea is to repeatedly "fold" each 512 bits into the next
 * 512 bits, producing an abbreviated message which is congruent the
 * original message modulo the generator polynomial G(x).
 *
 * Folding each 512 bits is implemented as eight 64-bit folds, each of
 * which uses one carryless multiplication instruction.  It's expected
 * that CPUs may be able to execute some of these multiplications in
 * parallel.
 *
 * Explanation of "folding": let A(x) be 64 bits from the message, and
 * let B(x) be 95 bits from a constant distance D later in the
 * message.  The relevant portion of the message can be written as:
 *
 *      M(x) = A(x)*x^D + B(x)
 *
 * ... where + and * represent addition and multiplication,
 * respectively, of polynomials over GF(2).  Note that when
 * implemented on a computer, these operations are equivalent to XOR
 * and carryless multiplication, respectively.
 *
 * For the purpose of CRC calculation, only the remainder modulo the
 * generator polynomial G(x) matters:
 *
 * M(x) mod G(x) = (A(x)*x^D + B(x)) mod G(x)
 *
 * Since the modulo operation can be applied anywhere in a sequence of
 * additions and multiplications without affecting the result, this is
 * equivalent to:
 *
 * M(x) mod G(x) = (A(x)*(x^D mod G(x)) + B(x)) mod G(x)
 *
 * For any D, 'x^D mod G(x)' will be a polynomial with maximum degree
 * 31, i.e.  a 32-bit quantity.  So 'A(x) * (x^D mod G(x))' is
 * equivalent to a carryless multiplication of a 64-bit quantity by a
 * 32-bit quantity, producing a 95-bit product.  Then, adding
 * (XOR-ing) the product to B(x) produces a polynomial with the same
 * length as B(x) but with the same remainder as 'A(x)*x^D + B(x)'.
 * This is the basic fold operation with 64 bits.
 *
 * Note that the carryless multiplication instruction PCLMULQDQ
 * actually takes two 64-bit inputs and produces a 127-bit product in
 * the low-order bits of a 128-bit XMM register.  This works fine, but
 * care must be taken to account for "bit endianness".  With the CRC
 * version implemented here, bits are always ordered such that the
 * lowest-order bit represents the coefficient of highest power of x
 * and the highest-order bit represents the coefficient of the lowest
 * power of x.  This is backwards from the more intuitive order.
 * Still, carryless multiplication works essentially the same either
 * way.  It just must be accounted for that when we XOR the 95-bit
 * product in the low-order 95 bits of a 128-bit XMM register into
 * 128-bits of later data held in another XMM register, we'll really
 * be XOR-ing the product into the mathematically higher degree end of
 * those later bits, not the lower degree end as may be expected.
 *
 * So given that caveat and the fact that we process 512 bits per
 * iteration, the 'D' values we need for the two 64-bit halves of each
 * 128 bits of data are:
 *
 * D = (512 + 95) - 64 for the higher-degree half of each 128
 *                 bits, i.e. the lower order bits in
 *                 the XMM register
 *
 *    D = (512 + 95) - 128 for the lower-degree half of each 128
 *                 bits, i.e. the higher order bits in
 *                 the XMM register
 *
 * The required 'x^D mod G(x)' values were precomputed.
 *
 * When <= 512 bits remain in the message, we finish up by folding
 * across smaller distances.  This works similarly; the distance D is
 * just different, so different constant multipliers must be used.
 * Finally, once the remaining message is just 64 bits, it is is
 * reduced to the CRC-32 using Barrett reduction (explained later).
 *
 * For more information see the original paper from Intel: "Fast CRC
 *    Computation for Generic Polynomials Using PCLMULQDQ
 *    Instruction" December 2009
 *    http://www.intel.com/content/dam/www/public/us/en/documents/
 *    white-papers/
 *    fast-crc-computation-generic-polynomials-pclmulqdq-paper.pdf
 */

#include <folly/CpuId.h>
#include <folly/Portability.h>
#include <folly/detail/TrapOnAvx512.h>
#include <folly/hash/detail/ChecksumDetail.h>

namespace folly {
namespace detail {

#if FOLLY_X64 && FOLLY_SSE_PREREQ(4, 2)

static __m128i crc32MulAdd(__m128i x, __m128i a, __m128i multiplier) {
  /*
   * Note: the immediate constant for PCLMULQDQ specifies which
   * 64-bit halves of the 128-bit vectors to multiply:
   *
   * 0x00 means low halves (higher degree polynomial terms for us)
   * 0x11 means high halves (lower degree polynomial terms for us)
   */
  const __m128i t = _mm_xor_si128(a, _mm_clmulepi64_si128(x, multiplier, 0x00));
  return _mm_xor_si128(t, _mm_clmulepi64_si128(x, multiplier, 0x11));
}

#if FOLLY_HAS_CRC32_VPCLMUL

FOLLY_TARGET_ATTRIBUTE("avx2,vpclmulqdq")
static __m256i crc32MulAdd256(__m256i x, __m256i a, __m256i multiplier) {
  const __m256i t =
      _mm256_xor_si256(a, _mm256_clmulepi64_epi128(x, multiplier, 0x00));
  return _mm256_xor_si256(t, _mm256_clmulepi64_epi128(x, multiplier, 0x11));
}

bool crc32_vpclmul_usable() {
  static const bool value = [] {
    CpuId id;
    return id.avx2() && id.vpclmulqdq() && id.pclmuldq();
  }();
  return value;
}

/*
 * The 1024-bit-at-a-time loop below folds eight 128-bit accumulators against
 * one multiplier. VPCLMULQDQ applies the carryless multiply independently
 * within each 128-bit lane and the XORs are lane-wise, so pairing those eight
 * accumulators into four 256-bit registers -- with the multiplier broadcast to
 * both lanes -- computes bit-identical values with half the instructions. The
 * folding constants are unchanged; none are re-derived for the wider form.
 *
 * Only the main loop widens. Everything after it, and every buffer too short
 * to enter it, runs the original code.
 */
FOLLY_TARGET_ATTRIBUTE("avx2,vpclmulqdq")
uint32_t crc32_hw_aligned_vpclmul(
    uint32_t remainder, const __m128i* p, size_t vec_count) {
  if (vec_count < 8) {
    return crc32_hw_aligned(remainder, p, vec_count);
  }

  /* Constants precomputed by gen_crc32_multipliers.c.  Do not edit! */
  const __m128i multipliers_8 = _mm_set_epi32(0, 0x910EEEC1, 0, 0x33FFF533);
  const __m128i multipliers_4 = _mm_set_epi32(0, 0x1D9513D7, 0, 0x8F352D95);
  const __m128i multipliers_2 = _mm_set_epi32(0, 0x81256527, 0, 0xF1DA05AA);
  const __m128i multipliers_1 = _mm_set_epi32(0, 0xCCAA009E, 0, 0xAE689191);
  const __m128i final_multiplier = _mm_set_epi32(0, 0, 0, 0xB8BC6765);
  const __m128i mask32 = _mm_set_epi32(0, 0, 0, 0xFFFFFFFF);
  const __m128i barrett_reduction_constants =
      _mm_set_epi32(0x1, 0xDB710641, 0x1, 0xF7011641);
  const __m256i multipliers_8_x2 = _mm256_broadcastsi128_si256(multipliers_8);

  const __m128i* const end = p + vec_count;
  const __m128i* const end512 = p + (vec_count & ~3);
  const __m128i* const end1024 = p + (vec_count & ~7);
  __m128i x0, x1, x2, x3, x4, x5, x6, x7;

  /* y0 carries accumulators 0 and 1, y1 carries 2 and 3, and so on, so a
   * 256-bit load of p[2i], p[2i+1] lands each in the lane that owns it. */
  __m256i y0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 0));
  __m256i y1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 2));
  __m256i y2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 4));
  __m256i y3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 6));
  y0 = _mm256_xor_si256(
      y0,
      _mm256_inserti128_si256(
          _mm256_setzero_si256(),
          _mm_set_epi32(0, 0, 0, static_cast<int>(remainder)),
          0));
  p += 8;

  for (; p != end1024; p += 8) {
    y0 = crc32MulAdd256(
        y0,
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 0)),
        multipliers_8_x2);
    y1 = crc32MulAdd256(
        y1,
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 2)),
        multipliers_8_x2);
    y2 = crc32MulAdd256(
        y2,
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 4)),
        multipliers_8_x2);
    y3 = crc32MulAdd256(
        y3,
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 6)),
        multipliers_8_x2);
  }

  x0 = _mm256_castsi256_si128(y0);
  x1 = _mm256_extracti128_si256(y0, 1);
  x2 = _mm256_castsi256_si128(y1);
  x3 = _mm256_extracti128_si256(y1, 1);
  x4 = _mm256_castsi256_si128(y2);
  x5 = _mm256_extracti128_si256(y2, 1);
  x6 = _mm256_castsi256_si128(y3);
  x7 = _mm256_extracti128_si256(y3, 1);

  /* Fold 1024 bits => 512 bits */
  x0 = crc32MulAdd(x0, x4, multipliers_4);
  x1 = crc32MulAdd(x1, x5, multipliers_4);
  x2 = crc32MulAdd(x2, x6, multipliers_4);
  x3 = crc32MulAdd(x3, x7, multipliers_4);

  /* Fold 512 bits at a time */
  for (; p != end512; p += 4) {
    x0 = crc32MulAdd(x0, p[0], multipliers_4);
    x1 = crc32MulAdd(x1, p[1], multipliers_4);
    x2 = crc32MulAdd(x2, p[2], multipliers_4);
    x3 = crc32MulAdd(x3, p[3], multipliers_4);
  }

  /* Fold 512 bits => 128 bits */
  x2 = crc32MulAdd(x0, x2, multipliers_2);
  x3 = crc32MulAdd(x1, x3, multipliers_2);
  x0 = crc32MulAdd(x2, x3, multipliers_1);

  while (p != end) {
    x1 = *p++;
    x0 = crc32MulAdd(x0, x1, multipliers_1);
  }

  x0 = _mm_xor_si128(
      _mm_srli_si128(x0, 8), _mm_clmulepi64_si128(x0, multipliers_1, 0x10));
  x0 = _mm_xor_si128(
      _mm_srli_si128(x0, 4),
      _mm_clmulepi64_si128(_mm_and_si128(x0, mask32), final_multiplier, 0x00));
  x1 = x0;
  x0 = _mm_clmulepi64_si128(
      _mm_and_si128(x0, mask32), barrett_reduction_constants, 0x00);
  x0 = _mm_clmulepi64_si128(
      _mm_and_si128(x0, mask32), barrett_reduction_constants, 0x10);
  return static_cast<uint32_t>(
      _mm_cvtsi128_si32(_mm_srli_si128(_mm_xor_si128(x0, x1), 4)));
}

// The 256-bit fold above pairs eight accumulators into four registers and
// combines with two XORs. On a part with AVX-512 the same eight fit in two
// registers and the combine is one three-input XOR. Eight vectors cost eight
// instructions here and twenty in the 256-bit fold. Lane-wise again, so the
// constants are unchanged and the result is bit-identical.
FOLLY_TARGET_ATTRIBUTE("avx512f,vpclmulqdq")
static __m512i crc32MulAdd512(__m512i x, __m512i a, __m512i multiplier) {
  return _mm512_ternarylogic_epi64(
      a,
      _mm512_clmulepi64_epi128(x, multiplier, 0x00),
      _mm512_clmulepi64_epi128(x, multiplier, 0x11),
      0x96);
}

// FOLLY_DISABLE_AVX512 and the entry check: see crc32c_hw_supported_avx512().
bool crc32_vpclmul512_usable() {
#if FOLLY_DISABLE_AVX512
  return false;
#else
  static const bool value = [] {
    CpuId id;
    return id.avx512f() && id.vpclmulqdq() && id.pclmuldq() &&
        !hasTrapOnAvx512() &&
        !hasTrapOnEntry(
               reinterpret_cast<const void*>(&crc32_hw_aligned_vpclmul512));
  }();
  return value;
#endif
}

FOLLY_TARGET_ATTRIBUTE("avx512f,vpclmulqdq")
uint32_t crc32_hw_aligned_vpclmul512(
    uint32_t remainder, const __m128i* p, size_t vec_count) {
  // Required, not tuning: the loop below loads eight vectors before it
  // checks anything. crc32_hw() also keeps short calls away from here, so
  // this covers callers that come in directly.
  if (vec_count < kCrc32Vpclmul512MinVectorsAmd) {
    return crc32_vpclmul_usable()
        ? crc32_hw_aligned_vpclmul(remainder, p, vec_count)
        : crc32_hw_aligned(remainder, p, vec_count);
  }

  /* Constants precomputed by gen_crc32_multipliers.c.  Do not edit! */
  const __m128i multipliers_8 = _mm_set_epi32(0, 0x910EEEC1, 0, 0x33FFF533);
  const __m128i multipliers_4 = _mm_set_epi32(0, 0x1D9513D7, 0, 0x8F352D95);
  const __m128i multipliers_2 = _mm_set_epi32(0, 0x81256527, 0, 0xF1DA05AA);
  const __m128i multipliers_1 = _mm_set_epi32(0, 0xCCAA009E, 0, 0xAE689191);
  const __m128i final_multiplier = _mm_set_epi32(0, 0, 0, 0xB8BC6765);
  const __m128i mask32 = _mm_set_epi32(0, 0, 0, 0xFFFFFFFF);
  const __m128i barrett_reduction_constants =
      _mm_set_epi32(0x1, 0xDB710641, 0x1, 0xF7011641);
  const __m512i multipliers_8_x4 = _mm512_broadcast_i32x4(multipliers_8);

  const __m128i* const end = p + vec_count;
  const __m128i* const end512 = p + (vec_count & ~3);
  const __m128i* const end1024 = p + (vec_count & ~7);
  __m128i x0, x1, x2, x3, x4, x5, x6, x7;

  /* z0 carries accumulators 0 to 3, z1 carries 4 to 7. */
  __m512i z0 = _mm512_loadu_si512(reinterpret_cast<const void*>(p + 0));
  __m512i z1 = _mm512_loadu_si512(reinterpret_cast<const void*>(p + 4));
  z0 = _mm512_xor_si512(
      z0,
      _mm512_inserti32x4(
          _mm512_setzero_si512(),
          _mm_set_epi32(0, 0, 0, static_cast<int>(remainder)),
          0));
  p += 8;

  for (; p != end1024; p += 8) {
    z0 = crc32MulAdd512(
        z0,
        _mm512_loadu_si512(reinterpret_cast<const void*>(p + 0)),
        multipliers_8_x4);
    z1 = crc32MulAdd512(
        z1,
        _mm512_loadu_si512(reinterpret_cast<const void*>(p + 4)),
        multipliers_8_x4);
  }

  x0 = _mm512_extracti32x4_epi32(z0, 0);
  x1 = _mm512_extracti32x4_epi32(z0, 1);
  x2 = _mm512_extracti32x4_epi32(z0, 2);
  x3 = _mm512_extracti32x4_epi32(z0, 3);
  x4 = _mm512_extracti32x4_epi32(z1, 0);
  x5 = _mm512_extracti32x4_epi32(z1, 1);
  x6 = _mm512_extracti32x4_epi32(z1, 2);
  x7 = _mm512_extracti32x4_epi32(z1, 3);

  /* Fold 1024 bits => 512 bits */
  x0 = crc32MulAdd(x0, x4, multipliers_4);
  x1 = crc32MulAdd(x1, x5, multipliers_4);
  x2 = crc32MulAdd(x2, x6, multipliers_4);
  x3 = crc32MulAdd(x3, x7, multipliers_4);

  /* Fold 512 bits at a time */
  for (; p != end512; p += 4) {
    x0 = crc32MulAdd(x0, p[0], multipliers_4);
    x1 = crc32MulAdd(x1, p[1], multipliers_4);
    x2 = crc32MulAdd(x2, p[2], multipliers_4);
    x3 = crc32MulAdd(x3, p[3], multipliers_4);
  }

  /* Fold 512 bits => 128 bits */
  x2 = crc32MulAdd(x0, x2, multipliers_2);
  x3 = crc32MulAdd(x1, x3, multipliers_2);
  x0 = crc32MulAdd(x2, x3, multipliers_1);

  while (p != end) {
    x1 = *p++;
    x0 = crc32MulAdd(x0, x1, multipliers_1);
  }

  x0 = _mm_xor_si128(
      _mm_srli_si128(x0, 8), _mm_clmulepi64_si128(x0, multipliers_1, 0x10));
  x0 = _mm_xor_si128(
      _mm_srli_si128(x0, 4),
      _mm_clmulepi64_si128(_mm_and_si128(x0, mask32), final_multiplier, 0x00));
  x1 = x0;
  x0 = _mm_clmulepi64_si128(
      _mm_and_si128(x0, mask32), barrett_reduction_constants, 0x00);
  x0 = _mm_clmulepi64_si128(
      _mm_and_si128(x0, mask32), barrett_reduction_constants, 0x10);
  return static_cast<uint32_t>(
      _mm_cvtsi128_si32(_mm_srli_si128(_mm_xor_si128(x0, x1), 4)));
}

#endif // FOLLY_HAS_CRC32_VPCLMUL

uint32_t crc32_hw_aligned(
    uint32_t remainder, const __m128i* p, size_t vec_count) {
  if (vec_count == 0) {
    return remainder;
  }

  /* Constants precomputed by gen_crc32_multipliers.c.  Do not edit! */
  const __m128i multipliers_8 = _mm_set_epi32(0, 0x910EEEC1, 0, 0x33FFF533);
  const __m128i multipliers_4 = _mm_set_epi32(0, 0x1D9513D7, 0, 0x8F352D95);
  const __m128i multipliers_2 = _mm_set_epi32(0, 0x81256527, 0, 0xF1DA05AA);
  const __m128i multipliers_1 = _mm_set_epi32(0, 0xCCAA009E, 0, 0xAE689191);
  const __m128i final_multiplier = _mm_set_epi32(0, 0, 0, 0xB8BC6765);
  const __m128i mask32 = _mm_set_epi32(0, 0, 0, 0xFFFFFFFF);
  const __m128i barrett_reduction_constants =
      _mm_set_epi32(0x1, 0xDB710641, 0x1, 0xF7011641);

  const __m128i* const end = p + vec_count;
  const __m128i* const end512 = p + (vec_count & ~3);
  const __m128i* const end1024 = p + (vec_count & ~7);
  __m128i x0, x1, x2, x3, x4, x5, x6, x7;

  /*
   * Account for the current 'remainder', i.e. the CRC of the part of
   * the message already processed.  Explanation: rewrite the message
   * polynomial M(x) in terms of the first part A(x), the second part
   * B(x), and the length of the second part in bits |B(x)| >= 32:
   *
   *    M(x) = A(x)*x^|B(x)| + B(x)
   *
   * Then the CRC of M(x) is:
   *
   *    CRC(M(x)) = CRC(A(x)*x^|B(x)| + B(x))
   *              = CRC(A(x)*x^32*x^(|B(x)| - 32) + B(x))
   *              = CRC(CRC(A(x))*x^(|B(x)| - 32) + B(x))
   *
   * Note: all arithmetic is modulo G(x), the generator polynomial; that's
   * why A(x)*x^32 can be replaced with CRC(A(x)) = A(x)*x^32 mod G(x).
   *
   * So the CRC of the full message is the CRC of the second part of the
   * message where the first 32 bits of the second part of the message
   * have been XOR'ed with the CRC of the first part of the message.
   */
  x0 = *p++;
  x0 = _mm_xor_si128(x0, _mm_set_epi32(0, 0, 0, remainder));

  if (p > end512) { /* only 128, 256, or 384 bits of input? */
    goto _128_bits_at_a_time;
  }
  x1 = *p++;
  x2 = *p++;
  x3 = *p++;
  if (p > end1024) { /* Less than 1024 bits of input available */
    goto _512_bits_at_a_time;
  }
  x4 = *p++;
  x5 = *p++;
  x6 = *p++;
  x7 = *p++;

  for (; p != end1024; p += 8) {
    x0 = crc32MulAdd(x0, p[0], multipliers_8);
    x1 = crc32MulAdd(x1, p[1], multipliers_8);
    x2 = crc32MulAdd(x2, p[2], multipliers_8);
    x3 = crc32MulAdd(x3, p[3], multipliers_8);
    x4 = crc32MulAdd(x4, p[4], multipliers_8);
    x5 = crc32MulAdd(x5, p[5], multipliers_8);
    x6 = crc32MulAdd(x6, p[6], multipliers_8);
    x7 = crc32MulAdd(x7, p[7], multipliers_8);
  }

  /* Fold 1024 bits => 512 bits */
  x0 = crc32MulAdd(x0, x4, multipliers_4);
  x1 = crc32MulAdd(x1, x5, multipliers_4);
  x2 = crc32MulAdd(x2, x6, multipliers_4);
  x3 = crc32MulAdd(x3, x7, multipliers_4);

_512_bits_at_a_time:
  /* Fold 512 bits at a time */
  for (; p != end512; p += 4) {
    x0 = crc32MulAdd(x0, p[0], multipliers_4);
    x1 = crc32MulAdd(x1, p[1], multipliers_4);
    x2 = crc32MulAdd(x2, p[2], multipliers_4);
    x3 = crc32MulAdd(x3, p[3], multipliers_4);
  }

  /* Fold 512 bits => 128 bits */
  x2 = crc32MulAdd(x0, x2, multipliers_2);
  x3 = crc32MulAdd(x1, x3, multipliers_2);
  x0 = crc32MulAdd(x2, x3, multipliers_1);

_128_bits_at_a_time:
  while (p != end) {
    /* Fold 128 bits into next 128 bits */
    x1 = *p++;
    x0 = crc32MulAdd(x0, x1, multipliers_1);
  }

  /* Now there are just 128 bits left, stored in 'x0'. */

  /*
   * Fold 128 => 96 bits.  This also implicitly appends 32 zero bits,
   * which is equivalent to multiplying by x^32.  This is needed because
   * the CRC is defined as M(x)*x^32 mod G(x), not just M(x) mod G(x).
   */
  x0 = _mm_xor_si128(
      _mm_srli_si128(x0, 8), _mm_clmulepi64_si128(x0, multipliers_1, 0x10));

  /* Fold 96 => 64 bits */
  x0 = _mm_xor_si128(
      _mm_srli_si128(x0, 4),
      _mm_clmulepi64_si128(_mm_and_si128(x0, mask32), final_multiplier, 0x00));

  /*
   * Finally, reduce 64 => 32 bits using Barrett reduction.
   *
   * Let M(x) = A(x)*x^32 + B(x) be the remaining message.  The goal is to
   * compute R(x) = M(x) mod G(x).  Since degree(B(x)) < degree(G(x)):
   *
   *    R(x) = (A(x)*x^32 + B(x)) mod G(x)
   *         = (A(x)*x^32) mod G(x) + B(x)
   *
   * Then, by the Division Algorithm there exists a unique q(x) such that:
   *
   *    A(x)*x^32 mod G(x) = A(x)*x^32 - q(x)*G(x)
   *
   * Since the left-hand side is of maximum degree 31, the right-hand side
   * must be too.  This implies that we can apply 'mod x^32' to the
   * right-hand side without changing its value:
   *
   *    (A(x)*x^32 - q(x)*G(x)) mod x^32 = q(x)*G(x) mod x^32
   *
   * Note that '+' is equivalent to '-' in polynomials over GF(2).
   *
   * We also know that:
   *
   *                  / A(x)*x^32 \
   *    q(x) = floor (  ---------  )
   *                  \    G(x)   /
   *
   * To compute this efficiently, we can multiply the top and bottom by
   * x^32 and move the division by G(x) to the top:
   *
   *                  / A(x) * floor(x^64 / G(x)) \
   *    q(x) = floor (  -------------------------  )
   *                  \           x^32            /
   *
   * Note that floor(x^64 / G(x)) is a constant.
   *
   * So finally we have:
   *
   *                              / A(x) * floor(x^64 / G(x)) \
   *    R(x) = B(x) + G(x)*floor (  -------------------------  )
   *                              \           x^32            /
   */
  x1 = x0;
  x0 = _mm_clmulepi64_si128(
      _mm_and_si128(x0, mask32), barrett_reduction_constants, 0x00);
  x0 = _mm_clmulepi64_si128(
      _mm_and_si128(x0, mask32), barrett_reduction_constants, 0x10);
  return _mm_cvtsi128_si32(_mm_srli_si128(_mm_xor_si128(x0, x1), 4));
}

#endif
} // namespace detail
} // namespace folly
