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

#include <folly/random/UniformRealDistribution.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <sstream>
#include <type_traits>

#include <folly/portability/GTest.h>

namespace folly {
namespace {

template <typename UInt>
struct ConstantRNG {
  using result_type = UInt;

  UInt value = 0;
  unsigned calls = 0;

  UInt operator()() {
    ++calls;
    return value;
  }
  static constexpr UInt min() { return 0; }
  static constexpr UInt max() { return std::numeric_limits<UInt>::max(); }
};

struct SequenceRNG {
  using result_type = uint32_t;

  std::array<uint32_t, 4> values{0x80000000, 0, 0, 0};
  unsigned calls = 0;

  uint32_t operator()() { return values.at(calls++); }
  static constexpr uint32_t min() { return 0; }
  static constexpr uint32_t max() { return UINT32_MAX; }
};

template <typename Real>
class UniformRealDistributionTest : public testing::Test {};

using RealTypes = testing::Types<float, double, long double>;
TYPED_TEST_SUITE(UniformRealDistributionTest, RealTypes);

TYPED_TEST(UniformRealDistributionTest, Parameters) {
  using Dist = uniform_real_distribution<TypeParam>;
  using Param = typename Dist::param_type;
  static_assert(std::is_same_v<typename Dist::result_type, TypeParam>);
  static_assert(std::is_same_v<typename Param::distribution_type, Dist>);

  Dist dist;
  EXPECT_EQ(dist.a(), 0);
  EXPECT_EQ(dist.b(), 1);
  EXPECT_EQ(dist.min(), 0);
  EXPECT_EQ(dist.max(), 1);
  EXPECT_EQ(dist.param(), Param());

  Param param(-5, 17);
  dist.param(param);
  EXPECT_EQ(dist, Dist(param));
  EXPECT_EQ(dist, Dist(-5, 17));
  EXPECT_NE(dist, Dist());
  EXPECT_EQ(dist.min(), -5);
  EXPECT_EQ(dist.max(), 17);
  dist.reset();
  EXPECT_EQ(dist.param(), param);

  ConstantRNG<uint32_t> rng;
  EXPECT_EQ(dist(rng, Param(2, 3)), 2);
  EXPECT_EQ(dist.param(), param);
}

TYPED_TEST(UniformRealDistributionTest, FullWidthEngines) {
  using Real = TypeParam;
  uniform_real_distribution<Real> dist;
  ConstantRNG<uint32_t> rng32;
  EXPECT_EQ(dist(rng32), 0);
  rng32.value = rng32.max();
  EXPECT_EQ(dist(rng32), std::nextafter(Real{1}, Real{0}));
  EXPECT_EQ(rng32.calls, 2 * ((std::numeric_limits<Real>::digits + 31) / 32));

  ConstantRNG<uint64_t> rng64;
  EXPECT_EQ(dist(rng64), 0);
  rng64.value = rng64.max();
  EXPECT_EQ(dist(rng64), std::nextafter(Real{1}, Real{0}));
  EXPECT_EQ(rng64.calls, 2 * ((std::numeric_limits<Real>::digits + 63) / 64));
}

TYPED_TEST(UniformRealDistributionTest, WordOrderAndPrecision) {
  using Real = TypeParam;
  uniform_real_distribution<Real> dist;
  SequenceRNG rng;
  EXPECT_EQ(dist(rng), Real{0.5});
  EXPECT_EQ(rng.calls, (std::numeric_limits<Real>::digits + 31) / 32);

  SequenceRNG low;
  constexpr int digits = std::numeric_limits<Real>::digits;
  constexpr int words = (digits + 31) / 32;
  low.values.fill(0);
  low.values[words - 1] = uint32_t{1} << ((32 - digits % 32) % 32);
  EXPECT_EQ(dist(low), std::ldexp(Real{1}, -digits));
}

TYPED_TEST(UniformRealDistributionTest, ExcludesUpperEndpoint) {
  using Real = TypeParam;
  ConstantRNG<uint32_t> rng{UINT32_MAX};
  EXPECT_EQ(
      uniform_real_distribution<Real>(1, 2)(rng),
      std::nextafter(Real{2}, Real{1}));
  EXPECT_EQ(
      uniform_real_distribution<Real>(-2, -1)(rng),
      std::nextafter(Real{-1}, Real{-2}));
  auto adjacent = std::nextafter(Real{1}, Real{2});
  EXPECT_EQ(uniform_real_distribution<Real>(1, adjacent)(rng), 1);
  if constexpr (std::numeric_limits<Real>::has_denorm == std::denorm_present) {
    EXPECT_EQ(
        uniform_real_distribution<Real>(
            0, std::numeric_limits<Real>::denorm_min())(rng),
        0);
  }
}

TYPED_TEST(UniformRealDistributionTest, ExtremeFiniteBounds) {
  using Real = TypeParam;
  auto max = std::numeric_limits<Real>::max() / 2;
  uniform_real_distribution<Real> dist(-max, max);
  ConstantRNG<uint32_t> zero;
  EXPECT_EQ(dist(zero), -max);
  SequenceRNG half;
  EXPECT_EQ(dist(half), 0);
  ConstantRNG<uint32_t> high{UINT32_MAX};
  auto value = dist(high);
  EXPECT_TRUE(std::isfinite(value));
  EXPECT_GT(value, 0);
  EXPECT_LT(value, max);
}

TYPED_TEST(UniformRealDistributionTest, EqualBoundsDoNotConsumeEngine) {
  ConstantRNG<uint32_t> rng;
  uniform_real_distribution<TypeParam> dist(3.5, 3.5);
  EXPECT_EQ(dist(rng), TypeParam{3.5});
  EXPECT_EQ(rng.calls, 0);
}

template <typename Real, typename RNG>
void checkEngine(RNG rng) {
  uniform_real_distribution<Real> dist(-5, 17);
  std::array<unsigned, 16> buckets{};
  long double sum = 0;
  for (unsigned i = 0; i < 4096; ++i) {
    auto value = dist(rng);
    ASSERT_GE(value, -5);
    ASSERT_LT(value, 17);
    ++buckets.at(static_cast<size_t>((value + 5) / 22 * buckets.size()));
    sum += value;
  }
  EXPECT_NEAR(sum / 4096, 6, 0.4);
  for (auto count : buckets) {
    EXPECT_GT(count, 128);
    EXPECT_LT(count, 384);
  }
}

TYPED_TEST(UniformRealDistributionTest, ArbitraryEngineRanges) {
  checkEngine<TypeParam>(std::mt19937(123));
  checkEngine<TypeParam>(std::mt19937_64(123));
  checkEngine<TypeParam>(std::minstd_rand(123));
  checkEngine<TypeParam>(std::ranlux24_base(123));
  checkEngine<TypeParam>(
      std::independent_bits_engine<std::mt19937, 8, uint8_t>(123));
}

TYPED_TEST(UniformRealDistributionTest, StreamRoundTrip) {
  using Real = TypeParam;
  uniform_real_distribution<Real> dist(
      std::nextafter(Real{1}, Real{2}), std::numeric_limits<Real>::max());
  std::stringstream stream;
  stream.setf(std::ios_base::fixed, std::ios_base::floatfield);
  stream.precision(3);
  auto flags = stream.flags();
  stream << dist;
  EXPECT_EQ(stream.flags(), flags);
  EXPECT_EQ(stream.precision(), 3);
  uniform_real_distribution<Real> restored;
  stream >> restored;
  ASSERT_FALSE(stream.fail());
  EXPECT_EQ(restored, dist);

  std::wstringstream wide;
  wide << dist;
  wide >> restored;
  ASSERT_FALSE(wide.fail());
  EXPECT_EQ(restored, dist);
}

TYPED_TEST(UniformRealDistributionTest, InvalidStreamPreservesParameters) {
  uniform_real_distribution<TypeParam> original(-5, 17);
  for (auto text : {"3 2", "bad input", "2"}) {
    auto dist = original;
    std::stringstream stream(text);
    stream >> dist;
    EXPECT_TRUE(stream.fail());
    EXPECT_EQ(dist, original);
  }
  auto max = std::numeric_limits<TypeParam>::max();
  std::stringstream stream;
  stream.precision(std::numeric_limits<TypeParam>::max_digits10);
  stream << -max << ' ' << max;
  auto dist = original;
  stream >> dist;
  EXPECT_TRUE(stream.fail());
  EXPECT_EQ(dist, original);
}

#ifndef NDEBUG
TYPED_TEST(UniformRealDistributionTest, RejectsInfiniteSpan) {
  auto max = std::numeric_limits<TypeParam>::max();
  EXPECT_DEATH((uniform_real_distribution<TypeParam>(-max, max)), "isfinite");
}
#endif

TEST(UniformRealDistributionTest, DoesNotCopyEngine) {
  struct NonCopyableRNG : ConstantRNG<uint32_t> {
    NonCopyableRNG() = default;
    NonCopyableRNG(const NonCopyableRNG&) = delete;
    NonCopyableRNG& operator=(const NonCopyableRNG&) = delete;
  } rng;
  uniform_real_distribution<> dist;
  EXPECT_EQ(dist(rng), 0);
  EXPECT_EQ(rng.calls, 2);
}

} // namespace
} // namespace folly
