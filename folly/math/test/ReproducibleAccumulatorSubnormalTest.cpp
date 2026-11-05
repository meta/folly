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

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include <folly/math/ReproducibleAccumulator.h>
#include <folly/portability/GTest.h>

namespace {

template <typename Accumulator>
class ReproducibleAccumulatorSubnormalTest : public ::testing::Test {};

using Accumulators = ::testing::Types<
    folly::reproducible_accumulator<float, 2>,
    folly::reproducible_accumulator<float>,
    folly::reproducible_accumulator<float, 4>,
    folly::reproducible_accumulator<float, 21>,
    folly::reproducible_accumulator<double, 2>,
    folly::reproducible_accumulator<double>,
    folly::reproducible_accumulator<double, 4>,
    folly::reproducible_accumulator<double, 52>>;
TYPED_TEST_SUITE(ReproducibleAccumulatorSubnormalTest, Accumulators);

TYPED_TEST(ReproducibleAccumulatorSubnormalTest, KeepsSignedAndEvenUnits) {
  using Accumulator = TypeParam;
  using T = decltype(Accumulator{}.value());
  const auto unit = std::numeric_limits<T>::denorm_min();
  for (int units : {-1024, -3, -2, -1, 0, 1, 2, 3, 1024}) {
    Accumulator sum;
    sum += T(units) * unit;
    EXPECT_EQ(T(units) * unit, sum.value());
  }
  Accumulator empty;
  empty += T(0);
  empty += T(-0.0);
  EXPECT_EQ(T(0), empty.value());
  EXPECT_FALSE(std::signbit(empty.value()));
}

TYPED_TEST(ReproducibleAccumulatorSubnormalTest, CrossesTheNormalBoundary) {
  using Accumulator = TypeParam;
  using T = decltype(Accumulator{}.value());
  const auto unit = std::numeric_limits<T>::denorm_min();
  const auto minimum = std::numeric_limits<T>::min();
  for (T value :
       {minimum - T(2) * unit,
        minimum - unit,
        minimum,
        minimum + unit,
        minimum + T(2) * unit}) {
    Accumulator sum;
    sum += value;
    if constexpr (std::is_same_v<
                      Accumulator,
                      folly::reproducible_accumulator<float, 2>>) {
      // This window ends before padding; its finite precision still bounds
      // the result rather than retaining every unit at this boundary.
      EXPECT_LE(
          std::abs(sum.value() - value),
          Accumulator::error_bound(1, value, sum.value()));
    } else {
      EXPECT_EQ(value, sum.value());
      sum += -value;
      sum += unit;
      EXPECT_EQ(unit, sum.value());
    }
  }
}

TYPED_TEST(
    ReproducibleAccumulatorSubnormalTest, RenormalizesWithoutLosingUnits) {
  using Accumulator = TypeParam;
  using T = decltype(Accumulator{}.value());
  const auto unit = std::numeric_limits<T>::denorm_min();
  Accumulator safe;
  Accumulator manual;
  manual.set_max_abs_val(T(3) * unit);
  constexpr int kTerms{65539};
  for (int i = 0; i < kTerms; ++i) {
    safe += T(3) * unit;
    manual.unsafe_add(T(3) * unit);
    if ((i + 1) % Accumulator::endurance() == 0) {
      manual.renorm();
    }
  }
  manual.renorm();
  EXPECT_EQ(T(3 * kTerms) * unit, safe.value());
  EXPECT_EQ(safe.value(), manual.value());
  auto promoted = safe;
  promoted += -T(3 * kTerms) * unit;
  if constexpr (std::is_same_v<
                    Accumulator,
                    folly::reproducible_accumulator<float, 2>>) {
    EXPECT_LE(
        std::abs(promoted.value()),
        Accumulator::error_bound(
            kTerms + 1, T(3 * kTerms) * unit, promoted.value()));
  } else {
    EXPECT_EQ(T(0), promoted.value());
  }
  for (int i = 0; i < kTerms; ++i) {
    safe += -T(3) * unit;
  }
  EXPECT_EQ(T(0), safe.value());
}

TYPED_TEST(ReproducibleAccumulatorSubnormalTest, MatchesPermutedMergeTrees) {
  using Accumulator = TypeParam;
  using T = decltype(Accumulator{}.value());
  const auto unit = std::numeric_limits<T>::denorm_min();
  std::vector<T> values;
  int units{0};
  for (int i = 0; i < 10003; ++i) {
    const int coefficient = i % 7 - 2;
    units += coefficient;
    values.push_back(T(coefficient) * unit);
  }
  std::mt19937 random(469);
  for (size_t partitions : {1, 2, 7, 37}) {
    for (int permutation = 0; permutation < 4; ++permutation) {
      std::shuffle(values.begin(), values.end(), random);
      std::vector<Accumulator> partials(partitions);
      for (size_t i = 0; i < values.size(); ++i) {
        partials[i % partitions] += values[i];
      }
      while (partials.size() > 1) {
        auto rhs = partials.back();
        partials.pop_back();
        partials.back() += rhs;
      }
      EXPECT_EQ(T(units) * unit, partials.front().value());
      Accumulator batch;
      batch.add(values);
      EXPECT_EQ(T(units) * unit, batch.value());
    }
  }
}

TYPED_TEST(
    ReproducibleAccumulatorSubnormalTest, CrossesRegularHalfUlpBoundaries) {
  using Accumulator = TypeParam;
  using T = decltype(Accumulator{}.value());
  constexpr int kHalfUlpUnits = std::is_same_v<T, double> ? (1 << 18) : 16;
  const auto unit = std::numeric_limits<T>::denorm_min();
  for (int sign : {-1, 1}) {
    for (int offset : {-1, 0, 1}) {
      const auto value = T(sign * (kHalfUlpUnits + offset)) * unit;
      Accumulator sum;
      sum += value;
      EXPECT_EQ(value, sum.value());
      sum += -value;
      EXPECT_EQ(T(0), sum.value());
    }
  }
}

TYPED_TEST(
    ReproducibleAccumulatorSubnormalTest, RetainsResidualsThroughPromotion) {
  using Accumulator = TypeParam;
  using T = decltype(Accumulator{}.value());
  const auto unit = std::numeric_limits<T>::denorm_min();
  const auto large = std::ldexp(
      std::numeric_limits<T>::min(), std::is_same_v<T, double> ? 7 : 0);
  Accumulator small;
  small += unit;
  Accumulator positive;
  positive += large;
  Accumulator negative;
  negative += -large;
  auto ascending = (small + positive) + negative;
  auto descending = (positive + small) + negative;
  auto balanced = small + (positive + negative);
  Accumulator sequential;
  sequential += unit;
  sequential += large;
  sequential += -large;
  if constexpr (
      std::is_same_v<Accumulator, folly::reproducible_accumulator<float, 2>> ||
      std::is_same_v<Accumulator, folly::reproducible_accumulator<double, 2>>) {
    EXPECT_EQ(sequential.value(), ascending.value());
    EXPECT_EQ(sequential.value(), descending.value());
    EXPECT_EQ(sequential.value(), balanced.value());
    EXPECT_LE(
        std::abs(sequential.value() - unit),
        Accumulator::error_bound(3, large, sequential.value()));
  } else {
    EXPECT_EQ(unit, sequential.value());
    EXPECT_EQ(unit, ascending.value());
    EXPECT_EQ(unit, descending.value());
    EXPECT_EQ(unit, balanced.value());
  }
}

TYPED_TEST(
    ReproducibleAccumulatorSubnormalTest, BatchTransitionsFromTinyWindows) {
  using Accumulator = TypeParam;
  using T = decltype(Accumulator{}.value());
  const auto unit = std::numeric_limits<T>::denorm_min();
  for (size_t prefix : {0, 1, 4, 127, 128, 257, 2048}) {
    const size_t suffix = 2 * Accumulator::endurance() + 259;
    std::vector<T> values(prefix + suffix, T(1));
    for (size_t i = 0; i < prefix; ++i) {
      values[i] = i % 2 == 0 ? T(0) : unit;
    }
    Accumulator batch;
    batch.add(values);
    EXPECT_EQ(T(suffix), batch.value());
    Accumulator sequential;
    for (const auto value : values) {
      sequential += value;
    }
    EXPECT_EQ(sequential.value(), batch.value());
  }
}

TYPED_TEST(ReproducibleAccumulatorSubnormalTest, BatchZeroLengthsAndSpecials) {
  using Accumulator = TypeParam;
  using T = decltype(Accumulator{}.value());
  for (size_t length : {0, 1, 4, 127, 128, 129, 257, 8195}) {
    std::vector<T> values(length, T(-0.0));
    Accumulator batch;
    batch.add(values);
    EXPECT_EQ(T(0), batch.value());
    EXPECT_FALSE(std::signbit(batch.value()));
  }
  std::vector<T> values(257, T(1));
  values[0] = std::numeric_limits<T>::denorm_min();
  values[1] = std::numeric_limits<T>::infinity();
  values[2] = -std::numeric_limits<T>::infinity();
  values[3] = std::numeric_limits<T>::quiet_NaN();
  Accumulator batch;
  batch.add(values);
  EXPECT_TRUE(std::isnan(batch.value()));
}

TYPED_TEST(
    ReproducibleAccumulatorSubnormalTest, BatchPromotesAndCancelsLargeTerms) {
  using Accumulator = TypeParam;
  using T = decltype(Accumulator{}.value());
  const auto unit = std::numeric_limits<T>::denorm_min();
  const auto large = std::ldexp(T(1), std::is_same_v<T, double> ? 60 : 20);
  std::vector<T> values(257, unit);
  for (size_t i = 0; i < 2049; ++i) {
    values.insert(values.end(), {large, T(1), -large});
  }
  Accumulator batch;
  batch.add(values);
  EXPECT_EQ(T(2049), batch.value());
  Accumulator sequential;
  for (const auto value : values) {
    sequential += value;
  }
  EXPECT_EQ(sequential.value(), batch.value());
}

TYPED_TEST(ReproducibleAccumulatorSubnormalTest, CarriesRepeatedSelfMerges) {
  using Accumulator = TypeParam;
  using T = decltype(Accumulator{}.value());
  const auto unit = std::numeric_limits<T>::denorm_min();
  constexpr int kDoublings = std::is_same_v<T, double> ? 50 : 21;
  Accumulator sum;
  sum += unit;
  for (int i = 0; i < kDoublings; ++i) {
    auto copy = sum;
    sum += copy;
    EXPECT_EQ(std::ldexp(unit, i + 1), sum.value());
  }
  auto opposite = -sum;
  sum += opposite;
  sum += unit;
  EXPECT_EQ(unit, sum.value());
}

TYPED_TEST(ReproducibleAccumulatorSubnormalTest, ShiftsTheTerminalResidual) {
  using Accumulator = TypeParam;
  using T = decltype(Accumulator{}.value());
  const auto unit = std::numeric_limits<T>::denorm_min();
  const auto minimum = std::numeric_limits<T>::min();
  for (int exponent = std::numeric_limits<T>::min_exponent;
       exponent < std::numeric_limits<T>::min_exponent + 80;
       exponent += 7) {
    const auto value = std::ldexp(T(1), exponent);
    Accumulator sequential;
    sequential += unit;
    sequential += value;
    Accumulator small;
    small += unit;
    Accumulator large;
    large += value;
    auto ascending = small + large;
    auto descending = large + small;
    EXPECT_EQ(sequential.value(), ascending.value());
    EXPECT_EQ(sequential.value(), descending.value());
    EXPECT_LE(
        std::abs(sequential.value() - value),
        Accumulator::error_bound(2, value, sequential.value()));
  }
  if constexpr (!std::is_same_v<
                    Accumulator,
                    folly::reproducible_accumulator<float, 2>>) {
    Accumulator cancellation;
    cancellation += minimum;
    cancellation += -minimum;
    cancellation += unit;
    EXPECT_EQ(unit, cancellation.value());
  }
}

} // namespace
