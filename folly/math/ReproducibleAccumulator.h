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

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <ranges>

#include <fmt/format.h>
#include <folly/ConstexprMath.h>

namespace folly {

/// Reproducible floating-point accumulator via binned floating-point
/// arithmetic.
///
/// Guarantees bitwise-identical sums regardless of summation order, assuming
/// IEEE 754 arithmetic. Based on the ReproBLAS algorithm by Ahrens, Nguyen,
/// and Demmel (https://bebop.cs.berkeley.edu/reproblas/).
///
/// Accuracy: the error has two components (see `error_bound()`). The first
/// grows as O(N * epsilon * max_abs_val), proportional to element count and
/// the largest element magnitude — not the condition number. The second is
/// O(epsilon * |sum|), independent of N. This is much more accurate than
/// naive summation (whose error grows as O(N * epsilon * |sum|) and is
/// sensitive to catastrophic cancellation), and comparable to Kahan
/// summation in practice — though the asymptotic bound has an N-dependent
/// term that Kahan lacks. The tradeoff is order-independence: unlike Kahan,
/// the result is bitwise identical regardless of summation order.
///
/// Note: `error_bound()` uses a tighter formula based on the internal bin
/// scale factor, which can be violated on highly ill-conditioned data
/// (condition number >> 1). The O(N * epsilon * max_abs_val) bound above
/// always holds.
///
/// Subnormal residuals survive when the fold's window includes terminal
/// padding. Promotion can truncate that padding; in particular, float with
/// Fold=2 does not retain every subnormal unit near the minimum normal value.
///
/// Performance: `operator+=` (one-at-a-time) costs roughly 4x a naive FP add
/// due to the serial dependency chain through Fold bin levels. The batch
/// `add(first, last)` method uses multiple independent accumulators for
/// instruction-level parallelism, reducing this to roughly 3x for inputs of
/// 128 elements or more. For smaller inputs, `add()` falls back to the serial
/// path to avoid accumulator merge overhead.
///
/// Cross-type conversion (e.g., between `reproducible_accumulator<float>` and
/// `reproducible_accumulator<double>`) is intentionally not supported. The
/// internal bin structures are incompatible across types, so any conversion
/// must collapse the accumulator to a scalar, discarding the bin state. Use
/// `.value()` to extract the scalar and construct/assign from that explicitly:
///
///   reproducible_accumulator<double> d = acc_float.value();
///
/// @param ftype Floating-point data type; either `float` or `double`
/// @param Fold  The fold; use 3 as a default unless you understand it.
template <class ftype, int Fold = 3>
  requires std::is_floating_point_v<ftype>
class reproducible_accumulator {
  static_assert(Fold >= 2, "Fold must be at least 2");

 private:
  // Renormalization preserves the logical value while redistributing carry
  // bits. Mutable allows const methods (value(), operator==) to lazily flush
  // pending renormalizations without changing the observable result.
  mutable std::array<ftype, Fold> primary = {0};
  mutable std::array<ftype, Fold> carry = {0};
  mutable uint16_t deposit_count_ = 0;
  int16_t max_safe_exp_ = -1;

  /// Floating-point precision bin width
  static constexpr auto kBinWidth = std::is_same_v<ftype, double> ? 40 : 13;
  static constexpr auto kMinExp = std::numeric_limits<ftype>::min_exponent;
  static constexpr auto kMaxExp = std::numeric_limits<ftype>::max_exponent;
  static constexpr auto kMantDig = std::numeric_limits<ftype>::digits;
  /// Binned floating-point maximum index
  static constexpr auto kMaxIndex =
      ((kMaxExp - kMinExp + kMantDig - 1) / kBinWidth) - 1;
  // The maximum floating-point fold supported by the library
  static constexpr auto kMaxFold = kMaxIndex + 1;
  /// Binned floating-point compression factor
  static constexpr auto kCompression = 1.0 / (1 << (kMantDig - kBinWidth + 1));
  /// Binned double precision expansion factor
  static constexpr auto kExpansion = 1.0 * (1 << (kMantDig - kBinWidth + 1));
  static constexpr auto kExpBias = kMaxExp - 2;
  static constexpr auto kEpsilon = std::numeric_limits<ftype>::epsilon();
  /// Binned floating-point deposit endurance
  static constexpr auto kEndurance = 1 << (kMantDig - kBinWidth - 2);

  /// Unsigned integer type matching the width of @p ftype, used for
  /// bit-level reinterpretation of floating-point values.
  using uint_type =
      std::conditional_t<std::is_same_v<ftype, float>, uint32_t, uint64_t>;

  /// Bitmask isolating the exponent field of an IEEE 754 floating-point
  /// value. Used by is_nan_or_inf() to detect special values in a single
  /// bitwise operation.
  static constexpr uint_type kExponentMask =
      static_cast<uint_type>(2 * kMaxExp - 1) << (kMantDig - 1);

  /// Generates binned floating-point reference bins at compile time
  static consteval std::array<ftype, kMaxIndex + kMaxFold> get_bins() {
    std::array<ftype, kMaxIndex + kMaxFold> bins{};

    if (std::is_same_v<ftype, float>) {
      bins[0] = static_cast<ftype>(0.75 * constexpr_pow(double(2), kMaxExp));
    } else {
      bins[0] = static_cast<ftype>(
          2.0 * 0.75 * constexpr_pow(double(2), kMaxExp - 1));
    }

    for (int index = 1; index <= kMaxIndex; index++) {
      int const bin_exponent =
          kMaxExp + kMantDig - kBinWidth + 1 - index * kBinWidth;
      bins[index] =
          static_cast<ftype>(0.75 * constexpr_pow(double(2), bin_exponent));
    }
    for (int index = kMaxIndex + 1; index < kMaxIndex + kMaxFold; index++) {
      // A normal sentinel with the minimum subnormal as its ULP retains the
      // residual below the last regular bin.
      bins[index] =
          static_cast<ftype>(0.75 * constexpr_pow(double(2), kMinExp));
    }

    return bins;
  }

  static constexpr auto kBins = get_bins();

  /// Returns a pointer into the reference bins starting at index @p x.
  static constexpr ftype const* binned_bins(int const x) { return &kBins[x]; }

  /// Reinterprets a floating-point value as its unsigned integer bit pattern.
  static constexpr uint_type get_bits(ftype const x) {
    return std::bit_cast<uint_type>(x);
  }

  /// Sets the lowest bit of the floating-point representation of @p x.
  static constexpr void set_low_bit(ftype& x) {
    x = std::bit_cast<ftype>(get_bits(x) | uint_type{1});
  }

  /// Extracts the carry value encoded in the primary accumulator element @p x.
  static constexpr int extract_carry(ftype const x) {
    return static_cast<int>((get_bits(x) >> (kMantDig - 3)) & 3) - 2;
  }

  /// Resets the primary accumulator element @p x after carry extraction,
  /// restoring its sentinel bit pattern.
  static constexpr ftype reset_primary(ftype const x) {
    auto bits = get_bits(x);
    bits &= ~(uint_type{1} << (kMantDig - 3));
    bits |= uint_type{1} << (kMantDig - 2);
    return std::bit_cast<ftype>(bits);
  }

  /// Returns true if @p x is NaN or infinity via exponent bit check.
  static constexpr bool is_nan_or_inf(ftype const x) {
    return (get_bits(x) & kExponentMask) == kExponentMask;
  }

  /// Extracts the biased exponent from the bit representation of @p x.
  static constexpr int get_exponent(ftype const x) {
    auto const bits = get_bits(x);
    return (bits >> (kMantDig - 1)) & (2 * kMaxExp - 1);
  }

  /// Returns the bin index for a given floating-point value @p x.
  /// Handles subnormals and zero as special cases.
  static int binned_dindex(ftype const x) {
    int exp = get_exponent(x);
    if (exp == 0) {
      if (x == 0.0) {
        return kMaxIndex;
      } else {
        frexp(x, &exp);
        return std::min((kMaxExp - exp) / kBinWidth, kMaxIndex);
      }
    }
    return ((kMaxExp + kExpBias) - exp) / kBinWidth;
  }

  /// Returns the bin index of this accumulator's primary element.
  int binned_index() const { return binned_index_of(primary[0]); }

  /// Returns true if this accumulator is in the highest (index-0) bin.
  bool binned_index0() const {
    return get_exponent(primary[0]) == kMaxExp + kExpBias;
  }

  /// Returns the bin index for a primary accumulator element @p x.
  static int binned_index_of(ftype const x) {
    constexpr int kBin0BiasedExp =
        kMaxExp + kMantDig - kBinWidth + 1 + kExpBias;
    return (kBin0BiasedExp - get_exponent(x)) / kBinWidth;
  }

  /// Updates the cached exponent threshold for fast bin-check skip.
  void update_max_safe_exp(int index) {
    max_safe_exp_ = (kMaxExp + kExpBias) - index * kBinWidth;
  }

  /// Flushes any pending renormalization. Safe to call from const context
  /// because renormalization only redistributes carry bits without changing
  /// the logical value of the accumulator.
  ///
  /// With lazy renormalization, operator+= only renorms every kEndurance
  /// deposits. Between renorms, carry bits remain embedded in the primary
  /// array's mantissa. Depositing more values tolerates this (the algorithm
  /// allows up to kEndurance un-renormed deposits), but any operation that
  /// reads the separated primary/carry representation — conversion to
  /// scalar, equality comparison, merge, or negation — requires them to
  /// be consistent. Those operations must call flush_renorm() first.
  void flush_renorm() const {
    if (deposit_count_ > 0) {
      binned_dmrenorm();
      deposit_count_ = 0;
    }
  }

  /// Updates the bin index of this accumulator to accommodate a value
  /// whose absolute value is @p max_abs_val. Shifts existing bins down
  /// if the new value requires a higher-order bin.
  void binned_dmdupdate(ftype const max_abs_val) {
    if (is_nan_or_inf(primary[0])) {
      return;
    }

    int const X_index = binned_dindex(max_abs_val);
    if (primary[0] == 0.0) {
      ftype const* const bins = binned_bins(X_index);
      for (int i = 0; i < Fold; i++) {
        primary[i] = bins[i];
        carry[i] = 0.0;
      }
      update_max_safe_exp(X_index);
    } else {
      int const shift = binned_index() - X_index;
      if (shift > 0) {
        int i;
        for (i = Fold - 1; i >= shift; i--) {
          primary[i] = primary[i - shift];
          carry[i] = carry[i - shift];
        }
        ftype const* const bins = binned_bins(X_index);
        for (int j = 0; j < i + 1; j++) {
          primary[j] = bins[j];
          carry[j] = 0.0;
        }
        update_max_safe_exp(X_index);
      }
    }
  }

  /// Deposits a single floating-point value @p X into the primary
  /// accumulator bins. The accumulator must already be updated to
  /// accommodate @p X via binned_dmdupdate().
  template <bool HasPadding>
  void binned_dmddeposit_impl(ftype x) {
    ftype m;

    if (is_nan_or_inf(x) || is_nan_or_inf(primary[0])) {
      primary[0] += x;
      return;
    }

    int i = 0;
    if (binned_index0()) {
      m = primary[0];
      ftype qd = x * kCompression;
      set_low_bit(qd);
      qd += m;
      primary[0] = qd;
      m -= qd;
      m *= kExpansion * 0.5;
      x += m;
      x += m;
      i = 1;
    }
    ftype qd;
    for (; i < Fold - 1; i++) {
      m = primary[i];
      qd = x;
      // Only padding has exponent 1, which valid deposits and renormalization
      // preserve. Its exact subnormal grid needs no sticky-bit adjustment.
      if (!HasPadding || get_exponent(m) != 1) {
        set_low_bit(qd);
      }
      qd += m;
      primary[i] = qd;
      m -= qd;
      x += m;
    }
    qd = x;
    if (!HasPadding || get_exponent(primary[i]) != 1) {
      set_low_bit(qd);
    }
    primary[i] += qd;
  }

  bool has_regular_window() const {
    constexpr int normalWindowThreshold =
        (kMaxExp + kExpBias) - (kMaxIndex - Fold + 1) * kBinWidth;
    return max_safe_exp_ >= normalWindowThreshold;
  }

  void binned_dmddeposit(ftype x) {
    if (has_regular_window()) {
      binned_dmddeposit_impl<false>(x);
    } else {
      binned_dmddeposit_impl<true>(x);
    }
  }

  /// Renormalizes the accumulator by extracting carry bits from
  /// primary elements and resetting their sentinel bit patterns.
  /// Must be called periodically to prevent overflow of the carry
  /// encoding within the primary bins.
  void binned_dmrenorm() const {
    if (primary[0] == 0.0 || is_nan_or_inf(primary[0])) {
      return;
    }

    for (int i = 0; i < Fold; i++) {
      carry[i] += extract_carry(primary[i]);
      primary[i] = reset_primary(primary[i]);
    }
  }

  /// Adds a single floating-point value @p X to this accumulator.
  /// Performs update, deposit, and renormalization in sequence.
  template <bool OrdinaryWindow = false>
  void binned_dmdadd(ftype const X) {
    if (get_exponent(X) > max_safe_exp_) {
      binned_dmdupdate(X);
    }
    if constexpr (OrdinaryWindow) {
      binned_dmddeposit_impl<false>(X);
    } else {
      binned_dmddeposit(X);
    }
    if (++deposit_count_ >= kEndurance) {
      binned_dmrenorm();
      deposit_count_ = 0;
    }
  }

  /// Converts the binned accumulator to its native floating-point result.
  /// Intermediate computation is performed in double precision. For float,
  /// this provides sufficient range to avoid overflow. For double, an
  /// additional scaling path (kScaleDown/kScaleUp) is needed when the
  /// accumulator bins are in the high exponent range.
  ftype binned_conv() const {
    flush_renorm();

    if (is_nan_or_inf(primary[0])) {
      return primary[0];
    }

    if (primary[0] == 0.0) {
      return 0.0;
    }

    double y = 0.0;
    int i = 0;
    auto const X_index = binned_index();
    auto const* const bins = binned_bins(X_index);

    // Double precision needs a scaling path to avoid overflow when
    // accumulator bins are in the high exponent range. Float doesn't
    // need this because computing in double already provides sufficient
    // range. All branches within this block return early, so the shared
    // direct accumulation path below is reached only when this block
    // doesn't apply.
    if constexpr (std::is_same_v<ftype, double>) {
      constexpr int kScaleThreshold = (3 * kMantDig) / kBinWidth;
      if (X_index <= kScaleThreshold) {
        int const scaled =
            std::max(std::min(Fold, kScaleThreshold - X_index), 0);
        if (X_index == 0) {
          y += carry[0] * ((bins[0] / 6.0) * kScaleDown * kExpansion);
          y += carry[1] * ((bins[1] / 6.0) * kScaleDown);
          y += (primary[0] - bins[0]) * kScaleDown * kExpansion;
          i = 2;
        } else {
          y += carry[0] * ((bins[0] / 6.0) * kScaleDown);
          i = 1;
        }
        for (; i < scaled; i++) {
          y += carry[i] * ((bins[i] / 6.0) * kScaleDown);
          y += (primary[i - 1] - bins[i - 1]) * kScaleDown;
        }
        if (i == Fold) {
          y += (primary[Fold - 1] - bins[Fold - 1]) * kScaleDown;
          return y * kScaleUp;
        }
        if (std::isinf(y * kScaleUp)) {
          return y * kScaleUp;
        }
        y *= kScaleUp;
        for (; i < Fold; i++) {
          y += carry[i] * (bins[i] / 6.0);
          y += primary[i - 1] - bins[i - 1];
        }
        y += primary[Fold - 1] - bins[Fold - 1];
        return y;
      }
    }

    // Direct accumulation path: used by float always, and by double when
    // X_index is large enough that overflow isn't a concern.
    //
    // The X_index == 0 case handles the highest-order bin where kExpansion
    // is needed. For double, X_index == 0 is always handled by the scaling
    // path above (since 0 <= (3 * kMantDig) / kBinWidth), so this case is
    // only reachable by float.
    if constexpr (std::is_same_v<ftype, float>) {
      if (X_index == 0) {
        y += (double)carry[0] * (double)(bins[0] / 6.0) * (double)kExpansion;
        y += (double)carry[1] * (double)(bins[1] / 6.0);
        y += (double)(primary[0] - bins[0]) * (double)kExpansion;
        i = 2;
      } else {
        y += (double)carry[0] * (double)(bins[0] / 6.0);
        i = 1;
      }
    } else {
      y += carry[0] * (bins[0] / 6.0);
      i = 1;
    }
    // The (double) casts widen float to double for intermediate precision;
    // for double they are no-ops.
    for (; i < Fold; i++) {
      y += (double)carry[i] * (double)(bins[i] / 6.0);
      y += (double)(primary[i - 1] - bins[i - 1]);
    }
    y += (double)(primary[Fold - 1] - bins[Fold - 1]);

    // Narrowing cast for float; no-op for double.
    return static_cast<ftype>(y);
  }

  /// Merges another binned accumulator @p other into this one.
  /// Handles misaligned bin indices by shifting and combining
  /// primary and carry arrays appropriately.
  void binned_dmdmadd(reproducible_accumulator const& other) {
    other.flush_renorm();

    if (other.primary[0] == 0.0) {
      return;
    }

    flush_renorm();

    if (primary[0] == 0.0) {
      for (int i = 0; i < Fold; i++) {
        primary[i] = other.primary[i];
        carry[i] = other.carry[i];
      }
      update_max_safe_exp(binned_index_of(other.primary[0]));
      return;
    }

    if (is_nan_or_inf(other.primary[0]) || is_nan_or_inf(primary[0])) {
      primary[0] += other.primary[0];
      return;
    }

    auto const X_index = binned_index_of(other.primary[0]);
    auto const Y_index = binned_index_of(primary[0]);
    auto const shift = Y_index - X_index;
    if (shift > 0) {
      auto const* const bins_val = binned_bins(Y_index);
      for (int i = Fold - 1; i >= shift; i--) {
        primary[i] =
            other.primary[i] + (primary[i - shift] - bins_val[i - shift]);
        carry[i] = other.carry[i] + carry[i - shift];
      }
      for (int i = 0; i < shift && i < Fold; i++) {
        primary[i] = other.primary[i];
        carry[i] = other.carry[i];
      }
      update_max_safe_exp(X_index);
    } else {
      auto const* const bins_val = binned_bins(X_index);
      for (int i = 0 - shift; i < Fold; i++) {
        primary[i] += other.primary[i + shift] - bins_val[i + shift];
        carry[i] += other.carry[i + shift];
      }
    }

    binned_dmrenorm();
    deposit_count_ = 0;
  }

  static constexpr double kMinVal = 0.5 * constexpr_pow(double(2), kMinExp - 1);
  static constexpr double kBinScale =
      0.5 * constexpr_pow(double(2), (1 - Fold) * kBinWidth + 1);
  static constexpr double kScaleDown =
      0.5 * constexpr_pow(double(2), 1 - (2 * kMantDig - kBinWidth));
  static constexpr double kScaleUp =
      0.5 * constexpr_pow(double(2), 1 + (2 * kMantDig - kBinWidth));

  /// Negates this accumulator in place by reflecting the primary
  /// elements around their corresponding bin values and negating
  /// the carry elements.
  void negate_in_place() {
    flush_renorm();
    if (primary[0] != 0.0) {
      auto const* const bins = binned_bins(binned_index());
      for (int i = 0; i < Fold; i++) {
        primary[i] = bins[i] - (primary[i] - bins[i]);
        carry[i] = -carry[i];
      }
    }
  }

 public:
  /// Default-constructs a zero-valued accumulator.
  reproducible_accumulator() = default;

  /// Copy/move constructors and assignment. Cross-type conversion is
  /// intentionally omitted: the bin structures differ across ftype, so
  /// conversion must go through .value() to make the lossy collapse explicit.
  reproducible_accumulator(reproducible_accumulator const&) = default;
  reproducible_accumulator(reproducible_accumulator&&) = default;
  reproducible_accumulator& operator=(reproducible_accumulator const&) =
      default;
  reproducible_accumulator& operator=(reproducible_accumulator&&) = default;
  ~reproducible_accumulator() = default;

  /// Construct from an arithmetic scalar value
  /// NOTE: Casts @p x to the type of the binned fp
  template <typename U>
    requires std::is_arithmetic_v<U>
  /* implicit */ reproducible_accumulator(U const x) {
    binned_dmdadd(static_cast<ftype>(x));
  }

  /// Sets this binned fp equal to the arithmetic value @p x
  /// NOTE: Casts @p x to the type of the binned fp
  template <typename U>
    requires std::is_arithmetic_v<U>
  reproducible_accumulator& operator=(U const x) {
    zero();
    binned_dmdadd(static_cast<ftype>(x));
    return *this;
  }

  /// Set the binned fp to zero
  void zero() {
    primary = {0};
    carry = {0};
    deposit_count_ = 0;
    max_safe_exp_ = -1;
  }

  /// Accumulate an arithmetic @p x into the binned fp.
  /// NOTE: Casts @p x to the type of the binned fp
  template <typename U>
    requires std::is_arithmetic_v<U>
  reproducible_accumulator& operator+=(U const x) {
    binned_dmdadd(static_cast<ftype>(x));
    return *this;
  }

  /// Accumulate-subtract an arithmetic @p x into the binned fp.
  /// NOTE: Casts @p x to the type of the binned fp
  template <typename U>
    requires std::is_arithmetic_v<U>
  reproducible_accumulator& operator-=(U const x) {
    binned_dmdadd(-static_cast<ftype>(x));
    return *this;
  }

  /// Accumulate a binned fp @p x into the binned fp.
  reproducible_accumulator& operator+=(reproducible_accumulator const& other) {
    binned_dmdmadd(other);
    return *this;
  }

  /// Accumulate-subtract a binned fp @p x into the binned fp.
  reproducible_accumulator& operator-=(reproducible_accumulator other) {
    other.negate_in_place();
    binned_dmdmadd(other);
    return *this;
  }

  /// Determines if two binned fp are equal
  bool operator==(reproducible_accumulator const& other) const {
    flush_renorm();
    other.flush_renorm();
    return primary == other.primary && carry == other.carry;
  }

  /// Returns the sum of two accumulators.
  friend reproducible_accumulator operator+(
      reproducible_accumulator lhs, reproducible_accumulator const& rhs) {
    lhs += rhs;
    return lhs;
  }

  /// Returns the difference of two accumulators.
  friend reproducible_accumulator operator-(
      reproducible_accumulator lhs, reproducible_accumulator const& rhs) {
    lhs -= rhs;
    return lhs;
  }

  /// Returns the negative of this binned fp
  reproducible_accumulator operator-() const {
    reproducible_accumulator temp = *this;
    temp.negate_in_place();
    return temp;
  }

  /// Convert this binned fp into its native floating-point representation
  ftype value() const { return binned_conv(); }

  /// Explicitly convert this binned fp to its native floating-point type
  explicit operator ftype() const { return value(); }

  ///@brief Get binned fp summation error bound
  static constexpr ftype error_bound(
      uint64_t const n, ftype const max_abs_val, ftype const binned_sum) {
    double const x = std::abs(max_abs_val);
    double const s = std::abs(binned_sum);
    double const magnitude_error = std::max(x, kMinVal) * kBinScale * n;
    double const sqrt_eps = std::sqrt(static_cast<double>(kEpsilon));
    double const rounding_coeff =
        (7.0 * kEpsilon) / (1.0 - 6.0 * sqrt_eps - 7.0 * kEpsilon);
    double const sum_error = rounding_coeff * s;
    return static_cast<ftype>(magnitude_error + sum_error);
  }

  /// Add @p x to the binned fp
  void add(ftype const x) { binned_dmdadd(x); }

  /// Add arithmetics in the range [first, last) to the binned fp.
  /// For random-access iterators, uses multiple independent accumulators
  /// to exploit instruction-level parallelism. The [[gnu::flatten]]
  /// attribute forces full inlining so the compiler and OOO engine can
  /// see across all deposits simultaneously.
  template <typename InputIt>
  [[gnu::flatten]] void add(InputIt first, InputIt last) {
    if constexpr (std::random_access_iterator<InputIt>) {
      constexpr int K = 4;
      constexpr int kILPThreshold = 128;
      using diff_t = std::iter_difference_t<InputIt>;
      auto const n = last - first;
      if (n < kILPThreshold) {
        for (diff_t i = 0; i < n; ++i) {
          binned_dmdadd(static_cast<ftype>(first[i]));
        }
        return;
      }

      std::array<reproducible_accumulator, K - 1> accs;
      auto const full = n - (n % K);
      diff_t i = 0;
      for (; i < full; i += K) {
        // Bin updates only promote a window. Once every lane has no padding,
        // the remaining groups need no per-deposit padding checks.
        if (has_regular_window() && accs[0].has_regular_window() &&
            accs[1].has_regular_window() && accs[2].has_regular_window()) {
          break;
        }
        binned_dmdadd(static_cast<ftype>(first[i]));
        accs[0].binned_dmdadd(static_cast<ftype>(first[i + 1]));
        accs[1].binned_dmdadd(static_cast<ftype>(first[i + 2]));
        accs[2].binned_dmdadd(static_cast<ftype>(first[i + 3]));
      }
      for (; i < full; i += K) {
        binned_dmdadd<true>(static_cast<ftype>(first[i]));
        accs[0].template binned_dmdadd<true>(static_cast<ftype>(first[i + 1]));
        accs[1].template binned_dmdadd<true>(static_cast<ftype>(first[i + 2]));
        accs[2].template binned_dmdadd<true>(static_cast<ftype>(first[i + 3]));
      }

      for (; i < n; ++i) {
        binned_dmdadd(static_cast<ftype>(first[i]));
      }

      for (int k = 0; k < K - 1; ++k) {
        *this += accs[k];
      }
    } else {
      for (; first != last; ++first) {
        binned_dmdadd(static_cast<ftype>(*first));
      }
    }
  }

  /// Add elements from a range to the binned fp.
  template <std::ranges::input_range R>
    requires std::is_arithmetic_v<std::ranges::range_value_t<R>>
  void add(R&& range) {
    add(std::ranges::begin(range), std::ranges::end(range));
  }

  //////////////////////////////////////
  // MANUAL OPERATIONS; USE WISELY
  //////////////////////////////////////

  /// Updates the accumulator's bin structure to accommodate values up to
  /// @p mav in absolute value. Must be called before unsafe_add() when the
  /// maximum absolute value of subsequent deposits is known in advance.
  template <typename T>
    requires std::is_arithmetic_v<T>
  void set_max_abs_val(T const mav) {
    flush_renorm();
    binned_dmdupdate(std::abs(mav));
  }

  /// Deposits @p x into the accumulator without updating bins or
  /// renormalizing. The caller must ensure set_max_abs_val() has been
  /// called and renorm() is called every endurance() deposits.
  void unsafe_add(ftype const x) { binned_dmddeposit(x); }

  /// Renormalizes the accumulator. Must be called at least every
  /// endurance() deposits when using unsafe_add().
  void renorm() {
    binned_dmrenorm();
    deposit_count_ = 0;
  }

  /// Returns the maximum number of unsafe_add() deposits allowed
  /// between renorm() calls.
  static constexpr size_t endurance() { return kEndurance; }
};

} // namespace folly

template <class ftype, int Fold>
struct fmt::formatter<folly::reproducible_accumulator<ftype, Fold>>
    : fmt::formatter<ftype> {
  template <typename FormatContext>
  auto format(
      folly::reproducible_accumulator<ftype, Fold> const& rfa,
      FormatContext& ctx) const {
    return fmt::formatter<ftype>::format(rfa.value(), ctx);
  }
};
