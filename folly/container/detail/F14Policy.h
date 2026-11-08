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
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include <folly/Memory.h>
#include <folly/Portability.h>
#include <folly/Traits.h>
#include <folly/Unit.h>
#include <folly/Utility.h>
#include <folly/container/HeterogeneousAccess.h>
#include <folly/container/detail/F14Table.h>
#include <folly/hash/Hash.h>
#include <folly/lang/Align.h>
#include <folly/lang/Exception.h>
#include <folly/lang/SafeAssert.h>
#include <folly/memory/Allocator.h>
#include <folly/memory/Malloc.h>

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

namespace folly {
namespace f14 {
namespace detail {

template <typename Ptr>
using NonConstPtr = typename std::pointer_traits<Ptr>::template rebind<
    std::remove_const_t<typename std::pointer_traits<Ptr>::element_type>>;

template <typename KeyType, typename MappedType>
using MapValueType = std::pair<KeyType const, MappedType>;

template <typename KeyType, typename MappedTypeOrVoid>
using SetOrMapValueType = std::conditional_t<
    std::is_same<MappedTypeOrVoid, void>::value,
    KeyType,
    MapValueType<KeyType, MappedTypeOrVoid>>;

template <typename T>
using IsNothrowMoveAndDestroy = std::conjunction<
    std::is_nothrow_move_constructible<T>,
    std::is_nothrow_destructible<T>>;

// Used to enable EBO for Hasher, KeyEqual, and Alloc.  std::tuple of
// all empty objects is empty in libstdc++ but not libc++.
template <
    char Tag,
    typename T,
    bool Inherit = std::is_empty<T>::value && !std::is_final<T>::value>
struct ObjectHolder {
  T value_;

  template <typename... Args>
  ObjectHolder(Args&&... args) : value_{std::forward<Args>(args)...} {}

  T& operator*() { return value_; }
  T const& operator*() const { return value_; }
};

template <char Tag, typename T>
struct ObjectHolder<Tag, T, true> : T {
  template <typename... Args>
  ObjectHolder(Args&&... args) : T{std::forward<Args>(args)...} {}

  T& operator*() { return *this; }
  T const& operator*() const { return *this; }
};

// Policy provides the functionality of hasher, key_equal, and
// allocator_type.  In addition, it can add indirection to the values
// contained in the base table by defining a non-trivial value() method.
//
// To facilitate stateful implementations it is guaranteed that there
// will be a 1:1 relationship between BaseTable and Policy instance:
// policies will only be copied when their owning table is copied, and
// they will only be moved when their owning table is moved.
//
// Key equality will have the user-supplied search key as its first
// argument and the table contents as its second.  Heterogeneous lookup
// should be handled on the first argument.
//
// Item is the data stored inline in the hash table's chunks.  The policy
// controls how this is mapped to the corresponding Value.
//
// The policies defined in this file work for either set or map types.
// Most of the functionality is identical. A few methods detect the
// collection type by checking to see if MappedType is void, and then use
// SFINAE to select the appropriate implementation.
template <
    typename KeyType,
    typename MappedTypeOrVoid,
    typename HasherOrVoid,
    typename KeyEqualOrVoid,
    typename AllocOrVoid,
    typename ItemType>
struct FOLLY_MSVC_DECLSPEC(empty_bases) BasePolicy
    : private ObjectHolder<
          'H',
          Defaulted<HasherOrVoid, DefaultHasher<KeyType>>>,
      private ObjectHolder<
          'E',
          Defaulted<KeyEqualOrVoid, DefaultKeyEqual<KeyType>>>,
      private ObjectHolder<
          'A',
          Defaulted<
              AllocOrVoid,
              DefaultAlloc<SetOrMapValueType<KeyType, MappedTypeOrVoid>>>> {
  //////// user-supplied types

  using Key = KeyType;
  using Mapped = MappedTypeOrVoid;
  using Value = SetOrMapValueType<Key, Mapped>;
  using Item = ItemType;
  using Hasher = Defaulted<HasherOrVoid, DefaultHasher<Key>>;
  using KeyEqual = Defaulted<KeyEqualOrVoid, DefaultKeyEqual<Key>>;
  using Alloc = Defaulted<AllocOrVoid, DefaultAlloc<Value>>;
  using AllocTraits = std::allocator_traits<Alloc>;

  using ByteAlloc = typename AllocTraits::template rebind_alloc<uint8_t>;
  using ByteAllocTraits = typename std::allocator_traits<ByteAlloc>;
  using BytePtr = typename ByteAllocTraits::pointer;

  //////// info about user-supplied types

  static_assert(
      std::is_same<typename AllocTraits::value_type, Value>::value,
      "wrong allocator value_type");

 private:
  using HasherHolder = ObjectHolder<'H', Hasher>;
  using KeyEqualHolder = ObjectHolder<'E', KeyEqual>;
  using AllocHolder = ObjectHolder<'A', Alloc>;

  // emulate c++17's std::allocator_traits<A>::is_always_equal

  template <typename A, typename = void>
  struct AllocIsAlwaysEqual : std::is_empty<A> {};

  template <typename A>
  struct AllocIsAlwaysEqual<A, typename A::is_always_equal>
      : A::is_always_equal {};

 public:
  static constexpr bool kAllocIsAlwaysEqual = AllocIsAlwaysEqual<Alloc>::value;

  static constexpr bool kDefaultConstructIsNoexcept =
      std::is_nothrow_default_constructible<Hasher>::value &&
      std::is_nothrow_default_constructible<KeyEqual>::value &&
      std::is_nothrow_default_constructible<Alloc>::value;

  static constexpr bool kSwapIsNoexcept = kAllocIsAlwaysEqual &&
      std::is_nothrow_swappable_v<Hasher> &&
      std::is_nothrow_swappable_v<KeyEqual>;

  static constexpr bool isAvalanchingHasher() {
    return IsAvalanchingHasher<Hasher, Key>::value;
  }

  static constexpr bool shouldAssume32BitHash() {
    return ShouldAssume32BitHash<Hasher>::value;
  }

  //////// internal types and constants

  using InternalSizeType = std::size_t;

  // if false, F14Table will be smaller but F14Table::begin() won't work
  static constexpr bool kEnableItemIteration = true;

  using Chunk = F14Chunk<Item>;
  using ChunkPtr = typename std::pointer_traits<
      typename AllocTraits::pointer>::template rebind<Chunk>;
  using ItemIter = F14ItemIter<ChunkPtr>;

  static constexpr std::size_t kChunkAllocAlignment = alignof(Chunk);

  static constexpr bool kIsMap = !std::is_same<Key, Value>::value;
  static_assert(
      kIsMap == !std::is_void<MappedTypeOrVoid>::value,
      "Assumption for the kIsMap check violated.");

  using MappedOrBool = std::conditional_t<kIsMap, Mapped, bool>;

  // if true, bucket_count() after reserve(n) will be as close as possible
  // to n for multi-chunk tables
  static constexpr bool kContinuousCapacity = false;

  //////// methods

  BasePolicy(Hasher const& hasher, KeyEqual const& keyEqual, Alloc const& alloc)
      : HasherHolder{hasher}, KeyEqualHolder{keyEqual}, AllocHolder{alloc} {}

  BasePolicy(BasePolicy const& rhs)
      : HasherHolder{rhs.hasher()},
        KeyEqualHolder{rhs.keyEqual()},
        AllocHolder{
            AllocTraits::select_on_container_copy_construction(rhs.alloc())} {}

  BasePolicy(BasePolicy const& rhs, Alloc const& alloc)
      : HasherHolder{rhs.hasher()},
        KeyEqualHolder{rhs.keyEqual()},
        AllocHolder{alloc} {}

  BasePolicy(BasePolicy&& rhs) noexcept
      : HasherHolder{std::move(rhs.hasher())},
        KeyEqualHolder{std::move(rhs.keyEqual())},
        AllocHolder{std::move(rhs.alloc())} {}

  BasePolicy(BasePolicy&& rhs, Alloc const& alloc) noexcept
      : HasherHolder{std::move(rhs.hasher())},
        KeyEqualHolder{std::move(rhs.keyEqual())},
        AllocHolder{alloc} {}

 private:
  template <typename Src>
  void maybeAssignAlloc(std::true_type, Src&& src) {
    alloc() = std::forward<Src>(src);
  }

  template <typename Src>
  void maybeAssignAlloc(std::false_type, Src&&) {}

  template <typename A>
  void maybeSwapAlloc(std::true_type, A& rhs) {
    using std::swap;
    swap(alloc(), rhs);
  }

  template <typename A>
  void maybeSwapAlloc(std::false_type, A&) {}

 public:
  BasePolicy& operator=(BasePolicy const& rhs) {
    hasher() = rhs.hasher();
    keyEqual() = rhs.keyEqual();
    maybeAssignAlloc(
        typename AllocTraits::propagate_on_container_copy_assignment{},
        rhs.alloc());
    return *this;
  }

  BasePolicy& operator=(BasePolicy&& rhs) noexcept {
    hasher() = std::move(rhs.hasher());
    keyEqual() = std::move(rhs.keyEqual());
    maybeAssignAlloc(
        typename AllocTraits::propagate_on_container_move_assignment{},
        std::move(rhs.alloc()));
    return *this;
  }

  void swapBasePolicy(BasePolicy& rhs) {
    using std::swap;
    swap(hasher(), rhs.hasher());
    swap(keyEqual(), rhs.keyEqual());
    maybeSwapAlloc(
        typename AllocTraits::propagate_on_container_swap{}, rhs.alloc());
  }

  Hasher& hasher() { return *static_cast<HasherHolder&>(*this); }
  Hasher const& hasher() const {
    return *static_cast<HasherHolder const&>(*this);
  }
  KeyEqual& keyEqual() { return *static_cast<KeyEqualHolder&>(*this); }
  KeyEqual const& keyEqual() const {
    return *static_cast<KeyEqualHolder const&>(*this);
  }
  Alloc& alloc() { return *static_cast<AllocHolder&>(*this); }
  Alloc const& alloc() const { return *static_cast<AllocHolder const&>(*this); }

  template <typename K>
  std::size_t computeKeyHash(K const& key) const {
    static_assert(
        isAvalanchingHasher() == IsAvalanchingHasher<Hasher, K>::value);
    static_assert(
        !isAvalanchingHasher() ||
            sizeof(decltype(hasher()(key))) >= sizeof(std::size_t),
        "hasher is not avalanching if it doesn't return enough bits");
    return hasher()(key);
  }

  Key const& keyForValue(Key const& v) const { return v; }
  Key const& keyForValue(std::pair<Key const, MappedOrBool> const& p) const {
    return p.first;
  }
  Key const& keyForValue(std::pair<Key&&, MappedOrBool&&> const& p) const {
    return p.first;
  }

  // map's choice of pair<K const, T> as value_type is unfortunate,
  // because it means we either need a proxy iterator, a pointless key
  // copy when moving items during rehash, or some sort of UB hack.
  //
  // This code implements the hack.  Use moveValue(v) instead of
  // std::move(v) as the source of a move construction.  enable_if_t is
  // used so that this works for maps while being a no-op for sets.
  template <typename Dummy = int>
  static std::pair<Key&&, MappedOrBool&&> moveValue(
      std::pair<Key const, MappedOrBool>& value,
      std::enable_if_t<kIsMap, Dummy> = 0) {
    return {std::move(const_cast<Key&>(value.first)), std::move(value.second)};
  }

  template <typename Dummy = int>
  static Value&& moveValue(Value& value, std::enable_if_t<!kIsMap, Dummy> = 0) {
    return std::move(value);
  }

  template <typename P>
  bool beforeBuild(
      std::size_t /*size*/, std::size_t /*capacity*/, P&& /*rhs*/) {
    return false;
  }

  template <typename P>
  void afterBuild(
      bool /*undoState*/,
      bool /*success*/,
      std::size_t /*size*/,
      std::size_t /*capacity*/,
      P&& /*rhs*/) {}

  std::size_t alignedAllocSize(std::size_t n) const {
    if (kChunkAllocAlignment <= alignof(max_align_t) ||
        std::is_same<ByteAlloc, std::allocator<uint8_t>>::value) {
      return n;
    } else {
      return n + kChunkAllocAlignment;
    }
  }

  bool beforeRehash(
      std::size_t /*size*/,
      std::size_t /*oldCapacity*/,
      std::size_t /*newCapacity*/,
      std::size_t chunkAllocSize,
      BytePtr& outChunkAllocation) {
    outChunkAllocation = allocateOverAligned<ByteAlloc, kChunkAllocAlignment>(
        ByteAlloc{alloc()}, chunkAllocSize);
    return false;
  }

  void afterRehash(
      bool /*undoState*/,
      bool /*success*/,
      std::size_t /*size*/,
      std::size_t /*oldCapacity*/,
      std::size_t /*newCapacity*/,
      BytePtr chunkAllocation,
      std::size_t chunkAllocSize) {
    // on success, this will be the old allocation, on failure the new one
    if (chunkAllocation != nullptr) {
      deallocateOverAligned<ByteAlloc, kChunkAllocAlignment>(
          ByteAlloc{alloc()}, chunkAllocation, chunkAllocSize);
    }
  }

  void beforeClear(std::size_t /*size*/, std::size_t /*capacity*/) {}

  void afterClear(std::size_t /*size*/, std::size_t /*capacity*/) {}

  void beforeReset(std::size_t /*size*/, std::size_t /*capacity*/) {}

  void afterReset(
      std::size_t /*size*/,
      std::size_t /*capacity*/,
      BytePtr chunkAllocation,
      std::size_t chunkAllocSize) {
    deallocateOverAligned<ByteAlloc, kChunkAllocAlignment>(
        ByteAlloc{alloc()}, chunkAllocation, chunkAllocSize);
  }

  void prefetchValue(Item const&) const {
    // Subclass should disable with prefetchBeforeRehash(),
    // prefetchBeforeCopy(), and prefetchBeforeDestroy().  if they don't
    // override this method, because neither gcc nor clang can figure
    // out that DenseMaskIter with an empty body can be elided.
    FOLLY_SAFE_DCHECK(false, "should be disabled");
  }

  void afterDestroyWithoutDeallocate(Value* addr, std::size_t n) {
    if (kIsLibrarySanitizeAddress) {
      memset(static_cast<void*>(addr), 0x66, sizeof(Value) * n);
    }
  }
};

// BaseIter is a convenience for concrete set and map implementations
template <typename ValuePtr, typename Item>
class BaseIter {
 private:
  using pointee = typename std::pointer_traits<ValuePtr>::element_type;

 public:
  using iterator_category = std::forward_iterator_tag;
  using value_type = std::remove_const_t<pointee>;
  using difference_type = std::ptrdiff_t;
  using pointer = ValuePtr;
  using reference = pointee&;

 protected:
  using Chunk = F14Chunk<Item>;
  using ChunkPtr =
      typename std::pointer_traits<ValuePtr>::template rebind<Chunk>;
  using ItemIter = F14ItemIter<ChunkPtr>;

  using ValueConstPtr = typename std::pointer_traits<ValuePtr>::template rebind<
      std::add_const_t<typename std::pointer_traits<ValuePtr>::element_type>>;
};

//////// ValueContainer

template <
    typename Key,
    typename Mapped,
    typename HasherOrVoid,
    typename KeyEqualOrVoid,
    typename AllocOrVoid>
class ValueContainerPolicy;

template <typename ValuePtr>
using ValueContainerIteratorBase = BaseIter<
    ValuePtr,
    std::remove_const_t<typename std::pointer_traits<ValuePtr>::element_type>>;

template <typename ValuePtr>
class ValueContainerIterator : public ValueContainerIteratorBase<ValuePtr> {
  using Super = ValueContainerIteratorBase<ValuePtr>;
  using ItemIter = typename Super::ItemIter;
  using ValueConstPtr = typename Super::ValueConstPtr;

 public:
  using pointer = typename Super::pointer;
  using reference = typename Super::reference;
  using value_type = typename Super::value_type;

  ValueContainerIterator() = default;
  ValueContainerIterator(ValueContainerIterator const&) = default;
  ValueContainerIterator(ValueContainerIterator&&) = default;
  ValueContainerIterator& operator=(ValueContainerIterator const&) = default;
  ValueContainerIterator& operator=(ValueContainerIterator&&) = default;
  ~ValueContainerIterator() = default;

  /*implicit*/ operator ValueContainerIterator<ValueConstPtr>() const {
    return ValueContainerIterator<ValueConstPtr>{underlying_};
  }

  reference operator*() const { return underlying_.item(); }

  pointer operator->() const {
    return std::pointer_traits<pointer>::pointer_to(**this);
  }

  ValueContainerIterator& operator++() {
    underlying_.advance();
    return *this;
  }

  ValueContainerIterator operator++(int) {
    auto cur = *this;
    ++*this;
    return cur;
  }

  friend bool operator==(
      ValueContainerIterator const& lhs, ValueContainerIterator const& rhs) {
    return lhs.underlying_ == rhs.underlying_;
  }
  friend bool operator!=(
      ValueContainerIterator const& lhs, ValueContainerIterator const& rhs) {
    return !(lhs == rhs);
  }

 private:
  ItemIter underlying_;

  explicit ValueContainerIterator(ItemIter const& underlying)
      : underlying_{underlying} {}

  template <typename K, typename M, typename H, typename E, typename A>
  friend class ValueContainerPolicy;

  template <typename P>
  friend class ValueContainerIterator;
};

template <
    typename Key,
    typename MappedTypeOrVoid,
    typename HasherOrVoid,
    typename KeyEqualOrVoid,
    typename AllocOrVoid>
class ValueContainerPolicy
    : public BasePolicy<
          Key,
          MappedTypeOrVoid,
          HasherOrVoid,
          KeyEqualOrVoid,
          AllocOrVoid,
          SetOrMapValueType<Key, MappedTypeOrVoid>> {
 public:
  using Super = BasePolicy<
      Key,
      MappedTypeOrVoid,
      HasherOrVoid,
      KeyEqualOrVoid,
      AllocOrVoid,
      SetOrMapValueType<Key, MappedTypeOrVoid>>;
  using Alloc = typename Super::Alloc;
  using AllocTraits = typename Super::AllocTraits;
  using Item = typename Super::Item;
  using ItemIter = typename Super::ItemIter;
  using Value = typename Super::Value;
  using KeyEqual = typename Super::KeyEqual;
  using Hasher = typename Super::Hasher;
  using Mapped = typename Super::Mapped;

  using Super::kChunkAllocAlignment;
  static constexpr bool kDefaultConstructIsNoexcept =
      Super::kDefaultConstructIsNoexcept;
  static constexpr bool kAllocIsAlwaysEqual = Super::kAllocIsAlwaysEqual;
  static constexpr bool kEnableItemIteration = Super::kEnableItemIteration;
  static constexpr bool kContinuousCapacity = Super::kContinuousCapacity;
  static constexpr auto isAvalanchingHasher = Super::isAvalanchingHasher;
  static constexpr bool kSwapIsNoexcept = Super::kSwapIsNoexcept;

 private:
  using ByteAlloc = typename Super::ByteAlloc;

  using Super::kIsMap;

 public:
  using ConstIter = ValueContainerIterator<typename AllocTraits::const_pointer>;
  using Iter = std::conditional_t<
      kIsMap,
      ValueContainerIterator<typename AllocTraits::pointer>,
      ConstIter>;

  //////// F14Table policy

  static constexpr bool prefetchBeforeRehash() { return false; }

  static constexpr bool prefetchBeforeCopy() { return false; }

  static constexpr bool prefetchBeforeDestroy() { return false; }

  static constexpr bool destroyItemOnClear() {
    return !std::is_trivially_destructible<Item>::value ||
        !AllocatorHasDefaultObjectDestroy<Alloc, Item>::value;
  }

  // inherit constructors
  using Super::Super;

  void swapPolicy(ValueContainerPolicy& rhs) { this->swapBasePolicy(rhs); }

  using Super::keyForValue;
  static_assert(
      std::is_same<Item, Value>::value,
      "Item and Value should be the same type for ValueContainerPolicy.");

  std::size_t computeItemHash(Item const& item) const {
    return this->computeKeyHash(keyForValue(item));
  }

  template <typename K>
  bool keyMatchesItem(K const& key, Item const& item) const {
    return this->keyEqual()(key, keyForValue(item));
  }

  Value const& buildArgForItem(Item const& item) const& { return item; }

  // buildArgForItem(Item&)&& is used when moving between unequal allocators
  decltype(auto) buildArgForItem(Item& item) && {
    return Super::moveValue(item);
  }

  Value const& valueAtItem(Item const& item) const { return item; }

  Value&& valueAtItemForExtract(Item& item) { return std::move(item); }

  template <typename Table, typename... Args>
  void constructValueAtItem(Table&&, Item* itemAddr, Args&&... args) {
    Alloc& a = this->alloc();
    // GCC < 6 doesn't use the fact that itemAddr came from a reference
    // to avoid a null-check in the placement new.  folly::assume-ing it
    // here gets rid of that branch.  The branch is very predictable,
    // but spoils some further optimizations.  All clang versions that
    // compile folly seem to be okay.
    //
    // TODO(T31574848): clean up assume-s used to optimize placement new
    assume(itemAddr != nullptr);
    AllocTraits::construct(a, itemAddr, std::forward<Args>(args)...);
  }

  template <typename T>
  std::enable_if_t<IsNothrowMoveAndDestroy<T>::value>
  complainUnlessNothrowMoveAndDestroy() {}

  template <typename T>
  [[deprecated(
      "mark {key_type,mapped_type} {move constructor,destructor} noexcept, or use F14Node* if they aren't")]] std::
      enable_if_t<!IsNothrowMoveAndDestroy<T>::value>
      complainUnlessNothrowMoveAndDestroy() {}

  void moveItemDuringRehash(Item* itemAddr, Item& src) {
    complainUnlessNothrowMoveAndDestroy<Key>();
    complainUnlessNothrowMoveAndDestroy<lift_unit_t<MappedTypeOrVoid>>();

    constructValueAtItem(0, itemAddr, Super::moveValue(src));
    if (destroyItemOnClear()) {
      if (kIsMap) {
        // Laundering in the standard is only described as a solution
        // for changes to const fields due to the creation of a new
        // object lifetime (destroy and then placement new in the same
        // location), but it seems highly likely that it will also cause
        // the compiler to drop such assumptions that are violated due
        // to our UB const_cast in moveValue.
        destroyItem(*std::launder(std::addressof(src)));
      } else {
        destroyItem(src);
      }
    }
  }

  void destroyItem(Item& item) noexcept {
    Alloc& a = this->alloc();
    auto ptr = std::addressof(item);
    AllocTraits::destroy(a, ptr);
    this->afterDestroyWithoutDeallocate(ptr, 1);
  }

  template <typename V>
  void visitPolicyAllocationClasses(
      std::size_t chunkAllocSize,
      std::size_t /*size*/,
      std::size_t /*capacity*/,
      V&& visitor) const {
    if (chunkAllocSize > 0) {
      visitor(
          allocationBytesForOverAligned<ByteAlloc, kChunkAllocAlignment>(
              chunkAllocSize),
          1);
    }
  }

  //////// F14BasicMap/Set policy

  FOLLY_ALWAYS_INLINE Iter makeIter(ItemIter const& underlying) const {
    return Iter{underlying};
  }
  ConstIter makeConstIter(ItemIter const& underlying) const {
    return ConstIter{underlying};
  }
  ItemIter const& unwrapIter(ConstIter const& iter) const {
    return iter.underlying_;
  }
};

//////// NodeContainer

template <
    typename Key,
    typename Mapped,
    typename HasherOrVoid,
    typename KeyEqualOrVoid,
    typename AllocOrVoid>
class NodeContainerPolicy;

template <typename ValuePtr>
class NodeContainerIterator : public BaseIter<ValuePtr, NonConstPtr<ValuePtr>> {
  using Super = BaseIter<ValuePtr, NonConstPtr<ValuePtr>>;
  using ItemIter = typename Super::ItemIter;
  using ValueConstPtr = typename Super::ValueConstPtr;

 public:
  using pointer = typename Super::pointer;
  using reference = typename Super::reference;
  using value_type = typename Super::value_type;

  NodeContainerIterator() = default;
  NodeContainerIterator(NodeContainerIterator const&) = default;
  NodeContainerIterator(NodeContainerIterator&&) = default;
  NodeContainerIterator& operator=(NodeContainerIterator const&) = default;
  NodeContainerIterator& operator=(NodeContainerIterator&&) = default;
  ~NodeContainerIterator() = default;

  /*implicit*/ operator NodeContainerIterator<ValueConstPtr>() const {
    return NodeContainerIterator<ValueConstPtr>{underlying_};
  }

  reference operator*() const { return *underlying_.item(); }

  pointer operator->() const {
    return std::pointer_traits<pointer>::pointer_to(**this);
  }

  NodeContainerIterator& operator++() {
    underlying_.advance();
    return *this;
  }

  NodeContainerIterator operator++(int) {
    auto cur = *this;
    ++*this;
    return cur;
  }

  friend bool operator==(
      NodeContainerIterator const& lhs, NodeContainerIterator const& rhs) {
    return lhs.underlying_ == rhs.underlying_;
  }
  friend bool operator!=(
      NodeContainerIterator const& lhs, NodeContainerIterator const& rhs) {
    return !(lhs == rhs);
  }

 private:
  ItemIter underlying_;

  explicit NodeContainerIterator(ItemIter const& underlying)
      : underlying_{underlying} {}

  template <typename K, typename M, typename H, typename E, typename A>
  friend class NodeContainerPolicy;

  template <typename P>
  friend class NodeContainerIterator;
};

template <
    typename Key,
    typename MappedTypeOrVoid,
    typename HasherOrVoid,
    typename KeyEqualOrVoid,
    typename AllocOrVoid>
class NodeContainerPolicy
    : public BasePolicy<
          Key,
          MappedTypeOrVoid,
          HasherOrVoid,
          KeyEqualOrVoid,
          AllocOrVoid,
          typename std::allocator_traits<Defaulted<
              AllocOrVoid,
              DefaultAlloc<std::conditional_t<
                  std::is_void<MappedTypeOrVoid>::value,
                  Key,
                  MapValueType<Key, MappedTypeOrVoid>>>>>::pointer> {
 public:
  using Super = BasePolicy<
      Key,
      MappedTypeOrVoid,
      HasherOrVoid,
      KeyEqualOrVoid,
      AllocOrVoid,
      typename std::allocator_traits<Defaulted<
          AllocOrVoid,
          DefaultAlloc<std::conditional_t<
              std::is_void<MappedTypeOrVoid>::value,
              Key,
              MapValueType<Key, MappedTypeOrVoid>>>>>::pointer>;
  using Alloc = typename Super::Alloc;
  using AllocTraits = typename Super::AllocTraits;
  using Item = typename Super::Item;
  using ItemIter = typename Super::ItemIter;
  using Value = typename Super::Value;

 private:
  using ByteAlloc = typename Super::ByteAlloc;

  using Super::kChunkAllocAlignment;
  using Super::kIsMap;

 public:
  using ConstIter = NodeContainerIterator<typename AllocTraits::const_pointer>;
  using Iter = std::conditional_t<
      kIsMap,
      NodeContainerIterator<typename AllocTraits::pointer>,
      ConstIter>;

  //////// F14Table policy

  static constexpr bool prefetchBeforeRehash() { return true; }

  static constexpr bool prefetchBeforeCopy() { return true; }

  static constexpr bool prefetchBeforeDestroy() {
    return !std::is_trivially_destructible<Value>::value;
  }

  static constexpr bool destroyItemOnClear() { return true; }

  // inherit constructors
  using Super::Super;

  void swapPolicy(NodeContainerPolicy& rhs) { this->swapBasePolicy(rhs); }

  using Super::keyForValue;

  std::size_t computeItemHash(Item const& item) const {
    return this->computeKeyHash(keyForValue(*item));
  }

  template <typename K>
  bool keyMatchesItem(K const& key, Item const& item) const {
    return this->keyEqual()(key, keyForValue(*item));
  }

  Value const& buildArgForItem(Item const& item) const& { return *item; }

  // buildArgForItem(Item&)&& is used when moving between unequal allocators
  decltype(auto) buildArgForItem(Item& item) && {
    return Super::moveValue(*item);
  }

  Value const& valueAtItem(Item const& item) const { return *item; }

  Value&& valueAtItemForExtract(Item& item) { return std::move(*item); }

  template <typename Table, typename... Args>
  void constructValueAtItem(Table&&, Item* itemAddr, Args&&... args) {
    Alloc& a = this->alloc();
    // TODO(T31574848): clean up assume-s used to optimize placement new
    assume(itemAddr != nullptr);
    new (itemAddr) Item{AllocTraits::allocate(a, 1)};
    auto p = std::addressof(**itemAddr);
    // TODO(T31574848): clean up assume-s used to optimize placement new
    assume(p != nullptr);
    auto rollback = makeGuard([&] { AllocTraits::deallocate(a, p, 1); });
    AllocTraits::construct(a, p, std::forward<Args>(args)...);
    rollback.dismiss();
  }

  void moveItemDuringRehash(Item* itemAddr, Item& src) {
    // This is basically *itemAddr = src; src = nullptr, but allowing
    // for fancy pointers.
    // TODO(T31574848): clean up assume-s used to optimize placement new
    assume(itemAddr != nullptr);
    new (itemAddr) Item{std::move(src)};
    src = nullptr;
    src.~Item();
  }

  void prefetchValue(Item const& item) const {
    prefetchAddr(std::addressof(*item));
  }

  template <typename T>
  std::enable_if_t<std::is_nothrow_destructible<T>::value>
  complainUnlessNothrowDestroy() {}

  template <typename T>
  [[deprecated("Mark key and mapped type destructor nothrow")]] std::
      enable_if_t<!std::is_nothrow_destructible<T>::value>
      complainUnlessNothrowDestroy() {}

  void destroyItem(Item& item) noexcept {
    complainUnlessNothrowDestroy<Key>();
    complainUnlessNothrowDestroy<lift_unit_t<MappedTypeOrVoid>>();
    if (item != nullptr) {
      Alloc& a = this->alloc();
      AllocTraits::destroy(a, std::addressof(*item));
      AllocTraits::deallocate(a, item, 1);
    }
    item.~Item();
  }

  template <typename V>
  void visitPolicyAllocationClasses(
      std::size_t chunkAllocSize,
      std::size_t size,
      std::size_t /*capacity*/,
      V&& visitor) const {
    if (chunkAllocSize > 0) {
      visitor(
          allocationBytesForOverAligned<ByteAlloc, kChunkAllocAlignment>(
              chunkAllocSize),
          1);
    }
    if (size > 0) {
      visitor(sizeof(Value), size);
    }
  }

  //////// F14BasicMap/Set policy

  FOLLY_ALWAYS_INLINE Iter makeIter(ItemIter const& underlying) const {
    return Iter{underlying};
  }
  ConstIter makeConstIter(ItemIter const& underlying) const {
    return Iter{underlying};
  }
  ItemIter const& unwrapIter(ConstIter const& iter) const {
    return iter.underlying_;
  }
};

//////// VectorContainer

template <
    typename Key,
    typename MappedTypeOrVoid,
    typename HasherOrVoid,
    typename KeyEqualOrVoid,
    typename AllocOrVoid,
    typename EligibleForPerturbedInsertionOrder>
class VectorContainerPolicy;

template <typename ValuePtr>
class VectorContainerIterator : public BaseIter<ValuePtr, uint32_t> {
  using Super = BaseIter<ValuePtr, uint32_t>;
  using ValueConstPtr = typename Super::ValueConstPtr;

 public:
  using pointer = typename Super::pointer;
  using reference = typename Super::reference;
  using value_type = typename Super::value_type;

  VectorContainerIterator() = default;
  VectorContainerIterator(VectorContainerIterator const&) = default;
  VectorContainerIterator(VectorContainerIterator&&) = default;
  VectorContainerIterator& operator=(VectorContainerIterator const&) = default;
  VectorContainerIterator& operator=(VectorContainerIterator&&) = default;
  ~VectorContainerIterator() = default;

  /*implicit*/ operator VectorContainerIterator<ValueConstPtr>() const {
    return VectorContainerIterator<ValueConstPtr>{current_, lowest_};
  }

  reference operator*() const { return *current_; }

  pointer operator->() const { return current_; }

  VectorContainerIterator& operator++() {
    if (current_ == lowest_) [[unlikely]] {
      current_ = nullptr;
    } else {
      --current_;
    }
    return *this;
  }

  VectorContainerIterator operator++(int) {
    auto cur = *this;
    ++*this;
    return cur;
  }

  friend bool operator==(
      VectorContainerIterator const& lhs, VectorContainerIterator const& rhs) {
    return lhs.current_ == rhs.current_;
  }
  friend bool operator!=(
      VectorContainerIterator const& lhs, VectorContainerIterator const& rhs) {
    return !(lhs == rhs);
  }

 private:
  ValuePtr current_;
  ValuePtr lowest_;

  explicit VectorContainerIterator(ValuePtr current, ValuePtr lowest)
      : current_(current), lowest_(lowest) {}

  std::size_t index() const { return current_ - lowest_; }

  template <
      typename K,
      typename M,
      typename H,
      typename E,
      typename A,
      typename P>
  friend class VectorContainerPolicy;

  template <typename P>
  friend class VectorContainerIterator;
};

struct VectorContainerIndexSearch {
  uint32_t index_;
};

template <
    typename Key,
    typename MappedTypeOrVoid,
    typename HasherOrVoid,
    typename KeyEqualOrVoid,
    typename AllocOrVoid,
    typename EligibleForPerturbedInsertionOrder>
class VectorContainerPolicy
    : public BasePolicy<
          Key,
          MappedTypeOrVoid,
          HasherOrVoid,
          KeyEqualOrVoid,
          AllocOrVoid,
          uint32_t> {
 public:
  using Super = BasePolicy<
      Key,
      MappedTypeOrVoid,
      HasherOrVoid,
      KeyEqualOrVoid,
      AllocOrVoid,
      uint32_t>;
  using Value = typename Super::Value;
  using Alloc = typename Super::Alloc;
  using AllocTraits = typename Super::AllocTraits;
  using ByteAlloc = typename Super::ByteAlloc;
  using ByteAllocTraits = typename Super::ByteAllocTraits;
  using BytePtr = typename Super::BytePtr;
  using Hasher = typename Super::Hasher;
  using Item = typename Super::Item;
  using ItemIter = typename Super::ItemIter;
  using KeyEqual = typename Super::KeyEqual;

  using Super::kAllocIsAlwaysEqual;

 private:
  using Super::kIsMap;

  // Required alignment for SIMD tags and values in their shared allocation,
  // independent of optional cache-line padding.
  static constexpr std::size_t kAllocAlignment =
      constexpr_max(Super::kChunkAllocAlignment, alignof(Value));

  // Chunks hold indices but share an allocation with values. Value size
  // affects the size class and padding cost. The 16-byte cutoff is a
  // benchmark-based footprint tradeoff. Custom and over-aligned paths
  // stay unchanged.
  static constexpr bool kShouldCacheLineAlignChunks = sizeof(Value) >= 16 &&
      kAllocAlignment <= alignof(max_align_t) &&
      std::is_same_v<ByteAlloc, std::allocator<uint8_t>>;

  // Fixed cache-line target for vector padding; prefetch policy is separate.
  static constexpr std::size_t kChunkCacheLineSize = 64;

  // A prefix of at most 32 bytes fits in one line with 32-byte alignment.
  // Larger prefixes use cache-line alignment to avoid extra line fetches.
  static constexpr std::size_t cacheLineAlignmentForChunk(
      std::size_t chunkAllocSize) {
    return chunkAllocSize <= 32 ? 32 : kChunkCacheLineSize;
  }

  // Pad the whole chunk/value allocation, not just its chunk prefix, when
  // eligible. Other allocator paths retain allocationSize.
  static std::size_t chunkStorageSize(
      std::size_t chunkAllocSize, std::size_t allocationSize) {
    if constexpr (kShouldCacheLineAlignChunks) {
      if (usingJEMalloc()) {
        using lim = std::numeric_limits<std::size_t>;

        // Jemalloc aligns each small allocation to the least-significant set
        // bit of its size class. Rounding the request gets that alignment
        // without using its slower explicit-alignment path.
        const auto alignment = cacheLineAlignmentForChunk(chunkAllocSize);
        if (allocationSize > (lim::max)() - (alignment - 1)) {
          throw_exception<std::bad_alloc>();
        }
        return align_ceil(allocationSize, alignment);
      }
    }
    return allocationSize;
  }

  // Allocate shared chunk/value storage using the size-class policy above.
  BytePtr allocateChunkStorage(
      std::size_t chunkAllocSize, std::size_t allocationSize) {
    const auto size = chunkStorageSize(chunkAllocSize, allocationSize);
    return allocateOverAligned<ByteAlloc, kAllocAlignment>(
        ByteAlloc{Super::alloc()}, size);
  }

  // Recompute the same padded size used for allocation from the original sizes.
  void deallocateChunkStorage(
      BytePtr allocation,
      std::size_t chunkAllocSize,
      std::size_t allocationSize) {
    const auto size = chunkStorageSize(chunkAllocSize, allocationSize);
    deallocateOverAligned<ByteAlloc, kAllocAlignment>(
        ByteAlloc{Super::alloc()}, allocation, size);
  }

  // Account for size-class padding as well as allocator alignment overhead.
  static std::size_t chunkStorageAllocationBytes(
      std::size_t chunkAllocSize, std::size_t allocationSize) {
    return allocationBytesForOverAligned<ByteAlloc, kAllocAlignment>(
        chunkStorageSize(chunkAllocSize, allocationSize));
  }

 public:
  static constexpr bool kEnableItemIteration = false;

  static constexpr bool kContinuousCapacity = true;

  using InternalSizeType = Item;

  using ConstIter =
      VectorContainerIterator<typename AllocTraits::const_pointer>;
  using Iter = std::conditional_t<
      kIsMap,
      VectorContainerIterator<typename AllocTraits::pointer>,
      ConstIter>;
  using ConstReverseIter = typename AllocTraits::const_pointer;
  using ReverseIter = std::
      conditional_t<kIsMap, typename AllocTraits::pointer, ConstReverseIter>;

  using ValuePtr = typename AllocTraits::pointer;

  //////// F14Table policy

  static constexpr bool prefetchBeforeRehash() { return true; }

  static constexpr bool prefetchBeforeCopy() { return false; }

  static constexpr bool prefetchBeforeDestroy() { return false; }

  static constexpr bool destroyItemOnClear() { return false; }

 private:
  static constexpr bool valueIsTriviallyCopyable() {
    return AllocatorHasDefaultObjectConstruct<Alloc, Value, Value>::value &&
        AllocatorHasDefaultObjectDestroy<Alloc, Value>::value &&
        std::is_trivially_copyable<Value>::value;
  }

 public:
  VectorContainerPolicy(
      Hasher const& hasher, KeyEqual const& keyEqual, Alloc const& alloc)
      : Super{hasher, keyEqual, alloc} {}

  VectorContainerPolicy(VectorContainerPolicy const& rhs) : Super{rhs} {
    // values_ will get allocated later to do the copy
  }

  VectorContainerPolicy(VectorContainerPolicy const& rhs, Alloc const& alloc)
      : Super{rhs, alloc} {
    // values_ will get allocated later to do the copy
  }

  VectorContainerPolicy(VectorContainerPolicy&& rhs) noexcept
      : Super{std::move(rhs)}, values_{rhs.values_} {
    rhs.values_ = nullptr;
  }

  VectorContainerPolicy(
      VectorContainerPolicy&& rhs, Alloc const& alloc) noexcept
      : Super{std::move(rhs), alloc} {
    if (kAllocIsAlwaysEqual || this->alloc() == rhs.alloc()) {
      // common case
      values_ = rhs.values_;
      rhs.values_ = nullptr;
    } else {
      // table must be constructed in new memory
      values_ = nullptr;
    }
  }

  VectorContainerPolicy& operator=(VectorContainerPolicy const& rhs) {
    if (this != &rhs) {
      FOLLY_SAFE_DCHECK(values_ == nullptr, "");
      Super::operator=(rhs);
    }
    return *this;
  }

  VectorContainerPolicy& operator=(VectorContainerPolicy&& rhs) noexcept {
    if (this != &rhs) {
      FOLLY_SAFE_DCHECK(values_ == nullptr, "");
      bool transfer =
          AllocTraits::propagate_on_container_move_assignment::value ||
          kAllocIsAlwaysEqual || this->alloc() == rhs.alloc();
      Super::operator=(std::move(rhs));
      if (transfer) {
        values_ = rhs.values_;
        rhs.values_ = nullptr;
      }
    }
    return *this;
  }

  void swapPolicy(VectorContainerPolicy& rhs) {
    using std::swap;
    this->swapBasePolicy(rhs);
    swap(values_, rhs.values_);
  }

  template <typename K>
  std::size_t computeKeyHash(K const& key) const {
    static_assert(
        Super::isAvalanchingHasher() == IsAvalanchingHasher<Hasher, K>::value);
    return this->hasher()(key);
  }

  std::size_t computeKeyHash(VectorContainerIndexSearch const& key) const {
    return computeItemHash(key.index_);
  }

  using Super::keyForValue;

  std::size_t computeItemHash(Item const& item) const {
    return this->computeKeyHash(keyForValue(values_[item]));
  }

  bool keyMatchesItem(
      VectorContainerIndexSearch const& key, Item const& item) const {
    return key.index_ == item;
  }

  template <typename K>
  bool keyMatchesItem(K const& key, Item const& item) const {
    return this->keyEqual()(key, keyForValue(values_[item]));
  }

  Key const& keyForValue(VectorContainerIndexSearch const& arg) const {
    return keyForValue(values_[arg.index_]);
  }

  VectorContainerIndexSearch buildArgForItem(Item const& item) const {
    return {item};
  }

  Value const& valueAtItem(Item const& item) const { return values_[item]; }

  Value&& valueAtItemForExtract(Item& item) { return std::move(values_[item]); }

  template <typename Table>
  void constructValueAtItem(
      Table&&, Item* itemAddr, VectorContainerIndexSearch arg) {
    *itemAddr = arg.index_;
  }

  template <typename Table, typename... Args>
  void constructValueAtItem(Table&& table, Item* itemAddr, Args&&... args) {
    Alloc& a = this->alloc();
    auto size = static_cast<InternalSizeType>(table.size());
    FOLLY_SAFE_DCHECK(
        table.size() < std::numeric_limits<InternalSizeType>::max(), "");
    *itemAddr = size;
    auto dst = std::addressof(values_[size]);
    // TODO(T31574848): clean up assume-s used to optimize placement new
    assume(dst != nullptr);
    AllocTraits::construct(a, dst, std::forward<Args>(args)...);

    constexpr bool perturb = FOLLY_F14_PERTURB_INSERTION_ORDER;
    if (EligibleForPerturbedInsertionOrder::value && perturb &&
        !tlsPendingSafeInserts()) {
      // Pick a random victim. We have to do this post-construction
      // because the item and tag are already set in the table before
      // calling constructValueAtItem, so if there is a tag collision
      // find may evaluate values_[size] during the search.
      auto i = static_cast<InternalSizeType>(tlsMinstdRand(size + 1));
      if (i != size) {
        auto& lhsItem = *itemAddr;
        auto rhsIter = table.find(
            VectorContainerIndexSearch{static_cast<InternalSizeType>(i)});
        FOLLY_SAFE_DCHECK(!rhsIter.atEnd(), "");
        auto& rhsItem = rhsIter.item();
        FOLLY_SAFE_DCHECK(lhsItem == size, "");
        FOLLY_SAFE_DCHECK(rhsItem == i, "");

        aligned_storage_for_t<Value> tmp;
        Value* tmpValue = static_cast<Value*>(static_cast<void*>(&tmp));
        transfer(a, std::addressof(values_[i]), tmpValue, 1);
        transfer(
            a, std::addressof(values_[size]), std::addressof(values_[i]), 1);
        transfer(a, tmpValue, std::addressof(values_[size]), 1);
        lhsItem = i;
        rhsItem = size;
      }
    }
  }

  void moveItemDuringRehash(Item* itemAddr, Item& src) { *itemAddr = src; }

  void prefetchValue(Item const& item) const {
    prefetchAddr(std::addressof(values_[item]));
  }

  void destroyItem(Item&) noexcept {}

  template <typename T>
  std::enable_if_t<IsNothrowMoveAndDestroy<T>::value>
  complainUnlessNothrowMoveAndDestroy() {}

  template <typename T>
  [[deprecated(
      "mark {key_type,mapped_type} {move constructor,destructor} noexcept, or use F14Node* if they aren't")]] std::
      enable_if_t<!IsNothrowMoveAndDestroy<T>::value>
      complainUnlessNothrowMoveAndDestroy() {}

  void transfer(Alloc& a, Value* src, Value* dst, std::size_t n) {
    complainUnlessNothrowMoveAndDestroy<Key>();
    complainUnlessNothrowMoveAndDestroy<lift_unit_t<MappedTypeOrVoid>>();

    auto origSrc = src;
    if (valueIsTriviallyCopyable()) {
      std::memcpy(
          static_cast<void*>(dst),
          static_cast<void const*>(src),
          n * sizeof(Value));
    } else {
      for (std::size_t i = 0; i < n; ++i, ++src, ++dst) {
        // TODO(T31574848): clean up assume-s used to optimize placement new
        assume(dst != nullptr);
        AllocTraits::construct(a, dst, Super::moveValue(*src));
        if (kIsMap) {
          AllocTraits::destroy(a, std::launder(src));
        } else {
          AllocTraits::destroy(a, src);
        }
      }
    }
    this->afterDestroyWithoutDeallocate(origSrc, n);
  }

  template <typename P, typename V>
  bool beforeBuildImpl(std::size_t size, P&& rhs, V const& constructorArgFor) {
    Alloc& a = this->alloc();

    FOLLY_SAFE_DCHECK(values_ != nullptr, "");

    auto src = std::addressof(rhs.values_[0]);
    Value* dst = std::addressof(values_[0]);

    if (valueIsTriviallyCopyable()) {
      std::memcpy(
          static_cast<void*>(dst),
          static_cast<void const*>(src),
          size * sizeof(Value));
    } else {
      for (std::size_t i = 0; i < size; ++i, ++src, ++dst) {
        catch_exception(
            [&] {
              // TODO(T31574848): clean up assume-s used to optimize placement
              // new
              assume(dst != nullptr);
              AllocTraits::construct(a, dst, constructorArgFor(*src));
            },
            [&] {
              for (Value* cleanup = std::addressof(values_[0]); cleanup != dst;
                   ++cleanup) {
                AllocTraits::destroy(a, cleanup);
              }
              rethrow_current_exception();
            });
      }
    }
    return true;
  }

  bool beforeBuild(
      std::size_t size,
      std::size_t /*capacity*/,
      VectorContainerPolicy const& rhs) {
    return beforeBuildImpl(size, rhs, std::identity{});
  }

  bool beforeBuild(
      std::size_t size, std::size_t /*capacity*/, VectorContainerPolicy&& rhs) {
    return beforeBuildImpl(size, rhs, [](Value& v) {
      return Super::moveValue(v);
    });
  }

  template <typename P>
  void afterBuild(
      bool /*undoState*/,
      bool success,
      std::size_t /*size*/,
      std::size_t /*capacity*/,
      P&& /*rhs*/) {
    // buildArgForItem can be used to construct a new item trivially,
    // so no failure between beforeBuild and afterBuild should be possible
    FOLLY_SAFE_DCHECK(success, "");
  }

 private:
  // Returns the byte offset of the first Value in a unified allocation
  // that first holds prefixBytes of data, where prefixBytes comes from
  // Chunk storage and may be only 4-byte aligned due to sub-chunk
  // allocation.
  static std::size_t valuesOffset(std::size_t prefixBytes) {
    FOLLY_SAFE_DCHECK((prefixBytes % alignof(Item)) == 0, "");
    if (alignof(Value) > alignof(Item)) {
      prefixBytes = -(-prefixBytes & ~(alignof(Value) - 1));
    }
    FOLLY_SAFE_DCHECK((prefixBytes % alignof(Value)) == 0, "");
    return prefixBytes;
  }

  // Returns the total number of bytes that should be allocated to store
  // prefixBytes of Chunks and valueCapacity values.
  static std::size_t allocSize(
      std::size_t prefixBytes, std::size_t valueCapacity) {
    return valuesOffset(prefixBytes) + sizeof(Value) * valueCapacity;
  }

 public:
  ValuePtr beforeRehash(
      std::size_t size,
      std::size_t oldCapacity,
      std::size_t newCapacity,
      std::size_t chunkAllocSize,
      BytePtr& outChunkAllocation) {
    FOLLY_SAFE_DCHECK(
        size <= oldCapacity && ((values_ == nullptr) == (oldCapacity == 0)) &&
            newCapacity > 0 &&
            newCapacity <= (std::numeric_limits<Item>::max)(),
        "");

    outChunkAllocation = allocateChunkStorage(
        chunkAllocSize, allocSize(chunkAllocSize, newCapacity));

    ValuePtr before = values_;
    ValuePtr after = std::pointer_traits<ValuePtr>::pointer_to(
        *static_cast<Value*>(static_cast<void*>(
            &*outChunkAllocation + valuesOffset(chunkAllocSize))));

    if (size > 0) {
      Alloc& a = this->alloc();
      transfer(a, std::addressof(before[0]), std::addressof(after[0]), size);
    }

    values_ = after;
    return before;
  }

  FOLLY_NOINLINE void afterFailedRehash(ValuePtr state, std::size_t size) {
    // state holds the old storage
    Alloc& a = this->alloc();
    if (size > 0) {
      transfer(a, std::addressof(values_[0]), std::addressof(state[0]), size);
    }
    values_ = state;
  }

  void afterRehash(
      ValuePtr state,
      bool success,
      std::size_t size,
      std::size_t oldCapacity,
      std::size_t newCapacity,
      BytePtr chunkAllocation,
      std::size_t chunkAllocSize) {
    if (!success) {
      afterFailedRehash(state, size);
    }

    // on success, chunkAllocation is the old allocation, on failure it is the
    // new one
    if (chunkAllocation != nullptr) {
      deallocateChunkStorage(
          chunkAllocation,
          chunkAllocSize,
          allocSize(chunkAllocSize, (success ? oldCapacity : newCapacity)));
    }
  }

  void beforeClear(std::size_t size, std::size_t capacity) {
    FOLLY_SAFE_DCHECK(
        size <= capacity && ((values_ == nullptr) == (capacity == 0)), "");
    Alloc& a = this->alloc();
    for (std::size_t i = 0; i < size; ++i) {
      AllocTraits::destroy(a, std::addressof(values_[i]));
    }
  }

  void beforeReset(std::size_t size, std::size_t capacity) {
    beforeClear(size, capacity);
  }

  void afterReset(
      std::size_t /*size*/,
      std::size_t capacity,
      BytePtr chunkAllocation,
      std::size_t chunkAllocSize) {
    if (chunkAllocation != nullptr) {
      deallocateChunkStorage(
          chunkAllocation, chunkAllocSize, allocSize(chunkAllocSize, capacity));
      values_ = nullptr;
    }
  }

  template <typename V>
  void visitPolicyAllocationClasses(
      std::size_t chunkAllocSize,
      std::size_t /*size*/,
      std::size_t capacity,
      V&& visitor) const {
    FOLLY_SAFE_DCHECK((chunkAllocSize == 0) == (capacity == 0), "");
    if (chunkAllocSize > 0) {
      visitor(
          chunkStorageAllocationBytes(
              chunkAllocSize, allocSize(chunkAllocSize, capacity)),
          1);
    }
  }

  // Iterator stuff

  Iter linearBegin(std::size_t size) const {
    return size > 0
        ? Iter{values_ + size - 1, values_}
        : Iter{nullptr, nullptr};
  }

  Iter linearEnd() const { return Iter{nullptr, nullptr}; }

  //////// F14BasicMap/Set policy

  Iter makeIter(ItemIter const& underlying) const {
    if (underlying.atEnd()) {
      return linearEnd();
    } else {
      assume(values_ + underlying.item() != nullptr);
      assume(values_ != nullptr);
      return Iter{values_ + underlying.item(), values_};
    }
  }

  ConstIter makeConstIter(ItemIter const& underlying) const {
    return makeIter(underlying);
  }

  Item iterToIndex(ConstIter const& iter) const {
    auto n = iter.index();
    assume(n <= std::numeric_limits<Item>::max());
    return static_cast<Item>(n);
  }

  Iter indexToIter(Item index) const { return Iter{values_ + index, values_}; }

  ValuePtr values_{nullptr};
};

template <
    template <
        typename,
        typename,
        typename,
        typename,
        typename,
        typename...> class Policy,
    typename Key,
    typename Mapped,
    typename Hasher,
    typename KeyEqual,
    typename Alloc,
    typename... Args>
using MapPolicyWithDefaults = Policy<
    Key,
    Mapped,
    VoidDefault<Hasher, DefaultHasher<Key>>,
    VoidDefault<KeyEqual, DefaultKeyEqual<Key>>,
    VoidDefault<Alloc, DefaultAlloc<std::pair<Key const, Mapped>>>,
    Args...>;

template <
    template <
        typename,
        typename,
        typename,
        typename,
        typename,
        typename...> class Policy,
    typename Key,
    typename Hasher,
    typename KeyEqual,
    typename Alloc,
    typename... Args>
using SetPolicyWithDefaults = Policy<
    Key,
    void,
    VoidDefault<Hasher, DefaultHasher<Key>>,
    VoidDefault<KeyEqual, DefaultKeyEqual<Key>>,
    VoidDefault<Alloc, DefaultAlloc<Key>>,
    Args...>;

} // namespace detail
} // namespace f14
} // namespace folly

#endif // FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
