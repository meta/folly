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

#include <array>
#include <compare>
#include <concepts>
#include <deque>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <folly/Traits.h>
#include <folly/container/Iterator.h>

namespace folly::iterator_test_utils {

template <typename It>
concept has_arrow = std::is_pointer_v<It> || requires(const It i) {
  i.operator->();
};

template <typename F>
void for_each_interesting_iterator_type(F f) {
  std::vector<int> ints;
  std::list<int> ints_list;

  auto ints_iota = std::views::iota(0, 1);
  auto ints_transformed = ints | std::views::transform([](int i) { return i; });
  auto ints_filtered =
      ints_list | std::views::filter([](int i) { return i > 0; });

  f(folly::tag<int*>);
  f(folly::tag<const int*>);
  f(folly::tag<std::array<int, 4>::iterator>);
  f(folly::tag<std::vector<int>::iterator>);
  f(folly::tag<std::vector<int>::const_iterator>);
  f(folly::tag<std::vector<bool>::iterator>);
  f(folly::tag<std::deque<int>::iterator>);
  f(folly::tag<std::list<int>::iterator>);
  f(folly::tag<std::set<int>::iterator>);
  f(folly::tag<std::map<int, int>::iterator>);
  f(folly::tag<std::string::iterator>);
  f(folly::tag<std::string_view::iterator>);
  f(folly::tag<std::span<int>::iterator>);
  f(folly::tag<std::reverse_iterator<int*>>);
  f(folly::tag<folly::reverse_iterator<int*>>);
  f(folly::tag<folly::index_iterator<std::vector<int>>>);
  f(folly::tag<decltype(ints_iota.begin())>);
  f(folly::tag<decltype(ints_transformed.begin())>);
  f(folly::tag<decltype(ints_filtered.begin())>);
}

template <typename It, typename Reference>
constexpr void check_traits_match() {
  static_assert(
      std::same_as<
          folly::iterator_concept_t<It>,
          folly::iterator_concept_t<Reference>>);
  static_assert(
      std::same_as<
          folly::iterator_category_t<It>,
          folly::iterator_category_t<Reference>>);
  static_assert(
      std::same_as<std::iter_value_t<It>, std::iter_value_t<Reference>>);
  static_assert(
      std::same_as<
          std::iter_difference_t<It>,
          std::iter_difference_t<Reference>>);
  static_assert(
      std::same_as<
          typename std::iterator_traits<It>::pointer,
          typename std::iterator_traits<Reference>::pointer>);
  static_assert(
      std::
          same_as<std::iter_reference_t<It>, std::iter_reference_t<Reference>>);
  static_assert(
      std::same_as<
          std::iter_rvalue_reference_t<It>,
          std::iter_rvalue_reference_t<Reference>>);
  static_assert(has_arrow<It> == has_arrow<Reference>);
  static_assert(
      std::sized_sentinel_for<It, It> ==
      std::sized_sentinel_for<Reference, Reference>);
  static_assert(
      std::three_way_comparable<It> == std::random_access_iterator<It>);
  static_assert(std::totally_ordered<It> == std::totally_ordered<Reference>);
  static_assert(std::forward_iterator<It> == std::forward_iterator<Reference>);
  static_assert(
      std::bidirectional_iterator<It> ==
      std::bidirectional_iterator<Reference>);
  static_assert(
      std::random_access_iterator<It> ==
      std::random_access_iterator<Reference>);
  static_assert(
      std::contiguous_iterator<It> == std::contiguous_iterator<Reference>);
  if constexpr (!std::bidirectional_iterator<It>) {
    static_assert(!requires(It it) { --it; });
    static_assert(!requires(It it) { it--; });
  }
  if constexpr (!std::random_access_iterator<It>) {
    static_assert(!requires(It it, std::iter_difference_t<It> n) { it += n; });
    static_assert(!requires(It it, std::iter_difference_t<It> n) { it -= n; });
    static_assert(!requires(It it, std::iter_difference_t<It> n) { it + n; });
    static_assert(!requires(It it, std::iter_difference_t<It> n) { n + it; });
    static_assert(!requires(It it, std::iter_difference_t<It> n) { it - n; });
    static_assert(!requires(It it, std::iter_difference_t<It> n) { it[n]; });
  }
}

template <typename A, typename B>
constexpr bool same_referent(A&& a, B&& b) {
  if constexpr (
      std::is_lvalue_reference_v<A> && std::is_lvalue_reference_v<B>) {
    return std::addressof(a) == std::addressof(b);
  } else {
    return a == b;
  }
}

// NOLINTBEGIN(performance-unnecessary-value-param)

// *, -> and std::to_address of it, and it[k] for random access
template <typename It, typename Ref>
constexpr bool same_access(It it, Ref ref, Ref ref_last) {
  if (!same_referent(*it, *ref)) {
    return false;
  }
  if constexpr (
      has_arrow<Ref> &&
      std::is_lvalue_reference_v<std::iter_reference_t<Ref>>) {
    if (it.operator->() != std::addressof(*ref)) {
      return false;
    }
  }
  if constexpr (std::contiguous_iterator<Ref>) {
    if (std::to_address(it) != std::to_address(ref)) {
      return false;
    }
  }
  if constexpr (std::random_access_iterator<Ref>) {
    for (std::iter_difference_t<Ref> k = 0; k != ref_last - ref; ++k) {
      if (!same_referent(it[k], ref[k])) {
        return false;
      }
    }
  }
  return true;
}

// ++, it++, --, it--, then + n, n +, += n, - n, -= n, where n is the size
template <typename It, typename Ref>
constexpr bool same_arithmetic(It first, Ref ref_first, Ref ref_last) {
  const auto same = [&](It it, Ref ref) {
    return std::ranges::distance(first, it) ==
        std::ranges::distance(ref_first, ref) &&
        (ref == ref_last || *it == *ref);
  };

  It it = first;
  Ref ref = ref_first;

  // each step checks what the operator returned and where it left `it`
  if (!same(++it, ++ref) || !same(it, ref)) {
    return false;
  }
  if (!same(it++, ref++) || !same(it, ref)) {
    return false;
  }
  if constexpr (std::bidirectional_iterator<Ref>) {
    if (!same(--it, --ref) || !same(it, ref)) {
      return false;
    }
    if (!same(it--, ref--) || !same(it, ref)) {
      return false;
    }
  }
  if constexpr (std::random_access_iterator<Ref>) {
    const auto n = ref_last - ref_first;
    if (!same(it + n, ref + n) || !same(it, ref)) {
      return false;
    }
    if (!same(n + it, n + ref) || !same(it, ref)) {
      return false;
    }
    if (!same(it += n, ref += n) || !same(it, ref)) {
      return false;
    }
    if (!same(it - n, ref - n) || !same(it, ref)) {
      return false;
    }
    if (!same(it -= n, ref -= n) || !same(it, ref)) {
      return false;
    }
  }
  return true;
}

// last - first, and between It and It2 (e.g. const): last - first2 and back
template <typename It, typename It2, typename Ref>
constexpr bool same_differences(
    It first, It last, It2 first2, Ref ref_first, Ref ref_last) {
  return last - first == ref_last - ref_first &&
      first - last == ref_first - ref_last &&
      last - first2 == ref_last - ref_first &&
      first2 - last == ref_first - ref_last;
}

// every comparison between lo and hi, which may be different types
template <typename It, typename It2, typename Ref>
constexpr bool same_comparisons(It lo, It2 hi, Ref ref_lo, Ref ref_hi) {
  // NOLINTNEXTLINE
  if ((lo == lo) != (ref_lo == ref_lo) || (lo == hi) != (ref_lo == ref_hi) ||
      (lo != hi) != (ref_lo != ref_hi)) {
    return false;
  }
  if constexpr (std::totally_ordered<Ref>) {
    if ((lo < hi) != (ref_lo < ref_hi) || (hi < lo) != (ref_hi < ref_lo) ||
        (hi > lo) != (ref_hi > ref_lo) || (lo > hi) != (ref_lo > ref_hi) ||
        (lo <= hi) != (ref_lo <= ref_hi) || (hi <= lo) != (ref_hi <= ref_lo) ||
        (hi >= lo) != (ref_hi >= ref_lo) || (lo >= hi) != (ref_lo >= ref_hi)) {
      return false;
    }
  }
  if constexpr (std::three_way_comparable<Ref>) {
    if ((lo <=> hi) != (ref_lo <=> ref_hi) ||
        (hi <=> lo) != (ref_hi <=> ref_lo) ||
        (lo <=> lo) != (ref_lo <=> ref_lo)) {
      return false;
    }
  }
  return true;
}

// NOLINTEND(performance-unnecessary-value-param)

} // namespace folly::iterator_test_utils
