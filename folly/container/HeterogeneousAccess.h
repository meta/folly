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

#include <functional>
#include <string>
#include <string_view>
#include <type_traits>

#include <folly/Range.h>
#include <folly/Traits.h>
#include <folly/container/HeterogeneousAccess-fwd.h>
#include <folly/hash/Hash.h>
#include <folly/hash/detail/RandomSeed.h>
#include <folly/hash/rapidhash.h>

namespace folly {

// folly::HeterogeneousAccessEqualTo<T>, and
// folly::HeterogeneousAccessHash<T> are functors suitable as defaults
// for containers that support heterogeneous access.  When possible, they
// will be marked as transparent.  When no transparent implementation
// is available then they fall back to std::equal_to and std::hash
// respectively.  Since the fallbacks are not marked as transparent,
// heterogeneous lookup won't be available in that case.  A corresponding
// HeterogeneousAccessLess<T> could be easily added if desired.
//
// If T can be implicitly converted to a StringPiece or
// to a Range<T::value_type const*> that is hashable, then
// HeterogeneousAccess{EqualTo,Hash}<T> will be transparent without any
// additional work.  In practice this is true for T that can be convered to
// StringPiece or Range<IntegralType const*>.  This includes std::string,
// std::string_view (when available), std::array, folly::Range,
// std::vector, and folly::small_vector.
//
// Additional specializations of HeterogeneousAccess*<T> should go in
// the header that declares T.  Don't forget to typedef is_transparent to
// void and folly_is_avalanching to std::true_type in the specializations.
//
// Hash values of folly::HeterogeneousAccessHash are not guaranteed to be
// stable across different program runs, so do not persist hash values
// anywhere.

template <typename T, typename Enable>
struct HeterogeneousAccessEqualTo : std::equal_to<T> {};

template <typename T, typename Enable>
struct HeterogeneousAccessHash : std::hash<T> {
  using folly_is_avalanching = IsAvalanchingHasher<std::hash<T>, T>;
};

//////// strings

namespace detail {

template <typename T, typename Enable = void>
struct ValueTypeForTransparentConversionToRange {
  using type = char;
};

// We assume that folly::hasher<folly::Range<T const*>> won't be enabled
// when it would be lower quality than std::hash<U> for a U that is
// convertible to folly::Range<T const*>.
template <typename T>
struct ValueTypeForTransparentConversionToRange<
    T,
    std::void_t<
        decltype(std::declval<hasher<Range<typename T::value_type const*>>>()(
            std::declval<Range<typename T::value_type const*>>()))>> {
  using type = std::remove_const_t<typename T::value_type>;
};

template <typename T>
using TransparentlyConvertibleToRange = std::is_convertible<
    T,
    Range<typename ValueTypeForTransparentConversionToRange<T>::type const*>>;

template <typename T>
struct TransparentRangeEqualTo {
  using is_transparent = void;

  template <typename TOtherHash>
  using is_compatible =
      std::is_base_of<detail::TransparentRangeEqualTo<T>, TOtherHash>;

  template <typename U1, typename U2>
  bool operator()(U1 const& lhs, U2 const& rhs) const {
    return Range<T const*>{lhs} == Range<T const*>{rhs};
  }

  // This overload is not required for functionality, but
  // guarantees that replacing std::equal_to<std::string> with
  // HeterogeneousAccessEqualTo<std::string> is truly zero overhead
  bool operator()(std::string const& lhs, std::string const& rhs) const {
    return lhs == rhs;
  }
};

template <typename T>
struct TransparentRangeHash {
  using is_transparent = void;
  using folly_is_avalanching = std::true_type;

  template <typename TOtherHash>
  using is_compatible =
      std::is_base_of<detail::TransparentRangeHash<T>, TOtherHash>;

  template <typename U>
  std::size_t operator()(U const& stringish) const {
    return hasher<Range<T const*>>{}(Range<T const*>{stringish});
  }
};

template <>
struct TransparentRangeHash<char> {
  using is_transparent = void;
  using folly_is_avalanching = std::true_type;

  template <typename TOtherHash>
  using is_compatible =
      std::is_base_of<detail::TransparentRangeHash<char>, TOtherHash>;

  template <typename U>
  std::size_t operator()(U const& stringish) const {
    auto sp = StringPiece{stringish};
    return static_cast<std::size_t>(folly::hash::rapidhashNano_with_seed(
        sp.data(), sp.size(), hash::detail::RandomSeed::seed()));
  }
};

template <
    typename TableKey,
    typename Hasher,
    typename KeyEqual,
    typename ArgKey>
struct EligibleForHeterogeneousFind
    : std::conjunction<
          is_transparent<Hasher>,
          is_transparent<KeyEqual>,
          is_invocable<Hasher, ArgKey const&>,
          is_invocable<KeyEqual, ArgKey const&, TableKey const&>> {};

template <
    typename TableKey,
    typename Hasher,
    typename KeyEqual,
    typename ArgKey>
using EligibleForHeterogeneousInsert = std::conjunction<
    EligibleForHeterogeneousFind<TableKey, Hasher, KeyEqual, ArgKey>,
    std::is_constructible<TableKey, ArgKey>>;

template <typename T, typename OtherT, typename Enable = void>
struct HasCompatibleTest : std::false_type {};

template <typename T, typename OtherT>
struct HasCompatibleTest<
    T,
    OtherT,
    std::void_t<typename T::template is_compatible<OtherT>>> : std::true_type {
};

} // namespace detail

template <typename T>
  requires detail::TransparentlyConvertibleToRange<T>::value
struct HeterogeneousAccessEqualTo<T, void>
    : detail::TransparentRangeEqualTo<
          typename detail::ValueTypeForTransparentConversionToRange<T>::type> {
};

template <typename T>
  requires detail::TransparentlyConvertibleToRange<T>::value
struct HeterogeneousAccessHash<T, void>
    : detail::TransparentRangeHash<
          typename detail::ValueTypeForTransparentConversionToRange<T>::type> {
};

template <typename Hash1, typename Hash2, typename Enable = void>
struct HeterogeneousPreHashCompatible : std::is_same<Hash1, Hash2> {};

template <typename Hash1, typename Hash2>
  requires(
      detail::HasCompatibleTest<Hash1, Hash2>::value &&
      detail::HasCompatibleTest<Hash2, Hash1>::value)
struct HeterogeneousPreHashCompatible<Hash1, Hash2, void>
    : std::disjunction<
          typename Hash1::template is_compatible<Hash2>,
          typename Hash2::template is_compatible<Hash1>> {};

template <typename Hash1, typename Hash2>
  requires(
      detail::HasCompatibleTest<Hash1, Hash2>::value &&
      !detail::HasCompatibleTest<Hash2, Hash1>::value)
struct HeterogeneousPreHashCompatible<Hash1, Hash2, void>
    : Hash1::template is_compatible<Hash2> {};

template <typename Hash1, typename Hash2>
  requires(
      !detail::HasCompatibleTest<Hash1, Hash2>::value &&
      detail::HasCompatibleTest<Hash2, Hash1>::value)
struct HeterogeneousPreHashCompatible<Hash1, Hash2, void>
    : Hash2::template is_compatible<Hash1> {};

} // namespace folly
