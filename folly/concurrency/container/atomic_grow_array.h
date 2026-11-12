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

#include <atomic>
#include <cstddef>
#include <iterator>
#include <new>
#include <span>
#include <thread>

#include <folly/CPortability.h>
#include <folly/ConstexprMath.h>
#include <folly/Likely.h>
#include <folly/ScopeGuard.h>
#include <folly/lang/Align.h>
#include <folly/lang/Bits.h>
#include <folly/lang/New.h>
#include <folly/synchronization/AtomicUtil.h>

namespace folly {

/// atomic_grow_array_policy_default
///
/// A default or example policy for use with atomic_grow_array.
template <typename Item, template <typename> class Atom = std::atomic>
struct atomic_grow_array_policy_default {
  template <typename V>
  using atom = Atom<V>;

  std::size_t grow(
      std::size_t /* const curr */, std::size_t const index) const noexcept {
    return nextPowTwo(index + 1);
  }
  Item make() const noexcept(noexcept(Item())) { return Item(); }
};

/// atomic_grow_array
///
/// A specialized data structure roughly modeling an infinite heap-allocated
/// array. The array is of course not actually infinite in size, but indexed
/// access at any index is always permitted. The container features both
/// reference-stability and iterator-stability.
///
/// Supports fast concurrent `operator[](size_t)`, which returns a reference to
/// the element at the given position. Indexed access is wait-free, modulo the
/// allocator algorithm and modulo element constructors. Indexed access has and
/// is intended to have a fast, minimal instruction sequence.
///
/// The array capacity may only grow and not shrink. When array capacity grows,
/// the array size grows to fill the capacity. This means that all the elements
/// that would be required to fill the capacity are allocated and constructed
/// during growth. Moreover, multiple threads may race to grow the capacity, in
/// which case only one thread wins the race - the threads losing the race then
/// destroy all elements they created in that round of racing to grow the array
/// capacity.
///
/// With the default policy featuring power-of-two exponential growth, the
/// number of outstanding allocations is:
///   log2(capacity)
/// And the outstanding allocations' sizes sum to:
///   log2(capacity) * 2 * sizeof(void*) <--- array-segment metadata
///   capacity * 2 * sizeof(void*) <--- array-segment pointers list
///   capacity * sizeof(value_type) <--- array-segment elements slab
/// Modulo allocator size-classes, of which this container makes no attempt to
/// take advantage, and value-type alignment.
///
/// This is a concurrent array. These are the available operations. They are all
/// safely concurrent with each other and they all provide the required memory
/// orderings for safe concurrent use:
///   operator[]
///   size
///   empty
///   as_view
///   as_ptr_span
///
/// The container has reference and iterator stability. The references, views,
/// and pointer-spans returned by operator[], as_view, and as_ptr_span are valid
/// for the lifetime of the container and will never become dangling.
///
/// The results of empty and size are monotonic: callers in happens-before order
/// will only ever see nondecreasing values returned by empty or by size. These
/// values serve as lower bounds valid for the lifetime of the container.
///
/// Example:
///
///   relaxed_atomic<size_t> count{0};
///   atomic_grow_array<object> array;
///
///   object& next() { return array[count++]; }
///
///   template <typename func>
///   void for_each(func f) { for (auto p : array.as_ptr_span(count)) f(*p); }
template <
    typename Item,
    typename Policy = atomic_grow_array_policy_default<Item>>
class atomic_grow_array : private Policy {
 public:
  using size_type = std::size_t;
  using value_type = Item;

  using pointer_span = std::span<value_type* const>;
  using const_pointer_span = std::span<value_type const* const>;

  class iterator;
  class const_iterator;

 private:
  static constexpr bool is_nothrow_grow_v =
      noexcept(FOLLY_DECLVAL(Policy const&).grow(0, 0)) &&
      noexcept(FOLLY_DECLVAL(Policy const&).make()) && new_is_infallible_v;

  struct array;

  struct end_tag {};

  template <bool>
  class basic_view;

  template <bool Const, typename Down>
  class basic_iterator {
   private:
    template <bool, typename>
    friend class basic_iterator;
    template <bool>
    friend class basic_view;

    using self = basic_iterator;
    using down = Down;
    friend down;

    template <typename T>
    using maybe_add_const_t = conditional_t<Const, T const, T>;

    array const* array_{};
    size_type index_{};

    basic_iterator(array const* const a, size_type const i) noexcept
        : array_{a}, index_{i} {}
    basic_iterator(array const* const a, end_tag) noexcept
        : array_{a}, index_{a ? a->size : 0} {}

    template <
        bool ThatC,
        typename ThatDown,
        bool ThisC = Const,
        typename = std::enable_if_t<!ThatC && ThisC>>
    explicit basic_iterator(basic_iterator<ThatC, ThatDown> that) noexcept
        : array_{that.array_}, index_{that.index_} {}

    down& as_down() noexcept { return static_cast<down&>(*this); }
    down const& as_down() const noexcept {
      return static_cast<down const&>(*this);
    }

   public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = atomic_grow_array::value_type;
    using difference_type = std::ptrdiff_t;
    using pointer = maybe_add_const_t<value_type>*;
    using reference = maybe_add_const_t<value_type>&;

    basic_iterator() = default; // produces an invalid iterator

    down& operator++() noexcept { return ++index_, as_down(); }
    down operator++(int) noexcept { return down{array_, index_++}; }
    down& operator--() noexcept { return --index_, as_down(); }
    down operator--(int) noexcept { return down{array_, index_--}; }
    down& operator+=(difference_type const n) noexcept {
      return index_ += n, as_down();
    }
    down operator+(difference_type const n) const noexcept {
      return down{as_down()} += n;
    }
    friend down operator+(difference_type const n, down const rhs) noexcept {
      return rhs + n;
    }
    down& operator-=(difference_type const n) noexcept {
      return index_ -= n, as_down();
    }
    down operator-(difference_type const n) const noexcept {
      return down{as_down()} -= n;
    }
    friend difference_type operator-(down const lhs, down const rhs) noexcept {
      return lhs.index_ - rhs.index_;
    }
    friend bool operator==(down const lhs, down const rhs) noexcept {
      return lhs.index_ == rhs.index_;
    }
    friend bool operator<(down const lhs, down const rhs) noexcept {
      return lhs.index_ < rhs.index_;
    }
    friend bool operator<=(down const lhs, down const rhs) noexcept {
      return lhs.index_ <= rhs.index_;
    }
    friend bool operator>(down const lhs, down const rhs) noexcept {
      return lhs.index_ > rhs.index_;
    }
    friend bool operator>=(down const lhs, down const rhs) noexcept {
      return lhs.index_ >= rhs.index_;
    }
    reference operator*() const noexcept { return *array_->list[index_]; }
    pointer operator->() const noexcept { return array_->list[index_]; }
    reference operator[](difference_type const n) const noexcept {
      return *(*this + n);
    }
  };

  template <bool Const>
  class basic_view {
   private:
    friend atomic_grow_array;

    template <typename T>
    using maybe_add_const_t = conditional_t<Const, T const, T>;

    using up = atomic_grow_array;

    array const* array_{};

    explicit basic_view(array const* arr) noexcept : array_{arr} {}

    template <
        bool ThatC,
        bool ThisC = Const,
        typename = std::enable_if_t<!ThatC && ThisC>>
    explicit basic_view(basic_view<ThatC> that) noexcept
        : array_{that.array_} {}

   public:
    using value_type = typename atomic_grow_array::value_type;
    using size_type = typename atomic_grow_array::size_type;
    using reference = maybe_add_const_t<value_type>&;
    using const_reference = value_type const&;
    using pointer = maybe_add_const_t<value_type>*;
    using const_pointer = value_type const*;
    using iterator = conditional_t<Const, up::const_iterator, up::iterator>;
    using const_iterator = up::const_iterator;

    basic_view() = default; // produces an invalid view

    iterator begin() noexcept { return iterator{array_, 0}; }
    const_iterator begin() const noexcept { return iterator{array_, 0}; }
    const_iterator cbegin() const noexcept { return iterator{array_, 0}; }
    iterator end() noexcept { return iterator{array_, end_tag{}}; }
    const_iterator end() const noexcept { return iterator{array_, end_tag{}}; }
    const_iterator cend() const noexcept { return iterator{array_, end_tag{}}; }

    size_type size() const noexcept { return array_ ? array_->size : 0; }
    bool empty() const noexcept { return !size(); }

    reference operator[](size_type index) noexcept {
      return *array_->list[index];
    }
    const_reference operator[](size_type index) const noexcept {
      return *array_->list[index];
    }

    std::span<pointer const> as_ptr_span() noexcept {
      using type = std::span<pointer const>;
      return array_ ? type{array_->list, array_->size} : type{};
    }
    std::span<const_pointer const> as_ptr_span() const noexcept {
      using type = std::span<const_pointer const>;
      return array_ ? type{array_->list, array_->size} : type{};
    }

    std::span<pointer const> as_ptr_span(size_type const sz) noexcept {
      auto ptrs = as_ptr_span();
      return ptrs.subspan(0, std::min(ptrs.size(), sz));
    }
    std::span<const_pointer const> as_ptr_span(
        size_type const sz) const noexcept {
      auto ptrs = as_ptr_span();
      return ptrs.subspan(0, std::min(ptrs.size(), sz));
    }
  };

 public:
  atomic_grow_array() = default;
  explicit atomic_grow_array(Policy const& policy_) //
      noexcept(noexcept(Policy{policy_}))
      : Policy{policy_} {}
  ~atomic_grow_array() { reset(); }

  Policy const& policy() const noexcept {
    return static_cast<Policy const&>(*this);
  }

  /// size
  ///
  /// A recent value of the true size.
  ///
  /// Always a lower bound of - ie, never larger than - the true size.
  ///
  /// Example:
  ///
  ///   atomic_grow_array& array = /* ... */;
  ///   for (size_t i = 0; i < array.size(); ++i) {
  ///     do_something_with(array[i]);
  ///   }
  size_t size() const noexcept { return size_.load(mo_acquire); }

  /// empty
  ///
  /// Equivalent to size() == 0.
  bool empty() const noexcept { return size() == 0; }

  /// operator[]
  ///
  /// A reference to the element at the given index.
  ///
  /// Every index is always valid, modulo system memory size of course.
  ///
  /// The principal meaning is that it is not necessary to configure a capacity
  /// limit in all deployed environments, to monitor the accuracies of those
  /// capacity limit in all deployed environments, and to update those capacity
  /// limits as their accuracies suffer.
  ///
  /// Intended for dense indexed access patterns and not for sparse indexed
  /// access patterns. For the sparse case, at large sizes, a concurrent
  /// unordered-map would have much less memory overhead and much less growth
  /// compute overhead. The reason is that the memory overhead and growth
  /// compute overhead in this data structure, with the default policy, is
  /// proportional to the maximum index accessed, while for an unordered-map it
  /// would be proportional to the number of accessed indices.
  ///
  /// Indexed access during element construction is forbidden but undiagnosed.
  /// One likely scenario is stack overflow; another is memory exhaustion.
  FOLLY_ALWAYS_INLINE value_type& operator[](size_type const index) //
      noexcept(is_nothrow_grow_v) {
    auto const x = index < size_.load(mo_acquire);
    auto const p = FOLLY_LIKELY(x) ? array_.load(mo_acquire) : at_slow(index);
    return *p->list[index];
  }

  /// iterator
  ///
  /// An iterator type used by view.
  class iterator : private basic_iterator<false, iterator> {
    using base = basic_iterator<false, iterator>;
    friend base;
    friend const_iterator;
    template <bool>
    friend class basic_view;

   public:
    using typename base::difference_type;
    using typename base::iterator_category;
    using typename base::pointer;
    using typename base::reference;
    using typename base::value_type;

    using base::base;
    using base::operator++;
    using base::operator--;
    using base::operator+;
    using base::operator+=;
    using base::operator-;
    using base::operator-=;
    using base::operator*;
    using base::operator->;
    using base::operator[];
  };

  /// const_iterator
  ///
  /// An iterator type used by view and const_view.
  class const_iterator : private basic_iterator<true, const_iterator> {
    using base = basic_iterator<true, const_iterator>;
    friend base;
    template <bool>
    friend class basic_view;

   public:
    using typename base::difference_type;
    using typename base::iterator_category;
    using typename base::pointer;
    using typename base::reference;
    using typename base::value_type;

    using base::base;
    using base::operator++;
    using base::operator--;
    using base::operator+;
    using base::operator+=;
    using base::operator-;
    using base::operator-=;
    using base::operator*;
    using base::operator->;
    using base::operator[];

    /* implicit */ const_iterator(iterator that) noexcept : base{that} {}
  };

  /// view
  ///
  /// Models std::ranges::range.
  ///
  /// Gives a view over all of the elements available at the time the view was
  /// created. If the array capacity is later increased, the view will not cover
  /// the new elements but it and its iterators will all remain valid.
  class view : private basic_view<false> {
    friend atomic_grow_array;
    using base = basic_view<false>;

   public:
    using typename base::const_iterator;
    using typename base::const_reference;
    using typename base::iterator;
    using typename base::reference;
    using typename base::size_type;
    using typename base::value_type;

    using base::as_ptr_span;
    using base::base;
    using base::begin;
    using base::cbegin;
    using base::cend;
    using base::empty;
    using base::end;
    using base::size;
    using base::operator[];
  };

  /// const_view
  ///
  /// Models std::ranges::range.
  ///
  /// Gives a view over all of the elements available at the time the view was
  /// created. If the array capacity is later increased, the view will not cover
  /// the new elements but it and its iterators will all remain valid.
  class const_view : private basic_view<true> {
    friend atomic_grow_array;
    using base = basic_view<true>;

   public:
    using typename base::const_iterator;
    using typename base::const_reference;
    using typename base::iterator;
    using typename base::reference;
    using typename base::size_type;
    using typename base::value_type;

    using base::as_ptr_span;
    using base::base;
    using base::begin;
    using base::cbegin;
    using base::cend;
    using base::empty;
    using base::end;
    using base::size;
    using base::operator[];

    /* implicit */ const_view(view that) noexcept : base{that} {}
  };

  /// as_view
  ///
  /// Example:
  ///
  ///   atomic_grow_array& array = /* ... */;
  ///   for (auto& item : array.as_view()) {
  ///     do_something_with(item);
  ///   }
  ///
  /// If the atomic_grow_array is grown after the call to as_view(), the
  /// returned view will provide access only to the size and elements at the
  /// time it was created and not to a later size or to elements created later.
  ///
  /// Notes:
  /// * This exists since the choice is for atomic_grow_array not to model a
  ///   range directly. Such a range could not be as performant and could have
  ///   surprising behavior, such as this expression maybe evaluating to false:
  ///     (array.begin() == array.end()) == (array.begin() == array.end())
  /// * May be more performant than repeatedly indexing with operator[] up until
  ///   size(), even when size() is gotten once and then cached.
  view as_view() noexcept { return view{array_.load(mo_acquire)}; }
  const_view as_view() const noexcept { return view{array_.load(mo_acquire)}; }

  /// as_ptr_span
  ///
  /// Convenience wrapper for view::as_ptr_span.
  pointer_span as_ptr_span() noexcept { return as_view().as_ptr_span(); }
  const_pointer_span as_ptr_span() const noexcept {
    return as_view().as_ptr_span();
  }
  pointer_span as_ptr_span(size_type const sz) noexcept {
    return as_view().as_ptr_span(sz);
  }
  const_pointer_span as_ptr_span(size_type const sz) const noexcept {
    return as_view().as_ptr_span(sz);
  }

 private:
  static constexpr auto mo_acquire = std::memory_order_acquire;
  static constexpr auto mo_release = std::memory_order_release;
  static constexpr auto mo_acq_rel = std::memory_order_acq_rel;

  //  prefix of layout structure
  //
  //  missing alignment and suffix because there are two variable-sized array
  //  fields
  struct array {
    array* next{}; // maybe null; const
    size_type size{}; // size != 0; size > (next ? next->size : 0); const
    value_type* list[]; // value_type* list[size]; const
    // value_type slab[size - (next ? next->size : 0)]; non-const
  };

  FOLLY_NOINLINE array* at_slow(size_type const index) //
      noexcept(is_nothrow_grow_v) {
    //  uses optimistic concurrency in order to avoid the space cost of any
    //  embedded mutex or the possible contention or deadlock from any global
    //  mutex or global mutex slab
    //
    //  deadlock from a global mutex or global mutex slab may occur if the
    //  value_type type constructor may also access the global mutex or global
    //  mutex slab, whether directly or indirectly
    array* p = array_.load(mo_acquire);
    array* q = nullptr;
    do {
      if (p && index < p->size) {
        return p;
      }
      //  p may have changed after a race loss, so grow from its current size
      size_type const size = policy().grow(p ? p->size : 0, index);
      assert(index < size);
      //  the race begins here
      q = new_array(size, p);
      if (!q) {
        //  the race is lost early
        continue;
      }
      //  this c/x only need success-release/failure-acquire, but c++
      //  implementations have trouble with that; so, success-acq-rel
      //  see: folly::atomic_compare_exchange_strong_explicit
      if (array_.compare_exchange_strong(p, q, mo_acq_rel, mo_acquire)) {
        //  the race is won
        //  but a winner of a later race may already have stored a larger size
        folly::atomic_fetch_max_cond(size_, size, mo_release);
        return q;
      }
      //  the race is lost
      del_array(q);
    } while (1);
  }

  static constexpr size_type array_align() {
    return folly::constexpr_max(folly::max_align_v, alignof(value_type));
  }
  static size_type array_size(size_type const size, size_type const base) {
    constexpr auto a = array_align();
    return //
        folly::constexpr_ceil(sizeof(array) + size * sizeof(value_type*), a) +
        folly::constexpr_ceil((size - base) * sizeof(value_type), a);
  }
  static value_type* array_slab(array* const curr) {
    return reinterpret_cast<value_type*>(folly::constexpr_ceil(
        reinterpret_cast<uintptr_t>(&curr->list[curr->size]),
        static_cast<uintptr_t>(array_align())));
  }

  array* new_array(size_type const size, array*& next) {
    auto const base = next ? next->size : 0;
    assert(size > base);
    array* curr = static_cast<array*>(
        operator_new(array_size(size, base), std::align_val_t{array_align()}));
    auto rollback =
        folly::makeGuard(std::bind(&atomic_grow_array::del_array, this, curr));
    curr->size = size;
    curr->next = next;
    auto const slab = array_slab(curr);
    //  copy pointers to all pre-existing elements; cannot throw
    for (size_type i = 0; i < base; ++i) {
      curr->list[i] = next->list[i];
    }
    //  zero-initialize to new elements; cannot throw
    for (size_type i = base; i < size; ++i) {
      curr->list[i] = nullptr;
    }
    //  initialize new elements and the pointers to them; may throw
    for (size_type i = base; i < size; ++i) {
      //  detect race losses early
      //  must be acquire, as for the failure order of the c/x in at_slow: on a
      //  race loss, the caller dereferences the array given back in next
      if (auto const p = array_.load(std::memory_order_acquire); p != next) {
        next = p;
        return nullptr;
      }
      //  no race loss yet
      curr->list[i] = ::new (&slab[i - base]) value_type(policy().make());
    }
    rollback.dismiss();
    return curr;
  }

  void del_array(array* const curr) {
    assert(curr);
    auto size = curr->size;
    auto const next = curr->next;
    auto const base = next ? next->size : 0;
    assert(size > base);
    //  skip past zero-initialized pointers at the end since their corresponding
    //  elements were never created - this situation arises when initialization
    //  of an element fails within new_array and the rollback calls del_array
    while (size > base && !curr->list[size - 1]) {
      --size;
    }
    //  destroy elements owned by this array only, and not elements owned by any
    //  other arrays - those elements will be destroyed by del_array calls on
    //  their owning arrays
    for (size_type i = 0; i < size - base; ++i) {
      curr->list[size - 1 - i]->~value_type();
    }
    operator_delete(
        static_cast<void*>(curr),
        array_size(curr->size, base),
        std::align_val_t{array_align()});
  }

  void reset() {
    auto curr = array_.load(mo_acquire);
    while (curr) {
      auto const next = curr->next;
      del_array(curr);
      curr = next;
    }
  }

  typename Policy::template atom<size_type> size_{0};
  typename Policy::template atom<array*> array_{nullptr};
};

} // namespace folly
