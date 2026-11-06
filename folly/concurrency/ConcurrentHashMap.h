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
#include <mutex>

#include <folly/Optional.h>
#include <folly/concurrency/detail/ConcurrentHashMap-detail.h>
#include <folly/synchronization/Hazptr.h>

namespace folly {

/**
 * Implementations of high-performance Concurrent Hashmaps that
 * support erase and update.
 *
 * Readers are always wait-free.
 * Writers are sharded, but take a lock that only locks part of the map.
 *
 * Multithreaded performance beats anything except the lock-free
 *      atomic maps (AtomicUnorderedMap, AtomicHashMap), BUT only
 *      if you can perfectly size the atomic maps, and you don't
 *      need erase().  If you don't know the size in advance or
 *      your workload needs erase(), this is the better choice.
 *
 * The interface is as close to std::unordered_map as possible, but there
 * are a handful of changes:
 *
 * * Iterators hold hazard pointers to the returned elements.  Elements can only
 *   be accessed while Iterators are still valid!
 *
 * * Therefore operator[] and at() return copies, since they do not
 *   return an iterator.  The returned value is const, to remind you
 *   that changes do not affect the value in the map.
 *
 * * erase() calls the hash function, and may fail if the hash
 *   function throws an exception.
 *
 * * clear() initializes new segments, and is not noexcept.
 *
 * * The interface adds assign_if_equal, since find() doesn't take a lock.
 *
 * * Only const version of find() is supported, and const iterators.
 *   Mutation must use functions provided, like assign().
 *
 * * iteration iterates over all the buckets in the table, unlike
 *   std::unordered_map which iterates over a linked list of elements.
 *   If the table is sparse, this may be more expensive.
 *
 * * Allocator must be stateless.
 *
 * 1: ConcurrentHashMap, based on Java's ConcurrentHashMap.
 *    Very similar to std::unordered_map in performance.
 *
 * 2: ConcurrentHashMapSIMD, based on F14ValueMap.  If the map is
 *    larger than the cache size, it has superior performance due to
 *    vectorized key lookup.
 *
 *
 *
 * USAGE FAQs
 *
 * Q: Is simultaneous iteration and erase() threadsafe?
 *       Example:
 *
 *       ConcurrentHashMap<int, int> map;
 *
 *       Thread 1: auto it = map.begin();
 *                   while (it != map.end()) {
 *                      // Do something with it
 *                      it++;
 *                   }
 *
 *       Thread 2:    map.insert(2, 2);  map.erase(2);
 *
 * A: Yes, this is safe.  However, the iterating thread is not
 * guaranteed to see (or not see) concurrent insertions and erasures.
 * Inserts may cause a rehash, but the old table is still valid as
 * long as any iterator pointing to it exists.
 *
 * Q: How do I update an existing object atomically?
 *
 * A: assign_if_equal is the recommended way - readers will see the
 * old value until the new value is completely constructed and
 * inserted.
 *
 * Q: Why do map.erase() and clear() not actually destroy elements?
 *
 * A: Hazard Pointers are used to improve the performance of
 * concurrent access.  They can be thought of as a simple Garbage
 * Collector.  To reduce the GC overhead, a GC pass is only run after
 * reaching a certain memory bound.  erase() will remove the element
 * from being accessed via the map, but actual destruction may happen
 * later, after iterators that may point to it have been deleted.
 *
 * The only guarantee is that a GC pass will be run on map destruction
 * - no elements will remain after map destruction.
 *
 * Q: Are pointers to values safe to access *without* holding an
 * iterator?
 *
 * A: The SIMD version guarantees that references to elements are
 * stable across rehashes, the non-SIMD version does *not*.  Note that
 * unless you hold an iterator, you need to ensure there are no
 * concurrent deletes/updates to that key if you are accessing it via
 * reference.
 */

template <
    typename KeyType,
    typename ValueType,
    typename HashFn = std::hash<KeyType>,
    typename KeyEqual = std::equal_to<KeyType>,
    typename Allocator = std::allocator<uint8_t>,
    uint8_t ShardBits = 8,
    template <typename> class Atom = std::atomic,
    class Mutex = std::mutex,
    template <
        typename,
        typename,
        uint8_t,
        typename,
        typename,
        typename,
        template <typename> class,
        class> class Impl = detail::concurrenthashmap::bucket::BucketTable>
class ConcurrentHashMap {
  using SegmentT = detail::ConcurrentHashMapSegment<
      KeyType,
      ValueType,
      ShardBits,
      HashFn,
      KeyEqual,
      Allocator,
      Atom,
      Mutex,
      Impl>;
  using SegmentTAllocator = typename std::allocator_traits<
      Allocator>::template rebind_alloc<SegmentT>;
  template <typename K>
  static constexpr bool kEligibleForHeterogeneousFind =
      detail::EligibleForHeterogeneousFind<KeyType, HashFn, KeyEqual, K>::value;

  float load_factor_ = SegmentT::kDefaultLoadFactor;

  static constexpr uint64_t NumShards = (1 << ShardBits);

 public:
  class ConstIterator;

  using key_type = KeyType;
  using mapped_type = ValueType;
  using value_type = std::pair<const KeyType, ValueType>;
  using size_type = std::size_t;
  using hasher = HashFn;
  using key_equal = KeyEqual;
  using const_iterator = ConstIterator;

 private:
  template <typename K>
  static constexpr bool kEligibleForHeterogeneousInsert = ::folly::detail::
      EligibleForHeterogeneousInsert<KeyType, HashFn, KeyEqual, K>::value;

  template <typename K>
  using IsIter = std::is_same<ConstIterator, std::remove_cvref_t<K>>;

  template <typename K>
  static constexpr bool kEligibleForHeterogeneousErase =
      ::folly::detail::EligibleForHeterogeneousFind<
          KeyType,
          HashFn,
          KeyEqual,
          std::conditional_t<IsIter<K>::value, KeyType, K>>::value &&
      !IsIter<K>::value;

 public:
  /*
   * Construct a ConcurrentHashMap with 1 << ShardBits shards, size
   * and max_size given.  Both size and max_size will be rounded up to
   * the next power of two, if they are not already a power of two, so
   * that we can index in to Shards efficiently. If the rounded max_size
   * would end up smaller than the rounded size, max_size is raised to
   * match it, since max_size can never be smaller than the initial size.
   *
   * Insertion functions will throw bad_alloc if max_size is exceeded.
   */
  explicit ConcurrentHashMap(size_t size = 8, size_t max_size = 0) {
    size_ = folly::nextPowTwo(size);
    if (max_size != 0) {
      max_size_ = folly::nextPowTwo(max_size);
      if (max_size_ < size_) {
        max_size_ = size_;
      }
    }
    for (uint64_t i = 0; i < NumShards; i++) {
      segments_[i].store(nullptr, std::memory_order_relaxed);
    }
  }

  ConcurrentHashMap(ConcurrentHashMap&& o) noexcept
      : size_(o.size_), max_size_(o.max_size_) {
    for (uint64_t i = 0; i < NumShards; i++) {
      segments_[i].store(
          o.segments_[i].load(std::memory_order_relaxed),
          std::memory_order_relaxed);
      o.segments_[i].store(nullptr, std::memory_order_relaxed);
    }
    cohort_.store(o.cohort(), std::memory_order_relaxed);
    o.cohort_.store(nullptr, std::memory_order_relaxed);
    beginSeg_.store(
        o.beginSeg_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    o.beginSeg_.store(NumShards, std::memory_order_relaxed);
    endSeg_.store(
        o.endSeg_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    o.endSeg_.store(0, std::memory_order_relaxed);
  }

  ConcurrentHashMap& operator=(ConcurrentHashMap&& o) {
    for (uint64_t i = 0; i < NumShards; i++) {
      auto seg = segments_[i].load(std::memory_order_relaxed);
      if (seg) {
        seg->~SegmentT();
        SegmentTAllocator().deallocate(seg, 1);
      }
      segments_[i].store(
          o.segments_[i].load(std::memory_order_relaxed),
          std::memory_order_relaxed);
      o.segments_[i].store(nullptr, std::memory_order_relaxed);
    }
    size_ = o.size_;
    max_size_ = o.max_size_;
    cohort_shutdown_cleanup();
    cohort_.store(o.cohort(), std::memory_order_relaxed);
    o.cohort_.store(nullptr, std::memory_order_relaxed);
    beginSeg_.store(
        o.beginSeg_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    o.beginSeg_.store(NumShards, std::memory_order_relaxed);
    endSeg_.store(
        o.endSeg_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    o.endSeg_.store(0, std::memory_order_relaxed);
    return *this;
  }

  ~ConcurrentHashMap() {
    uint64_t begin = beginSeg_.load(std::memory_order_acquire);
    uint64_t end = endSeg_.load(std::memory_order_acquire);
    for (uint64_t i = begin; i < end; ++i) {
      auto seg = segments_[i].load(std::memory_order_relaxed);
      if (seg) {
        seg->~SegmentT();
        SegmentTAllocator().deallocate(seg, 1);
      }
    }
    cohort_shutdown_cleanup();
  }

  bool empty() const noexcept {
    uint64_t begin = beginSeg_.load(std::memory_order_acquire);
    uint64_t end = endSeg_.load(std::memory_order_acquire);
    // Note: beginSeg_ and endSeg_ are only conservative hints of the
    // range of non-empty segments. This function cannot conclude that
    // a map is nonempty merely because beginSeg_ < endSeg_.
    for (uint64_t i = begin; i < end; ++i) {
      auto seg = segments_[i].load(std::memory_order_acquire);
      if (seg) {
        if (!seg->empty()) {
          return false;
        }
      }
    }
    return true;
  }

  ConstIterator find(const KeyType& k) const { return findImpl(k); }

  template <typename K, int = 0>
    requires kEligibleForHeterogeneousFind<K>
  ConstIterator find(const K& k) const {
    return findImpl(k);
  }

  /*
   * Intentionally marked as deleted to guard against common misuse.
   *
   * Checking for key existence via contains() is unsafe in concurrent
   * contexts. The key may be erased between contains() and subsequent
   * operations. Use find() and its iterator to ensure safe access and
   * prevent race conditions.
   */
  bool contains(const KeyType& k) const = delete;

  ConstIterator cend() const noexcept { return ConstIterator(NumShards); }

  ConstIterator cbegin() const noexcept { return ConstIterator(this); }

  ConstIterator end() const noexcept { return cend(); }

  ConstIterator begin() const noexcept { return cbegin(); }

  std::pair<ConstIterator, bool> insert(
      std::pair<key_type, mapped_type>&& foo) {
    return insertImpl(std::move(foo));
  }

  template <typename Key, int = 0>
    requires kEligibleForHeterogeneousInsert<Key>
  std::pair<ConstIterator, bool> insert(std::pair<Key, mapped_type>&& foo) {
    return insertImpl(std::move(foo));
  }

  template <typename Key, typename Value>
  std::pair<ConstIterator, bool> insert(Key&& k, Value&& v) {
    auto h = HashFn{}(k);
    auto segment = pickSegment(h);
    std::pair<ConstIterator, bool> res(
        std::piecewise_construct,
        std::forward_as_tuple(this, segment),
        std::forward_as_tuple(false));
    res.second = ensureSegment(segment)->insert(
        res.first.it_, h, std::forward<Key>(k), std::forward<Value>(v));
    return res;
  }

  template <typename Key, typename... Args>
  std::pair<ConstIterator, bool> try_emplace(Key&& k, Args&&... args) {
    auto h = HashFn{}(k);
    auto segment = pickSegment(h);
    std::pair<ConstIterator, bool> res(
        std::piecewise_construct,
        std::forward_as_tuple(this, segment),
        std::forward_as_tuple(false));
    res.second = ensureSegment(segment)->try_emplace(
        res.first.it_, h, std::forward<Key>(k), std::forward<Args>(args)...);
    return res;
  }

  template <typename... Args>
  std::pair<ConstIterator, bool> emplace(Args&&... args) {
    using Node = typename SegmentT::Node;
    detail::concurrenthashmap::AllocNodeGuard<Node, Allocator> g(
        Allocator(), ensureCohort(), std::forward<Args>(args)...);
    auto h = HashFn{}(g.node->getItem().first);
    auto segment = pickSegment(h);
    std::pair<ConstIterator, bool> res(
        std::piecewise_construct,
        std::forward_as_tuple(this, segment),
        std::forward_as_tuple(false));
    res.second = ensureSegment(segment)->emplace(
        res.first.it_, h, g.node->getItem().first, g.node);
    if (res.second) {
      g.dismiss();
    }
    return res;
  }

  /*
   * The bool component will always be true if the map has been updated via
   * either insertion or assignment. Note that this is different from the
   * std::map::insert_or_assign interface.
   */
  template <typename Key, typename Value>
  std::pair<ConstIterator, bool> insert_or_assign(Key&& k, Value&& v) {
    auto h = HashFn{}(k);
    auto segment = pickSegment(h);
    std::pair<ConstIterator, bool> res(
        std::piecewise_construct,
        std::forward_as_tuple(this, segment),
        std::forward_as_tuple(false));
    res.second = ensureSegment(segment)->insert_or_assign(
        res.first.it_, h, std::forward<Key>(k), std::forward<Value>(v));
    return res;
  }

  /*
   * Insert desired if the key doesn't exist, or assign to desired if the
   * predicate returns true for the current value. The bool component will
   * always be true if the map has been updated via either insertion or
   * assignment. Note that this is different from the std::map::insert_or_assign
   * interface.
   */
  template <typename Key, typename Value, typename Predicate>
  std::pair<ConstIterator, bool> insert_or_assign_if(
      Key&& k, Value&& desired, Predicate&& predicate) {
    auto h = HashFn{}(k);
    auto segment = pickSegment(h);
    std::pair<ConstIterator, bool> res(
        std::piecewise_construct,
        std::forward_as_tuple(this, segment),
        std::forward_as_tuple(false));
    res.second = ensureSegment(segment)->insert_or_assign_if(
        res.first.it_,
        h,
        std::forward<Key>(k),
        std::forward<Value>(desired),
        std::forward<Predicate>(predicate));
    return res;
  }

  template <typename Key, typename Value>
  folly::Optional<ConstIterator> assign(Key&& k, Value&& v) {
    auto h = HashFn{}(k);
    auto segment = pickSegment(h);
    ConstIterator res(this, segment);
    auto seg = segments_[segment].load(std::memory_order_acquire);
    if (!seg) {
      return none;
    } else {
      auto r =
          seg->assign(res.it_, h, std::forward<Key>(k), std::forward<Value>(v));
      if (!r) {
        return none;
      }
    }
    return std::move(res);
  }

  // Assign to desired if and only if the predicate returns true
  // for the current value.
  template <typename Key, typename Value, typename Predicate>
  folly::Optional<ConstIterator> assign_if(
      Key&& k, Value&& desired, Predicate&& predicate) {
    auto h = HashFn{}(k);
    auto segment = pickSegment(h);
    ConstIterator res(this, segment);
    auto seg = segments_[segment].load(std::memory_order_acquire);
    if (!seg) {
      return none;
    } else {
      auto r = seg->assign_if(
          res.it_,
          h,
          std::forward<Key>(k),
          std::forward<Value>(desired),
          std::forward<Predicate>(predicate));
      if (!r) {
        return none;
      }
    }
    return std::move(res);
  }

  // Assign to desired if and only if current value is equal to expected
  template <typename Key, typename Value>
  folly::Optional<ConstIterator> assign_if_equal(
      Key&& k, const ValueType& expected, Value&& desired) {
    auto h = HashFn{}(k);
    auto segment = pickSegment(h);
    ConstIterator res(this, segment);
    auto seg = segments_[segment].load(std::memory_order_acquire);
    if (!seg) {
      return none;
    } else {
      auto r = seg->assign_if_equal(
          res.it_,
          h,
          std::forward<Key>(k),
          expected,
          std::forward<Value>(desired));
      if (!r) {
        return none;
      }
    }
    return std::move(res);
  }

  // Copying wrappers around insert and find.
  // Only available for copyable types.
  const ValueType operator[](const KeyType& key) {
    auto item = insert(key, ValueType());
    return item.first->second;
  }

  template <typename Key, int = 0>
    requires kEligibleForHeterogeneousInsert<Key>
  const ValueType operator[](const Key& key) {
    auto item = insert(key, ValueType());
    return item.first->second;
  }

  const ValueType at(const KeyType& key) const { return atImpl(key); }

  template <typename K, int = 0>
    requires kEligibleForHeterogeneousFind<K>
  const ValueType at(const K& key) const {
    return atImpl(key);
  }

  // TODO update assign interface, operator[], at

  size_type erase(const key_type& k) { return eraseImpl(k); }

  template <typename K, int = 0>
    requires kEligibleForHeterogeneousErase<K>
  size_type erase(const K& k) {
    return eraseImpl(k);
  }

  // Calls the hash function, and therefore may throw.
  // This function doesn't necessarily delete the item that pos points to.
  // It simply tries erasing the item associated with the same key.
  // While this behavior can be confusing, erase(iterator) is often found in
  // std data structures so we follow a similar pattern here.
  ConstIterator erase(ConstIterator& pos) {
    auto h = HashFn{}(pos->first);
    auto segment = pickSegment(h);
    ConstIterator res(this, segment);
    ensureSegment(segment)->erase(res.it_, pos.it_, h);
    res.advanceIfAtSegmentEnd();
    return res;
  }

  // Erase if and only if key k is equal to expected
  size_type erase_if_equal(const key_type& k, const ValueType& expected) {
    return erase_key_if(k, detail::concurrenthashmap::EqualTo1{expected});
  }

  template <typename K, int = 0>
    requires kEligibleForHeterogeneousErase<K>
  size_type erase_if_equal(const K& k, const ValueType& expected) {
    return erase_key_if(k, detail::concurrenthashmap::EqualTo1{expected});
  }

  // Erase if predicate evaluates to true on the existing value
  template <typename Predicate>
  size_type erase_key_if(const key_type& k, Predicate&& predicate) {
    return eraseKeyIfImpl(k, std::forward<Predicate>(predicate));
  }

  template <typename K, typename Predicate, int = 0>
    requires kEligibleForHeterogeneousErase<K>
  size_type erase_key_if(const K& k, Predicate&& predicate) {
    return eraseKeyIfImpl(k, std::forward<Predicate>(predicate));
  }

  // NOT noexcept, initializes new shard segments vs.
  void clear() {
    uint64_t begin = beginSeg_.load(std::memory_order_acquire);
    uint64_t end = endSeg_.load(std::memory_order_acquire);
    for (uint64_t i = begin; i < end; ++i) {
      auto seg = segments_[i].load(std::memory_order_acquire);
      if (seg) {
        seg->clear();
      }
    }
  }

  void reserve(size_t count) {
    count = count >> ShardBits;
    if (!count) {
      return;
    }
    uint64_t begin = beginSeg_.load(std::memory_order_acquire);
    uint64_t end = endSeg_.load(std::memory_order_acquire);
    for (uint64_t i = begin; i < end; ++i) {
      auto seg = segments_[i].load(std::memory_order_acquire);
      if (seg) {
        seg->rehash(count);
      }
    }
  }

  // This is a rolling size, and is not exact at any moment in time.
  size_t size() const noexcept {
    size_t res = 0;
    uint64_t begin = beginSeg_.load(std::memory_order_acquire);
    uint64_t end = endSeg_.load(std::memory_order_acquire);
    for (uint64_t i = begin; i < end; ++i) {
      auto seg = segments_[i].load(std::memory_order_acquire);
      if (seg) {
        res += seg->size();
      }
    }
    return res;
  }

  float max_load_factor() const { return load_factor_; }

  void max_load_factor(float factor) {
    uint64_t begin = beginSeg_.load(std::memory_order_acquire);
    uint64_t end = endSeg_.load(std::memory_order_acquire);
    for (uint64_t i = begin; i < end; ++i) {
      auto seg = segments_[i].load(std::memory_order_acquire);
      if (seg) {
        seg->max_load_factor(factor);
      }
    }
  }

  class ConstIterator {
   public:
    friend class ConcurrentHashMap;

    const value_type& operator*() const { return *it_; }

    const value_type* operator->() const { return &*it_; }

    ConstIterator& operator++() {
      ++it_;
      advanceIfAtSegmentEnd();
      return *this;
    }

    bool operator==(const ConstIterator& o) const {
      return it_ == o.it_ && segment_ == o.segment_;
    }

    bool operator!=(const ConstIterator& o) const { return !(*this == o); }

    ConstIterator& operator=(const ConstIterator& o) = delete;

    ConstIterator& operator=(ConstIterator&& o) noexcept {
      if (this != &o) {
        it_ = std::move(o.it_);
        segment_ = std::exchange(o.segment_, uint64_t(NumShards));
        parent_ = std::exchange(o.parent_, nullptr);
      }
      return *this;
    }

    ConstIterator(const ConstIterator& o) = delete;

    ConstIterator(ConstIterator&& o) noexcept
        : it_(std::move(o.it_)),
          segment_(std::exchange(o.segment_, uint64_t(NumShards))),
          parent_(std::exchange(o.parent_, nullptr)) {}

    ConstIterator(const ConcurrentHashMap* parent, uint64_t segment)
        : segment_(segment), parent_(parent) {}

   private:
    // cbegin iterator
    explicit ConstIterator(const ConcurrentHashMap* parent)
        : it_(nullptr),
          segment_(parent->beginSeg_.load(std::memory_order_acquire)),
          parent_(parent) {
      advanceToSegmentBegin();
    }

    // cend iterator
    explicit ConstIterator(uint64_t shards) : it_(nullptr), segment_(shards) {}

    void advanceIfAtSegmentEnd() {
      DCHECK_LT(segment_, parent_->NumShards);
      SegmentT* seg =
          parent_->segments_[segment_].load(std::memory_order_acquire);
      DCHECK(seg);
      if (it_ == seg->cend()) {
        ++segment_;
        advanceToSegmentBegin();
      }
    }

    FOLLY_ALWAYS_INLINE void advanceToSegmentBegin() {
      // Advance to the beginning of the next nonempty segment
      // starting from segment_.
      uint64_t end = parent_->endSeg_.load(std::memory_order_acquire);
      while (segment_ < end) {
        SegmentT* seg =
            parent_->segments_[segment_].load(std::memory_order_acquire);
        if (seg) {
          it_ = seg->cbegin();
          if (it_ != seg->cend()) {
            return;
          }
        }
        ++segment_;
      }
      // All segments are empty. Advance to end.
      segment_ = parent_->NumShards;
    }

    typename SegmentT::Iterator it_;
    uint64_t segment_;
    const ConcurrentHashMap* parent_;
  };

 private:
  template <typename K>
  ConstIterator findImpl(const K& k) const {
    auto h = HashFn{}(k);
    auto segment = pickSegment(h);
    ConstIterator res(this, segment);
    auto seg = segments_[segment].load(std::memory_order_acquire);
    if (!seg || !seg->find(res.it_, h, k)) {
      res.segment_ = NumShards;
    }
    return res;
  }

  template <typename K>
  const ValueType atImpl(const K& k) const {
    auto item = find(k);
    if (item == cend()) {
      throw_exception<std::out_of_range>("at(): key not in map");
    }
    return item->second;
  }

  template <typename Key>
  std::pair<ConstIterator, bool> insertImpl(std::pair<Key, mapped_type>&& foo) {
    auto h = HashFn{}(foo.first);
    auto segment = pickSegment(h);
    std::pair<ConstIterator, bool> res(
        std::piecewise_construct,
        std::forward_as_tuple(this, segment),
        std::forward_as_tuple(false));
    res.second =
        ensureSegment(segment)->insert(res.first.it_, h, std::move(foo));
    return res;
  }

  template <typename K>
  size_type eraseImpl(const K& k) {
    auto h = HashFn{}(k);
    auto segment = pickSegment(h);
    auto seg = segments_[segment].load(std::memory_order_acquire);
    if (!seg) {
      return 0;
    } else {
      return seg->erase(h, k);
    }
  }

  template <typename K, typename Predicate>
  size_type eraseKeyIfImpl(const K& k, Predicate&& predicate) {
    auto h = HashFn{}(k);
    auto segment = pickSegment(h);
    auto seg = segments_[segment].load(std::memory_order_acquire);
    if (!seg) {
      return 0;
    }
    return seg->erase_key_if(h, k, std::forward<Predicate>(predicate));
  }

  uint64_t pickSegment(size_t h) const {
    // Use the lowest bits for our shard bits.
    //
    // This works well even if the hash function is biased towards the
    // low bits: The sharding will happen in the segments_ instead of
    // in the segment buckets, so we'll still get write sharding as
    // well.
    //
    // Low-bit bias happens often for std::hash using small numbers,
    // since the integer hash function is the identity function.
    return h & (NumShards - 1);
  }

  SegmentT* ensureSegment(uint64_t i) const {
    SegmentT* seg = segments_[i].load(std::memory_order_acquire);
    if (!seg) {
      auto b = ensureCohort();
      SegmentT* newseg = SegmentTAllocator().allocate(1);
      newseg = new (newseg)
          SegmentT(size_ >> ShardBits, load_factor_, max_size_ >> ShardBits, b);
      if (!segments_[i].compare_exchange_strong(seg, newseg)) {
        // seg is updated with new value, delete ours.
        newseg->~SegmentT();
        SegmentTAllocator().deallocate(newseg, 1);
      } else {
        seg = newseg;
        updateBeginAndEndSegments(i);
      }
    }
    return seg;
  }

  void updateBeginAndEndSegments(uint64_t i) const {
    uint64_t val = beginSeg_.load(std::memory_order_acquire);
    while (i < val && !casSeg(beginSeg_, val, i)) {
    }
    val = endSeg_.load(std::memory_order_acquire);
    while (i + 1 > val && !casSeg(endSeg_, val, i + 1)) {
    }
  }

  bool casSeg(Atom<uint64_t>& seg, uint64_t& expval, uint64_t newval) const {
    return seg.compare_exchange_weak(
        expval, newval, std::memory_order_acq_rel, std::memory_order_acquire);
  }

  hazptr_obj_cohort<Atom>* cohort() const noexcept {
    return cohort_.load(std::memory_order_acquire);
  }

  hazptr_obj_cohort<Atom>* ensureCohort() const {
    auto b = cohort();
    if (!b) {
      auto storage = Allocator().allocate(sizeof(hazptr_obj_cohort<Atom>));
      auto newcohort = new (storage) hazptr_obj_cohort<Atom>();
      if (cohort_.compare_exchange_strong(b, newcohort)) {
        b = newcohort;
      } else {
        newcohort->~hazptr_obj_cohort<Atom>();
        Allocator().deallocate(storage, sizeof(hazptr_obj_cohort<Atom>));
      }
    }
    return b;
  }

  void cohort_shutdown_cleanup() {
    auto b = cohort();
    if (b) {
      b->~hazptr_obj_cohort<Atom>();
      Allocator().deallocate((uint8_t*)b, sizeof(hazptr_obj_cohort<Atom>));
    }
  }

  mutable Atom<SegmentT*> segments_[NumShards];
  size_t size_{0};
  size_t max_size_{0};
  mutable Atom<hazptr_obj_cohort<Atom>*> cohort_{nullptr};
  mutable Atom<uint64_t> beginSeg_{NumShards};
  mutable Atom<uint64_t> endSeg_{0};
};

template <
    typename KeyType,
    typename ValueType,
    typename HashFn = std::hash<KeyType>,
    typename KeyEqual = std::equal_to<KeyType>,
    typename Allocator = std::allocator<uint8_t>,
    uint8_t ShardBits = 8,
    template <typename> class Atom = std::atomic,
    class Mutex = std::mutex>
using ConcurrentHashMapSIMD = ConcurrentHashMap<
    KeyType,
    ValueType,
    HashFn,
    KeyEqual,
    Allocator,
    ShardBits,
    Atom,
    Mutex,
#if (                                                        \
    FOLLY_SSE_PREREQ(4, 2) ||                                \
    (FOLLY_AARCH64 && FOLLY_F14_CRC_INTRINSIC_AVAILABLE)) && \
    FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
    detail::concurrenthashmap::simd::SIMDTable
#else
    // fallback to regular impl
    detail::concurrenthashmap::bucket::BucketTable
#endif
    >;

} // namespace folly
