/*
 * rapidhash V3 - Very fast, high quality, platform-independent hashing
algorithm.
 *
 * Based on 'wyhash', by Wang Yi <godspeed_china@yeah.net>
 *
 * Copyright (C) 2025 Nicolas De Carli
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * You can contact the author at:
 *   - rapidhash source repository: https://github.com/Nicoshev/rapidhash
 */

#pragma once

/*
 *  Includes.
 */
#include <stdint.h>
#include <string.h>
#if defined(_MSC_VER)
#include <intrin.h>
#if defined(_M_X64) && !defined(_M_ARM64EC)
#pragma intrinsic(_umul128)
#endif
#endif

#include <folly/CPortability.h>
#include <folly/lang/Bits.h>

FOLLY_UNSAFE_BUFFER_USAGE_BEGIN

namespace folly {
namespace external {
namespace rapidhash_detail {

/*
 *  C++ macros.
 */
#if __cplusplus >= 201402L && !defined(_MSC_VER)
#define FOLLY_EXTERNAL_RAPIDHASH_INLINE_CONSTEXPR FOLLY_ALWAYS_INLINE constexpr
#else
#define FOLLY_EXTERNAL_RAPIDHASH_INLINE_CONSTEXPR FOLLY_ALWAYS_INLINE
#endif

/*
 *  Unrolling macros, changes code definition for main hash function.
 *
 *  FOLLY_EXTERNAL_RAPIDHASH_COMPACT: Legacy variant, each loop process 48 bytes.
 *  FOLLY_EXTERNAL_RAPIDHASH_UNROLLED: Unrolled variant, each loop process 96 bytes.
 *
 *  Most modern CPUs should benefit from having RAPIDHASH_UNROLLED.
 *
 *  These macros do not alter the output hash.
 */
#ifndef FOLLY_EXTERNAL_RAPIDHASH_COMPACT
#define FOLLY_EXTERNAL_RAPIDHASH_UNROLLED
#elif defined(FOLLY_EXTERNAL_RAPIDHASH_UNROLLED)
#error "cannot define FOLLY_EXTERNAL_RAPIDHASH_COMPACT and FOLLY_EXTERNAL_RAPIDHASH_UNROLLED simultaneously."
#endif

/*
 *  Default secret parameters.
 */
 constexpr uint64_t rapidhash_secret[8] = {
    0x2d358dccaa6c78a5ull,
    0x8bb84b93962eacc9ull,
    0x4b33a62ed433d4a3ull,
    0x4d5a2da51de1aa47ull,
    0xa0761d6478bd642full,
    0xe7037ed1a0b428dbull,
    0x90ed1765281c388cull,
    0xaaaaaaaaaaaaaaaaull};

/*
 *  64*64 -> 128bit multiply function.
 *
 *  @param A  Address of 64-bit number.
 *  @param B  Address of 64-bit number.
 *
 *  Calculates 128-bit C = *A * *B.
 *
 */
FOLLY_EXTERNAL_RAPIDHASH_INLINE_CONSTEXPR void rapidhash_mum(uint64_t* A, uint64_t* B)
    noexcept {
#if defined(__SIZEOF_INT128__)
  __uint128_t r = *A;
  r *= *B;
  *A = static_cast<uint64_t>(r);
  *B = static_cast<uint64_t>(r >> 64);
#elif defined(_MSC_VER) && (defined(_WIN64) || defined(_M_HYBRID_CHPE_ARM64))
#if defined(_M_X64)
  *A = _umul128(*A, *B, B);
#else
  uint64_t c = __umulh(*A, *B);
  *A = *A * *B;
  *B = c;
#endif
#else
  uint64_t ha = *A >> 32, hb = *B >> 32, la = (uint32_t)*A, lb = (uint32_t)*B;
  uint64_t rh = ha * hb, rm0 = ha * lb, rm1 = hb * la, rl = la * lb,
           t = rl + (rm0 << 32), c = t < rl;
  uint64_t lo = t + (rm1 << 32);
  c += lo < t;
  uint64_t hi = rh + (rm0 >> 32) + (rm1 >> 32) + c;
  *A = lo;
  *B = hi;
#endif
}

/*
 *  Multiply and xor mix function.
 *
 *  @param A  64-bit number.
 *  @param B  64-bit number.
 *
 *  Calculates 128-bit C = A * B.
 *  Returns 64-bit xor between high and low 64 bits of C.
 */
FOLLY_EXTERNAL_RAPIDHASH_INLINE_CONSTEXPR uint64_t
rapidhash_mix(uint64_t A, uint64_t B) noexcept {
  rapidhash_mum(&A, &B);
  return A ^ B;
}

/*
 *  Read functions.
 */
FOLLY_EXTERNAL_RAPIDHASH_INLINE_CONSTEXPR uint32_t
rapidhash_read32(const char* p) noexcept {
  return folly::constexprLoadUnaligned<uint32_t, char>(p);
}

FOLLY_EXTERNAL_RAPIDHASH_INLINE_CONSTEXPR uint64_t
rapidhash_read64(const char* p) noexcept {
  return folly::constexprLoadUnaligned<uint64_t, char>(p);
}

/*
 *  rapidhash main function.
 *
 *  @param p       Buffer to be hashed.
 *  @param len     @key length, in bytes.
 *  @param seed    64-bit seed used to alter the hash result predictably.
 *  @param secret  Triplet of 64-bit secrets used to alter hash result
 * predictably.
 *
 *  Returns a 64-bit hash.
 */
FOLLY_EXTERNAL_RAPIDHASH_INLINE_CONSTEXPR uint64_t rapidhash_internal(
    const char* p, size_t len, uint64_t seed, const uint64_t* secret)
    noexcept {
  seed ^= rapidhash_mix(seed ^ secret[2], secret[1]);
  uint64_t a = 0, b = 0;
  size_t i = len;
  if (len <= 16) [[likely]] {
    if (len >= 4) {
      seed ^= len;
      if (len >= 8) {
        const char* plast = p + len - 8;
        a = rapidhash_read64(p);
        b = rapidhash_read64(plast);
      } else {
        const char* plast = p + len - 4;
        a = rapidhash_read32(p);
        b = rapidhash_read32(plast);
      }
    } else if (len > 0) {
      a = (static_cast<uint64_t>(p[0]) << 45) | static_cast<uint64_t>(p[len - 1]);
      b = static_cast<uint64_t>(p[len >> 1]);
    } else
      a = b = 0;
  } else {
    uint64_t see1 = seed, see2 = seed;
    uint64_t see3 = seed, see4 = seed;
    uint64_t see5 = seed, see6 = seed;
#ifdef FOLLY_EXTERNAL_RAPIDHASH_COMPACT
    if (i > 112) {
      do {
        seed =
            rapidhash_mix(rapidhash_read64(p) ^ secret[0], rapidhash_read64(p + 8) ^ seed);
        see1 = rapidhash_mix(
            rapidhash_read64(p + 16) ^ secret[1], rapidhash_read64(p + 24) ^ see1);
        see2 = rapidhash_mix(
            rapidhash_read64(p + 32) ^ secret[2], rapidhash_read64(p + 40) ^ see2);
        see3 = rapidhash_mix(
            rapidhash_read64(p + 48) ^ secret[3], rapidhash_read64(p + 56) ^ see3);
        see4 = rapidhash_mix(
            rapidhash_read64(p + 64) ^ secret[4], rapidhash_read64(p + 72) ^ see4);
        see5 = rapidhash_mix(
            rapidhash_read64(p + 80) ^ secret[5], rapidhash_read64(p + 88) ^ see5);
        see6 = rapidhash_mix(
            rapidhash_read64(p + 96) ^ secret[6], rapidhash_read64(p + 104) ^ see6);
        p += 112;
        i -= 112;
      } while (i > 112);
      seed ^= see1;
      see2 ^= see3;
      see4 ^= see5;
      seed ^= see6;
      see2 ^= see4;
      seed ^= see2;
    }
#else
    if (i > 224) {
      do {
        seed =
            rapidhash_mix(rapidhash_read64(p) ^ secret[0], rapidhash_read64(p + 8) ^ seed);
        see1 = rapidhash_mix(
            rapidhash_read64(p + 16) ^ secret[1], rapidhash_read64(p + 24) ^ see1);
        see2 = rapidhash_mix(
            rapidhash_read64(p + 32) ^ secret[2], rapidhash_read64(p + 40) ^ see2);
        see3 = rapidhash_mix(
            rapidhash_read64(p + 48) ^ secret[3], rapidhash_read64(p + 56) ^ see3);
        see4 = rapidhash_mix(
            rapidhash_read64(p + 64) ^ secret[4], rapidhash_read64(p + 72) ^ see4);
        see5 = rapidhash_mix(
            rapidhash_read64(p + 80) ^ secret[5], rapidhash_read64(p + 88) ^ see5);
        see6 = rapidhash_mix(
            rapidhash_read64(p + 96) ^ secret[6], rapidhash_read64(p + 104) ^ see6);
        seed = rapidhash_mix(
            rapidhash_read64(p + 112) ^ secret[0], rapidhash_read64(p + 120) ^ seed);
        see1 = rapidhash_mix(
            rapidhash_read64(p + 128) ^ secret[1], rapidhash_read64(p + 136) ^ see1);
        see2 = rapidhash_mix(
            rapidhash_read64(p + 144) ^ secret[2], rapidhash_read64(p + 152) ^ see2);
        see3 = rapidhash_mix(
            rapidhash_read64(p + 160) ^ secret[3], rapidhash_read64(p + 168) ^ see3);
        see4 = rapidhash_mix(
            rapidhash_read64(p + 176) ^ secret[4], rapidhash_read64(p + 184) ^ see4);
        see5 = rapidhash_mix(
            rapidhash_read64(p + 192) ^ secret[5], rapidhash_read64(p + 200) ^ see5);
        see6 = rapidhash_mix(
            rapidhash_read64(p + 208) ^ secret[6], rapidhash_read64(p + 216) ^ see6);
        p += 224;
        i -= 224;
      } while (i > 224);
    }
    if (i > 112) {
      seed = rapidhash_mix(rapidhash_read64(p) ^ secret[0], rapidhash_read64(p + 8) ^ seed);
      see1 = rapidhash_mix(
          rapidhash_read64(p + 16) ^ secret[1], rapidhash_read64(p + 24) ^ see1);
      see2 = rapidhash_mix(
          rapidhash_read64(p + 32) ^ secret[2], rapidhash_read64(p + 40) ^ see2);
      see3 = rapidhash_mix(
          rapidhash_read64(p + 48) ^ secret[3], rapidhash_read64(p + 56) ^ see3);
      see4 = rapidhash_mix(
          rapidhash_read64(p + 64) ^ secret[4], rapidhash_read64(p + 72) ^ see4);
      see5 = rapidhash_mix(
          rapidhash_read64(p + 80) ^ secret[5], rapidhash_read64(p + 88) ^ see5);
      see6 = rapidhash_mix(
          rapidhash_read64(p + 96) ^ secret[6], rapidhash_read64(p + 104) ^ see6);
      p += 112;
      i -= 112;
    }
    seed ^= see1;
    see2 ^= see3;
    see4 ^= see5;
    seed ^= see6;
    see2 ^= see4;
    seed ^= see2;
#endif
    if (i > 16) {
      seed = rapidhash_mix(rapidhash_read64(p) ^ secret[2], rapidhash_read64(p + 8) ^ seed);
      if (i > 32) {
        seed = rapidhash_mix(
            rapidhash_read64(p + 16) ^ secret[2], rapidhash_read64(p + 24) ^ seed);
        if (i > 48) {
          seed = rapidhash_mix(
              rapidhash_read64(p + 32) ^ secret[1], rapidhash_read64(p + 40) ^ seed);
          if (i > 64) {
            seed = rapidhash_mix(
                rapidhash_read64(p + 48) ^ secret[1], rapidhash_read64(p + 56) ^ seed);
            if (i > 80) {
              seed = rapidhash_mix(
                  rapidhash_read64(p + 64) ^ secret[2],
                  rapidhash_read64(p + 72) ^ seed);
              if (i > 96) {
                seed = rapidhash_mix(
                    rapidhash_read64(p + 80) ^ secret[1],
                    rapidhash_read64(p + 88) ^ seed);
              }
            }
          }
        }
      }
    }
    a = rapidhash_read64(p + i - 16) ^ i;
    b = rapidhash_read64(p + i - 8);
  }
  a ^= secret[1];
  b ^= seed;
  rapidhash_mum(&a, &b);
  return rapidhash_mix(a ^ secret[7], b ^ secret[1] ^ i);
}

FOLLY_EXTERNAL_RAPIDHASH_INLINE_CONSTEXPR uint64_t rapidhashMicro_internal(
    const char* p, size_t len, uint64_t seed, const uint64_t* secret)
    noexcept {
  seed ^= rapidhash_mix(seed ^ secret[2], secret[1]);
  uint64_t a = 0, b = 0;
  size_t i = len;
  if (len <= 16) [[likely]] {
    if (len >= 4) {
      seed ^= len;
      if (len >= 8) {
        const char* plast = p + len - 8;
        a = rapidhash_read64(p);
        b = rapidhash_read64(plast);
      } else {
        const char* plast = p + len - 4;
        a = rapidhash_read32(p);
        b = rapidhash_read32(plast);
      }
    } else if (len > 0) {
      a = (static_cast<uint64_t>(p[0]) << 45) | static_cast<uint64_t>(p[len - 1]);
      b = static_cast<uint64_t>(p[len >> 1]);
    } else
      a = b = 0;
  } else {
    if (i > 80) {
      uint64_t see1 = seed, see2 = seed;
      uint64_t see3 = seed, see4 = seed;
      do {
        seed =
            rapidhash_mix(rapidhash_read64(p) ^ secret[0], rapidhash_read64(p + 8) ^ seed);
        see1 = rapidhash_mix(
            rapidhash_read64(p + 16) ^ secret[1], rapidhash_read64(p + 24) ^ see1);
        see2 = rapidhash_mix(
            rapidhash_read64(p + 32) ^ secret[2], rapidhash_read64(p + 40) ^ see2);
        see3 = rapidhash_mix(
            rapidhash_read64(p + 48) ^ secret[3], rapidhash_read64(p + 56) ^ see3);
        see4 = rapidhash_mix(
            rapidhash_read64(p + 64) ^ secret[4], rapidhash_read64(p + 72) ^ see4);
        p += 80;
        i -= 80;
      } while (i > 80);
      seed ^= see1;
      see2 ^= see3;
      seed ^= see4;
      seed ^= see2;
    }
    if (i > 16) {
      seed = rapidhash_mix(rapidhash_read64(p) ^ secret[2], rapidhash_read64(p + 8) ^ seed);
      if (i > 32) {
        seed = rapidhash_mix(
            rapidhash_read64(p + 16) ^ secret[2], rapidhash_read64(p + 24) ^ seed);
        if (i > 48) {
          seed = rapidhash_mix(
              rapidhash_read64(p + 32) ^ secret[1], rapidhash_read64(p + 40) ^ seed);
          if (i > 64) {
            seed = rapidhash_mix(
                rapidhash_read64(p + 48) ^ secret[1], rapidhash_read64(p + 56) ^ seed);
          }
        }
      }
    }
    a = rapidhash_read64(p + i - 16) ^ i;
    b = rapidhash_read64(p + i - 8);
  }
  a ^= secret[1];
  b ^= seed;
  rapidhash_mum(&a, &b);
  return rapidhash_mix(a ^ secret[7], b ^ secret[1] ^ i);
}

FOLLY_EXTERNAL_RAPIDHASH_INLINE_CONSTEXPR uint64_t rapidhashNano_internal(
    const char* p, size_t len, uint64_t seed, const uint64_t* secret)
    noexcept {
  seed ^= rapidhash_mix(seed ^ secret[2], secret[1]);
  uint64_t a = 0, b = 0;
  size_t i = len;
  if (len <= 16) [[likely]] {
    if (len >= 4) {
      seed ^= len;
      if (len >= 8) {
        const char* plast = p + len - 8;
        a = rapidhash_read64(p);
        b = rapidhash_read64(plast);
      } else {
        const char* plast = p + len - 4;
        a = rapidhash_read32(p);
        b = rapidhash_read32(plast);
      }
    } else if (len > 0) {
      a = (static_cast<uint64_t>(p[0]) << 45) | static_cast<uint64_t>(p[len - 1]);
      b = static_cast<uint64_t>(p[len >> 1]);
    } else
      a = b = 0;
  } else {
    if (i > 48) {
      uint64_t see1 = seed, see2 = seed;
      do {
        seed =
            rapidhash_mix(rapidhash_read64(p) ^ secret[0], rapidhash_read64(p + 8) ^ seed);
        see1 = rapidhash_mix(
            rapidhash_read64(p + 16) ^ secret[1], rapidhash_read64(p + 24) ^ see1);
        see2 = rapidhash_mix(
            rapidhash_read64(p + 32) ^ secret[2], rapidhash_read64(p + 40) ^ see2);
        p += 48;
        i -= 48;
      } while (i > 48);
      seed ^= see1;
      seed ^= see2;
    }
    if (i > 16) {
      seed = rapidhash_mix(rapidhash_read64(p) ^ secret[2], rapidhash_read64(p + 8) ^ seed);
      if (i > 32) {
        seed = rapidhash_mix(
            rapidhash_read64(p + 16) ^ secret[2], rapidhash_read64(p + 24) ^ seed);
      }
    }
    a = rapidhash_read64(p + i - 16) ^ i;
    b = rapidhash_read64(p + i - 8);
  }
  a ^= secret[1];
  b ^= seed;
  rapidhash_mum(&a, &b);
  return rapidhash_mix(a ^ secret[7], b ^ secret[1] ^ i);
}

} // namespace rapidhash

/*
 *  rapidhash seeded hash function.
 *
 *  @param key     Buffer to be hashed.
 *  @param len     @key length, in bytes.
 *  @param seed    64-bit seed used to alter the hash result predictably.
 *
 *  Calls rapidhash_internal using provided parameters and default secrets.
 *
 *  Returns a 64-bit hash.
 */
FOLLY_EXTERNAL_RAPIDHASH_INLINE_CONSTEXPR uint64_t rapidhash_with_seed(
    const char* key, size_t len, uint64_t seed) noexcept {
  return rapidhash_detail::rapidhash_internal(key, len, seed, rapidhash_detail::rapidhash_secret);
}

/*
 *  rapidhash general purpose hash function.
 *
 *  @param key     Buffer to be hashed.
 *  @param len     @key length, in bytes.
 *
 *  Calls rapidhash_withSeed using provided parameters and the default seed.
 *
 *  Returns a 64-bit hash.
 */
FOLLY_EXTERNAL_RAPIDHASH_INLINE_CONSTEXPR uint64_t
rapidhash(const char* key, size_t len) noexcept {
  return rapidhash_with_seed(key, len, 0);
}

/*
 *  rapidhashMicro seeded hash function.
 *
 *  Designed for HPC and server applications, where cache misses make a
 * noticeable performance detriment. Clang-18+ compiles it to ~140 instructions
 * without stack usage, both on x86-64 and aarch64. Faster for sizes up to 512
 * bytes, just 15%-20% slower for inputs above 1kb.
 *
 *  @param key     Buffer to be hashed.
 *  @param len     @key length, in bytes.
 *  @param seed    64-bit seed used to alter the hash result predictably.
 *
 *  Calls rapidhash_internal using provided parameters and default secrets.
 *
 *  Returns a 64-bit hash.
 */
FOLLY_EXTERNAL_RAPIDHASH_INLINE_CONSTEXPR uint64_t rapidhashMicro_with_seed(
    const char* key, size_t len, uint64_t seed) noexcept {
  return rapidhash_detail::rapidhashMicro_internal(key, len, seed, rapidhash_detail::rapidhash_secret);
}

/*
 *  rapidhashMicro hash function.
 *
 *  @param key     Buffer to be hashed.
 *  @param len     @key length, in bytes.
 *
 *  Calls rapidhash_withSeed using provided parameters and the default seed.
 *
 *  Returns a 64-bit hash.
 */
FOLLY_EXTERNAL_RAPIDHASH_INLINE_CONSTEXPR uint64_t
rapidhashMicro(const char* key, size_t len) noexcept {
  return rapidhashMicro_with_seed(key, len, 0);
}

/*
 *  rapidhashNano seeded hash function.
 *
 *  @param key     Buffer to be hashed.
 *  @param len     @key length, in bytes.
 *  @param seed    64-bit seed used to alter the hash result predictably.
 *
 *  Calls rapidhash_internal using provided parameters and default secrets.
 *
 *  Returns a 64-bit hash.
 */
FOLLY_EXTERNAL_RAPIDHASH_INLINE_CONSTEXPR uint64_t rapidhashNano_with_seed(
    const char* key, size_t len, uint64_t seed) noexcept {
  return rapidhash_detail::rapidhashNano_internal(key, len, seed, rapidhash_detail::rapidhash_secret);
}

/*
 *  rapidhashNano hash function.
 *
 *  Designed for Mobile and embedded applications, where keeping a small code
 * size is a top priority. Clang-18+ compiles it to less than 100 instructions
 * without stack usage, both on x86-64 and aarch64. The fastest for sizes up to
 * 48 bytes, but may be considerably slower for larger inputs.
 *
 *  @param key     Buffer to be hashed.
 *  @param len     @key length, in bytes.
 *
 *  Calls rapidhash_withSeed using provided parameters and the default seed.
 *
 *  Returns a 64-bit hash.
 */
FOLLY_EXTERNAL_RAPIDHASH_INLINE_CONSTEXPR uint64_t
rapidhashNano(const char* key, size_t len) noexcept {
  return rapidhashNano_with_seed(key, len, 0);
}

} // namespace hash
} // namespace folly

FOLLY_UNSAFE_BUFFER_USAGE_END
