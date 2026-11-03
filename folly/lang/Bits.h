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

/**
 * Various low-level, bit-manipulation routines.
 *
 * findFirstSet(x)  [constexpr]
 *    find first (least significant) bit set in a value of an integral type,
 *    1-based (like ffs()).  0 = no bits are set (x == 0)
 *
 * findLastSet(x)  [constexpr]
 *    find last (most significant) bit set in a value of an integral type,
 *    1-based.  0 = no bits are set (x == 0)
 *    for x != 0, findLastSet(x) == 1 + floor(log2(x))
 *
 * extractFirstSet(x)  [constexpr]
 *    extract first (least significant) bit set in a value of an integral
 *    type, 0 = no bits are set (x == 0)
 *
 * nextPowTwo(x)  [constexpr]
 *    Finds the next power of two >= x.
 *
 * strictNextPowTwo(x)  [constexpr]
 *    Finds the next power of two > x.
 *
 * isPowTwo(x)  [constexpr]
 *    return true iff x is a power of two
 *
 * popcount(x)
 *    return the number of 1 bits in x
 *
 * Endian
 *    convert between native, big, and little endian representation
 *    Endian::big(x)      big <-> native
 *    Endian::little(x)   little <-> native
 *    Endian::swap(x)     big <-> little
 *
 */

#pragma once

#include <cassert>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include <folly/ConstexprMath.h>
#include <folly/Portability.h>
#include <folly/Traits.h>
#include <folly/Utility.h>
#include <folly/lang/Assume.h>
#include <folly/lang/CString.h>
#include <folly/portability/Builtins.h>

#ifdef __BMI2__
#include <immintrin.h>
#endif

#include <bit>

namespace folly {

namespace detail {
template <typename Dst, typename Src>
constexpr std::make_signed_t<Dst> bits_to_signed(Src const s) {
  static_assert(std::is_signed<Dst>::value, "unsigned type");
  return to_signed(static_cast<std::make_unsigned_t<Dst>>(to_unsigned(s)));
}
template <typename Dst, typename Src>
constexpr std::make_unsigned_t<Dst> bits_to_unsigned(Src const s) {
  static_assert(std::is_unsigned<Dst>::value, "signed type");
  return static_cast<Dst>(to_unsigned(s));
}

template <typename T>
inline constexpr bool supported_in_bits_operations_v =
    std::is_unsigned_v<T> && sizeof(T) <= 8;

} // namespace detail

/// findFirstSet
///
/// Return the 1-based index of the least significant bit which is set.
/// For x > 0, the exponent in the largest power of two which does not divide x.
template <typename T>
inline constexpr unsigned int findFirstSet(T const v) {
  using S0 = int;
  using S1 = long int;
  using S2 = long long int;
  using detail::bits_to_signed;
  static_assert(sizeof(T) <= sizeof(S2), "over-sized type");
  static_assert(std::is_integral<T>::value, "non-integral type");
  static_assert(!std::is_same<T, bool>::value, "bool type");

  // clang-format off
  return static_cast<unsigned int>(
      sizeof(T) <= sizeof(S0) ? __builtin_ffs(bits_to_signed<S0>(v)) :
      sizeof(T) <= sizeof(S1) ? __builtin_ffsl(bits_to_signed<S1>(v)) :
      sizeof(T) <= sizeof(S2) ? __builtin_ffsll(bits_to_signed<S2>(v)) :
      0);
  // clang-format on
}

/// findLastSet
///
/// Return the 1-based index of the most significant bit which is set.
/// For x > 0, findLastSet(x) == 1 + floor(log2(x)).
template <typename T>
FOLLY_ALWAYS_INLINE constexpr unsigned int findLastSet(T const v) {
  using U0 = unsigned int;
  using U1 = unsigned long int;
  using U2 = unsigned long long int;
  using detail::bits_to_unsigned;
  static_assert(sizeof(T) <= sizeof(U2), "over-sized type");
  static_assert(std::is_integral<T>::value, "non-integral type");
  static_assert(!std::is_same<T, bool>::value, "bool type");

  // If X is a power of two X - Y = 1 + ((X - 1) ^ Y). Doing this transformation
  // allows GCC to remove its own xor that it adds to implement clz using bsr.
  // clang-format off
  constexpr auto size = constexpr_max(sizeof(T), sizeof(U0));
  return v ? 1u + static_cast<unsigned int>((8u * size - 1u) ^ (
      sizeof(T) <= sizeof(U0) ? __builtin_clz(bits_to_unsigned<U0>(v)) :
      sizeof(T) <= sizeof(U1) ? __builtin_clzl(bits_to_unsigned<U1>(v)) :
      sizeof(T) <= sizeof(U2) ? __builtin_clzll(bits_to_unsigned<U2>(v)) :
      0)) : 0u;
  // clang-format on
}

/// extractFirstSet
///
/// Return a value where all the bits but the least significant are cleared.
template <typename T>
inline constexpr T extractFirstSet(T const v) {
  static_assert(std::is_integral<T>::value, "non-integral type");
  static_assert(std::is_unsigned<T>::value, "signed type");
  static_assert(!std::is_same<T, bool>::value, "bool type");

  return v & -v;
}

/// popcount
///
/// Returns the number of bits which are set.
template <typename T>
inline constexpr unsigned int popcount(T const v) {
  using U0 = unsigned int;
  using U1 = unsigned long int;
  using U2 = unsigned long long int;
  using detail::bits_to_unsigned;
  static_assert(sizeof(T) <= sizeof(U2), "over-sized type");
  static_assert(std::is_integral<T>::value, "non-integral type");
  static_assert(!std::is_same<T, bool>::value, "bool type");

  // clang-format off
  return static_cast<unsigned int>(
      sizeof(T) <= sizeof(U0) ? __builtin_popcount(bits_to_unsigned<U0>(v)) :
      sizeof(T) <= sizeof(U1) ? __builtin_popcountl(bits_to_unsigned<U1>(v)) :
      sizeof(T) <= sizeof(U2) ? __builtin_popcountll(bits_to_unsigned<U2>(v)) :
      0);
  // clang-format on
}

template <class T>
FOLLY_ALWAYS_INLINE constexpr T nextPowTwo(T const v) {
  static_assert(std::is_unsigned<T>::value, "signed type");
  return v ? (T(1) << findLastSet(v - 1)) : T(1);
}

template <class T>
inline constexpr bool isPowTwo(T const v) {
  static_assert(std::is_integral<T>::value, "non-integral type");
  static_assert(std::is_unsigned<T>::value, "signed type");
  static_assert(!std::is_same<T, bool>::value, "bool type");
  return (v != 0) && !(v & (v - 1));
}

template <class T>
inline constexpr T strictNextPowTwo(T const v) {
  static_assert(std::is_unsigned<T>::value, "signed type");
  return nextPowTwo(T(v + 1));
}

template <class T>
inline constexpr T strictPrevPowTwo(T const v) {
  static_assert(std::is_unsigned<T>::value, "signed type");
  return v > 1 ? std::bit_floor(T(v - 1)) : T(0);
}

/// n_least_significant_bits
/// n_least_significant_bits_fn
///
/// Returns an unsigned integer of type T, where n
/// least significant (right) bits are set and others are not.
template <class T>
struct n_least_significant_bits_fn {
  static_assert(detail::supported_in_bits_operations_v<T>);

  [[nodiscard]] constexpr T operator()(std::uint32_t n) const {
    if (!folly::is_constant_evaluated_or(true)) {
      compiler_may_unsafely_assume(n <= sizeof(T) * 8);

#ifdef __BMI2__
      if constexpr (sizeof(T) <= 4) {
        return static_cast<T>(_bzhi_u32(static_cast<std::uint32_t>(-1), n));
      }
      return static_cast<T>(_bzhi_u64(static_cast<std::uint64_t>(-1), n));
#endif
    }

    if (sizeof(T) == 8 && n == 64) {
      return static_cast<T>(-1);
    }
    return static_cast<T>((std::uint64_t{1} << n) - 1);
  }
};

template <class T>
inline constexpr n_least_significant_bits_fn<T> n_least_significant_bits;

/// n_most_significant_bits
/// n_most_significant_bits_fn
///
/// Returns an unsigned integer of type T, where n
/// most significant bits (left) are set and others are not.
template <class T>
struct n_most_significant_bits_fn {
  static_assert(detail::supported_in_bits_operations_v<T>);

  [[nodiscard]] constexpr T operator()(std::uint32_t n) const {
    if (!folly::is_constant_evaluated_or(true)) {
      compiler_may_unsafely_assume(n <= sizeof(T) * 8);

#ifdef __BMI2__
      // assembler looks smaller here, if we use bzhi from `set_lowest_n_bits`
      if constexpr (sizeof(T) == 8) {
        return static_cast<T>(~n_least_significant_bits<T>(64 - n));
      }
#endif
    }

    if (sizeof(T) == 8 && n == 0) {
      return 0;
    }
    n = sizeof(T) * 8 - n;

    std::uint64_t ones = static_cast<T>(~0);
    return static_cast<T>(ones << n);
  }
};

template <class T>
inline constexpr n_most_significant_bits_fn<T> n_most_significant_bits;

/// clear_n_least_significant_bits
/// clear_n_least_significant_bits_fn
///
/// Clears n least significant (right) bits. Other bits stay the same.
struct clear_n_least_significant_bits_fn {
  template <typename T>
  [[nodiscard]] constexpr T operator()(T x, std::uint32_t n) const {
    static_assert(detail::supported_in_bits_operations_v<T>);

    // alternative is to do two shifts but that has
    // a dependency between them, so is likely worse
    return x & n_most_significant_bits<T>(sizeof(T) * 8 - n);
  }
};

inline constexpr clear_n_least_significant_bits_fn
    clear_n_least_significant_bits;

/// set_n_least_significant_bits
/// set_n_least_significant_bits_fn
///
/// Sets n least significant (right) bits. Other bits stay the same.
struct set_n_least_significant_bits_fn {
  template <typename T>
  [[nodiscard]] constexpr T operator()(T x, std::uint32_t n) const {
    static_assert(detail::supported_in_bits_operations_v<T>);

    // alternative is to do two shifts but that has
    // a dependency between them, so is likely worse
    return x | n_least_significant_bits<T>(n);
  }
};

inline constexpr set_n_least_significant_bits_fn set_n_least_significant_bits;

/// clear_n_most_significant_bits
/// clear_n_most_significant_bits_fn
///
/// Clears n most significant (left) bits. Other bits stay the same.
struct clear_n_most_significant_bits_fn {
  template <typename T>
  [[nodiscard]] constexpr T operator()(T x, std::uint32_t n) const {
    static_assert(detail::supported_in_bits_operations_v<T>);

    if (!folly::is_constant_evaluated_or(true)) {
      compiler_may_unsafely_assume(n <= sizeof(T) * 8);

#ifdef __BMI2__
      if constexpr (sizeof(T) <= 4) {
        return static_cast<T>(_bzhi_u32(x, sizeof(T) * 8 - n));
      }
      return static_cast<T>(_bzhi_u64(x, sizeof(T) * 8 - n));
#endif
    }

    // alternative is to do two shifts but that has
    // a dependency between them, so is likely worse
    return x & n_least_significant_bits<T>(sizeof(T) * 8 - n);
  }
};

inline constexpr clear_n_most_significant_bits_fn clear_n_most_significant_bits;

/// set_n_most_significant_bits
/// set_n_most_significant_bits_fn
///
/// Sets n most significant (left) bits. Other bits stay the same.
struct set_n_most_significant_bits_fn {
  template <typename T>
  [[nodiscard]] constexpr T operator()(T x, std::uint32_t n) const {
    static_assert(detail::supported_in_bits_operations_v<T>);
    return x | n_most_significant_bits<T>(n);
  }
};

inline constexpr set_n_most_significant_bits_fn set_n_most_significant_bits;

/**
 * Endianness detection and manipulation primitives.
 */
namespace detail {

template <size_t Size>
struct uint_types_by_size;

#define FB_GEN(sz, fn)                                      \
  static inline uint##sz##_t byteswap_gen(uint##sz##_t v) { \
    return fn(v);                                           \
  }                                                         \
  template <>                                               \
  struct uint_types_by_size<sz / 8> {                       \
    using type = uint##sz##_t;                              \
  };

FB_GEN(8, uint8_t)
#ifdef _MSC_VER
FB_GEN(64, _byteswap_uint64)
FB_GEN(32, _byteswap_ulong)
FB_GEN(16, _byteswap_ushort)
#else
FB_GEN(64, __builtin_bswap64)
FB_GEN(32, __builtin_bswap32)
FB_GEN(16, __builtin_bswap16)
#endif

#undef FB_GEN

template <class T>
struct EndianInt {
  static_assert(
      (std::is_integral<T>::value && !std::is_same<T, bool>::value) ||
          std::is_floating_point<T>::value,
      "template type parameter must be non-bool integral or floating point");
  static T swap(T x) {
    // we implement this with bit_cast because that is defined behavior in C++
    // we rely on compilers to optimize away the bit_cast calls
    constexpr auto s = sizeof(T);
    using B = typename uint_types_by_size<s>::type;
    return std::bit_cast<T>(byteswap_gen(std::bit_cast<B>(x)));
  }
  static T big(T x) { return kIsLittleEndian ? EndianInt::swap(x) : x; }
  static T little(T x) { return kIsBigEndian ? EndianInt::swap(x) : x; }
};

} // namespace detail

// big* convert between native and big-endian representations
// little* convert between native and little-endian representations
// swap* convert between big-endian and little-endian representations
//
// ntohs, htons == big16
// ntohl, htonl == big32
#define FB_GEN1(fn, t, sz) \
  static t fn##sz(t x) {   \
    return fn<t>(x);       \
  }

#define FB_GEN2(t, sz) \
  FB_GEN1(swap, t, sz) \
  FB_GEN1(big, t, sz)  \
  FB_GEN1(little, t, sz)

#define FB_GEN(sz)          \
  FB_GEN2(uint##sz##_t, sz) \
  FB_GEN2(int##sz##_t, sz)

class Endian {
 public:
  enum class Order : uint8_t {
    LITTLE,
    BIG,
  };

  static constexpr Order order = kIsLittleEndian ? Order::LITTLE : Order::BIG;

  template <class T>
  static T swap(T x) {
    return folly::detail::EndianInt<T>::swap(x);
  }
  template <class T>
  static T big(T x) {
    return folly::detail::EndianInt<T>::big(x);
  }
  template <class T>
  static T little(T x) {
    return folly::detail::EndianInt<T>::little(x);
  }

#if !defined(__ANDROID__)
  FB_GEN(64)
  FB_GEN(32)
  FB_GEN(16)
  FB_GEN(8)
#endif
};

#undef FB_GEN
#undef FB_GEN2
#undef FB_GEN1

/// get_bit_at
/// get_bit_at_fn
///
/// From an array of unsigned integers get a bit at a position idx.
/// Lowest bits in an integer considered to come first.
///
struct get_bit_at_fn {
  template <typename Uint>
  [[nodiscard]] constexpr bool operator()(
      const Uint* ptr, std::size_t idx) const noexcept {
    static_assert(std::is_unsigned_v<std::remove_cv_t<Uint>>);
    static_assert(!std::is_same_v<std::remove_cv_t<Uint>, bool>);
    std::size_t uintIdx = idx / (sizeof(Uint) * 8);
    std::size_t bitIdx = idx % (sizeof(Uint) * 8);
    Uint loaded = ptr[uintIdx];

    Uint justOneBit = loaded & (Uint{1} << bitIdx);
    return !!justOneBit;
  }
};

inline constexpr get_bit_at_fn get_bit_at;

/**
 * Representation of an unaligned value of a POD type.
 */
FOLLY_PUSH_WARNING
FOLLY_CLANG_DISABLE_WARNING("-Wpacked")
FOLLY_PACK_PUSH
template <class T>
struct Unaligned {
 public:
  static_assert(std::is_standard_layout_v<T>);
  static_assert(std::is_trivial_v<T>);

  Unaligned() = default; // uninitialized
  /* implicit */ Unaligned(T v) noexcept : value_(v) {}

  /* implicit */ operator T() const noexcept { return value_; }

 private:
  T value_; // it must be an error to get a reference to a packed member
} FOLLY_PACK_ATTR;
FOLLY_PACK_POP
FOLLY_POP_WARNING

/**
 * Read an unaligned value of type T and return it.
 */
template <class T>
inline constexpr T loadUnaligned(const void* p) {
  static_assert(std::is_trivial_v<T>);
  T value{static_cast<T>(unsafe_default_initialized)};
  FOLLY_BUILTIN_MEMCPY(&value, p, sizeof(T));
  return value;
}

/**
 * Read l bytes into the low bits of a value of an unsigned integral
 * type T, where l < sizeof(T).
 *
 * This is intended as a complement to loadUnaligned to read the tail
 * of a buffer when it is processed one word at a time.
 */
template <class T>
inline T partialLoadUnaligned(const void* p, size_t l) {
  static_assert(
      std::is_integral<T>::value && std::is_unsigned<T>::value &&
          sizeof(T) <= 8,
      "Invalid type");
  assume(l < sizeof(T));

  auto cp = static_cast<const char*>(p);
  T value = 0;
  if constexpr (!kHasUnalignedAccess || !kIsLittleEndian) {
    // Unsupported, use memcpy.
    memcpy(&value, cp, l);
    return value;
  }

  auto avail = l;
  if (l & 4) {
    avail -= 4;
    value = static_cast<T>(loadUnaligned<uint32_t>(cp + avail)) << (avail * 8);
  }
  if (l & 2) {
    avail -= 2;
    value |= static_cast<T>(loadUnaligned<uint16_t>(cp + avail)) << (avail * 8);
  }
  if (l & 1) {
    value |= loadUnaligned<uint8_t>(cp);
  }
  return value;
}

namespace detail {

template <class T, class S>
constexpr T constexprLoadUnalignedImpl(const S* s) {
  T ret = T{0};
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    auto idx = kIsLittleEndian ? i : (sizeof(T) - 1 - i);
    ret |= T{static_cast<std::uint8_t>(s[i])} << (idx * 8);
  }
  return ret;
}

} // namespace detail

/**
 * Read an unaligned value of type T and return it.
 * Constexpr, but not optimized. Accepts inputs either of char-array types or
 * char-backed enum-array types.
 */
template <class T, class S>
constexpr T constexprLoadUnaligned(const S* s) {
  static_assert(std::is_integral_v<T>);
  static_assert(std::is_unsigned_v<T>);
  static_assert(!std::is_same_v<T, bool>);
  static_assert(std::is_integral_v<S> || std::is_enum_v<S>);
  static_assert(!std::is_same_v<S, bool>);
  static_assert(sizeof(S) == 1);

  return is_constant_evaluated_or(false)
      ? detail::constexprLoadUnalignedImpl<T>(s)
      : loadUnaligned<T>(s);
}

namespace detail {

template <class T, class S>
constexpr T constexprPartialLoadUnalignedImpl(const S* s, std::size_t l) {
  T ret = T{0};
  for (std::size_t i = 0; i < l; ++i) {
    auto idx = kIsLittleEndian ? i : (sizeof(T) - 1 - i);
    ret |= T{static_cast<std::uint8_t>(s[i])} << (idx * 8);
  }
  return ret;
}

} // namespace detail

/**
 * Read an unaligned value of type T and return it.
 * Constexpr, but not optimized. Accepts inputs either of char-array types or
 * char-backed enum-array types.
 */
template <class T, class S>
constexpr T constexprPartialLoadUnaligned(const S* s, std::size_t l) {
  static_assert(std::is_integral_v<T>);
  static_assert(std::is_unsigned_v<T>);
  static_assert(!std::is_same_v<T, bool>);
  static_assert(std::is_integral_v<S> || std::is_enum_v<S>);
  static_assert(!std::is_same_v<S, bool>);
  static_assert(sizeof(S) == 1);
  if (!(l < sizeof(T))) {
    assume_unreachable();
  }

  return is_constant_evaluated_or(false)
      ? detail::constexprPartialLoadUnalignedImpl<T>(s, l)
      : partialLoadUnaligned<T>(s, l);
}

/**
 * Write an unaligned value of type T.
 */
template <class T, class..., class V>
inline void storeUnaligned(void* p, V value) {
  static_assert(
      std::is_same_v<T, V>, "the type of value does not match the type param");
  static_assert(std::is_trivial_v<T>);
  FOLLY_BUILTIN_MEMCPY(p, &value, sizeof(T));
}

namespace detail {

template <typename T>
T bitReverseFallback(T n) {
  auto m = static_cast<typename std::make_unsigned<T>::type>(n);
  m = ((m & 0xAAAAAAAAAAAAAAAA) >> 1) | ((m & 0x5555555555555555) << 1);
  m = ((m & 0xCCCCCCCCCCCCCCCC) >> 2) | ((m & 0x3333333333333333) << 2);
  m = ((m & 0xF0F0F0F0F0F0F0F0) >> 4) | ((m & 0x0F0F0F0F0F0F0F0F) << 4);
  return static_cast<T>(Endian::swap(m));
}

} // namespace detail

template <typename T>
T bitReverse(T n) {
#if FOLLY_HAS_BUILTIN(__builtin_bitreverse64)
  if constexpr (sizeof(T) == 8) {
    return __builtin_bitreverse64(n);
  } else if constexpr (sizeof(T) == 4) {
    return __builtin_bitreverse32(n);
  } else if constexpr (sizeof(T) == 2) {
    return __builtin_bitreverse16(n);
  } else if constexpr (sizeof(T) == 1) {
    return __builtin_bitreverse8(n);
  }
#endif
  return detail::bitReverseFallback(n);
}

} // namespace folly
