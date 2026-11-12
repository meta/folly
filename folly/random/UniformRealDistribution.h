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
#include <cmath>
#include <cstdint>
#include <istream>
#include <limits>
#include <ostream>
#include <random>
#include <type_traits>

namespace folly {
namespace detail {

template <typename UInt, typename RNG>
UInt uniform_real_distribution_bits(RNG& rng) {
  constexpr bool uniform =
      RNG::min() == 0 && (RNG::max() & (RNG::max() + 1)) == 0;
  if constexpr (uniform && RNG::max() >= std::numeric_limits<UInt>::max()) {
    return static_cast<UInt>(rng());
  } else if constexpr (
      uniform && std::is_same_v<UInt, uint64_t> &&
      RNG::max() >= std::numeric_limits<uint32_t>::max()) {
    auto high = static_cast<uint64_t>(static_cast<uint32_t>(rng()));
    auto low = static_cast<uint32_t>(rng());
    return (high << 32) | low;
  } else {
    return std::uniform_int_distribution<UInt>()(rng);
  }
}

// Sample the exactly representable grid k / 2^Bits in [0, 1). Discarding
// low bits avoids rounding the largest RNG outputs up to 1.
template <
    typename Real,
    int Bits = std::numeric_limits<Real>::digits,
    typename RNG>
Real uniform_real_distribution_canonical(RNG& rng) {
  static_assert(std::numeric_limits<Real>::radix == 2);
  constexpr int bits = Bits < 64 ? Bits : 64;
  using UInt = std::conditional_t<(bits <= 32), uint32_t, uint64_t>;
  constexpr int shift = std::numeric_limits<UInt>::digits - bits;
  constexpr Real scale = Real{0x1p-64} * (uint64_t{1} << (64 - bits));
  auto randomBits = uniform_real_distribution_bits<UInt>(rng);
  auto value = static_cast<Real>(randomBits >> shift) * scale;
  if constexpr (Bits > 64) {
    auto tail = uniform_real_distribution_canonical<Real, Bits - 64>(rng);
    value += tail * Real{0x1p-64};
  }
  return value;
}

} // namespace detail

/**
 * A uniform real distribution in [a, b), with the interface of
 * std::uniform_real_distribution. Bounds must be finite and ordered, with a
 * finite span b - a; equal bounds return that value without consuming the RNG.
 *
 * Canonical values use numeric_limits<RealType>::digits random bits, without
 * the runtime logarithms some standard libraries use to count RNG draws.
 * Seeded sequences need not match std::uniform_real_distribution.
 */
template <typename RealType = double>
class uniform_real_distribution {
  static_assert(std::is_floating_point_v<RealType>);

 public:
  using result_type = RealType;

  class param_type {
   public:
    using distribution_type = uniform_real_distribution;

    explicit param_type(RealType a = 0, RealType b = 1) : a_(a), b_(b) {
      assert(std::isfinite(a) && std::isfinite(b) && a <= b);
      assert(std::isfinite(b - a));
    }

    RealType a() const { return a_; }
    RealType b() const { return b_; }

    friend bool operator==(const param_type&, const param_type&) = default;

   private:
    RealType a_;
    RealType b_;
  };

  explicit uniform_real_distribution(RealType a = 0, RealType b = 1)
      : param_(a, b) {}

  explicit uniform_real_distribution(const param_type& param) : param_(param) {}

  void reset() {}

  RealType a() const { return param_.a(); }
  RealType b() const { return param_.b(); }
  RealType min() const { return a(); }
  RealType max() const { return b(); }
  param_type param() const { return param_; }
  void param(const param_type& param) { param_ = param; }

  template <typename RNG>
  result_type operator()(RNG& rng) const {
    return (*this)(rng, param_);
  }

  template <typename RNG>
  result_type operator()(RNG& rng, const param_type& param) const {
    if (param.a() == param.b()) {
      return param.a();
    }
    auto value = detail::uniform_real_distribution_canonical<RealType>(rng);
    auto result = std::lerp(param.a(), param.b(), value);
    // Scaling can round up to the excluded upper endpoint.
    return result < param.b() ? result : std::nextafter(param.b(), param.a());
  }

  friend bool operator==(
      const uniform_real_distribution&,
      const uniform_real_distribution&) = default;

  template <typename Char, typename Traits>
  friend std::basic_ostream<Char, Traits>& operator<<(
      std::basic_ostream<Char, Traits>& out,
      const uniform_real_distribution& dist) {
    auto flags = out.flags();
    auto precision = out.precision();
    out.setf(std::ios_base::scientific, std::ios_base::floatfield);
    out.precision(std::numeric_limits<RealType>::max_digits10);
    out << dist.a() << out.widen(' ') << dist.b();
    out.flags(flags);
    out.precision(precision);
    return out;
  }

  template <typename Char, typename Traits>
  friend std::basic_istream<Char, Traits>& operator>>(
      std::basic_istream<Char, Traits>& in, uniform_real_distribution& dist) {
    RealType a;
    RealType b;
    if (in >> a >> b) {
      if (std::isfinite(a) && std::isfinite(b) && a <= b &&
          std::isfinite(b - a)) {
        dist.param(param_type(a, b));
      } else {
        in.setstate(std::ios_base::failbit);
      }
    }
    return in;
  }

 private:
  param_type param_;
};

} // namespace folly
