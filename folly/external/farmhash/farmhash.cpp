// Copyright (c) 2014 Google, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// FarmHash, by Geoff Pike

// clang-format off

#include <folly/external/farmhash/farmhash.h>

#include <folly/CPortability.h>

#include <exception>

FOLLY_UNSAFE_BUFFER_USAGE_BEGIN

// FARMHASH ASSUMPTIONS: Modify as needed, or use -DFARMHASH_ASSUME_SSE42 etc.
// Note that if you use -DFARMHASH_ASSUME_SSE42 you likely need -msse42
// (or its equivalent for your compiler); if you use -DFARMHASH_ASSUME_AESNI
// you likely need -maes (or its equivalent for your compiler).

#ifdef FARMHASH_ASSUME_SSSE3
#undef FARMHASH_ASSUME_SSSE3
#define FARMHASH_ASSUME_SSSE3 1
#endif

#ifdef FARMHASH_ASSUME_SSE41
#undef FARMHASH_ASSUME_SSE41
#define FARMHASH_ASSUME_SSE41 1
#endif

#ifdef FARMHASH_ASSUME_SSE42
#undef FARMHASH_ASSUME_SSE42
#define FARMHASH_ASSUME_SSE42 1
#endif

#ifdef FARMHASH_ASSUME_AESNI
#undef FARMHASH_ASSUME_AESNI
#define FARMHASH_ASSUME_AESNI 1
#endif

#ifdef FARMHASH_ASSUME_AVX
#undef FARMHASH_ASSUME_AVX
#define FARMHASH_ASSUME_AVX 1
#endif

#if !defined(FARMHASH_CAN_USE_CXX11) && defined(LANG_CXX11)
#define FARMHASH_CAN_USE_CXX11 1
#else
#undef FARMHASH_CAN_USE_CXX11
#define FARMHASH_CAN_USE_CXX11 0
#endif

// FARMHASH PORTABILITY LAYER: Runtime error if misconfigured

#ifndef FARMHASH_DIE_IF_MISCONFIGURED
#define FARMHASH_DIE_IF_MISCONFIGURED      \
  do {                                     \
    if (test::returnZeroIfMisconfigured) { \
      return 0;                            \
    } else {                               \
      std::terminate();                    \
    }                                      \
  } while (0)
#endif

// FARMHASH PORTABILITY LAYER: "static inline" or similar

#ifndef STATIC_INLINE
#define STATIC_INLINE static inline
#endif

// FARMHASH PORTABILITY LAYER: LIKELY and UNLIKELY

#if !defined(LIKELY)
#if defined(FARMHASH_NO_BUILTIN_EXPECT) || (defined(FARMHASH_OPTIONAL_BUILTIN_EXPECT) && !defined(HAVE_BUILTIN_EXPECT)) || !defined(__GNUC__)
#define LIKELY(x) (x)
#else
#define LIKELY(x) (__builtin_expect(!!(x), 1))
#endif
#endif

#undef UNLIKELY
#define UNLIKELY(x) !LIKELY(!(x))

// FARMHASH PORTABILITY LAYER: endianness and byteswapping functions

#ifdef WORDS_BIGENDIAN
#undef FARMHASH_BIG_ENDIAN
#define FARMHASH_BIG_ENDIAN 1
#endif

#if defined(FARMHASH_LITTLE_ENDIAN) && defined(FARMHASH_BIG_ENDIAN)
#error
#endif

#if !defined(FARMHASH_LITTLE_ENDIAN) && !defined(FARMHASH_BIG_ENDIAN)
#define FARMHASH_UNKNOWN_ENDIAN 1
#endif

#if !defined(bswap_32) || !defined(bswap_64)
#undef bswap_32
#undef bswap_64

#if defined(HAVE_BUILTIN_BSWAP) || defined(__clang__) ||                \
  (defined(__GNUC__) && ((__GNUC__ == 4 && __GNUC_MINOR__ >= 8) ||      \
                         __GNUC__ >= 5))
// Easy case for bswap: no header file needed.
#define bswap_32(x) __builtin_bswap32(x)
#define bswap_64(x) __builtin_bswap64(x)
#endif

#endif

#if defined(FARMHASH_UNKNOWN_ENDIAN) || !defined(bswap_64)

#ifdef _MSC_VER

#undef bswap_32
#undef bswap_64
#define bswap_32(x) _byteswap_ulong(x)
#define bswap_64(x) _byteswap_uint64(x)

#elif defined(__APPLE__)

// Mac OS X / Darwin features
#include <libkern/OSByteOrder.h>
#undef bswap_32
#undef bswap_64
#define bswap_32(x) OSSwapInt32(x)
#define bswap_64(x) OSSwapInt64(x)

#elif defined(__sun) || defined(sun)

#include <sys/byteorder.h>
#undef bswap_32
#undef bswap_64
#define bswap_32(x) BSWAP_32(x)
#define bswap_64(x) BSWAP_64(x)

#elif defined(__FreeBSD__)

#include <sys/endian.h>
#undef bswap_32
#undef bswap_64
#define bswap_32(x) bswap32(x)
#define bswap_64(x) bswap64(x)

#elif defined(__OpenBSD__)

#include <sys/types.h>
#undef bswap_32
#undef bswap_64
#define bswap_32(x) swap32(x)
#define bswap_64(x) swap64(x)

#elif defined(__NetBSD__)

#include <sys/types.h>
#include <machine/bswap.h>
#if defined(__BSWAP_RENAME) && !defined(__bswap_32)
#undef bswap_32
#undef bswap_64
#define bswap_32(x) bswap32(x)
#define bswap_64(x) bswap64(x)
#endif

#else

#undef bswap_32
#undef bswap_64
#include <byteswap.h>

#endif

#ifdef WORDS_BIGENDIAN
#define FARMHASH_BIG_ENDIAN 1
#endif

#endif

#ifdef FARMHASH_BIG_ENDIAN
#define uint32_in_expected_order(x) (bswap_32(x))
#define uint64_in_expected_order(x) (bswap_64(x))
#else
#define uint32_in_expected_order(x) (x)
#define uint64_in_expected_order(x) (x)
#endif

namespace folly {
namespace external {
namespace farmhash {

namespace test {
bool returnZeroIfMisconfigured = false;
}

STATIC_INLINE uint64_t Fetch64(const char *p) {
  uint64_t result;
  memcpy(&result, p, sizeof(result));
  return uint64_in_expected_order(result);
}

STATIC_INLINE uint32_t Fetch32(const char *p) {
  uint32_t result;
  memcpy(&result, p, sizeof(result));
  return uint32_in_expected_order(result);
}

STATIC_INLINE uint32_t Bswap32(uint32_t val) { return bswap_32(val); }
STATIC_INLINE uint64_t Bswap64(uint64_t val) { return bswap_64(val); }

// FARMHASH PORTABILITY LAYER: bitwise rot

STATIC_INLINE uint32_t BasicRotate32(uint32_t val, int shift) {
  // Avoid shifting by 32: doing so yields an undefined result.
  return shift == 0 ? val : ((val >> shift) | (val << (32 - shift)));
}

STATIC_INLINE uint64_t BasicRotate64(uint64_t val, int shift) {
  // Avoid shifting by 64: doing so yields an undefined result.
  return shift == 0 ? val : ((val >> shift) | (val << (64 - shift)));
}

#if defined(_MSC_VER) && defined(FARMHASH_ROTR)

STATIC_INLINE uint32_t Rotate32(uint32_t val, int shift) {
  return sizeof(unsigned long) == sizeof(val) ?
      _lrotr(val, shift) :
      BasicRotate32(val, shift);
}

STATIC_INLINE uint64_t Rotate64(uint64_t val, int shift) {
  return sizeof(unsigned long) == sizeof(val) ?
      _lrotr(val, shift) :
      BasicRotate64(val, shift);
}

#else

STATIC_INLINE uint32_t Rotate32(uint32_t val, int shift) {
  return BasicRotate32(val, shift);
}
STATIC_INLINE uint64_t Rotate64(uint64_t val, int shift) {
  return BasicRotate64(val, shift);
}

#endif

} // namespace farmhash
} // namespace external
} // namespace folly

// FARMHASH PORTABILITY LAYER: debug mode or max speed?
// One may use -DFARMHASH_DEBUG=1 or -DFARMHASH_DEBUG=0 to force the issue.

#if !defined(FARMHASH_DEBUG) && (!defined(NDEBUG) || defined(_DEBUG))
#define FARMHASH_DEBUG 1
#endif

#undef debug_mode
#if FARMHASH_DEBUG
#define debug_mode 1
#else
#define debug_mode 0
#endif

// PLATFORM-SPECIFIC FUNCTIONS AND MACROS

#undef x86_64
#if defined (__x86_64) || defined (__x86_64__)
#define x86_64 1
#else
#define x86_64 0
#endif

#undef x86
#if defined(__i386__) || defined(__i386) || defined(__X86__)
#define x86 1
#else
#define x86 x86_64
#endif

#if !defined(is_64bit)
#define is_64bit (x86_64 || (sizeof(void*) == 8))
#endif

#undef can_use_ssse3
#if defined(__SSSE3__) || defined(FARMHASH_ASSUME_SSSE3)

#include <immintrin.h>
#define can_use_ssse3 1
// Now we can use _mm_hsub_epi16 and so on.

#else
#define can_use_ssse3 0
#endif

#undef can_use_sse41
#if defined(__SSE4_1__) || defined(FARMHASH_ASSUME_SSE41)

#include <immintrin.h>
#define can_use_sse41 1
// Now we can use _mm_insert_epi64 and so on.

#else
#define can_use_sse41 0
#endif

#undef can_use_sse42
#if defined(__SSE4_2__) || defined(FARMHASH_ASSUME_SSE42)

#include <nmmintrin.h>
#define can_use_sse42 1
// Now we can use _mm_crc32_u{32,16,8}.  And on 64-bit platforms, _mm_crc32_u64.

#else
#define can_use_sse42 0
#endif

#undef can_use_aesni
#if defined(__AES__) || defined(FARMHASH_ASSUME_AESNI)

#include <wmmintrin.h>
#define can_use_aesni 1
// Now we can use _mm_aesimc_si128 and so on.

#else
#define can_use_aesni 0
#endif

#undef can_use_avx
#if defined(__AVX__) || defined(FARMHASH_ASSUME_AVX)

#include <immintrin.h>
#define can_use_avx 1

#else
#define can_use_avx 0
#endif

#if can_use_sse42 || (can_use_sse41 && x86_64)
STATIC_INLINE __m128i Fetch128(const char* s) {
  return _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
}
#endif
// Building blocks for hash functions

// std::swap() was in <algorithm> but is in <utility> from C++11 on.
#if !FARMHASH_CAN_USE_CXX11
#include <algorithm>
#endif

#undef PERMUTE3
#define PERMUTE3(a, b, c) do { std::swap(a, b); std::swap(a, c); } while (0)

namespace folly {
namespace external {
namespace farmhash {

// Some primes between 2^63 and 2^64 for various uses.
static const uint64_t k0 = 0xc3a5c85c97cb3127ULL;
static const uint64_t k1 = 0xb492b66fbe98f273ULL;
static const uint64_t k2 = 0x9ae16a3b2f90404fULL;

// Magic numbers for 32-bit hashing.  Copied from Murmur3.
static const uint32_t c1 = 0xcc9e2d51;
static const uint32_t c2 = 0x1b873593;

// A 32-bit to 32-bit integer hash copied from Murmur3.
STATIC_INLINE uint32_t fmix(uint32_t h)
{
  h ^= h >> 16;
  h *= 0x85ebca6b;
  h ^= h >> 13;
  h *= 0xc2b2ae35;
  h ^= h >> 16;
  return h;
}

STATIC_INLINE uint32_t Mur(uint32_t a, uint32_t h) {
  // Helper from Murmur3 for combining two 32-bit values.
  a *= c1;
  a = Rotate32(a, 17);
  a *= c2;
  h ^= a;
  h = Rotate32(h, 19);
  return h * 5 + 0xe6546b64;
}

template <typename T> STATIC_INLINE T DebugTweak(T x) {
  if (debug_mode) {
    if (sizeof(x) == 4) {
      x = ~Bswap32(x * c1);
    } else {
      x = ~Bswap64(x * k1);
    }
  }
  return x;
}

template <> uint128_t DebugTweak(uint128_t x) {
  if (debug_mode) {
    uint64_t y = DebugTweak(Uint128Low64(x));
    uint64_t z = DebugTweak(Uint128High64(x));
    y += z;
    z += y;
    x = Uint128(y, z * k1);
  }
  return x;
}

using namespace std;

namespace farmhashna {
#undef Fetch
#define Fetch Fetch64

#undef Rotate
#define Rotate Rotate64

#undef Bswap
#define Bswap Bswap64

STATIC_INLINE uint64_t ShiftMix(uint64_t val) {
  return val ^ (val >> 47);
}

STATIC_INLINE uint64_t HashLen16(uint64_t u, uint64_t v) {
  return Hash128to64(Uint128(u, v));
}

STATIC_INLINE uint64_t HashLen16(uint64_t u, uint64_t v, uint64_t mul) {
  // Murmur-inspired hashing.
  uint64_t a = (u ^ v) * mul;
  a ^= (a >> 47);
  uint64_t b = (v ^ a) * mul;
  b ^= (b >> 47);
  b *= mul;
  return b;
}

STATIC_INLINE uint64_t HashLen0to16(const char *s, size_t len) {
  if (len >= 8) {
    uint64_t mul = k2 + len * 2;
    uint64_t a = Fetch(s) + k2;
    uint64_t b = Fetch(s + len - 8);
    uint64_t c = Rotate(b, 37) * mul + a;
    uint64_t d = (Rotate(a, 25) + b) * mul;
    return HashLen16(c, d, mul);
  }
  if (len >= 4) {
    uint64_t mul = k2 + len * 2;
    uint64_t a = Fetch32(s);
    return HashLen16(len + (a << 3), Fetch32(s + len - 4), mul);
  }
  if (len > 0) {
    uint8_t a = s[0];
    uint8_t b = s[len >> 1];
    uint8_t c = s[len - 1];
    uint32_t y = static_cast<uint32_t>(a) + (static_cast<uint32_t>(b) << 8);
    uint32_t z = len + (static_cast<uint32_t>(c) << 2);
    return ShiftMix(y * k2 ^ z * k0) * k2;
  }
  return k2;
}

// This probably works well for 16-byte strings as well, but it may be overkill
// in that case.
STATIC_INLINE uint64_t HashLen17to32(const char *s, size_t len) {
  uint64_t mul = k2 + len * 2;
  uint64_t a = Fetch(s) * k1;
  uint64_t b = Fetch(s + 8);
  uint64_t c = Fetch(s + len - 8) * mul;
  uint64_t d = Fetch(s + len - 16) * k2;
  return HashLen16(Rotate(a + b, 43) + Rotate(c, 30) + d,
                   a + Rotate(b + k2, 18) + c, mul);
}

// Return a 16-byte hash for 48 bytes.  Quick and dirty.
// Callers do best to use "random-looking" values for a and b.
STATIC_INLINE pair<uint64_t, uint64_t> WeakHashLen32WithSeeds(
    uint64_t w, uint64_t x, uint64_t y, uint64_t z, uint64_t a, uint64_t b) {
  a += w;
  b = Rotate(b + a + z, 21);
  uint64_t c = a;
  a += x;
  a += y;
  b += Rotate(a, 44);
  return make_pair(a + z, b + c);
}

// Return a 16-byte hash for s[0] ... s[31], a, and b.  Quick and dirty.
STATIC_INLINE pair<uint64_t, uint64_t> WeakHashLen32WithSeeds(
    const char* s, uint64_t a, uint64_t b) {
  return WeakHashLen32WithSeeds(Fetch(s),
                                Fetch(s + 8),
                                Fetch(s + 16),
                                Fetch(s + 24),
                                a,
                                b);
}

// Return an 8-byte hash for 33 to 64 bytes.
STATIC_INLINE uint64_t HashLen33to64(const char *s, size_t len) {
  uint64_t mul = k2 + len * 2;
  uint64_t a = Fetch(s) * k2;
  uint64_t b = Fetch(s + 8);
  uint64_t c = Fetch(s + len - 8) * mul;
  uint64_t d = Fetch(s + len - 16) * k2;
  uint64_t y = Rotate(a + b, 43) + Rotate(c, 30) + d;
  uint64_t z = HashLen16(y, a + Rotate(b + k2, 18) + c, mul);
  uint64_t e = Fetch(s + 16) * mul;
  uint64_t f = Fetch(s + 24);
  uint64_t g = (y + Fetch(s + len - 32)) * mul;
  uint64_t h = (z + Fetch(s + len - 24)) * mul;
  return HashLen16(Rotate(e + f, 43) + Rotate(g, 30) + h,
                   e + Rotate(f + a, 18) + g, mul);
}

uint64_t Hash64(const char *s, size_t len) {
  const uint64_t seed = 81;
  if (len <= 32) {
    if (len <= 16) {
      return HashLen0to16(s, len);
    } else {
      return HashLen17to32(s, len);
    }
  } else if (len <= 64) {
    return HashLen33to64(s, len);
  }

  // For strings over 64 bytes we loop.  Internal state consists of
  // 56 bytes: v, w, x, y, and z.
  uint64_t x = seed;
  uint64_t y = seed * k1 + 113;
  uint64_t z = ShiftMix(y * k2 + 113) * k2;
  pair<uint64_t, uint64_t> v = make_pair(0, 0);
  pair<uint64_t, uint64_t> w = make_pair(0, 0);
  x = x * k2 + Fetch(s);

  // Set end so that after the loop we have 1 to 64 bytes left to process.
  const char* end = s + ((len - 1) / 64) * 64;
  const char* last64 = end + ((len - 1) & 63) - 63;
  assert(s + len - 64 == last64);
  do {
    x = Rotate(x + y + v.first + Fetch(s + 8), 37) * k1;
    y = Rotate(y + v.second + Fetch(s + 48), 42) * k1;
    x ^= w.second;
    y += v.first + Fetch(s + 40);
    z = Rotate(z + w.first, 33) * k1;
    v = WeakHashLen32WithSeeds(s, v.second * k1, x + w.first);
    w = WeakHashLen32WithSeeds(s + 32, z + w.second, y + Fetch(s + 16));
    std::swap(z, x);
    s += 64;
  } while (s != end);
  uint64_t mul = k1 + ((z & 0xff) << 1);
  // Make s point to the last 64 bytes of input.
  s = last64;
  w.first += ((len - 1) & 63);
  v.first += w.first;
  w.first += v.first;
  x = Rotate(x + y + v.first + Fetch(s + 8), 37) * mul;
  y = Rotate(y + v.second + Fetch(s + 48), 42) * mul;
  x ^= w.second * 9;
  y += v.first * 9 + Fetch(s + 40);
  z = Rotate(z + w.first, 33) * mul;
  v = WeakHashLen32WithSeeds(s, v.second * mul, x + w.first);
  w = WeakHashLen32WithSeeds(s + 32, z + w.second, y + Fetch(s + 16));
  std::swap(z, x);
  return HashLen16(HashLen16(v.first, w.first, mul) + ShiftMix(y) * k0 + z,
                   HashLen16(v.second, w.second, mul) + x,
                   mul);
}

uint64_t Hash64WithSeeds(const char *s, size_t len, uint64_t seed0, uint64_t seed1);

uint64_t Hash64WithSeed(const char *s, size_t len, uint64_t seed) {
  return Hash64WithSeeds(s, len, k2, seed);
}

uint64_t Hash64WithSeeds(const char *s, size_t len, uint64_t seed0, uint64_t seed1) {
  return HashLen16(Hash64(s, len) - seed0, seed1);
}
}  // namespace farmhashna
namespace farmhashuo {
#undef Fetch
#define Fetch Fetch64

#undef Rotate
#define Rotate Rotate64

STATIC_INLINE uint64_t H(uint64_t x, uint64_t y, uint64_t mul, int r) {
  uint64_t a = (x ^ y) * mul;
  a ^= (a >> 47);
  uint64_t b = (y ^ a) * mul;
  return Rotate(b, r) * mul;
}

uint64_t Hash64WithSeeds(const char *s, size_t len,
                         uint64_t seed0, uint64_t seed1) {
  if (len <= 64) {
    return farmhashna::Hash64WithSeeds(s, len, seed0, seed1);
  }

  // For strings over 64 bytes we loop.  Internal state consists of
  // 64 bytes: u, v, w, x, y, and z.
  uint64_t x = seed0;
  uint64_t y = seed1 * k2 + 113;
  uint64_t z = farmhashna::ShiftMix(y * k2) * k2;
  pair<uint64_t, uint64_t> v = make_pair(seed0, seed1);
  pair<uint64_t, uint64_t> w = make_pair(0, 0);
  uint64_t u = x - z;
  x *= k2;
  uint64_t mul = k2 + (u & 0x82);

  // Set end so that after the loop we have 1 to 64 bytes left to process.
  const char* end = s + ((len - 1) / 64) * 64;
  const char* last64 = end + ((len - 1) & 63) - 63;
  assert(s + len - 64 == last64);
  do {
    uint64_t a0 = Fetch(s);
    uint64_t a1 = Fetch(s + 8);
    uint64_t a2 = Fetch(s + 16);
    uint64_t a3 = Fetch(s + 24);
    uint64_t a4 = Fetch(s + 32);
    uint64_t a5 = Fetch(s + 40);
    uint64_t a6 = Fetch(s + 48);
    uint64_t a7 = Fetch(s + 56);
    x += a0 + a1;
    y += a2;
    z += a3;
    v.first += a4;
    v.second += a5 + a1;
    w.first += a6;
    w.second += a7;

    x = Rotate(x, 26);
    x *= 9;
    y = Rotate(y, 29);
    z *= mul;
    v.first = Rotate(v.first, 33);
    v.second = Rotate(v.second, 30);
    w.first ^= x;
    w.first *= 9;
    z = Rotate(z, 32);
    z += w.second;
    w.second += z;
    z *= 9;
    std::swap(u, y);

    z += a0 + a6;
    v.first += a2;
    v.second += a3;
    w.first += a4;
    w.second += a5 + a6;
    x += a1;
    y += a7;

    y += v.first;
    v.first += x - y;
    v.second += w.first;
    w.first += v.second;
    w.second += x - y;
    x += w.second;
    w.second = Rotate(w.second, 34);
    std::swap(u, z);
    s += 64;
  } while (s != end);
  // Make s point to the last 64 bytes of input.
  s = last64;
  u *= 9;
  v.second = Rotate(v.second, 28);
  v.first = Rotate(v.first, 20);
  w.first += ((len - 1) & 63);
  u += y;
  y += u;
  x = Rotate(y - x + v.first + Fetch(s + 8), 37) * mul;
  y = Rotate(y ^ v.second ^ Fetch(s + 48), 42) * mul;
  x ^= w.second * 9;
  y += v.first + Fetch(s + 40);
  z = Rotate(z + w.first, 33) * mul;
  v = farmhashna::WeakHashLen32WithSeeds(s, v.second * mul, x + w.first);
  w = farmhashna::WeakHashLen32WithSeeds(s + 32, z + w.second, y + Fetch(s + 16));
  return H(farmhashna::HashLen16(v.first + x, w.first ^ y, mul) + z - u,
           H(v.second + y, w.second + z, k2, 30) ^ x,
           k2,
           31);
}

uint64_t Hash64WithSeed(const char *s, size_t len, uint64_t seed) {
  return len <= 64 ? farmhashna::Hash64WithSeed(s, len, seed) :
      Hash64WithSeeds(s, len, 0, seed);
}

uint64_t Hash64(const char *s, size_t len) {
  return len <= 64 ? farmhashna::Hash64(s, len) :
      Hash64WithSeeds(s, len, 81, 0);
}
}  // namespace farmhashuo
namespace farmhashxo {
#undef Fetch
#define Fetch Fetch64

#undef Rotate
#define Rotate Rotate64

STATIC_INLINE uint64_t H32(const char *s, size_t len, uint64_t mul,
                           uint64_t seed0 = 0, uint64_t seed1 = 0) {
  uint64_t a = Fetch(s) * k1;
  uint64_t b = Fetch(s + 8);
  uint64_t c = Fetch(s + len - 8) * mul;
  uint64_t d = Fetch(s + len - 16) * k2;
  uint64_t u = Rotate(a + b, 43) + Rotate(c, 30) + d + seed0;
  uint64_t v = a + Rotate(b + k2, 18) + c + seed1;
  a = farmhashna::ShiftMix((u ^ v) * mul);
  b = farmhashna::ShiftMix((v ^ a) * mul);
  return b;
}

// Return an 8-byte hash for 33 to 64 bytes.
STATIC_INLINE uint64_t HashLen33to64(const char *s, size_t len) {
  uint64_t mul0 = k2 - 30;
  uint64_t mul1 = k2 - 30 + 2 * len;
  uint64_t h0 = H32(s, 32, mul0);
  uint64_t h1 = H32(s + len - 32, 32, mul1);
  return ((h1 * mul1) + h0) * mul1;
}

// Return an 8-byte hash for 65 to 96 bytes.
STATIC_INLINE uint64_t HashLen65to96(const char *s, size_t len) {
  uint64_t mul0 = k2 - 114;
  uint64_t mul1 = k2 - 114 + 2 * len;
  uint64_t h0 = H32(s, 32, mul0);
  uint64_t h1 = H32(s + 32, 32, mul1);
  uint64_t h2 = H32(s + len - 32, 32, mul1, h0, h1);
  return (h2 * 9 + (h0 >> 17) + (h1 >> 21)) * mul1;
}

uint64_t Hash64(const char *s, size_t len) {
  if (len <= 32) {
    if (len <= 16) {
      return farmhashna::HashLen0to16(s, len);
    } else {
      return farmhashna::HashLen17to32(s, len);
    }
  } else if (len <= 64) {
    return HashLen33to64(s, len);
  } else if (len <= 96) {
    return HashLen65to96(s, len);
  } else if (len <= 256) {
    return farmhashna::Hash64(s, len);
  } else {
    return farmhashuo::Hash64(s, len);
  }
}

uint64_t Hash64WithSeeds(const char *s, size_t len, uint64_t seed0, uint64_t seed1) {
  return farmhashuo::Hash64WithSeeds(s, len, seed0, seed1);
}

uint64_t Hash64WithSeed(const char *s, size_t len, uint64_t seed) {
  return farmhashuo::Hash64WithSeed(s, len, seed);
}
}  // namespace farmhashxo
namespace farmhashte {
#if !can_use_sse41 || !x86_64

uint64_t Hash64(const char *s, size_t len) {
  FARMHASH_DIE_IF_MISCONFIGURED;
  return s == NULL ? 0 : len;
}

uint64_t Hash64WithSeed(const char *s, size_t len, uint64_t seed) {
  FARMHASH_DIE_IF_MISCONFIGURED;
  return seed + Hash64(s, len);
}

uint64_t Hash64WithSeeds(const char *s, size_t len,
                         uint64_t seed0, uint64_t seed1) {
  FARMHASH_DIE_IF_MISCONFIGURED;
  return seed0 + seed1 + Hash64(s, len);
}

#else

#undef Fetch
#define Fetch Fetch64

#undef Rotate
#define Rotate Rotate64

#undef Bswap
#define Bswap Bswap64

// Helpers for data-parallel operations (1x 128 bits or 2x 64 or 4x 32).
STATIC_INLINE __m128i Add(__m128i x, __m128i y) { return _mm_add_epi64(x, y); }
STATIC_INLINE __m128i Xor(__m128i x, __m128i y) { return _mm_xor_si128(x, y); }
STATIC_INLINE __m128i Mul(__m128i x, __m128i y) { return _mm_mullo_epi32(x, y); }
STATIC_INLINE __m128i Shuf(__m128i x, __m128i y) { return _mm_shuffle_epi8(y, x); }

// Requires n >= 256.  Requires SSE4.1. Should be slightly faster if the
// compiler uses AVX instructions (e.g., use the -mavx flag with GCC).
STATIC_INLINE uint64_t Hash64Long(const char* s, size_t n,
                                  uint64_t seed0, uint64_t seed1) {
  const __m128i kShuf =
      _mm_set_epi8(4, 11, 10, 5, 8, 15, 6, 9, 12, 2, 14, 13, 0, 7, 3, 1);
  const __m128i kMult =
      _mm_set_epi8(0xbd, 0xd6, 0x33, 0x39, 0x45, 0x54, 0xfa, 0x03,
                   0x34, 0x3e, 0x33, 0xed, 0xcc, 0x9e, 0x2d, 0x51);
  uint64_t seed2 = (seed0 + 113) * (seed1 + 9);
  uint64_t seed3 = (Rotate(seed0, 23) + 27) * (Rotate(seed1, 30) + 111);
  __m128i d0 = _mm_cvtsi64_si128(seed0);
  __m128i d1 = _mm_cvtsi64_si128(seed1);
  __m128i d2 = Shuf(kShuf, d0);
  __m128i d3 = Shuf(kShuf, d1);
  __m128i d4 = Xor(d0, d1);
  __m128i d5 = Xor(d1, d2);
  __m128i d6 = Xor(d2, d4);
  __m128i d7 = _mm_set1_epi32(seed2 >> 32);
  __m128i d8 = Mul(kMult, d2);
  __m128i d9 = _mm_set1_epi32(seed3 >> 32);
  __m128i d10 = _mm_set1_epi32(seed3);
  __m128i d11 = Add(d2, _mm_set1_epi32(seed2));
  const char* end = s + (n & ~static_cast<size_t>(255));
  do {
    __m128i z;
    z = Fetch128(s);
    d0 = Add(d0, z);
    d1 = Shuf(kShuf, d1);
    d2 = Xor(d2, d0);
    d4 = Xor(d4, z);
    d4 = Xor(d4, d1);
    std::swap(d0, d6);
    z = Fetch128(s + 16);
    d5 = Add(d5, z);
    d6 = Shuf(kShuf, d6);
    d8 = Shuf(kShuf, d8);
    d7 = Xor(d7, d5);
    d0 = Xor(d0, z);
    d0 = Xor(d0, d6);
    std::swap(d5, d11);
    z = Fetch128(s + 32);
    d1 = Add(d1, z);
    d2 = Shuf(kShuf, d2);
    d4 = Shuf(kShuf, d4);
    d5 = Xor(d5, z);
    d5 = Xor(d5, d2);
    std::swap(d10, d4);
    z = Fetch128(s + 48);
    d6 = Add(d6, z);
    d7 = Shuf(kShuf, d7);
    d0 = Shuf(kShuf, d0);
    d8 = Xor(d8, d6);
    d1 = Xor(d1, z);
    d1 = Add(d1, d7);
    z = Fetch128(s + 64);
    d2 = Add(d2, z);
    d5 = Shuf(kShuf, d5);
    d4 = Add(d4, d2);
    d6 = Xor(d6, z);
    d6 = Xor(d6, d11);
    std::swap(d8, d2);
    z = Fetch128(s + 80);
    d7 = Xor(d7, z);
    d8 = Shuf(kShuf, d8);
    d1 = Shuf(kShuf, d1);
    d0 = Add(d0, d7);
    d2 = Add(d2, z);
    d2 = Add(d2, d8);
    std::swap(d1, d7);
    z = Fetch128(s + 96);
    d4 = Shuf(kShuf, d4);
    d6 = Shuf(kShuf, d6);
    d8 = Mul(kMult, d8);
    d5 = Xor(d5, d11);
    d7 = Xor(d7, z);
    d7 = Add(d7, d4);
    std::swap(d6, d0);
    z = Fetch128(s + 112);
    d8 = Add(d8, z);
    d0 = Shuf(kShuf, d0);
    d2 = Shuf(kShuf, d2);
    d1 = Xor(d1, d8);
    d10 = Xor(d10, z);
    d10 = Xor(d10, d0);
    std::swap(d11, d5);
    z = Fetch128(s + 128);
    d4 = Add(d4, z);
    d5 = Shuf(kShuf, d5);
    d7 = Shuf(kShuf, d7);
    d6 = Add(d6, d4);
    d8 = Xor(d8, z);
    d8 = Xor(d8, d5);
    std::swap(d4, d10);
    z = Fetch128(s + 144);
    d0 = Add(d0, z);
    d1 = Shuf(kShuf, d1);
    d2 = Add(d2, d0);
    d4 = Xor(d4, z);
    d4 = Xor(d4, d1);
    z = Fetch128(s + 160);
    d5 = Add(d5, z);
    d6 = Shuf(kShuf, d6);
    d8 = Shuf(kShuf, d8);
    d7 = Xor(d7, d5);
    d0 = Xor(d0, z);
    d0 = Xor(d0, d6);
    std::swap(d2, d8);
    z = Fetch128(s + 176);
    d1 = Add(d1, z);
    d2 = Shuf(kShuf, d2);
    d4 = Shuf(kShuf, d4);
    d5 = Mul(kMult, d5);
    d5 = Xor(d5, z);
    d5 = Xor(d5, d2);
    std::swap(d7, d1);
    z = Fetch128(s + 192);
    d6 = Add(d6, z);
    d7 = Shuf(kShuf, d7);
    d0 = Shuf(kShuf, d0);
    d8 = Add(d8, d6);
    d1 = Xor(d1, z);
    d1 = Xor(d1, d7);
    std::swap(d0, d6);
    z = Fetch128(s + 208);
    d2 = Add(d2, z);
    d5 = Shuf(kShuf, d5);
    d4 = Xor(d4, d2);
    d6 = Xor(d6, z);
    d6 = Xor(d6, d9);
    std::swap(d5, d11);
    z = Fetch128(s + 224);
    d7 = Add(d7, z);
    d8 = Shuf(kShuf, d8);
    d1 = Shuf(kShuf, d1);
    d0 = Xor(d0, d7);
    d2 = Xor(d2, z);
    d2 = Xor(d2, d8);
    std::swap(d10, d4);
    z = Fetch128(s + 240);
    d3 = Add(d3, z);
    d4 = Shuf(kShuf, d4);
    d6 = Shuf(kShuf, d6);
    d7 = Mul(kMult, d7);
    d5 = Add(d5, d3);
    d7 = Xor(d7, z);
    d7 = Xor(d7, d4);
    std::swap(d3, d9);
    s += 256;
  } while (s != end);
  d6 = Add(Mul(kMult, d6), _mm_cvtsi64_si128(n));
  if (n % 256 != 0) {
    d7 = Add(_mm_shuffle_epi32(d8, (0 << 6) + (3 << 4) + (2 << 2) + (1 << 0)), d7);
    d8 = Add(Mul(kMult, d8), _mm_cvtsi64_si128(farmhashxo::Hash64(s, n % 256)));
  }
  __m128i t[8];
  d0 = Mul(kMult, Shuf(kShuf, Mul(kMult, d0)));
  d3 = Mul(kMult, Shuf(kShuf, Mul(kMult, d3)));
  d9 = Mul(kMult, Shuf(kShuf, Mul(kMult, d9)));
  d1 = Mul(kMult, Shuf(kShuf, Mul(kMult, d1)));
  d0 = Add(d11, d0);
  d3 = Xor(d7, d3);
  d9 = Add(d8, d9);
  d1 = Add(d10, d1);
  d4 = Add(d3, d4);
  d5 = Add(d9, d5);
  d6 = Xor(d1, d6);
  d2 = Add(d0, d2);
  t[0] = d0;
  t[1] = d3;
  t[2] = d9;
  t[3] = d1;
  t[4] = d4;
  t[5] = d5;
  t[6] = d6;
  t[7] = d2;
  return farmhashxo::Hash64(reinterpret_cast<const char*>(t), sizeof(t));
}

uint64_t Hash64(const char *s, size_t len) {
  // Empirically, farmhashxo seems faster until length 512.
  return len >= 512 ? Hash64Long(s, len, k2, k1) : farmhashxo::Hash64(s, len);
}

uint64_t Hash64WithSeed(const char *s, size_t len, uint64_t seed) {
  return len >= 512 ? Hash64Long(s, len, k1, seed) :
      farmhashxo::Hash64WithSeed(s, len, seed);
}

uint64_t Hash64WithSeeds(const char *s, size_t len, uint64_t seed0, uint64_t seed1) {
  return len >= 512 ? Hash64Long(s, len, seed0, seed1) :
      farmhashxo::Hash64WithSeeds(s, len, seed0, seed1);
}

#endif
}  // namespace farmhashte
namespace farmhashnt {
#if !can_use_sse41 || !x86_64

uint32_t Hash32(const char *s, size_t len) {
  FARMHASH_DIE_IF_MISCONFIGURED;
  return s == NULL ? 0 : len;
}

uint32_t Hash32WithSeed(const char *s, size_t len, uint32_t seed) {
  FARMHASH_DIE_IF_MISCONFIGURED;
  return seed + Hash32(s, len);
}

#else

uint32_t Hash32(const char *s, size_t len) {
  return static_cast<uint32_t>(farmhashte::Hash64(s, len));
}

uint32_t Hash32WithSeed(const char *s, size_t len, uint32_t seed) {
  return static_cast<uint32_t>(farmhashte::Hash64WithSeed(s, len, seed));
}

#endif
}  // namespace farmhashnt
namespace farmhashmk {
#undef Fetch
#define Fetch Fetch32

#undef Rotate
#define Rotate Rotate32

#undef Bswap
#define Bswap Bswap32

STATIC_INLINE uint32_t Hash32Len13to24(const char *s, size_t len, uint32_t seed = 0) {
  uint32_t a = Fetch(s - 4 + (len >> 1));
  uint32_t b = Fetch(s + 4);
  uint32_t c = Fetch(s + len - 8);
  uint32_t d = Fetch(s + (len >> 1));
  uint32_t e = Fetch(s);
  uint32_t f = Fetch(s + len - 4);
  uint32_t h = d * c1 + len + seed;
  a = Rotate(a, 12) + f;
  h = Mur(c, h) + a;
  a = Rotate(a, 3) + c;
  h = Mur(e, h) + a;
  a = Rotate(a + f, 12) + d;
  h = Mur(b ^ seed, h) + a;
  return fmix(h);
}

STATIC_INLINE uint32_t Hash32Len0to4(const char *s, size_t len, uint32_t seed = 0) {
  uint32_t b = seed;
  uint32_t c = 9;
  for (size_t i = 0; i < len; i++) {
    signed char v = s[i];
    b = b * c1 + v;
    c ^= b;
  }
  return fmix(Mur(b, Mur(len, c)));
}

STATIC_INLINE uint32_t Hash32Len5to12(const char *s, size_t len, uint32_t seed = 0) {
  uint32_t a = len, b = len * 5, c = 9, d = b + seed;
  a += Fetch(s);
  b += Fetch(s + len - 4);
  c += Fetch(s + ((len >> 1) & 4));
  return fmix(seed ^ Mur(c, Mur(b, Mur(a, d))));
}

uint32_t Hash32(const char *s, size_t len) {
  if (len <= 24) {
    return len <= 12 ?
        (len <= 4 ? Hash32Len0to4(s, len) : Hash32Len5to12(s, len)) :
        Hash32Len13to24(s, len);
  }

  // len > 24
  uint32_t h = len, g = c1 * len, f = g;
  uint32_t a0 = Rotate(Fetch(s + len - 4) * c1, 17) * c2;
  uint32_t a1 = Rotate(Fetch(s + len - 8) * c1, 17) * c2;
  uint32_t a2 = Rotate(Fetch(s + len - 16) * c1, 17) * c2;
  uint32_t a3 = Rotate(Fetch(s + len - 12) * c1, 17) * c2;
  uint32_t a4 = Rotate(Fetch(s + len - 20) * c1, 17) * c2;
  h ^= a0;
  h = Rotate(h, 19);
  h = h * 5 + 0xe6546b64;
  h ^= a2;
  h = Rotate(h, 19);
  h = h * 5 + 0xe6546b64;
  g ^= a1;
  g = Rotate(g, 19);
  g = g * 5 + 0xe6546b64;
  g ^= a3;
  g = Rotate(g, 19);
  g = g * 5 + 0xe6546b64;
  f += a4;
  f = Rotate(f, 19) + 113;
  size_t iters = (len - 1) / 20;
  do {
    uint32_t a = Fetch(s);
    uint32_t b = Fetch(s + 4);
    uint32_t c = Fetch(s + 8);
    uint32_t d = Fetch(s + 12);
    uint32_t e = Fetch(s + 16);
    h += a;
    g += b;
    f += c;
    h = Mur(d, h) + e;
    g = Mur(c, g) + a;
    f = Mur(b + e * c1, f) + d;
    f += g;
    g += f;
    s += 20;
  } while (--iters != 0);
  g = Rotate(g, 11) * c1;
  g = Rotate(g, 17) * c1;
  f = Rotate(f, 11) * c1;
  f = Rotate(f, 17) * c1;
  h = Rotate(h + g, 19);
  h = h * 5 + 0xe6546b64;
  h = Rotate(h, 17) * c1;
  h = Rotate(h + f, 19);
  h = h * 5 + 0xe6546b64;
  h = Rotate(h, 17) * c1;
  return h;
}

uint32_t Hash32WithSeed(const char *s, size_t len, uint32_t seed) {
  if (len <= 24) {
    if (len >= 13) return Hash32Len13to24(s, len, seed * c1);
    else if (len >= 5) return Hash32Len5to12(s, len, seed);
    else return Hash32Len0to4(s, len, seed);
  }
  uint32_t h = Hash32Len13to24(s, 24, seed ^ len);
  return Mur(Hash32(s + 24, len - 24) + seed, h);
}
}  // namespace farmhashmk
namespace farmhashsu {
#if !can_use_sse42 || !can_use_aesni

uint32_t Hash32(const char *s, size_t len) {
  FARMHASH_DIE_IF_MISCONFIGURED;
  return s == nullptr ? 0 : len;
}

uint32_t Hash32WithSeed(const char *s, size_t len, uint32_t seed) {
  FARMHASH_DIE_IF_MISCONFIGURED;
  return seed + Hash32(s, len);
}

#else

#undef Fetch
#define Fetch Fetch32

#undef Rotate
#define Rotate Rotate32

#undef Bswap
#define Bswap Bswap32

// Helpers for data-parallel operations (4x 32-bits).
STATIC_INLINE __m128i Add(__m128i x, __m128i y) { return _mm_add_epi32(x, y); }
STATIC_INLINE __m128i Xor(__m128i x, __m128i y) { return _mm_xor_si128(x, y); }
STATIC_INLINE __m128i Or(__m128i x, __m128i y) { return _mm_or_si128(x, y); }
STATIC_INLINE __m128i Mul(__m128i x, __m128i y) { return _mm_mullo_epi32(x, y); }
STATIC_INLINE __m128i Mul5(__m128i x) { return Add(x, _mm_slli_epi32(x, 2)); }
STATIC_INLINE __m128i RotateLeft(__m128i x, int c) {
  return Or(_mm_slli_epi32(x, c),
            _mm_srli_epi32(x, 32 - c));
}
STATIC_INLINE __m128i Rol17(__m128i x) { return RotateLeft(x, 17); }
STATIC_INLINE __m128i Rol19(__m128i x) { return RotateLeft(x, 19); }
STATIC_INLINE __m128i Shuffle0321(__m128i x) {
  return _mm_shuffle_epi32(x, (0 << 6) + (3 << 4) + (2 << 2) + (1 << 0));
}

uint32_t Hash32(const char *s, size_t len) {
  const uint32_t seed = 81;
  if (len <= 24) {
    return len <= 12 ?
        (len <= 4 ?
         farmhashmk::Hash32Len0to4(s, len) :
         farmhashmk::Hash32Len5to12(s, len)) :
        farmhashmk::Hash32Len13to24(s, len);
  }

  if (len < 40) {
    uint32_t a = len, b = seed * c2, c = a + b;
    a += Fetch(s + len - 4);
    b += Fetch(s + len - 20);
    c += Fetch(s + len - 16);
    uint32_t d = a;
    a = folly::external::farmhash::Rotate32(a, 21);
    a = Mur(a, Mur(b, _mm_crc32_u32(c, d)));
    a += Fetch(s + len - 12);
    b += Fetch(s + len - 8);
    d += a;
    a += d;
    b = Mur(b, d) * c2;
    a = _mm_crc32_u32(a, b + c);
    return farmhashmk::Hash32Len13to24(s, (len + 1) / 2, a) + b;
  }

#undef Mulc1
#define Mulc1(x) Mul((x), cc1)

#undef Mulc2
#define Mulc2(x) Mul((x), cc2)

#undef Murk
#define Murk(a, h)                              \
  Add(k,                                        \
      Mul5(                                     \
          Rol19(                                \
              Xor(                              \
                  Mulc2(                        \
                      Rol17(                    \
                          Mulc1(a))),           \
                  (h)))))

  const __m128i cc1 = _mm_set1_epi32(c1);
  const __m128i cc2 = _mm_set1_epi32(c2);
  __m128i h = _mm_set1_epi32(seed);
  __m128i g = _mm_set1_epi32(c1 * seed);
  __m128i f = g;
  __m128i k = _mm_set1_epi32(0xe6546b64);
  __m128i q;
  if (len < 80) {
    __m128i a = Fetch128(s);
    __m128i b = Fetch128(s + 16);
    __m128i c = Fetch128(s + (len - 15) / 2);
    __m128i d = Fetch128(s + len - 32);
    __m128i e = Fetch128(s + len - 16);
    h = Add(h, a);
    g = Add(g, b);
    q = g;
    g = Shuffle0321(g);
    f = Add(f, c);
    __m128i be = Add(b, Mulc1(e));
    h = Add(h, f);
    f = Add(f, h);
    h = Add(Murk(d, h), e);
    k = Xor(k, _mm_shuffle_epi8(g, f));
    g = Add(Xor(c, g), a);
    f = Add(Xor(be, f), d);
    k = Add(k, be);
    k = Add(k, _mm_shuffle_epi8(f, h));
    f = Add(f, g);
    g = Add(g, f);
    g = Add(_mm_set1_epi32(len), Mulc1(g));
  } else {
    // len >= 80
    // The following is loosely modelled after farmhashmk::Hash32.
    size_t iters = (len - 1) / 80;
    len -= iters * 80;

#undef Chunk
#define Chunk() do {                            \
  __m128i a = Fetch128(s);                      \
  __m128i b = Fetch128(s + 16);                 \
  __m128i c = Fetch128(s + 32);                 \
  __m128i d = Fetch128(s + 48);                 \
  __m128i e = Fetch128(s + 64);                 \
  h = Add(h, a);                                \
  g = Add(g, b);                                \
  g = Shuffle0321(g);                           \
  f = Add(f, c);                                \
  __m128i be = Add(b, Mulc1(e));                \
  h = Add(h, f);                                \
  f = Add(f, h);                                \
  h = Add(h, d);                                \
  q = Add(q, e);                                \
  h = Rol17(h);                                 \
  h = Mulc1(h);                                 \
  k = Xor(k, _mm_shuffle_epi8(g, f));           \
  g = Add(Xor(c, g), a);                        \
  f = Add(Xor(be, f), d);                       \
  std::swap(f, q);                              \
  q = _mm_aesimc_si128(q);                      \
  k = Add(k, be);                               \
  k = Add(k, _mm_shuffle_epi8(f, h));           \
  f = Add(f, g);                                \
  g = Add(g, f);                                \
  f = Mulc1(f);                                 \
} while (0)

    q = g;
    while (iters-- != 0) {
      Chunk();
      s += 80;
    }

    if (len != 0) {
      h = Add(h, _mm_set1_epi32(len));
      s = s + len - 80;
      Chunk();
    }
  }

  g = Shuffle0321(g);
  k = Xor(k, g);
  k = Xor(k, q);
  h = Xor(h, q);
  f = Mulc1(f);
  k = Mulc2(k);
  g = Mulc1(g);
  h = Mulc2(h);
  k = Add(k, _mm_shuffle_epi8(g, f));
  h = Add(h, f);
  f = Add(f, h);
  g = Add(g, k);
  k = Add(k, g);
  k = Xor(k, _mm_shuffle_epi8(f, h));
  __m128i buf[4];
  buf[0] = f;
  buf[1] = g;
  buf[2] = k;
  buf[3] = h;
  s = reinterpret_cast<char*>(buf);
  uint32_t x = Fetch(s);
  uint32_t y = Fetch(s+4);
  uint32_t z = Fetch(s+8);
  x = _mm_crc32_u32(x, Fetch(s+12));
  y = _mm_crc32_u32(y, Fetch(s+16));
  z = _mm_crc32_u32(z * c1, Fetch(s+20));
  x = _mm_crc32_u32(x, Fetch(s+24));
  y = _mm_crc32_u32(y * c1, Fetch(s+28));
  uint32_t o = y;
  z = _mm_crc32_u32(z, Fetch(s+32));
  x = _mm_crc32_u32(x * c1, Fetch(s+36));
  y = _mm_crc32_u32(y, Fetch(s+40));
  z = _mm_crc32_u32(z * c1, Fetch(s+44));
  x = _mm_crc32_u32(x, Fetch(s+48));
  y = _mm_crc32_u32(y * c1, Fetch(s+52));
  z = _mm_crc32_u32(z, Fetch(s+56));
  x = _mm_crc32_u32(x, Fetch(s+60));
  return (o - x + y - z) * c1;
}

#undef Chunk
#undef Murk
#undef Mulc2
#undef Mulc1

uint32_t Hash32WithSeed(const char *s, size_t len, uint32_t seed) {
  if (len <= 24) {
    if (len >= 13) return farmhashmk::Hash32Len13to24(s, len, seed * c1);
    else if (len >= 5) return farmhashmk::Hash32Len5to12(s, len, seed);
    else return farmhashmk::Hash32Len0to4(s, len, seed);
  }
  uint32_t h = farmhashmk::Hash32Len13to24(s, 24, seed ^ len);
  return _mm_crc32_u32(Hash32(s + 24, len - 24) + seed, h);
}

#endif
}  // namespace farmhashsu
namespace farmhashsa {
#if !can_use_sse42

uint32_t Hash32(const char *s, size_t len) {
  FARMHASH_DIE_IF_MISCONFIGURED;
  return s == NULL ? 0 : len;
}

uint32_t Hash32WithSeed(const char *s, size_t len, uint32_t seed) {
  FARMHASH_DIE_IF_MISCONFIGURED;
  return seed + Hash32(s, len);
}

#else

#undef Fetch
#define Fetch Fetch32

#undef Rotate
#define Rotate Rotate32

#undef Bswap
#define Bswap Bswap32

// Helpers for data-parallel operations (4x 32-bits).
STATIC_INLINE __m128i Add(__m128i x, __m128i y) { return _mm_add_epi32(x, y); }
STATIC_INLINE __m128i Xor(__m128i x, __m128i y) { return _mm_xor_si128(x, y); }
STATIC_INLINE __m128i Or(__m128i x, __m128i y) { return _mm_or_si128(x, y); }
STATIC_INLINE __m128i Mul(__m128i x, __m128i y) { return _mm_mullo_epi32(x, y); }
STATIC_INLINE __m128i Mul5(__m128i x) { return Add(x, _mm_slli_epi32(x, 2)); }
STATIC_INLINE __m128i Rotate(__m128i x, int c) {
  return Or(_mm_slli_epi32(x, c),
            _mm_srli_epi32(x, 32 - c));
}
STATIC_INLINE __m128i Rot17(__m128i x) { return Rotate(x, 17); }
STATIC_INLINE __m128i Rot19(__m128i x) { return Rotate(x, 19); }
STATIC_INLINE __m128i Shuffle0321(__m128i x) {
  return _mm_shuffle_epi32(x, (0 << 6) + (3 << 4) + (2 << 2) + (1 << 0));
}

uint32_t Hash32(const char *s, size_t len) {
  const uint32_t seed = 81;
  if (len <= 24) {
    return len <= 12 ?
        (len <= 4 ?
         farmhashmk::Hash32Len0to4(s, len) :
         farmhashmk::Hash32Len5to12(s, len)) :
        farmhashmk::Hash32Len13to24(s, len);
  }

  if (len < 40) {
    uint32_t a = len, b = seed * c2, c = a + b;
    a += Fetch(s + len - 4);
    b += Fetch(s + len - 20);
    c += Fetch(s + len - 16);
    uint32_t d = a;
    a = folly::external::farmhash::Rotate32(a, 21);
    a = Mur(a, Mur(b, Mur(c, d)));
    a += Fetch(s + len - 12);
    b += Fetch(s + len - 8);
    d += a;
    a += d;
    b = Mur(b, d) * c2;
    a = _mm_crc32_u32(a, b + c);
    return farmhashmk::Hash32Len13to24(s, (len + 1) / 2, a) + b;
  }

#undef Mulc1
#define Mulc1(x) Mul((x), cc1)

#undef Mulc2
#define Mulc2(x) Mul((x), cc2)

#undef Murk
#define Murk(a, h)                              \
  Add(k,                                        \
      Mul5(                                     \
          Rot19(                                \
              Xor(                              \
                  Mulc2(                        \
                      Rot17(                    \
                          Mulc1(a))),           \
                  (h)))))

  const __m128i cc1 = _mm_set1_epi32(c1);
  const __m128i cc2 = _mm_set1_epi32(c2);
  __m128i h = _mm_set1_epi32(seed);
  __m128i g = _mm_set1_epi32(c1 * seed);
  __m128i f = g;
  __m128i k = _mm_set1_epi32(0xe6546b64);
  if (len < 80) {
    __m128i a = Fetch128(s);
    __m128i b = Fetch128(s + 16);
    __m128i c = Fetch128(s + (len - 15) / 2);
    __m128i d = Fetch128(s + len - 32);
    __m128i e = Fetch128(s + len - 16);
    h = Add(h, a);
    g = Add(g, b);
    g = Shuffle0321(g);
    f = Add(f, c);
    __m128i be = Add(b, Mulc1(e));
    h = Add(h, f);
    f = Add(f, h);
    h = Add(Murk(d, h), e);
    k = Xor(k, _mm_shuffle_epi8(g, f));
    g = Add(Xor(c, g), a);
    f = Add(Xor(be, f), d);
    k = Add(k, be);
    k = Add(k, _mm_shuffle_epi8(f, h));
    f = Add(f, g);
    g = Add(g, f);
    g = Add(_mm_set1_epi32(len), Mulc1(g));
  } else {
    // len >= 80
    // The following is loosely modelled after farmhashmk::Hash32.
    size_t iters = (len - 1) / 80;
    len -= iters * 80;

#undef Chunk
#define Chunk() do {                            \
  __m128i a = Fetch128(s);                       \
  __m128i b = Fetch128(s + 16);                  \
  __m128i c = Fetch128(s + 32);                  \
  __m128i d = Fetch128(s + 48);                  \
  __m128i e = Fetch128(s + 64);                  \
  h = Add(h, a);                                \
  g = Add(g, b);                                \
  g = Shuffle0321(g);                           \
  f = Add(f, c);                                \
  __m128i be = Add(b, Mulc1(e));                \
  h = Add(h, f);                                \
  f = Add(f, h);                                \
  h = Add(Murk(d, h), e);                       \
  k = Xor(k, _mm_shuffle_epi8(g, f));           \
  g = Add(Xor(c, g), a);                        \
  f = Add(Xor(be, f), d);                       \
  k = Add(k, be);                               \
  k = Add(k, _mm_shuffle_epi8(f, h));           \
  f = Add(f, g);                                \
  g = Add(g, f);                                \
  f = Mulc1(f);                                 \
} while (0)

    while (iters-- != 0) {
      Chunk();
      s += 80;
    }

    if (len != 0) {
      h = Add(h, _mm_set1_epi32(len));
      s = s + len - 80;
      Chunk();
    }
  }

  g = Shuffle0321(g);
  k = Xor(k, g);
  f = Mulc1(f);
  k = Mulc2(k);
  g = Mulc1(g);
  h = Mulc2(h);
  k = Add(k, _mm_shuffle_epi8(g, f));
  h = Add(h, f);
  f = Add(f, h);
  g = Add(g, k);
  k = Add(k, g);
  k = Xor(k, _mm_shuffle_epi8(f, h));
  __m128i buf[4];
  buf[0] = f;
  buf[1] = g;
  buf[2] = k;
  buf[3] = h;
  s = reinterpret_cast<char*>(buf);
  uint32_t x = Fetch(s);
  uint32_t y = Fetch(s+4);
  uint32_t z = Fetch(s+8);
  x = _mm_crc32_u32(x, Fetch(s+12));
  y = _mm_crc32_u32(y, Fetch(s+16));
  z = _mm_crc32_u32(z * c1, Fetch(s+20));
  x = _mm_crc32_u32(x, Fetch(s+24));
  y = _mm_crc32_u32(y * c1, Fetch(s+28));
  uint32_t o = y;
  z = _mm_crc32_u32(z, Fetch(s+32));
  x = _mm_crc32_u32(x * c1, Fetch(s+36));
  y = _mm_crc32_u32(y, Fetch(s+40));
  z = _mm_crc32_u32(z * c1, Fetch(s+44));
  x = _mm_crc32_u32(x, Fetch(s+48));
  y = _mm_crc32_u32(y * c1, Fetch(s+52));
  z = _mm_crc32_u32(z, Fetch(s+56));
  x = _mm_crc32_u32(x, Fetch(s+60));
  return (o - x + y - z) * c1;
}

#undef Chunk
#undef Murk
#undef Mulc2
#undef Mulc1

uint32_t Hash32WithSeed(const char *s, size_t len, uint32_t seed) {
  if (len <= 24) {
    if (len >= 13) return farmhashmk::Hash32Len13to24(s, len, seed * c1);
    else if (len >= 5) return farmhashmk::Hash32Len5to12(s, len, seed);
    else return farmhashmk::Hash32Len0to4(s, len, seed);
  }
  uint32_t h = farmhashmk::Hash32Len13to24(s, 24, seed ^ len);
  return _mm_crc32_u32(Hash32(s + 24, len - 24) + seed, h);
}

#endif
}  // namespace farmhashsa
namespace farmhashcc {
// This file provides a 32-bit hash equivalent to CityHash32 (v1.1.1)
// and a 128-bit hash equivalent to CityHash128 (v1.1.1).  It also provides
// a seeded 32-bit hash function similar to CityHash32.

#undef Fetch
#define Fetch Fetch32

#undef Rotate
#define Rotate Rotate32

#undef Bswap
#define Bswap Bswap32

STATIC_INLINE uint32_t Hash32Len13to24(const char *s, size_t len) {
  uint32_t a = Fetch(s - 4 + (len >> 1));
  uint32_t b = Fetch(s + 4);
  uint32_t c = Fetch(s + len - 8);
  uint32_t d = Fetch(s + (len >> 1));
  uint32_t e = Fetch(s);
  uint32_t f = Fetch(s + len - 4);
  uint32_t h = len;

  return fmix(Mur(f, Mur(e, Mur(d, Mur(c, Mur(b, Mur(a, h)))))));
}

STATIC_INLINE uint32_t Hash32Len0to4(const char *s, size_t len) {
  uint32_t b = 0;
  uint32_t c = 9;
  for (size_t i = 0; i < len; i++) {
    signed char v = s[i];
    b = b * c1 + v;
    c ^= b;
  }
  return fmix(Mur(b, Mur(len, c)));
}

STATIC_INLINE uint32_t Hash32Len5to12(const char *s, size_t len) {
  uint32_t a = len, b = len * 5, c = 9, d = b;
  a += Fetch(s);
  b += Fetch(s + len - 4);
  c += Fetch(s + ((len >> 1) & 4));
  return fmix(Mur(c, Mur(b, Mur(a, d))));
}

uint32_t Hash32(const char *s, size_t len) {
  if (len <= 24) {
    return len <= 12 ?
        (len <= 4 ? Hash32Len0to4(s, len) : Hash32Len5to12(s, len)) :
        Hash32Len13to24(s, len);
  }

  // len > 24
  uint32_t h = len, g = c1 * len, f = g;
  {
    uint32_t a0 = Rotate(Fetch(s + len - 4) * c1, 17) * c2;
    uint32_t a1 = Rotate(Fetch(s + len - 8) * c1, 17) * c2;
    uint32_t a2 = Rotate(Fetch(s + len - 16) * c1, 17) * c2;
    uint32_t a3 = Rotate(Fetch(s + len - 12) * c1, 17) * c2;
    uint32_t a4 = Rotate(Fetch(s + len - 20) * c1, 17) * c2;
    h ^= a0;
    h = Rotate(h, 19);
    h = h * 5 + 0xe6546b64;
    h ^= a2;
    h = Rotate(h, 19);
    h = h * 5 + 0xe6546b64;
    g ^= a1;
    g = Rotate(g, 19);
    g = g * 5 + 0xe6546b64;
    g ^= a3;
    g = Rotate(g, 19);
    g = g * 5 + 0xe6546b64;
    f += a4;
    f = Rotate(f, 19);
    f = f * 5 + 0xe6546b64;
  }
  size_t iters = (len - 1) / 20;
  do {
    uint32_t a0 = Rotate(Fetch(s) * c1, 17) * c2;
    uint32_t a1 = Fetch(s + 4);
    uint32_t a2 = Rotate(Fetch(s + 8) * c1, 17) * c2;
    uint32_t a3 = Rotate(Fetch(s + 12) * c1, 17) * c2;
    uint32_t a4 = Fetch(s + 16);
    h ^= a0;
    h = Rotate(h, 18);
    h = h * 5 + 0xe6546b64;
    f += a1;
    f = Rotate(f, 19);
    f = f * c1;
    g += a2;
    g = Rotate(g, 18);
    g = g * 5 + 0xe6546b64;
    h ^= a3 + a1;
    h = Rotate(h, 19);
    h = h * 5 + 0xe6546b64;
    g ^= a4;
    g = Bswap(g) * 5;
    h += a4 * 5;
    h = Bswap(h);
    f += a0;
    PERMUTE3(f, h, g);
    s += 20;
  } while (--iters != 0);
  g = Rotate(g, 11) * c1;
  g = Rotate(g, 17) * c1;
  f = Rotate(f, 11) * c1;
  f = Rotate(f, 17) * c1;
  h = Rotate(h + g, 19);
  h = h * 5 + 0xe6546b64;
  h = Rotate(h, 17) * c1;
  h = Rotate(h + f, 19);
  h = h * 5 + 0xe6546b64;
  h = Rotate(h, 17) * c1;
  return h;
}

uint32_t Hash32WithSeed(const char *s, size_t len, uint32_t seed) {
  if (len <= 24) {
    if (len >= 13) return farmhashmk::Hash32Len13to24(s, len, seed * c1);
    else if (len >= 5) return farmhashmk::Hash32Len5to12(s, len, seed);
    else return farmhashmk::Hash32Len0to4(s, len, seed);
  }
  uint32_t h = farmhashmk::Hash32Len13to24(s, 24, seed ^ len);
  return Mur(Hash32(s + 24, len - 24) + seed, h);
}

#undef Fetch
#define Fetch Fetch64

#undef Rotate
#define Rotate Rotate64

#undef Bswap
#define Bswap Bswap64

STATIC_INLINE uint64_t ShiftMix(uint64_t val) {
  return val ^ (val >> 47);
}

STATIC_INLINE uint64_t HashLen16(uint64_t u, uint64_t v) {
  return Hash128to64(Uint128(u, v));
}

STATIC_INLINE uint64_t HashLen16(uint64_t u, uint64_t v, uint64_t mul) {
  // Murmur-inspired hashing.
  uint64_t a = (u ^ v) * mul;
  a ^= (a >> 47);
  uint64_t b = (v ^ a) * mul;
  b ^= (b >> 47);
  b *= mul;
  return b;
}

STATIC_INLINE uint64_t HashLen0to16(const char *s, size_t len) {
  if (len >= 8) {
    uint64_t mul = k2 + len * 2;
    uint64_t a = Fetch(s) + k2;
    uint64_t b = Fetch(s + len - 8);
    uint64_t c = Rotate(b, 37) * mul + a;
    uint64_t d = (Rotate(a, 25) + b) * mul;
    return HashLen16(c, d, mul);
  }
  if (len >= 4) {
    uint64_t mul = k2 + len * 2;
    uint64_t a = Fetch32(s);
    return HashLen16(len + (a << 3), Fetch32(s + len - 4), mul);
  }
  if (len > 0) {
    uint8_t a = s[0];
    uint8_t b = s[len >> 1];
    uint8_t c = s[len - 1];
    uint32_t y = static_cast<uint32_t>(a) + (static_cast<uint32_t>(b) << 8);
    uint32_t z = len + (static_cast<uint32_t>(c) << 2);
    return ShiftMix(y * k2 ^ z * k0) * k2;
  }
  return k2;
}

// Return a 16-byte hash for 48 bytes.  Quick and dirty.
// Callers do best to use "random-looking" values for a and b.
STATIC_INLINE pair<uint64_t, uint64_t> WeakHashLen32WithSeeds(
    uint64_t w, uint64_t x, uint64_t y, uint64_t z, uint64_t a, uint64_t b) {
  a += w;
  b = Rotate(b + a + z, 21);
  uint64_t c = a;
  a += x;
  a += y;
  b += Rotate(a, 44);
  return make_pair(a + z, b + c);
}

// Return a 16-byte hash for s[0] ... s[31], a, and b.  Quick and dirty.
STATIC_INLINE pair<uint64_t, uint64_t> WeakHashLen32WithSeeds(
    const char* s, uint64_t a, uint64_t b) {
  return WeakHashLen32WithSeeds(Fetch(s),
                                Fetch(s + 8),
                                Fetch(s + 16),
                                Fetch(s + 24),
                                a,
                                b);
}



// A subroutine for CityHash128().  Returns a decent 128-bit hash for strings
// of any length representable in signed long.  Based on City and Murmur.
STATIC_INLINE uint128_t CityMurmur(const char *s, size_t len, uint128_t seed) {
  uint64_t a = Uint128Low64(seed);
  uint64_t b = Uint128High64(seed);
  uint64_t c = 0;
  uint64_t d = 0;
  signed long l = len - 16;
  if (l <= 0) {  // len <= 16
    a = ShiftMix(a * k1) * k1;
    c = b * k1 + HashLen0to16(s, len);
    d = ShiftMix(a + (len >= 8 ? Fetch(s) : c));
  } else {  // len > 16
    c = HashLen16(Fetch(s + len - 8) + k1, a);
    d = HashLen16(b + len, c + Fetch(s + len - 16));
    a += d;
    do {
      a ^= ShiftMix(Fetch(s) * k1) * k1;
      a *= k1;
      b ^= a;
      c ^= ShiftMix(Fetch(s + 8) * k1) * k1;
      c *= k1;
      d ^= c;
      s += 16;
      l -= 16;
    } while (l > 0);
  }
  a = HashLen16(a, c);
  b = HashLen16(d, b);
  return Uint128(a ^ b, HashLen16(b, a));
}

uint128_t CityHash128WithSeed(const char *s, size_t len, uint128_t seed) {
  if (len < 128) {
    return CityMurmur(s, len, seed);
  }

  // We expect len >= 128 to be the common case.  Keep 56 bytes of state:
  // v, w, x, y, and z.
  pair<uint64_t, uint64_t> v, w;
  uint64_t x = Uint128Low64(seed);
  uint64_t y = Uint128High64(seed);
  uint64_t z = len * k1;
  v.first = Rotate(y ^ k1, 49) * k1 + Fetch(s);
  v.second = Rotate(v.first, 42) * k1 + Fetch(s + 8);
  w.first = Rotate(y + z, 35) * k1 + x;
  w.second = Rotate(x + Fetch(s + 88), 53) * k1;

  // This is the same inner loop as CityHash64(), manually unrolled.
  do {
    x = Rotate(x + y + v.first + Fetch(s + 8), 37) * k1;
    y = Rotate(y + v.second + Fetch(s + 48), 42) * k1;
    x ^= w.second;
    y += v.first + Fetch(s + 40);
    z = Rotate(z + w.first, 33) * k1;
    v = WeakHashLen32WithSeeds(s, v.second * k1, x + w.first);
    w = WeakHashLen32WithSeeds(s + 32, z + w.second, y + Fetch(s + 16));
    std::swap(z, x);
    s += 64;
    x = Rotate(x + y + v.first + Fetch(s + 8), 37) * k1;
    y = Rotate(y + v.second + Fetch(s + 48), 42) * k1;
    x ^= w.second;
    y += v.first + Fetch(s + 40);
    z = Rotate(z + w.first, 33) * k1;
    v = WeakHashLen32WithSeeds(s, v.second * k1, x + w.first);
    w = WeakHashLen32WithSeeds(s + 32, z + w.second, y + Fetch(s + 16));
    std::swap(z, x);
    s += 64;
    len -= 128;
  } while (LIKELY(len >= 128));
  x += Rotate(v.first + z, 49) * k0;
  y = y * k0 + Rotate(w.second, 37);
  z = z * k0 + Rotate(w.first, 27);
  w.first *= 9;
  v.first *= k0;
  // If 0 < len < 128, hash up to 4 chunks of 32 bytes each from the end of s.
  for (size_t tail_done = 0; tail_done < len; ) {
    tail_done += 32;
    y = Rotate(x + y, 42) * k0 + v.second;
    w.first += Fetch(s + len - tail_done + 16);
    x = x * k0 + w.first;
    z += w.second + Fetch(s + len - tail_done);
    w.second += v.first;
    v = WeakHashLen32WithSeeds(s + len - tail_done, v.first + z, v.second);
    v.first *= k0;
  }
  // At this point our 56 bytes of state should contain more than
  // enough information for a strong 128-bit hash.  We use two
  // different 56-byte-to-8-byte hashes to get a 16-byte final result.
  x = HashLen16(x, v.first);
  y = HashLen16(y + z, w.first);
  return Uint128(HashLen16(x + v.second, w.second) + y,
                 HashLen16(x + w.second, y + v.second));
}

STATIC_INLINE uint128_t CityHash128(const char *s, size_t len) {
  return len >= 16 ?
      CityHash128WithSeed(s + 16, len - 16,
                          Uint128(Fetch(s), Fetch(s + 8) + k0)) :
      CityHash128WithSeed(s, len, Uint128(k0, k1));
}

uint128_t Fingerprint128(const char* s, size_t len) {
  return CityHash128(s, len);
}
}  // namespace farmhashcc

// BASIC STRING HASHING

// Hash function for a byte array.  See also Hash(), below.
// May change from time to time, may differ on different platforms, may differ
// depending on NDEBUG.
uint32_t Hash32(const char* s, size_t len) {
  return DebugTweak(
      (can_use_sse41 & x86_64) ? farmhashnt::Hash32(s, len) :
      (can_use_sse42 & can_use_aesni) ? farmhashsu::Hash32(s, len) :
      can_use_sse42 ? farmhashsa::Hash32(s, len) :
      farmhashmk::Hash32(s, len));
}

// Hash function for a byte array.  For convenience, a 32-bit seed is also
// hashed into the result.
// May change from time to time, may differ on different platforms, may differ
// depending on NDEBUG.
uint32_t Hash32WithSeed(const char* s, size_t len, uint32_t seed) {
  return DebugTweak(
      (can_use_sse41 & x86_64) ? farmhashnt::Hash32WithSeed(s, len, seed) :
      (can_use_sse42 & can_use_aesni) ? farmhashsu::Hash32WithSeed(s, len, seed) :
      can_use_sse42 ? farmhashsa::Hash32WithSeed(s, len, seed) :
      farmhashmk::Hash32WithSeed(s, len, seed));
}

// Hash function for a byte array.  For convenience, a 64-bit seed is also
// hashed into the result.  See also Hash(), below.
// May change from time to time, may differ on different platforms, may differ
// depending on NDEBUG.
uint64_t Hash64(const char* s, size_t len) {
  return DebugTweak(
      (can_use_sse42 & x86_64) ?
      farmhashte::Hash64(s, len) :
      farmhashxo::Hash64(s, len));
}

// Hash function for a byte array.
// May change from time to time, may differ on different platforms, may differ
// depending on NDEBUG.
size_t Hash(const char* s, size_t len) {
  return sizeof(size_t) == 8 ? Hash64(s, len) : Hash32(s, len);
}

// Hash function for a byte array.  For convenience, a 64-bit seed is also
// hashed into the result.
// May change from time to time, may differ on different platforms, may differ
// depending on NDEBUG.
uint64_t Hash64WithSeed(const char* s, size_t len, uint64_t seed) {
  return DebugTweak(farmhashna::Hash64WithSeed(s, len, seed));
}

// Hash function for a byte array.  For convenience, two seeds are also
// hashed into the result.
// May change from time to time, may differ on different platforms, may differ
// depending on NDEBUG.
uint64_t Hash64WithSeeds(const char* s, size_t len, uint64_t seed0, uint64_t seed1) {
  return DebugTweak(farmhashna::Hash64WithSeeds(s, len, seed0, seed1));
}

// Hash function for a byte array.
// May change from time to time, may differ on different platforms, may differ
// depending on NDEBUG.
uint128_t Hash128(const char* s, size_t len) {
  return DebugTweak(farmhashcc::Fingerprint128(s, len));
}

// Hash function for a byte array.  For convenience, a 128-bit seed is also
// hashed into the result.
// May change from time to time, may differ on different platforms, may differ
// depending on NDEBUG.
uint128_t Hash128WithSeed(const char* s, size_t len, uint128_t seed) {
  return DebugTweak(farmhashcc::CityHash128WithSeed(s, len, seed));
}

// BASIC NON-STRING HASHING

// FINGERPRINTING (i.e., good, portable, forever-fixed hash functions)

// Fingerprint function for a byte array.  Most useful in 32-bit binaries.
uint32_t Fingerprint32(const char* s, size_t len) {
  return farmhashmk::Hash32(s, len);
}

// Fingerprint function for a byte array.
uint64_t Fingerprint64(const char* s, size_t len) {
  return farmhashna::Hash64(s, len);
}

// Fingerprint function for a byte array.
uint128_t Fingerprint128(const char* s, size_t len) {
  return farmhashcc::Fingerprint128(s, len);
}

// Older and still available but perhaps not as fast as the above:
//   farmhashns::Hash32{,WithSeed}()

} // namespace farmhash
} // namespace external
} // namespace folly

FOLLY_UNSAFE_BUFFER_USAGE_END
