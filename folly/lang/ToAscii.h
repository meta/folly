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

#include <cstring>

#include <folly/ConstexprMath.h>
#include <folly/Likely.h>
#include <folly/Portability.h>
#include <folly/Utility.h>
#include <folly/lang/Align.h>
#include <folly/lang/CArray.h>
#include <folly/portability/Builtins.h>

namespace folly {

//  to_ascii_alphabet
//
//  Used implicity by to_ascii_lower and to_ascii_upper below.
//
//  This alphabet translates digits to 0-9,a-z or 0-9,A-Z. The largest supported
//  base is 36; operator() presumes an argument less than that.
//
//  Alternative alphabets may be used with to_ascii_with provided they match
//  the constructibility/destructibility and the interface of this one.
template <bool Upper>
struct to_ascii_alphabet {
  //  operator()
  //
  //  Translates a single digit to 0-9,a-z or 0-9,A-Z.
  //
  //  async-signal-safe
  constexpr char operator()(uint8_t b) const {
    return b < 10 ? '0' + b : (Upper ? 'A' : 'a') + (b - 10);
  }
};
using to_ascii_alphabet_lower = to_ascii_alphabet<false>;
using to_ascii_alphabet_upper = to_ascii_alphabet<true>;

namespace detail {

template <uint64_t Base, typename Alphabet>
struct to_ascii_array {
  using data_type_ = c_array<uint8_t, Base>;
  static constexpr data_type_ data_() {
    data_type_ result{};
    Alphabet alpha;
    for (size_t i = 0; i < Base; ++i) {
      result.data[i] = alpha(static_cast<uint8_t>(i));
    }
    return result;
  }
  // @lint-ignore CLANGTIDY
  static data_type_ const data;
  constexpr char operator()(uint8_t index) const { // also an alphabet
    return data.data[index];
  }
};
template <uint64_t Base, typename Alphabet>
alignas(kIsMobile ? sizeof(size_t) : hardware_constructive_interference_size)
    typename to_ascii_array<Base, Alphabet>::data_type_ const
    to_ascii_array<Base, Alphabet>::data =
        to_ascii_array<Base, Alphabet>::data_();

extern template to_ascii_array<8, to_ascii_alphabet_lower>::data_type_ const
    to_ascii_array<8, to_ascii_alphabet_lower>::data;
extern template to_ascii_array<10, to_ascii_alphabet_lower>::data_type_ const
    to_ascii_array<10, to_ascii_alphabet_lower>::data;
extern template to_ascii_array<16, to_ascii_alphabet_lower>::data_type_ const
    to_ascii_array<16, to_ascii_alphabet_lower>::data;
extern template to_ascii_array<8, to_ascii_alphabet_upper>::data_type_ const
    to_ascii_array<8, to_ascii_alphabet_upper>::data;
extern template to_ascii_array<10, to_ascii_alphabet_upper>::data_type_ const
    to_ascii_array<10, to_ascii_alphabet_upper>::data;
extern template to_ascii_array<16, to_ascii_alphabet_upper>::data_type_ const
    to_ascii_array<16, to_ascii_alphabet_upper>::data;

template <uint64_t Base, typename Alphabet>
struct to_ascii_table {
  using data_type_ = c_array<uint16_t, Base * Base>;
  static constexpr data_type_ data_() {
    data_type_ result{};
    Alphabet alpha;
    for (size_t i = 0; i < Base * Base; ++i) {
      result.data[i] = //
          (alpha(uint8_t(i / Base)) << (kIsLittleEndian ? 0 : 8)) |
          (alpha(uint8_t(i % Base)) << (kIsLittleEndian ? 8 : 0));
    }
    return result;
  }
  // @lint-ignore CLANGTIDY
  static data_type_ const data;
};
template <uint64_t Base, typename Alphabet>
alignas(hardware_constructive_interference_size)
    typename to_ascii_table<Base, Alphabet>::data_type_ const
    to_ascii_table<Base, Alphabet>::data =
        to_ascii_table<Base, Alphabet>::data_();

extern template to_ascii_table<8, to_ascii_alphabet_lower>::data_type_ const
    to_ascii_table<8, to_ascii_alphabet_lower>::data;
extern template to_ascii_table<10, to_ascii_alphabet_lower>::data_type_ const
    to_ascii_table<10, to_ascii_alphabet_lower>::data;
extern template to_ascii_table<16, to_ascii_alphabet_lower>::data_type_ const
    to_ascii_table<16, to_ascii_alphabet_lower>::data;
extern template to_ascii_table<8, to_ascii_alphabet_upper>::data_type_ const
    to_ascii_table<8, to_ascii_alphabet_upper>::data;
extern template to_ascii_table<10, to_ascii_alphabet_upper>::data_type_ const
    to_ascii_table<10, to_ascii_alphabet_upper>::data;
extern template to_ascii_table<16, to_ascii_alphabet_upper>::data_type_ const
    to_ascii_table<16, to_ascii_alphabet_upper>::data;

template <uint64_t Base, typename Int>
struct to_ascii_powers {
  static constexpr size_t size_(Int v) {
    return 1 + (v < Base ? 0 : size_(v / Base));
  }
  static constexpr size_t const size = size_(~Int(0));
  using data_type_ = c_array<Int, size>;
  static constexpr data_type_ data_() {
    data_type_ result{};
    for (size_t i = 0; i < size; ++i) {
      result.data[i] = constexpr_pow(Base, i);
    }
    return result;
  }
  // @lint-ignore CLANGTIDY
  static data_type_ const data;
};
template <uint64_t Base, typename Int>
alignas(hardware_constructive_interference_size)
    typename to_ascii_powers<Base, Int>::data_type_ const
    to_ascii_powers<Base, Int>::data = to_ascii_powers<Base, Int>::data_();

extern template to_ascii_powers<8, uint64_t>::data_type_ const
    to_ascii_powers<8, uint64_t>::data;
extern template to_ascii_powers<10, uint64_t>::data_type_ const
    to_ascii_powers<10, uint64_t>::data;
extern template to_ascii_powers<16, uint64_t>::data_type_ const
    to_ascii_powers<16, uint64_t>::data;

template <uint64_t Base>
FOLLY_ALWAYS_INLINE size_t to_ascii_size_imuls(uint64_t v) {
  using powers = to_ascii_powers<Base, uint64_t>;
  uint64_t p = 1;
  for (size_t i = 0u; i < powers::size; ++i, p *= Base) {
    if (v < p) [[unlikely]] {
      return i + size_t(i == 0);
    }
  }
  return powers::size;
}

template <uint64_t Base>
FOLLY_ALWAYS_INLINE size_t to_ascii_size_idivs(uint64_t v) {
  size_t i = 1;
  while (v >= Base) {
    i += 1;
    v /= Base;
  }
  return i;
}

template <uint64_t Base>
FOLLY_ALWAYS_INLINE size_t to_ascii_size_array(uint64_t v) {
  using powers = to_ascii_powers<Base, uint64_t>;
  for (size_t i = 0u; i < powers::size; ++i) {
    if (v < powers::data.data[i]) [[unlikely]] {
      return i + size_t(i == 0);
    }
  }
  return powers::size;
}

//  The largest e for which Base^e <= 2^bits, for bits in [0, 64], which is the
//  log change-of-base floor(bits / log<2>(Base)).
template <uint64_t Base>
constexpr size_t to_ascii_size_clzll_exp(size_t bits) {
  constexpr auto const max = ~uint64_t(0);
  //  Base^(e+1) <= 2^bits is Base^e <= floor(2^bits / Base), which avoids the
  //  overflow of forming Base^(e+1) directly. At bits == 64 the dividend is not
  //  representable; it exceeds max by exactly 1, which rounds up the quotient
  //  just when Base divides it, which is just when max % Base is Base - 1.
  uint64_t const lim = bits < 64
      ? (uint64_t(1) << bits) / Base
      : max / Base + (max % Base == Base - 1 ? 1 : 0);
  //  count the powers of Base within lim by dividing down rather than by
  //  multiplying up, since Base^(e+1) is 2^64 exactly when Base divides it
  size_t e = 0;
  for (uint64_t q = lim; q; q /= Base) {
    ++e;
  }
  return e;
}

//  The multiplier m for which (bits * m) >> shift is exactly the change-of-base
//  to_ascii_size_clzll_exp<Base>(bits) for every bits in [0, 64].
//
//  Each bits admits a contiguous interval of such multipliers; the intersection
//  over all bits is what makes the estimate below exact for every base rather
//  than only for the bases to_ascii_size_route happens to send here. Returns 0
//  when the intersection is empty, which callers reject at compile time.
template <uint64_t Base>
constexpr uint64_t to_ascii_size_clzll_mult(size_t shift) {
  uint64_t lo = 0;
  uint64_t hi = ~uint64_t(0);
  for (size_t bits = 1; bits <= 64; ++bits) {
    //  (bits * m) >> shift == e is e <= bits * m / 2^shift < e + 1
    uint64_t const e = to_ascii_size_clzll_exp<Base>(bits);
    uint64_t const elo = ((e << shift) + bits - 1) / bits;
    uint64_t const ehi = (((e + 1) << shift) + bits - 1) / bits - 1;
    lo = elo > lo ? elo : lo;
    hi = ehi < hi ? ehi : hi;
  }
  return lo <= hi ? lo : 0;
}

//  For some architectures, we can get a little help from clzll, the "count
//  leading zeros" builtin, which is backed by a single performant instruction.
//
//  Note that the compiler implements __builtin_clzll on all architectures, but
//  only emits a single clzll instruction when the architecture has one.
//
//  This implementation may be faster than the basic ones in the general case
//  because the time taken to compute this one is constant for non-zero v,
//  whereas the basic ones take time proportional to log<2>(v). Whether this one
//  is actually faster depends on the emitted code for this implementation and
//  on whether the loops in the basic implementations are unrolled.
template <uint64_t Base>
FOLLY_ALWAYS_INLINE size_t to_ascii_size_clzll(uint64_t v) {
  using powers = to_ascii_powers<Base, uint64_t>;

  //  clzll is undefined for 0; must special case this
  if (!v) [[unlikely]] {
    return 1;
  }

  //  vlog2 is approx log<2>(v)
  size_t const vlog2 = 64 - static_cast<size_t>(__builtin_clzll(v));

  //  handle directly when Base is power-of-two
  if constexpr (!(Base & (Base - 1))) {
    constexpr auto const blog2 = constexpr_log2(Base);
    return vlog2 / blog2 + size_t(vlog2 % blog2 != 0);
  }

  //  mult / 2^shift is 1 / log<2>(Base), to just enough precision that the log
  //  change-of-base just below is exact rather than approximate.
  //
  //  Any shift in [12, 57] serves, for every base up to 4096. Below that the
  //  multiplier is too coarse for some bases to admit an exact one at all,
  //  which the static_assert rejects; above it, the (e + 1) << shift within the
  //  derivation silently overflows, which nothing rejects. Within the range
  //  more bits buy nothing, since the multiplier is already exact, so take the
  //  largest at which every multiplier is at most 2^16 and so costs aarch64 a
  //  single movz rather than a movz/movk pair. At 17, base 3 already needs two.
  constexpr size_t const shift = 16;
  constexpr auto const mult = to_ascii_size_clzll_mult<Base>(shift);
  static_assert(mult, "no exact change-of-base multiplier for this base");

  //  vlogb is log<Base>(v) rounded down, so the digit count is vlogb or, once
  //  v reaches Base^vlogb, one more
  auto const vlogb = size_t((vlog2 * mult) >> shift);

  //  return vlogb, adjusted if necessary
  return vlogb + size_t(vlogb < powers::size && v >= powers::data.data[vlogb]);
}

//  Whether __builtin_clzll is cheap enough to beat the array loop. It is
//  wherever the target has a count-leading-zeros instruction - amd64 bsr or
//  lzcnt, aarch64 and arm clz, ppc64 cntlzd, s390x flogr, wasm i64.clz -
//  including the 32-bit ones, which take two 32-bit scans and a select. Not
//  so on riscv without the Zbb extension, where it expands to a longer inline
//  sequence or calls __clzdi2.
constexpr bool to_ascii_clzll_native = !kIsArchRISCV64 || kIsArchRISCVZbb;

template <uint64_t Base>
FOLLY_ALWAYS_INLINE size_t to_ascii_size_route(uint64_t v) {
  //  clzll sizing is branchless and constant-time, and is exact for every
  //  base. The array loop is no faster for narrow values and degrades with
  //  width, and its exit branch mispredicts whenever the width varies, so
  //  prefer clzll wherever the hardware backs it.
  return to_ascii_clzll_native //
      ? to_ascii_size_clzll<Base>(v)
      : to_ascii_size_array<Base>(v);
}

//  The straightforward implementation, assuming the size known in advance.
//
//  The straightforward implementation without the size known in advance would
//  entail emitting the bytes backward and then reversing them at the end, once
//  the size is known.
template <uint64_t Base, typename Alphabet>
FOLLY_ALWAYS_INLINE void to_ascii_with_basic(
    char* out, size_t size, uint64_t v) {
  Alphabet const xlate;
  for (auto pos = size - 1; pos; --pos) {
    //  keep /, % together so a peephole optimization computes them together
    auto const q = v / Base;
    auto const r = v % Base;
    out[pos] = xlate(uint8_t(r));
    v = q;
  }
  out[0] = xlate(uint8_t(v));
}

//  A variant of the straightforward implementation, but using a lookup table.
template <uint64_t Base, typename Alphabet>
FOLLY_ALWAYS_INLINE void to_ascii_with_array(
    char* out, size_t size, uint64_t v) {
  using array = to_ascii_array<Base, Alphabet>; // also an alphabet
  to_ascii_with_basic<Base, array>(out, size, v);
}

//  A trickier implementation which performs half as many divides as the other,
//  more straightforward, implementation. On modern hardware, the divides are
//  the bottleneck (even when the compiler emits a complicated sequence of add,
//  sub, and mul instructions with special constants to simulate a divide by a
//  fixed denominator).
//
//  The downside of this implementation is that the emitted code is larger,
//  especially when the divide is simulated, which affects inlining decisions.
template <uint64_t Base, typename Alphabet>
FOLLY_ALWAYS_INLINE void to_ascii_with_table(
    char* out, size_t size, uint64_t v) {
  using table = to_ascii_table<Base, Alphabet>;
  auto pos = size;
  while (pos > 2) {
    pos -= 2;
    //  keep /, % together so a peephole optimization computes them together
    auto const q = v / (Base * Base);
    auto const r = v % (Base * Base);
    auto const val = table::data.data[size_t(r)];
    std::memcpy(out + pos, &val, 2);
    v = q;
  }

  auto const val = table::data.data[size_t(v)];
  if (pos == 2) {
    std::memcpy(out, &val, 2);
  } else {
    *out = val >> (kIsLittleEndian ? 8 : 0);
  }
}

// Assumes that size >= number of digits in v. If >, the result is left-padded
// with 0s.
template <uint64_t Base, typename Alphabet>
FOLLY_ALWAYS_INLINE void to_ascii_with_route(
    char* outb, size_t size, uint64_t v) {
  kIsMobile //
      ? to_ascii_with_array<Base, Alphabet>(outb, size, v)
      : to_ascii_with_table<Base, Alphabet>(outb, size, v);
}
template <uint64_t Base, typename Alphabet>
FOLLY_ALWAYS_INLINE size_t
to_ascii_with_route(char* outb, char const* oute, uint64_t v) {
  auto const size = to_ascii_size_route<Base>(v);
  if (oute < outb || size_t(oute - outb) < size) [[unlikely]] {
    return 0;
  }
  to_ascii_with_route<Base, Alphabet>(outb, size, v);
  return size;
}
template <uint64_t Base, typename Alphabet, size_t N>
FOLLY_ALWAYS_INLINE size_t to_ascii_with_route(char (&out)[N], uint64_t v) {
  static_assert(N >= to_ascii_powers<Base, decltype(v)>::size, "out too small");
  auto const size = to_ascii_size_route<Base>(v);
  to_ascii_with_route<Base, Alphabet>(out, size, v);
  return size;
}

} // namespace detail

//  to_ascii_size_max
//
//  The maximum size buffer that might be required to hold the ascii-encoded
//  representation of any value of unsigned type I in base Base.
//
//  In base 10, u64 requires at most 20 bytes, u32 at most 10, u16 at most 5,
//  and u8 at most 3.
template <uint64_t Base, typename Int>
inline constexpr size_t to_ascii_size_max =
    detail::to_ascii_powers<Base, Int>::size;

//  to_ascii_size_max_decimal
//
//  An alias to to_ascii_size_max<10>.
template <typename Int>
inline constexpr size_t to_ascii_size_max_decimal = to_ascii_size_max<10, Int>;

//  to_ascii_size
//
//  Returns the number of digits in the base Base representation of a uint64_t.
//  Useful for preallocating buffers, etc.
//
//  async-signal-safe
template <uint64_t Base>
size_t to_ascii_size(uint64_t v) {
  return detail::to_ascii_size_route<Base>(v);
}

//  to_ascii_size_decimal
//
//  An alias to to_ascii_size<10>.
//
//  async-signal-safe
inline size_t to_ascii_size_decimal(uint64_t v) {
  return to_ascii_size<10>(v);
}

//  to_ascii_with
//
//  Copies the digits of v, in base Base, translated with Alphabet, into buffer
//  and returns the number of bytes written.
//
//  Does *not* append a null terminator. It is the caller's responsibility to
//  append a null terminator if one is required.
//
//  Requires at least to_ascii_size<Base>(v) bytes of writable memory. The two
//  overloads report a shortfall differently:
//
//  The [outb, oute) overload checks the range. When it is too small, nothing is
//  written and the return is 0. A successful call writes at least one byte, so
//  a return of 0 is unambiguously that failure and is never a short write.
//
//  The char[N] overload checks N against the largest value of type uint64_t at
//  compile time, so it is always large enough and never returns 0.
//
//  async-signal-safe
template <uint64_t Base, typename Alphabet>
size_t to_ascii_with(char* outb, char const* oute, uint64_t v) {
  return detail::to_ascii_with_route<Base, Alphabet>(outb, oute, v);
}
template <uint64_t Base, typename Alphabet, size_t N>
size_t to_ascii_with(char (&out)[N], uint64_t v) {
  return detail::to_ascii_with_route<Base, Alphabet>(out, v);
}

//  to_ascii_lower
//
//  Composes to_ascii_with with to_ascii_alphabet_lower.
//
//  async-signal-safe
template <uint64_t Base>
size_t to_ascii_lower(char* outb, char const* oute, uint64_t v) {
  return to_ascii_with<Base, to_ascii_alphabet_lower>(outb, oute, v);
}
template <uint64_t Base, size_t N>
size_t to_ascii_lower(char (&out)[N], uint64_t v) {
  return to_ascii_with<Base, to_ascii_alphabet_lower>(out, v);
}

//  to_ascii_upper
//
//  Composes to_ascii_with with to_ascii_alphabet_upper.
//
//  async-signal-safe
template <uint64_t Base>
size_t to_ascii_upper(char* outb, char const* oute, uint64_t v) {
  return to_ascii_with<Base, to_ascii_alphabet_upper>(outb, oute, v);
}
template <uint64_t Base, size_t N>
size_t to_ascii_upper(char (&out)[N], uint64_t v) {
  return to_ascii_with<Base, to_ascii_alphabet_upper>(out, v);
}

//  to_ascii_decimal
//
//  An alias to to_ascii_lower<10>.
//
//  async-signal-safe
inline size_t to_ascii_decimal(char* outb, char const* oute, uint64_t v) {
  return to_ascii_lower<10>(outb, oute, v);
}
template <size_t N>
inline size_t to_ascii_decimal(char (&out)[N], uint64_t v) {
  return to_ascii_lower<10>(out, v);
}

} // namespace folly
