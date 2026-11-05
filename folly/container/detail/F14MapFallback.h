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
#include <type_traits>
#include <unordered_map>

#include <folly/Optional.h>
#include <folly/lang/Assume.h>

#include <folly/container/detail/F14Table.h>
#include <folly/container/detail/Util.h>

/**
 * This file is intended to be included only by F14Map.h. It contains fallback
 * implementations of F14Map types for platforms that do not support the
 * required SIMD instructions, based on std::unordered_map.
 */

#if !FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

namespace folly {

namespace f14 {
namespace detail {
template <typename K, typename M, typename H, typename E, typename A>
class F14BasicMap : public std::unordered_map<K, M, H, E, A> {
  using Super = std::unordered_map<K, M, H, E, A>;

  template <typename K2>
  static constexpr bool kEligibleForHeterogeneousFind =
      ::folly::detail::EligibleForHeterogeneousFind<K, H, E, K2>::value;

  template <typename K2>
  static constexpr bool kEligibleForHeterogeneousInsert =
      ::folly::detail::EligibleForHeterogeneousInsert<K, H, E, K2>::value;

  template <typename K2>
  using IsIter = std::disjunction<
      std::is_same<typename Super::iterator, std::remove_cvref_t<K2>>,
      std::is_same<typename Super::const_iterator, std::remove_cvref_t<K2>>>;

  template <typename K2>
  static constexpr bool kEligibleForHeterogeneousErase =
      ::folly::detail::EligibleForHeterogeneousFind<
          K,
          H,
          E,
          std::conditional_t<IsIter<K2>::value, K, K2>>::value &&
      !IsIter<K2>::value;

 public:
  using typename Super::const_iterator;
  using typename Super::hasher;
  using typename Super::iterator;
  using typename Super::key_equal;
  using typename Super::key_type;
  using typename Super::mapped_type;
  using typename Super::pointer;
  using typename Super::size_type;
  using typename Super::value_type;

  F14BasicMap() = default;

  using Super::Super;

  //// PUBLIC - Modifiers

  std::pair<iterator, bool> insert(value_type const& value) {
    return emplace(value);
  }

  template <typename P>
    requires std::is_constructible_v<value_type, P&&>
  std::pair<iterator, bool> insert(P&& value) {
    return emplace(std::forward<P>(value));
  }

  // TODO(T31574848): Work around libstdc++ versions (e.g., GCC < 6) with no
  // implementation of N4387 ("perfect initialization" for pairs and tuples).
  template <typename U1, typename U2>
    requires(
        std::is_constructible_v<key_type, U1 const&> &&
        std::is_constructible_v<mapped_type, U2 const&>)
  std::pair<iterator, bool> insert(std::pair<U1, U2> const& value) {
    return emplace(value);
  }

  // TODO(T31574848)
  template <typename U1, typename U2>
    requires(
        std::is_constructible_v<key_type, U1 &&> &&
        std::is_constructible_v<mapped_type, U2 &&>)
  std::pair<iterator, bool> insert(std::pair<U1, U2>&& value) {
    return emplace(std::move(value));
  }

  std::pair<iterator, bool> insert(value_type&& value) {
    return emplace(std::move(value));
  }

  iterator insert(const_iterator /*hint*/, value_type const& value) {
    return insert(value).first;
  }

  template <typename P>
    requires std::is_constructible_v<value_type, P&&>
  iterator insert(const_iterator /*hint*/, P&& value) {
    return insert(std::forward<P>(value)).first;
  }

  iterator insert(const_iterator /*hint*/, value_type&& value) {
    return insert(std::move(value)).first;
  }

  template <class... Args>
  iterator emplace_hint(const_iterator /*hint*/, Args&&... args) {
    return emplace(std::forward<Args>(args)...).first;
  }

  template <class InputIt>
  void insert(InputIt first, InputIt last) {
    while (first != last) {
      insert(*first);
      ++first;
    }
  }

#if !defined(__cpp_lib_containers_ranges) || \
    __cpp_lib_containers_ranges < 202202L
  template <std::ranges::input_range R>
    requires std::convertible_to<std::ranges::range_reference_t<R>, value_type>
  void insert_range(R&& range) {
    insert(std::ranges::begin(range), std::ranges::end(range));
  }
#endif

  void insert(std::initializer_list<value_type> ilist) {
    insert(ilist.begin(), ilist.end());
  }

  template <typename M2>
  std::pair<iterator, bool> insert_or_assign(key_type const& key, M2&& obj) {
    auto rv = try_emplace(key, std::forward<M2>(obj));
    if (!rv.second) {
      rv.first->second = std::forward<M2>(obj);
    }
    return rv;
  }

  template <typename M2>
  std::pair<iterator, bool> insert_or_assign(key_type&& key, M2&& obj) {
    auto rv = try_emplace(std::move(key), std::forward<M2>(obj));
    if (!rv.second) {
      rv.first->second = std::forward<M2>(obj);
    }
    return rv;
  }

  template <typename M2>
  iterator insert_or_assign(
      const_iterator /*hint*/, key_type const& key, M2&& obj) {
    return insert_or_assign(key, std::forward<M2>(obj)).first;
  }

  template <typename M2>
  iterator insert_or_assign(const_iterator /*hint*/, key_type&& key, M2&& obj) {
    return insert_or_assign(std::move(key), std::forward<M2>(obj)).first;
  }

  template <typename K2, typename M2>
    requires kEligibleForHeterogeneousInsert<K2>
  std::pair<iterator, bool> insert_or_assign(K2&& key, M2&& obj) {
    auto rv = try_emplace(std::forward<K2>(key), std::forward<M2>(obj));
    if (!rv.second) {
      rv.first->second = std::forward<M2>(obj);
    }
    return rv;
  }

 private:
  template <typename Arg>
  using UsableAsKey = ::folly::detail::
      EligibleForHeterogeneousFind<key_type, hasher, key_equal, Arg>;

 public:
  template <typename... Args>
  std::pair<iterator, bool> emplace(Args&&... args) {
    auto alloc = this->get_allocator();
    return folly::detail::
        callWithExtractedKey<key_type, mapped_type, UsableAsKey>(
            alloc,
            [&](auto& key, auto&&... inner) {
              auto it = find(key);
              if (it != this->end()) {
                return std::make_pair(it, false);
              }
              auto rv = Super::emplace(std::forward<decltype(inner)>(inner)...);
              FOLLY_SAFE_DCHECK(
                  rv.second, "post-find emplace should always insert");
              return rv;
            },
            std::forward<Args>(args)...);
  }

  template <typename... Args>
  std::pair<iterator, bool> try_emplace(key_type const& key, Args&&... args) {
    return emplace(
        std::piecewise_construct,
        std::forward_as_tuple(key),
        std::forward_as_tuple(std::forward<Args>(args)...));
  }

  template <typename... Args>
  std::pair<iterator, bool> try_emplace(key_type&& key, Args&&... args) {
    return emplace(
        std::piecewise_construct,
        std::forward_as_tuple(std::move(key)),
        std::forward_as_tuple(std::forward<Args>(args)...));
  }

  template <typename... Args>
  iterator try_emplace(
      const_iterator /*hint*/, key_type const& key, Args&&... args) {
    return emplace(
               std::piecewise_construct,
               std::forward_as_tuple(key),
               std::forward_as_tuple(std::forward<Args>(args)...))
        .first;
  }

  template <typename... Args>
  iterator try_emplace(
      const_iterator /*hint*/, key_type&& key, Args&&... args) {
    return emplace(
               std::piecewise_construct,
               std::forward_as_tuple(std::move(key)),
               std::forward_as_tuple(std::forward<Args>(args)...))
        .first;
  }

  template <typename K2, typename... Args>
    requires kEligibleForHeterogeneousInsert<K2>
  std::pair<iterator, bool> try_emplace(K2&& key, Args&&... args) {
    return emplace(
        std::piecewise_construct,
        std::forward_as_tuple(std::forward<K2>(key)),
        std::forward_as_tuple(std::forward<Args>(args)...));
  }

  using Super::erase;

  template <typename K2>
    requires kEligibleForHeterogeneousErase<K2>
  size_type erase(K2 const& key) {
    auto it = find(key);
    if (it != this->end()) {
      erase(it);
      return 1;
    } else {
      return 0;
    }
  }

  //// PUBLIC - Lookup

 private:
  template <typename K2>
  struct BottomKeyEqual {
    [[noreturn]] bool operator()(K2 const&, K2 const&) const {
      assume_unreachable();
    }
  };

  template <typename Self, typename K2>
  static auto findLocal(Self& self, K2 const& key)
      -> folly::Optional<decltype(self.begin(0))> {
    if (self.empty()) {
      return none;
    }
    using A2 = typename std::allocator_traits<A>::template rebind_alloc<
        std::pair<K2 const, M>>;
    using E2 = BottomKeyEqual<K2>;
    // this is exceedingly wicked!
    auto slot =
        reinterpret_cast<std::unordered_map<K2, M, H, E2, A2> const&>(self)
            .bucket(key);
    auto b = self.begin(slot);
    auto e = self.end(slot);
    while (b != e) {
      if (self.key_eq()(key, b->first)) {
        return b;
      }
      ++b;
    }
    FOLLY_SAFE_DCHECK(
        self.size() > 3 ||
            std::none_of(
                self.begin(),
                self.end(),
                [&](auto const& kv) { return self.key_eq()(key, kv.first); }),
        "");
    return none;
  }

  template <typename Self, typename K2>
  static auto& atImpl(Self& self, K2 const& key) {
    auto it = findLocal(self, key);
    if (!it) {
      throw_exception<std::out_of_range>("at() did not find key");
    }
    return (*it)->second;
  }

 public:
  mapped_type& at(key_type const& key) { return Super::at(key); }

  mapped_type const& at(key_type const& key) const { return Super::at(key); }

  template <typename K2>
    requires kEligibleForHeterogeneousFind<K2>
  mapped_type& at(K2 const& key) {
    return atImpl(*this, key);
  }

  template <typename K2>
    requires kEligibleForHeterogeneousFind<K2>
  mapped_type const& at(K2 const& key) const {
    return atImpl(*this, key);
  }

  using Super::operator[];

  template <typename K2>
    requires kEligibleForHeterogeneousInsert<K2>
  mapped_type& operator[](K2&& key) {
    return try_emplace(std::forward<K2>(key)).first->second;
  }

  size_type count(key_type const& key) const { return Super::count(key); }

  template <typename K2>
    requires kEligibleForHeterogeneousFind<K2>
  size_type count(K2 const& key) const {
    return !findLocal(*this, key) ? 0 : 1;
  }

  bool contains(key_type const& key) const { return count(key) != 0; }

  template <typename K2>
    requires kEligibleForHeterogeneousFind<K2>
  bool contains(K2 const& key) const {
    return count(key) != 0;
  }

 private:
  template <typename Iter, typename LocalIter>
  static std::
      enable_if_t<std::is_constructible<Iter, LocalIter const&>::value, Iter>
      fromLocal(LocalIter const& src, int = 0) {
    return Iter(src);
  }

  template <typename Iter, typename LocalIter>
  static std::
      enable_if_t<!std::is_constructible<Iter, LocalIter const&>::value, Iter>
      fromLocal(LocalIter const& src) {
    Iter dst;
    static_assert(sizeof(dst) <= sizeof(src));
    std::memcpy(std::addressof(dst), std::addressof(src), sizeof(dst));
    FOLLY_SAFE_CHECK(
        std::addressof(*src) == std::addressof(*dst),
        "ABI-assuming local_iterator to iterator conversion failed");
    return dst;
  }

  template <typename Iter, typename Self, typename K2>
  static Iter findImpl(Self& self, K2 const& key) {
    auto optLocalIt = findLocal(self, key);
    if (!optLocalIt) {
      return self.end();
    } else {
      return fromLocal<Iter>(*optLocalIt);
    }
  }

 public:
  iterator find(key_type const& key) { return Super::find(key); }

  const_iterator find(key_type const& key) const { return Super::find(key); }

  template <typename K2>
    requires kEligibleForHeterogeneousFind<K2>
  iterator find(K2 const& key) {
    return findImpl<iterator>(*this, key);
  }

  template <typename K2>
    requires kEligibleForHeterogeneousFind<K2>
  const_iterator find(K2 const& key) const {
    return findImpl<const_iterator>(*this, key);
  }

 private:
  template <typename Self, typename K2>
  static auto equalRangeImpl(Self& self, K2 const& key) {
    auto first = self.find(key);
    auto last = first;
    if (last != self.end()) {
      ++last;
    }
    return std::make_pair(first, last);
  }

 public:
  std::pair<iterator, iterator> equal_range(key_type const& key) {
    return Super::equal_range(key);
  }

  std::pair<const_iterator, const_iterator> equal_range(
      key_type const& key) const {
    return Super::equal_range(key);
  }

  template <typename K2>
    requires kEligibleForHeterogeneousFind<K2>
  std::pair<iterator, iterator> equal_range(K2 const& key) {
    return equalRangeImpl(*this, key);
  }

  template <typename K2>
    requires kEligibleForHeterogeneousFind<K2>
  std::pair<const_iterator, const_iterator> equal_range(K2 const& key) const {
    return equalRangeImpl(*this, key);
  }

  //// PUBLIC - F14 Extensions

  template <typename BeforeDestroy>
  iterator eraseInto(const_iterator pos, BeforeDestroy&& beforeDestroy) {
    iterator it = erase(pos, pos);
    FOLLY_SAFE_CHECK(std::addressof(*it) == std::addressof(*pos), "");
    return eraseInto(it, beforeDestroy);
  }

  template <typename BeforeDestroy>
  iterator eraseInto(iterator pos, BeforeDestroy&& beforeDestroy) {
    const_iterator prev{pos};
    ++pos;
    auto nh = this->extract(prev);
    FOLLY_SAFE_CHECK(!nh.empty(), "");
    beforeDestroy(std::move(nh.key()), std::move(nh.mapped()));
    return pos;
  }

  template <typename BeforeDestroy>
  iterator eraseInto(
      const_iterator first,
      const_iterator last,
      BeforeDestroy&& beforeDestroy) {
    iterator pos = erase(first, first);
    FOLLY_SAFE_CHECK(std::addressof(*pos) == std::addressof(*first), "");
    while (pos != last) {
      pos = eraseInto(pos, beforeDestroy);
    }
    return pos;
  }

 private:
  template <typename K2, typename BeforeDestroy>
  size_type eraseIntoImpl(K2 const& key, BeforeDestroy& beforeDestroy) {
    auto it = find(key);
    if (it != this->end()) {
      eraseInto(it, beforeDestroy);
      return 1;
    } else {
      return 0;
    }
  }

 public:
  template <typename BeforeDestroy>
  size_type eraseInto(key_type const& key, BeforeDestroy&& beforeDestroy) {
    return eraseIntoImpl(key, beforeDestroy);
  }

  template <typename K2, typename BeforeDestroy>
    requires kEligibleForHeterogeneousErase<K2>
  size_type eraseInto(K2 const& key, BeforeDestroy&& beforeDestroy) {
    return eraseIntoImpl(key, beforeDestroy);
  }

  bool containsEqualValue(value_type const& value) const {
    // bucket isn't valid if bucket_count is zero
    if (this->empty()) {
      return false;
    }
    auto slot = this->bucket(value.first);
    auto e = this->end(slot);
    for (auto b = this->begin(slot); b != e; ++b) {
      if (b->first == value.first) {
        return b->second == value.second;
      }
    }
    return false;
  }

  // exact for libstdc++, approximate for others
  std::size_t getAllocatedMemorySize() const {
    std::size_t rv = 0;
    visitAllocationClasses([&](std::size_t bytes, std::size_t n) {
      rv += bytes * n;
    });
    return rv;
  }

  // exact for libstdc++, approximate for others
  template <typename V>
  void visitAllocationClasses(V&& visitor) const {
    auto bc = this->bucket_count();
    if (bc > 1) {
      visitor(bc * sizeof(pointer), 1);
    }
    if (this->size() > 0) {
      visitor(sizeof(StdNodeReplica<K, value_type, H>), this->size());
    }
  }

  template <typename V>
  void visitContiguousRanges(V&& visitor) const {
    for (value_type const& entry : *this) {
      value_type const* b = std::addressof(entry);
      visitor(b, b + 1);
    }
  }

  /// F14HashToken interface
  template <typename V>
  std::pair<iterator, bool> insert_or_assign(
      F14HashToken const&, key_type const& key, V&& obj) {
    return insert_or_assign(key, std::forward<V>(obj));
  }

  template <typename V>
  std::pair<iterator, bool> insert_or_assign(
      F14HashToken const&, key_type&& key, V&& obj) {
    return insert_or_assign(std::move(key), std::forward<V>(obj));
  }

  template <typename K2, typename V>
    requires kEligibleForHeterogeneousInsert<K2>
  std::pair<iterator, bool> insert_or_assign(
      F14HashToken const&, K2&& key, V&& obj) {
    return insert_or_assign(std::forward<K2>(key), std::forward<V>(obj));
  }

  template <typename... Args>
  std::pair<iterator, bool> try_emplace_token(
      F14HashToken const&, key_type const& key, Args&&... args) {
    return try_emplace(key, std::forward<Args>(args)...);
  }

  template <typename... Args>
  std::pair<iterator, bool> try_emplace_token(
      F14HashToken const&, key_type&& key, Args&&... args) {
    return try_emplace(std::move(key), std::forward<Args>(args)...);
  }

  template <typename K2, typename... Args>
    requires kEligibleForHeterogeneousInsert<K2>
  std::pair<iterator, bool> try_emplace_token(
      F14HashToken const&, K2&& key, Args&&... args) {
    return try_emplace(std::forward<K2>(key), std::forward<Args>(args)...);
  }

  F14HashToken prehash(key_type const&) const {
    return {}; // Ignored.
  }
  F14HashToken prehash(key_type const&, std::size_t) const {
    return {}; // Ignored.
  }

  template <typename K2>
    requires kEligibleForHeterogeneousFind<K2>
  F14HashToken prehash(K2 const&) const {
    return {}; // Ignored.
  }
  template <typename K2>
    requires kEligibleForHeterogeneousFind<K2>
  F14HashToken prehash(K2 const&, std::size_t) const {
    return {}; // Ignored.
  }

  void prefetch(F14HashToken const&) const {}

  iterator find(F14HashToken const&, key_type const& key) { return find(key); }

  const_iterator find(F14HashToken const&, key_type const& key) const {
    return find(key);
  }

  template <typename K2>
    requires kEligibleForHeterogeneousFind<K2>
  iterator find(F14HashToken const&, K2 const& key) {
    return find(key);
  }

  template <typename K2>
    requires kEligibleForHeterogeneousFind<K2>
  const_iterator find(F14HashToken const&, K2 const& key) const {
    return find(key);
  }

  bool contains(F14HashToken const&, key_type const& key) const {
    return contains(key);
  }

  template <typename K2>
    requires kEligibleForHeterogeneousFind<K2>
  bool contains(F14HashToken const&, K2 const& key) const {
    return contains(key);
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
class F14ValueMap
    : public f14::detail::F14BasicMap<Key, Mapped, Hasher, KeyEqual, Alloc> {
  using Super = f14::detail::F14BasicMap<Key, Mapped, Hasher, KeyEqual, Alloc>;

 public:
  using typename Super::value_type;

  F14ValueMap() = default;

  using Super::Super;

  F14ValueMap& operator=(std::initializer_list<value_type> ilist) {
    Super::operator=(ilist);
    return *this;
  }
};

template <
    typename Key,
    typename Mapped,
    typename Hasher,
    typename KeyEqual,
    typename Alloc>
class F14NodeMap
    : public f14::detail::F14BasicMap<Key, Mapped, Hasher, KeyEqual, Alloc> {
  using Super = f14::detail::F14BasicMap<Key, Mapped, Hasher, KeyEqual, Alloc>;

 public:
  using typename Super::value_type;

  F14NodeMap() = default;

  using Super::Super;

  F14NodeMap& operator=(std::initializer_list<value_type> ilist) {
    Super::operator=(ilist);
    return *this;
  }
};

template <
    typename Key,
    typename Mapped,
    typename Hasher,
    typename KeyEqual,
    typename Alloc>
class F14VectorMap
    : public f14::detail::F14BasicMap<Key, Mapped, Hasher, KeyEqual, Alloc> {
  using Super = f14::detail::F14BasicMap<Key, Mapped, Hasher, KeyEqual, Alloc>;

 public:
  using typename Super::const_iterator;
  using typename Super::iterator;
  using typename Super::value_type;

  F14VectorMap() = default;

  using Super::Super;

  F14VectorMap& operator=(std::initializer_list<value_type> ilist) {
    Super::operator=(ilist);
    return *this;
  }
};

template <
    typename Key,
    typename Mapped,
    typename Hasher,
    typename KeyEqual,
    typename Alloc>
class F14FastMap
    : public f14::detail::F14BasicMap<Key, Mapped, Hasher, KeyEqual, Alloc> {
  using Super = f14::detail::F14BasicMap<Key, Mapped, Hasher, KeyEqual, Alloc>;

 public:
  using typename Super::value_type;

  F14FastMap() = default;

  using Super::Super;

  F14FastMap& operator=(std::initializer_list<value_type> ilist) {
    Super::operator=(ilist);
    return *this;
  }
};

} // namespace folly

#endif // !if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
