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

#include <cstdint>
#include <stdexcept>

#include <folly/lang/Exception.h>

namespace folly {

enum class ordering : std::int8_t { lt = -1, eq = 0, gt = 1 };

template <typename T>
constexpr ordering to_ordering(T c) {
  return ordering(int(c < T(0)) * -1 + int(c > T(0)));
}

namespace detail {

template <typename C, ordering o, bool ne>
struct cmp_pred : private C {
  using C::C;

  template <typename A, typename B>
  constexpr bool operator()(A&& a, B&& b) const {
    return ne ^ (C::operator()(static_cast<A&&>(a), static_cast<B&&>(b)) == o);
  }
};

} // namespace detail

template <typename C>
struct compare_equal_to : detail::cmp_pred<C, ordering::eq, 0> {
  using detail::cmp_pred<C, ordering::eq, 0>::cmp_pred;
};

template <typename C>
struct compare_not_equal_to : detail::cmp_pred<C, ordering::eq, 1> {
  using detail::cmp_pred<C, ordering::eq, 1>::cmp_pred;
};

template <typename C>
struct compare_less : detail::cmp_pred<C, ordering::lt, 0> {
  using detail::cmp_pred<C, ordering::lt, 0>::cmp_pred;
};

template <typename C>
struct compare_less_equal : detail::cmp_pred<C, ordering::gt, 1> {
  using detail::cmp_pred<C, ordering::gt, 1>::cmp_pred;
};

template <typename C>
struct compare_greater : detail::cmp_pred<C, ordering::gt, 0> {
  using detail::cmp_pred<C, ordering::gt, 0>::cmp_pred;
};

template <typename C>
struct compare_greater_equal : detail::cmp_pred<C, ordering::lt, 1> {
  using detail::cmp_pred<C, ordering::lt, 1>::cmp_pred;
};

namespace detail {

// extracted to a template so initialization can be inline and visible in c++14
template <typename D>
struct partial_ordering_ {
  static D const less;
  static D const greater;
  static D const equivalent;
  static D const unordered;
};

template <typename D>
FOLLY_STORAGE_CONSTEXPR D const partial_ordering_<D>::less{ordering::lt};
template <typename D>
FOLLY_STORAGE_CONSTEXPR D const partial_ordering_<D>::greater{ordering::gt};
template <typename D>
FOLLY_STORAGE_CONSTEXPR D const partial_ordering_<D>::equivalent{ordering::eq};
template <typename D>
FOLLY_STORAGE_CONSTEXPR D const //
    partial_ordering_<D>::unordered{typename D::unordered_tag{}};

} // namespace detail

/// @def partial_ordering
///
/// mimic: std::partial_ordering, c++20 (partial)
class partial_ordering : private detail::partial_ordering_<partial_ordering> {
 private:
  using constants = detail::partial_ordering_<partial_ordering>;
  friend constants;
  class unordered_tag {};

 public:
  using constants::equivalent;
  using constants::greater;
  using constants::less;
  using constants::unordered;

  explicit constexpr partial_ordering(unordered_tag) noexcept : value_{undef} {}
  /* implicit */ constexpr partial_ordering(ordering o) noexcept
      : value_{int8_t(o)} {}

  explicit operator ordering() const noexcept(false) {
    switch (value_) {
      case undef:
        throw_exception<std::out_of_range>("unordered");
      default:
        return ordering(value_);
    }
  }

  friend bool operator==(partial_ordering a, partial_ordering b) noexcept {
    return a.value_ == b.value_;
  }

 private:
  static constexpr int8_t undef = 2;
  int8_t value_ = undef;
};

} // namespace folly
