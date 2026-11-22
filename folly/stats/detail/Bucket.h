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

#include <chrono>
#include <concepts>
#include <cstdint>
#include <type_traits>

#include <folly/ConstexprMath.h>

namespace folly {
namespace detail {

/*
 * Helper function to compute the average, given a specified input type and
 * return type.
 */

// If the input is long double, divide using long double to avoid losing
// precision.
//
// If the ReturnType is integral, the result might be clamped to avoid overflow.
template <typename ReturnType>
ReturnType avgHelper(long double sum, uint64_t count) {
  if (count == 0) {
    return ReturnType(0);
  }
  const long double countf = count;
  if constexpr (std::integral<ReturnType>) {
    return constexpr_clamp_cast<ReturnType>(sum / countf);
  }
  return static_cast<ReturnType>(sum / countf);
}

// In all other cases divide using double precision.
// This should be relatively fast, and accurate enough for most use cases.
//
// If the ReturnType is integral, the result might be clamped to avoid overflow.
template <typename ReturnType, typename ValueType>
  requires(!std::same_as<ValueType, long double>)
ReturnType avgHelper(ValueType sum, uint64_t count) {
  if (count == 0) {
    return ReturnType(0);
  }
  const double sumf = double(sum);
  const double countf = double(count);
  if constexpr (std::integral<ReturnType>) {
    return constexpr_clamp_cast<ReturnType>(sumf / countf);
  }
  return static_cast<ReturnType>(sumf / countf);
}

// Helpers to add bucket counts and values without
// ever causing undefined behavior
//
// For non-integral vlaues everyhing is easy
template <typename ValueType>
  requires(!std::integral<ValueType>)
void addHelper(ValueType& a, const ValueType& b) {
  a += b;
}

template <typename ValueType>
  requires(!std::integral<ValueType>)
void subtractHelper(ValueType& a, const ValueType& b) {
  a -= b;
}

// For integral values we use folly/ConstexprMath.h to make
// the math safe and predictable
template <typename ValueType>
  requires(std::integral<ValueType>)
void addHelper(ValueType& a, const ValueType& b) {
  a = constexpr_add_overflow_clamped(a, b);
}

template <typename ValueType>
  requires(std::integral<ValueType>)
void subtractHelper(ValueType& a, const ValueType& b) {
  a = constexpr_sub_overflow_clamped(a, b);
}

/*
 * Helper function to compute the rate per Interval,
 * given the specified count recorded over the elapsed time period.
 */
template <
    typename ReturnType = double,
    typename Duration = std::chrono::seconds,
    typename Interval = Duration>
  requires std::constructible_from<Duration, Interval>
ReturnType rateHelper(ReturnType count, Duration elapsed) {
  if (elapsed == Duration(0)) {
    return 0;
  }

  // elapsed is non-zero, and we increase elapsed to at least Interval{1}
  // for rate calculation to smooth out rate calculation at the beginning for
  // a timeseries' lifecycle, when elapsed is very short compared to the overall
  // timeseries duration.
  elapsed = std::max(elapsed, Duration(Interval{1}));

  // Use std::chrono::duration_cast to convert between the native
  // duration and the desired interval.  However, convert the rates,
  // rather than just converting the elapsed duration.  Converting the
  // elapsed time first may collapse it down to 0 if the elapsed interval
  // is less than the desired interval, which will incorrectly result in
  // an infinite rate.
  using NativeRate = std::chrono::duration<
      double,
      std::ratio<Duration::period::den, Duration::period::num>>;
  using DesiredRate = std::chrono::duration<
      double,
      std::ratio<Interval::period::den, Interval::period::num>>;

  // We use Rep=double to avoid rouding down too much here.
  NativeRate native((double)count / elapsed.count());
  DesiredRate desired = std::chrono::duration_cast<DesiredRate>(native);
  return static_cast<ReturnType>(desired.count());
}

template <typename T>
struct Bucket {
 public:
  using ValueType = T;

  Bucket() : sum(ValueType()), count(0) {}

  void clear() {
    sum = ValueType();
    count = 0;
  }

  void add(const ValueType& s, uint64_t c) {
    addHelper(sum, s);
    addHelper(count, c);
  }

  Bucket& operator+=(const Bucket& o) {
    add(o.sum, o.count);
    return *this;
  }

  Bucket& operator-=(const Bucket& o) {
    subtractHelper(sum, o.sum);
    subtractHelper(count, o.count);
    return *this;
  }

  template <typename ReturnType>
  ReturnType avg() const {
    return avgHelper<ReturnType>(sum, count);
  }

  ValueType sum;
  uint64_t count;
};
} // namespace detail
} // namespace folly
