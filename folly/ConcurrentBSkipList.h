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

// Concurrent B-tree/skip-list hybrid: HOH writes; reads escalate optimistic
// leaf scan → OLC descent → locked HOH.
//
// Level 0 = leaves; higher levels = routing keys + children. B is per-node
// key capacity; P is the promotion denominator (see topLevelForKey).
//
// Requirements on T: specialize std::numeric_limits (min/max are reserved
// sentinels). Hash must be deterministic across the container's lifetime.
//
// Most users want the named aliases at the bottom: ConcurrentBSkipSet,
// ConcurrentBSkipMap, ConcurrentBSkipInlineMap.
//
// Locking:
//   - HOH always couples locks top-down; horizontal moves crab.
//   - growthLock_ never overlaps any HOH lock.
//   - remove() tombstones; nodes are never reclaimed.
//
// Thread-safe: add, remove, contains, find, updatePayload, addOrUpdate.
// Skipper is safe against concurrent list mutation; one Skipper per thread.
// Destructor is not thread-safe.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <type_traits>
#include <utility>
#include <variant>

#include <glog/logging.h>
#include <folly/CPortability.h>
#include <folly/ConcurrentBSkipList-detail.h>
#include <folly/MicroLock.h>
#include <folly/Utility.h>
#include <folly/lang/Align.h>
#include <folly/lang/Assume.h>
#include <folly/small_vector.h>
#include <folly/synchronization/RWSpinLock.h>
#include <folly/synchronization/RelaxedAtomic.h>

namespace folly {
// add()/addOrUpdate() collapse this to bool via didInsert(); use directly
// to distinguish Updated from AlreadyPresent.
enum class BSkipInsertOutcome {
  // didInsert() == true:
  Inserted,
  Revived,
  // didInsert() == false:
  Updated,
  AlreadyPresent,
};

constexpr bool didInsert(BSkipInsertOutcome outcome) {
  return outcome == BSkipInsertOutcome::Inserted ||
      outcome == BSkipInsertOutcome::Revived;
}

namespace bskip_detail {
// Live exact-match handling: Add → AlreadyPresent; AddOrUpdate → fire updater.
enum class InsertSemantics { Add, AddOrUpdate };
} // namespace bskip_detail

// Two-layer config: per-axis template params (B, ReadPolicy, StoragePolicy)
// take their defaults from compile-time computation; Policy supplies the
// rest (Hash, Comp, NodeAlloc, kPromotionProbInverse, kMaxHeight). The named
// aliases at the bottom forward Policy::k* into the per-axis params.
// ConcurrentBSkipDefaultPolicy is defined at the bottom of this header.
template <
    typename T,
    typename PromotionHash = std::hash<T>,
    typename Compare = std::less<T>,
    typename Allocator = std::allocator<char>>
struct ConcurrentBSkipDefaultPolicy;

template <
    typename T,
    typename PayloadType = void,
    int B = 16,
    KeyReadPolicy ReadPolicy = bskip_detail::kDefaultReadPolicy<T>,
    LeafStoragePolicy StoragePolicy =
        LeafStoragePolicy::Separate, // accepts folly::LeafStoragePolicy values
    typename Policy = ConcurrentBSkipDefaultPolicy<T>>
// If you add a template parameter, also extend FOLLY_BSKIP_FRIEND_LIST in
// ConcurrentBSkipList-detail.h to match.
class ConcurrentBSkipList {
  using Comp = typename Policy::Comp;
  using Hash = typename Policy::Hash;
  using NodeAlloc = typename Policy::NodeAlloc;
  static constexpr uint8_t kPromotionProbInverse =
      Policy::kPromotionProbInverse;
  static constexpr uint8_t kMaxHeight = Policy::kMaxHeight;

  // Cross-parameter checks (T sentinels, payload triviality, etc.) live in
  // InternalTraits.
  static_assert(kMaxHeight > 0, "kMaxHeight must be > 0");
  static_assert(B <= 64, "B must be <= 64 (tombstone bitmap is uint64_t)");
  static_assert(
      kPromotionProbInverse > 1,
      "kPromotionProbInverse must be > 1 (height assignment uses log_P digits)");
  static_assert(
      (kPromotionProbInverse & (kPromotionProbInverse - 1)) == 0,
      "kPromotionProbInverse must be a power of 2");
  // Stateless Hash/Comp: promotion height is part of a key's structural
  // identity, so two instantiations must agree on Hash{}(k) and Comp.
  static_assert(std::is_empty_v<Hash>, "Hash must be stateless");
  static_assert(std::is_empty_v<Comp>, "Comp must be stateless");

  using Traits = bskip_detail::InternalTraits<
      T,
      Comp,
      B,
      kPromotionProbInverse,
      ReadPolicy,
      PayloadType,
      StoragePolicy>;
  using Node = bskip_detail::BSkipNode<Traits>;
  using LeafNode = bskip_detail::BSkipNodeLeaf<Traits>;
  using InternalNode = bskip_detail::BSkipNodeInternal<Traits>;

  template <typename LookupKey>
  static bool isSentinel(const LookupKey& k) {
    return Traits::equal(k, Traits::kMinSentinel) ||
        Traits::equal(k, Traits::kMaxSentinel);
  }

 public:
  using key_type = T;
  using payload_type = PayloadType;
  static constexpr bool kHasPayload = Traits::kHasPayload;
  static constexpr bool kInlinePayloadStorage = Traits::kInlinePayload;
  using leaf_node_type = LeafNode;
  static constexpr KeyReadPolicy kReadPolicy = ReadPolicy;

  using FindResult = std::conditional_t<
      kHasPayload,
      std::optional<std::pair<T, PayloadType>>,
      std::optional<T>>;

  ConcurrentBSkipList() : ConcurrentBSkipList(NodeAlloc{}) {}

  explicit ConcurrentBSkipList(const NodeAlloc& alloc) : alloc_(alloc) {
    beginLeaf_.level_ = 0;
    beginLeaf_.numElements_.store(1);
    beginLeaf_.storeKey(0, Traits::kMinSentinel);
    // Tombstone the sentinel at slot 0 so scans skip it.
    beginLeaf_.tombstones_.store(1);
    headers_[0] = &beginLeaf_;
  }

  // Not thread-safe. Must outlive every Skipper instance — Skipper's OLC
  // path relies on nodes never being reclaimed under it.
  ~ConcurrentBSkipList() {
    uint8_t height = height_.load(std::memory_order_relaxed);
    for (uint8_t level = 0; level < height; ++level) {
      // Single-threaded teardown; no concurrent access.
      Node* node = headers_[level].load(std::memory_order_relaxed);
      while (node != nullptr) {
        Node* nextNode = node->next_.load(std::memory_order_relaxed);
        if (node != static_cast<Node*>(&beginLeaf_)) {
          deallocByLevel(node);
        }
        node = nextNode;
      }
    }
  }

  ConcurrentBSkipList(const ConcurrentBSkipList&) = delete;
  ConcurrentBSkipList& operator=(const ConcurrentBSkipList&) = delete;
  ConcurrentBSkipList(ConcurrentBSkipList&&) = delete;
  ConcurrentBSkipList& operator=(ConcurrentBSkipList&&) = delete;

  // Returns true on fresh insert or tombstone revive; false if already present.
  bool add(const T& key) {
    return didInsert(insertImpl<bskip_detail::InsertSemantics::Add>(key));
  }

  // Existing live keys are not overwritten; use addOrUpdate for that.
  template <typename RequestedPayload = PayloadType>
  bool add(const T& key, const RequestedPayload& payload)
    requires(!std::is_void_v<RequestedPayload>)
  {
    auto initialValueSetter =
        [&payload](const T&, std::optional<RequestedPayload>& slot) {
          if (!slot) {
            slot = payload;
          }
        };
    return didInsert(
        insertImpl<bskip_detail::InsertSemantics::Add>(
            key, initialValueSetter));
  }

  // Fused upsert. Updater runs under the leaf mutex + seqlock write epoch;
  // must not allocate, take other locks, or throw. Returns true on insert/
  // revive, false on update.
  // Updater:
  //   void payload:    void(const T&)
  //   payload-bearing: void(const T&, std::optional<PayloadType>& slot)
  //     - empty slot in / left empty out: default-constructed payload.
  //     - filled slot: caller's value is stored.
  template <typename Updater>
  bool addOrUpdate(const T& key, Updater&& updater) {
    return didInsert(
        insertImpl<bskip_detail::InsertSemantics::AddOrUpdate>(
            key, std::forward<Updater>(updater)));
  }

  bool remove(const T& key) {
    DCHECK(!isSentinel(key))
        << "sentinel keys are reserved; inserting/removing them is undefined";
    return traverseHohLeaf<HeldNodeLockMode::Exclusive>(
        key,
        [&](LeafNode* leaf, uint8_t slot) {
          if (leaf->tombstoned(slot)) {
            return false;
          }
          auto guard = leaf->seq_.writeGuard();
          leaf->setTombstone(slot);
          size_.fetch_sub(1);
          return true;
        },
        [] { return false; });
  }

  template <typename RequestedPayload = PayloadType>
  std::optional<RequestedPayload> removeAndGetPayload(const T& key)
    requires(!std::is_void_v<RequestedPayload>)
  {
    DCHECK(!isSentinel(key))
        << "sentinel keys are reserved; inserting/removing them is undefined";
    return traverseHohLeaf<HeldNodeLockMode::Exclusive>(
        key,
        [&](LeafNode* leaf, uint8_t slot) -> std::optional<RequestedPayload> {
          if (leaf->tombstoned(slot)) {
            return std::nullopt;
          }
          auto oldPayload = leaf->loadPayload(slot);
          auto guard = leaf->seq_.writeGuard();
          leaf->setTombstone(slot);
          size_.fetch_sub(1);
          return oldPayload;
        },
        [] { return std::optional<RequestedPayload>{}; });
  }

  // Updates the payload of a live (non-tombstoned) key. Returns false if the
  // key is absent or tombstoned; use add() + updatePayload() or addOrUpdate().
  // Updater runs under the leaf mutex + seqlock write epoch; bounded work only.
  template <typename Updater>
  bool updatePayload(const T& key, Updater&& updater)
    requires(kHasPayload)
  {
    DCHECK(!isSentinel(key))
        << "sentinel keys are reserved; inserting/removing them is undefined";
    return traverseHohLeaf<HeldNodeLockMode::Exclusive>(
        key,
        [&](LeafNode* leaf, uint8_t slot) {
          if (leaf->tombstoned(slot)) {
            return false;
          }
          updateLeafSlot(leaf, slot, std::forward<Updater>(updater));
          return true;
        },
        [] { return false; });
  }

  bool contains(const T& key) const { return find(key).has_value(); }
  size_t count(const T& key) const { return contains(key) ? 1 : 0; }

  FindResult find(const T& key) const { return findImpl(key); }

  template <
      typename LookupKey,
      typename C = Comp,
      typename = typename C::is_transparent>
  bool contains(const LookupKey& key) const {
    return find(key).has_value();
  }
  template <
      typename LookupKey,
      typename C = Comp,
      typename = typename C::is_transparent>
  size_t count(const LookupKey& key) const {
    return contains(key) ? 1 : 0;
  }

  template <
      typename LookupKey,
      typename C = Comp,
      typename = typename C::is_transparent>
  FindResult find(const LookupKey& key) const {
    return findImpl(key);
  }

  // Live (non-tombstoned) count; eventually consistent with concurrent ops.
  size_t size() const { return size_.load(); }

  bool empty() const { return size() == 0; }
  size_t erase(const T& key) { return remove(key) ? 1 : 0; }

  // Thread-safe full scan. `func` runs under the leaf's read guard, so it
  // must be bounded work with no re-entry. Each live key is emitted exactly
  // once (deduped against lastEmittedKey across retries and concurrent
  // splits). Per-slot consistency, not snapshot isolation.
  template <typename Func>
  size_t forEachElement(Func&& func) const {
    size_t count = 0;
    auto* leaf =
        static_cast<LeafNode*>(headers_[0].load(std::memory_order_acquire));
    // kMinSentinel: slot 0 is tombstoned so it's never emitted, and every
    // real key compares greater, so the dedup check passes on the first hit.
    T lastEmittedKey = Traits::kMinSentinel;
    while (leaf != nullptr) {
      scanLeafLiveAdaptive(leaf, lastEmittedKey, count, func);
      leaf =
          static_cast<LeafNode*>(leaf->next_.load(std::memory_order_acquire));
    }
    return count;
  }

  // Forward-only cursor; safe against concurrent list mutation but not
  // itself synchronized (one Skipper per thread). skipTo(target) returns the
  // first element >= target via scanCurrentLeaf → rescanCurrentLeaf
  // → scanAdjacentLeaf → optimisticDescend → skipToLockedFromRoot.
  class Skipper;

 private:
  using PayloadPtr = typename Traits::PayloadPtr;

  // forEachElement helper. lastEmittedKey is bumped only after func returns,
  // so retries naturally skip already-emitted slots. Falls back to a locked
  // scan after kMaxOptimisticAttempts.
  template <typename Func>
  void scanLeafLiveAdaptive(
      LeafNode* leaf, T& lastEmittedKey, size_t& count, Func& func) const {
    if constexpr (Traits::kOptimistic) {
      constexpr int kMaxOptimisticAttempts = 3;
      for (int attempt = 0; attempt < kMaxOptimisticAttempts; ++attempt) {
        auto guard = leaf->adaptiveRead();
        auto validate = [&] { return guard.valid(); };
        const uint8_t n = leaf->numElements_.load();
        bool valid = true;
        for (uint8_t i = 0; i < n; ++i) {
          if (leaf->tombstoned(i)) {
            continue;
          }
          if constexpr (kHasPayload) {
            T k = static_cast<T>(leaf->keyStorageAt(i));
            PayloadType p = static_cast<PayloadType>(
                leaf->template payloadStorageAt<PayloadType>(i));
            if (!validate()) {
              valid = false;
              break;
            }
            if (!Traits::less(lastEmittedKey, k)) {
              continue;
            }
            func(k, p);
            lastEmittedKey = std::move(k);
          } else {
            T k = static_cast<T>(leaf->keyStorageAt(i));
            if (!validate()) {
              valid = false;
              break;
            }
            if (!Traits::less(lastEmittedKey, k)) {
              continue;
            }
            func(k);
            lastEmittedKey = std::move(k);
          }
          ++count;
        }
        if (valid && validate()) {
          return;
        }
      }
    }
    // Locked fallback.
    std::shared_lock<folly::RWSpinLock> lock(leaf->mutex_);
    const uint8_t n = leaf->numElements_.load();
    for (uint8_t i = 0; i < n; ++i) {
      if (leaf->tombstoned(i)) {
        continue;
      }
      T key = leaf->loadKey(i);
      if (!Traits::less(lastEmittedKey, key)) {
        continue;
      }
      if constexpr (kHasPayload) {
        func(key, leaf->loadPayload(i));
      } else {
        func(key);
      }
      lastEmittedKey = std::move(key);
      ++count;
    }
  }

  template <typename Validator>
  FOLLY_ALWAYS_INLINE std::optional<T> loadLeafKeyValidated(
      LeafNode* leaf, uint8_t slot, Validator&& validate) const {
    auto validateLoadedKey = [&] {
      // No-op in production (gated on FOLLY_BSKIP_TEST_HOOKS).
      bskip_detail::invokeBSkipTestHook(
          bskip_detail::BSkipTestHookEvent::LeafKeyPostLoadValidate,
          std::memory_order_acquire,
          leaf,
          nullptr);
      return validate();
    };
    return bskip_detail::loadValidated<T>(
        leaf->keyStorageAt(slot), validateLoadedKey);
  }

  template <typename Validator, typename RequestedPayload = PayloadType>
  FOLLY_ALWAYS_INLINE std::optional<RequestedPayload> loadLeafPayloadValidated(
      LeafNode* leaf, uint8_t slot, Validator&& validate) const
    requires(!std::is_void_v<RequestedPayload>)
  {
    return bskip_detail::loadValidated<RequestedPayload>(
        leaf->template payloadStorageAt<RequestedPayload>(slot),
        std::forward<Validator>(validate));
  }

  template <typename NodeType, typename Validator>
  FOLLY_ALWAYS_INLINE std::optional<T> loadNextMinKeyValidated(
      NodeType* node, Validator&& validate) const {
    return bskip_detail::loadValidated<T>(
        static_cast<const typename Traits::KeyStorage&>(node->nextMinKey_),
        std::forward<Validator>(validate));
  }

  template <typename LookupKey>
  FindResult findImpl(const LookupKey& key) const {
    DCHECK(!isSentinel(key))
        << "sentinel keys are reserved; looking them up is undefined";
    return traverseHohLeaf<HeldNodeLockMode::Shared>(
        key,
        [](LeafNode* leaf, uint8_t slot) -> FindResult {
          if (leaf->tombstoned(slot)) {
            return std::nullopt;
          }
          if constexpr (kHasPayload) {
            return std::pair{leaf->loadKey(slot), leaf->loadPayload(slot)};
          } else {
            return leaf->loadKey(slot);
          }
        },
        [] { return FindResult{std::nullopt}; });
  }

  LeafNode* allocLeaf() { return alloc_.template allocate<LeafNode>(); }
  InternalNode* allocInternal() {
    return alloc_.template allocate<InternalNode>();
  }
  void deallocLeaf(LeafNode* node) {
    alloc_.template deallocate<LeafNode>(node);
  }
  void deallocInternal(InternalNode* node) {
    alloc_.template deallocate<InternalNode>(node);
  }
  void deallocByLevel(Node* node) {
    if (node->level_ > 0) {
      deallocInternal(static_cast<InternalNode*>(node));
    } else {
      deallocLeaf(static_cast<LeafNode*>(node));
    }
  }

  // ---------------------------------------------------------------------------
  // Lock and allocation guards
  // ---------------------------------------------------------------------------

  // CarriedParentLock carries the parent node's lock across HOH descent until
  // the child lock is acquired. Mode is statically known at every adopt site
  // (callers template on HeldNodeLockMode).
  struct CarriedParentLock : folly::NonCopyableNonMovable {
    CarriedParentLock() = default;
    CarriedParentLock(const CarriedParentLock&) = delete;
    CarriedParentLock(CarriedParentLock&&) = delete;
    ~CarriedParentLock() { release(); }
    CarriedParentLock& operator=(const CarriedParentLock&) = delete;
    CarriedParentLock& operator=(CarriedParentLock&&) = delete;

    void adoptShared(folly::RWSpinLock& lock) {
      DCHECK(lock_ == nullptr) << "CarriedParentLock: adopt while holding";
      lock_ = &lock;
      isExclusive_ = false;
    }

    void adoptExclusive(folly::RWSpinLock& lock) {
      DCHECK(lock_ == nullptr) << "CarriedParentLock: adopt while holding";
      lock_ = &lock;
      isExclusive_ = true;
    }

    void release() {
      if (lock_ == nullptr) {
        return;
      }
      if (isExclusive_) {
        lock_->unlock();
      } else {
        lock_->unlock_shared();
      }
      lock_ = nullptr;
    }

   private:
    folly::RWSpinLock* lock_ = nullptr;
    bool isExclusive_ = false;
  };

  // Pre-allocated outside HOH locks so insert stays alloc-free under them.
  // For topLevel >= 1: slots[0..topLevel-2] = InternalNode siblings (consumed
  // top-down), slots[topLevel-1] = LeafNode, slots[topLevel] = split sibling.
  // For topLevel == 0: slots[0] = LeafNode (split sibling only). The level_
  // set in allocateLockedSlot is just an Internal-vs-Leaf type tag for the
  // destructor; beginSplitSiblingWrite overwrites it at publish.
  struct PreallocatedNodes : folly::NonCopyableNonMovable {
    ConcurrentBSkipList& list;
    std::array<Node*, kMaxHeight> slots{};
    uint8_t slotCount = 0;
    uint8_t topLevel = 0;
    uint8_t nodesConsumed = 0;

    explicit PreallocatedNodes(ConcurrentBSkipList& l) : list(l) {}
    PreallocatedNodes(const PreallocatedNodes&) = delete;
    PreallocatedNodes(PreallocatedNodes&&) = delete;

    ~PreallocatedNodes() {
      for (uint8_t i = 0; i < slotCount; ++i) {
        if (slots[i] != nullptr) {
          slots[i]->mutex_.unlock();
          list.deallocByLevel(slots[i]);
        }
      }
    }

    PreallocatedNodes& operator=(const PreallocatedNodes&) = delete;
    PreallocatedNodes& operator=(PreallocatedNodes&&) = delete;

    template <typename NodeType>
    NodeType* peekSlot(uint8_t slot) const {
      DCHECK_GE(slot, 0);
      DCHECK_LT(slot, slotCount);
      NodeType* node = static_cast<NodeType*>(slots[slot]);
      DCHECK(node);
      return node;
    }

    template <typename NodeType>
    NodeType* takeSlot(uint8_t slot) {
      NodeType* node = peekSlot<NodeType>(slot);
      slots[slot] = nullptr;
      return node;
    }

    Node* allocateLockedSlot(uint8_t level) {
      Node* node = level > 0
          ? static_cast<Node*>(list.allocInternal())
          : static_cast<Node*>(list.allocLeaf());
      node->level_ = level;
      node->mutex_.lock();
      return node;
    }

    // Top-down tower build: each slot is reachable from above before its
    // own beginSplitSiblingWrite. Racing OLC readers bail via numElements_
    // == 0; locked fallback then blocks on the inserter's mutex. Don't
    // widen the seqlock to signal "provisional" — kReadSpins exhausts
    // quickly and readers fall back to a locked path.
    void preallocateInsertPath(uint8_t desiredTopLevel) {
      topLevel = desiredTopLevel;
      slotCount = desiredTopLevel + 1;
      for (uint8_t i = 0; i < desiredTopLevel; ++i) {
        // level > 0 = internal node, level 0 = leaf.
        slots[i] = allocateLockedSlot((i + 1 < topLevel) ? 1 : 0);
      }
      // Split sibling: leaf when topLevel==0, internal otherwise.
      slots[topLevel] = allocateLockedSlot(this->topLevel);
    }

    // slots[0..topLevel-1] are the promoted tower nodes (consumed bottom-up).
    // slots[topLevel] is the split sibling.
    template <typename NodeType>
    NodeType* peekNext() const {
      DCHECK_LT(nodesConsumed, topLevel);
      return peekSlot<NodeType>(nodesConsumed);
    }

    template <typename NodeType>
    NodeType* takeNext() {
      NodeType* node = peekNext<NodeType>();
      slots[nodesConsumed] = nullptr;
      ++nodesConsumed;
      return node;
    }

    template <typename NodeType>
    NodeType* peekSibling() const {
      return peekSlot<NodeType>(topLevel);
    }

    template <typename NodeType>
    NodeType* takeSibling() {
      return takeSlot<NodeType>(topLevel);
    }
  };

  // Caller holds node->mutex_ and has opened a seqlock write epoch.
  FOLLY_ALWAYS_INLINE void insertInternalEntry(
      InternalNode* node,
      uint8_t insertionSlot,
      const T& key,
      Node* promotedChild) {
    // Data before size: OLC readers use numElements_ as a loop bound.
    node->insertKeyAtSlot(insertionSlot, key);
    node->insertChildAtSlot(insertionSlot, promotedChild);
    node->numElements_.fetch_add(1);
  }

  // Three leaf-write helpers under leaf->mutex_ + seq_.writeGuard(), differing
  // in pre-state and updater signature:
  //   reviveLeafSlot       tombstoned -> live; updater (T, optional<P>&), may
  //                        be nullptr_t (Add with no initial value).
  //   updateLeafSlot       live; updater (T, P&). Caller: updatePayload.
  //   addOrUpdateLeafSlot  live; updater (T, optional<P>&) seeded w/ current
  //                        payload. Caller: addOrUpdate.
  template <typename Updater>
  FOLLY_ALWAYS_INLINE void reviveLeafSlot(
      LeafNode* leaf, uint8_t exactMatchSlot, const T& key, Updater& updater) {
    auto guard = leaf->seq_.writeGuard();
    leaf->clearTombstone(exactMatchSlot);
    // Equality under Comp doesn't guarantee bitwise identity.
    leaf->storeKey(exactMatchSlot, key);
    if constexpr (kHasPayload) {
      std::optional<PayloadType> payload;
      if constexpr (!std::is_same_v<Updater, std::nullptr_t>) {
        updater(key, payload);
      }
      leaf->storePayload(
          exactMatchSlot, payload ? std::move(*payload) : PayloadType{});
    }
  }

  // updatePayload signature: void(const T&, PayloadType&).
  template <typename Updater>
  FOLLY_ALWAYS_INLINE void updateLeafSlot(
      LeafNode* leaf, uint8_t exactMatchSlot, Updater updater) {
    auto guard = leaf->seq_.writeGuard();
    const T updatedKey = leaf->loadKey(exactMatchSlot);
    if constexpr (kHasPayload) {
      PayloadType payload = leaf->loadPayload(exactMatchSlot);
      updater(updatedKey, payload);
      leaf->storePayload(exactMatchSlot, std::move(payload));
    } else {
      updater(updatedKey);
    }
  }

  // addOrUpdate signature: void(const T&, std::optional<PayloadType>&).
  template <typename Updater>
  FOLLY_ALWAYS_INLINE void addOrUpdateLeafSlot(
      LeafNode* leaf, uint8_t exactMatchSlot, Updater updater) {
    auto guard = leaf->seq_.writeGuard();
    const T updatedKey = leaf->loadKey(exactMatchSlot);
    if constexpr (kHasPayload) {
      std::optional<PayloadType> payload{leaf->loadPayload(exactMatchSlot)};
      updater(updatedKey, payload);
      leaf->storePayload(
          exactMatchSlot, payload ? std::move(*payload) : PayloadType{});
    } else {
      updater(updatedKey);
    }
  }

  // ---------------------------------------------------------------------------
  // HOH locking and traversal
  // ---------------------------------------------------------------------------

  enum class HeldNodeLockMode { Shared, Exclusive };
  enum class NextEdge { AssertNonNull, AllowNull };

  template <HeldNodeLockMode mode>
  FOLLY_ALWAYS_INLINE void lockNode(Node* node) const {
    if constexpr (mode == HeldNodeLockMode::Exclusive) {
      // RWSpinLock::lock() CAS-loops on bits_==0, so continuous readers
      // starve it. The UPGRADE bit blocks new lock_shared() callers,
      // letting existing readers drain before promotion to exclusive.
      node->mutex_.lock_upgrade();
      node->mutex_.unlock_upgrade_and_lock();
    } else {
      node->mutex_.lock_shared();
    }
  }

  template <HeldNodeLockMode mode>
  FOLLY_ALWAYS_INLINE void unlockNode(Node* node) const {
    if constexpr (mode == HeldNodeLockMode::Exclusive) {
      node->mutex_.unlock();
    } else {
      node->mutex_.unlock_shared();
    }
  }

  template <HeldNodeLockMode mode>
  using LeafGuard = std::conditional_t<
      mode == HeldNodeLockMode::Shared,
      std::shared_lock<folly::RWSpinLock>,
      std::unique_lock<folly::RWSpinLock>>;

  // Caller holds currNode->mutex_ in `mode`. Crabs right while `key >=
  // nextMinKey_`. AllowNull lets traversal stop at end-of-list (lookups);
  // AssertNonNull DCHECKs structural non-null next (inserts).
  template <HeldNodeLockMode mode, NextEdge edge, typename LookupKey>
  FOLLY_ALWAYS_INLINE Node* hohCrabRight(
      Node* currNode, const LookupKey& key) const {
    while (!Traits::less(key, currNode->loadNextMinKey())) {
      Node* nextNode = currNode->next_.load(std::memory_order_relaxed);
      if constexpr (edge == NextEdge::AllowNull) {
        if (nextNode == nullptr) {
          break;
        }
      } else {
        DCHECK(nextNode != nullptr) << "insert path invariant: non-null next";
      }
      lockNode<mode>(nextNode);
      unlockNode<mode>(currNode);
      currNode = nextNode;
    }
    return currNode;
  }

  template <
      HeldNodeLockMode mode,
      NextEdge edge = NextEdge::AssertNonNull,
      typename LookupKey>
  FOLLY_ALWAYS_INLINE Node* hohAcquireAndCrabRight(
      Node* currNode,
      const LookupKey& key,
      CarriedParentLock& parentLock) const {
    lockNode<mode>(currNode);
    parentLock.release();
    return hohCrabRight<mode, edge>(currNode, key);
  }

  // HOH descent to the candidate leaf. Caller owns the final leaf-level
  // rightward crab (lock mode depends on caller's intent).
  template <typename LookupKey>
  Node* hohDescendToLeaf(
      const LookupKey& k, CarriedParentLock& parentLock) const {
    uint8_t height = height_.load(std::memory_order_acquire);
    Node* currNode = headers_[height - 1].load(std::memory_order_acquire);

    for (uint8_t level = height - 1; level > 0; --level) {
      currNode = hohAcquireAndCrabRight<HeldNodeLockMode::Shared>(
          currNode, k, parentLock);
      InternalNode* internal = static_cast<InternalNode*>(currNode);
      typename InternalNode::InternalSearchResult search =
          internal->findChild(k);
      parentLock.adoptShared(internal->mutex_);
      currNode = static_cast<Node*>(internal->children_[search.slot]);
    }
    return currNode;
  }

  // visitor(leaf, slot) for an exact match (may be tombstoned — visitor's
  // call).
  template <
      HeldNodeLockMode mode,
      typename LookupKey,
      typename LeafVisitor,
      typename NotFound>
  auto traverseHohLeaf(
      const LookupKey& k, LeafVisitor visitor, NotFound notFound) const {
    CarriedParentLock parentLock;
    LeafNode* leaf = static_cast<LeafNode*>(hohDescendToLeaf(k, parentLock));
    // hohAcquireAndCrabRight releases parentLock.
    leaf = static_cast<LeafNode*>(
        hohAcquireAndCrabRight<mode>(leaf, k, parentLock));

    LeafGuard<mode> guard(leaf->mutex_, std::adopt_lock);
    typename LeafNode::LeafSearchResult search = leaf->findLeafSlot(k);
    if (search.exactMatch) {
      return visitor(leaf, search.slot);
    }
    return notFound();
  }

  // ---------------------------------------------------------------------------
  // Insert path
  // ---------------------------------------------------------------------------

  template <
      bskip_detail::InsertSemantics kSemantics =
          bskip_detail::InsertSemantics::Add,
      typename Updater = std::nullptr_t>
  BSkipInsertOutcome insertImpl(const T& key, Updater updater = nullptr) {
    DCHECK(!isSentinel(key))
        << "sentinel keys are reserved; inserting/removing them is undefined";

    // Highest level containing this key: 0 means leaf-only, 1 means leaf +
    // one internal level, etc. The same value also indexes the top split-
    // sibling candidate in preallocatedNodes.
    uint8_t topLevel = topLevelForKey(key);

    if (topLevel >= height_.load(std::memory_order_acquire)) {
      ensureHeightAbove(topLevel);
    }

    // Pre-alloc the promotion chain + top-level split sibling so the slow
    // path stays alloc-free under HOH locks. parentLock declared after so
    // LIFO unwind releases the lock before freeing nodes on throw.
    PreallocatedNodes preallocatedNodes{*this};
    CarriedParentLock parentLock;
    preallocatedNodes.preallocateInsertPath(topLevel);

    // Reload — a concurrent grower may have raised height_ since the
    // ensureHeightAbove check above.
    uint8_t height = height_.load(std::memory_order_acquire);
    Node* currNode = headers_[height - 1].load(std::memory_order_acquire);

    for (uint8_t level = height; level-- > 0;) {
      currNode = acquireAndCrabRightForInsert(currNode, level, topLevel, key);
      parentLock.release();

      if (level > 0) {
        if (auto outcome = insertAtInternalLevel<kSemantics>(
                currNode,
                level,
                key,
                topLevel,
                preallocatedNodes,
                parentLock,
                updater)) {
          return *outcome;
        }
      } else {
        return insertAtLeafLevel<kSemantics>(
            currNode, key, topLevel, preallocatedNodes, updater);
      }
    }

    folly::assume_unreachable();
  }

  // Lock currNode in the mode required for `level` (exclusive iff this level
  // will hold the new key, shared otherwise) and crab right past any node
  // whose nextMinKey_ <= key.
  FOLLY_ALWAYS_INLINE Node* acquireAndCrabRightForInsert(
      Node* currNode, uint8_t level, uint8_t topLevel, const T& key) {
    const bool needExclusive = (topLevel >= level);
    if (needExclusive) {
      lockNode<HeldNodeLockMode::Exclusive>(currNode);
    } else {
      lockNode<HeldNodeLockMode::Shared>(currNode);
    }

    return needExclusive
        ? hohCrabRight<HeldNodeLockMode::Exclusive, NextEdge::AssertNonNull>(
              currNode, key)
        : hohCrabRight<HeldNodeLockMode::Shared, NextEdge::AssertNonNull>(
              currNode, key);
  }

  // Internal-level body of insertImpl. Returns nullopt to continue descending;
  // returns an outcome if the key matched a routing key.
  template <
      bskip_detail::InsertSemantics kSemantics,
      typename Updater = std::nullptr_t>
  FOLLY_ALWAYS_INLINE std::optional<BSkipInsertOutcome> insertAtInternalLevel(
      Node*& currNode,
      uint8_t level,
      const T& key,
      uint8_t topLevel,
      PreallocatedNodes& preallocatedNodes,
      CarriedParentLock& parentLock,
      Updater updater) {
    InternalNode* currInternal = static_cast<InternalNode*>(currNode);
    typename InternalNode::InternalSearchResult search =
        currInternal->findChild(key);
    if (search.found) {
      return insertOrUpdatePromoted<kSemantics>(
          currNode, level, search.slot, key, topLevel, updater);
    }
    if (topLevel < level) {
      DCHECK_GT(currNode->level_, 0);
      parentLock.adoptShared(currInternal->mutex_);
      currNode = static_cast<Node*>(currInternal->children_[search.slot]);
      DCHECK(currNode != nullptr);
      return std::nullopt;
    }
    const bool isTopLevel = (topLevel == level);
    if (isTopLevel && currInternal->numElements_.load() == Traits::kMaxKeys) {
      currNode = splitAndInsertInternal(
          currInternal, search.slot, key, preallocatedNodes, parentLock);
    } else if (isTopLevel) {
      currNode = appendPromotedAtTopLevel(
          currInternal, search.slot, key, preallocatedNodes, parentLock);
    } else {
      currNode = splitOffPromotedSibling(
          currInternal, search.slot, key, preallocatedNodes, parentLock);
    }
    return std::nullopt;
  }

  // Promote-and-insert into an existing top-level node that still has room.
  FOLLY_ALWAYS_INLINE Node* appendPromotedAtTopLevel(
      InternalNode* currInternal,
      uint8_t predecessorSlot,
      const T& key,
      PreallocatedNodes& preallocatedNodes,
      CarriedParentLock& parentLock) {
    uint8_t insertionSlot = predecessorSlot + 1;
    Node* promotedChild = preallocatedNodes.template peekNext<Node>();
    {
      auto guard = currInternal->seq_.writeGuard();
      insertInternalEntry(currInternal, insertionSlot, key, promotedChild);
    }
    parentLock.adoptExclusive(currInternal->mutex_);
    return static_cast<Node*>(currInternal->children_[predecessorSlot]);
  }

  // Promoted-tower protocol below the top level: always create a new sibling
  // (NOT overflow handling).
  FOLLY_ALWAYS_INLINE Node* splitOffPromotedSibling(
      InternalNode* currInternal,
      uint8_t predecessorSlot,
      const T& key,
      PreallocatedNodes& preallocatedNodes,
      CarriedParentLock& parentLock) {
    InternalNode* newInternal =
        preallocatedNodes.template takeNext<InternalNode>();
    uint8_t insertionSlot = predecessorSlot + 1;
    Node* promotedChild = preallocatedNodes.template peekNext<Node>();
    insertPromotedSibling(
        currInternal,
        newInternal,
        [&](InternalNode* sibling) {
          insertInternalEntry(
              sibling, /* slot=0: promoted key */ 0, key, promotedChild);
        },
        [&](InternalNode* curr, InternalNode* sibling) {
          // destSlot=1: slot 0 reserved for the new routing key.
          curr->splitKeysAndChildren(sibling, insertionSlot, /* destSlot */ 1);
        });
    parentLock.adoptExclusive(currInternal->mutex_);
    return static_cast<Node*>(currInternal->children_[predecessorSlot]);
  }

  // Leaf-level body of insertImpl. Always terminal — returns the outcome.
  // size_ bump runs under the inserted leaf's lock so readers see the entry
  // and the counter consistently.
  template <
      bskip_detail::InsertSemantics kSemantics,
      typename Updater = std::nullptr_t>
  FOLLY_ALWAYS_INLINE BSkipInsertOutcome insertAtLeafLevel(
      Node* currNode,
      const T& key,
      uint8_t topLevel,
      PreallocatedNodes& preallocatedNodes,
      Updater updater) {
    LeafNode* currLeaf = static_cast<LeafNode*>(currNode);
    typename LeafNode::LeafSearchResult search = currLeaf->findLeafSlot(key);
    if (search.exactMatch) {
      return insertOrUpdateAtLeaf<kSemantics>(
          currLeaf, search.slot, key, updater);
    }
    const bool isTopLevel = (topLevel == 0);
    // Default: the new key lands in currLeaf and we hold its lock through
    // size_ bump. Only the split branch overrides lockedLeaf.
    LeafNode* lockedLeaf = currLeaf;
    if (isTopLevel && currLeaf->numElements_.load() == Traits::kMaxKeys) {
      lockedLeaf = splitAndInsertLeaf(
          currLeaf, search.slot, key, preallocatedNodes, updater);
    } else {
      // Build the initial value once. monostate fallback keeps
      // std::optional<void> from being instantiated for void payload.
      [[maybe_unused]] std::
          conditional_t<kHasPayload, std::optional<PayloadType>, std::monostate>
              initialValue{};
      PayloadPtr initialValuePtr = nullptr;
      if constexpr (kHasPayload && !std::is_same_v<Updater, std::nullptr_t>) {
        updater(key, initialValue);
        if (initialValue) {
          initialValuePtr = &*initialValue;
        }
      }
      uint8_t insertionSlot = search.slot + 1;
      if (isTopLevel) {
        currLeaf->insertKeyAtSlotGuarded(insertionSlot, key, initialValuePtr);
      } else {
        LeafNode* newLeaf = preallocatedNodes.template takeNext<LeafNode>();
        insertPromotedSibling(
            currLeaf,
            newLeaf,
            [&](LeafNode* sibling) {
              sibling->insertKeyAtSlotRaw(
                  /* slot=0: promoted key */ 0, key, initialValuePtr);
            },
            [&](LeafNode* curr, LeafNode* sibling) {
              // destSlot=1: slot 0 reserved for the new key.
              curr->splitKeys(sibling, insertionSlot, /* destSlot */ 1);
            });
      }
    }
    std::unique_lock<folly::RWSpinLock> insertedLeafLock(
        lockedLeaf->mutex_, std::adopt_lock);
    size_.fetch_add(1);
    return BSkipInsertOutcome::Inserted;
  }

  static constexpr uint8_t topLevelForKey(const T& k) {
    constexpr uint64_t kFibonacciMix = 0x9E3779B97F4A7C15ULL;
    constexpr int kBitsPerDigit = __builtin_ctz(kPromotionProbInverse);
    const uint64_t hashMixed = static_cast<uint64_t>(Hash{}(k)) * kFibonacciMix;
    if (hashMixed == 0) [[unlikely]] {
      return kMaxHeight - 1;
    }
    const int firstNonZeroDigitIndex =
        __builtin_ctzll(hashMixed) / kBitsPerDigit;
    return static_cast<uint8_t>(
        std::min(firstNonZeroDigitIndex, static_cast<int>(kMaxHeight - 1)));
  }

  template <
      bskip_detail::InsertSemantics kSemantics,
      typename Updater = std::nullptr_t>
  BSkipInsertOutcome insertOrUpdateAtLeaf(
      LeafNode* leaf, uint8_t exactMatchSlot, const T& key, Updater updater) {
    std::unique_lock guard(leaf->mutex_, std::adopt_lock);

    if (leaf->tombstoned(exactMatchSlot)) {
      reviveLeafSlot(leaf, exactMatchSlot, key, updater);
      size_.fetch_add(1);
      return BSkipInsertOutcome::Revived;
    }

    // Live slot: Add → AlreadyPresent (no updater); AddOrUpdate → Updated.
    if constexpr (
        kSemantics == bskip_detail::InsertSemantics::AddOrUpdate &&
        !std::is_same_v<Updater, std::nullptr_t>) {
      addOrUpdateLeafSlot(leaf, exactMatchSlot, updater);
      return BSkipInsertOutcome::Updated;
    }

    return BSkipInsertOutcome::AlreadyPresent;
  }

  // The search key matched a routing key at this internal level. The matching
  // leaf is at children_[slot]→...→slot 0. Descend directly to it.
  template <
      bskip_detail::InsertSemantics kSemantics,
      typename Updater = std::nullptr_t>
  BSkipInsertOutcome insertOrUpdatePromoted(
      Node* currNode,
      uint8_t level,
      uint8_t slot,
      const T& key,
      uint8_t topLevel,
      Updater updater = nullptr) {
    LeafNode* leaf = (topLevel >= level)
        ? descendPromotedToLeaf<HeldNodeLockMode::Exclusive>(currNode, slot)
        : descendPromotedToLeaf<HeldNodeLockMode::Shared>(currNode, slot);
    return insertOrUpdateAtLeaf<kSemantics>(
        leaf, /* slot=0: promoted key */ 0, key, updater);
  }

  // After the firstSlot pick, follows child 0 down to the leaf (where
  // the matching key sits in slot 0). Returned leaf is exclusive-locked.
  template <HeldNodeLockMode firstNodeLockMode>
  LeafNode* descendPromotedToLeaf(Node* currNode, uint8_t firstSlot) {
    auto fetchAndLockLeafChild = [&](Node* parent, uint8_t slot) {
      auto* parentInternal = static_cast<InternalNode*>(parent);
      auto* child = static_cast<Node*>(parentInternal->children_[slot]);
      auto* leaf = static_cast<LeafNode*>(child);
      lockNode<HeldNodeLockMode::Exclusive>(leaf);
      return leaf;
    };

    auto childAtSlot = [](Node* parent, uint8_t slot) -> Node* {
      auto* parentInternal = static_cast<InternalNode*>(parent);
      return static_cast<Node*>(parentInternal->children_[slot]);
    };

    if (currNode->level_ == 1) {
      LeafNode* leaf = fetchAndLockLeafChild(currNode, firstSlot);
      unlockNode<firstNodeLockMode>(currNode);
      return leaf;
    }

    // First descent step: held in firstNodeLockMode.
    Node* nextNode = childAtSlot(currNode, firstSlot);
    lockNode<HeldNodeLockMode::Shared>(nextNode);
    unlockNode<firstNodeLockMode>(currNode);
    currNode = nextNode;

    // Continued descent: every node held Shared, descending child-0.
    while (currNode->level_ > 1) {
      nextNode = childAtSlot(currNode, /* slot=0: promoted key */ 0);
      lockNode<HeldNodeLockMode::Shared>(nextNode);
      unlockNode<HeldNodeLockMode::Shared>(currNode);
      currNode = nextNode;
    }

    LeafNode* leaf =
        fetchAndLockLeafChild(currNode, /* slot=0: promoted key */ 0);
    unlockNode<HeldNodeLockMode::Shared>(currNode);
    return leaf;
  }

  // ---------------------------------------------------------------------------
  // Split, publish, and height growth
  // ---------------------------------------------------------------------------

  template <typename NodeType>
  FOLLY_ALWAYS_INLINE void beginSplitSiblingWrite(
      NodeType* currNode, NodeType* newNode) {
    newNode->next_.store(
        currNode->next_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    newNode->nextMinKey_ = currNode->loadNextMinKey();
    newNode->level_ = currNode->level_;
    newNode->seq_.beginWrite();
  }

  // Both mutexes held, both write epochs open, newNode fully initialized.
  // Order: close newNode → fence → store nextMinKey_ → release-store next_
  // → close currNode. Reader pairs acquire-load(next_) with the release
  // store; the fence orders nextMinKey_ before next_ on weak memory.
  template <typename NodeType>
  FOLLY_ALWAYS_INLINE void publishSplitSibling(
      NodeType* currNode, NodeType* newNode) {
    newNode->seq_.endWrite();
    // Orders currNode->nextMinKey_ write before the next_ publish below.
    std::atomic_thread_fence(std::memory_order_release);
    currNode->nextMinKey_ = newNode->minKey();
    currNode->next_.store(newNode, std::memory_order_release);
    currNode->seq_.endWrite();
    bskip_detail::invokeBSkipTestHook(
        bskip_detail::BSkipTestHookEvent::SplitPublishComplete,
        std::memory_order_release,
        currNode,
        newNode);
  }

  // splitOp(current, newNode, splitIndex) performs the type-specific key/child
  // movement. Returns leftSizeAfterSplit (= splitIndex, since splitKeys sets
  // current->numElements_ to splitIndex).
  template <typename NodeType, typename SplitOp>
  FOLLY_ALWAYS_INLINE uint8_t
  splitKeysAndPublish(NodeType* current, NodeType* newNode, SplitOp splitOp) {
    beginSplitSiblingWrite(current, newNode);
    uint8_t splitIndex = current->numElements_.load() / 2;
    current->seq_.beginWrite();
    splitOp(current, newNode, splitIndex);
    publishSplitSibling(current, newNode);
    return splitIndex;
  }

  // FOLLY_NOINLINE: cold full-node split; keeps insertImpl's hot path slim.
  // Adopts the keyRecipient's lock into parentLock; returns the descent
  // continuation node (predecessor child of the new key's slot in the
  // keyRecipient).
  FOLLY_NOINLINE Node* splitAndInsertInternal(
      InternalNode* currInternal,
      uint8_t slot,
      const T& key,
      PreallocatedNodes& preallocatedNodes,
      CarriedParentLock& parentLock) {
    InternalNode* newInternal =
        preallocatedNodes.template takeSibling<InternalNode>();

    uint8_t leftSizeAfterSplit = splitKeysAndPublish(
        currInternal,
        newInternal,
        [](InternalNode* curr, InternalNode* sibling, uint8_t splitIndex) {
          curr->splitKeysAndChildren(sibling, splitIndex, /* destSlot */ 0);
        });

    uint8_t insertionSlot = slot + 1;
    bool goesInLeftNode = (insertionSlot <= leftSizeAfterSplit);
    uint8_t localInsertionSlot = goesInLeftNode
        ? insertionSlot
        : static_cast<uint8_t>(insertionSlot - leftSizeAfterSplit);
    Node* promotedChild = preallocatedNodes.template peekNext<Node>();
    InternalNode* keyRecipient = goesInLeftNode ? currInternal : newInternal;
    InternalNode* nonRecipient = goesInLeftNode ? newInternal : currInternal;
    nonRecipient->mutex_.unlock();

    {
      auto guard = keyRecipient->seq_.writeGuard();
      insertInternalEntry(keyRecipient, localInsertionSlot, key, promotedChild);
    }
    // Continue descent through the predecessor child of the new key's slot.
    DCHECK_GT(localInsertionSlot, 0);
    parentLock.adoptExclusive(keyRecipient->mutex_);
    return static_cast<Node*>(keyRecipient->children_[localInsertionSlot - 1]);
  }

  // FOLLY_NOINLINE: same icache rationale as splitAndInsertInternal above.
  template <typename Updater>
  FOLLY_NOINLINE LeafNode* splitAndInsertLeaf(
      LeafNode* currLeaf,
      uint8_t predecessorSlot,
      const T& key,
      PreallocatedNodes& preallocatedNodes,
      Updater& updater) {
    LeafNode* newLeaf = preallocatedNodes.template takeSibling<LeafNode>();

    uint8_t leftSizeAfterSplit = splitKeysAndPublish(
        currLeaf,
        newLeaf,
        [](LeafNode* curr, LeafNode* sibling, uint8_t splitIndex) {
          // destSlot=0: overflow split, sibling starts empty.
          curr->splitKeys(sibling, splitIndex, /* destSlot */ 0);
        });

    uint8_t insertionSlot = predecessorSlot + 1;
    bool goesInLeftNode = (insertionSlot <= leftSizeAfterSplit);
    uint8_t localInsertionSlot = goesInLeftNode
        ? insertionSlot
        : static_cast<uint8_t>(insertionSlot - leftSizeAfterSplit);
    LeafNode* keyRecipient = goesInLeftNode ? currLeaf : newLeaf;
    LeafNode* nonRecipient = goesInLeftNode ? newLeaf : currLeaf;
    nonRecipient->mutex_.unlock();

    if constexpr (kHasPayload && !std::is_same_v<Updater, std::nullptr_t>) {
      std::optional<PayloadType> payload;
      // Updater MUST NOT throw: split is already published.
      updater(key, payload);
      keyRecipient->insertKeyAtSlotGuarded(
          localInsertionSlot, key, payload ? &*payload : nullptr);
    } else {
      keyRecipient->insertKeyAtSlotGuarded(localInsertionSlot, key, nullptr);
    }
    return keyRecipient;
  }

  // Caller holds currNode->mutex_ exclusive; newNode->mutex_ is held from
  // preallocateInsertPath. Opens seqlock write epochs on both nodes.
  template <typename NodeType, typename InitNewSibling, typename SplitCurrent>
  FOLLY_ALWAYS_INLINE void insertPromotedSibling(
      NodeType* currNode,
      NodeType* newNode,
      InitNewSibling initNewSibling,
      SplitCurrent splitCurrent) {
    beginSplitSiblingWrite(currNode, newNode);
    initNewSibling(newNode);
    currNode->seq_.beginWrite();
    splitCurrent(currNode, newNode);
    publishSplitSibling(currNode, newNode);
    newNode->mutex_.unlock();
  }

  void ensureHeightAbove(uint8_t level) {
    while (true) {
      uint8_t height = height_.load(std::memory_order_acquire);
      if (height > level) {
        return;
      }
      // Allocate before growthLock_ (a spin lock) — jemalloc may take arena
      // locks or page-fault. Cost is an extra dealloc on lost races.
      // The init is sequenced-before the release stores on headers_/
      // height_ below, so readers see a fully-initialized sentinel.
      InternalNode* sentinel = allocInternal();
      sentinel->level_ = height;
      sentinel->numElements_.store(1);
      sentinel->keys_[0] = Traits::kMinSentinel;
      sentinel->nextMinKey_ = Traits::kMaxSentinel;
      sentinel->children_[0] =
          headers_[height - 1].load(std::memory_order_relaxed);

      bool published = false;
      {
        std::lock_guard<folly::MicroLock> lock(growthLock_);
        if (height_.load(std::memory_order_acquire) == height) {
          // Publish order: header before height. Readers acquire
          // height_ first, so they see headers_[N] only after it is
          // set.
          headers_[height].store(sentinel, std::memory_order_release);
          height_.store(height + 1, std::memory_order_release);
          published = true;
        }
      }
      if (!published) {
        // Lost race to another writer that grew this level (or beyond).
        // Drop the alloc and re-check the loop condition.
        deallocInternal(sentinel);
      }
    }
  }

  // Write-once per level: each slot is set by ensureHeightAbove, then
  // immutable.
  std::array<std::atomic<Node*>, kMaxHeight> headers_{};
  std::atomic<uint8_t> height_{1};
  // MicroLock (1B): growth is rare; a parking mutex would be overkill.
  folly::MicroLock growthLock_;
  // Inline sentinel avoids a jemalloc per empty container.
  LeafNode beginLeaf_;
  [[no_unique_address]] bskip_detail::BSkipAllocator<NodeAlloc> alloc_;
  folly::relaxed_atomic<size_t> size_;
};

} // namespace folly

namespace folly {

// ---------------------------------------------------------------------------
// Skipper — forward-only cursor for ConcurrentBSkipList. Defined out-of-line
// because it depends on the full ConcurrentBSkipList class above.
// ---------------------------------------------------------------------------

template <
    typename T,
    typename PayloadType,
    int B,
    KeyReadPolicy ReadPolicy,
    LeafStoragePolicy StoragePolicy,
    typename Policy>
class ConcurrentBSkipList<
    T,
    PayloadType,
    B,
    ReadPolicy,
    StoragePolicy,
    Policy>::Skipper {
  using List =
      ConcurrentBSkipList<T, PayloadType, B, ReadPolicy, StoragePolicy, Policy>;
  using Traits = typename List::Traits;
  using Node = typename List::Node;
  using LeafNode = typename List::LeafNode;
  using InternalNode = typename List::InternalNode;
  using CarriedParentLock = typename List::CarriedParentLock;
  using HeldNodeLockMode = typename List::HeldNodeLockMode;
  using NextEdge = typename List::NextEdge;
  static constexpr uint8_t kMaxHeight = Policy::kMaxHeight;

 public:
  explicit Skipper(const List& list) : list_(list) {
    // cachedPath_ entries are advisory; every optimistic path revalidates.
    cachedHeight_ = list_.height_.load(std::memory_order_acquire);
    currentLeaf_ = static_cast<LeafNode*>(
        list_.headers_[0].load(std::memory_order_relaxed));
    for (uint8_t level = 1; level < cachedHeight_; ++level) {
      cachedPath_[level].node =
          list_.headers_[level].load(std::memory_order_relaxed);
    }
    // Single acquire fence pairs with release stores in ensureHeightAbove,
    // avoiding per-element acquire on weak-memory archs.
    std::atomic_thread_fence(std::memory_order_acquire);

    DCHECK_GE(currentLeaf_->numElements_.load(), 1u);
    initCursor(currentLeaf_);
  }

  // Copying a skipper copies its current position and advisory descent cache.
  // The copied node pointers are safe because nodes live with the list; every
  // optimistic use still revalidates and falls back to locked descent if stale.
  Skipper(const Skipper&) = default;
  Skipper(Skipper&&) = default;
  ~Skipper() = default;
  Skipper& operator=(const Skipper&) = delete;
  Skipper& operator=(Skipper&&) = delete;

  FOLLY_ALWAYS_INLINE bool good() const { return currentLeaf_ != nullptr; }

  // LookupKey allows heterogeneous lookup without materializing T.
  template <typename LookupKey>
  std::optional<T> skipTo(const LookupKey& target) {
    if (!good()) {
      return std::nullopt;
    }
    if (!Traits::less(lastKey_, target)) {
      return lastKey_;
    }

    if constexpr (!Traits::kOptimistic) {
      return skipToLockedFromRoot(target);
    } else {
      LeafNode* leaf = currentLeaf_;
      // Leaf-range heuristic: target past the current leaf → skip the
      // current-leaf scan/retry. Stale cache only wastes one extra
      // traversal, never wrong-key.
      if (cachedNextMinKeyValid_ && !Traits::less(target, cachedNextMinKey_)) {
        return scanNextLeafOrDescend(leaf, target);
      }
      if (auto hit = scanCurrentLeaf(leaf, target)) {
        return hit;
      }
      if (auto hit = rescanCurrentLeaf(leaf, target)) {
        return hit;
      }
      return scanNextLeafOrDescend(leaf, target);
    }
  }

  std::optional<T> advance() {
    // kMaxKeys < 255 so leafSlot_ + 1 never wraps in uint8_t arithmetic.
    static_assert(Traits::kMaxKeys < 255);
    if (!good()) {
      return std::nullopt;
    }
    if constexpr (!Traits::kOptimistic) {
      return advanceLocked();
    } else {
      LeafNode* leaf = currentLeaf_;
      if (auto hit = scanLeafOptimistic(leaf, leafSlot_ + 1, [](const T&) {
            return true;
          })) {
        if (Traits::less(lastKey_, hit->key)) {
          cacheLeafHit(leaf, std::move(*hit));
          return lastKey_;
        }
      }
      return advanceLocked();
    }
  }

  std::optional<T> current() const {
    if (!good()) {
      return std::nullopt;
    }
    return lastKey_;
  }

  const auto& currPayload() const
    requires(List::kHasPayload)
  {
    DCHECK(lastPayload_.has_value());
    return *lastPayload_;
  }

  PayloadType peekCurrentPayload() const
    requires(List::kHasPayload)
  {
    DCHECK(currentLeaf_ != nullptr);
    return currentLeaf_->loadPayload(leafSlot_);
  }

  LeafNode* currentLeaf() const { return currentLeaf_; }
  uint8_t currentSlot() const { return leafSlot_; }

 private:
  // skipTo escalation order (each step falls through on failure):
  //   1. scanCurrentLeaf        — scan from current position (hot path)
  //   2. rescanCurrentLeaf      — re-check leaf range, retry scan
  //   3. scanAdjacentLeaf       — try the next sibling leaf
  //   4. optimisticDescend      — descend from cached internal nodes
  //   5. skipToLockedFromRoot   — full locked descent (always succeeds)
  //
  // Steps 1-4 are lock-free and may bail on concurrent writes.
  // Step 5 guarantees progress by taking shared locks.

  // scanAdjacentLeaf outcome.
  struct AdjacentProbeResult {
    enum class Kind { Hit, Miss, FallbackToLocked };
    Kind kind;
    std::optional<T> hit;
  };

  struct LeafHit {
    uint8_t slot = 0;
    T key{};
    [[no_unique_address]]
    std::conditional_t<List::kHasPayload, PayloadType, bskip_detail::Empty>
        payload{};
  };

  // Used both as in-flight OLC descent state and as cachedPath_ entries.
  // version=kInvalidVersion means "node is seeded but version was not
  // captured under a fresh readBegin()" — guaranteed to fail validate()
  // because Seqlock's beginWrite DCHECKs version_ never reaches UINT32_MAX.
  static constexpr uint32_t kInvalidVersion =
      std::numeric_limits<uint32_t>::max();
  struct SeqlockedNode {
    Node* node = nullptr;
    uint32_t version = kInvalidVersion;
    T nextMinKey{};
  };

  void initCursor(LeafNode* leaf) {
    // Sentinel at slot 0 is tombstoned; scans skip it naturally.
    if constexpr (Traits::kOptimistic) {
      if (auto hit = scanLeafOptimistic(
              leaf,
              /* startSlot=0: sentinel is tombstoned */ 0,
              [](const T&) { return true; },
              bskip_detail::BSkipTuning::kSeqlockRetries)) {
        cacheLeafHit(leaf, std::move(*hit));
        return;
      }
    }

    leaf->mutex_.lock_shared();
    scanLockedLeafChain(
        leaf, /* startSlot=0: sentinel is tombstoned */ 0, [](const T&) {
          return true;
        });
  }

  // Scan helpers; never mutate cursor state. Locked variants require
  // the caller to hold the shared lock. Lower-bound callers pass
  // `[&](const T& key) { return !Traits::less(key, target); }`.
  template <typename Match>
  std::optional<LeafHit> scanLeafOptimistic(
      LeafNode* leaf, uint8_t startSlot, const Match& match, int attempts = 1)
      const {
    static_assert(Traits::kOptimistic);
    for (int retry = 0; retry < attempts; ++retry) {
      auto guard = leaf->adaptiveRead();
      auto validate = [&] { return guard.valid(); };
      const uint8_t n = leaf->numElements_.load();
      std::optional<LeafHit> hit;
      bool valid = true;
      for (uint8_t i = startSlot; i < n; ++i) {
        if (leaf->tombstoned(i)) {
          continue;
        }
        auto key = list_.loadLeafKeyValidated(leaf, i, validate);
        if (!key) {
          valid = false;
          break;
        }
        if (!match(*key)) {
          continue;
        }
        LeafHit candidate{i, std::move(*key)};
        if constexpr (List::kHasPayload) {
          auto payload = list_.loadLeafPayloadValidated(leaf, i, validate);
          if (!payload) {
            valid = false;
            break;
          }
          candidate.payload = std::move(*payload);
        }
        hit = std::move(candidate);
        break;
      }
      if (hit && valid && validate()) {
        return hit;
      }
    }
    return std::nullopt;
  }

  // Single publication point for cursor-visible state. Monotonic: lastKey_
  // never decreases.
  FOLLY_ALWAYS_INLINE void cacheLeafHit(LeafNode* leaf, LeafHit&& hit) {
    DCHECK(!Traits::less(hit.key, lastKey_)) << "Skipper monotonicity violated";
    currentLeaf_ = leaf;
    leafSlot_ = hit.slot;
    // Unvalidated: stale read wastes one round-trip, never wrong-key.
    // OLC / locked fallbacks redo descent under fresh validation.
    cachedNextMinKey_ = static_cast<T>(leaf->nextMinKey_);
    cachedNextMinKeyValid_ = true;
    if constexpr (List::kHasPayload) {
      lastPayload_ = hit.payload;
    }
    lastKey_ = std::move(hit.key);
  }

  FOLLY_ALWAYS_INLINE Node* loadNextPointer(
      Node* node, bskip_detail::BSkipTestHookEvent event) const {
    Node* next = node->next_.load(std::memory_order_acquire);
    bskip_detail::invokeBSkipTestHook(
        event, std::memory_order_acquire, node, next);
    return next;
  }

  template <typename LookupKey>
  FOLLY_ALWAYS_INLINE std::optional<T> scanCurrentLeaf(
      LeafNode* leaf, const LookupKey& target) {
    // target > lastKey_ here, so leafSlot_ is guaranteed less; start at +1.
    // (advance()'s static_assert keeps leafSlot_+1 non-wrapping.)
    if (auto hit = scanLeafOptimistic(
            leaf, static_cast<uint8_t>(leafSlot_ + 1), [&](const T& key) {
              return !Traits::less(key, target);
            })) {
      cacheLeafHit(leaf, std::move(*hit));
      return lastKey_;
    }
    return std::nullopt;
  }

  // FOLLY_NOINLINE: cold retry path; keeps skipTo()'s icache footprint small.
  template <typename LookupKey>
  FOLLY_NOINLINE std::optional<T> rescanCurrentLeaf(
      LeafNode* leaf, const LookupKey& target) {
    {
      // Bracket: confirm the current leaf still owns the target's range
      // before paying for a full re-scan.
      auto guard = leaf->adaptiveRead();
      auto nextMinKey = list_.loadNextMinKeyValidated(leaf, [&] {
        return guard.valid();
      });
      if (!nextMinKey || !Traits::less(target, *nextMinKey)) {
        return std::nullopt;
      }
    }
    if (auto hit = scanLeafOptimistic(
            leaf,
            leafSlot_,
            [&](const T& key) { return !Traits::less(key, target); },
            bskip_detail::BSkipTuning::kSeqlockRetries)) {
      cacheLeafHit(leaf, std::move(*hit));
      return lastKey_;
    }
    return std::nullopt;
  }

  // path[] is filled as a side effect; caller revalidates via
  // path validation.
  template <typename LookupKey>
  LeafNode* optimisticDescendOnce(
      const LookupKey& target,
      uint8_t startLevel,
      std::array<SeqlockedNode, kMaxHeight>& path) const {
    Node* current = cachedPath_[startLevel].node;
    if (current == nullptr) {
      uint8_t height = list_.height_.load(std::memory_order_acquire);
      startLevel = height - 1;
      current = list_.headers_[startLevel].load(std::memory_order_acquire);
    }

    for (uint8_t level = startLevel; level > 0; --level) {
      InternalNode* internal = static_cast<InternalNode*>(current);
      uint32_t version =
          internal->seq_
              .template readBegin<bskip_detail::BSkipTuning::kReadSpins>();
      if (version & 1) {
        return nullptr;
      }

      // Settled nextMinKey: cached upper bound for highestValidCacheLevel.
      T settledNextMinKey{};
      while (true) {
        auto nextMinKey = list_.loadNextMinKeyValidated(internal, [&] {
          return internal->seq_.validate(version);
        });
        if (!nextMinKey) {
          return nullptr;
        }
        Node* nextRaw = loadNextPointer(
            internal, bskip_detail::BSkipTestHookEvent::OlcLoadNext);
        if (!internal->seq_.validate(version)) {
          return nullptr;
        }
        if (Traits::less(target, *nextMinKey) || nextRaw == nullptr) {
          settledNextMinKey = std::move(*nextMinKey);
          break;
        }
        internal = static_cast<InternalNode*>(nextRaw);
        version =
            internal->seq_
                .template readBegin<bskip_detail::BSkipTuning::kReadSpins>();
        if (version & 1) {
          return nullptr;
        }
      }

      std::optional<uint8_t> slot =
          findChildLockfree(internal, target, version);
      if (!slot) {
        return nullptr;
      }

      path[level] = {internal, version, std::move(settledNextMinKey)};
      // Unvalidated load — path validation catches a racing split. Safe
      // to dereference because nodes are never reclaimed.
      current = static_cast<Node*>(internal->children_[*slot]);
      if (current == nullptr) {
        return nullptr;
      }
    }

    return static_cast<LeafNode*>(current);
  }

  // Subsequent good()/current()/advance()/skipTo() observe nullopt.
  void markExhausted() {
    currentLeaf_ = nullptr;
    for (uint8_t level = 1; level < kMaxHeight; ++level) {
      cachedPath_[level].node = nullptr;
    }
    leafSlot_ = 0;
    // Defensive: avoid stale cache if a future path re-arms currentLeaf_.
    cachedNextMinKeyValid_ = false;
  }

  // Caller passes leaf locked shared; ownership transferred — leaf is unlocked
  // on every exit path. Hit publishes cursor; exhaustion calls markExhausted.
  template <typename Match>
  std::optional<T> scanLockedLeafChain(
      LeafNode* leaf, uint8_t startSlot, Match&& match) {
    while (true) {
      // Scan current leaf under shared lock.
      const uint8_t n = leaf->numElements_.load();
      for (uint8_t i = startSlot; i < n; ++i) {
        if (leaf->tombstoned(i)) {
          continue;
        }
        T key = leaf->loadKey(i);
        if (!match(key)) {
          continue;
        }
        LeafHit hit{i, std::move(key)};
        if constexpr (List::kHasPayload) {
          hit.payload = leaf->loadPayload(i);
        }
        leaf->mutex_.unlock_shared();
        cacheLeafHit(leaf, std::move(hit));
        return lastKey_;
      }
      // Hand off to next leaf.
      Node* nextRaw = leaf->next_.load(std::memory_order_relaxed);
      if (nextRaw == nullptr) {
        leaf->mutex_.unlock_shared();
        markExhausted();
        return std::nullopt;
      }
      auto* nextLeaf = static_cast<LeafNode*>(nextRaw);
      nextLeaf->mutex_.lock_shared();
      leaf->mutex_.unlock_shared();
      leaf = nextLeaf;
      startSlot = 0;
    }
  }

  // Probe only the immediate sibling. Publishing requires both leaves AND
  // leaf->next to stay valid until publish. NOINLINE: cold path.
  template <typename LookupKey>
  FOLLY_NOINLINE AdjacentProbeResult
  scanAdjacentLeaf(LeafNode* leaf, const LookupKey& target) {
    using Kind = typename AdjacentProbeResult::Kind;
    auto currentGuard = leaf->adaptiveRead();

    auto currentNextMinKey = list_.loadNextMinKeyValidated(leaf, [&] {
      return currentGuard.valid();
    });
    if (!currentNextMinKey) {
      return {Kind::FallbackToLocked, std::nullopt};
    }
    if (Traits::less(target, *currentNextMinKey)) {
      return {Kind::Miss, std::nullopt};
    }

    // Load 1/3: initial routing.
    Node* nextRaw = loadNextPointer(
        leaf, bskip_detail::BSkipTestHookEvent::ProbeNextLeafLoadNext);
    if (nextRaw == nullptr) {
      // List exhausted; locked path returns nullopt without an OLC descent.
      return {Kind::FallbackToLocked, std::nullopt};
    }
    LeafNode* nextLeaf = static_cast<LeafNode*>(nextRaw);
    auto nextGuard = nextLeaf->adaptiveRead();

    auto nextNextMinKey = list_.loadNextMinKeyValidated(nextLeaf, [&] {
      return nextGuard.valid() && currentGuard.valid();
    });
    // Load 2/3: confirm the routing target still equals the guarded leaf
    // (a racing split could have spliced in a new sibling).
    if (!nextNextMinKey ||
        loadNextPointer(
            leaf,
            bskip_detail::BSkipTestHookEvent::ProbeNextLeafValidateNext) !=
            nextLeaf) {
      return {Kind::FallbackToLocked, std::nullopt};
    }
    if (!Traits::less(target, *nextNextMinKey)) {
      return {Kind::Miss, std::nullopt};
    }

    // Per-slot validator: guard validity only. The next-pointer is checked
    // once before the loop and once after, so per-slot re-reads of
    // leaf->next_ (ARM: LDAR ×B) are unnecessary.
    auto validateNextLeaf = [&] {
      return nextGuard.valid() && currentGuard.valid();
    };
    std::optional<LeafHit> matched;
    {
      const uint8_t n = nextLeaf->numElements_.load();
      bool valid = true;
      for (uint8_t i = 0; i < n; ++i) {
        if (nextLeaf->tombstoned(i)) {
          continue;
        }
        auto key = list_.loadLeafKeyValidated(nextLeaf, i, validateNextLeaf);
        if (!key) {
          valid = false;
          break;
        }
        if (Traits::less(*key, target)) {
          continue;
        }
        LeafHit hit{i, std::move(*key)};
        if constexpr (List::kHasPayload) {
          auto payload =
              list_.loadLeafPayloadValidated(nextLeaf, i, validateNextLeaf);
          if (!payload) {
            valid = false;
            break;
          }
          hit.payload = std::move(*payload);
        }
        matched = std::move(hit);
        break;
      }
      if (!valid) {
        return {Kind::FallbackToLocked, std::nullopt};
      }
      if (!matched && !validateNextLeaf()) {
        return {Kind::FallbackToLocked, std::nullopt};
      }
      if (!matched) {
        return {Kind::Miss, std::nullopt};
      }
    }

    // Load 3/3: publish guard — both read guards plus leaf->next_ must still
    // hold so the matched slot is reachable from the new cursor position.
    if (!nextGuard.valid() || !currentGuard.valid() ||
        loadNextPointer(
            leaf,
            bskip_detail::BSkipTestHookEvent::ProbeNextLeafValidateNext) !=
            nextLeaf) {
      return {Kind::FallbackToLocked, std::nullopt};
    }
    currentGuard.releaseSharedLockEarly();
    nextGuard.releaseSharedLockEarly();
    cacheLeafHit(nextLeaf, std::move(*matched));
    return {Kind::Hit, lastKey_};
  }

  // scanAdjacentLeaf → optimisticDescend → skipToLockedFromRoot; called when
  // the leaf-range heuristic says the target is past the current leaf so the
  // current-leaf scan/retry would be wasted.
  template <typename LookupKey>
  FOLLY_NOINLINE std::optional<T> scanNextLeafOrDescend(
      LeafNode* leaf, const LookupKey& target) {
    DCHECK(leaf != nullptr)
        << "scanNextLeafOrDescend called on exhausted Skipper";
    static_assert(Traits::kOptimistic);

    AdjacentProbeResult adjacent = scanAdjacentLeaf(leaf, target);
    using Kind = typename AdjacentProbeResult::Kind;
    if (adjacent.kind == Kind::Hit) {
      return std::move(adjacent.hit);
    }
    if (adjacent.kind == Kind::FallbackToLocked) {
      return skipToLockedFromRoot(target);
    }
    // Miss: continue with OLC.
    return optimisticDescend(target, highestValidCacheLevel(target));
  }

  // FOLLY_NOINLINE: cold OLC fallback.
  template <typename LookupKey>
  FOLLY_NOINLINE std::optional<T> optimisticDescend(
      const LookupKey& target, uint8_t startLevel) {
    for (int retry = 0; retry < bskip_detail::BSkipTuning::kOlcAttempts;
         ++retry) {
      std::array<SeqlockedNode, kMaxHeight> path{};
      LeafNode* leaf = optimisticDescendOnce(target, startLevel, path);
      if (leaf == nullptr) {
        startLevel = kMaxHeight - 1;
        continue;
      }
      std::optional<LeafHit> hit = scanLeafOptimistic(
          leaf,
          /* startSlot=0: sentinel is tombstoned */ 0,
          [&](const T& key) { return !Traits::less(key, target); },
          bskip_detail::BSkipTuning::kSeqlockRetries + 1);

      if (!hit) {
        startLevel = kMaxHeight - 1;
        continue;
      }

      // Validate that no writer modified the internal nodes we descended
      // through.
      bool pathValid = true;
      for (uint8_t vl = 1; vl < kMaxHeight && path[vl].node != nullptr; ++vl) {
        if (!path[vl].node->seq_.validate(path[vl].version)) {
          pathValid = false;
          break;
        }
      }
      if (!pathValid) {
        startLevel = kMaxHeight - 1;
        continue;
      }

      for (uint8_t level = 1; level < kMaxHeight; ++level) {
        if (path[level].node != nullptr) {
          cachedPath_[level] = path[level];
        }
      }
      cacheLeafHit(leaf, std::move(*hit));
      return lastKey_;
    }

    return skipToLockedFromRoot(target);
  }

  // Guaranteed-progress fallback: full HOH descent with shared locks.
  // Used when optimistic attempts exhaust their retry budget or when a
  // concurrent split invalidates the cached path. Taking the shared lock
  // is cheaper than infinite spinning on a contended seqlock (profiled).
  template <typename LookupKey>
  std::optional<T> skipToLockedFromRoot(const LookupKey& target) {
    uint8_t h = list_.height_.load(std::memory_order_acquire);
    Node* current = list_.headers_[h - 1].load(std::memory_order_acquire);
    CarriedParentLock parentLock;

    for (uint8_t level = h; level-- > 0;) {
      current = list_.template hohAcquireAndCrabRight<
          HeldNodeLockMode::Shared,
          NextEdge::AllowNull>(current, target, parentLock);

      if (level > 0) {
        InternalNode* internal = static_cast<InternalNode*>(current);
        uint8_t slot = findChildLocked(internal, target);
        cachedPath_[level].node = internal;
        // Locked descent may have moved to a different node; invalidate the
        // cached version so highestValidCacheLevel doesn't reuse it.
        cachedPath_[level].version = kInvalidVersion;
        parentLock.adoptShared(internal->mutex_);
        current = static_cast<Node*>(internal->children_[slot]);
      } else {
        return scanLockedLeafChain(
            static_cast<LeafNode*>(current),
            /* startSlot=0: sentinel is tombstoned */ 0,
            [&](const T& key) { return !Traits::less(key, target); });
      }
    }
    return std::nullopt;
  }

  // Optimistic; nullopt on stale reads. Uses bare static_cast + manual
  // validate (vs loadValidated) because the bracketing loop needs to inspect
  // each loaded key, and the validation is batched.
  template <typename LookupKey>
  FOLLY_ALWAYS_INLINE std::optional<uint8_t> findChildLockfree(
      const InternalNode* internal,
      const LookupKey& target,
      uint32_t version) const {
    const uint8_t n = internal->numElements_.load();
    // Preallocated node not yet populated — bail, seqlock will catch it.
    if (n == 0) {
      return std::nullopt;
    }
    // Validate after loading n: the loop uses n as its bound, so we must
    // confirm n was read during the same epoch the caller sampled.
    if (!internal->seq_.validate(version)) {
      return std::nullopt;
    }
    // Keys are hw-atomic (static_assert in InternalTraits), so each load
    // returns a complete value (old or new, never torn). Calling less() on
    // a stale-but-complete value is safe for our key types (pure arithmetic
    // comparison, no pointer dereferences). A stale key may mis-route to
    // the wrong child, but validate() at return catches it.
    // TODO: validate before each less() call without performance penalty.
    // i=1: keys_[0] is the leftmost routing key; target >= keys_[0] is
    // guaranteed by the HOH descent, so the search starts at keys_[1].
    for (uint8_t i = 1; i < n; ++i) {
      T key = static_cast<T>(internal->keys_[i]);
      if (Traits::less(target, key)) {
        return internal->seq_.validate(version)
            ? std::optional<uint8_t>{static_cast<uint8_t>(i - 1)}
            : std::nullopt;
      }
    }
    return internal->seq_.validate(version)
        ? std::optional<uint8_t>{static_cast<uint8_t>(n - 1)}
        : std::nullopt;
  }

  template <typename LookupKey>
  uint8_t highestValidCacheLevel(const LookupKey& target) const {
    uint8_t startLevel = 0;
    // Cheap revalidation: cached {version, nextMinKey} per level lets us
    // bracket-check with one validate per level. Stale falls through to a
    // live load. cachedHeight_ caps reuse at construction-time height —
    // perf cap only; the locked fallback handles taller trees.
    for (uint8_t level = 1; level < cachedHeight_; ++level) {
      Node* node = cachedPath_[level].node;
      if (node == nullptr) {
        return startLevel;
      }
      // Sentinel version (kInvalidVersion) reliably fails validate() since
      // Seqlock's beginWrite DCHECKs version_ < UINT32_MAX-1.
      if (node->seq_.validate(cachedPath_[level].version)) {
        startLevel = level;
        if (Traits::less(target, cachedPath_[level].nextMinKey)) {
          return level;
        }
        continue;
      }
      // Stale or sentinel — issue a live load (advisory only).
      uint32_t v = node->seq_.template readBegin<0>();
      auto nextMinKey = list_.loadNextMinKeyValidated(node, [&] {
        return node->seq_.validate(v);
      });
      if (nextMinKey) {
        startLevel = level;
        if (Traits::less(target, *nextMinKey)) {
          return level;
        }
      }
    }
    return startLevel;
  }

  // Caller holds shared lock; no seqlock validation needed.
  template <typename LookupKey>
  FOLLY_ALWAYS_INLINE uint8_t
  findChildLocked(const InternalNode* internal, const LookupKey& target) const {
    const uint8_t n = internal->numElements_.load();
    uint8_t i = bskip_detail::findFirstGreaterLinear<Traits>(
        1, n, target, [&](uint8_t slot) {
          return static_cast<T>(internal->keys_[slot]);
        });
    return static_cast<uint8_t>(i - 1);
  }

  std::optional<T> advanceLocked() {
    DCHECK(currentLeaf_ != nullptr);
    LeafNode* leaf = currentLeaf_;
    leaf->mutex_.lock_shared();
    return scanLockedLeafChain(leaf, leafSlot_ + 1, [&](const T& key) {
      return Traits::less(lastKey_, key);
    });
  }

  static_assert(kMaxHeight <= 255, "cachedHeight_ is uint8_t");

  const List& list_;
  // currentLeaf_ = level-0 cursor; null = exhausted.
  // cachedPath_[level>=1] = last validated OLC descent state for cheap reuse.
  LeafNode* currentLeaf_{nullptr};
  std::array<SeqlockedNode, kMaxHeight> cachedPath_{};
  // Captured at construction; never refreshed (perf cap, not correctness).
  uint8_t cachedHeight_{0};
  uint8_t leafSlot_{0};
  // currentLeaf_'s upper bound, snapshotted at cacheLeafHit. Stale-but-safe:
  // skipTo's leaf-range heuristic only chooses which traversal to attempt.
  T cachedNextMinKey_{};
  bool cachedNextMinKeyValid_{false};
  T lastKey_{Traits::kMinSentinel};
  // optional<> for payload-bearing; Empty (zero-byte) for void payload.
  [[no_unique_address]] std::conditional_t<
      List::kHasPayload,
      std::optional<PayloadType>,
      bskip_detail::Empty> lastPayload_{};
};

template <
    typename T,
    typename PromotionHash,
    typename Compare,
    typename Allocator>
struct ConcurrentBSkipDefaultPolicy {
  using Hash = PromotionHash;
  using Comp = Compare;
  using NodeAlloc = Allocator;

  static constexpr uint8_t kLeafSlots = 16;
  static constexpr uint8_t kPromotionProbInverse = 16;
  static constexpr uint8_t kMaxHeight = 5;
  static constexpr KeyReadPolicy kReadPolicy =
      bskip_detail::kDefaultReadPolicy<T>;
  static constexpr LeafStoragePolicy kStoragePolicy =
      LeafStoragePolicy::Separate;
};

// Aliases forward Policy::k* into the per-axis params; pass a custom Policy
// to override Hash/Comp/Allocator/kMaxHeight/kPromotionProbInverse.
template <typename T, typename Policy = ConcurrentBSkipDefaultPolicy<T>>
using ConcurrentBSkipSet = ConcurrentBSkipList<
    T,
    void,
    Policy::kLeafSlots,
    Policy::kReadPolicy,
    LeafStoragePolicy::Separate,
    Policy>;

template <
    typename T,
    typename PayloadType,
    typename Policy = ConcurrentBSkipDefaultPolicy<T>>
using ConcurrentBSkipMap = ConcurrentBSkipList<
    T,
    PayloadType,
    Policy::kLeafSlots,
    Policy::kReadPolicy,
    LeafStoragePolicy::Separate,
    Policy>;

template <
    typename T,
    typename PayloadType,
    typename Policy = ConcurrentBSkipDefaultPolicy<T>>
using ConcurrentBSkipInlineMap = ConcurrentBSkipList<
    T,
    PayloadType,
    Policy::kLeafSlots,
    Policy::kReadPolicy,
    LeafStoragePolicy::Inline,
    Policy>;

} // namespace folly
