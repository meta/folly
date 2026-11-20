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

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <folly/CPortability.h>
#include <folly/Traits.h>
#include <folly/lang/Exception.h>
#include <folly/stats/detail/Bucket.h>

namespace folly {

namespace detail {

/*
 * A helper class to manage a set of histogram buckets.
 */
template <typename T, typename BucketT>
class HistogramBuckets {
 public:
  using ValueType = T;
  using BucketType = BucketT;

  /*
   * Create a set of histogram buckets.
   *
   * One bucket will be created for each bucketSize interval of values within
   * the specified range.  Additionally, one bucket will be created to track
   * all values that fall below the specified minimum, and one bucket will be
   * created for all values above the specified maximum.
   *
   * If (max - min) is not a multiple of bucketSize, the last bucket will cover
   * a smaller range of values than the other buckets.
   *
   * (max - min) must be larger than or equal to bucketSize.
   */
  HistogramBuckets(
      ValueType bucketSize,
      ValueType min,
      ValueType max,
      const BucketType& defaultBucket);

  /* Returns the bucket size of each bucket in the histogram. */
  ValueType getBucketSize() const { return bucketSize_; }

  /* Returns the min value at which bucketing begins. */
  ValueType getMin() const { return min_; }

  /* Returns the max value at which bucketing ends. */
  ValueType getMax() const { return max_; }

  /*
   * Returns the number of buckets.
   *
   * This includes the total number of buckets for the [min, max) range,
   * plus 2 extra buckets, one for handling values less than min, and one for
   * values greater than max.
   */
  size_t getNumBuckets() const { return buckets_.size(); }

  /* Returns the bucket index into which the given value would fall. */
  size_t getBucketIdx(ValueType value) const;

  /* Returns the bucket for the specified value */
  BucketType& getByValue(ValueType value) {
    return buckets_[getBucketIdx(value)];
  }

  /* Returns the bucket for the specified value */
  const BucketType& getByValue(ValueType value) const {
    return buckets_[getBucketIdx(value)];
  }

  /*
   * Returns the bucket at the specified index.
   *
   * Note that index 0 is the bucket for all values less than the specified
   * minimum.  Index 1 is the first bucket in the specified bucket range.
   */
  BucketType& getByIndex(size_t idx) { return buckets_[idx]; }

  /* Returns the bucket at the specified index. */
  const BucketType& getByIndex(size_t idx) const { return buckets_[idx]; }

  /*
   * Returns the minimum threshold for the bucket at the given index.
   *
   * The bucket at the specified index will store values in the range
   * [bucketMin, bucketMin + bucketSize), or [bucketMin, max), if the overall
   * max is smaller than bucketMin + bucketSize.
   */
  ValueType getBucketMin(size_t idx) const {
    if (idx == 0) {
      return std::numeric_limits<ValueType>::min();
    }
    if (idx == buckets_.size() - 1) {
      return max_;
    }

    return ValueType(min_ + ((idx - 1) * bucketSize_));
  }

  /*
   * Returns the maximum threshold for the bucket at the given index.
   *
   * The bucket at the specified index will store values in the range
   * [bucketMin, bucketMin + bucketSize), or [bucketMin, max), if the overall
   * max is smaller than bucketMin + bucketSize.
   */
  ValueType getBucketMax(size_t idx) const {
    if (idx == buckets_.size() - 1) {
      return std::numeric_limits<ValueType>::max();
    }

    return ValueType(min_ + (idx * bucketSize_));
  }

  /**
   * Computes the total number of values stored across all buckets.
   *
   * Runs in O(numBuckets)
   *
   * @param countFn A function that takes a const BucketType&, and returns the
   *                number of values in that bucket
   * @return Returns the total number of values stored across all buckets
   */
  template <typename CountFn>
  uint64_t computeTotalCount(CountFn countFromBucket) const;

  /**
   * Determine which bucket the specified percentile falls into.
   *
   * Looks for the bucket that contains the Nth percentile data point.
   *
   * @param pct     The desired percentile to find, as a value from 0.0 to 1.0.
   * @param countFn A function that takes a const BucketType&, and returns the
   *                number of values in that bucket.
   * @param lowPct  The lowest percentile stored in the selected bucket will be
   *                returned via this parameter.
   * @param highPct The highest percentile stored in the selected bucket will
   *                be returned via this parameter.
   *
   * @return Returns the index of the bucket that contains the Nth percentile
   *         data point.
   *
   * @throws std::invalid_argument if pct is outside [0.0, 1.0], including NaN.
   */
  template <typename CountFn>
  size_t getPercentileBucketIdx(
      double pct,
      CountFn countFromBucket,
      double* lowPct = nullptr,
      double* highPct = nullptr) const;

  /**
   * Estimate the value at the specified percentile.
   *
   * @param pct     The desired percentile to find, as a value from 0.0 to 1.0.
   * @param countFn A function that takes a const BucketType&, and returns the
   *                number of values in that bucket.
   * @param avgFn   A function that takes a const BucketType&, and returns the
   *                average of all the values in that bucket.
   *
   * @return Returns an estimate for N, where N is the number where exactly pct
   *         percentage of the data points in the histogram are less than N.
   *
   * @throws std::invalid_argument if pct is outside [0.0, 1.0], including NaN.
   */
  template <typename CountFn, typename AvgFn>
  ValueType getPercentileEstimate(
      double pct, CountFn countFromBucket, AvgFn avgFromBucket) const;

  /*
   * Iterator access to the buckets.
   *
   * Note that the first bucket is for all values less than min, and the last
   * bucket is for all values greater than max.  The buckets tracking values in
   * the [min, max) actually start at the second bucket.
   */
  typename std::vector<BucketType>::const_iterator begin() const {
    return buckets_.begin();
  }
  typename std::vector<BucketType>::iterator begin() {
    return buckets_.begin();
  }
  typename std::vector<BucketType>::const_iterator end() const {
    return buckets_.end();
  }
  typename std::vector<BucketType>::iterator end() { return buckets_.end(); }

 private:
  static constexpr bool kIsExact = std::numeric_limits<ValueType>::is_exact;
  ValueType bucketSize_;
  ValueType min_;
  ValueType max_;
  std::vector<BucketType> buckets_;
};

} // namespace detail

/*
 * A basic histogram class.
 *
 * Groups data points into equally-sized buckets, and stores the overall sum of
 * the data points in each bucket, as well as the number of data points in the
 * bucket.
 *
 * The caller must specify the minimum and maximum data points to expect ahead
 * of time, as well as the bucket width.
 */
template <typename T>
class Histogram {
 public:
  using ValueType = T;
  using Bucket = detail::Bucket<T>;

  Histogram(ValueType bucketSize, ValueType min, ValueType max)
      : buckets_(bucketSize, min, max, Bucket()) {}

  /* Add a data point to the histogram */
  void addValue(ValueType value) {
    Bucket& bucket = buckets_.getByValue(value);
    addToAccum(bucket.sum, value);
    bucket.count += 1;
  }

  /* Add multiple same data points to the histogram */
  void addRepeatedValue(ValueType value, uint64_t nSamples) {
    Bucket& bucket = buckets_.getByValue(value);
    // Saturating arithmetic: clamps at the type's limits instead of
    // overflowing, and runs in O(1) even for huge nSamples.
    detail::repeatedValueHelper(bucket.sum, value, nSamples);
    bucket.count += nSamples;
  }

  /*
   * Remove a data point to the histogram
   *
   * Note that this method does not actually verify that this exact data point
   * had previously been added to the histogram; it merely subtracts the
   * requested value from the appropriate bucket's sum.
   */
  void removeValue(ValueType value) {
    Bucket& bucket = buckets_.getByValue(value);
    if (bucket.count > 0) {
      subFromAccum(bucket.sum, value);
      bucket.count -= 1;
    } else {
      bucket.sum = ValueType();
      bucket.count = 0;
    }
  }

  /* Remove multiple same data points from the histogram */
  void removeRepeatedValue(ValueType value, uint64_t nSamples) {
    Bucket& bucket = buckets_.getByValue(value);
    if (bucket.count >= nSamples) {
      // Saturating arithmetic: clamps at the type's limits instead of
      // overflowing, and runs in O(1) even for huge nSamples.
      detail::subtractRepeatedHelper(bucket.sum, value, nSamples);
      bucket.count -= nSamples;
    } else {
      bucket.sum = ValueType();
      bucket.count = 0;
    }
  }

  /* Remove all data points from the histogram */
  void clear() {
    for (size_t i = 0; i < buckets_.getNumBuckets(); i++) {
      buckets_.getByIndex(i).clear();
    }
  }

  /* Subtract another histogram data from the histogram */
  void subtract(const Histogram& hist) {
    // the two histogram bucket definitions must match to support
    // subtract.
    if (getBucketSize() != hist.getBucketSize() || getMin() != hist.getMin() ||
        getMax() != hist.getMax() || getNumBuckets() != hist.getNumBuckets()) {
      throw_exception<std::invalid_argument>(
          "Cannot subtract input histogram.");
    }

    for (size_t i = 0; i < buckets_.getNumBuckets(); i++) {
      buckets_.getByIndex(i) -= hist.buckets_.getByIndex(i);
    }
  }

  /* Merge two histogram data together */
  void merge(const Histogram& hist) {
    // the two histogram bucket definitions must match to support
    // a merge.
    if (getBucketSize() != hist.getBucketSize() || getMin() != hist.getMin() ||
        getMax() != hist.getMax() || getNumBuckets() != hist.getNumBuckets()) {
      throw_exception<std::invalid_argument>(
          "Cannot merge from input histogram.");
    }

    for (size_t i = 0; i < buckets_.getNumBuckets(); i++) {
      buckets_.getByIndex(i) += hist.buckets_.getByIndex(i);
    }
  }

  /* Copy bucket values from another histogram */
  void copy(const Histogram& hist) {
    // the two histogram bucket definition must match
    if (getBucketSize() != hist.getBucketSize() || getMin() != hist.getMin() ||
        getMax() != hist.getMax() || getNumBuckets() != hist.getNumBuckets()) {
      throw_exception<std::invalid_argument>(
          "Cannot copy from input histogram.");
    }

    for (size_t i = 0; i < buckets_.getNumBuckets(); i++) {
      buckets_.getByIndex(i) = hist.buckets_.getByIndex(i);
    }
  }

  /* Returns the bucket size of each bucket in the histogram. */
  ValueType getBucketSize() const { return buckets_.getBucketSize(); }
  /* Returns the min value at which bucketing begins. */
  ValueType getMin() const { return buckets_.getMin(); }
  /* Returns the max value at which bucketing ends. */
  ValueType getMax() const { return buckets_.getMax(); }
  /* Returns the number of buckets */
  size_t getNumBuckets() const { return buckets_.getNumBuckets(); }

  /* Returns the specified bucket (for reading only!) */
  const Bucket& getBucketByIndex(size_t idx) const {
    return buckets_.getByIndex(idx);
  }

  /*
   * Returns the minimum threshold for the bucket at the given index.
   *
   * The bucket at the specified index will store values in the range
   * [bucketMin, bucketMin + bucketSize), or [bucketMin, max), if the overall
   * max is smaller than bucketMin + bucketSize.
   */
  ValueType getBucketMin(size_t idx) const {
    return buckets_.getBucketMin(idx);
  }

  /*
   * Returns the maximum threshold for the bucket at the given index.
   *
   * The bucket at the specified index will store values in the range
   * [bucketMin, bucketMin + bucketSize), or [bucketMin, max), if the overall
   * max is smaller than bucketMin + bucketSize.
   */
  ValueType getBucketMax(size_t idx) const {
    return buckets_.getBucketMax(idx);
  }

  /**
   * Computes the total number of values stored across all buckets.
   *
   * Runs in O(numBuckets)
   */
  uint64_t computeTotalCount() const {
    CountFromBucket countFn;
    return buckets_.computeTotalCount(countFn);
  }

  /*
   * Get the bucket that the specified percentile falls into
   *
   * The lowest and highest percentile data points in returned bucket will be
   * returned in the lowPct and highPct arguments, if they are not nullptr.
   *
   * @throws std::invalid_argument if pct is outside [0.0, 1.0], including NaN.
   */
  size_t getPercentileBucketIdx(
      double pct, double* lowPct = nullptr, double* highPct = nullptr) const {
    // We unfortunately can't use lambdas here yet;
    // Some users of this code are still built with gcc-4.4.
    CountFromBucket countFn;
    return buckets_.getPercentileBucketIdx(pct, countFn, lowPct, highPct);
  }

  /**
   * Estimate the value at the specified percentile.
   *
   * @param pct     The desired percentile to find, as a value from 0.0 to 1.0.
   *
   * @return Returns an estimate for N, where N is the number where exactly pct
   *         percentage of the data points in the histogram are less than N.
   *
   * @throws std::invalid_argument if pct is outside [0.0, 1.0], including NaN.
   */
  ValueType getPercentileEstimate(double pct) const {
    CountFromBucket countFn;
    AvgFromBucket avgFn;
    return buckets_.getPercentileEstimate(pct, countFn, avgFn);
  }

  /*
   * Get a human-readable string describing the histogram contents
   */
  std::string debugString() const;

  /*
   * Write the histogram contents in tab-separated values (TSV) format.
   * Format is "min max count sum".
   */
  void toTSV(std::ostream& out, bool skipEmptyBuckets = true) const;

  struct CountFromBucket {
    uint64_t operator()(const Bucket& bucket) const { return bucket.count; }
  };
  struct AvgFromBucket {
    ValueType operator()(const Bucket& bucket) const {
      if (bucket.count == 0) {
        return ValueType(0);
      }
      // Cast bucket.count to a signed integer type.  This ensures that we
      // perform division properly here: If bucket.sum is a signed integer
      // type but we divide by an unsigned number, unsigned division will be
      // performed and bucket.sum will be converted to unsigned first.
      // If bucket.sum is unsigned, the code will still do unsigned division
      // correctly.
      //
      // The only downside is if bucket.count is large enough to be negative
      // when treated as signed.  That should be extremely unlikely, though.
      return bucket.sum / static_cast<int64_t>(bucket.count);
    }
  };

 private:
  template <typename S>
  static constexpr auto to_unsigned(S s) {
    if constexpr (std::is_integral_v<S>) {
      return static_cast<std::make_unsigned_t<S>>(s);
    } else {
      return s;
    }
  }

  /// Add value to accumulator, clamping to numeric limits on overflow.
  /// Unlike raw arithmetic, this never invokes signed integer overflow UB.
  static constexpr void addToAccum(ValueType& a, ValueType value) {
    detail::addHelper(a, value);
  }

  /// Subtract value from accumulator, clamping to zero on underflow.
  /// Unlike raw arithmetic, this never invokes signed integer underflow UB.
  static constexpr void subFromAccum(ValueType& a, ValueType value) {
    detail::subtractHelper(a, value);
  }

  detail::HistogramBuckets<ValueType, Bucket> buckets_;
};

} // namespace folly

#include <folly/stats/Histogram-inl.h>
