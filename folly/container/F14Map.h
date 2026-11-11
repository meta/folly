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

/**
 * F14NodeMap, F14ValueMap, and F14VectorMap
 *
 * F14FastMap conditionally works like F14ValueMap or F14VectorMap
 *
 * See F14.md
 */

#include <concepts>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <tuple>

#include <folly/CppAttributes.h>
#include <folly/Portability.h>
#include <folly/Range.h>
#include <folly/Traits.h>
#include <folly/container/View.h>
#include <folly/lang/Exception.h>
#include <folly/lang/SafeAssert.h>

#include <folly/container/F14Map-fwd.h>
#include <folly/container/Iterator.h>
#include <folly/container/detail/F14Policy.h>
#include <folly/container/detail/F14Table.h>
#include <folly/container/detail/Util.h>

// If !FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE, fallback definitions are exported
// in this file
#include <folly/container/detail/F14MapFallback.h> // IWYU pragma: export

namespace folly {

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

//////// Common case for supported platforms

namespace f14 {
namespace detail {

template <typename Policy>
class F14BasicMap {
  template <typename K>
  static constexpr bool kEligibleForHeterogeneousFind =
      ::folly::detail::EligibleForHeterogeneousFind<
          typename Policy::Key,
          typename Policy::Hasher,
          typename Policy::KeyEqual,
          K>::value;

  template <typename K>
  static constexpr bool kEligibleForHeterogeneousInsert =
      ::folly::detail::EligibleForHeterogeneousInsert<
          typename Policy::Key,
          typename Policy::Hasher,
          typename Policy::KeyEqual,
          K>::value;

  template <typename K>
  using IsIter = std::disjunction<
      std::is_same<typename Policy::Iter, std::remove_cvref_t<K>>,
      std::is_same<typename Policy::ConstIter, std::remove_cvref_t<K>>>;

  template <typename K>
  static constexpr bool kEligibleForHeterogeneousErase =
      ::folly::detail::EligibleForHeterogeneousFind<
          typename Policy::Key,
          typename Policy::Hasher,
          typename Policy::KeyEqual,
          std::conditional_t<IsIter<K>::value, typename Policy::Key, K>>::
          value &&
      !IsIter<K>::value;

 public:
  //// PUBLIC - Member types

  using key_type = typename Policy::Key;
  using mapped_type = typename Policy::Mapped;
  using value_type = typename Policy::Value;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using hash_token_type = F14HashToken;
  using hasher = typename Policy::Hasher;
  using key_equal = typename Policy::KeyEqual;
  using hashed_key_type = F14HashedKey<key_type, hasher, key_equal>;
  using allocator_type = typename Policy::Alloc;
  using reference = value_type&;
  using const_reference = value_type const&;
  using pointer = typename Policy::AllocTraits::pointer;
  using const_pointer = typename Policy::AllocTraits::const_pointer;
  using iterator = typename Policy::Iter;
  using const_iterator = typename Policy::ConstIter;

 private:
  using ItemIter = typename Policy::ItemIter;

 public:
  //// PUBLIC - Member functions

  FOLLY_F14_CONSTEXPR_CONSTRUCTOR F14BasicMap() noexcept(
      Policy::kDefaultConstructIsNoexcept)
      : table_{} {}

  explicit F14BasicMap(
      std::size_t initialCapacity,
      hasher const& hash = hasher{},
      key_equal const& eq = key_equal{},
      allocator_type const& alloc = allocator_type{})
      : table_{initialCapacity, hash, eq, alloc} {}

  explicit F14BasicMap(std::size_t initialCapacity, allocator_type const& alloc)
      : F14BasicMap(initialCapacity, hasher{}, key_equal{}, alloc) {}

  explicit F14BasicMap(
      std::size_t initialCapacity,
      hasher const& hash,
      allocator_type const& alloc)
      : F14BasicMap(initialCapacity, hash, key_equal{}, alloc) {}

  explicit F14BasicMap(allocator_type const& alloc)
      : F14BasicMap(0, hasher{}, key_equal{}, alloc) {}

  template <typename InputIt>
  F14BasicMap(
      InputIt first,
      InputIt last,
      std::size_t initialCapacity = 0,
      hasher const& hash = hasher{},
      key_equal const& eq = key_equal{},
      allocator_type const& alloc = allocator_type{})
      : table_{initialCapacity, hash, eq, alloc} {
    bool autoReserve = initialCapacity == 0;
    bulkInsert(std::move(first), std::move(last), autoReserve);
  }

  template <typename InputIt>
  F14BasicMap(
      InputIt first,
      InputIt last,
      std::size_t initialCapacity,
      allocator_type const& alloc)
      : table_{initialCapacity, hasher{}, key_equal{}, alloc} {
    bool autoReserve = initialCapacity == 0;
    bulkInsert(std::move(first), std::move(last), autoReserve);
  }

  template <typename InputIt>
  F14BasicMap(
      InputIt first,
      InputIt last,
      std::size_t initialCapacity,
      hasher const& hash,
      allocator_type const& alloc)
      : table_{initialCapacity, hash, key_equal{}, alloc} {
    bool autoReserve = initialCapacity == 0;
    bulkInsert(std::move(first), std::move(last), autoReserve);
  }

  F14BasicMap(F14BasicMap const& rhs) = default;

  F14BasicMap(F14BasicMap const& rhs, allocator_type const& alloc)
      : table_{rhs.table_, alloc} {}

  F14BasicMap(F14BasicMap&& rhs) = default;

  F14BasicMap(F14BasicMap&& rhs, allocator_type const& alloc) noexcept(
      Policy::kAllocIsAlwaysEqual)
      : table_{std::move(rhs.table_), alloc} {}

  /* implicit */ F14BasicMap(
      std::initializer_list<value_type> init,
      std::size_t initialCapacity = 0,
      hasher const& hash = hasher{},
      key_equal const& eq = key_equal{},
      allocator_type const& alloc = allocator_type{})
      : table_{initialCapacity, hash, eq, alloc} {
    bool autoReserve = initialCapacity == 0;
    bulkInsert(init.begin(), init.end(), autoReserve);
  }

  F14BasicMap(
      std::initializer_list<value_type> init,
      std::size_t initialCapacity,
      allocator_type const& alloc)
      : table_{initialCapacity, hasher{}, key_equal{}, alloc} {
    bool autoReserve = initialCapacity == 0;
    bulkInsert(init.begin(), init.end(), autoReserve);
  }

  F14BasicMap(
      std::initializer_list<value_type> init,
      std::size_t initialCapacity,
      hasher const& hash,
      allocator_type const& alloc)
      : table_{initialCapacity, hash, key_equal{}, alloc} {
    bool autoReserve = initialCapacity == 0;
    bulkInsert(init.begin(), init.end(), autoReserve);
  }

  F14BasicMap& operator=(F14BasicMap const&) = default;

  F14BasicMap& operator=(F14BasicMap&&) = default;

  F14BasicMap& operator=(std::initializer_list<value_type> ilist) {
    clear();
    bulkInsert(ilist.begin(), ilist.end(), true);
    return *this;
  }

  /// Get the allocator for this container.
  /// @methodset Allocator
  allocator_type get_allocator() const noexcept { return table_.alloc(); }

  //// PUBLIC - Iterators

  /// Get an iterator to the beginning
  /// @methodset Iterators
  iterator begin() noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return table_.makeIter(table_.begin());
  }
  const_iterator begin() const noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return cbegin();
  }
  /// Get an iterator to the beginning
  /// @methodset Iterators
  const_iterator cbegin() const noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return table_.makeConstIter(table_.begin());
  }

  /// Get an iterator to the end
  /// @methodset Iterators
  iterator end() noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return table_.makeIter(table_.end());
  }
  const_iterator end() const noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return cend();
  }
  /// Get an iterator to the end
  /// @methodset Iterators
  const_iterator cend() const noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return table_.makeConstIter(table_.end());
  }

  //// PUBLIC - Capacity

  /// Check if this container has any elements.
  /// @methodset Capacity
  bool empty() const noexcept { return table_.empty(); }

  /// Number of elements in this container.
  /// @methodset Capacity
  std::size_t size() const noexcept { return table_.size(); }

  /// The maximum size of this container.
  /// @methodset Capacity
  std::size_t max_size() const noexcept { return table_.max_size(); }

  //// PUBLIC - Modifiers

  /**
   * Remove all elements.
   * @methodset Modifiers
   *
   * Frees heap-allocated memory; bucket_count is returned to 0.
   */
  void clear() noexcept { table_.clear(); }

  /**
   * Add a single element.
   * @overloadbrief Add elements.
   * @methodset Modifiers
   */
  std::pair<iterator, bool> insert(value_type const& value) {
    return emplace(value);
  }

  /// Add a single element, of a heterogeneous type.
  template <typename P>
    requires std::is_constructible_v<value_type, P&&>
  std::pair<iterator, bool> insert(P&& value) {
    return emplace(std::forward<P>(value));
  }

  /**
   * Add a single element, of a heterogeneous type.
   *
   * TODO(T31574848): Work around libstdc++ versions (e.g., GCC < 6) with no
   * implementation of N4387 ("perfect initialization" for pairs and tuples).
   */
  template <typename U1, typename U2>
    requires(
        std::is_constructible_v<key_type, U1 const&> &&
        std::is_constructible_v<mapped_type, U2 const&>)
  std::pair<iterator, bool> insert(std::pair<U1, U2> const& value) {
    return emplace(value);
  }

  /**
   * Add a single element, of a heterogeneous type.
   *
   * TODO(T31574848): Work around libstdc++ versions (e.g., GCC < 6) with no
   * implementation of N4387 ("perfect initialization" for pairs and tuples).
   */
  template <typename U1, typename U2>
    requires(
        std::is_constructible_v<key_type, U1 &&> &&
        std::is_constructible_v<mapped_type, U2 &&>)
  std::pair<iterator, bool> insert(std::pair<U1, U2>&& value) {
    return emplace(std::move(value));
  }

  /// Add a single element.
  std::pair<iterator, bool> insert(value_type&& value) {
    return emplace(std::move(value));
  }

  // std::unordered_map's hinted insertion API is misleading.  No
  // implementation I've seen actually uses the hint.  Code restructuring
  // by the caller to use the hinted API is at best unnecessary, and at
  // worst a pessimization.  It is used, however, so we provide it.

  /// Add a single element, with a locational hint.
  iterator insert(const_iterator /*hint*/, value_type const& value) {
    return insert(value).first;
  }

  /// Add a single element, of heterogeneous type, with a locational hint.
  template <typename P>
    requires std::is_constructible_v<value_type, P&&>
  iterator insert(const_iterator /*hint*/, P&& value) {
    return insert(std::forward<P>(value)).first;
  }

  /// Add a single element, with a locational hint.
  iterator insert(const_iterator /*hint*/, value_type&& value) {
    return insert(std::move(value)).first;
  }

  /// Emplace with hint.
  /// @methodset Modifiers
  template <class... Args>
  iterator emplace_hint(const_iterator /*hint*/, Args&&... args) {
    return emplace(std::forward<Args>(args)...).first;
  }

 private:
  template <class InputIt>
  FOLLY_ALWAYS_INLINE void bulkInsert(
      InputIt first, InputIt last, bool autoReserve) {
    if (autoReserve && std::random_access_iterator<InputIt>) {
      // Will over-reserve if first..last contains duplicates
      auto n = std::distance(first, last);
      if (n == 0) {
        return;
      }
      table_.reserveForInsert(n);
    }
    while (first != last) {
      insert(*first);
      ++first;
    }
  }

 public:
  /// Add elements
  template <class InputIt>
  void insert(InputIt first, InputIt last) {
    // Bulk reserve is a heuristic choice, so it can backfire.  We restrict
    // ourself to situations that mimic bulk construction without an
    // explicit initialCapacity.
    bool autoReserve = bucket_count() == 0;
    bulkInsert(std::move(first), std::move(last), autoReserve);
  }

  /// Add elements from a range
  template <std::ranges::input_range R>
    requires std::convertible_to<std::ranges::range_reference_t<R>, value_type>
  void insert_range(R&& range) {
    insert(std::ranges::begin(range), std::ranges::end(range));
  }

  /// Add elements from an initializer list
  void insert(std::initializer_list<value_type> ilist) {
    insert(ilist.begin(), ilist.end());
  }

  /// Insert if the key is missing, overwrite using operator= if present.
  /// @methodset Modifiers
  template <typename M>
  std::pair<iterator, bool> insert_or_assign(key_type const& key, M&& obj) {
    auto rv = try_emplace(key, std::forward<M>(obj));
    if (!rv.second) {
      rv.first->second = std::forward<M>(obj);
    }
    return rv;
  }

  template <typename M>
  std::pair<iterator, bool> insert_or_assign(key_type&& key, M&& obj) {
    auto rv = try_emplace(std::move(key), std::forward<M>(obj));
    if (!rv.second) {
      rv.first->second = std::forward<M>(obj);
    }
    return rv;
  }

  template <typename M>
  std::pair<iterator, bool> insert_or_assign(
      F14HashToken const& token, key_type const& key, M&& obj) {
    auto rv = try_emplace_token(token, key, std::forward<M>(obj));
    if (!rv.second) {
      rv.first->second = std::forward<M>(obj);
    }
    return rv;
  }

  template <typename M>
  std::pair<iterator, bool> insert_or_assign(
      F14HashToken const& token, key_type&& key, M&& obj) {
    auto rv = try_emplace_token(token, std::move(key), std::forward<M>(obj));
    if (!rv.second) {
      rv.first->second = std::forward<M>(obj);
    }
    return rv;
  }

  template <typename M>
  iterator insert_or_assign(
      const_iterator /*hint*/, key_type const& key, M&& obj) {
    return insert_or_assign(key, std::forward<M>(obj)).first;
  }

  template <typename M>
  iterator insert_or_assign(const_iterator /*hint*/, key_type&& key, M&& obj) {
    return insert_or_assign(std::move(key), std::forward<M>(obj)).first;
  }

  template <typename K, typename M>
    requires kEligibleForHeterogeneousInsert<K>
  std::pair<iterator, bool> insert_or_assign(K&& key, M&& obj) {
    auto rv = try_emplace(std::forward<K>(key), std::forward<M>(obj));
    if (!rv.second) {
      rv.first->second = std::forward<M>(obj);
    }
    return rv;
  }

  template <typename K, typename M>
    requires kEligibleForHeterogeneousInsert<K>
  std::pair<iterator, bool> insert_or_assign(
      F14HashToken const& token, K&& key, M&& obj) {
    auto rv =
        try_emplace_token(token, std::forward<K>(key), std::forward<M>(obj));
    if (!rv.second) {
      rv.first->second = std::forward<M>(obj);
    }
    return rv;
  }

 private:
  template <typename Arg>
  using UsableAsKey = ::folly::detail::
      EligibleForHeterogeneousFind<key_type, hasher, key_equal, Arg>;

 public:
  /**
   * Add an element by constructing it in-place.
   * @overloadbrief Add elements in-place.
   * @methodset Modifiers
   */
  template <typename... Args>
  std::pair<iterator, bool> emplace(Args&&... args) {
    auto rv =
        folly::detail::callWithExtractedKey<key_type, mapped_type, UsableAsKey>(
            table_.alloc(),
            [&](auto&&... inner) {
              return table_.tryEmplaceValue(
                  std::forward<decltype(inner)>(inner)...);
            },
            std::forward<Args>(args)...);
    return std::make_pair(table_.makeIter(rv.first), rv.second);
  }

  /**
   * Add an element by constructing it in-place.
   * @methodset Modifiers
   *
   * Does nothing if the key already exists.
   */
  template <typename... Args>
  std::pair<iterator, bool> try_emplace(key_type const& key, Args&&... args) {
    auto rv = table_.tryEmplaceValue(
        key,
        std::piecewise_construct,
        std::forward_as_tuple(key),
        std::forward_as_tuple(std::forward<Args>(args)...));
    return std::make_pair(table_.makeIter(rv.first), rv.second);
  }

  template <typename... Args>
  std::pair<iterator, bool> try_emplace(key_type&& key, Args&&... args) {
    auto rv = table_.tryEmplaceValue(
        key,
        std::piecewise_construct,
        std::forward_as_tuple(std::move(key)),
        std::forward_as_tuple(std::forward<Args>(args)...));
    return std::make_pair(table_.makeIter(rv.first), rv.second);
  }

  /// @copydoc try_emplace
  template <typename... Args>
  std::pair<iterator, bool> try_emplace_token(
      F14HashToken const& token, key_type const& key, Args&&... args) {
    auto rv = table_.tryEmplaceValueWithToken(
        token,
        key,
        std::piecewise_construct,
        std::forward_as_tuple(key),
        std::forward_as_tuple(std::forward<Args>(args)...));
    return std::make_pair(table_.makeIter(rv.first), rv.second);
  }

  template <typename... Args>
  std::pair<iterator, bool> try_emplace_token(
      F14HashToken const& token, key_type&& key, Args&&... args) {
    auto rv = table_.tryEmplaceValueWithToken(
        token,
        key,
        std::piecewise_construct,
        std::forward_as_tuple(std::move(key)),
        std::forward_as_tuple(std::forward<Args>(args)...));
    return std::make_pair(table_.makeIter(rv.first), rv.second);
  }

  template <typename... Args>
  iterator try_emplace(
      const_iterator /*hint*/, key_type const& key, Args&&... args) {
    auto rv = table_.tryEmplaceValue(
        key,
        std::piecewise_construct,
        std::forward_as_tuple(key),
        std::forward_as_tuple(std::forward<Args>(args)...));
    return table_.makeIter(rv.first);
  }

  template <typename... Args>
  iterator try_emplace(
      const_iterator /*hint*/, key_type&& key, Args&&... args) {
    auto rv = table_.tryEmplaceValue(
        key,
        std::piecewise_construct,
        std::forward_as_tuple(std::move(key)),
        std::forward_as_tuple(std::forward<Args>(args)...));
    return table_.makeIter(rv.first);
  }

  template <typename K, typename... Args>
    requires kEligibleForHeterogeneousInsert<K>
  std::pair<iterator, bool> try_emplace(K&& key, Args&&... args) {
    auto rv = table_.tryEmplaceValue(
        key,
        std::piecewise_construct,
        std::forward_as_tuple(std::forward<K>(key)),
        std::forward_as_tuple(std::forward<Args>(args)...));
    return std::make_pair(table_.makeIter(rv.first), rv.second);
  }

  template <typename K, typename... Args>
    requires kEligibleForHeterogeneousInsert<K>
  std::pair<iterator, bool> try_emplace_token(
      F14HashToken const& token, K&& key, Args&&... args) {
    auto rv = table_.tryEmplaceValueWithToken(
        token,
        key,
        std::piecewise_construct,
        std::forward_as_tuple(std::forward<K>(key)),
        std::forward_as_tuple(std::forward<Args>(args)...));
    return std::make_pair(table_.makeIter(rv.first), rv.second);
  }

  /**
   * Remove element at a specific position (iterator).
   * @overloadbrief Remove elements.
   * @methodset Modifiers
   */
  FOLLY_ALWAYS_INLINE iterator erase(const_iterator pos) {
    return eraseInto(pos, variadic_noop);
  }

  /**
   * Remove element at a specific position (iterator).
   *
   * This form avoids ambiguity when key_type has a templated constructor
   * that accepts const_iterator
   */
  FOLLY_ALWAYS_INLINE iterator erase(iterator pos) {
    return eraseInto(pos, variadic_noop);
  }

  /// Remove a range of elements.
  iterator erase(const_iterator first, const_iterator last) {
    return eraseInto(first, last, variadic_noop);
  }

  /// Remove a specific key.
  size_type erase(key_type const& key) { return eraseInto(key, variadic_noop); }

  /// Remove a key, using a heterogeneous representation.
  template <typename K>
    requires kEligibleForHeterogeneousErase<K>
  size_type erase(K const& key) {
    return eraseInto(key, variadic_noop);
  }

 protected:
  template <typename BeforeDestroy>
  FOLLY_ALWAYS_INLINE void tableEraseIterInto(
      ItemIter pos, BeforeDestroy&& beforeDestroy) {
    table_.eraseIterInto(pos, [&](value_type&& v) {
      auto p = Policy::moveValue(v);
      beforeDestroy(std::move(p.first), std::move(p.second));
    });
  }

  template <typename K, typename BeforeDestroy>
  FOLLY_ALWAYS_INLINE std::size_t tableEraseKeyInto(
      K const& key, BeforeDestroy&& beforeDestroy) {
    return table_.eraseKeyInto(key, [&](value_type&& v) {
      auto p = Policy::moveValue(v);
      beforeDestroy(std::move(p.first), std::move(p.second));
    });
  }

 public:
  /**
   * Callback-erase a single iterator.
   * @overloadbrief Erase with pre-destruction callback.
   * @methodset Modifiers
   *
   * eraseInto contains the same overloads as erase but provides
   * an additional callback argument which is called with an rvalue
   * reference (not const) to the key and an rvalue reference to the
   * mapped value directly before it is destroyed. This can be used
   * to extract an entry out of a F14Map while avoiding a copy.
   */
  template <typename BeforeDestroy>
  FOLLY_ALWAYS_INLINE iterator
  eraseInto(const_iterator pos, BeforeDestroy&& beforeDestroy) {
    // If we are inlined then gcc and clang can optimize away all of the
    // work of itemPos.advance() if our return value is discarded.
    auto itemPos = table_.unwrapIter(pos);
    tableEraseIterInto(itemPos, beforeDestroy);
    itemPos.advanceLikelyDead();
    return table_.makeIter(itemPos);
  }

  /// This form avoids ambiguity when key_type has a templated constructor
  /// that accepts const_iterator
  template <typename BeforeDestroy>
  FOLLY_ALWAYS_INLINE iterator
  eraseInto(iterator pos, BeforeDestroy&& beforeDestroy) {
    const_iterator cpos{pos};
    return eraseInto(cpos, beforeDestroy);
  }

  /// Callback-erase a range of values.
  template <typename BeforeDestroy>
  iterator eraseInto(
      const_iterator first,
      const_iterator last,
      BeforeDestroy&& beforeDestroy) {
    auto itemFirst = table_.unwrapIter(first);
    auto itemLast = table_.unwrapIter(last);
    while (itemFirst != itemLast) {
      tableEraseIterInto(itemFirst, beforeDestroy);
      itemFirst.advance();
    }
    return table_.makeIter(itemFirst);
  }

  template <typename BeforeDestroy>
  size_type eraseInto(key_type const& key, BeforeDestroy&& beforeDestroy) {
    return tableEraseKeyInto(key, beforeDestroy);
  }

  /// Callback-erase a specific key, using a heterogeneous representation.
  template <typename K, typename BeforeDestroy>
    requires kEligibleForHeterogeneousErase<K>
  size_type eraseInto(K const& key, BeforeDestroy&& beforeDestroy) {
    return tableEraseKeyInto(key, beforeDestroy);
  }

  //// PUBLIC - Lookup

  /// Get a value for a key
  /// @methodset Element Access
  FOLLY_ALWAYS_INLINE mapped_type& at(key_type const& key)
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return at(*this, key);
  }

  FOLLY_ALWAYS_INLINE mapped_type const& at(key_type const& key) const
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return at(*this, key);
  }

  template <typename K>
    requires kEligibleForHeterogeneousFind<K>
  mapped_type& at(K const& key) [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return at(*this, key);
  }

  template <typename K>
    requires kEligibleForHeterogeneousFind<K>
  mapped_type const& at(K const& key) const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return at(*this, key);
  }

  /// Get a value for a key; create the value if it doesn't already exist
  /// @methodset Element Access
  mapped_type& operator[](key_type const& key)
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return try_emplace(key).first->second;
  }

  mapped_type& operator[](key_type&& key) [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return try_emplace(std::move(key)).first->second;
  }

  template <typename K>
    requires kEligibleForHeterogeneousInsert<K>
  mapped_type& operator[](K&& key) [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return try_emplace(std::forward<K>(key)).first->second;
  }

  /// @overloadbrief Number of elements matching the given key.
  /// @methodset Lookup
  FOLLY_ALWAYS_INLINE size_type count(key_type const& key) const {
    return contains(key) ? 1 : 0;
  }

  template <typename K>
    requires kEligibleForHeterogeneousFind<K>
  FOLLY_ALWAYS_INLINE size_type count(K const& key) const {
    return contains(key) ? 1 : 0;
  }

  /**
   * @overloadbrief Prehash a key.
   * @methodset Lookup
   *
   * prehash(key) does the work of evaluating hash_function()(key)
   * (including additional bit-mixing for non-avalanching hash functions),
   * and wraps the result of that work in a token for later reuse.
   *
   * The returned token may be used at any time, may be used more than
   * once, and may be used in other F14 sets and maps.  Tokens are
   * transferrable between any F14 containers (maps and sets) with the
   * same key_type and equal hash_function()s.
   *
   * Hash tokens are not hints -- it is a bug to call any method on this
   * class with a token t and key k where t isn't the result of a call
   * to prehash(k2) with k2 == k.
   */
  F14HashToken prehash(key_type const& key) const {
    return table_.prehash(key);
  }
  /// @copydoc prehash
  F14HashToken prehash(key_type const& key, std::size_t hash) const {
    return table_.prehash(key, hash);
  }

  /// @copydoc prehash
  template <typename K>
    requires kEligibleForHeterogeneousFind<K>
  F14HashToken prehash(K const& key) const {
    return table_.prehash(key);
  }
  /// @copydoc prehash
  template <typename K>
    requires kEligibleForHeterogeneousFind<K>
  F14HashToken prehash(K const& key, std::size_t hash) const {
    return table_.prehash(key, hash);
  }

  /**
   * @overloadbrief Prefetch cachelines associated with a key.
   * @methodset Lookup
   *
   * prefetch(token) begins prefetching the first steps of looking for key into
   * the local CPU cache.
   *
   * Example Scenario: Loading 2 values from a cold map.
   * You have a map that is cold, meaning it is out of the local CPU cache,
   * and you want to load two values from the map. This can be extended to
   * load N values, but we're loading 2 for simplicity.
   *
   * When the map is cold the dominating factor in the latency is loading the
   * cache line of the entry into the local CPU cache. Using prehash() will
   * issue these cache line fetches in parallel.  That means that by the time we
   * finish map.find(token1, key1) the cache lines needed by map.find(token2,
   * key2) may already be in the local CPU cache. In the best case this will
   * half the latency.
   *
   * It is always okay to call prefetch() before a find() or other lookup
   * operation, as it only prefetches cache lines that are guaranteed to be
   * needed by the lookup.
   *
   *   std::pair<iterator, iterator> find2(
   *       auto& map, key_type const& key1, key_type const& key2) {
   *     auto const token1 = map.prehash(key1);
   *     map.prefetch(token1);
   *     auto const token2 = map.prehash(key2);
   *     map.prefetch(token2);
   *     return std::make_pair(map.find(token1, key1), map.find(token2, key2));
   *   }
   */
  void prefetch(F14HashToken const& token) const { table_.prefetch(token); }

  /// @overloadbrief Get the iterator for a key.
  /// @methodset Lookup
  FOLLY_ALWAYS_INLINE iterator find(key_type const& key)
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return table_.makeIter(table_.find(key));
  }

  /// Get the iterator for a key.
  FOLLY_ALWAYS_INLINE const_iterator find(key_type const& key) const
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return table_.makeConstIter(table_.find(key));
  }

  FOLLY_ALWAYS_INLINE iterator
  find(F14HashToken const& token, key_type const& key)
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return table_.makeIter(table_.find(token, key));
  }

  FOLLY_ALWAYS_INLINE const_iterator
  find(F14HashToken const& token, key_type const& key) const
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return table_.makeConstIter(table_.find(token, key));
  }

  template <typename K>
    requires kEligibleForHeterogeneousFind<K>
  FOLLY_ALWAYS_INLINE iterator find(K const& key)
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return table_.makeIter(table_.find(key));
  }

  template <typename K>
    requires kEligibleForHeterogeneousFind<K>
  FOLLY_ALWAYS_INLINE const_iterator find(K const& key) const
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return table_.makeConstIter(table_.find(key));
  }

  template <typename K>
    requires kEligibleForHeterogeneousFind<K>
  FOLLY_ALWAYS_INLINE iterator find(F14HashToken const& token, K const& key)
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return table_.makeIter(table_.find(token, key));
  }

  template <typename K>
    requires kEligibleForHeterogeneousFind<K>
  FOLLY_ALWAYS_INLINE const_iterator
  find(F14HashToken const& token, K const& key) const
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return table_.makeConstIter(table_.find(token, key));
  }

  /**
   * @overloadbrief Checks if the container contains an element with the
   * specific key.
   * @methodset Lookup
   */
  FOLLY_ALWAYS_INLINE bool contains(key_type const& key) const {
    return !table_.find(key).atEnd();
  }

  template <typename K>
    requires kEligibleForHeterogeneousFind<K>
  FOLLY_ALWAYS_INLINE bool contains(K const& key) const {
    return !table_.find(key).atEnd();
  }

  FOLLY_ALWAYS_INLINE bool contains(
      F14HashToken const& token, key_type const& key) const {
    return !table_.find(token, key).atEnd();
  }

  template <typename K>
    requires kEligibleForHeterogeneousFind<K>
  FOLLY_ALWAYS_INLINE bool contains(
      F14HashToken const& token, K const& key) const {
    return !table_.find(token, key).atEnd();
  }

  /// @overloadbrief Returns the range of elements matching a specific key.
  /// @methodset Lookup
  std::pair<iterator, iterator> equal_range(key_type const& key) {
    return equal_range(*this, key);
  }

  std::pair<const_iterator, const_iterator> equal_range(
      key_type const& key) const {
    return equal_range(*this, key);
  }

  template <typename K>
    requires kEligibleForHeterogeneousFind<K>
  std::pair<iterator, iterator> equal_range(K const& key) {
    return equal_range(*this, key);
  }

  template <typename K>
    requires kEligibleForHeterogeneousFind<K>
  std::pair<const_iterator, const_iterator> equal_range(K const& key) const {
    return equal_range(*this, key);
  }

  //// PUBLIC - Bucket interface

  /// The number of buckets in this container.
  /// @methodset Bucket interface
  std::size_t bucket_count() const noexcept { return table_.bucket_count(); }

  /// The maximum number of buckets for this container.
  /// @methodset Bucket interface
  std::size_t max_bucket_count() const noexcept {
    return table_.max_bucket_count();
  }

  //// PUBLIC - Hash policy

  /// Load factor of the underlying hashtable.
  /// @methodset Hash policy
  float load_factor() const noexcept { return table_.load_factor(); }

  /**
   * @overloadbrief Load factor control.
   * Get the maximum load factor for this container.
   * @methodset Hash policy
   */
  float max_load_factor() const noexcept { return table_.max_load_factor(); }

  /// Set the maximum load factor for this container.
  /// @methodset Hash policy
  void max_load_factor(float v) { table_.max_load_factor(v); }

  /**
   * Rehash this container.
   * @methodset Hash policy
   *
   * This function is provided for compliance with C++'s requirements for
   * hashtables, but is no better than a simple `reserve` call for F14.
   *
   * @param bucketCapacity  The desired capacity across all buckets.
   */
  void rehash(std::size_t bucketCapacity) {
    // The standard's rehash() requires understanding the max load factor,
    // which is easy to get wrong.  Since we don't actually allow adjustment
    // of max_load_factor there is no difference.
    reserve(bucketCapacity);
  }

  /**
   * Pre-allocate space for at least this many elements.
   * @methodset Capacity
   *
   * @param capacity  The number of elements to pre-allocate space for.
   */
  void reserve(std::size_t capacity) { table_.reserve(capacity); }

  //// PUBLIC - Observers

  /// Get the hasher.
  /// @methodset Observers
  hasher hash_function() const { return table_.hasher(); }

  /// Get the key_equal.
  /// @methodset Observers
  key_equal key_eq() const { return table_.keyEqual(); }

  //// PUBLIC - F14 Extensions

  /**
   * Checks for a value using operator==
   * @methodset Lookup
   *
   * containsEqualValue returns true iff there is an element in the map
   * that compares equal to value using operator==.  It is undefined
   * behavior to call this function if operator== on key_type can ever
   * return true when the same keys passed to key_eq() would return false
   * (the opposite is allowed).
   */
  bool containsEqualValue(value_type const& value) const {
    auto it = table_.findMatching(value.first, [&](auto& key) {
      return value.first == key;
    });
    return !it.atEnd() && value.second == table_.valueAtItem(it.citem()).second;
  }

  /// Get memory footprint, not including sizeof(*this).
  /// @methodset Capacity
  ///
  /// Returns the memory allocated by the container itself (hash table
  /// metadata, chunk storage, and for F14NodeMap the per-node allocations).
  /// This does NOT include heap memory owned by the stored keys or values
  /// themselves. For example, if keys or values are std::string with
  /// contents exceeding the SSO buffer, the external character buffers are
  /// not counted. To get a more complete memory footprint, iterate over
  /// the map and account for the dynamic allocations of each key/value.
  std::size_t getAllocatedMemorySize() const {
    return table_.getAllocatedMemorySize();
  }

  /**
   * In-depth memory analysis.
   * @methodset Capacity
   *
   * Enumerates classes of allocated memory blocks currently owned
   * by this table, calling visitor(allocationSize, allocationCount).
   * This can be used to get a more accurate indication of memory footprint
   * than getAllocatedMemorySize() if you have some way of computing the
   * internal fragmentation of the allocator, such as JEMalloc's nallocx.
   * The visitor might be called twice with the same allocationSize. The
   * visitor's computation should produce the same result for visitor(8,
   * 2) as for two calls to visitor(8, 1), for example.  The visitor may
   * be called with a zero allocationCount.
   */
  template <typename V>
  void visitAllocationClasses(V&& visitor) const {
    return table_.visitAllocationClasses(visitor);
  }

  /**
   * Visit contiguous ranges of elements.
   * @methodset Iterators
   *
   * Calls visitor with two value_type const*, b and e, such that every
   * entry in the table is included in exactly one of the ranges [b,e).
   * This can be used to efficiently iterate elements in bulk when crossing
   * an API boundary that supports contiguous blocks of items.
   */
  template <typename V>
  void visitContiguousRanges(V&& visitor) const;

  /// Get stats.
  /// @methodset Hash policy
  F14TableStats computeStats() const noexcept { return table_.computeStats(); }

 private:
  template <typename Self, typename K>
  FOLLY_ALWAYS_INLINE static auto& at(Self& self, K const& key) {
    auto iter = self.find(key);
    if (iter == self.end()) {
      throw_exception<std::out_of_range>("at() did not find key");
    }
    return iter->second;
  }

  template <typename Self, typename K>
  static auto equal_range(Self& self, K const& key) {
    auto first = self.find(key);
    auto last = first;
    if (last != self.end()) {
      ++last;
    }
    return std::make_pair(first, last);
  }

 protected:
  F14Table<Policy> table_;
};
} // namespace detail
} // namespace f14

template <
    typename Key,
    typename Mapped,
    typename Hasher,
    typename KeyEqual,
    typename Alloc>
class F14ValueMap
    : public f14::detail::F14BasicMap<f14::detail::MapPolicyWithDefaults<
          f14::detail::ValueContainerPolicy,
          Key,
          Mapped,
          Hasher,
          KeyEqual,
          Alloc>> {
 protected:
  using Policy = f14::detail::MapPolicyWithDefaults<
      f14::detail::ValueContainerPolicy,
      Key,
      Mapped,
      Hasher,
      KeyEqual,
      Alloc>;

 private:
  using Super = f14::detail::F14BasicMap<Policy>;

 public:
  using typename Super::value_type;

  F14ValueMap() = default;

  using Super::Super;

  F14ValueMap& operator=(std::initializer_list<value_type> ilist) {
    Super::operator=(ilist);
    return *this;
  }

  /// Swaps contained objects with another F14Map
  /// @methodset Modifiers
  void swap(F14ValueMap& rhs) noexcept(Policy::kSwapIsNoexcept) {
    this->table_.swap(rhs.table_);
  }

  /**
   * Visit contiguous ranges of elements.
   * @methodset Iterators
   *
   * Calls visitor with two value_type const*, b and e, such that every
   * entry in the table is included in exactly one of the ranges [b,e).
   * This can be used to efficiently iterate elements in bulk when crossing
   * an API boundary that supports contiguous blocks of items.
   */
  template <typename V>
  void visitContiguousRanges(V&& visitor) const {
    this->table_.visitContiguousItemRanges(visitor);
  }
};
#endif // FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

template <
    typename InputIt,
    typename Hasher = f14::DefaultHasher<iterator_key_type_t<InputIt>>,
    typename KeyEqual = f14::DefaultKeyEqual<iterator_key_type_t<InputIt>>,
    typename Alloc = f14::DefaultAlloc<iterator_value_type_t<InputIt>>,
    typename = detail::RequireInputIterator<InputIt>,
    // Next two constraints are necessary to disambiguate from next constructor
    typename = detail::RequireNotAllocator<Hasher>,
    typename = detail::RequireNotAllocator<KeyEqual>,
    typename = detail::RequireAllocator<Alloc>>
F14ValueMap(
    InputIt, InputIt, std::size_t = {}, Hasher = {}, KeyEqual = {}, Alloc = {})
    -> F14ValueMap<
        iterator_key_type_t<InputIt>,
        iterator_mapped_type_t<InputIt>,
        Hasher,
        KeyEqual,
        Alloc>;

template <
    typename InputIt,
    typename Alloc,
    typename = detail::RequireInputIterator<InputIt>,
    typename = detail::RequireAllocator<Alloc>>
F14ValueMap(InputIt, InputIt, std::size_t, Alloc) -> F14ValueMap<
    iterator_key_type_t<InputIt>,
    iterator_mapped_type_t<InputIt>,
    f14::DefaultHasher<iterator_key_type_t<InputIt>>,
    f14::DefaultKeyEqual<iterator_key_type_t<InputIt>>,
    Alloc>;

template <
    typename InputIt,
    typename Hasher,
    typename Alloc,
    typename = detail::RequireInputIterator<InputIt>,
    typename = detail::RequireNotAllocator<Hasher>,
    typename = detail::RequireAllocator<Alloc>>
F14ValueMap(InputIt, InputIt, std::size_t, Hasher, Alloc) -> F14ValueMap<
    iterator_key_type_t<InputIt>,
    iterator_mapped_type_t<InputIt>,
    Hasher,
    f14::DefaultKeyEqual<iterator_key_type_t<InputIt>>,
    Alloc>;

template <
    typename Key,
    typename Mapped,
    typename Hasher = f14::DefaultHasher<Key>,
    typename KeyEqual = f14::DefaultKeyEqual<Key>,
    typename Alloc = f14::DefaultAlloc<std::pair<const Key, Mapped>>,
    typename = detail::RequireNotAllocator<Hasher>,
    typename = detail::RequireNotAllocator<KeyEqual>,
    typename = detail::RequireAllocator<Alloc>>
F14ValueMap(
    std::initializer_list<std::pair<Key, Mapped>>,
    std::size_t = {},
    Hasher = {},
    KeyEqual = {},
    Alloc = {}) -> F14ValueMap<Key, Mapped, Hasher, KeyEqual, Alloc>;

template <
    typename Key,
    typename Mapped,
    typename Alloc,
    typename = detail::RequireAllocator<Alloc>>
F14ValueMap(std::initializer_list<std::pair<Key, Mapped>>, std::size_t, Alloc)
    -> F14ValueMap<
        Key,
        Mapped,
        f14::DefaultHasher<Key>,
        f14::DefaultKeyEqual<Key>,
        Alloc>;

template <
    typename Key,
    typename Mapped,
    typename Hasher,
    typename Alloc,
    typename = detail::RequireAllocator<Alloc>>
F14ValueMap(
    std::initializer_list<std::pair<Key, Mapped>>, std::size_t, Hasher, Alloc)
    -> F14ValueMap<Key, Mapped, Hasher, f14::DefaultKeyEqual<Key>, Alloc>;

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
template <
    typename Key,
    typename Mapped,
    typename Hasher,
    typename KeyEqual,
    typename Alloc>
class F14NodeMap
    : public f14::detail::F14BasicMap<f14::detail::MapPolicyWithDefaults<
          f14::detail::NodeContainerPolicy,
          Key,
          Mapped,
          Hasher,
          KeyEqual,
          Alloc>> {
 protected:
  using Policy = f14::detail::MapPolicyWithDefaults<
      f14::detail::NodeContainerPolicy,
      Key,
      Mapped,
      Hasher,
      KeyEqual,
      Alloc>;

 private:
  using Super = f14::detail::F14BasicMap<Policy>;

 public:
  using typename Super::value_type;

  F14NodeMap() = default;

  using Super::Super;

  F14NodeMap& operator=(std::initializer_list<value_type> ilist) {
    Super::operator=(ilist);
    return *this;
  }

  /// @methodset Modifiers
  void swap(F14NodeMap& rhs) noexcept(Policy::kSwapIsNoexcept) {
    this->table_.swap(rhs.table_);
  }

  /**
   * Visit contiguous ranges of elements.
   * @methodset Iterators
   *
   * Calls visitor with two value_type const*, b and e, such that every
   * entry in the table is included in exactly one of the ranges [b,e).
   * This can be used to efficiently iterate elements in bulk when crossing
   * an API boundary that supports contiguous blocks of items.
   */
  template <typename V>
  void visitContiguousRanges(V&& visitor) const {
    this->table_.visitItems([&](typename Policy::Item ptr) {
      value_type const* b = std::addressof(*ptr);
      visitor(b, b + 1);
    });
  }

  // TODO extract and node_handle insert
};
#endif // FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

template <
    typename InputIt,
    typename Hasher = f14::DefaultHasher<iterator_key_type_t<InputIt>>,
    typename KeyEqual = f14::DefaultKeyEqual<iterator_key_type_t<InputIt>>,
    typename Alloc = f14::DefaultAlloc<iterator_value_type_t<InputIt>>,
    typename = detail::RequireInputIterator<InputIt>,
    typename = detail::RequireNotAllocator<Hasher>,
    typename = detail::RequireNotAllocator<KeyEqual>,
    typename = detail::RequireAllocator<Alloc>>
F14NodeMap(
    InputIt, InputIt, std::size_t = {}, Hasher = {}, KeyEqual = {}, Alloc = {})
    -> F14NodeMap<
        iterator_key_type_t<InputIt>,
        iterator_mapped_type_t<InputIt>,
        Hasher,
        KeyEqual,
        Alloc>;

template <
    typename InputIt,
    typename Alloc,
    typename = detail::RequireInputIterator<InputIt>,
    typename = detail::RequireAllocator<Alloc>>
F14NodeMap(InputIt, InputIt, std::size_t, Alloc) -> F14NodeMap<
    iterator_key_type_t<InputIt>,
    iterator_mapped_type_t<InputIt>,
    f14::DefaultHasher<iterator_key_type_t<InputIt>>,
    f14::DefaultKeyEqual<iterator_key_type_t<InputIt>>,
    Alloc>;

template <
    typename InputIt,
    typename Hasher,
    typename Alloc,
    typename = detail::RequireInputIterator<InputIt>,
    typename = detail::RequireNotAllocator<Hasher>,
    typename = detail::RequireAllocator<Alloc>>
F14NodeMap(InputIt, InputIt, std::size_t, Hasher, Alloc) -> F14NodeMap<
    iterator_key_type_t<InputIt>,
    iterator_mapped_type_t<InputIt>,
    Hasher,
    f14::DefaultKeyEqual<iterator_key_type_t<InputIt>>,
    Alloc>;

template <
    typename Key,
    typename Mapped,
    typename Hasher = f14::DefaultHasher<Key>,
    typename KeyEqual = f14::DefaultKeyEqual<Key>,
    typename Alloc = f14::DefaultAlloc<std::pair<const Key, Mapped>>,
    typename = detail::RequireNotAllocator<Hasher>,
    typename = detail::RequireNotAllocator<KeyEqual>,
    typename = detail::RequireAllocator<Alloc>>
F14NodeMap(
    std::initializer_list<std::pair<Key, Mapped>>,
    std::size_t = {},
    Hasher = {},
    KeyEqual = {},
    Alloc = {}) -> F14NodeMap<Key, Mapped, Hasher, KeyEqual, Alloc>;

template <
    typename Key,
    typename Mapped,
    typename Alloc,
    typename = detail::RequireAllocator<Alloc>>
F14NodeMap(std::initializer_list<std::pair<Key, Mapped>>, std::size_t, Alloc)
    -> F14NodeMap<
        Key,
        Mapped,
        f14::DefaultHasher<Key>,
        f14::DefaultKeyEqual<Key>,
        Alloc>;

template <
    typename Key,
    typename Mapped,
    typename Hasher,
    typename Alloc,
    typename = detail::RequireAllocator<Alloc>>
F14NodeMap(
    std::initializer_list<std::pair<Key, Mapped>>, std::size_t, Hasher, Alloc)
    -> F14NodeMap<Key, Mapped, Hasher, f14::DefaultKeyEqual<Key>, Alloc>;

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
namespace f14 {
namespace detail {
template <
    typename Key,
    typename Mapped,
    typename Hasher,
    typename KeyEqual,
    typename Alloc,
    typename EligibleForPerturbedInsertionOrder>
class F14VectorMapImpl
    : public F14BasicMap<MapPolicyWithDefaults<
          VectorContainerPolicy,
          Key,
          Mapped,
          Hasher,
          KeyEqual,
          Alloc,
          EligibleForPerturbedInsertionOrder>> {
 protected:
  using Policy = MapPolicyWithDefaults<
      VectorContainerPolicy,
      Key,
      Mapped,
      Hasher,
      KeyEqual,
      Alloc,
      EligibleForPerturbedInsertionOrder>;

 private:
  using Super = F14BasicMap<Policy>;

  template <typename K>
  using IsIter = std::disjunction<
      std::is_same<typename Policy::Iter, std::remove_cvref_t<K>>,
      std::is_same<typename Policy::ConstIter, std::remove_cvref_t<K>>,
      std::is_same<typename Policy::ReverseIter, std::remove_cvref_t<K>>,
      std::is_same<typename Policy::ConstReverseIter, std::remove_cvref_t<K>>>;

  template <typename K>
  static constexpr bool kEligibleForHeterogeneousVectorErase =
      ::folly::detail::EligibleForHeterogeneousFind<
          Key,
          Hasher,
          KeyEqual,
          std::conditional_t<IsIter<K>::value, Key, K>>::value &&
      !IsIter<K>::value;

 public:
  using typename Super::const_iterator;
  using typename Super::iterator;
  using typename Super::key_type;
  using typename Super::mapped_type;
  using typename Super::value_type;

  F14VectorMapImpl() = default;

  // inherit constructors
  using Super::Super;

  F14VectorMapImpl& operator=(std::initializer_list<value_type> ilist) {
    Super::operator=(ilist);
    return *this;
  }

  /// @methodset Iterators
  iterator begin() [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return this->table_.linearBegin(this->size());
  }
  /// @methodset Iterators
  const_iterator begin() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return cbegin();
  }
  /// @methodset Iterators
  const_iterator cbegin() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return this->table_.linearBegin(this->size());
  }

  /// @methodset Iterators
  iterator end() [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return this->table_.linearEnd();
  }
  /// @methodset Iterators
  const_iterator end() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return cend();
  }
  /// @methodset Iterators
  const_iterator cend() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return this->table_.linearEnd();
  }

 private:
  template <typename BeforeDestroy>
  void eraseUnderlying(
      typename Policy::ItemIter underlying, BeforeDestroy&& beforeDestroy) {
    Alloc& a = this->table_.alloc();
    auto values = this->table_.values_;

    // Remove the ptr from the base table and destroy the value.
    auto index = underlying.item();
    // The item still needs to be hashable during this call, so we must destroy
    // the value _afterwards_.
    this->tableEraseIterInto(underlying, beforeDestroy);
    Policy::AllocTraits::destroy(a, std::addressof(values[index]));

    // move the last element in values_ down and fix up the inbound index
    auto tailIndex = this->size();
    if (tailIndex != index) {
      auto tail = this->table_.find(
          VectorContainerIndexSearch{static_cast<uint32_t>(tailIndex)});
      tail.item() = index;
      auto p = std::addressof(values[index]);
      assume(p != nullptr);
      this->table_.transfer(a, std::addressof(values[tailIndex]), p, 1);
    }
  }

  template <typename K, typename BeforeDestroy>
  std::size_t eraseUnderlyingKey(K const& key, BeforeDestroy&& beforeDestroy) {
    auto underlying = this->table_.find(key);
    if (underlying.atEnd()) {
      return 0;
    } else {
      eraseUnderlying(underlying, beforeDestroy);
      return 1;
    }
  }

 public:
  /**
   * Remove element at a specific position (iterator).
   * @overloadbrief Remove elements.
   * @methodset Modifiers
   */
  FOLLY_ALWAYS_INLINE iterator erase(const_iterator pos) {
    return eraseInto(pos, variadic_noop);
  }

  // This form avoids ambiguity when key_type has a templated constructor
  // that accepts const_iterator
  FOLLY_ALWAYS_INLINE iterator erase(iterator pos) {
    return eraseInto(pos, variadic_noop);
  }

  /// Remove a range of elements.
  iterator erase(const_iterator first, const_iterator last) {
    return eraseInto(first, last, variadic_noop);
  }

  /// Remove a specific key.
  std::size_t erase(key_type const& key) {
    return eraseInto(key, variadic_noop);
  }

  /// Remove a key, using a heterogeneous representation.
  template <typename K>
    requires kEligibleForHeterogeneousVectorErase<K>
  std::size_t erase(K const& key) {
    return eraseInto(key, variadic_noop);
  }

  /**
   * Callback-erase a single iterator.
   * @overloadbrief Erase with pre-destruction callback.
   * @methodset Modifiers
   *
   * Like erase, but with an additional callback argument which is called with
   * an rvalue reference to the item directly before it is destroyed. This can
   * be used to extract an item out of a F14Set while avoiding a copy.
   */
  template <typename BeforeDestroy>
  FOLLY_ALWAYS_INLINE iterator
  eraseInto(const_iterator pos, BeforeDestroy&& beforeDestroy) {
    FOLLY_SAFE_DCHECK(pos != cend(), "erase() of a past-the-end iterator");
    auto index = this->table_.iterToIndex(pos);
    auto underlying = this->table_.find(VectorContainerIndexSearch{index});
    eraseUnderlying(underlying, beforeDestroy);
    return std::next(this->table_.indexToIter(index));
  }

  // This form avoids ambiguity when key_type has a templated constructor
  // that accepts const_iterator
  template <typename BeforeDestroy>
  FOLLY_ALWAYS_INLINE iterator
  eraseInto(iterator pos, BeforeDestroy&& beforeDestroy) {
    const_iterator cpos{pos};
    return eraseInto(cpos, beforeDestroy);
  }

  /// Callback-erase a range of values.
  template <typename BeforeDestroy>
  iterator eraseInto(
      const_iterator first,
      const_iterator last,
      BeforeDestroy&& beforeDestroy) {
    while (first != last) {
      first = eraseInto(first, beforeDestroy);
    }
    return begin() + (first - cbegin());
  }

  /// Callback-erase a specific key.
  template <typename BeforeDestroy>
  std::size_t eraseInto(key_type const& key, BeforeDestroy&& beforeDestroy) {
    return eraseUnderlyingKey(key, beforeDestroy);
  }

  /// Callback-erase a specific key, using a heterogeneous representation.
  template <typename K, typename BeforeDestroy>
    requires kEligibleForHeterogeneousVectorErase<K>
  std::size_t eraseInto(K const& key, BeforeDestroy&& beforeDestroy) {
    return eraseUnderlyingKey(key, beforeDestroy);
  }

  /**
   * Visit contiguous ranges of elements.
   * @methodset Iterators
   *
   * Calls visitor with two value_type const*, b and e, such that every
   * entry in the table is included in exactly one of the ranges [b,e).
   * This can be used to efficiently iterate elements in bulk when crossing
   * an API boundary that supports contiguous blocks of items.
   */
  template <typename V>
  void visitContiguousRanges(V&& visitor) const {
    auto n = this->table_.size();
    if (n > 0) {
      value_type const* b = std::addressof(this->table_.values_[0]);
      visitor(b, b + n);
    }
  }
};
} // namespace detail
} // namespace f14

template <
    typename Key,
    typename Mapped,
    typename Hasher,
    typename KeyEqual,
    typename Alloc>
class F14VectorMap
    : public f14::detail::F14VectorMapImpl<
          Key,
          Mapped,
          Hasher,
          KeyEqual,
          Alloc,
          std::false_type> {
  using Super = f14::detail::
      F14VectorMapImpl<Key, Mapped, Hasher, KeyEqual, Alloc, std::false_type>;

 public:
  using typename Super::const_iterator;
  using typename Super::iterator;
  using typename Super::value_type;
  using reverse_iterator = typename Super::Policy::ReverseIter;
  using const_reverse_iterator = typename Super::Policy::ConstReverseIter;

  F14VectorMap() = default;

  // inherit constructors
  using Super::Super;

  F14VectorMap& operator=(std::initializer_list<value_type> ilist) {
    Super::operator=(ilist);
    return *this;
  }

  /// @methodset Modifiers
  void swap(F14VectorMap& rhs) noexcept(Super::Policy::kSwapIsNoexcept) {
    this->table_.swap(rhs.table_);
  }

  // ITERATION ORDER
  //
  // Deterministic iteration order for insert-only workloads is part of
  // F14VectorMap's supported API: iterator is LIFO and reverse_iterator
  // is FIFO.
  //
  // If there have been no calls to erase() then iterator and
  // const_iterator enumerate entries in the opposite of insertion order.
  // begin()->first is the key most recently inserted.  reverse_iterator
  // and reverse_const_iterator, therefore, enumerate in LIFO (insertion)
  // order for insert-only workloads.  Deterministic iteration order is
  // only guaranteed if no keys were removed since the last time the
  // map was empty.  Iteration order is preserved across rehashes and
  // F14VectorMap copies and moves.
  //
  // iterator uses LIFO order so that erasing while iterating with begin()
  // and end() is safe using the erase(it++) idiom, which is supported
  // by std::map and std::unordered_map.  erase(iter) invalidates iter
  // and all iterators before iter in the non-reverse iteration order.
  // Every successful erase invalidates all reverse iterators.
  //
  // No erase is provided for reverse_iterator or const_reverse_iterator
  // to make it harder to shoot yourself in the foot by erasing while
  // reverse-iterating.  You can write that as map.erase(riter->first)
  // if you really need it; erase(iter) hashes the key too.

  /// @methodset Iterators
  reverse_iterator rbegin() [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return reverse_iterator{this->table_.values_};
  }
  /// @methodset Iterators
  const_reverse_iterator rbegin() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return crbegin();
  }
  /// @methodset Iterators
  const_reverse_iterator crbegin() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return const_reverse_iterator{this->table_.values_};
  }

  /// @methodset Iterators
  reverse_iterator rend() [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return reverse_iterator{this->table_.values_ + this->table_.size()};
  }
  /// @methodset Iterators
  const_reverse_iterator rend() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return crend();
  }
  /// @methodset Iterators
  const_reverse_iterator crend() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return const_reverse_iterator{this->table_.values_ + this->table_.size()};
  }

  /**
   * Pointer to the elements in storage order, like `std::vector::data()`.
   * @methodset Iterators
   *
   * Storage order is the reverse of iteration order (the [rbegin, rend)
   * range) and is otherwise unspecified; erase() moves the last element
   * into the erased slot. Invalidated by any mutation, like iterators.
   */
  value_type* data() noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return std::to_address(this->table_.values_);
  }
  /// @methodset Iterators
  value_type const* data() const noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return std::to_address(this->table_.values_);
  }

  /**
   * Contiguous view of all elements in storage order, i.e. `{data(),
   * size()}`.
   * @methodset Iterators
   */
  std::span<value_type> as_span() noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return {this->data(), this->size()};
  }
  /// @methodset Iterators
  std::span<value_type const> as_span() const noexcept
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return {this->data(), this->size()};
  }

  friend Range<const_reverse_iterator> tag_invoke(
      order_preserving_reinsertion_view_fn, F14VectorMap const& c) noexcept {
    return {c.rbegin(), c.rend()};
  }
};
#endif // FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

template <
    typename InputIt,
    typename Hasher = f14::DefaultHasher<iterator_key_type_t<InputIt>>,
    typename KeyEqual = f14::DefaultKeyEqual<iterator_key_type_t<InputIt>>,
    typename Alloc = f14::DefaultAlloc<iterator_value_type_t<InputIt>>,
    typename = detail::RequireInputIterator<InputIt>,
    typename = detail::RequireNotAllocator<Hasher>,
    typename = detail::RequireNotAllocator<KeyEqual>,
    typename = detail::RequireAllocator<Alloc>>
F14VectorMap(
    InputIt, InputIt, std::size_t = {}, Hasher = {}, KeyEqual = {}, Alloc = {})
    -> F14VectorMap<
        iterator_key_type_t<InputIt>,
        iterator_mapped_type_t<InputIt>,
        Hasher,
        KeyEqual,
        Alloc>;

template <
    typename InputIt,
    typename Alloc,
    typename = detail::RequireInputIterator<InputIt>,
    typename = detail::RequireAllocator<Alloc>>
F14VectorMap(InputIt, InputIt, std::size_t, Alloc) -> F14VectorMap<
    iterator_key_type_t<InputIt>,
    iterator_mapped_type_t<InputIt>,
    f14::DefaultHasher<iterator_key_type_t<InputIt>>,
    f14::DefaultKeyEqual<iterator_key_type_t<InputIt>>,
    Alloc>;

template <
    typename InputIt,
    typename Hasher,
    typename Alloc,
    typename = detail::RequireInputIterator<InputIt>,
    typename = detail::RequireNotAllocator<Hasher>,
    typename = detail::RequireAllocator<Alloc>>
F14VectorMap(InputIt, InputIt, std::size_t, Hasher, Alloc) -> F14VectorMap<
    iterator_key_type_t<InputIt>,
    iterator_mapped_type_t<InputIt>,
    Hasher,
    f14::DefaultKeyEqual<iterator_key_type_t<InputIt>>,
    Alloc>;

template <
    typename Key,
    typename Mapped,
    typename Hasher = f14::DefaultHasher<Key>,
    typename KeyEqual = f14::DefaultKeyEqual<Key>,
    typename Alloc = f14::DefaultAlloc<std::pair<const Key, Mapped>>,
    typename = detail::RequireNotAllocator<Hasher>,
    typename = detail::RequireNotAllocator<KeyEqual>,
    typename = detail::RequireAllocator<Alloc>>
F14VectorMap(
    std::initializer_list<std::pair<Key, Mapped>>,
    std::size_t = {},
    Hasher = {},
    KeyEqual = {},
    Alloc = {}) -> F14VectorMap<Key, Mapped, Hasher, KeyEqual, Alloc>;

template <
    typename Key,
    typename Mapped,
    typename Alloc,
    typename = detail::RequireAllocator<Alloc>>
F14VectorMap(std::initializer_list<std::pair<Key, Mapped>>, std::size_t, Alloc)
    -> F14VectorMap<
        Key,
        Mapped,
        f14::DefaultHasher<Key>,
        f14::DefaultKeyEqual<Key>,
        Alloc>;

template <
    typename Key,
    typename Mapped,
    typename Hasher,
    typename Alloc,
    typename = detail::RequireAllocator<Alloc>>
F14VectorMap(
    std::initializer_list<std::pair<Key, Mapped>>, std::size_t, Hasher, Alloc)
    -> F14VectorMap<Key, Mapped, Hasher, f14::DefaultKeyEqual<Key>, Alloc>;

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
/**
 * F14FastMap is, under the hood, either an F14ValueMap or an F14VectorMap.
 * F14FastMap chooses which of these two representations to use based on the
 * size of a node.
 */
template <
    typename Key,
    typename Mapped,
    typename Hasher,
    typename KeyEqual,
    typename Alloc>
class F14FastMap
    : public std::conditional_t<
          (sizeof(std::pair<Key const, Mapped>) < 24),
          F14ValueMap<Key, Mapped, Hasher, KeyEqual, Alloc>,
          f14::detail::F14VectorMapImpl<
              Key,
              Mapped,
              Hasher,
              KeyEqual,
              Alloc,
              std::true_type>> {
  using Super = std::conditional_t<
      sizeof(std::pair<Key const, Mapped>) < 24,
      F14ValueMap<Key, Mapped, Hasher, KeyEqual, Alloc>,
      f14::detail::F14VectorMapImpl<
          Key,
          Mapped,
          Hasher,
          KeyEqual,
          Alloc,
          std::true_type>>;

 public:
  using typename Super::value_type;

  F14FastMap() = default;

  using Super::Super;

  F14FastMap& operator=(std::initializer_list<value_type> ilist) {
    Super::operator=(ilist);
    return *this;
  }

  /// @methodset Modifiers
  void swap(F14FastMap& rhs) noexcept(Super::Policy::kSwapIsNoexcept) {
    this->table_.swap(rhs.table_);
  }
};
#endif // if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

template <
    typename InputIt,
    typename Hasher = f14::DefaultHasher<iterator_key_type_t<InputIt>>,
    typename KeyEqual = f14::DefaultKeyEqual<iterator_key_type_t<InputIt>>,
    typename Alloc = f14::DefaultAlloc<iterator_value_type_t<InputIt>>,
    typename = detail::RequireInputIterator<InputIt>,
    typename = detail::RequireNotAllocator<Hasher>,
    typename = detail::RequireNotAllocator<KeyEqual>,
    typename = detail::RequireAllocator<Alloc>>
F14FastMap(
    InputIt, InputIt, std::size_t = {}, Hasher = {}, KeyEqual = {}, Alloc = {})
    -> F14FastMap<
        iterator_key_type_t<InputIt>,
        iterator_mapped_type_t<InputIt>,
        Hasher,
        KeyEqual,
        Alloc>;

template <
    typename InputIt,
    typename Alloc,
    typename = detail::RequireInputIterator<InputIt>,
    typename = detail::RequireAllocator<Alloc>>
F14FastMap(InputIt, InputIt, std::size_t, Alloc) -> F14FastMap<
    iterator_key_type_t<InputIt>,
    iterator_mapped_type_t<InputIt>,
    f14::DefaultHasher<iterator_key_type_t<InputIt>>,
    f14::DefaultKeyEqual<iterator_key_type_t<InputIt>>,
    Alloc>;

template <
    typename InputIt,
    typename Hasher,
    typename Alloc,
    typename = detail::RequireInputIterator<InputIt>,
    typename = detail::RequireNotAllocator<Hasher>,
    typename = detail::RequireAllocator<Alloc>>
F14FastMap(InputIt, InputIt, std::size_t, Hasher, Alloc) -> F14FastMap<
    iterator_key_type_t<InputIt>,
    iterator_mapped_type_t<InputIt>,
    Hasher,
    f14::DefaultKeyEqual<iterator_key_type_t<InputIt>>,
    Alloc>;

template <
    typename Key,
    typename Mapped,
    typename Hasher = f14::DefaultHasher<Key>,
    typename KeyEqual = f14::DefaultKeyEqual<Key>,
    typename Alloc = f14::DefaultAlloc<std::pair<const Key, Mapped>>,
    typename = detail::RequireNotAllocator<Hasher>,
    typename = detail::RequireNotAllocator<KeyEqual>,
    typename = detail::RequireAllocator<Alloc>>
F14FastMap(
    std::initializer_list<std::pair<Key, Mapped>>,
    std::size_t = {},
    Hasher = {},
    KeyEqual = {},
    Alloc = {}) -> F14FastMap<Key, Mapped, Hasher, KeyEqual, Alloc>;

template <
    typename Key,
    typename Mapped,
    typename Alloc,
    typename = detail::RequireAllocator<Alloc>>
F14FastMap(std::initializer_list<std::pair<Key, Mapped>>, std::size_t, Alloc)
    -> F14FastMap<
        Key,
        Mapped,
        f14::DefaultHasher<Key>,
        f14::DefaultKeyEqual<Key>,
        Alloc>;

template <
    typename Key,
    typename Mapped,
    typename Hasher,
    typename Alloc,
    typename = detail::RequireAllocator<Alloc>>
F14FastMap(
    std::initializer_list<std::pair<Key, Mapped>>, std::size_t, Hasher, Alloc)
    -> F14FastMap<Key, Mapped, Hasher, f14::DefaultKeyEqual<Key>, Alloc>;

} // namespace folly

namespace folly {
namespace f14 {
namespace detail {
template <typename M>
bool mapsEqual(M const& lhs, M const& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (auto& kv : lhs) {
    if (!rhs.containsEqualValue(kv)) {
      return false;
    }
  }
  return true;
}
} // namespace detail
} // namespace f14

template <typename K, typename M, typename H, typename E, typename A>
bool operator==(
    F14ValueMap<K, M, H, E, A> const& lhs,
    F14ValueMap<K, M, H, E, A> const& rhs) {
  return mapsEqual(lhs, rhs);
}

template <typename K, typename M, typename H, typename E, typename A>
bool operator!=(
    F14ValueMap<K, M, H, E, A> const& lhs,
    F14ValueMap<K, M, H, E, A> const& rhs) {
  return !(lhs == rhs);
}

template <typename K, typename M, typename H, typename E, typename A>
bool operator==(
    F14NodeMap<K, M, H, E, A> const& lhs,
    F14NodeMap<K, M, H, E, A> const& rhs) {
  return mapsEqual(lhs, rhs);
}

template <typename K, typename M, typename H, typename E, typename A>
bool operator!=(
    F14NodeMap<K, M, H, E, A> const& lhs,
    F14NodeMap<K, M, H, E, A> const& rhs) {
  return !(lhs == rhs);
}

template <typename K, typename M, typename H, typename E, typename A>
bool operator==(
    F14VectorMap<K, M, H, E, A> const& lhs,
    F14VectorMap<K, M, H, E, A> const& rhs) {
  return mapsEqual(lhs, rhs);
}

template <typename K, typename M, typename H, typename E, typename A>
bool operator!=(
    F14VectorMap<K, M, H, E, A> const& lhs,
    F14VectorMap<K, M, H, E, A> const& rhs) {
  return !(lhs == rhs);
}

template <typename K, typename M, typename H, typename E, typename A>
bool operator==(
    F14FastMap<K, M, H, E, A> const& lhs,
    F14FastMap<K, M, H, E, A> const& rhs) {
  return mapsEqual(lhs, rhs);
}

template <typename K, typename M, typename H, typename E, typename A>
bool operator!=(
    F14FastMap<K, M, H, E, A> const& lhs,
    F14FastMap<K, M, H, E, A> const& rhs) {
  return !(lhs == rhs);
}

template <typename K, typename M, typename H, typename E, typename A>
void swap(
    F14ValueMap<K, M, H, E, A>& lhs,
    F14ValueMap<K, M, H, E, A>& rhs) noexcept(noexcept(lhs.swap(rhs))) {
  lhs.swap(rhs);
}

template <typename K, typename M, typename H, typename E, typename A>
void swap(
    F14NodeMap<K, M, H, E, A>& lhs,
    F14NodeMap<K, M, H, E, A>& rhs) noexcept(noexcept(lhs.swap(rhs))) {
  lhs.swap(rhs);
}

template <typename K, typename M, typename H, typename E, typename A>
void swap(
    F14VectorMap<K, M, H, E, A>& lhs,
    F14VectorMap<K, M, H, E, A>& rhs) noexcept(noexcept(lhs.swap(rhs))) {
  lhs.swap(rhs);
}

template <typename K, typename M, typename H, typename E, typename A>
void swap(
    F14FastMap<K, M, H, E, A>& lhs,
    F14FastMap<K, M, H, E, A>& rhs) noexcept(noexcept(lhs.swap(rhs))) {
  lhs.swap(rhs);
}

template <
    typename K,
    typename M,
    typename H,
    typename E,
    typename A,
    typename Pred>
std::size_t erase_if(F14ValueMap<K, M, H, E, A>& c, Pred pred) {
  return f14::detail::erase_if_impl(c, pred);
}

template <
    typename K,
    typename M,
    typename H,
    typename E,
    typename A,
    typename Pred>
std::size_t erase_if(F14NodeMap<K, M, H, E, A>& c, Pred pred) {
  return f14::detail::erase_if_impl(c, pred);
}

template <
    typename K,
    typename M,
    typename H,
    typename E,
    typename A,
    typename Pred>
std::size_t erase_if(F14VectorMap<K, M, H, E, A>& c, Pred pred) {
  return f14::detail::erase_if_impl(c, pred);
}

template <
    typename K,
    typename M,
    typename H,
    typename E,
    typename A,
    typename Pred>
std::size_t erase_if(F14FastMap<K, M, H, E, A>& c, Pred pred) {
  return f14::detail::erase_if_impl(c, pred);
}

} // namespace folly
