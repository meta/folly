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
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <folly/CPortability.h>
#include <folly/ConstexprMath.h>
#include <folly/ScopeGuard.h>
#include <folly/Utility.h>
#include <folly/container/range_traits.h>
#include <folly/lang/Exception.h>
#include <folly/memory/MemoryResource.h>

namespace folly {

/// Inclusive minimum and maximum element capacities for the memory blocks a
/// hive allocates.
struct hive_limits {
  std::size_t min;
  std::size_t max;

  constexpr hive_limits(std::size_t minimum, std::size_t maximum) noexcept
      : min{minimum}, max{maximum} {}
};

namespace detail {

// Relational operators on pointers into distinct allocations are unspecified,
// while std::less over pointers is required to be a total order.
inline bool hive_pointer_in_range(
    const void* pointer, const void* first, const void* last) noexcept {
  auto less = std::less<const void*>{};
  return !less(pointer, first) && less(pointer, last);
}

// The range a container may be built from or extended by: exactly the
// standard's container-compatible-range<T>.
template <typename R, typename T>
concept hive_compatible_range = std::ranges::input_range<R> &&
    std::convertible_to<std::ranges::range_reference_t<R>, T>;

// What the paper's InputIterator template parameters mean: Cpp17InputIterator,
// which is not std::input_iterator. The two differ in both directions, and
// both differences matter here. A legacy proxy iterator - one whose
// operator* hands back a prvalue proxy - is a Cpp17InputIterator the paper
// accepts but may fail std::input_iterator's indirectly_readable
// requirements; a move-only C++20 iterator models std::input_iterator but is
// no Cpp17InputIterator at all, and iterator_traits gives it no
// iterator_category, so it is rejected here by constraint rather than by a
// hard error from inside the container.
template <typename It>
concept hive_cpp17_input_iterator =
    requires { typename std::iterator_traits<It>::iterator_category; } &&
    std::is_convertible_v<
        typename std::iterator_traits<It>::iterator_category,
        std::input_iterator_tag>;

template <typename It>
concept hive_cpp17_forward_iterator = //
    hive_cpp17_input_iterator<It> &&
    std::is_convertible_v<
        typename std::iterator_traits<It>::iterator_category,
        std::forward_iterator_tag>;

// Cpp17MoveInsertable / Cpp17CopyInsertable / Cpp17EmplaceConstructible: what
// the paper actually requires of an element type, which is that the
// ALLOCATOR can build it, not that the type is core-move-constructible. An
// allocator whose construct() takes over - scoped_allocator_adaptor passing
// itself down, or any allocator with a custom construct - makes types
// insertable that std::move_constructible rejects.
//
// Asking allocator_traits<A>::construct directly would answer this wrong:
// its fallback is not constrained on the construction being valid, so the
// call expression is well-formed for every argument list and only the body
// fails, long after the answer was needed. So the allocator's own construct
// is what gets probed, and only when it has none does the question fall back
// to direct construction, which is what allocator_traits would then do.
template <typename Alloc, typename T, typename... Args>
concept hive_alloc_constructs = requires(
    Alloc& alloc, T* address, Args&&... args) {
  alloc.construct(address, std::forward<Args>(args)...);
};

template <typename Alloc, typename T, typename... Args>
concept hive_emplace_constructible = hive_alloc_constructs<Alloc, T, Args...> ||
    std::is_constructible_v<T, Args...>;

// Whether constructing a T through Alloc from Args can throw, decided the way
// allocator_traits<Alloc>::construct decides what to call: the allocator's
// own construct() when it has one, the constructor otherwise. Asking
// noexcept(allocator_traits<Alloc>::construct(...)) directly would not do,
// because whether that function carries a noexcept-specification is up to
// the standard library: libstdc++ forwards one, libc++ declares none.
template <typename Alloc, typename T, typename... Args>
constexpr bool hive_nothrow_alloc_construct() noexcept {
  if constexpr (hive_alloc_constructs<Alloc, T, Args...>) {
    return noexcept(std::declval<Alloc&>().construct(
        std::declval<T*>(), std::declval<Args>()...));
  } else {
    return std::is_nothrow_constructible_v<T, Args...>;
  }
}

// A type usable as this container's allocator. Testing for the member alone
// is SFINAE-friendly, where instantiating allocator_traits for a
// non-allocator is a hard error.
template <typename A>
concept hive_allocator_like = requires { typename A::value_type; };

// size_type and difference_type appear in constructor parameter lists, so
// the implicit deduction guides substitute them for whatever third argument
// a call happens to carry - a hive_limits, say. Naming
// allocator_traits<hive_limits>::size_type there would be a hard error
// outside the immediate context, and CTAD would fail to compile rather than
// discarding the candidate. Going through this trait keeps the class
// template itself unconstrained, exactly the shape P0447R28 declares, while
// leaving those candidates merely non-viable.
template <typename A, typename = void>
struct hive_size_types {
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
};
template <hive_allocator_like A>
struct hive_size_types<A> {
  using size_type = typename std::allocator_traits<A>::size_type;
  using difference_type = typename std::allocator_traits<A>::difference_type;
};

template <typename Alloc, typename T>
concept hive_move_insertable = hive_emplace_constructible<Alloc, T, T&&>;

template <typename Alloc, typename T>
concept hive_copy_insertable = hive_emplace_constructible<Alloc, T, const T&> &&
    hive_emplace_constructible<Alloc, T, T&>;

// An allocator whose max_size() a default-constructed instance reports in a
// constant expression. That bound is a property of the type, so the static
// block_capacity_hard_limits() can honour it; a bound that depends on an
// instance's state is reported by max_size() instead.
template <typename A>
concept hive_constant_max_size = std::default_initializable<A> && requires {
  typename std::integral_constant<
      std::size_t,
      static_cast<std::size_t>(std::allocator_traits<A>::max_size(A()))>;
};

// The largest count Traits::size_type can express, as a std::size_t.
template <
    typename Traits,
    typename Count = typename Traits::size_type,
    bool Big = (sizeof(Count) >= sizeof(std::size_t)),
    typename Size = std::conditional_t<Big, std::size_t, Count>>
constexpr std::size_t hive_count_ceiling = std::numeric_limits<Size>::max();

// The fixed bound an allocator type reports through max_size(), or no bound
// at all when that bound is not a property of the type. Reduce shortens the
// bound by that many elements, for an allocation that is that much longer
// than the block it serves.
template <typename A, std::size_t Reduce = 0>
constexpr std::size_t hive_type_max_size() noexcept {
  if constexpr (hive_constant_max_size<A>) {
    constexpr auto bound =
        static_cast<std::size_t>(std::allocator_traits<A>::max_size(A()));
    return bound < Reduce ? 0 : bound - Reduce;
  } else {
    return std::numeric_limits<std::size_t>::max();
  }
}

} // namespace detail

/// An allocator-aware sequence container whose elements keep their addresses,
/// references and iterators for as long as they are not themselves erased.
/// Erasing an element never moves any other element, and the storage it frees
/// is reused by later insertions.
///
/// This implements P0447R28's std::hive, in folly's namespace rather than
/// the standard's, and is self-contained: no external engine behind it.
/// It also defines behaviour for the precondition violations listed below;
/// that narrows nothing a conforming program may do.
///
/// Storage is a linked list of blocks. Each block carries its elements plus a
/// jump-counting skipfield with one entry per element slot and a trailing
/// sentinel: an occupied slot's entry is zero, and a run of consecutive erased
/// slots stores its own length in the entries at both ends of the run, so an
/// iterator crosses any number of erased elements in a single step. Erased
/// slots are threaded into a per-block free list - one node per erased run,
/// held in the erased storage itself - so insertion reuses erased storage in
/// constant time. A block that has an erased run, or slots it has never used,
/// is on the hive's list of blocks with room, which is where an insertion
/// looks before it allocates anything; that keeps every free slot of every
/// block reachable, wherever the block sits in the chain.
///
/// Insertion order is the iteration order only until the first erasure; after
/// that, an insertion goes wherever storage is free. This is a bag, not a
/// sequence whose order a caller controls.
///
/// Validity. Insertion and erasure both leave every element where it is, so
/// pointers, references and iterators to elements stay valid. The exceptions
/// are end(), which both of them can move - an insertion that lands past the
/// last block's high-water mark, including one that links a new block on the
/// back, and an erasure that empties the last block, which unlinks it and
/// keeps its storage as a spare - and the three operations that do move
/// elements: reshape(), shrink_to_fit() and sort() (the last moves values
/// between fixed addresses rather than moving elements). Read end() again
/// rather than holding a copy of it across an insertion or an erasure.
///
/// Complexity: insert, emplace and single-element erase are amortized
/// constant time. splice is linear in the number of blocks of the source
/// hive: it validates each source block against the destination's block
/// capacity limits and renumbers the blocks it takes on. get_iterator is
/// linear in the number of blocks. Iteration is linear in the number of live
/// elements, and costs one skipfield load per element however many erased
/// slots lie between them. trim_capacity(n) is linear in the number of blocks
/// it releases.
///
/// Exception safety: every insertion is transactional. A single insert or
/// emplace that throws leaves the hive exactly as it was, down to the erased
/// slot it was aiming at and the capacity it had. A bulk insertion that throws
/// erases whatever it had already inserted and releases exactly the blocks it
/// allocated, so the hive keeps precisely the elements, at precisely the
/// addresses, with precisely the capacity and spare blocks, that it held
/// beforehand. reshape() and shrink_to_fit() build their new blocks on a
/// temporary and commit it only once every element is in place, so if they
/// throw this hive keeps its elements, blocks and limits; an element already
/// moved into the temporary is left moved-from, which the paper allows.
/// shrink_to_fit() first releases the blocks that hold no elements, and that
/// release stands even if the compaction after it fails.
///
/// Capacity. max_size() is the allocator's own bound, kept within
/// difference_type so that the distance between any two iterators can
/// represent it. Blocks are whole, so the storage a hive holds can run past
/// that bound by less than one block; capacity() counts the elements the hive
/// can hold without allocating, which is never more than max_size(). Block
/// capacity limits the allocator could never satisfy leave a hive whose
/// max_size() is zero.
///
/// Behaviour where the paper leaves a precondition, and this container
/// defines one instead. None of these narrows what a conforming program may
/// do; each one gives a defined result where the paper gives none, so a
/// program written to the paper cannot tell the difference:
///  - Block capacity limits outside the hard limits throw std::length_error,
///    from the constructor or reshape() given them, rather than being
///    undefined. Limits within the hard limits that a particular allocator
///    could never satisfy are accepted, and leave a hive whose max_size() is
///    zero and whose first insertion throws std::length_error. splice()
///    likewise throws std::invalid_argument on unequal allocators, and
///    std::length_error when the combined size would pass max_size(), alongside
///    the std::length_error the paper specifies for a source block outside the
///    destination's limits.
///  - get_iterator() returns end() for an address that does not hold a live
///    element of this hive.
///  - sort() is stable. Note what stability means here: no element moves, so
///    it is the values that are permuted between fixed addresses, and a
///    pointer held across a sort names the same address and so a different
///    element.
///  - clear() keeps the hive's blocks for reuse; trim_capacity() returns
///    them. The paper requires only that capacity() not grow.
///  - erase_if() walks an element count taken before it starts rather than
///    re-testing end() each time round, because an erasure that empties the
///    last block unlinks it and moves end().
///  - A bulk insertion into a hive that already holds elements, of elements
///    whose construction can throw, remembers one iterator per element so
///    that it can roll back exactly: a transient cost of three pointers per
///    element inserted, which is what makes a throwing bulk insert leave the
///    hive precisely as it was. Where nothing can need undoing element by
///    element - the hive started empty, or every construction after the
///    up-front reservation is noexcept - nothing is remembered.
///
/// The from-range constructors and their deduction guides take
/// folly::from_range_t, which is std::from_range_t where the standard library
/// provides it and folly's backport of it otherwise: they are there in a
/// C++20 build, and accept std::from_range in a C++23 one. folly defines no
/// __cpp_lib_hive macro.
template <typename T, typename Allocator = std::allocator<T>>
class hive {
 private:
  using AllocTraits = std::allocator_traits<Allocator>;
  // One skipfield entry per slot. The entry has to be able to hold the
  // length of the longest run of erased slots a block can hold, which bounds
  // the block capacity at kHardMaxBlockCapacity.
  using SkipType = std::uint16_t;

  static_assert(
      std::same_as<T, typename AllocTraits::value_type>,
      "folly::hive: the allocator's value_type must be the element type");

  // Never a valid slot index, because a block holds at most
  // kHardMaxBlockCapacity elements and so indexes 0 .. kNullIndex - 1.
  static constexpr SkipType kNullIndex = std::numeric_limits<SkipType>::max();

  // Doubly-linked list node for a block's free list, stored in the storage of
  // the erased element that starts an erased run.
  struct FreeNode {
    SkipType prev;
    SkipType next;
  };

  // An element slot holds either a live element or, once erased, the free-list
  // node for its run. Lifetimes are managed by hand; neither member is ever
  // implicitly constructed or destroyed.
  union Slot {
    Slot() noexcept {}
    ~Slot() {}
    Slot(const Slot&) = delete;
    Slot(Slot&&) = delete;
    Slot& operator=(const Slot&) = delete;
    Slot& operator=(Slot&&) = delete;

    T value;
    FreeNode free;
  };

  using SlotAlloc = typename AllocTraits::template rebind_alloc<Slot>;
  using SlotAllocTraits = typename AllocTraits::template rebind_traits<Slot>;
  using SkipAlloc = typename AllocTraits::template rebind_alloc<SkipType>;
  using SkipAllocTraits =
      typename AllocTraits::template rebind_traits<SkipType>;
  // Only named here. Nothing at class scope may ask these anything, because
  // that instantiates the allocator for Block while Block is incomplete, and
  // an allocator is entitled to need a complete value_type - one choosing a
  // size class from sizeof(T), say. They are consulted only inside member
  // functions, where Block is complete.
  struct Block;
  using BlockAlloc = typename AllocTraits::template rebind_alloc<Block>;
  using BlockAllocTraits = typename AllocTraits::template rebind_traits<Block>;

  // What the slot and skipfield allocators hand back. These are the
  // allocator's own pointer types, which need not be raw: deallocate() has to
  // be given back what allocate() returned, so both are kept, while all
  // arithmetic and traversal runs on the raw addresses std::to_address gives.
  using SlotHandle = typename SlotAllocTraits::pointer;
  using SkipHandle = typename SkipAllocTraits::pointer;

  static constexpr std::size_t kHardMinBlockCapacity = 3;
  // Every ceiling a block capacity has to fit under, whichever binds first: a
  // skipfield entry has to hold the longest run of erased slots a block can
  // contain; each allocator that sizes a block - the hive's own, the slots'
  // rebind, and the skipfield's rebind, which allocates one entry more than
  // the block has slots - has to be able to express the count it is asked
  // for; and where those allocator types have a fixed max_size(), the block
  // has to fit under that as well. The paper requires the last of these of
  // the hive's own allocator.
  static constexpr std::size_t kHardMaxBlockCapacity = constexpr_min(
      static_cast<std::size_t>(kNullIndex),
      detail::hive_count_ceiling<AllocTraits>,
      detail::hive_count_ceiling<SlotAllocTraits>,
      detail::hive_count_ceiling<SkipAllocTraits> - 1,
      detail::hive_type_max_size<Allocator>(),
      detail::hive_type_max_size<SlotAlloc>(),
      detail::hive_type_max_size<SkipAlloc, 1>());
  static constexpr std::size_t kDefaultMinBlockCapacity =
      constexpr_min(std::size_t{8}, kHardMaxBlockCapacity);
  static constexpr std::size_t kDefaultMaxBlockCapacity = kHardMaxBlockCapacity;
  static_assert(
      kHardMinBlockCapacity <= kHardMaxBlockCapacity,
      "folly::hive: the allocator cannot address a block of three elements");

  struct Block {
    SlotHandle slotsHandle{};
    SkipHandle skipHandle{};
    Slot* slots = nullptr;
    // One entry per slot, plus one. An occupied slot's entry is zero; a run of
    // erased slots holds its own length in the entries at both of its ends.
    // Advancing therefore reads one entry and adds it: the jump is direct, and
    // a run of any length costs the same as an occupied neighbour. Nothing
    // here is ever scanned for a terminator, and the extra trailing entry is
    // not one: it is a permanent zero that makes the single read at the
    // high-water mark - by an iterator stepping off the last slot, or by
    // markErased looking at the slot to the right - well defined without a
    // bounds test.
    SkipType* skipfield = nullptr;
    Block* prev = nullptr;
    Block* next = nullptr;
    // Links for the hive's list of blocks with somewhere to put a new
    // element - an erased run, unused slots at the end, or both. A block is
    // on that list exactly while inRoomList is set.
    Block* prevWithRoom = nullptr;
    Block* nextWithRoom = nullptr;
    // Strictly increasing along the chain, so that comparing two iterators
    // is comparing this and then the slot address. Assigned when the block
    // is linked in, and left alone for as long as it stays there; a block
    // that leaves and comes back gets a new one.
    std::size_t order = 0;
    // When the block was allocated, in the hive's count of allocations: a
    // bulk insertion that throws releases exactly the blocks allocated after
    // it began.
    std::size_t serial = 0;
    SkipType capacity = 0;
    // High-water mark: slots [0, used) have held an element at some point, and
    // slots [used, capacity) are untouched.
    SkipType used = 0;
    SkipType liveCount = 0;
    SkipType freeHead = kNullIndex;
    bool inRoomList = false;
  };

  static constexpr T* elementOf(Slot* slot) noexcept {
    return std::addressof(slot->value);
  }
  static constexpr const T* elementOf(const Slot* slot) noexcept {
    return std::addressof(slot->value);
  }

  template <bool IsConst>
  class Iterator {
   private:
    friend class hive<T, Allocator>;
    template <bool>
    friend class Iterator;

    using BlockPtr = std::conditional_t<IsConst, const Block*, Block*>;
    using SlotPtr = std::conditional_t<IsConst, const Slot*, Slot*>;

    constexpr Iterator(
        BlockPtr block, SlotPtr slot, const SkipType* skip) noexcept
        : block_{block}, slot_{slot}, skip_{skip} {}

   public:
    using iterator_category = std::bidirectional_iterator_tag;
    using iterator_concept = std::bidirectional_iterator_tag;
    using value_type = T;
    using difference_type =
        typename detail::hive_size_types<Allocator>::difference_type;
    // The allocator's own pointer types, which need not be raw.
    using pointer = std::conditional_t<
        IsConst,
        typename AllocTraits::const_pointer,
        typename AllocTraits::pointer>;
    using reference = std::conditional_t<IsConst, const T&, T&>;

    constexpr Iterator() = default;

    template <bool OtherConst>
      requires(IsConst && !OtherConst)
    constexpr /* implicit */ Iterator(
        const Iterator<OtherConst>& other) noexcept
        : block_{other.block_}, slot_{other.slot_}, skip_{other.skip_} {}

    constexpr reference operator*() const noexcept { return *elementOf(slot_); }
    constexpr pointer operator->() const noexcept {
      // Rebuilding the allocator's pointer from the element's address: the
      // iterator walks raw addresses, and only hands one back in whatever
      // representation the allocator uses. The allocator requirements give
      // pointer_to to the allocator's pointer, not to its const_pointer, so
      // a const iterator rebuilds a pointer and converts it, which those
      // requirements do provide. The element itself is never const, so
      // naming it through a non-const reference to do that is sound.
      using PtrTraits = std::pointer_traits<typename AllocTraits::pointer>;
      T& element = const_cast<T&>(*elementOf(slot_));
      return PtrTraits::pointer_to(element);
    }

    constexpr Iterator& operator++() noexcept {
      ++slot_;
      ++skip_;
      // One read and one add crosses however many erased slots follow: the
      // entry that starts a run holds the run's whole length, so this is a
      // direct jump, never a scan, and the per-element cost of iterating does
      // not depend on how much of the hive has been erased. The read is
      // unconditional on purpose - branching on it to skip the add off a
      // block with nothing erased in it measures faster on such a block and
      // slower on every block that does have an erased run, which is the
      // state a hive is kept in.
      const SkipType skip = *skip_;
      slot_ += skip;
      skip_ += skip;
      if (slot_ == block_->slots + block_->used && block_->next != nullptr) {
        *this = firstIn<Iterator>(block_->next);
      }
      return *this;
    }

    constexpr Iterator operator++(int) noexcept {
      Iterator copy = *this;
      ++*this;
      return copy;
    }

    constexpr Iterator& operator--() noexcept {
      while (true) {
        const std::size_t index =
            static_cast<std::size_t>(skip_ - block_->skipfield);
        if (index != 0) {
          // A non-zero entry one slot back ends a run of erased slots and holds
          // its length, so the preceding live element is that far back again.
          const SkipType skip = skip_[-1];
          if (skip == 0) {
            --slot_;
            --skip_;
            return *this;
          }
          if (index > skip) {
            slot_ -= skip + 1;
            skip_ -= skip + 1;
            return *this;
          }
        }
        // Every linked block holds at least one live element, so this crosses
        // at most one block boundary.
        block_ = block_->prev;
        slot_ = block_->slots + block_->used;
        skip_ = block_->skipfield + block_->used;
      }
    }

    constexpr Iterator operator--(int) noexcept {
      Iterator copy = *this;
      --*this;
      return copy;
    }

    friend constexpr bool operator==(
        const Iterator& lhs, const Iterator& rhs) noexcept {
      return lhs.slot_ == rhs.slot_ && lhs.block_ == rhs.block_;
    }
    /// Iterators into one hive are totally ordered, and the order is the
    /// iteration order: i < j exactly when advancing i enough times reaches
    /// j. Blocks carry a key that increases along the chain, so this is that
    /// key and then the slot address, both of which stay put while the
    /// element does. Comparing iterators from different hives, like
    /// comparing pointers into different allocations, says nothing.
    friend constexpr std::strong_ordering operator<=>(
        const Iterator& lhs, const Iterator& rhs) noexcept {
      if (lhs.block_ == rhs.block_) {
        return std::compare_three_way{}(lhs.slot_, rhs.slot_);
      }
      // A default-constructed iterator has no block and so no key: it sorts
      // before every iterator that has one, and is equivalent only to
      // another default-constructed iterator, which keeps <=> agreeing
      // with ==.
      if (lhs.block_ == nullptr || rhs.block_ == nullptr) {
        return (lhs.block_ != nullptr) <=> (rhs.block_ != nullptr);
      }
      return lhs.block_->order <=> rhs.block_->order;
    }

   private:
    BlockPtr block_ = nullptr;
    SlotPtr slot_ = nullptr;
    const SkipType* skip_ = nullptr;
  };

 public:
  using value_type = T;
  using allocator_type = Allocator;
  using size_type = typename detail::hive_size_types<Allocator>::size_type;
  using difference_type =
      typename detail::hive_size_types<Allocator>::difference_type;
  using reference = T&;
  using const_reference = const T&;
  using pointer = typename AllocTraits::pointer;
  using const_pointer = typename AllocTraits::const_pointer;
  using iterator = Iterator<false>;
  using const_iterator = Iterator<true>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  static_assert(std::bidirectional_iterator<iterator>);
  static_assert(std::bidirectional_iterator<const_iterator>);
  static_assert(std::sentinel_for<const_iterator, iterator>);

  //
  // construct, copy, move, destroy
  //

  /// Constructs an empty hive with the default block capacity limits.
  ///
  /// The constructors are declared as P0447R28 declares them: the one-argument
  /// limits constructor is explicit and the two-argument one is not, and
  /// type_identity_t keeps the allocator of the allocator-extended copy and
  /// move constructors from being deduced from. Every delegation and every
  /// temporary hive inside the class is built with parentheses, never braces:
  /// for an element type that can be built from anything - std::any, say -
  /// braces would select the initializer_list constructor instead.
  constexpr hive() noexcept(noexcept(Allocator())) : hive(Allocator()) {}

  constexpr explicit hive(const Allocator& allocator) noexcept
      : alloc_(allocator) {}

  constexpr explicit hive(hive_limits limits) : hive(limits, Allocator()) {}

  constexpr hive(hive_limits limits, const Allocator& allocator)
      : alloc_(allocator) {
    setLimits(limits);
  }

  constexpr explicit hive(
      size_type count, const Allocator& allocator = Allocator())
      : alloc_(allocator) {
    fillOrUnwind([&] { appendDefault(count); });
  }

  constexpr hive(
      size_type count,
      hive_limits limits,
      const Allocator& allocator = Allocator())
      : alloc_(allocator) {
    setLimits(limits);
    fillOrUnwind([&] { appendDefault(count); });
  }

  constexpr hive(
      size_type count, const T& value, const Allocator& allocator = Allocator())
      : alloc_(allocator) {
    fillOrUnwind([&] { appendCopies(count, value); });
  }

  constexpr hive(
      size_type count,
      const T& value,
      hive_limits limits,
      const Allocator& allocator = Allocator())
      : alloc_(allocator) {
    setLimits(limits);
    fillOrUnwind([&] { appendCopies(count, value); });
  }

  template <detail::hive_cpp17_input_iterator InputIt>
  constexpr hive(
      InputIt first, InputIt last, const Allocator& allocator = Allocator())
      : alloc_(allocator) {
    fillOrUnwind([&] { appendRange(std::move(first), std::move(last)); });
  }

  template <detail::hive_cpp17_input_iterator InputIt>
  constexpr hive(
      InputIt first,
      InputIt last,
      hive_limits limits,
      const Allocator& allocator = Allocator())
      : alloc_(allocator) {
    setLimits(limits);
    fillOrUnwind([&] { appendRange(std::move(first), std::move(last)); });
  }

  /// The from-range constructors. folly::from_range_t is std::from_range_t
  /// where the standard library has it, so these take std::from_range too.
  template <detail::hive_compatible_range<T> R>
  constexpr hive(
      from_range_t, R&& range, const Allocator& allocator = Allocator())
      : alloc_(allocator) {
    fillOrUnwind([&] {
      appendRange(std::ranges::begin(range), std::ranges::end(range));
    });
  }

  template <detail::hive_compatible_range<T> R>
  constexpr hive(
      from_range_t,
      R&& range,
      hive_limits limits,
      const Allocator& allocator = Allocator())
      : alloc_(allocator) {
    setLimits(limits);
    fillOrUnwind([&] {
      appendRange(std::ranges::begin(range), std::ranges::end(range));
    });
  }

  constexpr /* implicit */ hive(
      std::initializer_list<T> values, const Allocator& allocator = Allocator())
      : hive(values.begin(), values.end(), allocator) {}

  constexpr hive(
      std::initializer_list<T> values,
      hive_limits limits,
      const Allocator& allocator = Allocator())
      : hive(values.begin(), values.end(), limits, allocator) {}

  constexpr hive(const hive& other)
      : hive(
            other,
            AllocTraits::select_on_container_copy_construction(other.alloc_)) {}

  constexpr hive(
      const hive& other, const std::type_identity_t<Allocator>& allocator)
      : alloc_(allocator),
        minBlockCapacity_(other.minBlockCapacity_),
        maxBlockCapacity_(other.maxBlockCapacity_) {
    fillOrUnwind([&] { appendRange(other.begin(), other.end()); });
  }

  constexpr hive(hive&& other) noexcept : alloc_(std::move(other.alloc_)) {
    adoptStorageAndLimits(other);
  }

  /// When the allocators are equal the storage changes hands, limits and
  /// all. When they are not, the elements are moved one by one into blocks
  /// this hive allocates under the default limits, as for any constructor
  /// not given limits: the paper transfers the source's limits only along
  /// with its storage. An allocator type whose instances always compare
  /// equal never takes the elementwise path, which is not even instantiated
  /// for it - so an element type it cannot move is fine.
  constexpr hive(hive&& other, const std::type_identity_t<Allocator>& allocator)
      : alloc_(allocator) {
    if constexpr (AllocTraits::is_always_equal::value) {
      adoptStorageAndLimits(other);
    } else if (alloc_ == other.alloc_) {
      adoptStorageAndLimits(other);
    } else {
      moveElementsFrom(other);
    }
  }

  /// Constexpr so that hive is a literal type: an empty hive allocates
  /// nothing, so it can be built and destroyed during constant evaluation.
  /// A hive that has allocated cannot be, because the slots are a union
  /// whose storage is reused by hand.
  constexpr ~hive() {
    destroyAllElements();
    freeAllBlocks();
  }

  /// Copy assignment. The destination keeps its own block capacity limits:
  /// the limits are a property of the hive, not of its contents, and the
  /// paper does not transfer them here. Spare blocks are kept only if those
  /// limits still allow them.
  constexpr hive& operator=(const hive& other) {
    if (this == &other) {
      return *this;
    }
    if (AllocTraits::propagate_on_container_copy_assignment::value &&
        !(alloc_ == other.alloc_)) {
      destroyAllElements();
      freeAllBlocks();
    }
    copyAllocator(other.alloc_);
    clear();
    appendRange(other.begin(), other.end());
    return *this;
  }

  /// Move assignment. When the storage itself changes hands - the allocators
  /// are equal, or the allocator propagates - the destination takes on the
  /// source's block capacity limits along with the blocks those limits
  /// describe. When instead the elements have to be moved one by one, the
  /// destination keeps its own limits, because it keeps its own blocks.
  constexpr hive& operator=(hive&& other) noexcept(
      AllocTraits::propagate_on_container_move_assignment::value ||
      AllocTraits::is_always_equal::value) {
    if (this == &other) {
      return *this;
    }
    // When the allocator type guarantees that the storage can change hands,
    // the elementwise path is not instantiated at all: the paper asks
    // nothing of the element type then, not even that it be movable.
    if constexpr (
        AllocTraits::propagate_on_container_move_assignment::value ||
        AllocTraits::is_always_equal::value) {
      adoptAssigned(other);
    } else if (alloc_ == other.alloc_) {
      adoptAssigned(other);
    } else {
      clear();
      moveElementsFrom(other);
    }
    return *this;
  }

  constexpr hive& operator=(std::initializer_list<T> values) {
    assign(values.begin(), values.end());
    return *this;
  }

  /// Replaces the contents. The hive keeps its blocks for reuse.
  template <detail::hive_cpp17_input_iterator InputIt>
  constexpr void assign(InputIt first, InputIt last) {
    clear();
    appendRange(std::move(first), std::move(last));
  }

  /// Replaces the contents with count copies of value. Safe when value names
  /// an element of this hive: the copy that outlives the clear() is made
  /// through the allocator, so it works for an element type the allocator
  /// can copy even though the type itself has no copy constructor.
  constexpr void assign(size_type count, const T& value) {
    static_assert(
        detail::hive_copy_insertable<Allocator, T>,
        "folly::hive::assign: T must be Cpp17CopyInsertable into hive");
    if (count == 0) {
      clear();
      return;
    }
    assignCopiesNonEmpty(count, value);
  }

  constexpr void assign(std::initializer_list<T> values) {
    assign(values.begin(), values.end());
  }

  /// Replaces the contents with the elements of a range.
  template <detail::hive_compatible_range<T> R>
  constexpr void assign_range(R&& range) {
    clear();
    appendRange(std::ranges::begin(range), std::ranges::end(range));
  }

  constexpr allocator_type get_allocator() const noexcept { return alloc_; }

  //
  // iterators
  //

  /// Iteration visits every live element exactly once, in block order and
  /// then in slot order within a block. Each step is a single skipfield read
  /// and add, whatever the number of erased slots it passes over.
  constexpr iterator begin() noexcept {
    return front_ == nullptr ? iterator() : firstIn<iterator>(front_);
  }

  constexpr const_iterator begin() const noexcept {
    return front_ == nullptr
        ? const_iterator()
        : firstIn<const_iterator>(front_);
  }

  constexpr iterator end() noexcept {
    return back_ == nullptr ? iterator() : endOf<iterator>(back_);
  }

  constexpr const_iterator end() const noexcept {
    return back_ == nullptr ? const_iterator() : endOf<const_iterator>(back_);
  }

  constexpr const_iterator cbegin() const noexcept { return begin(); }
  constexpr const_iterator cend() const noexcept { return end(); }

  constexpr reverse_iterator rbegin() noexcept {
    return reverse_iterator{end()};
  }
  constexpr const_reverse_iterator rbegin() const noexcept {
    return const_reverse_iterator{end()};
  }
  constexpr reverse_iterator rend() noexcept {
    return reverse_iterator{begin()};
  }
  constexpr const_reverse_iterator rend() const noexcept {
    return const_reverse_iterator{begin()};
  }
  constexpr const_reverse_iterator crbegin() const noexcept { return rbegin(); }
  constexpr const_reverse_iterator crend() const noexcept { return rend(); }

  //
  // capacity
  //

  [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

  /// The number of live elements.
  constexpr size_type size() const noexcept { return size_; }

  /// The number of elements the hive can hold without allocating another
  /// block: its free slots, every one of them reachable by an insertion, plus
  /// its elements. Blocks are whole, so the slots the hive owns can add up to
  /// a little more than max_size(); this counts only what it can hold.
  constexpr size_type capacity() const noexcept {
    const size_type ceiling = max_size();
    return capacity_ < ceiling ? static_cast<size_type>(capacity_) : ceiling;
  }

  /// The allocator's own bound, kept within difference_type so that the
  /// distance between any two iterators can represent it; zero when the
  /// block capacity limits ask for blocks larger than the allocator allows.
  constexpr size_type max_size() const noexcept {
    const size_type bound = AllocTraits::max_size(alloc_);
    if (bound < minBlockCapacity_) {
      return 0;
    }
    constexpr auto kDistanceCeiling =
        std::numeric_limits<difference_type>::max();
    return to_unsigned(kDistanceCeiling) < bound
        ? static_cast<size_type>(kDistanceCeiling)
        : bound;
  }

  /// Allocates blocks until capacity() is at least count. Never invalidates
  /// anything, and never moves an element. Strongly exception safe: an
  /// allocation failure leaves the hive, capacity() included, untouched.
  constexpr void reserve(size_type count) {
    if (count > max_size()) {
      throw_exception<std::length_error>(
          "folly::hive::reserve: count exceeds max_size");
    }
    // The new blocks are staged off to the side and linked in only once they
    // have all been allocated. allocateBlock has already counted each of them
    // in capacity_, so rolling back by deallocating restores it exactly. The
    // guard rather than a catch block keeps the failing allocation's own
    // exception - and its stack trace - on its way out untouched.
    Block* staged = nullptr;
    auto rollback = makeGuard([&] {
      while (staged != nullptr) {
        Block* next = staged->next;
        deallocateBlock(staged);
        staged = next;
      }
    });
    while (capacity_ < count) {
      const std::size_t deficit = count - capacity_;
      std::size_t wanted = nextBlockCapacity();
      if (wanted > deficit) {
        wanted = constexpr_max(deficit, minBlockCapacity_);
      }
      Block* block = allocateBlock(wanted);
      block->next = staged;
      staged = block;
    }
    rollback.dismiss();

    while (staged != nullptr) {
      Block* next = staged->next;
      pushReserved(staged);
      staged = next;
    }
  }

  /// Releases every block that holds no elements. Does not move elements, and
  /// so invalidates nothing.
  constexpr void trim_capacity() noexcept { freeReservedBlocks(); }

  /// Releases blocks that hold no elements, never taking capacity() below
  /// count. Linear in the number of blocks released, as the paper requires:
  /// spare blocks are taken from the front of the spare list for as long as
  /// each one fits within the excess, and the first that does not ends the
  /// walk rather than being stepped over.
  constexpr void trim_capacity(size_type count) noexcept {
    if (count >= capacity()) {
      return;
    }
    while ( //
        reservedHead_ != nullptr &&
        capacity_ - reservedHead_->capacity >= count) {
      Block* const block = reservedHead_;
      reservedHead_ = block->next;
      deallocateBlock(block);
    }
  }

  /// Reduces capacity() towards size(), moving elements into a compact set of
  /// blocks. Like reshape(), this invalidates iterators, pointers and
  /// references when it moves elements into new blocks. The blocks holding no
  /// elements are released first, which stands even if the compaction that
  /// follows throws; the compaction itself is done on a temporary and committed
  /// only on success, and only if it actually came out smaller. capacity()
  /// never rises. If moving an element throws, this hive keeps every element,
  /// though those already moved are left moved-from.
  constexpr void shrink_to_fit() {
    static_assert(
        detail::hive_move_insertable<Allocator, T> ||
            detail::hive_copy_insertable<Allocator, T>,
        "folly::hive::shrink_to_fit: T must be Cpp17MoveInsertable into hive");
    freeReservedBlocks();
    if (capacity_ == size_) {
      return;
    }
    hive compact(blockLimits(), alloc_);
    compact.reserve(size_);
    // The block capacity limits bound how small a set of blocks can be, so
    // the compacted hive is not always smaller than this one: limits that
    // have been widened since these blocks were allocated, or a size that
    // whole blocks cannot cover exactly, can land it higher. Committing that
    // would make shrink_to_fit grow the hive.
    if (compact.capacity_ >= capacity_) {
      return;
    }
    compact.appendMovedFrom(*this);
    swapStorage(compact);
  }

  //
  // modifiers
  //

  /// Constructs an element in place and returns an iterator to it. The element
  /// takes whatever storage is free - an erased slot, or a slot a block has
  /// never used - so nothing already in the hive moves and no pointer,
  /// reference or iterator to an element is invalidated. end() can move,
  /// though: an insertion past the last block's high-water mark moves it,
  /// whether into that block's never-used slots or into a new block linked
  /// onto the back. Read it again rather than holding a copy across an
  /// insertion. Amortized constant time, and strongly exception safe.
  template <typename... Args>
  constexpr iterator emplace(Args&&... args) {
    // Every block with room is on one list, so the common insertion is a load
    // and a branch. The slow path takes an insertion that has to grow the
    // hive - the head of that list being null - and one that would pass
    // max_size(), which free slots alone do not rule out: whole blocks can
    // take the storage a little past it.
    Block* const block = roomHead_;
    if (block == nullptr || size_ >= max_size()) [[unlikely]] {
      return emplaceSlow(std::forward<Args>(args)...);
    }
    if (block->freeHead != kNullIndex) {
      return emplaceInErasedSlot(block, std::forward<Args>(args)...);
    }
    return emplaceInUnusedSlot(block, std::forward<Args>(args)...);
  }

  /// Inserts a copy of value, or moves it in, and returns an iterator to it.
  constexpr iterator insert(const T& value) { return emplace(value); }
  constexpr iterator insert(T&& value) { return emplace(std::move(value)); }

  /// Hinted insertion. P0447R28 requires the hint to be ignored because a
  /// hive, rather than its caller, chooses the available slot.
  template <typename... Args>
  constexpr iterator emplace_hint(const_iterator, Args&&... args) {
    return emplace(std::forward<Args>(args)...);
  }
  constexpr iterator insert(const_iterator, const T& value) {
    return emplace(value);
  }
  constexpr iterator insert(const_iterator, T&& value) {
    return emplace(std::move(value));
  }

  /// Bulk insertion. Transactional: an element that throws leaves the hive
  /// with exactly the elements, addresses and capacity it had beforehand.
  constexpr void insert(size_type count, const T& value) {
    appendCopies(count, value);
  }

  template <detail::hive_cpp17_input_iterator InputIt>
  constexpr void insert(InputIt first, InputIt last) {
    appendRange(std::move(first), std::move(last));
  }

  constexpr void insert(std::initializer_list<T> values) {
    appendRange(values.begin(), values.end());
  }

  /// Inserts every element of a range, which need not be a common range:
  /// the internals take an iterator and a sentinel, so an istream_view or a
  /// take_while view works as well as a container.
  template <detail::hive_compatible_range<T> R>
  constexpr void insert_range(R&& range) {
    appendRange(std::ranges::begin(range), std::ranges::end(range));
  }

  /// Erases the element at position and returns an iterator to the next
  /// element. Every pointer, reference and iterator to another element stays
  /// valid; end() moves when the erasure empties the last block, which is
  /// unlinked and kept as a spare, so read end() again rather than holding a
  /// copy across an erasure.
  constexpr iterator erase(const_iterator position) {
    Block* const block = const_cast<Block*>(position.block_);
    const SkipType index = indexOf(block, position.slot_);

    AllocTraits::destroy(alloc_, elementOf(block->slots + index));
    --size_;
    --block->liveCount;

    if (block->liveCount == 0) [[unlikely]] {
      return eraseSlow(block);
    }

    // Reading the next position before the skipfield records this erasure
    // keeps the step a plain one off a live slot.
    iterator next = unconst(position);
    ++next;
    markErased(block, index);
    return next;
  }

  /// Erases [first, last) and returns an iterator to the element that
  /// followed the range.
  constexpr iterator erase(const_iterator first, const_iterator last) {
    // Emptying the last block unlinks it and moves end(), so a copy of it
    // taken before the erasures cannot be used as the loop's stopping point.
    const bool toEnd = last == cend();
    iterator it = unconst(first);
    if (toEnd) {
      while (it != end()) {
        it = erase(it);
      }
      return end();
    }
    const iterator stop = unconst(last);
    while (it != stop) {
      it = erase(it);
    }
    return it;
  }

  /// Destroys every element. The hive keeps its blocks as spares for reuse;
  /// call trim_capacity() to return them.
  constexpr void clear() noexcept {
    Block* block = front_;
    while (block != nullptr) {
      Block* next = block->next;
      destroyBlockElements(block);
      recycleBlock(block);
      block = next;
    }
    front_ = nullptr;
    back_ = nullptr;
    roomHead_ = nullptr;
    size_ = 0;
  }

  /// Exchanges contents with another hive. No element moves, so iterators,
  /// pointers and references stay valid - and now designate elements of the
  /// other hive, which is where their elements now are.
  constexpr void swap(hive& other) noexcept(
      AllocTraits::propagate_on_container_swap::value ||
      AllocTraits::is_always_equal::value) {
    swapAllocator(other.alloc_);
    swapStorage(other);
  }

  //
  // hive operations
  //

  /// Moves every element of source into *this without moving any element in
  /// memory: whole blocks change owner, so pointers and iterators into source
  /// stay valid and now refer to elements of *this. Linear in the number of
  /// blocks source holds - each is checked against this hive's block capacity
  /// limits and renumbered into this hive's iterator order - and no free slot
  /// of either hive is lost.
  constexpr void splice(hive& source) {
    if (this == &source || source.empty()) {
      return;
    }
    if (!(alloc_ == source.alloc_)) {
      throw_exception<std::invalid_argument>(
          "folly::hive::splice: allocators are not equal");
    }
    // Everything that can fail is checked before anything moves, so a
    // rejected splice leaves both hives exactly as they were. The paper names
    // one failure, a source block outside this hive's limits; the combined
    // size passing max_size() is the other, since no hive may hold more.
    // Spare blocks on either side play no part: the source keeps its own,
    // and this hive's never stand in the way.
    const size_type ceiling = max_size();
    if (size_ > ceiling || source.size_ > ceiling - size_) {
      throw_exception<std::length_error>(
          "folly::hive::splice: the combined size would exceed max_size");
    }
    std::size_t movedCapacity = 0;
    size_type movedBlocks = 0;
    const hive_limits limits = blockLimits();
    for (Block* block = source.front_; block != nullptr; block = block->next) {
      if (!blockFitsLimits(block, limits)) {
        throw_exception<std::length_error>(
            "folly::hive::splice: a source block is outside this hive's block "
            "capacity limits");
      }
      movedCapacity += block->capacity;
      ++movedBlocks;
    }
    // The keys the source's blocks carry belong to its order, not to this
    // one, so they are reassigned to continue this hive's. Renumbering the
    // whole chain first, if this hive's keys are too near the ceiling to fit
    // them all, keeps that arithmetic from wrapping.
    if (std::numeric_limits<std::size_t>::max() - orderCounter_ < movedBlocks) {
      renumberBlocks();
    }

    // Every block keeps its own free space reachable through the list of
    // blocks with room, so the source's blocks can simply be appended: no
    // element moves, no slot is stranded, and only a constant number of
    // links change.
    for (Block* block = source.roomHead_; block != nullptr;) {
      Block* const nextWithRoom = block->nextWithRoom;
      block->inRoomList = false;
      roomListPush(block);
      block = nextWithRoom;
    }

    if (front_ == nullptr) {
      front_ = source.front_;
    } else {
      back_->next = source.front_;
      source.front_->prev = back_;
    }
    // The blocks' allocation stamps were issued by the source's count, so
    // they are cleared rather than read against this hive's: a block that
    // was already allocated is never one a later rollback here may release.
    for (Block* block = source.front_; block != nullptr; block = block->next) {
      block->order = orderCounter_++;
      block->serial = 0;
    }
    back_ = source.back_;
    size_ += source.size_;
    capacity_ += movedCapacity;

    source.front_ = nullptr;
    source.back_ = nullptr;
    source.roomHead_ = nullptr;
    source.size_ = 0;
    source.capacity_ -= movedCapacity;
  }

  constexpr void splice(hive&& source) { splice(source); }

  /// Sorts the elements. No element moves, so every pointer, reference and
  /// iterator stays valid - and comes to name a different value, because it
  /// is the values that are permuted between the fixed addresses. Identity
  /// here travels with the address, not with the element. Stable: equivalent
  /// elements keep their relative order.
  template <typename Compare = std::less<T>>
  constexpr void sort(Compare compare = Compare()) {
    static_assert(
        detail::hive_move_insertable<Allocator, T> &&
            std::is_move_assignable_v<T>,
        "folly::hive::sort: T must be Cpp17MoveInsertable into hive and "
        "Cpp17MoveAssignable");
    if (size_ < 2) {
      return;
    }
    sortNonEmpty(compare);
  }

 private:
  template <typename Compare>
  void sortNonEmpty(Compare& compare) {
    // Scratch comes from this container's allocator, rebound, not from the
    // global heap: a hive over an arena must not step outside it to sort.
    const ElementPtrAlloc elementAlloc(alloc_);
    std::vector<T*, ElementPtrAlloc> elements(elementAlloc);
    elements.reserve(size_);
    for (T& element : *this) {
      elements.push_back(std::addressof(element));
    }
    const std::size_t count = elements.size();

    // order[i] is the index of the element whose value belongs at position i.
    std::vector<std::size_t, IndexAlloc> order(count, IndexAlloc(alloc_));
    for (std::size_t i = 0; i < count; ++i) {
      order[i] = i;
    }
    {
      std::vector<std::size_t, IndexAlloc> buffer(count, IndexAlloc(alloc_));
      stableSortIndices(
          order.data(), buffer.data(), count, [&](auto l, auto r) {
            return compare(*elements[l], *elements[r]);
          });
    }

    // Apply the permutation in place, one cycle at a time: the value that ends
    // up at position i is the one that started at order[i]. The value lifted
    // out to open each cycle is held in a temporary the allocator builds, so
    // sorting asks of T only what the paper does - Cpp17MoveInsertable and
    // Cpp17MoveAssignable - and not an accessible move constructor. Each
    // position a cycle fills is marked done by pointing its entry in order
    // at itself, which is how a position already in place looks too.
    for (std::size_t i = 0; i < count; ++i) {
      if (order[i] == i) {
        continue;
      }
      AllocatorTemporary held(alloc_, std::move(*elements[i]));
      std::size_t slot = i;
      while (true) {
        const std::size_t source = std::exchange(order[slot], slot);
        if (source == i) {
          *elements[slot] = std::move(held.value());
          break;
        }
        *elements[slot] = std::move(*elements[source]);
        slot = source;
      }
    }
  }

  /// A bottom-up merge sort of indices: stable, O(N log N) comparisons, and
  /// self-contained, so that this header does without <algorithm>. Each pass
  /// merges runs from one buffer into the other, the left run winning ties,
  /// and the result is copied back if it ends in the scratch buffer.
  template <typename Less>
  static void stableSortIndices(
      std::size_t* data,
      std::size_t* scratch,
      std::size_t count,
      const Less& less) {
    std::size_t* from = data;
    std::size_t* to = scratch;
    for (std::size_t width = 1; width < count; width *= 2) {
      for (std::size_t low = 0; low < count; low += 2 * width) {
        const std::size_t middle = constexpr_min(low + width, count);
        const std::size_t high = constexpr_min(middle + width, count);
        std::size_t left = low;
        std::size_t right = middle;
        std::size_t out = low;
        while (left < middle && right < high) {
          to[out++] =
              less(from[right], from[left]) ? from[right++] : from[left++];
        }
        while (left < middle) {
          to[out++] = from[left++];
        }
        while (right < high) {
          to[out++] = from[right++];
        }
        if (high == count) {
          break;
        }
      }
      std::swap(from, to);
    }
    if (from != data) {
      for (std::size_t i = 0; i < count; ++i) {
        data[i] = from[i];
      }
    }
  }

 public:
  /// Erases all but the first element of every group of consecutive equivalent
  /// elements, and returns the number erased.
  template <typename BinaryPredicate = std::equal_to<T>>
  constexpr size_type unique(BinaryPredicate predicate = BinaryPredicate()) {
    if (size_ < 2) {
      return 0;
    }
    // How many elements the loop has to consider is known before it starts,
    // and each pass consumes exactly one of them, so a counter can stand in
    // for the stopping iterator: end() cannot be hoisted out of a loop that
    // erases, because an erasure which empties the last block unlinks it and
    // moves end().
    size_type erased = 0;
    iterator previous = begin();
    iterator current = std::next(previous);
    for (size_type remaining = size_ - 1; remaining != 0; --remaining) {
      // The predicate is invoked as pred(*i, *(i - 1)) - the candidate for
      // erasure first, the element being kept second. Symmetric predicates
      // cannot tell, but an asymmetric one can.
      if (predicate(*current, *previous)) {
        current = erase(current);
        ++erased;
      } else {
        previous = current;
        ++current;
      }
    }
    return erased;
  }

  //
  // block capacity limits
  //

  /// The block capacity limits in force, as set by a constructor or reshape().
  constexpr hive_limits block_capacity_limits() const noexcept {
    return blockLimits();
  }

  /// The limits a hive uses when none are given.
  static constexpr hive_limits block_capacity_default_limits() noexcept {
    return hive_limits{kDefaultMinBlockCapacity, kDefaultMaxBlockCapacity};
  }

  /// The widest limits any hive of this type will accept.
  static constexpr hive_limits block_capacity_hard_limits() noexcept {
    return hive_limits{kHardMinBlockCapacity, kHardMaxBlockCapacity};
  }

  /// Changes the block capacity limits. Blocks that already conform are kept as
  /// they are; if any does not, elements are moved into conforming blocks,
  /// which invalidates iterators, pointers and references. The new blocks
  /// are filled on a temporary and committed only once every element is in
  /// them, so a failure leaves this hive with its own elements, blocks and
  /// limits - though an element whose move had already happened is left
  /// moved-from, which the paper allows.
  constexpr void reshape(hive_limits limits) {
    validateLimits(limits);

    if (blocksConformToLimits(limits)) {
      setLimits(limits);
      dropNonConformingReserved();
      return;
    }

    static_assert(
        detail::hive_move_insertable<Allocator, T> ||
            detail::hive_copy_insertable<Allocator, T>,
        "folly::hive::reshape: T must be Cpp17MoveInsertable into hive to "
        "move elements out of non-conforming blocks");
    // Everything that can throw - the new blocks and the moves into them -
    // happens on the temporary, so a failure leaves this hive holding its
    // own elements, blocks and limits.
    hive reshaped(limits, alloc_);
    reshaped.reserve(size_);
    reshaped.appendMovedFrom(*this);
    swapStorage(reshaped);
  }

  //
  // element lookup
  //

  /// Returns an iterator to the element at the given address, or end() if
  /// that address does not currently hold a live element of this hive.
  /// Linear in the number of blocks.
  constexpr iterator get_iterator(const_pointer element) noexcept {
    return unconst(std::as_const(*this).get_iterator(element));
  }

  constexpr const_iterator get_iterator(const_pointer element) const noexcept {
    const T* const address = std::to_address(element);
    for (const Block* block = front_; block != nullptr; block = block->next) {
      const SkipType index = liveIndexOf(block, address);
      if (index != kNullIndex) {
        return const_iterator(
            block, block->slots + index, block->skipfield + index);
      }
    }
    return end();
  }

 private:
  /// One element, built and destroyed through the allocator in storage this
  /// object owns. Cpp17CopyInsertable and Cpp17MoveInsertable say the
  /// ALLOCATOR can copy or move an element, which is not the same as the
  /// element having an accessible copy or move constructor: `T held = value;`
  /// would demand the latter.
  class AllocatorTemporary {
   public:
    template <typename... Args>
    constexpr explicit AllocatorTemporary(Allocator& alloc, Args&&... args)
        : alloc_(alloc) {
      AllocTraits::construct(alloc_, address(), std::forward<Args>(args)...);
    }
    AllocatorTemporary(const AllocatorTemporary&) = delete;
    AllocatorTemporary(AllocatorTemporary&&) = delete;
    AllocatorTemporary& operator=(const AllocatorTemporary&) = delete;
    AllocatorTemporary& operator=(AllocatorTemporary&&) = delete;
    constexpr ~AllocatorTemporary() { AllocTraits::destroy(alloc_, address()); }

    constexpr T& value() noexcept { return storage_.value; }

   private:
    constexpr T* address() noexcept { return std::addressof(storage_.value); }

    Allocator& alloc_;
    Slot storage_{};
  };

  //
  // bulk insertion
  //

  // Bookkeeping for this container comes from this container's allocator,
  // rebound: the positions a bulk insertion remembers, and the scratch
  // sort() needs to permute values between fixed addresses.
  using PositionAlloc = typename AllocTraits::template rebind_alloc<iterator>;
  using ElementPtrAlloc = typename AllocTraits::template rebind_alloc<T*>;
  using IndexAlloc = typename AllocTraits::template rebind_alloc<std::size_t>;

  /// Undoes a bulk insertion that threw part-way: everything inserted under
  /// the guard is erased again, in reverse, and the blocks allocated since
  /// the guard went up are released - those and no others, since no element
  /// that was already in the hive can be in one of them - so the hive keeps
  /// exactly the elements, at exactly the addresses, with exactly the
  /// capacity and spare blocks, that it held beforehand.
  ///
  /// Remembering every position costs three pointers per element, so it is
  /// done only where an element-by-element undo is the only way back. When
  /// the hive started empty, undoing means clearing it. When every insertion
  /// after the up-front reservation is noexcept, only the reservation can
  /// fail, and it undoes itself.
  class InsertionRollback {
   public:
    constexpr InsertionRollback(
        hive& owner, size_type expected, bool insertionsCannotThrow)
        : owner_(owner),
          allocations_(owner.allocations_),
          undo_(
              owner.size_ == 0 ? Undo::kClear
                  : insertionsCannotThrow
                  ? Undo::kNothing
                  : Undo::kErase),
          positions_(PositionAlloc(owner.alloc_)) {
      if (undo_ == Undo::kErase && expected != 0) {
        // Reserving up front keeps record() from allocating, and a failure
        // here happens before anything has been inserted.
        positions_.reserve(expected);
      }
    }
    InsertionRollback(const InsertionRollback&) = delete;
    InsertionRollback(InsertionRollback&&) = delete;
    InsertionRollback& operator=(const InsertionRollback&) = delete;
    InsertionRollback& operator=(InsertionRollback&&) = delete;

    constexpr ~InsertionRollback() {
      if (!armed_) {
        return;
      }
      if (undo_ == Undo::kErase) {
        for (auto it = positions_.rbegin(); it != positions_.rend(); ++it) {
          owner_.erase(*it);
        }
      } else if (undo_ == Undo::kClear) {
        owner_.clear();
      }
      owner_.releaseBlocksAllocatedAfter(allocations_);
    }

    constexpr void record(iterator position) {
      if (undo_ != Undo::kErase) {
        return;
      }
      auto rollback = makeGuard([&] { owner_.erase(position); });
      positions_.push_back(position);
      rollback.dismiss();
    }

    constexpr void commit() noexcept { armed_ = false; }

   private:
    enum class Undo { kErase, kClear, kNothing };

    hive& owner_;
    std::size_t allocations_;
    Undo undo_;
    std::vector<iterator, PositionAlloc> positions_;
    bool armed_ = true;
  };

  //
  // block allocation
  //

  constexpr Block* allocateBlock(std::size_t requested) {
    // Every block this hive owns honours the block capacity limits, and none
    // is larger than the allocator will hand over in one piece.
    const std::size_t ceiling = largestBlockCapacity();
    if (ceiling < minBlockCapacity_) {
      throw_exception<std::length_error>(
          "folly::hive: the allocator cannot provide a block within the block "
          "capacity limits");
    }
    const SkipType blockCapacity = static_cast<SkipType>(
        std::clamp(requested, minBlockCapacity_, ceiling));
    const std::size_t skipfieldLength =
        static_cast<std::size_t>(blockCapacity) + 1;

    BlockAlloc blockAlloc(alloc_);
    SlotAlloc slotAlloc(alloc_);
    SkipAlloc skipAlloc(alloc_);
    // The counts are checked against what the rebound allocators will
    // accept before anything is asked of them. The hard limits already keep
    // each count within its allocator's size_type, so the conversions below
    // never narrow.
    if (blockCapacity > SlotAllocTraits::max_size(slotAlloc) ||
        skipfieldLength > SkipAllocTraits::max_size(skipAlloc)) {
      throw_exception<std::length_error>(
          "folly::hive: the allocator cannot provide a block within the block "
          "capacity limits");
    }
    const auto slotCount =
        static_cast<typename SlotAllocTraits::size_type>(blockCapacity);
    const auto skipCount =
        static_cast<typename SkipAllocTraits::size_type>(skipfieldLength);

    // Each piece is guarded as it is acquired and the guards are dismissed
    // together once nothing can throw any more, so a failing allocation's own
    // exception - stack trace and all - travels out untouched. Each allocate
    // hands back the allocator's own pointer type, which is kept for the
    // matching deallocate; to_address gives the raw address everything else
    // works on.
    const auto blockHandle = BlockAllocTraits::allocate(blockAlloc, 1);
    auto blockGuard = makeGuard([&] {
      BlockAllocTraits::deallocate(blockAlloc, blockHandle, 1);
    });
    const SlotHandle slotsHandle =
        SlotAllocTraits::allocate(slotAlloc, slotCount);
    auto slotsGuard = makeGuard([&] {
      SlotAllocTraits::deallocate(slotAlloc, slotsHandle, slotCount);
    });
    const SkipHandle skipHandle =
        SkipAllocTraits::allocate(skipAlloc, skipCount);
    auto skipfieldGuard = makeGuard([&] {
      SkipAllocTraits::deallocate(skipAlloc, skipHandle, skipCount);
    });

    Block* const block = std::to_address(blockHandle);
    Slot* const slots = std::to_address(slotsHandle);
    SkipType* const skipfield = std::to_address(skipHandle);

    // Block is this container's own bookkeeping rather than an element, so
    // it is constructed directly instead of through the rebound allocator's
    // construct(). An allocator that does something in construct() - a
    // scoped_allocator_adaptor handing itself to whatever is being built -
    // must not do it to a Block, which knows nothing of allocators; only the
    // storage for it comes from the allocator.
    std::construct_at(block);
    skipfieldGuard.dismiss();
    slotsGuard.dismiss();
    blockGuard.dismiss();

    // The slots are ours to keep raw until an element is placed in one, but
    // the Slot objects themselves have to exist before their storage is named
    // through the union. Slot's default constructor does nothing and cannot
    // throw, so no rollback is needed here. construct_at rather than
    // placement new, which is not allowed during constant evaluation.
    for (std::size_t i = 0; i < blockCapacity; ++i) {
      std::construct_at(slots + i);
    }
    for (std::size_t i = 0; i < skipfieldLength; ++i) {
      skipfield[i] = 0;
    }

    block->slotsHandle = slotsHandle;
    block->skipHandle = skipHandle;
    block->slots = slots;
    block->skipfield = skipfield;
    block->capacity = blockCapacity;
    block->serial = ++allocations_;
    capacity_ += blockCapacity;
    return block;
  }

  constexpr void deallocateBlock(Block* block) noexcept {
    for (std::size_t i = 0; i < block->capacity; ++i) {
      block->slots[i].~Slot();
    }
    const SlotHandle slotsHandle = block->slotsHandle;
    const SkipHandle skipHandle = block->skipHandle;
    const std::size_t blockCapacity = block->capacity;

    SkipAlloc skipAlloc(alloc_);
    SkipAllocTraits::deallocate(
        skipAlloc,
        skipHandle,
        static_cast<typename SkipAllocTraits::size_type>(blockCapacity + 1));
    SlotAlloc slotAlloc(alloc_);
    SlotAllocTraits::deallocate(
        slotAlloc,
        slotsHandle,
        static_cast<typename SlotAllocTraits::size_type>(blockCapacity));
    capacity_ -= blockCapacity;
    // The block's own handle is rebuilt from its address rather than kept in
    // it: keeping it would make Block name its allocator's pointer type while
    // Block is still incomplete. pointer_to is what the allocator
    // requirements provide for exactly this.
    BlockAlloc blockAlloc(alloc_);
    using PtrTraits = std::pointer_traits<typename BlockAllocTraits::pointer>;
    const auto self = PtrTraits::pointer_to(*block);
    std::destroy_at(block);
    BlockAllocTraits::deallocate(blockAlloc, self, 1);
  }

  /// The largest block this hive may allocate: within its block capacity
  /// limits, and no larger than the allocator allows.
  constexpr std::size_t largestBlockCapacity() const noexcept {
    const std::size_t bound = AllocTraits::max_size(alloc_);
    return constexpr_min(bound, maxBlockCapacity_);
  }

  constexpr std::size_t nextBlockCapacity() const noexcept {
    // Each new block is as large as everything allocated so far, so total
    // capacity doubles, bounded by the block capacity limits.
    return std::clamp(capacity_, minBlockCapacity_, maxBlockCapacity_);
  }

  constexpr Block* acquireBlock(bool& fromSpare) {
    if (reservedHead_ != nullptr) {
      Block* block = reservedHead_;
      reservedHead_ = block->next;
      block->next = nullptr;
      fromSpare = true;
      return block;
    }
    fromSpare = false;
    return allocateBlock(nextBlockCapacity());
  }

  constexpr void pushReserved(Block* block) noexcept {
    block->prev = nullptr;
    block->next = reservedHead_;
    reservedHead_ = block;
  }

  /// Returns an emptied block to the reserved list for a later insertion.
  constexpr void recycleBlock(Block* block) noexcept {
    roomListRemove(block);
    for (std::size_t i = 0; i <= block->used; ++i) {
      block->skipfield[i] = 0;
    }
    block->used = 0;
    block->liveCount = 0;
    block->freeHead = kNullIndex;
    pushReserved(block);
  }

  /// Releases every spare block the predicate selects - the one walk of the
  /// spare list that unlinks and deallocates, so that the list surgery is
  /// written once.
  template <typename Predicate>
  constexpr void releaseReservedIf(Predicate predicate) noexcept {
    Block** link = &reservedHead_;
    while (*link != nullptr) {
      Block* const block = *link;
      if (predicate(static_cast<const Block*>(block))) {
        *link = block->next;
        deallocateBlock(block);
      } else {
        link = &block->next;
      }
    }
  }

  constexpr void freeReservedBlocks() noexcept {
    releaseReservedIf([](const Block*) { return true; });
  }

  /// Releases the spare blocks allocated after the given point in this
  /// hive's count of allocations. Used only to undo a failed bulk insertion,
  /// whose blocks are all spare again by the time this runs.
  constexpr void releaseBlocksAllocatedAfter(std::size_t allocations) noexcept {
    releaseReservedIf([=](const Block* block) {
      return block->serial > allocations;
    });
  }

  constexpr void dropNonConformingReserved() noexcept {
    const hive_limits limits = blockLimits();
    releaseReservedIf([=](const Block* block) {
      return !blockFitsLimits(block, limits);
    });
  }

  /// Whether a block is within the bounds of the given limits: the one test
  /// of which blocks a hive may own.
  static constexpr bool blockFitsLimits(
      const Block* block, hive_limits limits) noexcept {
    return block->capacity >= limits.min && block->capacity <= limits.max;
  }

  constexpr void linkBack(Block* block) noexcept {
    block->prev = back_;
    block->next = nullptr;
    block->order = nextOrder();
    if (back_ == nullptr) {
      front_ = block;
    } else {
      back_->next = block;
    }
    back_ = block;
  }

  /// The next key for a block being linked onto the back of the chain. Keys
  /// only ever increase, so this runs out eventually; renumbering the chain
  /// when it does keeps the order correct at a cost linear in the number of
  /// blocks. Reaching this needs 2^64 block links, so it is a guard rather
  /// than a path anything takes - renumberBlocks is exercised by splice.
  constexpr std::size_t nextOrder() noexcept {
    constexpr auto kLastOrder = std::numeric_limits<std::size_t>::max();
    if (orderCounter_ == kLastOrder) [[unlikely]] {
      renumberBlocks();
    }
    return orderCounter_++;
  }

  /// Renumbers the whole chain from zero, keeping the existing order.
  constexpr void renumberBlocks() noexcept {
    std::size_t order = 0;
    for (Block* block = front_; block != nullptr; block = block->next) {
      block->order = order++;
    }
    orderCounter_ = order;
  }

  constexpr void unlinkBlock(Block* block) noexcept {
    if (block->prev != nullptr) {
      block->prev->next = block->next;
    } else {
      front_ = block->next;
    }
    if (block->next != nullptr) {
      block->next->prev = block->prev;
    } else {
      back_ = block->prev;
    }
    block->prev = nullptr;
    block->next = nullptr;
  }

  //
  // free lists
  //

  constexpr void roomListPush(Block* block) noexcept {
    if (block->inRoomList) {
      return;
    }
    block->prevWithRoom = nullptr;
    block->nextWithRoom = roomHead_;
    if (roomHead_ != nullptr) {
      roomHead_->prevWithRoom = block;
    }
    roomHead_ = block;
    block->inRoomList = true;
  }

  constexpr void roomListRemove(Block* block) noexcept {
    if (!block->inRoomList) {
      return;
    }
    if (block->prevWithRoom != nullptr) {
      block->prevWithRoom->nextWithRoom = block->nextWithRoom;
    } else {
      roomHead_ = block->nextWithRoom;
    }
    if (block->nextWithRoom != nullptr) {
      block->nextWithRoom->prevWithRoom = block->prevWithRoom;
    }
    block->prevWithRoom = nullptr;
    block->nextWithRoom = nullptr;
    block->inRoomList = false;
  }

  static constexpr bool blockHasRoom(const Block* block) noexcept {
    return block->freeHead != kNullIndex || block->used < block->capacity;
  }

  /// Brings a block's membership of the list of blocks with room back in line
  /// with the block's state, after an insertion or an erasure changed it.
  constexpr void refreshRoom(Block* block) noexcept {
    if (blockHasRoom(block)) {
      roomListPush(block);
    } else {
      roomListRemove(block);
    }
  }

  // The free list holds one node per run of erased slots, in the storage of the
  // slot that starts the run, so that reuse always splits a run at its front.
  constexpr void freeListPush(Block* block, SkipType index) noexcept {
    block->slots[index].free.prev = kNullIndex;
    block->slots[index].free.next = block->freeHead;
    if (block->freeHead != kNullIndex) {
      block->slots[block->freeHead].free.prev = index;
    } else {
      roomListPush(block);
    }
    block->freeHead = index;
  }

  constexpr void freeListRemove(Block* block, SkipType index) noexcept {
    freeListUnlink(
        block, block->slots[index].free.prev, block->slots[index].free.next);
  }

  constexpr void freeListMove(
      Block* block, SkipType from, SkipType to) noexcept {
    freeListRelink(
        block, block->slots[from].free.prev, block->slots[from].free.next, to);
  }

  // The two halves of the free-list surgery, taking a node's links by value.
  // An insertion into an erased slot needs them in that form: by the time it
  // updates the list, the element it constructed has overwritten the node.

  /// Takes the node whose links are prev and next out of the list.
  constexpr void freeListUnlink(
      Block* block, SkipType prev, SkipType next) noexcept {
    if (prev != kNullIndex) {
      block->slots[prev].free.next = next;
    } else {
      block->freeHead = next;
    }
    if (next != kNullIndex) {
      block->slots[next].free.prev = prev;
    }
    if (block->freeHead == kNullIndex) {
      refreshRoom(block);
    }
  }

  /// Puts a node with links prev and next in the list at index to.
  constexpr void freeListRelink(
      Block* block, SkipType prev, SkipType next, SkipType to) noexcept {
    block->slots[to].free.prev = prev;
    block->slots[to].free.next = next;
    if (prev != kNullIndex) {
      block->slots[prev].free.next = to;
    } else {
      block->freeHead = to;
    }
    if (next != kNullIndex) {
      block->slots[next].free.prev = to;
    }
  }

  /// Marks a slot whose element has just been destroyed as erased, merging it
  /// with any erased run on either side. A run records its length in the
  /// entries at both of its ends; entries inside a run are merely non-zero.
  constexpr void markErased(Block* block, SkipType index) noexcept {
    SkipType* skipfield = block->skipfield;
    // Reading one past the last used slot is safe and always zero: it is the
    // block's trailing sentinel or an untouched slot.
    const bool mergeLeft = index != 0 && skipfield[index - 1] != 0;
    const bool mergeRight = skipfield[index + 1] != 0;

    if (mergeLeft && mergeRight) {
      const SkipType first =
          static_cast<SkipType>(index - skipfield[index - 1]);
      const SkipType last = static_cast<SkipType>(index + skipfield[index + 1]);
      const SkipType length = static_cast<SkipType>(last - first + 1);
      skipfield[first] = length;
      skipfield[last] = length;
      skipfield[index] = length;
      freeListRemove(block, static_cast<SkipType>(index + 1));
    } else if (mergeLeft) {
      const SkipType first =
          static_cast<SkipType>(index - skipfield[index - 1]);
      const SkipType length = static_cast<SkipType>(index - first + 1);
      skipfield[first] = length;
      skipfield[index] = length;
    } else if (mergeRight) {
      const SkipType last = static_cast<SkipType>(index + skipfield[index + 1]);
      const SkipType length = static_cast<SkipType>(last - index + 1);
      skipfield[index] = length;
      skipfield[last] = length;
      freeListMove(block, static_cast<SkipType>(index + 1), index);
    } else {
      skipfield[index] = 1;
      freeListPush(block, index);
    }
  }

  //
  // insertion
  //

  template <typename... Args>
  constexpr iterator emplaceInErasedSlot(Block* block, Args&&... args) {
    const SkipType index = block->freeHead;
    const SkipType runLength = block->skipfield[index];
    // The slot heads the block's free list, so its node has no predecessor
    // and only the successor needs keeping. Constructing the element writes
    // over the node, and a constructor that throws may already have written
    // part of it: read the successor first, put the node back if construction
    // fails, and commit the bookkeeping only once it has succeeded.
    const SkipType nextFree = block->slots[index].free.next;

    auto restore = makeGuard([&] {
      block->slots[index].free.prev = kNullIndex;
      block->slots[index].free.next = nextFree;
    });
    AllocTraits::construct(
        alloc_, elementOf(block->slots + index), std::forward<Args>(args)...);
    restore.dismiss();

    block->skipfield[index] = 0;
    if (runLength > 1) {
      const SkipType last = static_cast<SkipType>(index + runLength - 1);
      const SkipType shortened = static_cast<SkipType>(runLength - 1);
      const SkipType moved = static_cast<SkipType>(index + 1);
      block->skipfield[moved] = shortened;
      block->skipfield[last] = shortened;
      freeListRelink(block, kNullIndex, nextFree, moved);
    } else {
      freeListUnlink(block, kNullIndex, nextFree);
    }
    ++block->liveCount;
    ++size_;
    return iterator(block, block->slots + index, block->skipfield + index);
  }

  /// Puts an element in the first slot a block has never used. Those slots
  /// are past the block's high-water mark, so nothing - not iteration, not
  /// get_iterator - can reach them until this moves the mark.
  template <typename... Args>
  constexpr iterator emplaceInUnusedSlot(Block* block, Args&&... args) {
    const SkipType index = block->used;
    AllocTraits::construct(
        alloc_, elementOf(block->slots + index), std::forward<Args>(args)...);
    block->used = static_cast<SkipType>(index + 1);
    ++block->liveCount;
    ++size_;
    refreshRoom(block);
    return iterator{block, block->slots + index, block->skipfield + index};
  }

  /// The insertion that has to grow the hive, or that would pass max_size():
  /// with no block that has room, this takes a spare block if the hive is
  /// holding one and allocates otherwise. Kept out of line so that the
  /// common insertion, which only has to find a free slot, stays small
  /// enough to inline into its caller.
  template <typename... Args>
  FOLLY_NOINLINE constexpr iterator emplaceSlow(Args&&... args) {
    if (size_ >= max_size()) {
      throw_exception<std::length_error>(
          "folly::hive::emplace: size would exceed max_size");
    }
    return emplaceInNewBlock(std::forward<Args>(args)...);
  }

  template <typename... Args>
  constexpr iterator emplaceInNewBlock(Args&&... args) {
    bool fromSpare = false;
    Block* block = acquireBlock(fromSpare);
    // A block allocated for this insertion is released again: a failed
    // insertion must not leave capacity() larger than it found it.
    auto release = makeGuard([&] {
      if (fromSpare) {
        pushReserved(block);
      } else {
        deallocateBlock(block);
      }
    });
    AllocTraits::construct(
        alloc_, elementOf(block->slots), std::forward<Args>(args)...);
    release.dismiss();

    block->used = 1;
    block->liveCount = 1;
    linkBack(block);
    refreshRoom(block);
    ++size_;
    return iterator{block, block->slots, block->skipfield};
  }

  // The guard goes up before the storage is reserved, so a bulk insertion
  // that fails puts capacity() back exactly as well.
  constexpr void appendDefault(size_type count) {
    if (count == 0) {
      return;
    }
    appendDefaultNonEmpty(count);
  }

  void appendDefaultNonEmpty(size_type count) {
    constexpr bool kCannotThrow =
        detail::hive_nothrow_alloc_construct<Allocator, T>();
    appendCountedNonEmpty(count, kCannotThrow, [&] { return emplace(); });
  }

  void assignCopiesNonEmpty(size_type count, const T& value) {
    AllocatorTemporary retained(alloc_, value);
    clear();
    appendCopiesNonEmpty(count, retained.value());
  }

  constexpr void appendCopies(size_type count, const T& value) {
    if (count == 0) {
      return;
    }
    appendCopiesNonEmpty(count, value);
  }

  void appendCopiesNonEmpty(size_type count, const T& value) {
    constexpr bool kCannotThrow =
        detail::hive_nothrow_alloc_construct<Allocator, T, const T&>();
    appendCountedNonEmpty(count, kCannotThrow, [&] { return emplace(value); });
  }

  /// The transaction every insertion of a known count runs: the guard goes
  /// up, the storage is reserved, and each element is inserted and
  /// recorded. Reserving first means that nothing after it allocates, so
  /// when constructing an element cannot throw either, nothing can fail
  /// once the reservation has succeeded.
  template <typename EmplaceOne>
  void appendCountedNonEmpty(
      size_type count, bool insertionsCannotThrow, EmplaceOne emplaceOne) {
    InsertionRollback rollback(*this, count, insertionsCannotThrow);
    reserveForAppend(count);
    for (size_type i = 0; i < count; ++i) {
      rollback.record(emplaceOne());
    }
    rollback.commit();
  }

  /// Appends everything in [first, last), where last is a sentinel that need
  /// not have the iterator's type: a non-common range - an istream_view, a
  /// take_while, a counted iterator against default_sentinel - arrives here
  /// as it is rather than being forced into a common range first.
  ///
  /// The element count is taken up front only when it is available in
  /// constant time, so that the storage can be reserved in one step; a
  /// single-pass source simply grows the hive as it goes.
  template <typename InputIt, typename Sentinel>
  constexpr void appendRange(InputIt first, Sentinel last) {
    // An empty range inserts nothing, so there is nothing to roll back and
    // no bookkeeping worth building - which is also what lets an empty hive
    // be copied during constant evaluation.
    if (first == last) {
      return;
    }
    appendNonEmptyRange(first, last);
  }

  template <typename InputIt, typename Sentinel>
  void appendNonEmptyRange(InputIt& first, Sentinel& last) {
    size_type expected = 0;
    if constexpr (std::sized_sentinel_for<Sentinel, InputIt>) {
      const auto count = last - first;
      expected = count > 0 ? static_cast<size_type>(count) : 0;
    } else if constexpr (
        std::same_as<InputIt, Sentinel> &&
        detail::hive_cpp17_forward_iterator<InputIt>) {
      expected = static_cast<size_type>(std::distance(first, last));
    }
    InsertionRollback rollback(*this, expected, false);
    if (expected != 0) {
      reserveForAppend(expected);
    }
    for (; first != last; ++first) {
      rollback.record(emplace(*first));
    }
    rollback.commit();
  }

  /// Moves every element of source into *this and empties source. Elements do
  /// move in memory, so this is only used where invalidation is expected.
  ///
  /// Each element is moved through the allocator, which is all the paper
  /// asks: it does not ask for a copy whenever a move might throw, and
  /// choosing between the two by the core-language traits, as
  /// std::move_if_noexcept does, would reject element types the allocator
  /// can move and copy elements that could have been moved. A copy is made
  /// only of a type the allocator can copy but not move. The source keeps its
  /// elements until every one of them has been moved, so a throwing element
  /// leaves both hives holding their elements - those already moved are
  /// left moved-from.
  constexpr void appendMovedFrom(hive& source) {
    if (source.size_ == 0) {
      return;
    }
    appendMovedFromNonEmpty(source);
  }

  void appendMovedFromNonEmpty(hive& source) {
    iterator next = source.begin();
    if constexpr (detail::hive_move_insertable<Allocator, T>) {
      constexpr bool kCannotThrow =
          detail::hive_nothrow_alloc_construct<Allocator, T, T&&>();
      appendCountedNonEmpty(source.size_, kCannotThrow, [&] {
        const iterator position = emplace(std::move(*next));
        ++next;
        return position;
      });
    } else {
      constexpr bool kCannotThrow =
          detail::hive_nothrow_alloc_construct<Allocator, T, const T&>();
      appendCountedNonEmpty(source.size_, kCannotThrow, [&] {
        const iterator position = emplace(std::as_const(*next));
        ++next;
        return position;
      });
    }
    source.clear();
  }

  /// Takes on the elements of source one at a time, for the cases where the
  /// storage cannot simply change hands because the allocators differ.
  ///
  /// Whether an element can be moved is an allocator question, not a core
  /// language one: an allocator whose construct() takes over can build types
  /// that std::move_constructible rejects, and those go down the ordinary
  /// path here. Only a type the allocator genuinely cannot build leaves no
  /// way through - the paper makes that a precondition, and this reports it
  /// instead of moving nothing and saying nothing.
  constexpr void moveElementsFrom(hive& source) {
    if constexpr (
        detail::hive_move_insertable<Allocator, T> ||
        detail::hive_copy_insertable<Allocator, T>) {
      fillOrUnwind([&] { appendMovedFrom(source); });
    } else {
      throw_exception<std::invalid_argument>(
          "folly::hive: moving between unequal allocators needs an element "
          "type the allocator can construct");
    }
  }

  /// Makes room for count more elements. A batch that needs more room grows
  /// the hive geometrically, as insertion one element at a time does,
  /// rather than by exactly the batch: a run of short batches would
  /// otherwise allocate a block each.
  constexpr void reserveForAppend(size_type count) {
    const size_type limit = max_size();
    if (size_ > limit || count > limit - size_) {
      throw_exception<std::length_error>(
          "folly::hive: size would exceed max_size");
    }
    const std::size_t needed = static_cast<std::size_t>(size_) + count;
    if (needed <= capacity_) {
      return;
    }
    const std::size_t grown = constexpr_min(
        capacity_ + nextBlockCapacity(), static_cast<std::size_t>(limit));
    reserve(static_cast<size_type>(constexpr_max(needed, grown)));
  }

  //
  // erasure
  //

  /// The erasure that empties a block: the block leaves the chain and its
  /// storage joins the spare list, ready for a later insertion. Kept out of
  /// line - the common erasure only destroys an element and marks a slot -
  /// and it is also what moves end(), since the block it unlinks may be the
  /// last one.
  FOLLY_NOINLINE constexpr iterator eraseSlow(Block* block) noexcept {
    Block* const following = block->next;
    unlinkBlock(block);
    recycleBlock(block);
    return following == nullptr ? end() : firstIn<iterator>(following);
  }

  //
  // element and block teardown
  //

  static constexpr bool kTrivialTeardown =
      std::is_trivially_destructible_v<T> &&
      std::same_as<Allocator, std::allocator<T>>;

  constexpr void destroyBlockElements(Block* block) noexcept {
    if constexpr (kTrivialTeardown) {
      return;
    } else {
      destroyBlockElementsSlow(block);
    }
  }

  constexpr void destroyBlockElementsSlow(Block* block) noexcept {
    SkipType index = 0;
    while (index < block->used) {
      const SkipType skip = block->skipfield[index];
      if (skip != 0) {
        index = static_cast<SkipType>(index + skip);
        continue;
      }
      AllocTraits::destroy(alloc_, elementOf(block->slots + index));
      ++index;
    }
  }

  constexpr void destroyAllElements() noexcept {
    for (Block* block = front_; block != nullptr; block = block->next) {
      destroyBlockElements(block);
    }
  }

  constexpr void freeAllBlocks() noexcept {
    Block* block = front_;
    while (block != nullptr) {
      Block* next = block->next;
      deallocateBlock(block);
      block = next;
    }
    front_ = nullptr;
    back_ = nullptr;
    roomHead_ = nullptr;
    size_ = 0;
    freeReservedBlocks();
  }

  //
  // whole-container plumbing
  //

  /// Runs a constructor's body, releasing everything already built if it
  /// throws: a constructor that exits by exception leaves no destructor to run.
  template <typename Fn>
  constexpr void fillOrUnwind(Fn fn) {
    auto unwind = makeGuard([&] {
      destroyAllElements();
      freeAllBlocks();
    });
    fn();
    unwind.dismiss();
  }

  constexpr void adoptStorage(hive& source) noexcept {
    front_ = std::exchange(source.front_, nullptr);
    back_ = std::exchange(source.back_, nullptr);
    roomHead_ = std::exchange(source.roomHead_, nullptr);
    reservedHead_ = std::exchange(source.reservedHead_, nullptr);
    size_ = std::exchange(source.size_, size_type{0});
    capacity_ = std::exchange(source.capacity_, std::size_t{0});
    // The chain arrives already ordered, so its keys can be kept as they are,
    // and the blocks' allocation stamps go on being read against the count
    // that issued them.
    orderCounter_ = std::exchange(source.orderCounter_, std::size_t{0});
    allocations_ = std::exchange(source.allocations_, std::size_t{0});
  }

  /// Takes over source's storage and the block capacity limits that describe
  /// it, which the paper transfers together.
  constexpr void adoptStorageAndLimits(hive& source) noexcept {
    minBlockCapacity_ = source.minBlockCapacity_;
    maxBlockCapacity_ = source.maxBlockCapacity_;
    adoptStorage(source);
  }

  /// Move assignment where the storage changes hands.
  constexpr void adoptAssigned(hive& source) noexcept {
    destroyAllElements();
    freeAllBlocks();
    moveAllocator(source.alloc_);
    adoptStorageAndLimits(source);
  }

  constexpr void swapStorage(hive& other) noexcept {
    std::swap(front_, other.front_);
    std::swap(back_, other.back_);
    std::swap(roomHead_, other.roomHead_);
    std::swap(reservedHead_, other.reservedHead_);
    std::swap(size_, other.size_);
    std::swap(capacity_, other.capacity_);
    std::swap(minBlockCapacity_, other.minBlockCapacity_);
    std::swap(maxBlockCapacity_, other.maxBlockCapacity_);
    std::swap(orderCounter_, other.orderCounter_);
    std::swap(allocations_, other.allocations_);
  }

  constexpr void copyAllocator(const Allocator& source) {
    if constexpr (AllocTraits::propagate_on_container_copy_assignment::value) {
      alloc_ = source;
    }
  }
  constexpr void moveAllocator(Allocator& source) {
    if constexpr (AllocTraits::propagate_on_container_move_assignment::value) {
      alloc_ = std::move(source);
    }
  }
  constexpr void swapAllocator(Allocator& other) noexcept {
    if constexpr (AllocTraits::propagate_on_container_swap::value) {
      using std::swap;
      swap(alloc_, other);
    }
  }

  constexpr hive_limits blockLimits() const noexcept {
    return hive_limits{minBlockCapacity_, maxBlockCapacity_};
  }

  static constexpr void validateLimits(hive_limits limits) {
    if (limits.min < kHardMinBlockCapacity ||
        limits.max > kHardMaxBlockCapacity || limits.min > limits.max) {
      throw_exception<std::length_error>(
          "folly::hive: block capacity limits are outside the hard limits");
    }
  }

  /// The hard limits are a property of the type: block_capacity_hard_limits
  /// is static in the paper, so it cannot consult a stateful allocator, and
  /// it reports what the representation allows - a skipfield entry has to
  /// hold the longest run of erased slots a block can contain, and a block
  /// capacity has to fit the allocator's size_type.
  ///
  /// What a particular allocator will actually hand over is a property of
  /// the instance, and it is max_size() that reports that: limits this
  /// allocator could never satisfy leave a hive whose max_size() is zero,
  /// which is a usable empty hive whose first insertion throws
  /// std::length_error. That keeps the static limits and the per-instance
  /// bound coherent without the static function having to see the instance.
  constexpr void setLimits(hive_limits limits) {
    validateLimits(limits);
    minBlockCapacity_ = limits.min;
    maxBlockCapacity_ = limits.max;
  }

  constexpr bool blocksConformToLimits(hive_limits limits) const noexcept {
    for (const Block* block = front_; block != nullptr; block = block->next) {
      if (!blockFitsLimits(block, limits)) {
        return false;
      }
    }
    return true;
  }

  //
  // position helpers
  //

  static constexpr SkipType indexOf(
      const Block* block, const Slot* slot) noexcept {
    return static_cast<SkipType>(slot - block->slots);
  }

  /// The index of element within block, or kNullIndex when block does not hold
  /// a live element at that address.
  static constexpr SkipType liveIndexOf(
      const Block* block, const T* element) noexcept {
    const bool inUsedSlots = detail::hive_pointer_in_range(
        element, block->slots, block->slots + block->used);
    if (!inUsedSlots) {
      return kNullIndex;
    }
    // reinterpret_cast is never allowed during constant evaluation, so the
    // constant-evaluated path walks the slots instead - which would be a
    // complexity regression at run time, where get_iterator is documented
    // linear in the number of blocks.
    if (std::is_constant_evaluated()) {
      for (std::size_t index = 0; index < block->used; ++index) {
        if (elementOf(block->slots + index) == element) {
          return block->skipfield[index] != 0
              ? kNullIndex
              : static_cast<SkipType>(index);
        }
      }
      return kNullIndex;
    }
    // The slot index comes from the integer values of the two addresses,
    // which is arithmetic on integers and so defined whatever element points
    // at; subtracting the pointers themselves would be defined only if
    // element were already known to point into the slot array, which is what
    // is being established. The candidate is then confirmed by comparing the
    // typed address of that slot's element with element - a comparison that
    // is always defined - so an address that is not exactly an element's,
    // such as one inside an element, finds nothing.
    const std::uintptr_t offset = reinterpret_cast<std::uintptr_t>(element) -
        reinterpret_cast<std::uintptr_t>(block->slots);
    const std::size_t index = static_cast<std::size_t>(offset / sizeof(Slot));
    if (index >= block->used || elementOf(block->slots + index) != element ||
        block->skipfield[index] != 0) {
      return kNullIndex;
    }
    return static_cast<SkipType>(index);
  }

  static constexpr iterator unconst(const_iterator position) noexcept {
    return iterator(
        const_cast<Block*>(position.block_),
        const_cast<Slot*>(position.slot_),
        position.skip_);
  }

  /// An iterator, of either constness, to a block's first live element:
  /// the first skipfield entry is the length of any erased run the block
  /// starts with, so it is also the index of that element.
  template <typename It, typename B>
  static constexpr It firstIn(B* block) noexcept {
    const SkipType lead = block->skipfield[0];
    return It(block, block->slots + lead, block->skipfield + lead);
  }

  /// An iterator, of either constness, to the position past a block's
  /// high-water mark: end() when the block is the last one.
  template <typename It, typename B>
  static constexpr It endOf(B* block) noexcept {
    return It(
        block, block->slots + block->used, block->skipfield + block->used);
  }

  Allocator alloc_{};
  Block* front_ = nullptr;
  Block* back_ = nullptr;
  Block* roomHead_ = nullptr;
  Block* reservedHead_ = nullptr;
  size_type size_ = 0;
  // Every slot of every block the hive owns, live, free and spare. This can
  // pass max_size() by less than one block, which is why it is a std::size_t
  // rather than a size_type and why capacity() reports it capped.
  std::size_t capacity_ = 0;
  std::size_t minBlockCapacity_ = kDefaultMinBlockCapacity;
  std::size_t maxBlockCapacity_ = kDefaultMaxBlockCapacity;
  std::size_t orderCounter_ = 0;
  // How many blocks this hive has allocated; each block is stamped with the
  // count at its allocation.
  std::size_t allocations_ = 0;
};

/// Exchanges the contents of two hives, without moving an element.
///
/// A hive has no operator== or operator!=, as P0447R28 specifies: its
/// iteration order depends on where storage happened to be free, so
/// elementwise comparison in that order would not be an equality.
template <typename T, typename Allocator>
constexpr void swap(hive<T, Allocator>& lhs, hive<T, Allocator>& rhs) noexcept(
    noexcept(lhs.swap(rhs))) {
  lhs.swap(rhs);
}

/// Erases every element that satisfies predicate, and returns the number
/// erased.
template <typename T, typename Allocator, typename Predicate>
constexpr auto erase_if(hive<T, Allocator>& container, Predicate predicate) ->
    typename hive<T, Allocator>::size_type {
  using size_type = typename hive<T, Allocator>::size_type;
  // How many elements the loop has to consider is known before it starts, and
  // each pass consumes exactly one of them - erased or kept - so a counter
  // stands in for the stopping iterator. end() cannot simply be hoisted out
  // of a loop that erases, because an erasure which empties the last block
  // unlinks it and moves end(); re-reading end() every pass, the alternative,
  // costs a load of the hive's tail block and two comparisons per element.
  size_type erased = 0;
  auto it = container.begin();
  for (size_type remaining = container.size(); remaining != 0; --remaining) {
    if (predicate(*it)) {
      it = container.erase(it);
      ++erased;
    } else {
      ++it;
    }
  }
  return erased;
}

/// Erases every element equal to value, and returns the number erased. The
/// predicate binds the element by `auto&`, exactly as the paper writes it,
/// so the comparison is against the element itself rather than against a
/// conversion of it.
template <typename T, typename Allocator, typename U = T>
constexpr auto erase(hive<T, Allocator>& container, const U& value) ->
    typename hive<T, Allocator>::size_type {
  return folly::erase_if(container, [&](auto& element) {
    return element == value;
  });
}

//
// deduction guides
//

template <
    detail::hive_cpp17_input_iterator InputIt,
    detail::hive_allocator_like Allocator =
        std::allocator<typename std::iterator_traits<InputIt>::value_type>>
hive(InputIt, InputIt, Allocator = Allocator())
    -> hive<typename std::iterator_traits<InputIt>::value_type, Allocator>;

template <
    detail::hive_cpp17_input_iterator InputIt,
    detail::hive_allocator_like Allocator =
        std::allocator<typename std::iterator_traits<InputIt>::value_type>>
hive(InputIt, InputIt, hive_limits, Allocator = Allocator())
    -> hive<typename std::iterator_traits<InputIt>::value_type, Allocator>;

template <typename T, detail::hive_allocator_like Allocator = std::allocator<T>>
hive(std::initializer_list<T>, Allocator = Allocator()) -> hive<T, Allocator>;

template <typename T, detail::hive_allocator_like Allocator = std::allocator<T>>
hive(std::initializer_list<T>, hive_limits, Allocator = Allocator())
    -> hive<T, Allocator>;

template <
    std::ranges::input_range R,
    detail::hive_allocator_like Allocator =
        std::allocator<std::ranges::range_value_t<R>>>
hive(from_range_t, R&&, Allocator = Allocator())
    -> hive<std::ranges::range_value_t<R>, Allocator>;

template <
    std::ranges::input_range R,
    detail::hive_allocator_like Allocator =
        std::allocator<std::ranges::range_value_t<R>>>
hive(from_range_t, R&&, hive_limits, Allocator = Allocator())
    -> hive<std::ranges::range_value_t<R>, Allocator>;

#if FOLLY_HAS_MEMORY_RESOURCE

namespace pmr {

template <typename T>
using hive = folly::hive<T, std::pmr::polymorphic_allocator<T>>;

} // namespace pmr

#endif

} // namespace folly
