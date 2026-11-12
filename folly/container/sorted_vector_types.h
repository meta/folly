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

/*
 * This header defines two classes that very nearly model
 * AssociativeContainer (but not quite).  These implement set-like and
 * map-like behavior on top of a sorted vector, instead of using
 * rb-trees like std::set and std::map.
 *
 * This is potentially useful in cases where the number of elements in
 * the set or map is small, or when you want to avoid using more
 * memory than necessary and insertions/deletions are much more rare
 * than lookups (these classes have O(N) insertions/deletions).
 *
 * In the interest of using these in conditions where the goal is to
 * minimize memory usage, they support a GrowthPolicy parameter, which
 * is a class defining a single function called increase_capacity,
 * which will be called whenever we are about to insert something: you
 * can then decide to call reserve() based on the current capacity()
 * and size() of the passed in vector-esque Container type.  An
 * example growth policy that grows one element at a time:
 *
 *    struct OneAtATimePolicy {
 *      template <class Container>
 *      void increase_capacity(Container& c) {
 *        if (c.size() == c.capacity()) {
 *          c.reserve(c.size() + 1);
 *        }
 *      }
 *    };
 *
 *    typedef sorted_vector_set<int,
 *                              std::less<int>,
 *                              std::allocator<int>,
 *                              OneAtATimePolicy>
 *            OneAtATimeIntSet;
 *
 * Important differences from std::set and std::map:
 *   - insert() and erase() invalidate iterators and references.
       erase(iterator) returns an iterator pointing to the next valid element.
 *   - insert() and erase() are O(N)
 *   - our iterators model RandomAccessIterator
 *   - sorted_vector_map::value_type is pair<K,V>, not pair<const K,V>.
 *     (This is basically because we want to store the value_type in
 *     std::vector<>, which requires it to be Assignable.)
 *   - insert() single key variants, emplace(), and emplace_hint() only provide
 *     the strong exception guarantee (unchanged when exception is thrown) when
 *     std::is_nothrow_move_constructible<value_type>::value is true.
 */

#pragma once

#include <algorithm>
#include <cassert>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <folly/CppAttributes.h>
#include <folly/ScopeGuard.h>
#include <folly/Traits.h>
#include <folly/Utility.h>
#include <folly/container/range_traits.h>
#include <folly/lang/Access.h>
#include <folly/lang/Exception.h>
#include <folly/memory/MemoryResource.h>
#include <folly/small_vector.h>

namespace folly {

//////////////////////////////////////////////////////////////////////

namespace detail {

// This wrapper goes around a GrowthPolicy and provides iterator
// preservation semantics, but only if the growth policy is not the
// default (i.e. nothing).
template <class Policy>
struct growth_policy_wrapper : private Policy {
  template <class Container, class Iterator>
  Iterator increase_capacity(Container& c, Iterator desired_insertion) {
    using diff_t = typename Container::difference_type;
    diff_t d = desired_insertion - c.begin();
    Policy::increase_capacity(c);
    return c.begin() + d;
  }
};
template <>
struct growth_policy_wrapper<void> {
  template <class Container, class Iterator>
  Iterator increase_capacity(Container&, Iterator it) {
    return it;
  }
};

template <class OurContainer, class Vector, class GrowthPolicy, class Value>
typename OurContainer::iterator insert_with_hint(
    OurContainer& sorted,
    Vector& cont,
    typename OurContainer::const_iterator hint,
    Value&& value,
    GrowthPolicy& po) {
  const typename OurContainer::value_compare& cmp(sorted.value_comp());
  if (hint == cont.end() || cmp(value, *hint)) {
    if (hint == cont.begin() || cmp(*(hint - 1), value)) {
      hint = po.increase_capacity(cont, hint);
      return cont.emplace(hint, std::forward<Value>(value));
    } else {
      return sorted.emplace(std::forward<Value>(value)).first;
    }
  }

  if (cmp(*hint, value)) {
    if (hint + 1 == cont.end() || cmp(value, *(hint + 1))) {
      hint = po.increase_capacity(cont, hint + 1);
      return cont.emplace(hint, std::forward<Value>(value));
    } else {
      return sorted.emplace(std::forward<Value>(value)).first;
    }
  }

  // Value and *hint did not compare, so they are equal keys.
  return sorted.begin() + std::distance(sorted.cbegin(), hint);
}

template <typename Iterator, typename Compare>
bool is_sorted_unique(Iterator begin, Iterator end, Compare const& comp) {
  if (begin == end) {
    return true;
  }
  for (auto next = std::next(begin); next != end; ++begin, ++next) {
    if (!comp(*begin, *next)) {
      return false;
    }
  }
  return true;
}

template <typename Container, typename Compare>
Container&& as_sorted_unique(Container&& container, Compare const& comp) {
  std::sort(container.begin(), container.end(), comp);
  container.erase(
      std::unique(
          container.begin(),
          container.end(),
          [&](auto const& a, auto const& b) {
            return !comp(a, b) && !comp(b, a);
          }),
      container.end());
  return static_cast<Container&&>(container);
}

template <typename Container, typename Compare>
class DirectMutationGuard {
 public:
  DirectMutationGuard(
      Container& container, const Compare& comp, bool isSortedUnique)
      : container_(container), comp_(comp), isSortedUnique_(isSortedUnique) {}

  ~DirectMutationGuard() noexcept(false) {
    if (isSortedUnique_) {
      assert(
          detail::is_sorted_unique(
              container_.begin(), container_.end(), comp_));
      return;
    }
    as_sorted_unique(container_, comp_);
  }

  Container& get() { return container_; }

 private:
  Container& container_;
  const Compare comp_;
  const bool isSortedUnique_;
};

template <class OurContainer, class Vector, class InputIterator>
void bulk_insert(
    OurContainer& sorted,
    Vector& cont,
    InputIterator first,
    InputIterator last,
    bool range_is_sorted_unique = false) {
  // Prevent deref of middle where middle == cont.end().
  if (first == last) {
    return;
  }

  auto const prev_size = cont.size();
  cont.insert(cont.end(), first, last);
  auto const middle = cont.begin() + prev_size;

  auto const& cmp(sorted.value_comp());
  if (range_is_sorted_unique) {
    assert(is_sorted_unique(middle, cont.end(), cmp));
  } else if (!std::is_sorted(middle, cont.end(), cmp)) {
    std::sort(middle, cont.end(), cmp);
  }

  // We do not need to consider elements strictly smaller than the smallest new
  // element in merge/unique.
  auto merge_begin = middle;
  while (merge_begin != cont.begin() && !cmp(*(merge_begin - 1), *middle)) {
    --merge_begin;
  }

  if (merge_begin != middle) {
    std::inplace_merge(cont.begin(), middle, cont.end(), cmp);
  } else if (range_is_sorted_unique) {
    // Old and new elements are already disjoint and unique. This includes the
    // case when cont is initially empty.
    return;
  }

  cont.erase(
      std::unique(
          merge_begin,
          cont.end(),
          [&](typename OurContainer::value_type const& a,
              typename OurContainer::value_type const& b) {
            return !cmp(a, b);
          }),
      cont.end());
}

} // namespace detail

//////////////////////////////////////////////////////////////////////

/**
 * A sorted_vector_set is a container similar to std::set<>, but
 * implemented as a sorted array with std::vector<>.
 *
 * @tparam T               Data type to store
 * @tparam Compare         Comparison function that imposes a
 *                              strict weak ordering over instances of T
 * @tparam Allocator       allocation policy
 * @tparam GrowthPolicy    policy object to control growth
 */
template <
    class T,
    class Compare = std::less<T>,
    class Allocator = std::allocator<T>,
    class GrowthPolicy = void,
    class Container = std::vector<T, Allocator>>
class sorted_vector_set : detail::growth_policy_wrapper<GrowthPolicy> {
  detail::growth_policy_wrapper<GrowthPolicy>& get_growth_policy() {
    return *this;
  }

  struct EBO;

 public:
  using value_type = T;
  using key_type = T;
  using key_compare = Compare;
  using value_compare = Compare;
  using allocator_type = Allocator;
  using container_type = Container;

  using pointer = typename Container::pointer;
  using reference = typename Container::reference;
  using const_reference = typename Container::const_reference;
  using const_pointer = typename Container::const_pointer;
  /*
   * XXX: Our normal iterator ought to also be a constant iterator
   * (cf. Defect Report 103 for std::set), but this is a bit more of a
   * pain.
   */
  using iterator = typename Container::iterator;
  using const_iterator = typename Container::const_iterator;
  using difference_type = typename Container::difference_type;
  using size_type = typename Container::size_type;
  using reverse_iterator = typename Container::reverse_iterator;
  using const_reverse_iterator = typename Container::const_reverse_iterator;
  using direct_mutation_guard =
      detail::DirectMutationGuard<Container, value_compare>;

  sorted_vector_set() : m_(Compare(), Allocator()) {}

  sorted_vector_set(const sorted_vector_set&) = default;

  sorted_vector_set(const sorted_vector_set& other, const Allocator& alloc)
      : m_(other.m_, alloc) {}

  sorted_vector_set(sorted_vector_set&&) = default;

  sorted_vector_set(sorted_vector_set&& other, const Allocator& alloc) noexcept(
      std::is_nothrow_constructible<EBO, EBO&&, const Allocator&>::value)
      : m_(std::move(other.m_), alloc) {}

  explicit sorted_vector_set(const Allocator& alloc) : m_(Compare(), alloc) {}

  explicit sorted_vector_set(
      const Compare& comp, const Allocator& alloc = Allocator())
      : m_(comp, alloc) {}

  template <class InputIterator>
  sorted_vector_set(
      InputIterator first,
      InputIterator last,
      const Compare& comp = Compare(),
      const Allocator& alloc = Allocator())
      : m_(comp, alloc) {
    // This is linear if [first, last) is already sorted (and if we
    // can figure out the distance between the two iterators).
    insert(first, last);
  }

  template <class InputIterator>
  sorted_vector_set(
      InputIterator first, InputIterator last, const Allocator& alloc)
      : m_(Compare(), alloc) {
    // This is linear if [first, last) is already sorted (and if we
    // can figure out the distance between the two iterators).
    insert(first, last);
  }

  /* implicit */ sorted_vector_set(
      std::initializer_list<value_type> list,
      const Compare& comp = Compare(),
      const Allocator& alloc = Allocator())
      : m_(comp, alloc) {
    insert(list.begin(), list.end());
  }

  sorted_vector_set(
      std::initializer_list<value_type> list, const Allocator& alloc)
      : m_(Compare(), alloc) {
    insert(list.begin(), list.end());
  }

  // Construct a sorted_vector_set by stealing the storage of a prefilled
  // container. The container need not be sorted already. This supports
  // bulk construction of sorted_vector_set with zero allocations, not counting
  // those performed by the caller. (The iterator range constructor performs at
  // least one allocation).
  //
  // Note that `sorted_vector_set(const Container& container)` is not provided,
  // since the purpose of this constructor is to avoid an unnecessary copy.
  explicit sorted_vector_set(
      Container&& container, const Compare& comp = Compare())
      : sorted_vector_set(
            sorted_unique,
            detail::as_sorted_unique(std::move(container), comp),
            comp) {}

  // Construct a sorted_vector_set by stealing the storage of a prefilled
  // container. Its elements must be sorted and unique, as sorted_unique_t
  // hints. Supports bulk construction of sorted_vector_set with zero
  // allocations, not counting those performed by the caller. (The iterator
  // range constructor performs at least one allocation).
  //
  // Note that `sorted_vector_set(sorted_unique_t, const Container& container)`
  // is not provided, since the purpose of this constructor is to avoid an extra
  // copy.
  sorted_vector_set(
      sorted_unique_t,
      Container&& container,
      const Compare& comp =
          Compare()) noexcept(std::
                                  is_nothrow_constructible<
                                      EBO,
                                      const Compare&,
                                      Container&&>::value)
      : m_(comp, std::move(container)) {
    assert(
        detail::is_sorted_unique(
            m_.cont_.begin(), m_.cont_.end(), value_comp()));
  }

  Allocator get_allocator() const { return m_.cont_.get_allocator(); }

  const Container& get_container() const noexcept { return m_.cont_; }

  /**
   * Contiguous view of all elements in sorted order.
   *
   * Invalidated by any mutation, like iterators. Elements are const even
   * through a non-const set.
   *
   * Only available when the underlying container exposes data() and size().
   */
  std::span<value_type const> as_span() noexcept
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]]
    requires requires(Container& c) {
      c.data();
      c.size();
    }
  {
    static_assert(is_contiguous_range_v<Container>);
    return {data(), size()};
  }
  std::span<value_type const> as_span() const noexcept
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]]
    requires requires(Container const& c) {
      c.data();
      c.size();
    }
  {
    static_assert(is_contiguous_range_v<Container>);
    return {data(), size()};
  }

  /**
   * Directly mutate the container.
   *
   * Get a guarded reference to the underlying container for direct mutation.
   * sorted_unique_t signals that user will make sure that after the
   * modification the container will have its values as sorted-unique
   * (conforming to container's value_comp). Violating this assumption will
   * result in undefined behavior.
   *
   * This function is not safe to use concurrently with other functions.
   */
  direct_mutation_guard get_container_for_direct_mutation(
      sorted_unique_t) noexcept {
    return direct_mutation_guard{
        m_.cont_, value_comp(), /* range_is_sorted_unique */ true};
  }

  /**
   * Directly mutate the container.
   *
   * Get a guarded reference to the underlying container for direct mutation.
   * The container will initially be sorted and unique. You are not required to
   * maintain the sorted-unique invariant while mutating. When the guard is
   * released, it will sort and unique-ify the container.
   *
   * This function is not safe to use concurrently with other functions.
   */
  direct_mutation_guard get_container_for_direct_mutation() noexcept {
    return direct_mutation_guard{
        m_.cont_, value_comp(), /* range_is_sorted_unique */ false};
  }

  /**
   * Directly swap the container. Similar to swap()
   */
  void swap_container(Container& newContainer) {
    detail::as_sorted_unique(newContainer, value_comp());
    using std::swap;
    swap(m_.cont_, newContainer);
  }
  void swap_container(sorted_unique_t, Container& newContainer) {
    assert(
        detail::is_sorted_unique(
            newContainer.begin(), newContainer.end(), value_comp()));
    using std::swap;
    swap(m_.cont_, newContainer);
  }

  sorted_vector_set& operator=(const sorted_vector_set& other) = default;

  sorted_vector_set& operator=(sorted_vector_set&& other) = default;

  sorted_vector_set& operator=(std::initializer_list<value_type> ilist) {
    clear();
    insert(ilist.begin(), ilist.end());
    return *this;
  }

  key_compare key_comp() const { return m_; }
  value_compare value_comp() const { return m_; }

  iterator begin() [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return m_.cont_.begin();
  }
  iterator end() [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] { return m_.cont_.end(); }
  const_iterator cbegin() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return m_.cont_.cbegin();
  }
  const_iterator begin() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return m_.cont_.begin();
  }
  const_iterator cend() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return m_.cont_.cend();
  }
  const_iterator end() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return m_.cont_.end();
  }
  reverse_iterator rbegin() [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return m_.cont_.rbegin();
  }
  reverse_iterator rend() [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return m_.cont_.rend();
  }
  const_reverse_iterator rbegin() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return m_.cont_.rbegin();
  }
  const_reverse_iterator rend() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return m_.cont_.rend();
  }

  void clear() { return m_.cont_.clear(); }
  size_type size() const { return m_.cont_.size(); }
  size_type max_size() const { return m_.cont_.max_size(); }
  bool empty() const { return m_.cont_.empty(); }
  void reserve(size_type s) { return m_.cont_.reserve(s); }
  void shrink_to_fit() { m_.cont_.shrink_to_fit(); }
  size_type capacity() const { return m_.cont_.capacity(); }

  std::pair<iterator, bool> insert(const value_type& value) {
    iterator it = lower_bound(value);
    if (it == end() || value_comp()(value, *it)) {
      it = get_growth_policy().increase_capacity(m_.cont_, it);
      return std::make_pair(m_.cont_.emplace(it, value), true);
    }
    return std::make_pair(it, false);
  }

  std::pair<iterator, bool> insert(value_type&& value) {
    iterator it = lower_bound(value);
    if (it == end() || value_comp()(value, *it)) {
      it = get_growth_policy().increase_capacity(m_.cont_, it);
      return std::make_pair(m_.cont_.emplace(it, std::move(value)), true);
    }
    return std::make_pair(it, false);
  }

  iterator insert(const_iterator hint, const value_type& value) {
    return detail::insert_with_hint(
        *this, m_.cont_, hint, value, get_growth_policy());
  }

  iterator insert(const_iterator hint, value_type&& value) {
    return detail::insert_with_hint(
        *this, m_.cont_, hint, std::move(value), get_growth_policy());
  }

  template <class InputIterator>
  void insert(InputIterator first, InputIterator last) {
    detail::bulk_insert(*this, m_.cont_, first, last);
  }

  // If [first, last) is known to be sorted and unique according to the
  // comparator (for example if the range comes from a sorted container of the
  // same type) this version can save unnecessary operations, especially if
  // *this is empty.
  template <class InputIterator>
  void insert(sorted_unique_t, InputIterator first, InputIterator last) {
    detail::bulk_insert(
        *this, m_.cont_, first, last, /* range_is_sorted_unique */ true);
  }

  void insert(std::initializer_list<value_type> ilist) {
    insert(ilist.begin(), ilist.end());
  }

  // emplace isn't better than insert for sorted_vector_set, but aids
  // compatibility
  template <typename... Args>
  std::pair<iterator, bool> emplace(Args&&... args) {
    folly::aligned_storage_for_t<value_type> b;
    value_type* p = static_cast<value_type*>(static_cast<void*>(&b));
    auto a = get_allocator();
    std::allocator_traits<allocator_type>::construct(
        a, p, std::forward<Args>(args)...);
    auto g = makeGuard([&]() {
      std::allocator_traits<allocator_type>::destroy(a, p);
    });
    return insert(std::move(*p));
  }

  std::pair<iterator, bool> emplace(const value_type& value) {
    return insert(value);
  }

  std::pair<iterator, bool> emplace(value_type&& value) {
    return insert(std::move(value));
  }

  // emplace_hint isn't better than insert for sorted_vector_set, but aids
  // compatibility
  template <typename... Args>
  iterator emplace_hint(const_iterator hint, Args&&... args) {
    folly::aligned_storage_for_t<value_type> b;
    value_type* p = static_cast<value_type*>(static_cast<void*>(&b));
    auto a = get_allocator();
    std::allocator_traits<allocator_type>::construct(
        a, p, std::forward<Args>(args)...);
    auto g = makeGuard([&]() {
      std::allocator_traits<allocator_type>::destroy(a, p);
    });
    return insert(hint, std::move(*p));
  }

  iterator emplace_hint(const_iterator hint, const value_type& value) {
    return insert(hint, value);
  }

  iterator emplace_hint(const_iterator hint, value_type&& value) {
    return insert(hint, std::move(value));
  }

  size_type erase(const key_type& key) {
    iterator it = find(key);
    if (it == end()) {
      return 0;
    }
    m_.cont_.erase(it);
    return 1;
  }

  iterator erase(const_iterator it) { return m_.cont_.erase(it); }

  iterator erase(const_iterator first, const_iterator last) {
    return m_.cont_.erase(first, last);
  }

  template <class Predicate>
  friend size_type erase_if(sorted_vector_set& container, Predicate predicate) {
    auto& c = container.m_.cont_;
    const auto preEraseSize = c.size();
    c.erase(std::remove_if(c.begin(), c.end(), std::ref(predicate)), c.end());
    return preEraseSize - c.size();
  }

  iterator find(const key_type& key) [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return find_(*this, key);
  }

  const_iterator find(const key_type& key) const
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return find_(*this, key);
  }

  template <typename K>
    requires is_transparent_v<Compare>
  iterator find(const K& key) [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return find_(*this, key);
  }

  template <typename K>
    requires is_transparent_v<Compare>
  const_iterator find(const K& key) const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return find_(*this, key);
  }

  size_type count(const key_type& key) const {
    return find(key) == end() ? 0 : 1;
  }

  std::pair<iterator, iterator> find(
      const key_type& key1, const key_type& key2) {
    if (key_comp()(key2, key1)) {
      auto iterators = find2_(*this, key2, key1);
      access::swap(iterators.first, iterators.second);
      return iterators;
    } else {
      return find2_(*this, key1, key2);
    }
  }

  std::pair<const_iterator, const_iterator> find(
      const key_type& key1, const key_type& key2) const {
    if (key_comp()(key2, key1)) {
      auto iterators = find2_(*this, key2, key1);
      access::swap(iterators.first, iterators.second);
      return iterators;
    } else {
      return find2_(*this, key1, key2);
    }
  }

  template <typename K>
    requires is_transparent_v<Compare>
  std::pair<iterator, iterator> find(const K& key1, const K& key2) {
    if (key_comp()(key2, key1)) {
      auto iterators = find2_(*this, key2, key1);
      access::swap(iterators.first, iterators.second);
      return iterators;
    } else {
      return find2_(*this, key1, key2);
    }
  }

  template <typename K>
    requires is_transparent_v<Compare>
  std::pair<const_iterator, const_iterator> find(
      const K& key1, const K& key2) const {
    if (key_comp()(key2, key1)) {
      auto iterators = find2_(*this, key2, key1);
      access::swap(iterators.first, iterators.second);
      return iterators;
    } else {
      return find2_(*this, key1, key2);
    }
  }

  template <typename K>
    requires is_transparent_v<Compare>
  size_type count(const K& key) const {
    return find(key) == end() ? 0 : 1;
  }

  bool contains(const key_type& key) const { return find(key) != end(); }

  template <typename K>
    requires is_transparent_v<Compare>
  bool contains(const K& key) const {
    return find(key) != end();
  }

  iterator lower_bound(const key_type& key) [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return std::lower_bound(begin(), end(), key, key_comp());
  }

  const_iterator lower_bound(const key_type& key) const
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return std::lower_bound(begin(), end(), key, key_comp());
  }

  template <typename K>
    requires is_transparent_v<Compare>
  iterator lower_bound(const K& key) [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return std::lower_bound(begin(), end(), key, key_comp());
  }

  template <typename K>
    requires is_transparent_v<Compare>
  const_iterator lower_bound(const K& key) const
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return std::lower_bound(begin(), end(), key, key_comp());
  }

  iterator upper_bound(const key_type& key) [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return std::upper_bound(begin(), end(), key, key_comp());
  }

  const_iterator upper_bound(const key_type& key) const
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return std::upper_bound(begin(), end(), key, key_comp());
  }

  template <typename K>
    requires is_transparent_v<Compare>
  iterator upper_bound(const K& key) [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return std::upper_bound(begin(), end(), key, key_comp());
  }

  template <typename K>
    requires is_transparent_v<Compare>
  const_iterator upper_bound(const K& key) const
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return std::upper_bound(begin(), end(), key, key_comp());
  }

  std::pair<iterator, iterator> equal_range(const key_type& key) {
    return std::equal_range(begin(), end(), key, key_comp());
  }

  std::pair<const_iterator, const_iterator> equal_range(
      const key_type& key) const {
    return std::equal_range(begin(), end(), key, key_comp());
  }

  template <typename K>
    requires is_transparent_v<Compare>
  std::pair<iterator, iterator> equal_range(const K& key) {
    return std::equal_range(begin(), end(), key, key_comp());
  }

  template <typename K>
    requires is_transparent_v<Compare>
  std::pair<const_iterator, const_iterator> equal_range(const K& key) const {
    return std::equal_range(begin(), end(), key, key_comp());
  }

  void swap(sorted_vector_set& o) noexcept(
      std::is_nothrow_swappable_v<Compare> &&
      noexcept(std::declval<Container&>().swap(o.m_.cont_))) {
    using std::swap; // Allow ADL for swap(); fall back to std::swap().
    Compare& a = m_;
    Compare& b = o.m_;
    swap(a, b);
    m_.cont_.swap(o.m_.cont_);
  }

  bool operator==(const sorted_vector_set& other) const {
    return other.m_.cont_ == m_.cont_;
  }
  bool operator!=(const sorted_vector_set& other) const {
    return !operator==(other);
  }

  bool operator<(const sorted_vector_set& other) const {
    return m_.cont_ < other.m_.cont_;
  }
  bool operator>(const sorted_vector_set& other) const { return other < *this; }
  bool operator<=(const sorted_vector_set& other) const {
    return !operator>(other);
  }
  bool operator>=(const sorted_vector_set& other) const {
    return !operator<(other);
  }

  template <typename U = Container>
  friend auto operator<=>(
      const sorted_vector_set& lhs, const sorted_vector_set& rhs)
      -> decltype(std::declval<const U&>() <=> std::declval<const U&>()) {
    return lhs.m_.cont_ <=> rhs.m_.cont_;
  }

  const value_type* data() const noexcept { return m_.cont_.data(); }

 private:
  /*
   * This structure derives from the comparison object in order to
   * make use of the empty base class optimization if our comparison
   * functor is an empty class (usual case).
   *
   * Wrapping up this member like this is better than deriving from
   * the Compare object ourselves (there are some perverse edge cases
   * involving virtual functions).
   *
   * More info:  http://www.cantrip.org/emptyopt.html
   */
  struct EBO : Compare {
    explicit EBO(const Compare& c, const Allocator& alloc) noexcept(
        std::is_nothrow_default_constructible<Container>::value)
        : Compare(c), cont_(alloc) {}
    EBO(const EBO& other, const Allocator& alloc) noexcept(
        std::is_nothrow_constructible<
            Container,
            const Container&,
            const Allocator&>::value)
        : Compare(static_cast<const Compare&>(other)),
          cont_(other.cont_, alloc) {}
    EBO(EBO&& other, const Allocator& alloc) noexcept(
        std::is_nothrow_constructible<
            Container,
            Container&&,
            const Allocator&>::value)
        : Compare(static_cast<Compare&&>(other)),
          cont_(std::move(other.cont_), alloc) {}
    EBO(const Compare& c, Container&& cont) noexcept(
        std::is_nothrow_move_constructible<Container>::value)
        : Compare(c), cont_(std::move(cont)) {}
    Container cont_;
  } m_;

  template <typename Self>
  using self_iterator_t = _t<
      std::conditional<std::is_const<Self>::value, const_iterator, iterator>>;

  template <typename Self, typename K>
  static self_iterator_t<Self> find_(Self& self, K const& key) {
    auto end = self.end();
    auto it = self.lower_bound(key);
    if (it == end || !self.key_comp()(key, *it)) {
      return it;
    }
    return end;
  }
  template <typename Self, typename K>
  static std::pair<self_iterator_t<Self>, self_iterator_t<Self>> lower_bound2_(
      Self& self, K const& key1, K const& key2) {
    auto len = self.size();
    auto first = self.begin(), second = self.begin();
    auto c = self.key_comp();
    assert(!c(key2, key1));
    while (true) {
      if (len == 0) {
        return std::make_pair(first, first);
      }
      auto half = len / 2;
      auto middle = first + half;
      if (c(*middle, key1)) {
        first = middle + 1;
        half = len - half - 1;
      } else if (c(*middle, key2)) {
        second = middle + (len & 1);
        len = half;
        break;
      }
      len = half;
    }
    while (len) {
      auto half = len / 2;
      auto middle1 = first + half;
      auto middle2 = second + half;
      if (c(*middle1, key1)) {
        first = middle1 + (len & 1);
      }
      if (c(*middle2, key2)) {
        second = middle2 + (len & 1);
      }
      len = half;
    }
    return std::make_pair(first, second);
  }

  template <typename Self, typename K>
  static std::pair<self_iterator_t<Self>, self_iterator_t<Self>> find2_(
      Self& self, K const& key1, K const& key2) {
    auto end = self.end();
    auto its = lower_bound2_(self, key1, key2);
    if (its.second != end) {
      if (self.key_comp()(key1, *its.first)) {
        its.first = end;
      }
      if (self.key_comp()(key2, *its.second)) {
        its.second = end;
      }
    } else if (its.first != end && self.key_comp()(key1, *its.first)) {
      its.first = end;
    }
    return its;
  }
};

// Swap function that can be found using ADL.
template <class T, class C, class A, class G>
inline void swap(
    sorted_vector_set<T, C, A, G>& a, sorted_vector_set<T, C, A, G>& b) {
  return a.swap(b);
}

template <typename T>
inline constexpr bool is_sorted_vector_set_v =
    is_instantiation_of_v<sorted_vector_set, T>;

template <typename T>
struct is_sorted_vector_set : std::bool_constant<is_sorted_vector_set_v<T>> {};

template <
    class T,
    size_t N = 1,
    class Compare = std::less<T>,
    class Allocator = std::allocator<T>,
    class GrowthPolicy = void,
    class SmallVectorPolicy = void>
using small_sorted_vector_set = sorted_vector_set<
    T,
    Compare,
    Allocator,
    GrowthPolicy,
    folly::small_vector<T, N, SmallVectorPolicy>>;

template <typename T>
inline constexpr bool is_small_sorted_vector_set_v =
    is_sorted_vector_set_v<T> && is_small_vector_v<typename T::container_type>;

template <typename T>
struct is_small_sorted_vector_set
    : std::bool_constant<is_small_sorted_vector_set_v<T>> {};

#if FOLLY_HAS_MEMORY_RESOURCE

namespace pmr {

template <
    class T,
    class Compare = std::less<T>,
    class GrowthPolicy = void,
    class Container = std::vector<T, std::pmr::polymorphic_allocator<T>>>
using sorted_vector_set = folly::sorted_vector_set<
    T,
    Compare,
    std::pmr::polymorphic_allocator<T>,
    GrowthPolicy,
    Container>;

} // namespace pmr

#endif

//////////////////////////////////////////////////////////////////////

/**
 * A sorted_vector_map is similar to a sorted_vector_set but stores
 * <key,value> pairs instead of single elements.
 *
 * @tparam Key           Key type
 * @tparam Value         Value type
 * @tparam Compare       Function that can compare key types and impose
 *                            a strict weak ordering over them.
 * @tparam Allocator     allocation policy
 * @tparam GrowthPolicy  policy object to control growth
 */
template <
    class Key,
    class Value,
    class Compare = std::less<Key>,
    class Allocator = std::allocator<std::pair<Key, Value>>,
    class GrowthPolicy = void,
    class Container = std::vector<std::pair<Key, Value>, Allocator>>
class sorted_vector_map : detail::growth_policy_wrapper<GrowthPolicy> {
  detail::growth_policy_wrapper<GrowthPolicy>& get_growth_policy() {
    return *this;
  }

  struct EBO;

 public:
  using key_type = Key;
  using mapped_type = Value;
  using value_type = typename Container::value_type;
  using key_compare = Compare;
  using allocator_type = Allocator;
  using container_type = Container;

  struct value_compare : private Compare {
    bool operator()(const value_type& a, const value_type& b) const {
      return Compare::operator()(a.first, b.first);
    }

   protected:
    friend class sorted_vector_map;
    explicit value_compare(const Compare& c) : Compare(c) {}
  };

  using pointer = typename Container::pointer;
  using const_pointer = typename Container::const_pointer;
  using reference = typename Container::reference;
  using const_reference = typename Container::const_reference;
  using iterator = typename Container::iterator;
  using const_iterator = typename Container::const_iterator;
  using difference_type = typename Container::difference_type;
  using size_type = typename Container::size_type;
  using reverse_iterator = typename Container::reverse_iterator;
  using const_reverse_iterator = typename Container::const_reverse_iterator;
  using direct_mutation_guard =
      detail::DirectMutationGuard<Container, value_compare>;

  sorted_vector_map() noexcept(
      std::is_nothrow_constructible<EBO, value_compare, Allocator>::value)
      : m_(value_compare(Compare()), Allocator()) {}

  sorted_vector_map(const sorted_vector_map&) = default;

  sorted_vector_map(const sorted_vector_map& other, const Allocator& alloc)
      : m_(other.m_, alloc) {}

  sorted_vector_map(sorted_vector_map&&) = default;

  sorted_vector_map(sorted_vector_map&& other, const Allocator& alloc) noexcept(
      std::is_nothrow_constructible<EBO, EBO&&, const Allocator&>::value)
      : m_(std::move(other.m_), alloc) {}

  explicit sorted_vector_map(const Allocator& alloc)
      : m_(value_compare(Compare()), alloc) {}

  explicit sorted_vector_map(
      const Compare& comp, const Allocator& alloc = Allocator())
      : m_(value_compare(comp), alloc) {}

  template <class InputIterator>
  explicit sorted_vector_map(
      InputIterator first,
      InputIterator last,
      const Compare& comp = Compare(),
      const Allocator& alloc = Allocator())
      : m_(value_compare(comp), alloc) {
    insert(first, last);
  }

  template <class InputIterator>
  sorted_vector_map(
      InputIterator first, InputIterator last, const Allocator& alloc)
      : m_(value_compare(Compare()), alloc) {
    insert(first, last);
  }

  /* implicit */ sorted_vector_map(
      std::initializer_list<value_type> list,
      const Compare& comp = Compare(),
      const Allocator& alloc = Allocator())
      : m_(value_compare(comp), alloc) {
    insert(list.begin(), list.end());
  }

  sorted_vector_map(
      std::initializer_list<value_type> list, const Allocator& alloc)
      : m_(value_compare(Compare()), alloc) {
    insert(list.begin(), list.end());
  }

  // Construct a sorted_vector_map by stealing the storage of a prefilled
  // container. The container need not be sorted already. This supports
  // bulk construction of sorted_vector_map with zero allocations, not counting
  // those performed by the caller. (The iterator range constructor performs at
  // least one allocation).
  //
  // Note that `sorted_vector_map(const Container& container)` is not provided,
  // since the purpose of this constructor is to avoid an unnecessary copy.
  explicit sorted_vector_map(
      Container&& container, const Compare& comp = Compare())
      : sorted_vector_map(
            sorted_unique,
            detail::as_sorted_unique(std::move(container), value_compare(comp)),
            comp) {}

  // Construct a sorted_vector_map by stealing the storage of a prefilled
  // container. Its elements must be sorted and unique, as sorted_unique_t
  // hints. Supports bulk construction of sorted_vector_map with zero
  // allocations, not counting those performed by the caller. (The iterator
  // range constructor performs at least one allocation).
  //
  // Note that `sorted_vector_map(sorted_unique_t, const Container& container)`
  // is not provided, since the purpose of this constructor is to avoid an extra
  // copy.
  sorted_vector_map(
      sorted_unique_t,
      Container&& container,
      const Compare& comp =
          Compare()) noexcept(std::
                                  is_nothrow_constructible<
                                      EBO,
                                      value_compare,
                                      Container&&>::value)
      : m_(value_compare(comp), std::move(container)) {
    assert(
        detail::is_sorted_unique(
            m_.cont_.begin(), m_.cont_.end(), value_comp()));
  }

  Allocator get_allocator() const { return m_.cont_.get_allocator(); }

  const Container& get_container() const noexcept { return m_.cont_; }

  /**
   * Contiguous view of all elements in sorted order.
   *
   * Invalidated by any mutation, like iterators. Do not mutate keys or
   * otherwise break the sorted order through the span.
   *
   * Only available when the underlying container exposes data() and size().
   */
  std::span<value_type> as_span() noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]]
    requires requires(Container& c) {
      c.data();
      c.size();
    }
  {
    static_assert(is_contiguous_range_v<Container>);
    return {data(), size()};
  }
  std::span<value_type const> as_span() const noexcept
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]]
    requires requires(Container const& c) {
      c.data();
      c.size();
    }
  {
    static_assert(is_contiguous_range_v<Container>);
    return {data(), size()};
  }

  /**
   * Directly mutate the container.
   *
   * Get a guarded reference to the underlying container for direct mutation.
   * sorted_unique_t signals that user will make sure that after the
   * modification the container will have its values as sorted-unique
   * (conforming to container's value_comp). Violating this assumption will
   * result in undefined behavior.
   *
   * This function is not safe to use concurrently with other functions.
   */
  direct_mutation_guard get_container_for_direct_mutation(
      sorted_unique_t) noexcept {
    return direct_mutation_guard{
        m_.cont_, value_comp(), /* range_is_sorted_unique */ true};
  }

  /**
   * Directly mutate the container.
   *
   * Get a guarded reference to the underlying container for direct mutation.
   * The container will initially be sorted and unique. You are not required to
   * maintain the sorted-unique invariant while mutating. When the guard is
   * released, it will sort and unique-ify the container.
   *
   * This function is not safe to use concurrently with other functions.
   */
  direct_mutation_guard get_container_for_direct_mutation() noexcept {
    return direct_mutation_guard{
        m_.cont_, value_comp(), /* range_is_sorted_unique */ false};
  }

  /**
   * Directly swap the container. Similar to swap()
   */
  void swap_container(Container& newContainer) {
    detail::as_sorted_unique(newContainer, value_comp());
    using std::swap;
    swap(m_.cont_, newContainer);
  }
  void swap_container(sorted_unique_t, Container& newContainer) {
    assert(
        detail::is_sorted_unique(
            newContainer.begin(), newContainer.end(), value_comp()));
    using std::swap;
    swap(m_.cont_, newContainer);
  }

  sorted_vector_map& operator=(const sorted_vector_map& other) = default;

  sorted_vector_map& operator=(sorted_vector_map&& other) = default;

  sorted_vector_map& operator=(std::initializer_list<value_type> ilist) {
    clear();
    insert(ilist.begin(), ilist.end());
    return *this;
  }

  key_compare key_comp() const { return m_; }
  value_compare value_comp() const { return m_; }

  iterator begin() [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return m_.cont_.begin();
  }
  iterator end() [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] { return m_.cont_.end(); }
  const_iterator cbegin() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return m_.cont_.cbegin();
  }
  const_iterator begin() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return m_.cont_.begin();
  }
  const_iterator cend() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return m_.cont_.cend();
  }
  const_iterator end() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return m_.cont_.end();
  }
  reverse_iterator rbegin() [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return m_.cont_.rbegin();
  }
  reverse_iterator rend() [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return m_.cont_.rend();
  }
  const_reverse_iterator crbegin() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return m_.cont_.crbegin();
  }
  const_reverse_iterator rbegin() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return m_.cont_.rbegin();
  }
  const_reverse_iterator crend() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return m_.cont_.crend();
  }
  const_reverse_iterator rend() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return m_.cont_.rend();
  }

  void clear() { return m_.cont_.clear(); }
  size_type size() const { return m_.cont_.size(); }
  size_type max_size() const { return m_.cont_.max_size(); }
  bool empty() const { return m_.cont_.empty(); }
  void reserve(size_type s) { return m_.cont_.reserve(s); }
  void shrink_to_fit() { m_.cont_.shrink_to_fit(); }
  size_type capacity() const { return m_.cont_.capacity(); }

  std::pair<iterator, bool> insert(const value_type& value) {
    iterator it = lower_bound(value.first);
    if (it == end() || value_comp()(value, *it)) {
      it = get_growth_policy().increase_capacity(m_.cont_, it);
      return std::make_pair(m_.cont_.emplace(it, value), true);
    }
    return std::make_pair(it, false);
  }

  std::pair<iterator, bool> insert(value_type&& value) {
    iterator it = lower_bound(value.first);
    if (it == end() || value_comp()(value, *it)) {
      it = get_growth_policy().increase_capacity(m_.cont_, it);
      return std::make_pair(m_.cont_.emplace(it, std::move(value)), true);
    }
    return std::make_pair(it, false);
  }

  iterator insert(const_iterator hint, const value_type& value) {
    return detail::insert_with_hint(
        *this, m_.cont_, hint, value, get_growth_policy());
  }

  iterator insert(const_iterator hint, value_type&& value) {
    return detail::insert_with_hint(
        *this, m_.cont_, hint, std::move(value), get_growth_policy());
  }

  template <class InputIterator>
  void insert(InputIterator first, InputIterator last) {
    detail::bulk_insert(*this, m_.cont_, first, last);
  }

  // If [first, last) is known to be sorted and unique according to the
  // comparator (for example if the range comes from a sorted container of the
  // same type) this version can save unnecessary operations, especially if
  // *this is empty.
  template <class InputIterator>
  void insert(sorted_unique_t, InputIterator first, InputIterator last) {
    detail::bulk_insert(
        *this, m_.cont_, first, last, /* range_is_sorted_unique */ true);
  }

  void insert(std::initializer_list<value_type> ilist) {
    insert(ilist.begin(), ilist.end());
  }

  // emplace isn't better than insert for sorted_vector_map, but aids
  // compatibility
  template <typename... Args>
  std::pair<iterator, bool> emplace(Args&&... args) {
    folly::aligned_storage_for_t<value_type> b;
    value_type* p = static_cast<value_type*>(static_cast<void*>(&b));
    auto a = get_allocator();
    std::allocator_traits<allocator_type>::construct(
        a, p, std::forward<Args>(args)...);
    auto g = makeGuard([&]() {
      std::allocator_traits<allocator_type>::destroy(a, p);
    });
    return insert(std::move(*p));
  }

  std::pair<iterator, bool> emplace(const value_type& value) {
    return insert(value);
  }

  std::pair<iterator, bool> emplace(value_type&& value) {
    return insert(std::move(value));
  }

  // emplace_hint isn't better than insert for sorted_vector_set, but aids
  // compatibility
  template <typename... Args>
  iterator emplace_hint(const_iterator hint, Args&&... args) {
    folly::aligned_storage_for_t<value_type> b;
    value_type* p = static_cast<value_type*>(static_cast<void*>(&b));
    auto a = get_allocator();
    std::allocator_traits<allocator_type>::construct(
        a, p, std::forward<Args>(args)...);
    auto g = makeGuard([&]() {
      std::allocator_traits<allocator_type>::destroy(a, p);
    });
    return insert(hint, std::move(*p));
  }

  iterator emplace_hint(const_iterator hint, const value_type& value) {
    return insert(hint, value);
  }

  iterator emplace_hint(const_iterator hint, value_type&& value) {
    return insert(hint, std::move(value));
  }

  template <typename... Args>
  std::pair<iterator, bool> try_emplace(key_type&& k, Args&&... args) {
    return try_emplace_impl(std::move(k), std::forward<Args>(args)...);
  }

  template <typename... Args>
  std::pair<iterator, bool> try_emplace(const key_type& k, Args&&... args) {
    return try_emplace_impl(k, std::forward<Args>(args)...);
  }

  template <typename M>
  std::pair<iterator, bool> insert_or_assign(const key_type& k, M&& obj) {
    auto itAndInserted = try_emplace(k, std::forward<M>(obj));
    if (!itAndInserted.second) {
      itAndInserted.first->second = std::forward<M>(obj);
    }
    return itAndInserted;
  }

  template <typename M>
  std::pair<iterator, bool> insert_or_assign(key_type&& k, M&& obj) {
    auto itAndInserted = try_emplace(std::move(k), std::forward<M>(obj));
    if (!itAndInserted.second) {
      itAndInserted.first->second = std::forward<M>(obj);
    }
    return itAndInserted;
  }

  template <class M>
  iterator insert_or_assign(const_iterator hint, const key_type& k, M&& obj) {
    return insert_or_assign_impl(hint, k, std::forward<M>(obj));
  }

  template <class M>
  iterator insert_or_assign(const_iterator hint, key_type&& k, M&& obj) {
    return insert_or_assign_impl(hint, std::move(k), std::forward<M>(obj));
  }

  size_type erase(const key_type& key) {
    iterator it = find(key);
    if (it == end()) {
      return 0;
    }
    m_.cont_.erase(it);
    return 1;
  }

  iterator erase(const_iterator it) { return m_.cont_.erase(it); }

  iterator erase(const_iterator first, const_iterator last) {
    return m_.cont_.erase(first, last);
  }

  template <class Predicate>
  friend size_type erase_if(sorted_vector_map& container, Predicate predicate) {
    auto& c = container.m_.cont_;
    const auto preEraseSize = c.size();
    c.erase(std::remove_if(c.begin(), c.end(), std::ref(predicate)), c.end());
    return preEraseSize - c.size();
  }

  iterator find(const key_type& key) [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return find_(*this, key);
  }

  const_iterator find(const key_type& key) const
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return find_(*this, key);
  }

  template <typename K>
    requires is_transparent_v<Compare>
  iterator find(const K& key) [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return find_(*this, key);
  }

  template <typename K>
    requires is_transparent_v<Compare>
  const_iterator find(const K& key) const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return find_(*this, key);
  }

  std::pair<iterator, iterator> find(
      const key_type& key1, const key_type& key2) {
    if (key_comp()(key2, key1)) {
      auto iterators = find2_(*this, key2, key1);
      access::swap(iterators.first, iterators.second);
      return iterators;
    } else {
      return find2_(*this, key1, key2);
    }
  }

  std::pair<const_iterator, const_iterator> find(
      const key_type& key1, const key_type& key2) const {
    if (key_comp()(key2, key1)) {
      auto iterators = find2_(*this, key2, key1);
      access::swap(iterators.first, iterators.second);
      return iterators;
    } else {
      return find2_(*this, key1, key2);
    }
  }

  template <typename K>
    requires is_transparent_v<Compare>
  std::pair<iterator, iterator> find(const K& key1, const K& key2) {
    if (key_comp()(key2, key1)) {
      auto iterators = find2_(*this, key2, key1);
      access::swap(iterators.first, iterators.second);
      return iterators;
    } else {
      return find2_(*this, key1, key2);
    }
  }

  template <typename K>
    requires is_transparent_v<Compare>
  std::pair<const_iterator, const_iterator> find(
      const K& key1, const K& key2) const {
    if (key_comp()(key2, key1)) {
      auto iterators = find2_(*this, key2, key1);
      access::swap(iterators.first, iterators.second);
      return iterators;
    } else {
      return find2_(*this, key1, key2);
    }
  }

  mapped_type& at(const key_type& key) [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    iterator it = find(key);
    if (it != end()) {
      return it->second;
    }
    throw_exception<std::out_of_range>("sorted_vector_map::at");
  }

  const mapped_type& at(const key_type& key) const
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    const_iterator it = find(key);
    if (it != end()) {
      return it->second;
    }
    throw_exception<std::out_of_range>("sorted_vector_map::at");
  }

  size_type count(const key_type& key) const {
    return find(key) == end() ? 0 : 1;
  }

  template <typename K>
    requires is_transparent_v<Compare>
  size_type count(const K& key) const {
    return find(key) == end() ? 0 : 1;
  }

  bool contains(const key_type& key) const { return find(key) != end(); }

  template <typename K>
    requires is_transparent_v<Compare>
  bool contains(const K& key) const {
    return find(key) != end();
  }

  iterator lower_bound(const key_type& key) [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return lower_bound(*this, key);
  }

  const_iterator lower_bound(const key_type& key) const
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return lower_bound(*this, key);
  }

  template <typename K>
    requires is_transparent_v<Compare>
  iterator lower_bound(const K& key) [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return lower_bound(*this, key);
  }

  template <typename K>
    requires is_transparent_v<Compare>
  const_iterator lower_bound(const K& key) const
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return lower_bound(*this, key);
  }

  iterator upper_bound(const key_type& key) [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return upper_bound(*this, key);
  }

  const_iterator upper_bound(const key_type& key) const
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return upper_bound(*this, key);
  }

  template <typename K>
    requires is_transparent_v<Compare>
  iterator upper_bound(const K& key) [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return upper_bound(*this, key);
  }

  template <typename K>
    requires is_transparent_v<Compare>
  const_iterator upper_bound(const K& key) const
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return upper_bound(*this, key);
  }

  std::pair<iterator, iterator> equal_range(const key_type& key) {
    return equal_range(*this, key);
  }

  std::pair<const_iterator, const_iterator> equal_range(
      const key_type& key) const {
    return equal_range(*this, key);
  }

  template <typename K>
    requires is_transparent_v<Compare>
  std::pair<iterator, iterator> equal_range(const K& key) {
    return equal_range(*this, key);
  }

  template <typename K>
    requires is_transparent_v<Compare>
  std::pair<const_iterator, const_iterator> equal_range(const K& key) const {
    return equal_range(*this, key);
  }

  // Nothrow as long as swap() on the Compare type is nothrow.
  void swap(sorted_vector_map& o) {
    using std::swap; // Allow ADL for swap(); fall back to std::swap().
    Compare& a = m_;
    Compare& b = o.m_;
    swap(a, b);
    m_.cont_.swap(o.m_.cont_);
  }

  mapped_type& operator[](const key_type& key)
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    iterator it = lower_bound(key);
    if (it == end() || key_comp()(key, it->first)) {
      return insert(it, value_type(key, mapped_type()))->second;
    }
    return it->second;
  }

  bool operator==(const sorted_vector_map& other) const {
    return m_.cont_ == other.m_.cont_;
  }
  bool operator!=(const sorted_vector_map& other) const {
    return !operator==(other);
  }

  bool operator<(const sorted_vector_map& other) const {
    return m_.cont_ < other.m_.cont_;
  }
  bool operator>(const sorted_vector_map& other) const { return other < *this; }
  bool operator<=(const sorted_vector_map& other) const {
    return !operator>(other);
  }
  bool operator>=(const sorted_vector_map& other) const {
    return !operator<(other);
  }

  template <typename U = Container>
  friend auto operator<=>(
      const sorted_vector_map& lhs, const sorted_vector_map& rhs)
      -> decltype(std::declval<const U&>() <=> std::declval<const U&>()) {
    return lhs.m_.cont_ <=> rhs.m_.cont_;
  }

  // Mutable overload; do not mutate keys or otherwise break the sorted order
  // through the pointer.
  value_type* data() noexcept { return m_.cont_.data(); }
  const value_type* data() const noexcept { return m_.cont_.data(); }

 private:
  // This is to get the empty base optimization; see the comment in
  // sorted_vector_set.
  struct EBO : value_compare {
    explicit EBO(const value_compare& c, const Allocator& alloc) noexcept(
        std::is_nothrow_default_constructible<Container>::value)
        : value_compare(c), cont_(alloc) {}
    EBO(const EBO& other, const Allocator& alloc) noexcept(
        std::is_nothrow_constructible<
            Container,
            const Container&,
            const Allocator&>::value)
        : value_compare(static_cast<const value_compare&>(other)),
          cont_(other.cont_, alloc) {}
    EBO(EBO&& other, const Allocator& alloc) noexcept(
        std::is_nothrow_constructible<
            Container,
            Container&&,
            const Allocator&>::value)
        : value_compare(static_cast<value_compare&&>(other)),
          cont_(std::move(other.cont_), alloc) {}
    EBO(const Compare& c, Container&& cont) noexcept(
        std::is_nothrow_move_constructible<Container>::value)
        : value_compare(c), cont_(std::move(cont)) {}
    Container cont_;
  } m_;

  template <typename Self>
  using self_iterator_t = _t<
      std::conditional<std::is_const<Self>::value, const_iterator, iterator>>;

  template <typename Self, typename K>
  static self_iterator_t<Self> find_(Self& self, K const& key) {
    auto end = self.end();
    auto it = self.lower_bound(key);
    if (it == end || !self.key_comp()(key, it->first)) {
      return it;
    }
    return end;
  }

  template <typename Self, typename K>
  static self_iterator_t<Self> lower_bound(Self& self, K const& key) {
    auto f = [c = self.key_comp()](auto const& a, K const& b) {
      return c(a.first, b);
    };
    return std::lower_bound(self.begin(), self.end(), key, f);
  }

  template <typename Self, typename K>
  static self_iterator_t<Self> upper_bound(Self& self, K const& key) {
    auto f = [c = self.key_comp()](K const& a, auto const& b) {
      return c(a, b.first);
    };
    return std::upper_bound(self.begin(), self.end(), key, f);
  }

  template <typename Self, typename K>
  static std::pair<self_iterator_t<Self>, self_iterator_t<Self>> equal_range(
      Self& self, K const& key) {
    // Note: std::equal_range can't be passed a functor that takes
    // argument types different from the iterator value_type, so we
    // have to do this.
    return {lower_bound(self, key), upper_bound(self, key)};
  }

  template <typename Self, typename K>
  static std::pair<self_iterator_t<Self>, self_iterator_t<Self>> lower_bound2_(
      Self& self, K const& key1, K const& key2) {
    auto len = self.size();
    auto first = self.begin(), second = self.begin();
    auto c = self.key_comp();
    assert(!c(key2, key1));
    while (true) {
      if (len == 0) {
        return std::make_pair(first, first);
      }
      auto half = len / 2;
      auto middle = first + half;
      if (c(middle->first, key1)) {
        first = middle + 1;
        half = len - half - 1;
      } else if (c(middle->first, key2)) {
        second = middle + (len & 1);
        len = half;
        break;
      }
      len = half;
    }
    while (len) {
      auto half = len / 2;
      auto middle1 = first + half;
      auto middle2 = second + half;
      if (c(middle1->first, key1)) {
        first = middle1 + (len & 1);
      }
      if (c(middle2->first, key2)) {
        second = middle2 + (len & 1);
      }
      len = half;
    }
    return std::make_pair(first, second);
  }

  template <typename Self, typename K>
  static std::pair<self_iterator_t<Self>, self_iterator_t<Self>> find2_(
      Self& self, K const& key1, K const& key2) {
    auto end = self.end();
    auto its = lower_bound2_(self, key1, key2);
    if (its.second != end) {
      if (self.key_comp()(key1, its.first->first)) {
        its.first = end;
      }
      if (self.key_comp()(key2, its.second->first)) {
        its.second = end;
      }
    } else if (its.first != end && self.key_comp()(key1, its.first->first)) {
      its.first = end;
    }
    return its;
  }

  template <typename K, typename... Args>
  std::pair<iterator, bool> try_emplace_impl(K&& key, Args&&... args) {
    iterator it = lower_bound(key);
    if (it == end() || key_comp()(key, it->first)) {
      return std::make_pair(
          emplace_hint(
              it,
              std::piecewise_construct,
              std::forward_as_tuple(std::forward<K>(key)),
              std::forward_as_tuple(std::forward<Args>(args)...)),
          true);
    }
    return std::make_pair(it, false);
  }

  template <class K, class M>
  iterator insert_or_assign_impl(const_iterator hint, K&& k, M&& obj) {
    if (hint == end() || key_comp()(k, hint->first)) {
      if (hint == begin() || key_comp()((hint - 1)->first, k)) {
        auto it = get_growth_policy().increase_capacity(m_.cont_, hint);
        return m_.cont_.emplace(
            it, std::make_pair(std::forward<K>(k), std::forward<M>(obj)));
      } else {
        return insert_or_assign(std::forward<K>(k), std::forward<M>(obj)).first;
      }
    }

    if (key_comp()(hint->first, k)) {
      if (hint + 1 == end() || key_comp()(k, (hint + 1)->first)) {
        auto it = get_growth_policy().increase_capacity(m_.cont_, hint + 1);
        return m_.cont_.emplace(
            it, std::make_pair(std::forward<K>(k), std::forward<M>(obj)));
      } else {
        return insert_or_assign(std::forward<K>(k), std::forward<M>(obj)).first;
      }
    }

    // Value and *hint did not compare, so they are equal keys.
    auto it = begin() + std::distance(cbegin(), hint);
    it->second = std::forward<M>(obj);
    return it;
  }
};

// Swap function that can be found using ADL.
template <class K, class V, class C, class A, class G>
inline void swap(
    sorted_vector_map<K, V, C, A, G>& a, sorted_vector_map<K, V, C, A, G>& b) {
  return a.swap(b);
}

template <typename T>
inline constexpr bool is_sorted_vector_map_v =
    is_instantiation_of_v<sorted_vector_map, T>;

template <typename T>
struct is_sorted_vector_map : std::bool_constant<is_sorted_vector_map_v<T>> {};

template <
    class Key,
    class Value,
    size_t N = 1,
    class Compare = std::less<Key>,
    class Allocator = std::allocator<std::pair<Key, Value>>,
    class GrowthPolicy = void,
    class SmallVectorPolicy = void>
using small_sorted_vector_map = sorted_vector_map<
    Key,
    Value,
    Compare,
    Allocator,
    GrowthPolicy,
    folly::small_vector<std::pair<Key, Value>, N, SmallVectorPolicy>>;

template <typename T>
inline constexpr bool is_small_sorted_vector_map_v =
    is_sorted_vector_map_v<T> && is_small_vector_v<typename T::container_type>;

template <typename T>
struct is_small_sorted_vector_map
    : std::bool_constant<is_small_sorted_vector_map_v<T>> {};

#if FOLLY_HAS_MEMORY_RESOURCE

namespace pmr {

template <
    class Key,
    class Value,
    class Compare = std::less<Key>,
    class GrowthPolicy = void,
    class Container = std::vector<
        std::pair<Key, Value>,
        std::pmr::polymorphic_allocator<std::pair<Key, Value>>>>
using sorted_vector_map = folly::sorted_vector_map<
    Key,
    Value,
    Compare,
    std::pmr::polymorphic_allocator<std::pair<Key, Value>>,
    GrowthPolicy,
    Container>;

} // namespace pmr

#endif

//////////////////////////////////////////////////////////////////////

} // namespace folly
