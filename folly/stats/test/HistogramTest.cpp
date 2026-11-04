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

#include <folly/stats/Histogram.h>

#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <type_traits>

#include <folly/portability/GTest.h>
#include <folly/stats/detail/Bucket.h>

using folly::Histogram;

// Insert 100 evenly distributed values into a histogram with 100 buckets
TEST(Histogram, Test100) {
  Histogram<int64_t> h(1, 0, 100);

  for (unsigned int n = 0; n < 100; ++n) {
    h.addValue(n);
  }

  // 100 buckets, plus 1 for below min, and 1 for above max
  EXPECT_EQ(h.getNumBuckets(), 102);

  double epsilon = 1e-6;
  for (unsigned int n = 0; n <= 100; ++n) {
    double pct = n / 100.0;

    // Floating point arithmetic isn't 100% accurate, and if we just divide
    // (n / 100) the value should be exactly on a bucket boundary.  Add espilon
    // to ensure we fall in the upper bucket.
    if (n < 100) {
      double lowPct = -1.0;
      double highPct = -1.0;
      unsigned int bucketIdx =
          h.getPercentileBucketIdx(pct + epsilon, &lowPct, &highPct);
      EXPECT_EQ(n + 1, bucketIdx);
      EXPECT_FLOAT_EQ(n / 100.0, lowPct);
      EXPECT_FLOAT_EQ((n + 1) / 100.0, highPct);
    }

    // Also test n - epsilon, to test falling in the lower bucket.
    if (n > 0) {
      double lowPct = -1.0;
      double highPct = -1.0;
      unsigned int bucketIdx =
          h.getPercentileBucketIdx(pct - epsilon, &lowPct, &highPct);
      EXPECT_EQ(n, bucketIdx);
      EXPECT_FLOAT_EQ((n - 1) / 100.0, lowPct);
      EXPECT_FLOAT_EQ(n / 100.0, highPct);
    }

    // Check getPercentileEstimate()
    EXPECT_EQ(n, h.getPercentileEstimate(pct));
  }
}

// Test calling getPercentileBucketIdx() and getPercentileEstimate() on an
// empty histogram
TEST(Histogram, TestEmpty) {
  Histogram<int64_t> h(1, 0, 100);

  for (unsigned int n = 0; n <= 100; ++n) {
    double pct = n / 100.0;

    double lowPct = -1.0;
    double highPct = -1.0;
    unsigned int bucketIdx = h.getPercentileBucketIdx(pct, &lowPct, &highPct);
    EXPECT_EQ(1, bucketIdx);
    EXPECT_FLOAT_EQ(0.0, lowPct);
    EXPECT_FLOAT_EQ(0.0, highPct);

    EXPECT_EQ(0, h.getPercentileEstimate(pct));
  }
}

// Test calling getPercentileBucketIdx() and getPercentileEstimate() on a
// histogram with just a single value.
TEST(Histogram, Test1) {
  Histogram<int64_t> h(1, 0, 100);
  h.addValue(42);

  for (unsigned int n = 0; n < 100; ++n) {
    double pct = n / 100.0;

    double lowPct = -1.0;
    double highPct = -1.0;
    unsigned int bucketIdx = h.getPercentileBucketIdx(pct, &lowPct, &highPct);
    EXPECT_EQ(43, bucketIdx);
    EXPECT_FLOAT_EQ(0.0, lowPct);
    EXPECT_FLOAT_EQ(1.0, highPct);

    auto p = h.getPercentileEstimate(pct);
    EXPECT_TRUE(p == 42 || p == 43);
  }
}

// Test adding enough numbers to make the sum value overflow in the
// "below min" bucket
TEST(Histogram, TestOverflowMin) {
  Histogram<int64_t> h(1, 0, 100);

  for (unsigned int n = 0; n < 9; ++n) {
    h.addValue(-0x0fffffffffffffff);
  }

  EXPECT_EQ(uint64_t(9), h.getBucketByIndex(0).count);

  // The clamped sum saturates at the most negative value instead of wrapping
  // through zero, so the bucket average stays negative and the estimator
  // interpolates a genuine below-min value. Unlike the old wrapped-sum
  // behavior, the result is no longer the overflow sentinel.
  int64_t estimate = h.getPercentileEstimate(0.05);
  EXPECT_LT(estimate, int64_t(0));
  EXPECT_GT(estimate, std::numeric_limits<int64_t>::min());
}

// Test adding enough numbers to make the sum value overflow in the
// "above max" bucket
TEST(Histogram, TestOverflowMax) {
  Histogram<int64_t> h(1, 0, 100);

  for (unsigned int n = 0; n < 9; ++n) {
    h.addValue(0x0fffffffffffffff);
  }

  EXPECT_EQ(uint64_t(9), h.getBucketByIndex(h.getNumBuckets() - 1).count);

  // Same as TestOverflowMin, mirrored: the saturated average stays positive
  // and the estimator interpolates a genuine above-max value that is finite
  // instead of the overflow sentinel.
  int64_t estimate = h.getPercentileEstimate(0.95);
  EXPECT_GT(estimate, int64_t(0));
  EXPECT_LT(estimate, std::numeric_limits<int64_t>::max());
}

// Test adding enough numbers to make the sum value overflow in one of the
// normal buckets
TEST(Histogram, TestOverflowBucket) {
  // Use a very narrow histogram range so the value exceeds max.
  Histogram<int64_t> h(0x0100000000000000, 0, 0x1000000000000000);

  // 0x0fffffffffffffff < 0x1000000000000000, so these go into a regular
  // bucket, not the overflow bucket.  The clamped sum saturates at the most
  // positive value, the estimator detects an average outside the bucket
  // range, and falls back to the bucket midpoint exactly as before.
  for (unsigned int n = 0; n < 9; ++n) {
    h.addValue(0x0fffffffffffffff);
  }

  int64_t estimate = h.getPercentileEstimate(0.95);
  EXPECT_EQ(0x0f80000000000000, estimate);
}

TEST(Histogram, TestDouble) {
  // Insert 100 evenly spaced values into a histogram
  Histogram<double> h(100.0, 0.0, 5000.0);
  for (double n = 50; n < 5000; n += 100) {
    h.addValue(n);
  }
  EXPECT_EQ(52, h.getNumBuckets());
  EXPECT_EQ(2500.0, h.getPercentileEstimate(0.5));
  EXPECT_EQ(4500.0, h.getPercentileEstimate(0.9));
}

// Test where the bucket width is not an even multiple of the histogram range
TEST(Histogram, TestDoubleInexactWidth) {
  Histogram<double> h(100.0, 0.0, 4970.0);
  for (double n = 50; n < 5000; n += 100) {
    h.addValue(n);
  }
  EXPECT_EQ(52, h.getNumBuckets());
  EXPECT_EQ(2500.0, h.getPercentileEstimate(0.5));
  EXPECT_EQ(4500.0, h.getPercentileEstimate(0.9));

  EXPECT_EQ(0, h.getBucketByIndex(51).count);
  h.addValue(4990);
  h.addValue(5100);
  EXPECT_EQ(2, h.getBucketByIndex(51).count);
  EXPECT_EQ(2600.0, h.getPercentileEstimate(0.5));
}

// Test where the bucket width is larger than the histogram range
// (There isn't really much point to defining a histogram this way,
// but we want to ensure that it still works just in case.)
TEST(Histogram, TestDoubleWidthTooBig) {
  Histogram<double> h(100.0, 0.0, 7.0);
  EXPECT_EQ(3, h.getNumBuckets());

  for (double n = 0; n < 7; n += 1) {
    h.addValue(n);
  }
  EXPECT_EQ(0, h.getBucketByIndex(0).count);
  EXPECT_EQ(7, h.getBucketByIndex(1).count);
  EXPECT_EQ(0, h.getBucketByIndex(2).count);
  EXPECT_EQ(3.0, h.getPercentileEstimate(0.5));

  h.addValue(-1.0);
  EXPECT_EQ(1, h.getBucketByIndex(0).count);
  h.addValue(7.5);
  EXPECT_EQ(1, h.getBucketByIndex(2).count);
  EXPECT_NEAR(3.0, h.getPercentileEstimate(0.5), 1e-14);
}

// Test that we get counts right
TEST(Histogram, Counts) {
  Histogram<int32_t> h(1, 0, 10);
  EXPECT_EQ(12, h.getNumBuckets());
  EXPECT_EQ(0, h.computeTotalCount());

  // Add one to each bucket, make sure the counts match
  for (int32_t i = 0; i < 10; i++) {
    h.addValue(i);
    EXPECT_EQ(i + 1, h.computeTotalCount());
  }

  // Add a lot to one bucket, make sure the counts still make sense
  for (int32_t i = 0; i < 100; i++) {
    h.addValue(0);
  }
  EXPECT_EQ(110, h.computeTotalCount());
}

TEST(Histogram, ConstructWithNonPositiveBucketSizeThrows) {
  EXPECT_THROW((Histogram<int64_t>(0, 0, 100)), std::invalid_argument);
  EXPECT_THROW((Histogram<int64_t>(-1, 0, 100)), std::invalid_argument);
}

TEST(Histogram, ConstructWithNaNBucketSizeThrows) {
  EXPECT_THROW(
      (Histogram<double>(std::nan(""), 0.0, 100.0)), std::invalid_argument);
}

TEST(Histogram, ConstructWithMinNotLessThanMaxThrows) {
  EXPECT_THROW((Histogram<int64_t>(1, 100, 100)), std::invalid_argument);
  EXPECT_THROW((Histogram<int64_t>(1, 100, 0)), std::invalid_argument);
}

TEST(Histogram, ConstructWithNaNMinThrows) {
  EXPECT_THROW(
      (Histogram<double>(1.0, std::nan(""), 100.0)), std::invalid_argument);
}

TEST(Histogram, GetPercentileEstimateWithOutOfRangePctThrows) {
  Histogram<int64_t> h(1, 0, 100);
  h.addValue(50);
  EXPECT_THROW(h.getPercentileEstimate(-0.01), std::invalid_argument);
  EXPECT_THROW(h.getPercentileEstimate(1.01), std::invalid_argument);
}

TEST(Histogram, GetPercentileEstimateWithNaNPctThrows) {
  Histogram<double> h(1.0, 0.0, 100.0);
  h.addValue(50.0);
  EXPECT_THROW(h.getPercentileEstimate(std::nan("")), std::invalid_argument);
}

// Test that addValue clamps the bucket sum on overflow instead of
// invoking undefined behavior.
TEST(Histogram, AddValueClampsOnOverflow) {
  Histogram<int64_t> h(1, 0, 100);

  // Add INT64_MAX once — no overflow.
  h.addValue(std::numeric_limits<int64_t>::max());
  EXPECT_EQ(
      std::numeric_limits<int64_t>::max(), h.getBucketByIndex(101).sum);
  EXPECT_EQ(uint64_t(1), h.getBucketByIndex(101).count);

  // Add INT64_MAX again — overflow should be clamped, not wrapped.
  h.addValue(std::numeric_limits<int64_t>::max());
  EXPECT_EQ(
      std::numeric_limits<int64_t>::max(), h.getBucketByIndex(101).sum);
  EXPECT_EQ(uint64_t(2), h.getBucketByIndex(101).count);
}

// Test that removeValue clamps on underflow instead of wrapping.
TEST(Histogram, RemoveValueClampsOnUnderflow) {
  Histogram<int64_t> h(1, 0, 100);

  // Add a small positive value.
  h.addValue(10);
  // Value 10 falls in the bucket [10, 11), which is bucket index 11.
  EXPECT_EQ(int64_t(10), h.getBucketByIndex(11).sum);

  // Remove the same value — underflow should be clamped to zero.
  h.removeValue(10);
  EXPECT_EQ(int64_t(0), h.getBucketByIndex(11).sum);
  EXPECT_EQ(uint64_t(0), h.getBucketByIndex(11).count);
}

// A single add of the most negative value saturates the below-min bucket sum
// and exercises the percentile estimator's extrapolation with that average.
TEST(Histogram, BelowMinBucketClampedSumEstimate) {
  Histogram<int64_t> h(1, 0, 100);
  h.addValue(std::numeric_limits<int64_t>::min());
  EXPECT_EQ(
      std::numeric_limits<int64_t>::min(), h.getBucketByIndex(0).sum);
  EXPECT_EQ(
      std::numeric_limits<int64_t>::min(), h.getPercentileEstimate(0.05));
}

// The mirror case for the above-max bucket.
TEST(Histogram, AboveMaxBucketClampedSumEstimate) {
  Histogram<int64_t> h(1, 0, 100);
  h.addValue(std::numeric_limits<int64_t>::max());
  EXPECT_EQ(
      std::numeric_limits<int64_t>::max(),
      h.getBucketByIndex(h.getNumBuckets() - 1).sum);
  // The saturated average is exactly the most positive value, so the
  // estimator returns it directly without wrapping into negative territory.
  int64_t estimate = h.getPercentileEstimate(0.95);
  EXPECT_EQ(std::numeric_limits<int64_t>::max(), estimate);
}

// addRepeatedValue with the maximum possible count must terminate instantly
// and saturate rather than wrap, for both signs.
TEST(Histogram, AddRepeatedValueHugeCountSaturates) {
  Histogram<int64_t> h(1, 0, 100);

  // Value 5 lands in the bucket covering [5, 6), which is index 6.
  h.addRepeatedValue(5, ~uint64_t(0));
  EXPECT_EQ(std::numeric_limits<int64_t>::max(), h.getBucketByIndex(6).sum);
  EXPECT_EQ(~uint64_t(0), h.getBucketByIndex(6).count);

  // -5 lands in the below-min bucket (index 0) and saturates its sum there.
  h.addRepeatedValue(-5, ~uint64_t(0));
  EXPECT_EQ(std::numeric_limits<int64_t>::min(), h.getBucketByIndex(0).sum);

  // Removing the maximum possible count drives the sum all the way down to
  // the opposite limit: max - 5*(2^64 - 1) is far below min.
  h.removeRepeatedValue(5, ~uint64_t(0));
  EXPECT_EQ(std::numeric_limits<int64_t>::min(), h.getBucketByIndex(6).sum);
  EXPECT_EQ(uint64_t(0), h.getBucketByIndex(6).count);
}

// Repeated adds and removes of an extreme count land exactly on the limits,
// matching what sequential clamped addition would produce.
TEST(Histogram, AddRepeatedValueExtremeEquivalence) {
  Histogram<int64_t> h(1, 0, 100);

  // Adding (2^64 - 1) ones to a fresh bucket climbs to exactly max.
  h.addRepeatedValue(1, ~uint64_t(0));
  EXPECT_EQ(std::numeric_limits<int64_t>::max(), h.getBucketByIndex(2).sum);

  // Subtracting (2^64 - 1) ones from there lands on exactly min.
  h.removeRepeatedValue(1, ~uint64_t(0));
  EXPECT_EQ(std::numeric_limits<int64_t>::min(), h.getBucketByIndex(2).sum);
}

// Unsigned sums saturate at the unsigned limits.
TEST(Histogram, UnsignedRepeatedValueSaturates) {
  Histogram<uint64_t> h(1, 0, 100);

  h.addValue(std::numeric_limits<uint64_t>::max());
  h.addRepeatedValue(std::numeric_limits<uint64_t>::max(), 5);
  EXPECT_EQ(
      std::numeric_limits<uint64_t>::max(),
      h.getBucketByIndex(h.getNumBuckets() - 1).sum);
  EXPECT_EQ(uint64_t(6), h.getBucketByIndex(h.getNumBuckets() - 1).count);

  h.removeRepeatedValue(std::numeric_limits<uint64_t>::max(), 7);
  EXPECT_EQ(uint64_t(0), h.getBucketByIndex(h.getNumBuckets() - 1).sum);
  EXPECT_EQ(uint64_t(0), h.getBucketByIndex(h.getNumBuckets() - 1).count);
}

// Test that removeRepeatedValue handles overflow-safe subtraction.
TEST(Histogram, RemoveRepeatedValueSafe) {
  Histogram<int64_t> h(1, 0, 100);

  // Add several values.
  h.addRepeatedValue(10, 5);
  EXPECT_EQ(int64_t(50), h.getBucketByIndex(11).sum);
  EXPECT_EQ(uint64_t(5), h.getBucketByIndex(11).count);

  // Remove some — should work normally.
  h.removeRepeatedValue(10, 3);
  EXPECT_EQ(int64_t(20), h.getBucketByIndex(11).sum);
  EXPECT_EQ(uint64_t(2), h.getBucketByIndex(11).count);

  // Remove more than exists — count should go to 0.
  h.removeRepeatedValue(10, 5);
  EXPECT_EQ(int64_t(0), h.getBucketByIndex(11).sum);
  EXPECT_EQ(uint64_t(0), h.getBucketByIndex(11).count);
}

// Floating-point histograms have no overflow concern (sums relax to +/-inf),
// but they must still compile and behave like the pre-clamping arithmetic.
TEST(Histogram, FloatingPointRepeatedValue) {
  Histogram<double> h(1.0, 0.0, 100.0);

  h.addRepeatedValue(2.5, 4);
  EXPECT_EQ(10.0, h.getBucketByIndex(3).sum);
  EXPECT_EQ(uint64_t(4), h.getBucketByIndex(3).count);

  h.removeRepeatedValue(2.5, 2);
  EXPECT_EQ(5.0, h.getBucketByIndex(3).sum);
  EXPECT_EQ(uint64_t(2), h.getBucketByIndex(3).count);

  // Removing more than was added resets the bucket.
  h.removeRepeatedValue(2.5, 99);
  EXPECT_EQ(0.0, h.getBucketByIndex(3).sum);
  EXPECT_EQ(uint64_t(0), h.getBucketByIndex(3).count);
}

namespace {

// Compares the O(1) repeated helpers against two independent references on
// random inputs: a naive loop of clamped single-value operations, and exact
// 128-bit arithmetic. Small types are cross-checked exhaustively over their
// whole domain.
template <typename T>
void checkRepeatedAgainstReference(uint64_t iterations, uint64_t maxN) {
  static_assert(std::is_integral<T>::value, "integral types only");
  std::mt19937_64 rng(0xFACE + sizeof(T));

  auto checkOne = [](T a, T v, uint64_t n) {
    T gotAdd = a;
    folly::detail::repeatedValueHelper(gotAdd, v, n);
    T gotSub = a;
    folly::detail::subtractRepeatedHelper(gotSub, v, n);

    // Ground truth in 128-bit space, for both operations. Signed types fit in
    // signed __int128 (|v| * n <= 2^63 * (2^64-1) < 2^127), but unsigned
    // products only fit in unsigned __int128, so the two cases are kept
    // separate.
    T wantAdd;
    T wantSub;
    if constexpr (std::is_signed<T>::value) {
      const __int128 addTotal =
          static_cast<__int128>(a) + static_cast<__int128>(v) * static_cast<__int128>(n);
      const __int128 subTotal =
          static_cast<__int128>(a) - static_cast<__int128>(v) * static_cast<__int128>(n);
      wantAdd =
          addTotal > std::numeric_limits<T>::max()
          ? std::numeric_limits<T>::max()
          : (addTotal < std::numeric_limits<T>::min()
                 ? std::numeric_limits<T>::min()
                 : static_cast<T>(addTotal));
      wantSub =
          subTotal > std::numeric_limits<T>::max()
          ? std::numeric_limits<T>::max()
          : (subTotal < std::numeric_limits<T>::min()
                 ? std::numeric_limits<T>::min()
                 : static_cast<T>(subTotal));
    } else {
      const unsigned __int128 addTotal =
          static_cast<unsigned __int128>(a) +
          static_cast<unsigned __int128>(v) * n;
      const unsigned __int128 prod =
          static_cast<unsigned __int128>(v) * n;
      wantAdd = addTotal > std::numeric_limits<T>::max()
          ? std::numeric_limits<T>::max()
          : static_cast<T>(addTotal);
      wantSub = prod >= static_cast<unsigned __int128>(a)
          ? T(0)
          : static_cast<T>(a - static_cast<T>(prod));
    }

    ASSERT_EQ(wantAdd, gotAdd) << "a=" << +a << " v=" << +v << " n=" << n;
    ASSERT_EQ(wantSub, gotSub) << "a=" << +a << " v=" << +v << " n=" << n;

    // The clamped result must also match a literal loop of clamped
    // single-value operations, which is what the O(1) helpers replace.
    if (n <= 1000) {
      T loopAdd = a;
      T loopSub = a;
      for (uint64_t i = 0; i < n; i++) {
        folly::detail::addHelper(loopAdd, v);
        folly::detail::subtractHelper(loopSub, v);
      }
      ASSERT_EQ(loopAdd, gotAdd) << "a=" << +a << " v=" << +v << " n=" << n;
      ASSERT_EQ(loopSub, gotSub) << "a=" << +a << " v=" << +v << " n=" << n;
    }
  };

  // Adversarial cases around the limits: ±1 steps at the extremes, huge
  // sample counts, zero value, zero count, and min/max accumulators.
  const T tmax = std::numeric_limits<T>::max();
  const T tmin = std::numeric_limits<T>::min();
  const uint64_t huge = std::numeric_limits<uint64_t>::max();
  const T vs[] = {T(0), T(1), T(tmax), tmin, static_cast<T>(tmax - 1),
      static_cast<T>(tmin + 1)};
  const T as[] = {T(0), T(1), tmax, tmin};
  for (T a : as) {
    for (T v : vs) {
      for (uint64_t n : {uint64_t(0), uint64_t(1), uint64_t(2), uint64_t(3),
               uint64_t(1000), uint64_t(1001), huge, huge - 1, huge / 2}) {
        checkOne(a, v, n);
      }
    }
  }

  // Random cases.
  if (sizeof(T) == 1) {
    // Small types: exhaustive over (accumulator, value) pairs.
    for (int64_t ai = std::numeric_limits<T>::min();
         ai <= std::numeric_limits<T>::max();
         ai++) {
      for (int64_t vi = std::numeric_limits<T>::min();
           vi <= std::numeric_limits<T>::max();
           vi++) {
        for (uint64_t n : {uint64_t(0), uint64_t(1), uint64_t(2), uint64_t(3),
                 uint64_t(255), uint64_t(256), uint64_t(1000)}) {
          checkOne(static_cast<T>(ai), static_cast<T>(vi), n);
        }
      }
    }
    return;
  }
  // A 64-bit unsigned ValueType spans a range no signed distribution can
  // cover, so pick the distribution type accordingly.
  using DistT =
      std::conditional_t<std::is_unsigned<T>::value && sizeof(T) == 8,
          uint64_t,
          int64_t>;
  std::uniform_int_distribution<DistT> dist(
      std::numeric_limits<T>::min(), std::numeric_limits<T>::max());
  for (uint64_t i = 0; i < iterations; i++) {
    const T a = static_cast<T>(dist(rng));
    const T v = static_cast<T>(dist(rng));
    const uint64_t n = rng() % (maxN + 1);
    checkOne(a, v, n);
    // Also test near-limit n values with random a and v.
    checkOne(a, v, huge);
  }
}

} // namespace

// The O(1) repeated-value helpers must behave exactly like sequential clamped
// operations and like exact wide arithmetic, for every input type Histogram
// supports.
TEST(Histogram, RepeatedValueMatchesClampedLoop) {
  checkRepeatedAgainstReference<int8_t>(0, 0);
  checkRepeatedAgainstReference<uint8_t>(0, 0);
  checkRepeatedAgainstReference<int16_t>(100000, 1000);
  checkRepeatedAgainstReference<uint16_t>(100000, 1000);
  checkRepeatedAgainstReference<int32_t>(50000, 1000000);
  checkRepeatedAgainstReference<uint32_t>(50000, 1000000);
  checkRepeatedAgainstReference<int64_t>(50000, 1000000);
  checkRepeatedAgainstReference<uint64_t>(50000, 1000000);
}
