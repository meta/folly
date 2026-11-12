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
 * This header defines two new containers, heap_vector_set and heap_vector_map
 * classes. These containers are designed to be a drop-in replacement of
 * sorted_vector_set and sorted_vector_map. Similarly to sorted_vector_map/set,
 * heap_vector_map/set models AssociativeContainers. Below, we list important
 * differences from std::set and std::map (also documented in
 * folly/sorted_vector_types.h):
 *
 *   - insert() and erase() invalidate iterators and references.
 *   - erase(iterator) returns an iterator pointing to the next valid element.
 *   - insert() and erase() are O(N)
 *   - our iterators model RandomAccessIterator
 *   - heap_vector_map::value_type is pair<K,V>, not pair<const K,V>.
 *     (This is basically because we want to store the value_type in
 *     std::vector<>, which requires it to be Assignable.)
 *   - insert() single key variants, emplace(), and emplace_hint() only provide
 *     the strong exception guarantee (unchanged when exception is thrown) when
 *     std::is_nothrow_move_constructible<value_type>::value is true.
 *
 * heap_vector_map/set have exactly the same size as sorted_vector_map/set.
 * These containers utilizes a vector container (e.g. std::vector) to store the
 * values. Heap containers (similarly to sorted containers) have no additional
 * memory overhead. They lay out the data in an optimal way w.r.t locality (see
 * https://algorithmica.org/en/eytzinger), called eytzinger or heap order. For
 * example in a sorted_vector_set, the underlying vector contains:
 *              index    0   1   2   3   4   5   6   7   8   9
 *       vector[index]   0, 10, 20, 30, 40, 50, 60, 70, 80, 90
 * while in a heap_vector_set, the underlying vector contains:
 *              index    0   1   2   3   4   5   6   7   8   9
 *       vector[index]  60, 30, 80, 10, 50, 70, 90,  0, 20, 40
 * Lookup elements in sorted vector containers relies on binary search,
 * std::lower_bound. While in heap containers, lookup operation has two
 * benefits:
 *
 * 1. Cache locality, the container is traversed sequentially instead of binary
 * search that jumps around the sorted vector.
 * 2. The branches in a binary search are mispredicted resulting in hardware
 * penalty while using heap lookup search the branch can be avoided by using
 * cmov instruction. We observerd look up operations are up to 2X faster than
 * sorted_vector_map.
 *
 * However, Insertion/deletion operations are much slower. If insertions and
 * deletions are rare operations for your use case then heap containers might
 * be the right choice for you. Also, to minimize impact of insertions while
 * creating heap containers, we recommend not to insert element by element
 * instead first collect elements in a vector, then construct the heap map from
 * it.
 *
 * Another substantial trade off, inorder traversal of heap container elements
 * is slower than sorted vector containers. This is expected as heap map needs
 * to jump around to access map elements in order. A remedy is to use underlying
 * vector iterators. This works only when the order is irrelevant. For example,
 * using heap container iterator:
 *         for (auto& e: HeapSet)
 *           std::cout << e << ", ";
 * Prints:  0, 10, 20, 30, 40, 50, 60, 70, 80, 90
 * and using underlying vector container iterator:
 *         for (auto& e : HeapSet.iterate())
 *           std::cout << e << ", ";
 * Prints:  60, 30, 80, 10, 50, 70, 90,  0, 20, 40
 * The latter loop is the fastest traversal.
 *
 * Finally The main benefit of heap containers is a compact representation
 * that achieves fast random lookup. Use this map when lookup is the
 * dominant operation and at the same time saving memory is important.
 */

#pragma once

#include <algorithm>
#include <cassert>
#include <compare>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <folly/CppAttributes.h>
#include <folly/Range.h>
#include <folly/ScopeGuard.h>
#include <folly/Traits.h>
#include <folly/Utility.h>
#include <folly/container/Iterator.h>
#include <folly/container/range_traits.h>
#include <folly/functional/Invoke.h>
#include <folly/lang/Exception.h>
#include <folly/memory/MemoryResource.h>
#include <folly/portability/Builtins.h>
#include <folly/small_vector.h>

namespace folly {
template <
    typename Key,
    typename Value,
    typename Compare,
    typename Allocator,
    typename GrowthPolicy,
    typename Container>
class heap_vector_map;

namespace detail {

namespace heap_vector_detail {

/*
 * Heap Containers Helper Functions
 * ---------------------------------
 * Terminology:
 * - offset means the index at which element is stored in the container vector,
 *   following the heap order.
 * - index means the element rank following the container compare order.
 *
 * Introduction:
 * Heap order can be constructed from a sorted input vector using below
 * naive algorithm.
 *
 *     heapify(input, output, index = 0, offset = 1) {
 *       if (offset <= size) {
 *         index = heapify(input, output, i, 2 * offset);
 *         output[offset - 1] = input[index++];
 *         index = heapify(input, output, i, 2 * offset + 1);
 *       }
 *       return index;
 *     }
 *
 * Helper functions below implement efficient algorithms to:
 *  - Find offsets of the smallest/greatest elements.
 *  - Given an offset, calculate the offset where next/previous element is
 *  stored if it exists.
 *  - Given a start and an end offsets, calculate distance between their
 *  corresponding indexes.
 *  - Fast inplace heapification.
 *  - Insert a new element while preserving heap order.
 *  - Delete an element and preserve heap order.
 *  - find lower/upper bound of a key following container compare order.
 */

// Returns the offset of the smallest element in the heap container.
// The samllest element is stored at:
//     vector[2^n - 1] where 2^n <= size < 2^(n+1).
// firstOffset returns 2^n - 1 if size > 0, 0 otherwise
template <typename size_type>
size_type firstOffset(size_type size) {
  if (size) {
    return (1
            << (CHAR_BIT * sizeof(unsigned long) -
                __builtin_clzl((unsigned long)size) - 1)) -
        1;
  } else {
    return 0;
  }
}

// Returns the offset of greatest element in heap container.
// the greatest element is storted at:
//     vector[2^n - 2] if 2^n - 1 <= size < 2^(n+1)
// lastOffset returns 2^n - 2 if size > 0, 0 otherwise
template <typename size_type>
size_type lastOffset(size_type size) {
  if (size) {
    return ((size & (size + 1)) == 0 ? size : firstOffset(size)) - 1;
  }
  return 0;
}

// Returns the offset of the next element. It is calculated based on the
// size of the map.
// To simplify implementation, offset must be 1-based
// return value is also 1-based.
template <typename size_type>
size_type next(size_type offset, size_type size) {
  auto next = (2 * offset);
  if (next >= size) {
    return offset >> __builtin_ffsl((unsigned long)~offset);
  } else {
    next += 1;
    for (auto n = 2 * next; n <= size; n *= 2) {
      next = n;
    }
    return next;
  }
}

// Returns the offset of the previous element. It is calculated based on
// the size of the map.
// To simplify implementation, offset must be 1-based
// return value is also 1-based.
template <typename size_type>
size_type prev(size_type offset, size_type size) {
  auto prev = 2 * offset;
  if (prev <= size) {
    for (auto p = 2 * prev + 1; p <= size; p = 2 * p + 1) {
      if (2 * p >= size) {
        return p;
      }
    }
    return prev;
  }
  return offset >> __builtin_ffsl((unsigned long)offset);
}

// To avoid scanning all offsets, skip offsets that cannot be within the
// range of offset1 and offset2.
// Note We could compute least common ancestor of offset1 and offset2
// to further minimize scanning. However it is not profitable when container
// is relatively small.
template <typename size_type>
size_type getStartOffsetToScan(size_type offset1, size_type offset2) {
  while ((offset1 & (offset1 - 1)) != 0) {
    offset1 >>= 1;
  }
  if (offset1 > 1) {
    while ((offset2 & (offset2 - 1)) != 0) {
      offset2 >>= 1;
    }
    offset1 = std::min(offset1, offset2);
  }
  return offset1;
}

// Given a start and end offsets, returns the distance between
// their corresponding indexes.
template <typename Container, typename size_type>
typename Container::difference_type distance(
    Container& cont, size_type start, size_type end) {
  using difference_type = typename Container::difference_type;
  difference_type dist = 0;
  size_type size = cont.size();
  // To simplify logic base start and end from one.
  start++;
  end++;
  std::function<bool(size_type, size_type)> calculateDistance =
      [&](size_type offset, size_type lb) {
        if (offset > size) {
          return false;
        }
        for (; offset <= size; offset <<= 1) {
          ;
        }
        offset >>= 1;
        for (; offset > lb; offset >>= 1) {
          if (offset == start) {
            if (dist) {
              dist *= -1;
              return true;
            }
            dist = 1;
          } else if (offset == end) {
            if (dist) {
              return true;
            }
            dist = 1;
          } else if (dist) {
            dist++;
          }
          if (calculateDistance(2 * offset + 1, offset)) {
            return true;
          }
        }
        return false;
      };
  auto offset = getStartOffsetToScan(start, end);
  calculateDistance(offset, size_type(0));
  // Handle start == end()
  if (start > size) {
    dist *= -1;
  }

  return dist;
}

// Returns the offset for each index in heap container
// for example if size = 7 then
//       index     0  1  2  3  4  5  6
//     offsets = { 3, 1, 4, 0, 5, 2, 6 }
// The smallest element (index = 0) of heap container is stored at cont[3] and
// so on.
template <typename size_type, typename Offsets>
void getOffsets(size_type size, Offsets& offsets) {
  size_type i = 0;
  size_type offset = 0;
  size_type index = size;
  do {
    for (size_type o = offset; o < size; o = 2 * o + 2) {
      offsets[i++] = o;
    }
    offset = offsets[--i];
    offsets[--index] = offset;
    offset = 2 * offset + 1;
  } while (i || offset < size);
}

// Inplace conversion of a sorted vector to heap layout
// This algorithm utilizes circular swaps to position each element in its heap
// order offset in the vector. For example, given a sorted vector below:
//     cont = { 0, 10, 20, 30, 40, 50, 60, 70 }
// getOffsets returns:
//       index     0  1  2  3  4  5  6  7
//     offsets = { 4, 2, 6, 1, 3, 5, 7, 0 }
//
// The algorithm moves elements circularly:
// cont[4]->cont[0]->cont[7]->cont[6]->cont[2]->cont[1]->cont[3]-> cont[4]
// cont[5] remains inplace
// returns:
// cont = { 40, 20, 60, 10, 30, 50, 70, 0 }
template <class Container>
void heapify(Container& cont) {
  using size_type = typename Container::size_type;
  size_type size = cont.size();
  if (size == 0) [[unlikely]] {
    return;
  }
  std::vector<size_type> offsets;
  offsets.resize(size);
  getOffsets(size, offsets);

  std::function<void(size_type, size_type)> rotate =
      [&](size_type next, size_type index) {
        std::vector<size_type> worklist;
        while (index != next) {
          worklist.push_back(next);
          next = offsets[next];
        }
        while (!worklist.empty()) {
          auto cur = worklist.back();
          worklist.pop_back();
          cont[offsets[cur]] = std::move(cont[cur]);
          offsets[cur] = size;
        }
      };

  for (size_type index = 0; index < size; index++) {
    // already moved
    if (offsets[index] == size) {
      continue;
    }
    size_type next = offsets[index];
    if (next == index) {
      continue;
    }
    // Subtlety: operator[] returns a Container::reference. Because
    // Container::reference can be a proxy, using bare `auto` is not
    // sufficient to remove the "reference nature" of
    // Container::reference and force a move out of the container;
    // instead, we need Container::value_type.
    typename Container::value_type tmp = std::move(cont[index]);
    rotate(next, index);
    cont[next] = std::move(tmp);
  }
}

// Below helper functions to implement inplace insertion/deletion.

// Returns the sequence of offsets that need to be moved. This sequence
// is the range between size-1 and the offset of the inserted element.
// There are two cases: {size - 1, ..., offset} or {offset, ..., size - 1}
// For example if 45 is inserted at offset == 0 and size == 9.
// Before insertion:
//     element    0 10   20 30 40     50 60 70
//     offset     7  3    1  4  0      5  2  6
// After inserting 45:
//     element    0 10   20 30 40 45  50 60 70
//     offset     7  3    8  1  4  0   5  2  6
// This function returns:
//     offsets = { 8, 1, 4, 0 }
template <typename size_type, typename Offsets>
bool getOffsetRange(
    size_type size, Offsets& offsets, size_type offset, size_type current) {
  for (; current <= size; current <<= 1) {
    if (getOffsetRange(size, offsets, offset, 2 * current + 1)) {
      return true;
    }
    if (offsets.empty()) {
      if (offset == current || size == current) {
        // Start recording offsets that need to be moved.
        offsets.push_back(current - 1);
      }
    } else {
      // record offset
      offsets.push_back(current - 1);
      if (offset == current || size == current) {
        // Stop recording offsets
        return true;
      }
    }
  }
  return false;
}

template <typename size_type, typename Offsets>
void getOffsetRange(size_type size, Offsets& offsets, size_type offset) {
  auto start = getStartOffsetToScan(offset, size);
  getOffsetRange(size, offsets, offset, start);
}

// Insert a new element in heap order
// Assumption: the inserted element is already pushed at the back of the
// container vector (i.e. located at vector[size - 1]).
template <typename size_type, class Container>
size_type insert(size_type offset, Container& cont) {
  size_type size = cont.size();
  if (size == 1) {
    return 0;
  }
  size_type adjust = 1;
  if (offset == size - 1) {
    adjust = 0;
    auto last = lastOffset(size);
    if (last == offset) {
      return offset;
    }
    offset = last;
  }
  std::vector<size_type> offsets;
  offsets.reserve(size);
  getOffsetRange(size, offsets, offset + 1);
  typename Container::value_type v = std::move(cont[size - 1]);
  if (offsets[0] != offset) {
    for (size_type i = 1, e = offsets.size(); i < e; ++i) {
      cont[offsets[i - 1]] = std::move(cont[offsets[i]]);
    }
    cont[offset] = std::move(v);
    return offset;
  }
  for (size_type i = offsets.size() - 1; i > adjust; --i) {
    cont[offsets[i]] = std::move(cont[offsets[i - 1]]);
  }
  cont[offsets[adjust]] = std::move(v);
  return offsets[adjust];
}

// Erase one element and preserve heap order
template <typename size_type, class Container>
size_type erase(size_type offset, Container& cont) {
  size_type size = cont.size();
  if (offset + 1 == size) {
    auto ret = next(offset + 1, size);
    cont.resize(size - 1);
    return ret ? ret - 1 : size - 1;
  }
  std::vector<size_type> offsets;
  offsets.reserve(size);
  getOffsetRange(size, offsets, offset + 1);
  if (offsets[0] == offset) {
    for (size_type i = 1, e = offsets.size(); i < e; i++) {
      cont[offsets[i - 1]] = std::move(cont[offsets[i]]);
    }
  } else {
    for (size_type i = offsets.size() - 1; i > 0; --i) {
      cont[offsets[i]] = std::move(cont[offsets[i - 1]]);
    }
  }
  cont.resize(size - 1);
  return offset;
}

// Search lower bound in a container sorted in heap order.
// To speed up lower_bound for small containers, peel four iterations and use
// reverse compare to exit quickly.
// The branch inside the loop is converted to a cmov by the compiler. cmov are
// more efficient when the branch is unpredictable.
template <typename Compare, typename RCompare, typename Container>
typename Container::size_type lower_bound(
    Container& cont, Compare cmp, RCompare reverseCmp) {
  using size_type = typename Container::size_type;
  size_type size = cont.size();
  auto last = size;
  size_type offset = 0;
  if (size) {
    if (cmp(cont[offset])) {
      offset = 2 * offset + 2;
    } else {
      if (!reverseCmp(cont[offset])) {
        return offset;
      }
      last = offset;
      offset = 2 * offset + 1;
    }
    if (offset < size) {
      if (cmp(cont[offset])) {
        offset = 2 * offset + 2;
      } else {
        if (!reverseCmp(cont[offset])) {
          return offset;
        }
        last = offset;
        offset = 2 * offset + 1;
      }
      if (offset < size) {
        if (cmp(cont[offset])) {
          offset = 2 * offset + 2;
        } else {
          if (!reverseCmp(cont[offset])) {
            return offset;
          }
          last = offset;
          offset = 2 * offset + 1;
        }
        if (offset < size) {
          if (cmp(cont[offset])) {
            offset = 2 * offset + 2;
          } else {
            if (!reverseCmp(cont[offset])) {
              return offset;
            }
            last = offset;
            offset = 2 * offset + 1;
          }
          for (; offset < size; offset++) {
            if (cmp(cont[offset])) {
              offset = 2 * offset + 1;
            } else {
              last = offset;
              offset = 2 * offset;
            }
          }
        }
      }
    }
  }
  return last;
}

template <typename Compare, typename Container>
typename Container::size_type upper_bound(Container& cont, Compare cmp) {
  using size_type = typename Container::size_type;
  auto size = cont.size();
  auto last = size;
  for (size_type offset = 0; offset < size; offset++) {
    if (!cmp(cont[offset])) {
      offset = 2 * offset + 1;
    } else {
      last = offset;
      offset = 2 * offset;
    }
  }
  return last;
}

// Helper functions below are similar to sorted containers. Wherever
// applicable renamed to heap containers.

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

template <class OurContainer, class Container, class InputIterator>
void bulk_insert(
    OurContainer& sorted,
    Container& cont,
    InputIterator first,
    InputIterator last) {
  assert(first != last);

  auto const prev_size = cont.size();
  cont.insert(cont.end(), first, last);
  auto const middle = cont.begin() + prev_size;

  auto const& cmp(sorted.value_comp());
  if (!std::is_sorted(middle, cont.end(), cmp)) {
    std::sort(middle, cont.end(), cmp);
  }
  if (middle != cont.begin() && !cmp(*(middle - 1), *middle)) {
    std::inplace_merge(cont.begin(), middle, cont.end(), cmp);
  }
  cont.erase(
      std::unique(
          cont.begin(),
          cont.end(),
          [&](typename OurContainer::value_type const& a,
              typename OurContainer::value_type const& b) {
            return !cmp(a, b) && !cmp(b, a);
          }),
      cont.end());
  heapify(cont);
}

template <typename Container, typename Compare>
bool is_sorted_unique(Container const& container, Compare const& comp) {
  if (container.empty()) {
    return true;
  }
  auto const e = container.end();
  for (auto a = container.begin(), b = std::next(a); b != e; ++a, ++b) {
    if (!comp(*a, *b)) {
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

// class value_compare_map is used to compare map elements.
template <class Compare>
struct value_compare_map : Compare {
  template <typename... value_type>
  auto operator()(const value_type&... a) const
      noexcept(is_nothrow_invocable_v<const Compare&, decltype((a.first))...>)
          -> invoke_result_t<const Compare&, decltype((a.first))...> {
    return Compare::operator()(a.first...);
  }

  template <typename value_type>
  const auto& getKey(const value_type& a) const noexcept {
    return a.first;
  }

  explicit value_compare_map(const Compare& c) noexcept(
      std::is_nothrow_copy_constructible<Compare>::value)
      : Compare(c) {}
};

// wrapper class value_compare_set for set elements.
template <class Compare>
struct value_compare_set : Compare {
  using Compare::operator();

  template <typename value_type>
  value_type& getKey(value_type& a) const noexcept {
    return a;
  }

  explicit value_compare_set(const Compare& c) noexcept(
      std::is_nothrow_copy_constructible<Compare>::value)
      : Compare(c) {}
};

/**
 * A heap_vector_container is a container similar to std::set<>, but
 * implemented as a heap array with std::vector<>.
 * This class contains shared implementation between set and map. It
 * fully implements set methods and used as base class for map.
 *
 * @tparam T               Data type to store
 * @tparam Compare         Comparison function that imposes a
 *                              strict weak ordering over instances of T
 * @tparam Allocator       allocation policy
 * @tparam GrowthPolicy    policy object to control growth
 * @tparam Container       underlying vector where elements are stored
 * @tparam KeyT            key type, for set it is same as T.
 * @tparam ValueCompare    wrapper class to compare Container::value_type
 *
 */
template <
    class T,
    class Compare = std::less<T>,
    class Allocator = std::allocator<T>,
    class GrowthPolicy = void,
    class Container = std::vector<T, Allocator>,
    class KeyT = T,
    class ValueCompare = value_compare_set<Compare>>
class heap_vector_container : growth_policy_wrapper<GrowthPolicy> {
 protected:
  growth_policy_wrapper<GrowthPolicy>& get_growth_policy() { return *this; }

  struct EBO;

 public:
  using key_type = KeyT;
  using value_type = T;
  using key_compare = Compare;
  using value_compare = ValueCompare;
  using allocator_type = Allocator;
  using container_type = Container;
  using pointer = typename Container::pointer;
  using reference = typename Container::reference;
  using const_reference = typename Container::const_reference;
  using difference_type = typename Container::difference_type;
  using size_type = typename Container::size_type;

  // Defines inorder iterator for heap set.
  template <typename Iter>
  struct heap_iterator {
    using iterator_category = std::random_access_iterator_tag;
    using size_type = typename Container::size_type;
    using difference_type =
        typename std::iterator_traits<Iter>::difference_type;
    using value_type = typename std::iterator_traits<Iter>::value_type;
    using pointer = typename std::iterator_traits<Iter>::pointer;
    using reference = typename std::iterator_traits<Iter>::reference;

    heap_iterator() = default;
    template <typename C>
    heap_iterator(Iter ptr, C* cont) {
      ptr_ = ptr;
      cont_ = const_cast<Container*>(cont);
    }

    template <
        typename I2,
        typename = typename std::enable_if<
            std::is_same<typename Container::iterator, I2>::value>::type>
    /* implicit */ heap_iterator(const heap_iterator<I2>& rawIterator)
        : ptr_(rawIterator.ptr_), cont_(rawIterator.cont_) {}

    ~heap_iterator() = default;

    heap_iterator(const heap_iterator& rawIterator) = default;

    heap_iterator& operator=(const heap_iterator& rawIterator) = default;
    heap_iterator& operator=(Iter ptr) {
      assert(
          (ptr - cont_->begin()) >= 0 &&
          (ptr - cont_->begin()) <= (difference_type)cont_->size());
      ptr_ = ptr;
      return (*this);
    }

    bool operator==(const heap_iterator& rawIterator) const {
      return ptr_ == rawIterator.ptr_;
    }
    bool operator!=(const heap_iterator& rawIterator) const {
      return !operator==(rawIterator);
    }

    heap_iterator& operator+=(const difference_type& movement) {
      size_type offset = ptr_ - cont_->begin() + 1;
      auto size = cont_->size();
      if (movement < 0) {
        difference_type i = 0;

        if (offset - 1 == size) {
          // handle --end()
          offset = heap_vector_detail::lastOffset(size) + 1;
          i = -1;
        }
        for (; i > movement; i--) {
          offset = heap_vector_detail::prev(offset, size);
        }
      } else {
        for (difference_type i = 0; i < movement; i++) {
          offset = heap_vector_detail::next(offset, size);
        }
      }
      ptr_ = cont_->begin() + (offset == 0 ? cont_->size() : offset - 1);
      return (*this);
    }

    heap_iterator& operator-=(const difference_type& movement) {
      return operator+=(-movement);
    }
    heap_iterator& operator++() { return operator+=(1); }
    heap_iterator& operator--() { return operator-=(1); }
    heap_iterator operator++(int) {
      auto temp(*this);
      operator+=(1);
      return temp;
    }
    heap_iterator operator--(int) {
      auto temp(*this);
      operator-=(1);
      return temp;
    }
    heap_iterator operator+(const difference_type& movement) {
      auto temp(*this);
      temp += movement;
      return temp;
    }
    heap_iterator operator+(const difference_type& movement) const {
      auto temp(*this);
      temp += movement;
      return temp;
    }

    heap_iterator operator-(const difference_type& movement) {
      auto temp(*this);
      temp -= movement;
      return temp;
    }

    heap_iterator operator-(const difference_type& movement) const {
      auto temp(*this);
      temp -= movement;
      return temp;
    }

    difference_type operator-(const heap_iterator& rawIterator) {
      assert(cont_ == rawIterator.cont_);
      size_type offset0 = ptr_ - cont_->begin();
      size_type offset1 = rawIterator.ptr_ - cont_->begin();
      if (offset1 == offset0) {
        return 0;
      }
      return heap_vector_detail::distance(*cont_, offset1, offset0);
    }

    difference_type operator-(const heap_iterator& rawIterator) const {
      assert(cont_ == rawIterator.cont_);
      size_type offset0 = ptr_ - cont_->begin();
      size_type offset1 = rawIterator.ptr_ - cont_->begin();
      if (offset1 == offset0) {
        return 0;
      }
      return heap_vector_detail::distance(*cont_, offset1, offset0);
    }

    reference operator*() const { return *ptr_; }
    pointer operator->() const {
      if constexpr (std::is_pointer_v<Iter>) {
        return ptr_;
      } else {
        return ptr_.operator->();
      }
    }

   protected:
    template <typename I2>
    friend struct heap_iterator;

    template <
        typename T2,
        typename Compare2,
        typename Allocator2,
        typename GrowthPolicy2,
        typename Container2,
        typename KeyT2,
        typename ValueCompare2>
    friend class heap_vector_container;

    template <
        typename Key2,
        typename Value2,
        typename Compare2,
        typename Allocator2,
        typename GrowthPolicy2,
        typename Container2>
    friend class ::folly::heap_vector_map;

    Iter ptr_;
    Container* cont_;
  };

  using iterator = heap_iterator<typename Container::iterator>;
  using const_iterator = heap_iterator<typename Container::const_iterator>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  heap_vector_container() : m_(value_compare(Compare()), Allocator()) {}

  heap_vector_container(const heap_vector_container&) = default;

  heap_vector_container(
      const heap_vector_container& other, const Allocator& alloc)
      : m_(other.m_, alloc) {}

  heap_vector_container(heap_vector_container&&) = default;

  heap_vector_container(
      heap_vector_container&& other,
      const Allocator&
          alloc) noexcept(std::
                              is_nothrow_constructible<
                                  EBO,
                                  EBO&&,
                                  const Allocator&>::value)
      : m_(std::move(other.m_), alloc) {}

  explicit heap_vector_container(const Allocator& alloc)
      : m_(value_compare(Compare()), alloc) {}

  explicit heap_vector_container(
      const Compare& comp, const Allocator& alloc = Allocator())
      : m_(value_compare(comp), alloc) {}

  template <class InputIterator>
  explicit heap_vector_container(
      InputIterator first,
      InputIterator last,
      const Compare& comp = Compare(),
      const Allocator& alloc = Allocator())
      : m_(value_compare(comp), alloc) {
    insert(first, last);
  }

  template <class InputIterator>
  heap_vector_container(
      InputIterator first, InputIterator last, const Allocator& alloc)
      : m_(value_compare(Compare()), alloc) {
    insert(first, last);
  }

  /* implicit */ heap_vector_container(
      std::initializer_list<value_type> list,
      const Compare& comp = Compare(),
      const Allocator& alloc = Allocator())
      : m_(value_compare(comp), alloc) {
    insert(list.begin(), list.end());
  }

  heap_vector_container(
      std::initializer_list<value_type> list, const Allocator& alloc)
      : m_(value_compare(Compare()), alloc) {
    insert(list.begin(), list.end());
  }

  // Construct a heap_vector_container by stealing the storage of a prefilled
  // container. The container need not be sorted already. This supports
  // bulk construction of heap_vector_container with zero allocations, not
  // counting those performed by the caller.
  // Note that `heap_vector_container(const Container& container)` is not
  // provided, since the purpose of this constructor is to avoid an unnecessary
  // copy.
  explicit heap_vector_container(
      Container&& container,
      const Compare& comp =
          Compare()) noexcept(std::
                                  is_nothrow_constructible<
                                      EBO,
                                      value_compare,
                                      Container&&>::value)
      : heap_vector_container(
            sorted_unique,
            heap_vector_detail::as_sorted_unique(
                std::move(container), value_compare(comp)),
            comp) {}

  // Construct a heap_vector_container by stealing the storage of a prefilled
  // container. Its elements must be sorted and unique, as sorted_unique_t
  // hints. Supports bulk construction of heap_vector_container with zero
  // allocations, not counting those performed by the caller.
  // Note that `heap_vector_container(sorted_unique_t, const Container&
  // container)` is not provided, since the purpose of this constructor is to
  // avoid an extra copy.
  heap_vector_container(
      sorted_unique_t /* unused */,
      Container&& container,
      const Compare& comp =
          Compare()) noexcept(std::
                                  is_nothrow_constructible<
                                      EBO,
                                      value_compare,
                                      Container&&>::value)
      : m_(value_compare(comp), std::move(container)) {
    assert(heap_vector_detail::is_sorted_unique(m_.cont_, value_comp()));
    heap_vector_detail::heapify(m_.cont_);
  }

  Allocator get_allocator() const { return m_.cont_.get_allocator(); }

  const Container& get_container() const noexcept { return m_.cont_; }

  /**
   * Directly swap the container. Similar to swap()
   */
  void swap_container(Container& newContainer) {
    heap_vector_detail::as_sorted_unique(newContainer, value_comp());
    heap_vector_detail::heapify(newContainer);
    using std::swap;
    swap(m_.cont_, newContainer);
  }
  void swap_container(sorted_unique_t, Container& newContainer) {
    assert(heap_vector_detail::is_sorted_unique(newContainer, value_comp()));
    heap_vector_detail::heapify(newContainer);
    using std::swap;
    swap(m_.cont_, newContainer);
  }

  heap_vector_container& operator=(const heap_vector_container& other) =
      default;

  heap_vector_container& operator=(heap_vector_container&& other) = default;

  heap_vector_container& operator=(std::initializer_list<value_type> ilist) {
    clear();
    insert(ilist.begin(), ilist.end());
    return *this;
  }

  key_compare key_comp() const { return m_; }
  value_compare value_comp() const { return m_; }

  iterator begin() {
    if (size()) {
      return iterator(
          m_.cont_.begin() + heap_vector_detail::firstOffset(size()),
          &m_.cont_);
    }
    return iterator(m_.cont_.begin(), &m_.cont_);
  }
  iterator end() { return iterator(m_.cont_.end(), &m_.cont_); }
  const_iterator cbegin() const {
    if (size()) {
      return const_iterator(
          m_.cont_.cbegin() + heap_vector_detail::firstOffset(size()),
          &m_.cont_);
    }
    return const_iterator(m_.cont_.cbegin(), &m_.cont_);
  }
  const_iterator begin() const {
    if (size()) {
      return const_iterator(
          m_.cont_.cbegin() + heap_vector_detail::firstOffset(size()),
          &m_.cont_);
    }
    return const_iterator(m_.cont_.begin(), &m_.cont_);
  }
  const_iterator cend() const {
    return const_iterator(m_.cont_.cend(), &m_.cont_);
  }
  const_iterator end() const {
    return const_iterator(m_.cont_.end(), &m_.cont_);
  }
  reverse_iterator rbegin() { return reverse_iterator(end()); }
  reverse_iterator rend() { return reverse_iterator(begin()); }
  const_reverse_iterator rbegin() const {
    return const_reverse_iterator(end());
  }
  const_reverse_iterator rend() const {
    return const_reverse_iterator(begin());
  }
  const_reverse_iterator crbegin() const {
    return const_reverse_iterator(end());
  }
  const_reverse_iterator crend() const {
    return const_reverse_iterator(begin());
  }

  void clear() { return m_.cont_.clear(); }
  size_type size() const { return m_.cont_.size(); }
  size_type max_size() const { return m_.cont_.max_size(); }
  bool empty() const { return m_.cont_.empty(); }
  void reserve(size_type s) { return m_.cont_.reserve(s); }
  void shrink_to_fit() { m_.cont_.shrink_to_fit(); }
  size_type capacity() const { return m_.cont_.capacity(); }
  const value_type* data() const noexcept { return m_.cont_.data(); }

  std::pair<iterator, bool> insert(const value_type& value) {
    iterator it = lower_bound(m_.getKey(value));
    if (it == end() || value_comp()(value, *it)) {
      auto offset = it.ptr_ - m_.cont_.begin();
      get_growth_policy().increase_capacity(*this, it);
      m_.cont_.push_back(value);
      offset = heap_vector_detail::insert(offset, m_.cont_);
      it = m_.cont_.begin() + offset;
      return std::make_pair(it, true);
    }
    return std::make_pair(it, false);
  }

  std::pair<iterator, bool> insert(value_type&& value) {
    iterator it = lower_bound(m_.getKey(value));
    if (it == end() || value_comp()(value, *it)) {
      auto offset = it.ptr_ - m_.cont_.begin();
      get_growth_policy().increase_capacity(*this, it);
      m_.cont_.push_back(std::move(value));
      offset = heap_vector_detail::insert(offset, m_.cont_);
      it = m_.cont_.begin() + offset;
      return std::make_pair(it, true);
    }
    return std::make_pair(it, false);
  }
  /* There is no benefit of using hint. Keep it for compatibility
   * Ignore and insert */
  iterator insert(const_iterator /* hint */, const value_type& value) {
    return insert(value).first;
  }

  iterator insert(const_iterator /* hint */, value_type&& value) {
    return insert(std::move(value)).first;
  }

  template <class InputIterator>
  void insert(InputIterator first, InputIterator last) {
    if (first == last) {
      return;
    }
    if (iterator_has_known_distance_v<InputIterator, InputIterator> &&
        std::distance(first, last) == 1) {
      insert(*first);
      return;
    }
    std::sort(m_.cont_.begin(), m_.cont_.end(), value_comp());
    heap_vector_detail::bulk_insert(*this, m_.cont_, first, last);
  }

  void insert(std::initializer_list<value_type> ilist) {
    insert(ilist.begin(), ilist.end());
  }
  // emplace isn't better than insert for heap_vector_container, but aids
  // compatibility
  template <typename... Args>
  std::pair<iterator, bool> emplace(Args&&... args) {
    folly::aligned_storage_for_t<value_type> b;
    auto* p = static_cast<value_type*>(static_cast<void*>(&b));
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

  // emplace_hint isn't better than insert for heap_vector_container, but aids
  // compatibility
  template <typename... Args>
  iterator emplace_hint(const_iterator /* hint */, Args&&... args) {
    return emplace(std::forward<Args>(args)...).first;
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
    heap_vector_detail::erase(it.ptr_ - m_.cont_.begin(), m_.cont_);
    return 1;
  }

  iterator erase(const_iterator it) {
    auto offset =
        heap_vector_detail::erase(it.ptr_ - m_.cont_.begin(), m_.cont_);
    iterator ret = end();
    ret = m_.cont_.begin() + offset;
    return ret;
  }

  iterator erase(const_iterator first, const_iterator last) {
    if (first == last) {
      return end();
    }
    auto dist = last - first;
    if (dist <= 0) {
      return end();
    }
    if (dist == 1) {
      return erase(first);
    }
    auto it = m_.cont_.begin() + (first - begin());
    std::sort(m_.cont_.begin(), m_.cont_.end(), value_comp());
    it = m_.cont_.erase(it, it + dist);
    heap_vector_detail::heapify(m_.cont_);
    return begin() + (it - m_.cont_.begin());
  }

  iterator find(const key_type& key) { return find_(*this, key); }

  const_iterator find(const key_type& key) const { return find_(*this, key); }

  template <typename K>
    requires is_transparent_v<Compare>
  iterator find(const K& key) {
    return find_(*this, key);
  }

  template <typename K>
    requires is_transparent_v<Compare>
  const_iterator find(const K& key) const {
    return find_(*this, key);
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

  iterator lower_bound(const key_type& key) { return lower_bound(*this, key); }

  const_iterator lower_bound(const key_type& key) const {
    return lower_bound(*this, key);
  }

  template <typename K>
    requires is_transparent_v<Compare>
  iterator lower_bound(const K& key) {
    return lower_bound(*this, key);
  }

  template <typename K>
    requires is_transparent_v<Compare>
  const_iterator lower_bound(const K& key) const {
    return lower_bound(*this, key);
  }

  iterator upper_bound(const key_type& key) { return upper_bound(*this, key); }

  const_iterator upper_bound(const key_type& key) const {
    return upper_bound(*this, key);
  }

  template <typename K>
    requires is_transparent_v<Compare>
  iterator upper_bound(const K& key) {
    return upper_bound(*this, key);
  }

  template <typename K>
    requires is_transparent_v<Compare>
  const_iterator upper_bound(const K& key) const {
    return upper_bound(*this, key);
  }

  std::pair<iterator, iterator> equal_range(const key_type& key) {
    return {lower_bound(key), upper_bound(key)};
  }

  std::pair<const_iterator, const_iterator> equal_range(
      const key_type& key) const {
    return {lower_bound(key), upper_bound(key)};
  }

  template <typename K>
    requires is_transparent_v<Compare>
  std::pair<iterator, iterator> equal_range(const K& key) {
    return {lower_bound(key), upper_bound(key)};
  }

  template <typename K>
    requires is_transparent_v<Compare>
  std::pair<const_iterator, const_iterator> equal_range(const K& key) const {
    return {lower_bound(key), upper_bound(key)};
  }

  void swap(heap_vector_container& o) noexcept(
      std::is_nothrow_swappable<Compare>::value &&
      noexcept(std::declval<Container&>().swap(std::declval<Container&>()))) {
    using std::swap; // Allow ADL for swap(); fall back to std::swap().
    Compare& a = m_;
    Compare& b = o.m_;
    swap(a, b);
    m_.cont_.swap(o.m_.cont_);
  }

  bool operator==(const heap_vector_container& other) const {
    return m_.cont_ == other.m_.cont_;
  }

  bool operator!=(const heap_vector_container& other) const {
    return !operator==(other);
  }

  auto operator<=>(const heap_vector_container& other) const {
    return std::lexicographical_compare_three_way(
        begin(),
        end(),
        other.begin(),
        other.end(),
        [cmp = value_comp()](const auto& a, const auto& b) {
          if (cmp(a, b)) {
            return std::weak_ordering::less;
          } else if (cmp(b, a)) {
            return std::weak_ordering::greater;
          }
          return std::weak_ordering::equivalent;
        });
  }

  // Use underlying vector iterators to quickly traverse heap container.
  // Note elements are traversed following the heap order, i.e., memory
  // storage order.
  Range<typename Container::iterator> iterate() noexcept {
    return Range<typename Container::iterator>(
        m_.cont_.begin(), m_.cont_.end());
  }

  const Range<typename Container::const_iterator> iterate() const noexcept {
    return Range<typename Container::const_iterator>(
        m_.cont_.begin(), m_.cont_.end());
  }

  // Contiguous view of all elements in heap (storage) order, as with
  // iterate(): {data(), size()}. Invalidated by any mutation, like iterators.
  //
  // Only available when the underlying container exposes data() and size().
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

 protected:
  // This is to get the empty base optimization
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
  using self_iterator_t = typename std::
      conditional<std::is_const<Self>::value, const_iterator, iterator>::type;

  template <typename Self, typename K>
  static self_iterator_t<Self> find_(Self& self, K const& key) {
    self_iterator_t<Self> end = self.end();
    self_iterator_t<Self> it = self.lower_bound(key);
    if (it == end || !self.key_comp()(key, self.m_.getKey(*it))) {
      return it;
    }
    return end;
  }

  template <typename Self, typename K>
  static self_iterator_t<Self> lower_bound(Self& self, K const& key) {
    auto c = self.key_comp();
    auto cmp = [&](auto const& a) { return c(self.m_.getKey(a), key); };
    auto reverseCmp = [&](auto const& a) { return c(key, self.m_.getKey(a)); };
    auto offset =
        heap_vector_detail::lower_bound(self.m_.cont_, cmp, reverseCmp);
    self_iterator_t<Self> ret = self.end();
    ret = self.m_.cont_.begin() + offset;
    return ret;
  }

  template <typename Self, typename K>
  static self_iterator_t<Self> upper_bound(Self& self, K const& key) {
    auto c = self.key_comp();
    auto cmp = [&](auto const& a) { return c(key, self.m_.getKey(a)); };
    auto offset = heap_vector_detail::upper_bound(self.m_.cont_, cmp);
    self_iterator_t<Self> ret = self.end();
    ret = self.m_.cont_.begin() + offset;
    return ret;
  }
};

} // namespace heap_vector_detail

} // namespace detail

/* heap_vector_set is a specialization of heap_vector_container
 *
 * @tparam T               Data type to store
 * @tparam Compare         Comparison function that imposes a
 *                              strict weak ordering over instances of T
 * @tparam Allocator       allocation policy
 * @tparam GrowthPolicy    policy object to control growth
 * @tparam Container       underlying vector where elements are stored
 */
template <
    class T,
    class Compare = std::less<T>,
    class Allocator = std::allocator<T>,
    class GrowthPolicy = void,
    class Container = std::vector<T, Allocator>>
class heap_vector_set
    : public detail::heap_vector_detail::heap_vector_container<
          T,
          Compare,
          Allocator,
          GrowthPolicy,
          Container,
          T,
          detail::heap_vector_detail::value_compare_set<Compare>> {
 private:
  using heap_vector_container =
      detail::heap_vector_detail::heap_vector_container<
          T,
          Compare,
          Allocator,
          GrowthPolicy,
          Container,
          T,
          detail::heap_vector_detail::value_compare_set<Compare>>;

 public:
  using heap_vector_container::as_span;
  using heap_vector_container::heap_vector_container;

  // Non-const overload; elements stay const, like set iterators.
  std::span<T const> as_span() noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]]
    requires requires(Container& c) {
      c.data();
      c.size();
    }
  {
    static_assert(is_contiguous_range_v<Container>);
    return {this->data(), this->size()};
  }
};

// Swap function that can be found using ADL.
template <class T, class C, class A, class G>
inline void swap(
    heap_vector_set<T, C, A, G>& a, heap_vector_set<T, C, A, G>& b) noexcept {
  return a.swap(b);
}

template <typename T>
inline constexpr bool is_heap_vector_set_v =
    is_instantiation_of_v<heap_vector_set, T>;

template <typename T>
struct is_heap_vector_set : std::bool_constant<is_heap_vector_set_v<T>> {};

#if FOLLY_HAS_MEMORY_RESOURCE

namespace pmr {

template <
    class T,
    class Compare = std::less<T>,
    class GrowthPolicy = void,
    class Container = std::vector<T, std::pmr::polymorphic_allocator<T>>>
using heap_vector_set = folly::heap_vector_set<
    T,
    Compare,
    std::pmr::polymorphic_allocator<T>,
    GrowthPolicy,
    Container>;

} // namespace pmr

#endif

//////////////////////////////////////////////////////////////////////

/**
 * A heap_vector_map based on heap layout.
 *
 * @tparam Key           Key type
 * @tparam Value         Value type
 * @tparam Compare       Function that can compare key types and impose
 *                            a strict weak ordering over them.
 * @tparam Allocator     allocation policy
 * @tparam GrowthPolicy  policy object to control growth
 *
 */

template <
    class Key,
    class Value,
    class Compare = std::less<Key>,
    class Allocator = std::allocator<std::pair<Key, Value>>,
    class GrowthPolicy = void,
    class Container = std::vector<std::pair<Key, Value>, Allocator>>
class heap_vector_map
    : public detail::heap_vector_detail::heap_vector_container<
          typename Container::value_type,
          Compare,
          Allocator,
          GrowthPolicy,
          Container,
          Key,
          detail::heap_vector_detail::value_compare_map<Compare>> {
 public:
  using key_type = Key;
  using mapped_type = Value;
  using value_type = typename Container::value_type;
  using key_compare = Compare;
  using allocator_type = Allocator;
  using container_type = Container;
  using pointer = typename Container::pointer;
  using reference = typename Container::reference;
  using const_reference = typename Container::const_reference;
  using difference_type = typename Container::difference_type;
  using size_type = typename Container::size_type;
  using value_compare = detail::heap_vector_detail::value_compare_map<Compare>;

 protected:
  using heap_vector_container =
      detail::heap_vector_detail::heap_vector_container<
          value_type,
          key_compare,
          Allocator,
          GrowthPolicy,
          Container,
          key_type,
          value_compare>;
  using heap_vector_container::get_growth_policy;
  using heap_vector_container::m_;

 public:
  using iterator = typename heap_vector_container::iterator;
  using const_iterator = typename heap_vector_container::const_iterator;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  // Since heap_vector_container methods are publicly available through
  // inheritance, just expose method used within this class.
  using heap_vector_container::as_span;
  using heap_vector_container::data;
  using heap_vector_container::end;
  using heap_vector_container::find;
  using heap_vector_container::heap_vector_container;
  using heap_vector_container::key_comp;
  using heap_vector_container::lower_bound;

  // Mutable overload; do not mutate keys through the pointer.
  value_type* data() noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return std::to_address(m_.cont_.data());
  }

  // Mutable overload; do not mutate keys through the span.
  std::span<value_type> as_span() noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]]
    requires requires(Container& c) {
      c.data();
      c.size();
    }
  {
    static_assert(is_contiguous_range_v<Container>);
    return {data(), this->size()};
  }

  mapped_type& at(const key_type& key) [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    iterator it = find(key);
    if (it != end()) {
      return it->second;
    }
    throw_exception<std::out_of_range>("heap_vector_map::at");
  }

  const mapped_type& at(const key_type& key) const
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    const_iterator it = find(key);
    if (it != end()) {
      return it->second;
    }
    throw_exception<std::out_of_range>("heap_vector_map::at");
  }

  mapped_type& operator[](const key_type& key)
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    iterator it = lower_bound(key);
    if (it == end() || key_comp()(key, it->first)) {
      auto offset = it.ptr_ - m_.cont_.begin();
      get_growth_policy().increase_capacity(*this, it);
      m_.cont_.emplace_back(key, mapped_type());
      offset = detail::heap_vector_detail::insert(offset, m_.cont_);
      it = m_.cont_.begin() + offset;
      return it->second;
    }
    return it->second;
  }
};

// Swap function that can be found using ADL.
template <class K, class V, class C, class A, class G>
inline void swap(
    heap_vector_map<K, V, C, A, G>& a,
    heap_vector_map<K, V, C, A, G>& b) noexcept {
  return a.swap(b);
}

template <typename T>
inline constexpr bool is_heap_vector_map_v =
    is_instantiation_of_v<heap_vector_map, T>;

template <typename T>
struct is_heap_vector_map : std::bool_constant<is_heap_vector_map_v<T>> {};

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
using heap_vector_map = folly::heap_vector_map<
    Key,
    Value,
    Compare,
    std::pmr::polymorphic_allocator<std::pair<Key, Value>>,
    GrowthPolicy,
    Container>;

} // namespace pmr

#endif

// Specialize heap_vector_map to integral key type and std::less comparaison.
// small_heap_map achieve a very fast find for small map < 200 elements.
template <
    typename Key,
    typename Value,
    typename SizeType = uint32_t,
    class Container = folly::small_vector<
        std::pair<Key, Value>,
        0,
        folly::small_vector_policy::policy_size_type<SizeType>>,
    typename = std::enable_if_t<
        std::is_integral<Key>::value || std::is_enum<Key>::value>>
class small_heap_vector_map
    : public folly::heap_vector_map<
          Key,
          Value,
          std::less<Key>,
          typename Container::allocator_type,
          void,
          Container> {
 public:
  using key_type = Key;
  using mapped_type = Value;
  using value_type = typename Container::value_type;
  using key_compare = std::less<Key>;
  using allocator_type = typename Container::allocator_type;
  using container_type = Container;
  using pointer = typename Container::pointer;
  using reference = typename Container::reference;
  using const_reference = typename Container::const_reference;
  using difference_type = typename Container::difference_type;
  using size_type = typename Container::size_type;
  using value_compare =
      detail::heap_vector_detail::value_compare_map<std::less<Key>>;

 private:
  using heap_vector_map = folly::
      heap_vector_map<Key, Value, key_compare, allocator_type, void, Container>;

  using heap_vector_map::m_;

 public:
  using iterator = typename heap_vector_map::iterator;
  using const_iterator = typename heap_vector_map::const_iterator;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  using heap_vector_map::begin;
  using heap_vector_map::heap_vector_map;
  using heap_vector_map::size;
  iterator find(const key_type key) {
    auto offset = find_(*this, key);
    iterator ret = begin();
    ret = m_.cont_.begin() + offset;
    return ret;
  }

  const_iterator find(const key_type key) const {
    auto offset = find_(*this, key);
    const_iterator ret = begin();
    ret = m_.cont_.begin() + offset;
    return ret;
  }

 private:
  template <typename Self, typename K>
  static inline size_type find_(Self& self, K const key) {
    auto size = self.size();
    if (!size) {
      return 0;
    }
    auto& cont = self.m_.cont_;
    size_type offset = 1;
    auto cur_k = self.m_.getKey(cont[0]);
    for (int i = 0; i < 6; i++) {
      auto o = offset;
      offset = 2 * offset;
      if (cur_k <= key) {
        ++offset;
        if (cur_k == key) {
          return o - 1;
        }
      }
      if (offset > size) {
        return size;
      }
      cur_k = self.m_.getKey(cont[offset - 1]);
    }
    while (true) {
      auto lt = cur_k < key;
      if (cur_k == key) {
        return offset - 1;
      }
      offset = 2 * offset + lt;
      if (offset > size) {
        return size;
      }
      cur_k = self.m_.getKey(cont[offset - 1]);
    }
    return size;
  }
};

} // namespace folly
