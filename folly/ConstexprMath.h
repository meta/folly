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

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numbers>
#include <type_traits>

#include <folly/Portability.h>
#include <folly/lang/CheckedMath.h>

// Apple Clang in Xcode 16.3 thru 26.3 reports Clang 17, when it is actually
// based on upstream Clang 19
#if __clang_major__ >= 18 || \
    (defined(__apple_build_version__) && __clang_major__ >= 17)
#define FOLLY_DETAIL_DISABLE_NAN_INFINITY_DISABLED \
  FOLLY_CLANG_DISABLE_WARNING("-Wnan-infinity-disabled")
#else
#define FOLLY_DETAIL_DISABLE_NAN_INFINITY_DISABLED
#endif

#if FOLLY_HAS_FEATURE(cxx_constexpr_string_builtins) || \
    FOLLY_HAS_BUILTIN(__builtin_strcmp)
#define FOLLY_DETAIL_STRCMP __builtin_strcmp
#else
#define FOLLY_DETAIL_STRCMP ::std::strcmp
#endif

namespace folly {

/// floating_point_integral_constant
///
/// Like std::integral_constant but for floating-point types holding integral
/// values representable in an integral type.
template <typename T, typename S, S Value>
struct floating_point_integral_constant {
  using value_type = T;
  static constexpr value_type value = static_cast<value_type>(Value);
  constexpr operator value_type() const noexcept { return value; }
  constexpr value_type operator()() const noexcept { return value; }
};

//  ----

namespace detail {

template <typename T>
constexpr size_t constexpr_iterated_squares_desc_size_(T const base) {
  using lim = std::numeric_limits<T>;
  size_t s = 1;
  auto r = base;
  while (r <= lim::max() / r) {
    ++s;
    r *= r;
  }
  return s;
}

} // namespace detail

/// constexpr_iterated_squares_desc_size_v
///
/// Effectively calculates: floor(log(max_exponent)/log(base))
///
/// For use with constexpr_iterated_squares_desc below.
template <typename Base>
inline constexpr size_t constexpr_iterated_squares_desc_size_v =
    detail::constexpr_iterated_squares_desc_size_(Base::value);

/// constexpr_iterated_squares_desc
///
/// A constexpr scaling array of integer powers-of-powers-of-two, descending,
/// with the associated powers-of-two.
///
/// scaling = [..., {8, b^8}, {4, b^4}, {2, b^2}, {1, b^1}] for b = base
///
/// Includes select constexpr scaling algorithms based on the scaling array.
///
/// The scaling array and the scaling algorithms are general-purpose, if niche.
/// They may be used by other constexpr math functions (floating-point) either
/// to improve runtime performance or to improve numerical approximations.
///
/// Some compilers fail to support passing some types as non-type template
/// params. In particular, long double is not universally supported. Therefore,
/// this utility takes its base as a type rather than as a value. For floating-
/// point integral bases, that is, bases of floating-point type but of integral
/// value, floating_point_integral_constant is the easiest parameterization.
template <typename T, std::size_t Size>
struct constexpr_iterated_squares_desc {
  static_assert(Size > 0, "requires non-zero size");

  using size_type = decltype(Size);
  using base_type = T;

  struct item_type {
    size_type power;
    base_type scale;
  };

  static constexpr size_type size = Size;
  base_type base;
  item_type scaling[size];

 private:
  using lim = std::numeric_limits<base_type>;

  static_assert(
      lim::max_exponent < std::numeric_limits<size_type>::max(),
      "size_type too small for base_type");

 public:
  explicit constexpr constexpr_iterated_squares_desc(base_type r) noexcept
      : base{r}, scaling{} {
    assert(size <= detail::constexpr_iterated_squares_desc_size_(base));
    size_type i = 0;
    size_type p = 1;
    while (true) { // a for-loop might cause multiplication overflow below
      scaling[size - 1 - i] = {p, r};
      if (++i == size) {
        break;
      }
      p *= 2;
      r *= r;
    }
  }

  /// shrink
  ///
  /// Returns scaling params of the form:
  ///   item_type{power, scale} with scale = base ^ power
  /// With power the smallest nonnegative integer such that:
  ///   abs(num) / scale <= max
  constexpr item_type shrink(base_type const num, base_type const max) const {
    assert(max > base_type(0));
    auto const rmax = max / base;
    auto const snum = num < base_type(0) ? -num : num;
    auto power = size_type(0);
    auto scale = base_type(1);
    if (!(snum / scale <= max)) {
      for (auto const& i : scaling) {
        auto const next = scale * i.scale;
        auto const div = snum / next;
        if (div <= rmax) {
          continue;
        }
        power += i.power;
        scale = next;
        if (div <= max) {
          break;
        }
      }
    }
    assert(snum / scale <= max);
    return {power, scale};
  }

  /// growth
  ///
  /// Returns scaling params of the form:
  ///   item_type{power, scale} with scale = base ^ power
  /// With power the smallest nonnegative integer such that:
  ///   abs(num) * scale >= min
  constexpr item_type growth(base_type const num, base_type const min) const {
    assert(min > base_type(0));
    auto const rmin = min * base;
    auto const snum = num < base_type(0) ? -num : num;
    auto power = size_type(0);
    auto scale = base_type(1);
    if (!(snum * scale >= min)) {
      for (auto const& i : scaling) {
        auto const next = scale * i.scale;
        auto const mul = snum * next;
        if (mul >= rmin) {
          continue;
        }
        power += i.power;
        scale = next;
        if (mul >= min) {
          break;
        }
      }
    }
    assert(snum * scale >= min);
    return {power, scale};
  }
};

/// constexpr_iterated_squares_desc_v
///
/// An instance of constexpr_iterated_squares_desc of max size with the given
/// base.
template <typename Base>
inline constexpr auto constexpr_iterated_squares_desc_v =
    constexpr_iterated_squares_desc<
        typename Base::value_type,
        constexpr_iterated_squares_desc_size_v<Base>>{Base::value};

/// constexpr_iterated_squares_desc_2_v
///
/// An alias for constexpr_iterated_squares_desc_v with base 2, which is the
/// most common base to use with iterated-squares.
template <typename T>
constexpr auto& constexpr_iterated_squares_desc_2_v =
    constexpr_iterated_squares_desc_v<
        floating_point_integral_constant<T, int, 2>>;

// TLDR: Prefer using operator< for ordering. And when
// a and b are equivalent objects, we return b to make
// sorting stable.
// See http://stepanovpapers.com/notes.pdf for details.
template <typename T, typename... Ts>
constexpr T constexpr_max(T a, Ts... ts) {
  T list[] = {ts..., a}; // 0-length arrays are illegal
  for (auto i = 0u; i < sizeof...(Ts); ++i) {
    a = list[i] < a ? a : list[i];
  }
  return a;
}

// When a and b are equivalent objects, we return a to
// make sorting stable.
template <typename T, typename... Ts>
constexpr T constexpr_min(T a, Ts... ts) {
  T list[] = {ts..., a}; // 0-length arrays are illegal
  for (auto i = 0u; i < sizeof...(Ts); ++i) {
    a = list[i] < a ? list[i] : a;
  }
  return a;
}

template <typename T, typename Less>
constexpr T const& constexpr_clamp(
    T const& v, T const& lo, T const& hi, Less less) {
  T const& a = less(v, lo) ? lo : v;
  T const& b = less(hi, a) ? hi : a;
  return b;
}
template <typename T>
constexpr T const& constexpr_clamp(T const& v, T const& lo, T const& hi) {
  return constexpr_clamp(v, lo, hi, std::less<T>{});
}

template <typename T>
constexpr bool constexpr_isnan(T const t) {
  return t != t; // NOLINT
}

/// constexpr_abs
///
/// Returns the absolute value of t. For a signed integral type, the result has
/// the corresponding unsigned type.
template <typename T>
  requires std::floating_point<T> ||
    (std::integral<T> && !std::same_as<T, bool>)
constexpr auto constexpr_abs(T t) {
  if constexpr (std::floating_point<T>) {
    return t < static_cast<T>(0) ? -t : t;
  } else if constexpr (std::unsigned_integral<T>) {
    return t;
  } else {
    return std::make_unsigned_t<T>(t < static_cast<T>(0) ? -t : t);
  }
}

namespace detail {

template <typename T>
constexpr T constexpr_log2_(T a, T e) {
  return e == T(1) ? a : constexpr_log2_(a + T(1), e / T(2));
}

template <typename T>
constexpr T constexpr_log2_ceil_(T l2, T t) {
  return l2 + T(T(1) << l2 < t ? 1 : 0);
}

} // namespace detail

template <typename T>
constexpr T constexpr_log2(T t) {
  return detail::constexpr_log2_(T(0), t);
}

template <typename T>
constexpr T constexpr_log2_ceil(T t) {
  return detail::constexpr_log2_ceil_(constexpr_log2(t), t);
}

/// constexpr_trunc
///
/// mimic: std::trunc (C++23)
template <typename T>
  requires std::floating_point<T>
constexpr T constexpr_trunc(T const t) {
  using lim = std::numeric_limits<T>;
  using int_type = std::uintmax_t;
  using int_lim = std::numeric_limits<int_type>;
  static_assert(lim::radix == 2, "non-binary radix");
  static_assert(lim::digits <= int_lim::digits, "overwide mantissa");
  constexpr auto bound = static_cast<T>(std::uintmax_t(1) << (lim::digits - 1));
  auto const neg = !constexpr_isnan(t) && t < T(0);
  auto const s = neg ? -t : t;
  if (constexpr_isnan(t) || t == T(0) || !(s < bound)) {
    return t;
  }
  if (s < T(1)) {
    return neg ? -T(0) : T(0);
  }
  auto const r = static_cast<T>(static_cast<int_type>(s));
  return neg ? -r : r;
}

template <typename T>
  requires std::integral<T>
constexpr T constexpr_trunc(T const t) {
  return t;
}

/// constexpr_round
///
/// mimic: std::round (C++23)
template <typename T>
constexpr T constexpr_round(T const t) {
  constexpr auto half = T(1) / T(2);
  auto const same = constexpr_isnan(t) || t == T(0);
  return same ? t : constexpr_trunc(t < T(0) ? t - half : t + half);
}

/// constexpr_floor
///
/// mimic: std::floor (C++23)
template <typename T>
constexpr T constexpr_floor(T const t) {
  auto const s = constexpr_trunc(t);
  return t < s ? s - T(1) : s;
}

/// constexpr_ceil
///
/// mimic: std::ceil (C++23)
template <typename T>
constexpr T constexpr_ceil(T const t) {
  auto const s = constexpr_trunc(t);
  return s < t ? s + T(1) : s;
}

/// constexpr_ceil
///
/// The least integer at least t that round divides.
template <typename T>
constexpr T constexpr_ceil(T t, T round) {
  return round == T(0)
      ? t
      : ((t + (t <= T(0) ? T(0) : round - T(1))) / round) * round;
}

/// constexpr_mult
///
/// Multiply two values, allowing for constexpr floating-point overflow to
/// infinity.
template <typename T>
constexpr T constexpr_mult(T const a, T const b) {
  using lim = std::numeric_limits<T>;
  FOLLY_PUSH_WARNING
  FOLLY_DETAIL_DISABLE_NAN_INFINITY_DISABLED
  if (constexpr_isnan(a) || constexpr_isnan(b)) {
    return constexpr_isnan(a) ? a : b;
  }
  if (std::is_floating_point<T>::value) {
    constexpr auto inf = lim::infinity();
    auto const ax = constexpr_abs(a);
    auto const bx = constexpr_abs(b);
    if ((ax == T(0) && bx == inf) || (bx == T(0) && ax == inf)) {
      return lim::quiet_NaN();
    }
    // floating-point multiplication overflow, ie where multiplication of two
    // finite values overflows to infinity of either sign, is not constexpr per
    // gcc
    // floating-point division overflow, ie where division of two finite values
    // overflows to infinity of either sign, is not constexpr per gcc
    // floating-point division by zero is not constexpr per any compiler, but we
    // use it in the checks for the other two conditions
    if (ax != inf && bx != inf && T(1) < bx && lim::max() / bx < ax) {
      auto const a_neg = static_cast<bool>(a < T(0));
      auto const b_neg = static_cast<bool>(b < T(0));
      auto const sign = a_neg == b_neg ? T(1) : T(-1);
      return sign * inf;
    }
  }
  FOLLY_POP_WARNING
  return a * b;
}

namespace detail {

template <typename T, typename E>
  requires(std::signed_integral<E> || std::floating_point<E>)
constexpr T constexpr_ipow(T const base, E const exp) {
  if (std::is_floating_point<T>::value) {
    if (exp < E(0)) {
      return T(1) / constexpr_ipow(base, -exp);
    }
    if (exp == E(0)) {
      return T(1);
    }
    if (constexpr_isnan(base)) {
      return base;
    }
  }
  assert(!(exp < E(0)) && "negative exponent with integral base");
  if (exp == E(0)) {
    return T(1);
  }
  if (exp == E(1)) {
    return base;
  }
  auto const hexp = constexpr_trunc(exp / E(2));
  auto const div = constexpr_ipow(base, hexp);
  auto const rem = hexp * E(2) == exp ? T(1) : base;
  return constexpr_mult(constexpr_mult(div, div), rem);
}

template <typename T, typename E>
  requires std::unsigned_integral<E>
constexpr T constexpr_ipow(T const base, E const exp) {
  if (std::is_floating_point<T>::value) {
    if (exp == E(0)) {
      return T(1);
    }
    if (constexpr_isnan(base)) {
      return base;
    }
  }
  if (exp == E(0)) {
    return T(1);
  }
  if (exp == E(1)) {
    return base;
  }
  auto const hexp = constexpr_trunc(exp / E(2));
  auto const div = constexpr_ipow(base, hexp);
  auto const rem = hexp * E(2) == exp ? T(1) : base;
  return constexpr_mult(constexpr_mult(div, div), rem);
}

} // namespace detail

/// constexpr_exp
///
/// Calculates an approximation of the mathematical function exp(num). Usable in
/// constant evaluations. Like std::exp, which becomes constexpr in C++26.
///
/// The integer overload uses iterated squaring and multiplication. The
/// floating-point overload naively evaluates the taylor series of exp(num)
/// until approximate convergence.
///
/// mimic: std::exp (C++23, C++26)
template <typename T, typename N>
  requires(std::floating_point<T> && std::integral<N> && !std::same_as<N, bool>)
constexpr T constexpr_exp(N const power) {
  auto const npower = constexpr_abs(power);
  auto const result = detail::constexpr_ipow(std::numbers::e_v<T>, npower);
  return power < N(0) ? T(1) / result : result;
}
template <typename N>
  requires(std::integral<N> && !std::same_as<N, bool>)
constexpr double constexpr_exp(N const power) {
  return constexpr_exp<double>(power);
}
template <typename T>
  requires std::floating_point<T>
constexpr T constexpr_exp(T const power) {
  using lim = std::numeric_limits<T>;

  FOLLY_PUSH_WARNING
  FOLLY_DETAIL_DISABLE_NAN_INFINITY_DISABLED
  // edge cases
  if (constexpr_isnan(power)) {
    return power;
  }
  if (power == -lim::infinity()) {
    return +T(0);
  }
  if (power == +lim::infinity()) {
    return power;
  }

  // convergence works better with positive powers since signs do not alternate
  auto const abspower = constexpr_abs(power);
  // convergence must short-circuit when terms grow to floating-point infinity
  auto const bound = T(1) < abspower ? lim::max() / abspower : lim::infinity();

  // term #index = power * coeff
  auto index = size_t(0);
  auto term = T(1);
  // result = sum of terms
  auto result = T(1);
  // sum the terms until ~convergence
  while (!(constexpr_abs(term) < lim::epsilon())) {
    if (bound < term) {
      return power < T(0) ? T(0) : lim::infinity();
    }
    index += 1;
    term = term * abspower / index;
    result += term;
  }
  FOLLY_POP_WARNING
  return power < T(0) ? T(1) / result : result;
}

/// constexpr_log
///
/// Calculates an approximation of the natural logarithm ln(num).
///
/// The implementation uses a quickly-converging, high-precision iterative
/// technique as described in:
///   https://en.wikipedia.org/wiki/Natural_logarithm#High_precision
///
/// The technique works best with numbers that are close enough to 1, so the
/// implementation uses a quick shrink/growth technique as described in:
///   https://en.wikipedia.org/wiki/Natural_logarithm#Efficient_computation
template <typename T>
  requires std::floating_point<T>
constexpr T constexpr_log(T const num) {
  using lim = std::numeric_limits<T>;
  constexpr auto& isq = constexpr_iterated_squares_desc_2_v<T>;

  FOLLY_PUSH_WARNING
  FOLLY_DETAIL_DISABLE_NAN_INFINITY_DISABLED
  // edge cases
  if (constexpr_isnan(num)) {
    return num;
  }
  if (num < T(0)) {
    return lim::quiet_NaN();
  }
  if (num == T(0)) {
    return -lim::infinity();
  }
  if (num == lim::infinity()) {
    return num;
  }
  FOLLY_POP_WARNING

  // compression
  auto const shrink = isq.shrink(num, isq.base);
  auto const growth = isq.growth(num, T(1));
  auto const scaled = num * growth.scale / shrink.scale;
  assert(scaled <= isq.base);
  assert(scaled >= T(1));

  auto sum = T(0);
  auto delta = T(2);
  while (constexpr_abs(delta) >= lim::epsilon()) {
    auto expterm = constexpr_exp(sum);
    delta = T(2) * (scaled - expterm) / (scaled + expterm);
    sum += delta;
  }
  auto const ln2 = std::numbers::ln2_v<T>;
  return sum - growth.power * ln2 + shrink.power * ln2;
}

/// constexpr_pow
///
/// Calculates an approximation of the value of base raised to the exponent exp.
///
/// The implementation uses iterated squaring and multiplication for the integer
/// part of the exponent and uses the identity x^y = exp(y * log(x)) for the
/// fractional part of the exponent.
///
/// Notes:
/// * Forbids base of +0 or -0 with finite non-positive exponent: in part since
///   the plausible infinite result would be sensitive to the sign of the zero;
///   and in part since std::pow would be required or permitted to raise error
///   div-by-zero.
/// * Forbids finite negative base with finite non-integer exponent: in part
///   since std::pow would be required to raise error invalid.
///
/// mimic: std::pow (C++26)
template <typename T, typename E>
  requires(std::integral<E> && !std::same_as<E, bool>)
constexpr T constexpr_pow(T const base, E const exp) {
  return detail::constexpr_ipow(base, exp);
}
template <typename T>
  requires std::floating_point<T>
constexpr T constexpr_pow(T const base, T const exp) {
  using lim = std::numeric_limits<T>;

  // edge cases
  if (exp == T(0)) {
    return T(1);
  }
  if (base == T(1)) {
    return base;
  }
  FOLLY_PUSH_WARNING
  FOLLY_DETAIL_DISABLE_NAN_INFINITY_DISABLED
  if (constexpr_isnan(base)) {
    return base;
  }
  if (exp == lim::infinity() || exp == -lim::infinity()) {
    auto const abase = constexpr_abs(base);
    if (abase < T(1)) {
      return exp == lim::infinity() ? T(0) : lim::infinity();
    }
    if (T(1) < abase) {
      return exp == lim::infinity() ? lim::infinity() : T(0);
    }
    return T(1);
  }
  if (constexpr_isnan(exp)) {
    return exp;
  }
  assert(base != T(0) || exp > T(0)); // error div-by-zero
  if (base == lim::infinity()) {
    return exp < T(0) ? T(0) : lim::infinity();
  }
  if (base == -lim::infinity()) {
    auto const oddi = //
        exp == constexpr_trunc(exp) &&
        exp != constexpr_trunc(exp / T(2)) * T(2);
    return (oddi ? -T(1) : T(1)) * (exp < T(0) ? T(0) : lim::infinity());
  }
  FOLLY_POP_WARNING
  if (base == T(0)) {
    auto const oddi = //
        exp == constexpr_trunc(exp) &&
        exp != constexpr_trunc(exp / T(2)) * T(2);
    return oddi ? base : T(0);
  }
  if (exp < T(0)) {
    return T(1) / constexpr_pow(base, -exp);
  }

  // as an identity: x^y = exp(y * log(x)); but calculation is imprecise ... so,
  // for better precision, split the calculation into its integral-power and its
  // fractional-power components
  // as a cost, the complexity of constexpr_ipow here is logarithmic in y, i.e.,
  // linear in the logarithm of y, which can be prohibitive
  auto const exp_trunc = constexpr_trunc(exp);
  assert(T(0) < base || exp == exp_trunc); // error invalid
  auto const exp_fract = exp - exp_trunc;
  auto const anyi = exp_fract == T(0);
  return constexpr_mult(
      detail::constexpr_ipow(base, exp_trunc),
      anyi ? T(1) : constexpr_exp(exp_fract * constexpr_log(base)));
}

/// constexpr_find_last_set
///
/// Return the 1-based index of the most significant bit which is set.
/// For x > 0, constexpr_find_last_set(x) == 1 + floor(log2(x)).
template <typename T>
constexpr std::size_t constexpr_find_last_set(T const t) {
  using U = std::make_unsigned_t<T>;
  return t == T(0) ? 0 : 1 + constexpr_log2(static_cast<U>(t));
}

namespace detail {
template <typename U>
constexpr std::size_t constexpr_find_first_set_(
    std::size_t s, std::size_t a, U const u) {
  return s == 0
      ? a
      : constexpr_find_first_set_(
            s / 2, a + s * bool((u >> a) % (U(1) << s) == U(0)), u);
}
} // namespace detail

/// constexpr_find_first_set
///
/// Return the 1-based index of the least significant bit which is set.
/// For x > 0, the exponent in the largest power of two which does not divide x.
template <typename T>
constexpr std::size_t constexpr_find_first_set(T t) {
  using U = std::make_unsigned_t<T>;
  using size = std::integral_constant<std::size_t, sizeof(T) * 4>;
  return t == T(0)
      ? 0
      : 1 + detail::constexpr_find_first_set_(size{}, 0, static_cast<U>(t));
}

template <typename T>
constexpr T constexpr_add_overflow_clamped(T a, T b) {
  using L = std::numeric_limits<T>;
  using M = std::intmax_t;
  static_assert(
      !std::is_integral<T>::value || sizeof(T) <= sizeof(M),
      "Integral type too large!");
  if (!std::is_constant_evaluated()) {
    if constexpr (std::is_integral_v<T>) {
      T ret{};
      if (!checked_add(&ret, a, b)) [[unlikely]] {
        if constexpr (std::is_signed_v<T>) {
          // Could be either overflow or underflow for signed types.
          // Can only be underflow if both inputs are negative.
          if (a < 0 && b < 0) {
            return L::min();
          }
        }
        return L::max();
      }
      return ret;
    }
  }
  // clang-format off
  return
    // don't do anything special for non-integral types.
    !std::is_integral<T>::value ? a + b :
    // for narrow integral types, just convert to intmax_t.
    sizeof(T) < sizeof(M)
      ? T(constexpr_clamp(
          static_cast<M>(a) + static_cast<M>(b),
          static_cast<M>(L::min()),
          static_cast<M>(L::max()))) :
    // when a >= 0, cannot add more than `MAX - a` onto a.
    !(a < 0) ? a + constexpr_min(b, T(L::max() - a)) :
    // a < 0 && b >= 0, `a + b` will always be in valid range of type T.
    !(b < 0) ? a + b :
    // a < 0 && b < 0, keep the result >= MIN.
              a + constexpr_max(b, T(L::min() - a));
  // clang-format on
}

template <typename T>
constexpr T constexpr_sub_overflow_clamped(T a, T b) {
  using L = std::numeric_limits<T>;
  using M = std::intmax_t;
  static_assert(
      !std::is_integral<T>::value || sizeof(T) <= sizeof(M),
      "Integral type too large!");
  // clang-format off
  return
    // don't do anything special for non-integral types.
    !std::is_integral<T>::value ? a - b :
    // for unsigned type, keep result >= 0.
    std::is_unsigned<T>::value ? (a < b ? 0 : a - b) :
    // for narrow signed integral types, just convert to intmax_t.
    sizeof(T) < sizeof(M)
      ? T(constexpr_clamp(
          static_cast<M>(a) - static_cast<M>(b),
          static_cast<M>(L::min()),
          static_cast<M>(L::max()))) :
    // (a >= 0 && b >= 0) || (a < 0 && b < 0), `a - b` will always be valid.
    (a < 0) == (b < 0) ? a - b :
    // MIN < b, so `-b` should be in valid range (-MAX <= -b <= MAX),
    // convert subtraction to addition.
    L::min() < b ? constexpr_add_overflow_clamped(a, T(-b)) :
    // -b = -MIN = (MAX + 1) and a <= -1, result is in valid range.
    a < 0 ? a - b :
    // -b = -MIN = (MAX + 1) and a >= 0, result > MAX.
            L::max();
  // clang-format on
}

// clamp_cast<> provides sane numeric conversions from float point numbers to
// integral numbers, and between different types of integral numbers. It helps
// to avoid unexpected bugs introduced by bad conversion, and undefined behavior
// like overflow when casting float point numbers to integral numbers.
//
// When doing clamp_cast<Dst>(value), if `value` is in valid range of Dst,
// it will give correct result in Dst, equal to `value`.
//
// If `value` is outside the representable range of Dst, it will be clamped to
// MAX or MIN in Dst, instead of being undefined behavior.
//
// Float NaNs are converted to 0 in integral type.
//
// Here's some comparison with static_cast<>:
// (with FB-internal gcc-5-glibc-2.23 toolchain)
//
// static_cast<int32_t>(NaN) = 6
// clamp_cast<int32_t>(NaN) = 0
//
// static_cast<int32_t>(9999999999.0f) = -348639895
// clamp_cast<int32_t>(9999999999.0f) = 2147483647
//
// static_cast<int32_t>(2147483647.0f) = -348639895
// clamp_cast<int32_t>(2147483647.0f) = 2147483647
//
// static_cast<uint32_t>(4294967295.0f) = 0
// clamp_cast<uint32_t>(4294967295.0f) = 4294967295
//
// static_cast<uint32_t>(-1) = 4294967295
// clamp_cast<uint32_t>(-1) = 0
//
// static_cast<int16_t>(32768u) = -32768
// clamp_cast<int16_t>(32768u) = 32767

template <typename Dst, typename Src>
  requires std::integral<Src>
constexpr Dst constexpr_clamp_cast(Src src) {
  static_assert(
      std::is_integral<Dst>::value && sizeof(Dst) <= sizeof(int64_t),
      "constexpr_clamp_cast can only cast into integral type (up to 64bit)");

  using L = std::numeric_limits<Dst>;
  // clang-format off
  return
    // Check if Src and Dst have same signedness.
    std::is_signed<Src>::value == std::is_signed<Dst>::value
    ? (
      // Src and Dst have same signedness. If sizeof(Src) <= sizeof(Dst),
      // we can safely convert Src to Dst without any loss of accuracy.
      sizeof(Src) <= sizeof(Dst) ? Dst(src) :
      // If Src is larger in size, we need to clamp it to valid range in Dst.
      Dst(constexpr_clamp(src, Src(L::min()), Src(L::max()))))
    // Src and Dst have different signedness.
    // Check if it's signed -> unsigned cast.
    : std::is_signed<Src>::value && std::is_unsigned<Dst>::value
    ? (
      // If src < 0, the result should be 0.
      src < 0 ? Dst(0) :
      // Otherwise, src >= 0. If src can fit into Dst, we can safely cast it
      // without loss of accuracy.
      sizeof(Src) <= sizeof(Dst) ? Dst(src) :
      // If Src is larger in size than Dst, we need to ensure the result is
      // at most Dst MAX.
      Dst(constexpr_min(src, Src(L::max()))))
    // It's unsigned -> signed cast.
    : (
      // Since Src is unsigned, and Dst is signed, Src can fit into Dst only
      // when sizeof(Src) < sizeof(Dst).
      sizeof(Src) < sizeof(Dst) ? Dst(src) :
      // If Src does not fit into Dst, we need to ensure the result is at most
      // Dst MAX.
      Dst(constexpr_min(src, Src(L::max()))));
  // clang-format on
}

namespace detail {
// Upper/lower bound values that could be accurately represented in both
// integral and float point types.
constexpr double kClampCastLowerBoundDoubleToInt64F = -9223372036854774784.0;
constexpr double kClampCastUpperBoundDoubleToInt64F = 9223372036854774784.0;
constexpr double kClampCastUpperBoundDoubleToUInt64F = 18446744073709549568.0;

constexpr float kClampCastLowerBoundFloatToInt32F = -2147483520.0f;
constexpr float kClampCastUpperBoundFloatToInt32F = 2147483520.0f;
constexpr float kClampCastUpperBoundFloatToUInt32F = 4294967040.0f;

// This works the same as constexpr_clamp, but the comparison are done in Src
// to prevent any implicit promotions.
template <typename D, typename S>
constexpr D constexpr_clamp_cast_helper(S src, S sl, S su, D dl, D du) {
  return src < sl ? dl : (src > su ? du : D(src));
}
} // namespace detail

template <typename Dst, typename Src>
  requires std::floating_point<Src>
constexpr Dst constexpr_clamp_cast(Src src) {
  static_assert(
      std::is_integral<Dst>::value && sizeof(Dst) <= sizeof(int64_t),
      "constexpr_clamp_cast can only cast into integral type (up to 64bit)");

  using L = std::numeric_limits<Dst>;
  // clang-format off
  return
    // Special case: cast NaN into 0.
    constexpr_isnan(src) ? Dst(0) :
    // using `sizeof(Src) > sizeof(Dst)` as a heuristic that Dst can be
    // represented in Src without loss of accuracy.
    // see: https://en.wikipedia.org/wiki/Floating-point_arithmetic
    sizeof(Src) > sizeof(Dst) ?
      detail::constexpr_clamp_cast_helper(
          src, Src(L::min()), Src(L::max()), L::min(), L::max()) :
    // sizeof(Src) < sizeof(Dst) only happens when doing cast of
    // 32bit float -> u/int64_t.
    // Losslessly promote float into double, change into double -> u/int64_t.
    sizeof(Src) < sizeof(Dst) ? (
      src >= 0.0
      ? constexpr_clamp_cast<Dst>(
            constexpr_clamp_cast<std::uint64_t>(double(src)))
      : constexpr_clamp_cast<Dst>(
            constexpr_clamp_cast<std::int64_t>(double(src)))) :
    // The following are for sizeof(Src) == sizeof(Dst).
    std::is_same<Src, double>::value && std::is_same<Dst, int64_t>::value ?
      detail::constexpr_clamp_cast_helper(
          double(src),
          detail::kClampCastLowerBoundDoubleToInt64F,
          detail::kClampCastUpperBoundDoubleToInt64F,
          L::min(),
          L::max()) :
    std::is_same<Src, double>::value && std::is_same<Dst, uint64_t>::value ?
      detail::constexpr_clamp_cast_helper(
          double(src),
          0.0,
          detail::kClampCastUpperBoundDoubleToUInt64F,
          L::min(),
          L::max()) :
    std::is_same<Src, float>::value && std::is_same<Dst, int32_t>::value ?
      detail::constexpr_clamp_cast_helper(
          float(src),
          detail::kClampCastLowerBoundFloatToInt32F,
          detail::kClampCastUpperBoundFloatToInt32F,
          L::min(),
          L::max()) :
      detail::constexpr_clamp_cast_helper(
          float(src),
          0.0f,
          detail::kClampCastUpperBoundFloatToUInt32F,
          L::min(),
          L::max());
  // clang-format on
}

} // namespace folly
