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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#define FOLLY_BSKIP_TEST_HOOKS 1
#include <folly/ConcurrentBSkipList.h>

#include <folly/ConcurrentSkipList.h>
#include <folly/Conv.h>
#include <folly/Random.h>
#include <folly/portability/GTest.h>

// LargeKey: 16-byte key type for testing KeyReadPolicy::Locked path.
struct LargeKey {
  int64_t a;
  int64_t b;

  bool operator==(const LargeKey& o) const { return a == o.a && b == o.b; }
};
static_assert(sizeof(LargeKey) > 8);

struct LargeKeyComp {
  bool operator()(const LargeKey& a, const LargeKey& b) const {
    return a.a < b.a || (a.a == b.a && a.b < b.b);
  }
};

// 16-byte DocId-shaped key used to exercise the versioned large-key policy in
// folly without depending on downstream Unicorn types.
struct VersionedKey {
  int64_t timestamp;
  int64_t id;

  bool operator==(const VersionedKey& o) const {
    return timestamp == o.timestamp && id == o.id;
  }
  bool operator!=(const VersionedKey& o) const { return !(*this == o); }
  bool operator<(const VersionedKey& o) const {
    return timestamp > o.timestamp || (timestamp == o.timestamp && id < o.id);
  }
};

static_assert(sizeof(VersionedKey) == 16);
static_assert(std::is_trivially_copyable_v<VersionedKey>);
static_assert(folly::bskip_detail::kIsHardwareAtomic<VersionedKey>);

// Non-trivial large key used to verify that locked-only read paths do not
// surface invalid key copies while concurrent updates keep taking the leaf
// write path. Ordering and uniqueness depend only on id.
struct CheckedNonTrivialKey {
  static inline std::atomic<int64_t> invalidCopies{0};
  static inline std::atomic<int64_t> invalidDestructions{0};

  int64_t id{};
  uint64_t generation{};
  uint64_t checksum{};

  CheckedNonTrivialKey() : CheckedNonTrivialKey(0, 0) {}

  explicit CheckedNonTrivialKey(int64_t id_, uint64_t generation_ = 0)
      : id(id_),
        generation(generation_),
        checksum(checksumFor(id_, generation_)) {}

  CheckedNonTrivialKey(const CheckedNonTrivialKey& other) { copyFrom(other); }

  CheckedNonTrivialKey& operator=(const CheckedNonTrivialKey& other) {
    if (this != &other) {
      copyFrom(other);
    }
    return *this;
  }

  CheckedNonTrivialKey(CheckedNonTrivialKey&& other) noexcept {
    copyFrom(other);
  }

  CheckedNonTrivialKey& operator=(CheckedNonTrivialKey&& other) noexcept {
    if (this != &other) {
      copyFrom(other);
    }
    return *this;
  }

  ~CheckedNonTrivialKey() {
    if (!valid()) {
      invalidDestructions.fetch_add(1, std::memory_order_relaxed);
    }
  }

  static uint64_t checksumFor(int64_t id, uint64_t generation) {
    constexpr uint64_t kSalt = 0x9e3779b97f4a7c15ULL;
    return (static_cast<uint64_t>(id) + kSalt) ^
        (generation * 0xbf58476d1ce4e5b9ULL);
  }

  static void resetCounters() {
    invalidCopies.store(0, std::memory_order_relaxed);
    invalidDestructions.store(0, std::memory_order_relaxed);
  }

  static void widenRaceWindow() {
    for (int i = 0; i < 128; ++i) {
      folly::asm_volatile_pause();
    }
  }

  void bumpGeneration() {
    ++generation;
    checksum = checksumFor(id, generation);
  }

  bool valid() const { return checksum == checksumFor(id, generation); }

  bool operator==(const CheckedNonTrivialKey& other) const {
    return id == other.id;
  }

 private:
  void copyFrom(const CheckedNonTrivialKey& other) {
    id = other.id;
    widenRaceWindow();
    generation = other.generation;
    widenRaceWindow();
    checksum = other.checksum;
    if (!valid()) {
      invalidCopies.fetch_add(1, std::memory_order_relaxed);
    }
  }
};

static_assert(sizeof(CheckedNonTrivialKey) > 8);
static_assert(!std::is_trivially_copyable_v<CheckedNonTrivialKey>);

struct CheckedNonTrivialKeyComp {
  bool operator()(
      const CheckedNonTrivialKey& a, const CheckedNonTrivialKey& b) const {
    return a.id < b.id;
  }
};

namespace std {
template <>
struct numeric_limits<LargeKey> {
  static constexpr bool is_specialized = true;
  static constexpr LargeKey min() {
    return {numeric_limits<int64_t>::min(), numeric_limits<int64_t>::min()};
  }
  static constexpr LargeKey max() {
    return {numeric_limits<int64_t>::max(), numeric_limits<int64_t>::max()};
  }
};
template <>
struct hash<LargeKey> {
  size_t operator()(const LargeKey& k) const {
    return hash<int64_t>{}(k.a) ^ (hash<int64_t>{}(k.b) << 1);
  }
};
template <>
struct numeric_limits<VersionedKey> {
  static constexpr bool is_specialized = true;
  static constexpr VersionedKey min() {
    return {numeric_limits<int64_t>::max(), numeric_limits<int64_t>::min()};
  }
  static constexpr VersionedKey max() {
    return {numeric_limits<int64_t>::min(), numeric_limits<int64_t>::max()};
  }
};
template <>
struct hash<VersionedKey> {
  size_t operator()(const VersionedKey& k) const {
    return hash<int64_t>{}(k.id) ^ (hash<int64_t>{}(k.timestamp) << 1);
  }
};
template <>
struct numeric_limits<CheckedNonTrivialKey> {
  static constexpr bool is_specialized = true;
  static CheckedNonTrivialKey min() {
    return CheckedNonTrivialKey{
        numeric_limits<int64_t>::min(),
        0,
    };
  }
  static CheckedNonTrivialKey max() {
    return CheckedNonTrivialKey{
        numeric_limits<int64_t>::max(),
        0,
    };
  }
};
template <>
struct hash<CheckedNonTrivialKey> {
  size_t operator()(const CheckedNonTrivialKey& k) const {
    return hash<int64_t>{}(k.id);
  }
};
} // namespace std

struct OptimisticLargeKeyHash {
  size_t operator()(const LargeKey& k) const {
    return std::hash<LargeKey>{}(k);
  }
};

struct OptimisticVersionedKeyHash {
  size_t operator()(const VersionedKey& k) const {
    return std::hash<VersionedKey>{}(k);
  }
};

namespace folly {
namespace {

// Tests vary B per-axis and override the always-defaulted
// kPromotionProbInverse/kMaxHeight via a thin policy struct so each
// instantiation reads as `TestList<KeyType, B, Prom, MH>` instead of a 6-arg
// template.
template <typename T, int Prom = 16, int MH = 5>
struct TestPolicy : ConcurrentBSkipDefaultPolicy<T> {
  static constexpr uint8_t kPromotionProbInverse = Prom;
  static constexpr uint8_t kMaxHeight = MH;
};

template <typename T, int B, int Prom = 16, int MH = 5>
using TestList = ConcurrentBSkipList<
    T,
    void,
    B,
    bskip_detail::kDefaultReadPolicy<T>,
    LeafStoragePolicy::Separate,
    TestPolicy<T, Prom, MH>>;

// Large-node config (B=64, P=2) — for stress testing with more keys per node
// and higher promotion probability.
using LargeNodeList = TestList<int64_t, 64, 2>;

// Default config (B=16, P=16)
using DefaultList = ConcurrentBSkipList<int64_t>;

using HookedAdjacentProbeList = TestList<int64_t, 6, 2, 1>;

using HookedOlcTraversalList = TestList<int64_t, 4, 2, 6>;

using HookedRelaxedAtomicList = TestList<uint32_t, 8, 2, 1>;

// Verify public type aliases and trait constants used by downstream code.
static_assert(
    std::is_same_v<
        DefaultList::leaf_node_type,
        bskip_detail::BSkipNodeLeaf<
            bskip_detail::
                InternalTraits<int64_t, std::less<int64_t>, 16, 16>>>);
static_assert(
    bskip_detail::InternalTraits<int64_t, std::less<int64_t>, 16, 16>::
        kPromotionProbInverse == 16);
static_assert(
    bskip_detail::InternalTraits<int64_t, std::less<int64_t>, 64, 2>::
        kPromotionProbInverse == 2);

template <class List, class Func>
size_t scanWithForEachElement(List& list, Func&& func) {
  return list.forEachElement(std::forward<Func>(func));
}

template <class List, class Func>
size_t scanWithSkipper(List& list, Func&& func) {
  size_t count = 0;
  typename List::Skipper skipper(list);
  for (auto pos = skipper.current(); pos; pos = skipper.advance()) {
    func(*pos);
    ++count;
  }
  return count;
}

// Insert keys [start, end) into `list` in increasing order. Used by tests
// that just need a populated list as a precondition; concurrency tests with
// specific insertion orders (reverse, shuffle) keep their own loops.
template <class List, class Key = int64_t>
void populateRange(List& list, Key start, Key end) {
  for (Key i = start; i < end; ++i) {
    list.add(i);
  }
}

// Assert every key in [start, end) is present in `list`.
template <class List, class Key = int64_t>
void expectAllPresent(List& list, Key start, Key end) {
  for (Key i = start; i < end; ++i) {
    EXPECT_TRUE(list.contains(i)) << "Key " << i << " should exist";
  }
}

// Assert every key in [start, end) is absent from `list`.
template <class List, class Key = int64_t>
void expectAllAbsent(List& list, Key start, Key end) {
  for (Key i = start; i < end; ++i) {
    EXPECT_FALSE(list.contains(i)) << "Key " << i << " should not exist";
  }
}

struct BSkipHookState {
  std::atomic<bool> sawProbeNextLoadAcquire{false};
  std::atomic<bool> sawProbeNextValidateAcquire{false};
  std::atomic<bool> sawOlcLoadAcquire{false};
  std::atomic<bool> sawLeafKeyPostLoadValidateAcquire{false};
  std::mutex mutex;
  std::condition_variable cv;
  bool blockOnSplitPublish{false};
  bool splitPublished{false};
  bool releaseSplitPublish{false};

  void onEvent(
      bskip_detail::BSkipTestHookEvent event,
      std::memory_order order,
      const void* /*node*/,
      const void* /*peer*/) {
    if (event == bskip_detail::BSkipTestHookEvent::ProbeNextLeafLoadNext &&
        order == std::memory_order_acquire) {
      sawProbeNextLoadAcquire.store(true, std::memory_order_relaxed);
    }
    if (event == bskip_detail::BSkipTestHookEvent::ProbeNextLeafValidateNext &&
        order == std::memory_order_acquire) {
      sawProbeNextValidateAcquire.store(true, std::memory_order_relaxed);
    }
    if (event == bskip_detail::BSkipTestHookEvent::OlcLoadNext &&
        order == std::memory_order_acquire) {
      sawOlcLoadAcquire.store(true, std::memory_order_relaxed);
    }
    if (event == bskip_detail::BSkipTestHookEvent::LeafKeyPostLoadValidate &&
        order == std::memory_order_acquire) {
      sawLeafKeyPostLoadValidateAcquire.store(true, std::memory_order_relaxed);
    }
    if (event != bskip_detail::BSkipTestHookEvent::SplitPublishComplete) {
      return;
    }
    std::unique_lock<std::mutex> lock(mutex);
    splitPublished = true;
    cv.notify_all();
    if (!blockOnSplitPublish) {
      return;
    }
    cv.wait(lock, [&] { return releaseSplitPublish; });
  }
};

std::atomic<BSkipHookState*> gBSkipActiveHookState{nullptr};

void bskipTestHook(
    bskip_detail::BSkipTestHookEvent event,
    std::memory_order order,
    const void* node,
    const void* peer) {
  if (auto* state = gBSkipActiveHookState.load(std::memory_order_relaxed)) {
    state->onEvent(event, order, node, peer);
  }
}

struct ScopedBSkipTestHook : folly::NonCopyableNonMovable {
  explicit ScopedBSkipTestHook(BSkipHookState& state) : state_(state) {
    gBSkipActiveHookState.store(&state_, std::memory_order_relaxed);
    bskip_detail::setBSkipTestHook(&bskipTestHook);
  }

  ~ScopedBSkipTestHook() {
    bskip_detail::setBSkipTestHook(nullptr);
    gBSkipActiveHookState.store(nullptr, std::memory_order_relaxed);
  }

 private:
  BSkipHookState& state_;
};

// ============================================================================
// Layout / sizeof guards
// ============================================================================
// These guards exist because BSkipNode layout is load-bearing for cache
// behavior (every traversal touches one or more nodes per leaf). A
// sub-cacheline node is the goal for narrow keys; growing it past a cacheline
// boundary on the production int64_t configuration would silently regress
// performance. If a layout change is intentional, update the expected size
// below — the test failure is the prompt to think about it, not a vote against
// the change.

TEST(ConcurrentBSkipList, BSkipNodeLayoutDefaultInt64) {
  // Default config: int64_t key (8B), B=16. Hot production target for DOCID
  // posting lists. The base node is ~32B (one half of a 64B cacheline).
  using LeafNode = DefaultList::leaf_node_type;
  EXPECT_EQ(sizeof(LeafNode) % alignof(LeafNode), 0u)
      << "BSkipNodeLeaf<int64,B=16> alignment invariant broken";
  EXPECT_LE(sizeof(LeafNode), 256u)
      << "BSkipNodeLeaf<int64,B=16> grew unexpectedly: actual="
      << sizeof(LeafNode);
}

TEST(ConcurrentBSkipList, BSkipNodeLayoutLargeNode) {
  // Large-node config: int64_t key, B=64. Stress test config; node size scales
  // linearly in B and the layout invariants must still hold.
  using LeafNode = LargeNodeList::leaf_node_type;
  EXPECT_EQ(sizeof(LeafNode) % alignof(LeafNode), 0u)
      << "BSkipNodeLeaf<int64,B=64> alignment invariant broken";
}

// ============================================================================
// Basic functionality tests
// ============================================================================

TEST(ConcurrentBSkipList, BasicInsertAndFind) {
  LargeNodeList list;
  populateRange(list, int64_t{0}, int64_t{10000});

  EXPECT_EQ(list.size(), 10000);
  expectAllPresent(list, int64_t{0}, int64_t{10000});
  expectAllAbsent(list, int64_t{10000}, int64_t{20000});
}

TEST(ConcurrentBSkipList, InsertReverseOrder) {
  LargeNodeList list;

  for (int64_t i = 9999; i >= 0; --i) {
    list.add(i);
  }

  EXPECT_EQ(list.size(), 10000);
  expectAllPresent(list, int64_t{0}, int64_t{10000});
}

TEST(ConcurrentBSkipList, InsertRandomOrder) {
  LargeNodeList list;

  std::vector<int64_t> keys;
  keys.reserve(10000);
  for (int64_t i = 0; i < 10000; ++i) {
    keys.push_back(i);
  }

  std::mt19937_64 rng(folly::Random::rand64());
  std::shuffle(keys.begin(), keys.end(), rng);

  for (auto k : keys) {
    list.add(k);
  }

  EXPECT_EQ(list.size(), 10000);
  expectAllPresent(list, int64_t{0}, int64_t{10000});
}

TEST(ConcurrentBSkipList, DuplicateInsert) {
  LargeNodeList list;

  populateRange(list, int64_t{0}, int64_t{1000});
  populateRange(list, int64_t{0}, int64_t{1000}); // Duplicates

  EXPECT_EQ(list.size(), 1000);
  expectAllPresent(list, int64_t{0}, int64_t{1000});
}

TEST(ConcurrentBSkipList, AddOrUpdateRevivesAndUpdatesExactMatch) {
  LargeNodeList list;
  int updaterCalls = 0;

  EXPECT_TRUE(list.add(int64_t(10)));
  EXPECT_TRUE(list.remove(int64_t(10)));
  EXPECT_TRUE(list.addOrUpdate(int64_t(10), [&](const int64_t& key) {
    ++updaterCalls;
    EXPECT_EQ(key, 10);
  }));
  EXPECT_TRUE(list.contains(int64_t(10)));
  EXPECT_EQ(updaterCalls, 0);

  EXPECT_FALSE(list.addOrUpdate(int64_t(10), [&](const int64_t& key) {
    ++updaterCalls;
    EXPECT_EQ(key, 10);
  }));
  EXPECT_TRUE(list.contains(int64_t(10)));
  EXPECT_EQ(updaterCalls, 1);
}

TEST(ConcurrentBSkipList, EmptyList) {
  LargeNodeList list;

  EXPECT_EQ(list.size(), 0);
  EXPECT_TRUE(list.empty());
  EXPECT_FALSE(list.contains(0));
  EXPECT_FALSE(list.contains(100));
}

TEST(ConcurrentBSkipList, SingleElement) {
  LargeNodeList list;

  list.add(int64_t(42));

  EXPECT_EQ(list.size(), 1);
  EXPECT_TRUE(list.contains(42));
  EXPECT_FALSE(list.contains(41));
  EXPECT_FALSE(list.contains(43));
}

TEST(ConcurrentBSkipList, StructureVerification) {
  LargeNodeList list;
  populateRange(list, int64_t{1}, int64_t{10001});

  EXPECT_EQ(list.size(), 10000);
  expectAllPresent(list, int64_t{1}, int64_t{10001});
}

// ============================================================================
// Sequential scan tests
// ============================================================================

TEST(ConcurrentBSkipList, SequentialScanBasic) {
  LargeNodeList list;

  for (int64_t i = 0; i < 100; ++i) {
    list.add(i);
  }

  std::vector<int64_t> visited;
  scanWithForEachElement(list, [&](int64_t key) { visited.push_back(key); });

  EXPECT_EQ(visited.size(), 100);
  for (size_t i = 0; i < visited.size(); ++i) {
    EXPECT_EQ(visited[i], static_cast<int64_t>(i));
  }
}

TEST(ConcurrentBSkipList, SequentialScanRandomInsert) {
  LargeNodeList list;

  std::vector<int64_t> keys;
  keys.reserve(1000);
  for (int64_t i = 0; i < 1000; ++i) {
    keys.push_back(i * 2);
  }

  std::mt19937_64 rng(folly::Random::rand64());
  std::shuffle(keys.begin(), keys.end(), rng);

  for (auto k : keys) {
    list.add(k);
  }

  std::vector<int64_t> result;
  scanWithForEachElement(list, [&](int64_t key) { result.push_back(key); });

  EXPECT_EQ(result.size(), 1000);
  for (size_t i = 0; i < result.size(); ++i) {
    EXPECT_EQ(result[i], static_cast<int64_t>(i * 2));
  }
}

TEST(ConcurrentBSkipList, SequentialScanEmptyList) {
  LargeNodeList list;
  size_t count = list.forEachElement([](int64_t) { FAIL(); });
  EXPECT_EQ(count, 0u);
}

TEST(ConcurrentBSkipList, SequentialScanAcrossMultipleNodes) {
  LargeNodeList list;

  // Insert enough keys to span many nodes
  for (int64_t i = 0; i < 1000; ++i) {
    list.add(i);
  }

  std::vector<int64_t> iterated;
  scanWithForEachElement(list, [&](int64_t key) { iterated.push_back(key); });

  EXPECT_EQ(iterated.size(), 1000);
  for (size_t i = 0; i < iterated.size(); ++i) {
    EXPECT_EQ(iterated[i], static_cast<int64_t>(i));
  }
}

TEST(ConcurrentBSkipList, ForEachElementSkipsHeadSentinel) {
  LargeNodeList list;

  for (int64_t i = 1; i <= 1000; ++i) {
    list.add(i);
  }

  std::vector<int64_t> iterated;
  scanWithForEachElement(list, [&](int64_t key) { iterated.push_back(key); });

  std::vector<int64_t> scanned;
  list.forEachElement([&](int64_t key) { scanned.push_back(key); });

  EXPECT_EQ(scanned, iterated);
}

TEST(ConcurrentBSkipList, ForEachElementSkipsTombstones) {
  LargeNodeList list;

  for (int64_t i = 1; i <= 100; ++i) {
    list.add(i);
  }
  for (int64_t i = 1; i <= 100; i += 2) {
    list.remove(i);
  }

  std::vector<int64_t> scanned;
  size_t count = list.forEachElement([&](int64_t key) {
    scanned.push_back(key);
  });

  EXPECT_EQ(count, 50u);
  EXPECT_EQ(scanned.size(), 50u);
  for (size_t j = 0; j < scanned.size(); ++j) {
    EXPECT_EQ(scanned[j], static_cast<int64_t>((j + 1) * 2));
  }
}

TEST(ConcurrentBSkipList, ForEachElementReturnValue) {
  DefaultList list;

  for (int64_t i = 1; i <= 500; ++i) {
    list.add(i);
  }
  for (int64_t i = 100; i <= 200; ++i) {
    list.remove(i);
  }

  size_t count = list.forEachElement([](int64_t) {});
  EXPECT_EQ(count, 399u);
  EXPECT_EQ(list.size(), 399u);
}

TEST(ConcurrentBSkipList, ForEachElementEmptyList) {
  LargeNodeList list;

  size_t count = list.forEachElement([](int64_t) { FAIL(); });
  EXPECT_EQ(count, 0u);
}

// ============================================================================
// Skipper tests
// ============================================================================

TEST(ConcurrentBSkipList, SkipperBasic) {
  LargeNodeList list;

  for (int64_t i = 0; i < 10000; i += 2) {
    list.add(i);
  }

  LargeNodeList::Skipper skipper(list);
  EXPECT_TRUE(skipper.good());
  {
    auto pos = skipper.current();
    ASSERT_TRUE(pos);
    EXPECT_EQ(*pos, 0);
  }

  {
    auto pos = skipper.skipTo(static_cast<int64_t>(100));
    ASSERT_TRUE(pos);
    EXPECT_EQ(*pos, 100);
  }

  {
    auto pos = skipper.skipTo(static_cast<int64_t>(101));
    ASSERT_TRUE(pos);
    EXPECT_EQ(*pos, 102);
  }

  {
    auto pos = skipper.skipTo(static_cast<int64_t>(9998));
    ASSERT_TRUE(pos);
    EXPECT_EQ(*pos, 9998);
  }

  {
    auto pos = skipper.skipTo(static_cast<int64_t>(10000));
    EXPECT_FALSE(pos);
  }
}

TEST(ConcurrentBSkipList, SkipperSequentialAccess) {
  LargeNodeList list;

  for (int64_t i = 0; i < 1000; ++i) {
    list.add(i);
  }

  LargeNodeList::Skipper skipper(list);
  int64_t count = 0;
  for (auto pos = skipper.current(); pos; pos = skipper.advance()) {
    EXPECT_EQ(*pos, count);
    ++count;
  }

  EXPECT_EQ(count, 1000);
}

TEST(ConcurrentBSkipList, SkipperCopyPreservesPosition) {
  LargeNodeList list;

  for (int64_t i = 0; i < 10; ++i) {
    list.add(i);
  }

  LargeNodeList::Skipper skipper(list);
  ASSERT_EQ(skipper.skipTo(static_cast<int64_t>(4)), 4);

  auto copy = skipper;
  EXPECT_EQ(copy.current(), 4);
  EXPECT_EQ(skipper.current(), 4);

  EXPECT_EQ(copy.advance(), 5);
  EXPECT_EQ(skipper.current(), 4);
  EXPECT_EQ(skipper.advance(), 5);
}

TEST(ConcurrentBSkipList, SkipperCopyAdvancesPastRemovedCurrent) {
  LargeNodeList list;

  for (int64_t i = 0; i < 10; ++i) {
    list.add(i);
  }

  LargeNodeList::Skipper skipper(list);
  ASSERT_EQ(skipper.skipTo(static_cast<int64_t>(4)), 4);

  auto copy = skipper;
  ASSERT_TRUE(list.remove(4));

  EXPECT_EQ(copy.current(), 4);
  EXPECT_EQ(copy.advance(), 5);
}

TEST(ConcurrentBSkipList, SkipperJumps) {
  LargeNodeList list;

  for (int64_t i = 0; i < 10000; ++i) {
    list.add(i);
  }

  LargeNodeList::Skipper skipper(list);

  for (int64_t target = 0; target < 10000; target += 1000) {
    auto pos = skipper.skipTo(target);
    ASSERT_TRUE(pos);
    EXPECT_EQ(*pos, target);
  }
}

TEST(ConcurrentBSkipList, SkipperEmptyList) {
  LargeNodeList list;

  LargeNodeList::Skipper skipper(list);
  EXPECT_FALSE(skipper.good());
  EXPECT_FALSE(skipper.skipTo(static_cast<int64_t>(0)));
}

TEST(ConcurrentBSkipList, SkipperAcrossNodeBoundaries) {
  LargeNodeList list;

  for (int64_t i = 0; i < 10000; i += 2) {
    list.add(i);
  }

  LargeNodeList::Skipper skipper(list);

  // Skip in steps of 100 (should cross many node boundaries)
  for (int64_t target = 0; target < 10000; target += 100) {
    auto pos = skipper.skipTo(target);
    ASSERT_TRUE(pos) << "skipTo(" << target << ") should succeed";
    EXPECT_EQ(*pos, target)
        << "skipTo(" << target << ") should find exact match";
  }
}

TEST(ConcurrentBSkipList, SkipperMixedCurrentAdvanceAndSkipTo) {
  LargeNodeList list;

  for (int64_t i = 0; i < 10000; ++i) {
    list.add(i);
  }

  LargeNodeList::Skipper skipper(list);

  auto pos = skipper.current();
  ASSERT_TRUE(pos);
  EXPECT_EQ(*pos, 0);

  pos = skipper.advance();
  ASSERT_TRUE(pos);
  EXPECT_EQ(*pos, 1);

  pos = skipper.skipTo(static_cast<int64_t>(127));
  ASSERT_TRUE(pos);
  EXPECT_EQ(*pos, 127);

  pos = skipper.current();
  ASSERT_TRUE(pos);
  EXPECT_EQ(*pos, 127);

  pos = skipper.advance();
  ASSERT_TRUE(pos);
  EXPECT_EQ(*pos, 128);

  pos = skipper.skipTo(static_cast<int64_t>(4096));
  ASSERT_TRUE(pos);
  EXPECT_EQ(*pos, 4096);

  pos = skipper.advance();
  ASSERT_TRUE(pos);
  EXPECT_EQ(*pos, 4097);
}

TEST(ConcurrentBSkipList, SkipperAdjacentLeafDeleteGapFallsThrough) {
  using SmallLeafList = TestList<int64_t, 4, 4>;
  SmallLeafList list;

  for (int64_t i = 0; i < 16; ++i) {
    list.add(i);
  }
  for (int64_t i = 4; i < 8; ++i) {
    list.remove(i);
  }

  SmallLeafList::Skipper skipper(list);
  auto pos = skipper.skipTo(static_cast<int64_t>(3));
  ASSERT_TRUE(pos);
  EXPECT_EQ(*pos, 3);

  pos = skipper.skipTo(static_cast<int64_t>(4));
  ASSERT_TRUE(pos);
  EXPECT_EQ(*pos, 8);
}

TEST(ConcurrentBSkipList, SkipperToEndAndBeyond) {
  LargeNodeList list;

  for (int64_t i = 0; i < 100; ++i) {
    list.add(i * 10);
  }

  LargeNodeList::Skipper skipper(list);

  auto pos = skipper.skipTo(static_cast<int64_t>(990));
  ASSERT_TRUE(pos);
  EXPECT_EQ(*pos, 990);

  pos = skipper.advance();
  EXPECT_FALSE(pos);
  EXPECT_FALSE(skipper.good());
}

// ============================================================================
// Concurrent tests
// ============================================================================

TEST(ConcurrentBSkipList, ConcurrentInsert) {
  LargeNodeList list;
  constexpr int kNumThreads = folly::kIsSanitizeThread ? 4 : 12;
  constexpr int kKeysPerThread = folly::kIsSanitizeThread ? 1000 : 10000;

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&list, t]() {
      for (int i = 0; i < kKeysPerThread; ++i) {
        list.add(static_cast<int64_t>(t * kKeysPerThread + i));
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(list.size(), kNumThreads * kKeysPerThread);

  for (int64_t i = 0; i < kNumThreads * kKeysPerThread; ++i) {
    EXPECT_TRUE(list.contains(i));
  }
}

TEST(ConcurrentBSkipList, ConcurrentRead) {
  LargeNodeList list;

  for (int64_t i = 0; i < 10000; ++i) {
    list.add(i);
  }

  constexpr int kNumThreads = 12;
  std::atomic<int> failCount{0};

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&list, &failCount]() {
      for (int64_t i = 0; i < 10000; ++i) {
        if (!list.contains(i)) {
          failCount.fetch_add(1);
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(failCount.load(), 0);
}

TEST(ConcurrentBSkipList, ConcurrentMixedReadWrite) {
  LargeNodeList list;

  // Pre-populate
  for (int64_t i = 0; i < 5000; ++i) {
    list.add(i);
  }

  constexpr int kWriterThreads = 4;
  constexpr int kReaderThreads = 8;
  std::atomic<bool> stop{false};
  std::atomic<int> readFails{0};

  std::vector<std::thread> threads;

  // Writers: insert new keys
  threads.reserve(kWriterThreads);
  for (int t = 0; t < kWriterThreads; ++t) {
    threads.emplace_back([&list, &stop, t]() {
      int64_t key = 5000 + t * 10000;
      while (!stop.load()) {
        list.add(key++);
      }
    });
  }

  // Readers: check existing keys
  threads.reserve(kReaderThreads);
  for (int t = 0; t < kReaderThreads; ++t) {
    threads.emplace_back([&list, &stop, &readFails]() {
      while (!stop.load()) {
        for (int64_t i = 0; i < 5000; ++i) {
          if (!list.contains(i)) {
            readFails.fetch_add(1);
          }
        }
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(readFails.load(), 0);
}

// ============================================================================
// Large-scale tests
// ============================================================================

TEST(ConcurrentBSkipList, LargeScale) {
  LargeNodeList list;

  constexpr int64_t kCount = folly::kIsSanitizeThread ? 10000 : 100000;

  for (int64_t i = 0; i < kCount; ++i) {
    list.add(i);
  }

  EXPECT_EQ(list.size(), kCount);

  for (int64_t i = 0; i < kCount; ++i) {
    EXPECT_TRUE(list.contains(i));
  }
}

TEST(ConcurrentBSkipList, LargeScaleRandom) {
  LargeNodeList list;

  constexpr int64_t kCount = folly::kIsSanitizeThread ? 10000 : 100000;

  std::vector<int64_t> keys;
  keys.reserve(kCount);
  for (int64_t i = 0; i < kCount; ++i) {
    keys.push_back(i);
  }

  std::mt19937_64 rng(folly::Random::rand64());
  std::shuffle(keys.begin(), keys.end(), rng);

  for (auto k : keys) {
    list.add(k);
  }

  EXPECT_EQ(list.size(), kCount);

  for (int64_t i = 0; i < kCount; ++i) {
    EXPECT_TRUE(list.contains(i)) << "Key " << i << " should exist";
  }
}

TEST(ConcurrentBSkipList, BoundaryValues) {
  LargeNodeList list;

  list.add(int64_t(0));
  list.add(int64_t(1));

  EXPECT_TRUE(list.contains(0));
  EXPECT_TRUE(list.contains(1));
  EXPECT_FALSE(list.contains(-1));
}

TEST(ConcurrentBSkipList, NegativeKeys) {
  LargeNodeList list;

  for (int64_t i = -5000; i < 5000; ++i) {
    list.add(i);
  }

  EXPECT_EQ(list.size(), 10000);

  for (int64_t i = -5000; i < 5000; ++i) {
    EXPECT_TRUE(list.contains(i));
  }
}

TEST(ConcurrentBSkipList, SparseKeys) {
  LargeNodeList list;

  // Very sparse keys
  for (int64_t i = 0; i < 1000; ++i) {
    list.add(i * 1000);
  }

  EXPECT_EQ(list.size(), 1000);

  for (int64_t i = 0; i < 1000; ++i) {
    EXPECT_TRUE(list.contains(i * 1000));
    EXPECT_FALSE(list.contains(i * 1000 + 1));
  }
}

TEST(ConcurrentBSkipList, NodeSplitBoundary) {
  LargeNodeList list;

  // Insert exactly B keys (should fit in one node)
  for (int64_t i = 0; i < 64; ++i) {
    list.add(i);
  }

  EXPECT_EQ(list.size(), 64);

  // Insert one more to trigger split
  list.add(int64_t(64));
  EXPECT_EQ(list.size(), 65);

  for (int64_t i = 0; i <= 64; ++i) {
    EXPECT_TRUE(list.contains(i));
  }
}

TEST(ConcurrentBSkipList, MultipleSplits) {
  LargeNodeList list;

  // Force many splits
  for (int64_t i = 0; i < 10000; ++i) {
    list.add(i);
    EXPECT_EQ(list.size(), static_cast<size_t>(i + 1));
  }
}

TEST(ConcurrentBSkipList, StressInsertAndVerify) {
  LargeNodeList list;

  constexpr int kCount = folly::kIsSanitizeThread ? 5000 : 50000;

  std::vector<int64_t> keys;
  keys.reserve(kCount);
  for (int64_t i = 0; i < kCount; ++i) {
    keys.push_back(i);
  }

  std::mt19937_64 rng(folly::Random::rand64());
  std::shuffle(keys.begin(), keys.end(), rng);

  for (auto k : keys) {
    EXPECT_TRUE(list.add(k));
  }

  EXPECT_EQ(list.size(), kCount);

  // Verify sorted full scan
  std::vector<int64_t> iterated;
  scanWithForEachElement(list, [&](int64_t key) { iterated.push_back(key); });

  EXPECT_EQ(iterated.size(), kCount);
  for (int64_t i = 0; i < kCount; ++i) {
    EXPECT_EQ(iterated[i], i);
  }
}

TEST(ConcurrentBSkipList, ConcurrentReadDuringWrite) {
  LargeNodeList list;

  for (int64_t i = 0; i < 5000; ++i) {
    list.add(i);
  }

  std::atomic<bool> stop{false};
  std::atomic<int> anomalies{0};

  // Under sanitizers, 45k split-heavy inserts (P=2) with continuous full
  // scans crawls toward the 600s timeout on loaded CI hosts; scale the
  // writer down. Non-sanitizer runs keep the full window.
  const int64_t kWriteEnd = folly::kIsSanitizeThread ? 15000 : 50000;

  std::thread writer([&]() {
    for (int64_t i = 5000; i < kWriteEnd && !stop.load(); ++i) {
      list.add(i);
    }
  });

  std::vector<std::thread> readers;
  readers.reserve(4);
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&]() {
      while (!stop.load()) {
        LargeNodeList::Skipper skipper(list);
        int64_t prev = -1;
        for (auto pos = skipper.current(); pos; pos = skipper.advance()) {
          if (*pos <= prev) {
            anomalies.fetch_add(1);
          }
          prev = *pos;
        }
      }
    });
  }

  writer.join();
  stop.store(true);
  for (auto& t : readers) {
    t.join();
  }

  EXPECT_EQ(anomalies.load(), 0);
}

// ============================================================================
// Delete tests
// ============================================================================

TEST(ConcurrentBSkipListDelete, BasicDelete) {
  LargeNodeList list;

  for (int64_t i = 0; i < 100; ++i) {
    list.add(i);
  }

  EXPECT_EQ(list.size(), 100);

  for (int64_t i = 0; i < 50; ++i) {
    EXPECT_TRUE(list.remove(i));
  }

  EXPECT_EQ(list.size(), 50);

  for (int64_t i = 0; i < 50; ++i) {
    EXPECT_FALSE(list.contains(i));
  }
  for (int64_t i = 50; i < 100; ++i) {
    EXPECT_TRUE(list.contains(i));
  }
}

TEST(ConcurrentBSkipListDelete, DeleteNonExistent) {
  LargeNodeList list;

  for (int64_t i = 0; i < 100; ++i) {
    list.add(i * 2);
  }

  for (int64_t i = 0; i < 100; ++i) {
    EXPECT_FALSE(list.remove(i * 2 + 1));
  }

  EXPECT_EQ(list.size(), 100);
}

TEST(ConcurrentBSkipListDelete, DeleteAll) {
  LargeNodeList list;

  for (int64_t i = 0; i < 1000; ++i) {
    list.add(i);
  }

  for (int64_t i = 0; i < 1000; ++i) {
    EXPECT_TRUE(list.remove(i));
  }

  EXPECT_EQ(list.size(), 0);

  for (int64_t i = 0; i < 1000; ++i) {
    EXPECT_FALSE(list.contains(i));
  }
}

TEST(ConcurrentBSkipListDelete, DeleteAndReinsert) {
  LargeNodeList list;

  for (int64_t i = 0; i < 1000; ++i) {
    list.add(i);
  }

  for (int64_t i = 0; i < 500; ++i) {
    EXPECT_TRUE(list.remove(i));
  }

  EXPECT_EQ(list.size(), 500);

  for (int64_t i = 0; i < 500; ++i) {
    EXPECT_TRUE(list.add(i));
  }

  EXPECT_EQ(list.size(), 1000);

  for (int64_t i = 0; i < 1000; ++i) {
    EXPECT_TRUE(list.contains(i));
  }
}

TEST(ConcurrentBSkipListDelete, DeleteRandomOrder) {
  LargeNodeList list;

  constexpr int kCount = 5000;
  std::vector<int64_t> keys;
  keys.reserve(kCount);
  for (int64_t i = 0; i < kCount; ++i) {
    keys.push_back(i);
    list.add(i);
  }

  // Shuffle keys for random deletion order
  std::mt19937_64 rng(folly::Random::rand64());
  std::shuffle(keys.begin(), keys.end(), rng);

  for (auto k : keys) {
    EXPECT_TRUE(list.remove(k)) << "Failed to remove key " << k;
  }

  EXPECT_EQ(list.size(), 0);
}

TEST(ConcurrentBSkipListDelete, DeleteDoubleFails) {
  LargeNodeList list;

  list.add(int64_t(42));
  EXPECT_TRUE(list.remove(42));
  EXPECT_FALSE(list.remove(42)); // already deleted
}

TEST(ConcurrentBSkipListDelete, SkipperAfterDelete) {
  LargeNodeList list;

  for (int64_t i = 0; i < 100; ++i) {
    list.add(i);
  }

  // Delete even keys
  for (int64_t i = 0; i < 100; i += 2) {
    list.remove(i);
  }

  // Skipper should only see odd keys
  LargeNodeList::Skipper skipper(list);
  int count = 0;
  for (auto pos = skipper.current(); pos; pos = skipper.advance()) {
    EXPECT_EQ(*pos % 2, 1) << "Found even key " << *pos << " after delete";
    ++count;
  }
  EXPECT_EQ(count, 50);
}

TEST(ConcurrentBSkipListDelete, ConcurrentDelete) {
  LargeNodeList list;

  constexpr int kCount = 10000;
  for (int64_t i = 0; i < kCount; ++i) {
    list.add(i);
  }

  constexpr int kNumThreads = 8;
  std::vector<std::thread> threads;

  threads.reserve(kNumThreads);
  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&list, t]() {
      for (int64_t i = t; i < kCount; i += kNumThreads) {
        list.remove(i);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(list.size(), 0);
}

TEST(ConcurrentBSkipListDelete, ConcurrentMixedInsertDelete) {
  LargeNodeList list;

  for (int64_t i = 0; i < 5000; ++i) {
    list.add(i);
  }

  std::atomic<bool> stop{false};

  std::vector<std::thread> threads;

  // Writers: insert new keys
  threads.reserve(4);
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&list, &stop, t]() {
      int64_t key = 5000 + t * 100000;
      while (!stop.load()) {
        list.add(key++);
      }
    });
  }

  // Deleters: delete some existing keys
  threads.reserve(4);
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&list, &stop, t]() {
      std::mt19937 rng(42 + t);
      while (!stop.load()) {
        int64_t key = rng() % 5000;
        list.remove(key);
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  for (auto& t : threads) {
    t.join();
  }

  // Just verify no crash and structure is valid
  size_t iterCount = list.forEachElement([](int64_t) {});
  EXPECT_EQ(iterCount, list.size());
}

// ============================================================================
// SkipListComparison — ConcurrentBSkipList vs folly::ConcurrentSkipList
// ============================================================================

TEST(SkipListComparison, SequentialInsertAndContains) {
  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  constexpr int kCount = 10000;

  for (int64_t i = 0; i < kCount; ++i) {
    bskip.add(i);
    follyAcc.add(i);
  }

  EXPECT_EQ(bskip.size(), static_cast<size_t>(kCount));
  EXPECT_EQ(follyAcc.size(), static_cast<size_t>(kCount));

  for (int64_t i = 0; i < kCount; ++i) {
    EXPECT_EQ(bskip.contains(i), follyAcc.contains(i))
        << "Mismatch at key " << i;
  }

  for (int64_t i = kCount; i < kCount + 1000; ++i) {
    EXPECT_EQ(bskip.contains(i), follyAcc.contains(i))
        << "Mismatch at absent key " << i;
  }
}

TEST(SkipListComparison, RandomInsertAndContains) {
  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  constexpr int kCount = 10000;

  std::vector<int64_t> keys;
  keys.reserve(kCount);
  for (int64_t i = 0; i < kCount; ++i) {
    keys.push_back(i);
  }

  std::mt19937_64 rng(folly::Random::rand64());
  std::shuffle(keys.begin(), keys.end(), rng);

  for (auto k : keys) {
    bskip.add(k);
    follyAcc.add(k);
  }

  EXPECT_EQ(bskip.size(), follyAcc.size());

  for (int64_t i = 0; i < kCount * 2; ++i) {
    EXPECT_EQ(bskip.contains(i), follyAcc.contains(i))
        << "Mismatch at key " << i;
  }
}

TEST(SkipListComparison, IterationOrder) {
  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  std::vector<int64_t> keys;
  keys.reserve(5000);
  for (int64_t i = 0; i < 5000; ++i) {
    keys.push_back(i);
  }

  std::mt19937_64 rng(folly::Random::rand64());
  std::shuffle(keys.begin(), keys.end(), rng);

  for (auto k : keys) {
    bskip.add(k);
    follyAcc.add(k);
  }

  std::vector<int64_t> bskipOrder;
  scanWithForEachElement(bskip, [&](int64_t key) {
    bskipOrder.push_back(key);
  });

  std::vector<int64_t> follyOrder(follyAcc.begin(), follyAcc.end());

  EXPECT_EQ(bskipOrder, follyOrder);
}

TEST(SkipListComparison, DuplicateInserts) {
  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  for (int64_t i = 0; i < 100; ++i) {
    bskip.add(i);
    follyAcc.add(i);
  }

  for (int64_t i = 0; i < 100; ++i) {
    bskip.add(i);
    follyAcc.add(i);
  }

  EXPECT_EQ(bskip.size(), follyAcc.size());
}

TEST(SkipListComparison, EmptyListBehavior) {
  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  EXPECT_EQ(bskip.size(), follyAcc.size());
  EXPECT_EQ(bskip.contains(0), follyAcc.contains(0));
  auto follyIt = follyAcc.begin();
  EXPECT_EQ(bskip.empty(), follyIt == follyAcc.end());
}

TEST(SkipListComparison, VerifyAgainstStdSet) {
  LargeNodeList bskip;
  std::set<int64_t> stdSet;

  constexpr int kCount = 10000;

  std::vector<int64_t> keys;
  keys.reserve(kCount);
  for (int64_t i = 0; i < kCount; ++i) {
    keys.push_back(i);
  }
  std::mt19937_64 rng(folly::Random::rand64());
  std::shuffle(keys.begin(), keys.end(), rng);

  for (auto k : keys) {
    bskip.add(k);
    stdSet.insert(k);
  }

  EXPECT_EQ(bskip.size(), stdSet.size());

  for (int64_t i = 0; i < kCount; ++i) {
    bool bskipHas = bskip.contains(i);
    bool setHas = stdSet.contains(i);
    EXPECT_EQ(bskipHas, setHas) << "Mismatch at key " << i;
  }

  std::vector<int64_t> bskipOrder;
  scanWithForEachElement(bskip, [&](int64_t key) {
    bskipOrder.push_back(key);
  });
  std::vector<int64_t> setOrder(stdSet.begin(), stdSet.end());
  EXPECT_EQ(bskipOrder, setOrder);
}

TEST(SkipListComparison, DeleteBehavior) {
  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  for (int64_t i = 0; i < 1000; ++i) {
    bskip.add(i);
    follyAcc.add(i);
  }

  for (int64_t i = 0; i < 500; ++i) {
    bskip.remove(i);
    follyAcc.remove(i);
  }

  EXPECT_EQ(bskip.size(), follyAcc.size());

  for (int64_t i = 0; i < 1000; ++i) {
    EXPECT_EQ(bskip.contains(i), follyAcc.contains(i))
        << "Mismatch at key " << i;
  }
}

TEST(SkipListComparison, InsertDeleteReinsert) {
  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  for (int64_t i = 0; i < 1000; ++i) {
    bskip.add(i);
    follyAcc.add(i);
  }

  for (int64_t i = 0; i < 500; ++i) {
    bskip.remove(i);
    follyAcc.remove(i);
  }

  for (int64_t i = 0; i < 500; ++i) {
    bskip.add(i);
    follyAcc.add(i);
  }

  EXPECT_EQ(bskip.size(), follyAcc.size());

  for (int64_t i = 0; i < 1000; ++i) {
    EXPECT_EQ(bskip.contains(i), follyAcc.contains(i));
  }
}

// ============================================================================
// Skipper Parity Tests — folly::ConcurrentSkipList vs ConcurrentBSkipList
//
// These tests feed identical operation streams to both implementations and
// verify they produce identical outputs. Single-threaded / deterministic.
// ============================================================================

namespace {

// Helper: get the position value from folly Skipper after to().
// Folly's to() positions succs_[0] at the lower_bound regardless of return
// value. We access it via data()/operator*.
// Returns nullopt if !good() (past end).
std::optional<int64_t> follySkipperPos(
    folly::ConcurrentSkipList<int64_t>::Accessor::Skipper& s) {
  if (!s.good()) {
    return std::nullopt;
  }
  return s.data();
}

template <typename T>
std::string optionalToString(const std::optional<T>& value) {
  return value ? folly::to<std::string>(*value) : "nullopt";
}

} // namespace

TEST(SkipperParity, ForwardSkipToBasic) {
  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  for (int64_t i = 0; i < 1000; ++i) {
    bskip.add(i);
    follyAcc.add(i);
  }

  LargeNodeList::Skipper bskipS(bskip);
  folly::ConcurrentSkipList<int64_t>::Accessor::Skipper follyS(follyAcc);

  for (int64_t target = 0; target < 1000; target += 10) {
    auto bResult = bskipS.skipTo(target);
    follyS.to(target);
    auto fResult = follySkipperPos(follyS);

    EXPECT_EQ(bResult, fResult)
        << "Forward skipTo parity failure at target=" << target;
  }
}

TEST(SkipperParity, ForwardSkipToWithGaps) {
  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  // Insert only even numbers
  for (int64_t i = 0; i < 2000; i += 2) {
    bskip.add(i);
    follyAcc.add(i);
  }

  LargeNodeList::Skipper bskipS(bskip);
  folly::ConcurrentSkipList<int64_t>::Accessor::Skipper follyS(follyAcc);

  // skipTo odd numbers — should land on the next even
  for (int64_t target = 1; target < 2000; target += 10) {
    auto bResult = bskipS.skipTo(target);
    follyS.to(target);
    auto fResult = follySkipperPos(follyS);

    EXPECT_EQ(bResult, fResult)
        << "SkipTo with gaps parity failure at target=" << target << " bskip="
        << optionalToString(bResult) << " folly=" << optionalToString(fResult);
  }
}

TEST(SkipperParity, SkipToPastEnd) {
  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  for (int64_t i = 0; i < 100; ++i) {
    bskip.add(i);
    follyAcc.add(i);
  }

  LargeNodeList::Skipper bskipS(bskip);
  folly::ConcurrentSkipList<int64_t>::Accessor::Skipper follyS(follyAcc);

  // Skip to within range
  auto bResult = bskipS.skipTo(static_cast<int64_t>(50));
  follyS.to(50);
  EXPECT_EQ(bResult, follySkipperPos(follyS));

  // Skip to past end
  bResult = bskipS.skipTo(static_cast<int64_t>(200));
  follyS.to(200);
  auto fResult = follySkipperPos(follyS);
  EXPECT_EQ(bResult, fResult)
      << "SkipTo past end: bskip=" << optionalToString(bResult)
      << " folly=" << optionalToString(fResult);
}

TEST(SkipperParity, SkipToAfterDeletes) {
  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  for (int64_t i = 0; i < 1000; ++i) {
    bskip.add(i);
    follyAcc.add(i);
  }

  // Delete a contiguous range
  for (int64_t i = 100; i < 300; ++i) {
    bskip.remove(i);
    follyAcc.remove(i);
  }

  LargeNodeList::Skipper bskipS(bskip);
  folly::ConcurrentSkipList<int64_t>::Accessor::Skipper follyS(follyAcc);

  for (int64_t target = 0; target < 1000; target += 50) {
    auto bResult = bskipS.skipTo(target);
    follyS.to(target);
    auto fResult = follySkipperPos(follyS);

    EXPECT_EQ(bResult, fResult)
        << "SkipTo after deletes parity at target=" << target << " bskip="
        << optionalToString(bResult) << " folly=" << optionalToString(fResult);
  }
}

TEST(SkipperParity, SkipToJumpAheadThenForward) {
  // THE BUG SCENARIO: skipTo jumps ahead because intermediate keys are
  // deleted, then next skipTo with a higher target but lower than lastKey_
  // should NOT go backward.
  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  for (int64_t i = 0; i < 1000; ++i) {
    bskip.add(i);
    follyAcc.add(i);
  }

  // Delete keys 100-499 — skipTo(100) should jump to 500
  for (int64_t i = 100; i < 500; ++i) {
    bskip.remove(i);
    follyAcc.remove(i);
  }

  LargeNodeList::Skipper bskipS(bskip);
  folly::ConcurrentSkipList<int64_t>::Accessor::Skipper follyS(follyAcc);

  // Step 1: skipTo(100) → should return 500 (first non-deleted key >= 100)
  auto bResult = bskipS.skipTo(static_cast<int64_t>(100));
  follyS.to(100);
  auto fResult = follySkipperPos(follyS);
  EXPECT_EQ(bResult, fResult)
      << "Step 1: skipTo(100) bskip=" << optionalToString(bResult)
      << " folly=" << optionalToString(fResult);

  // Step 2: skipTo(200) — target < lastResult. Folly returns 500 (stays put).
  // BSkip must do the same.
  auto bResult2 = bskipS.skipTo(static_cast<int64_t>(200));
  follyS.to(200);
  auto fResult2 = follySkipperPos(follyS);
  EXPECT_EQ(bResult2, fResult2)
      << "Step 2: skipTo(200) bskip=" << optionalToString(bResult2)
      << " folly=" << optionalToString(fResult2);

  // Step 3: skipTo(300) — still < 500. Same.
  auto bResult3 = bskipS.skipTo(static_cast<int64_t>(300));
  follyS.to(300);
  auto fResult3 = follySkipperPos(follyS);
  EXPECT_EQ(bResult3, fResult3)
      << "Step 3: skipTo(300) bskip=" << optionalToString(bResult3)
      << " folly=" << optionalToString(fResult3);

  // Step 4: skipTo(600) — past the jump, normal forward
  auto bResult4 = bskipS.skipTo(static_cast<int64_t>(600));
  follyS.to(600);
  auto fResult4 = follySkipperPos(follyS);
  EXPECT_EQ(bResult4, fResult4)
      << "Step 4: skipTo(600) bskip=" << optionalToString(bResult4)
      << " folly=" << optionalToString(fResult4);
}

TEST(SkipperParity, MonotonicityAfterDeleteGaps) {
  // Multiple delete gaps causing multiple jumps. Results must be monotonic
  // and match folly.
  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  for (int64_t i = 0; i < 2000; ++i) {
    bskip.add(i);
    follyAcc.add(i);
  }

  // Create multiple gaps
  for (int64_t i = 100; i < 200; ++i) {
    bskip.remove(i);
    follyAcc.remove(i);
  }
  for (int64_t i = 400; i < 600; ++i) {
    bskip.remove(i);
    follyAcc.remove(i);
  }
  for (int64_t i = 800; i < 1200; ++i) {
    bskip.remove(i);
    follyAcc.remove(i);
  }

  LargeNodeList::Skipper bskipS(bskip);
  folly::ConcurrentSkipList<int64_t>::Accessor::Skipper follyS(follyAcc);

  std::optional<int64_t> prevBskip;
  for (int64_t target = 0; target < 2000; target += 50) {
    auto bResult = bskipS.skipTo(target);
    follyS.to(target);
    auto fResult = follySkipperPos(follyS);

    EXPECT_EQ(bResult, fResult) << "Monotonicity parity at target=" << target;

    // Also check monotonicity of bskip results
    if (bResult && prevBskip) {
      EXPECT_GE(*bResult, *prevBskip)
          << "BSkip non-monotonic: prev=" << *prevBskip << " curr=" << *bResult
          << " target=" << target;
    }
    if (bResult) {
      prevBskip = bResult;
    }
  }
}

TEST(SkipperParity, SkipToSameTarget) {
  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  for (int64_t i = 0; i < 500; ++i) {
    bskip.add(i);
    follyAcc.add(i);
  }

  LargeNodeList::Skipper bskipS(bskip);
  folly::ConcurrentSkipList<int64_t>::Accessor::Skipper follyS(follyAcc);

  // Same target multiple times
  for (int rep = 0; rep < 5; ++rep) {
    auto bResult = bskipS.skipTo(static_cast<int64_t>(100));
    follyS.to(100);
    EXPECT_EQ(bResult, follySkipperPos(follyS)) << "Same target rep=" << rep;
  }
}

TEST(SkipperParity, SkipToAllDeletedRange) {
  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  for (int64_t i = 0; i < 100; ++i) {
    bskip.add(i);
    follyAcc.add(i);
  }

  // Delete everything except 0 and 99
  for (int64_t i = 1; i < 99; ++i) {
    bskip.remove(i);
    follyAcc.remove(i);
  }

  LargeNodeList::Skipper bskipS(bskip);
  folly::ConcurrentSkipList<int64_t>::Accessor::Skipper follyS(follyAcc);

  // skipTo(0) → 0
  auto bResult = bskipS.skipTo(static_cast<int64_t>(0));
  follyS.to(0);
  EXPECT_EQ(bResult, follySkipperPos(follyS));

  // skipTo(1) → 99 (everything 1-98 deleted)
  bResult = bskipS.skipTo(static_cast<int64_t>(1));
  follyS.to(1);
  EXPECT_EQ(bResult, follySkipperPos(follyS));

  // skipTo(50) → still 99 (folly stays put, bskip must too)
  bResult = bskipS.skipTo(static_cast<int64_t>(50));
  follyS.to(50);
  EXPECT_EQ(bResult, follySkipperPos(follyS));
}

TEST(SkipperParity, RandomWorkloadSmall) {
  // Random sequence of adds, deletes, and skipTo — compare outputs.
  std::mt19937_64 rng(12345);
  constexpr int64_t kKeySpace = 500;
  constexpr int kOps = 2000;

  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  // Populate both
  for (int64_t i = 0; i < kKeySpace; ++i) {
    bskip.add(i);
    follyAcc.add(i);
  }

  // Random deletions
  for (int i = 0; i < kKeySpace / 3; ++i) {
    int64_t key = static_cast<int64_t>(rng() % kKeySpace);
    bskip.remove(key);
    follyAcc.remove(key);
  }

  // Create skippers and do random skipTo sequence
  LargeNodeList::Skipper bskipS(bskip);
  folly::ConcurrentSkipList<int64_t>::Accessor::Skipper follyS(follyAcc);

  for (int i = 0; i < kOps; ++i) {
    int64_t target = static_cast<int64_t>(rng() % (kKeySpace + 100));
    auto bResult = bskipS.skipTo(target);
    follyS.to(target);
    auto fResult = follySkipperPos(follyS);

    EXPECT_EQ(bResult, fResult)
        << "Random workload op=" << i << " target=" << target << " bskip="
        << optionalToString(bResult) << " folly=" << optionalToString(fResult);
  }
}

TEST(SkipperParity, RandomWorkloadLargeForwardOnly) {
  // Large random forward-only workload — monotonically increasing targets
  std::mt19937_64 rng(67890);
  constexpr int64_t kKeySpace = 10000;

  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  for (int64_t i = 0; i < kKeySpace; ++i) {
    bskip.add(i);
    follyAcc.add(i);
  }

  // Random sparse deletions
  for (int i = 0; i < kKeySpace / 2; ++i) {
    int64_t key = static_cast<int64_t>(rng() % kKeySpace);
    bskip.remove(key);
    follyAcc.remove(key);
  }

  LargeNodeList::Skipper bskipS(bskip);
  folly::ConcurrentSkipList<int64_t>::Accessor::Skipper follyS(follyAcc);

  int64_t target = 0;
  int failures = 0;
  while (target < kKeySpace + 100) {
    auto bResult = bskipS.skipTo(target);
    follyS.to(target);
    auto fResult = follySkipperPos(follyS);

    if (bResult != fResult) {
      ++failures;
      if (failures <= 10) {
        ADD_FAILURE()
            << "Forward-only parity failure at target=" << target
            << " bskip=" << optionalToString(bResult)
            << " folly=" << optionalToString(fResult);
      }
    }

    target += static_cast<int64_t>(rng() % 20) + 1; // random stride 1-20
  }
  EXPECT_EQ(failures, 0) << failures << " total parity failures";
}

TEST(SkipperParity, RandomWorkloadMultipleSeeds) {
  // Run the parity check with multiple random seeds for broader coverage
  for (uint64_t seed = 0; seed < 20; ++seed) {
    std::mt19937_64 rng(seed * 7919 + 42);
    constexpr int64_t kKeySpace = 2000;

    LargeNodeList bskip;
    auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
    folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

    // Random inserts
    std::vector<int64_t> keys;
    keys.reserve(kKeySpace);
    for (int64_t i = 0; i < kKeySpace; ++i) {
      keys.push_back(i);
    }
    std::shuffle(keys.begin(), keys.end(), rng);
    for (auto k : keys) {
      bskip.add(k);
      follyAcc.add(k);
    }

    // Random deletes
    int numDeletes = static_cast<int>(rng() % (kKeySpace / 2));
    for (int i = 0; i < numDeletes; ++i) {
      int64_t key = static_cast<int64_t>(rng() % kKeySpace);
      bskip.remove(key);
      follyAcc.remove(key);
    }

    // Forward-only skipTo sweep
    LargeNodeList::Skipper bskipS(bskip);
    folly::ConcurrentSkipList<int64_t>::Accessor::Skipper follyS(follyAcc);

    int64_t target = 0;
    while (target < kKeySpace) {
      auto bResult = bskipS.skipTo(target);
      follyS.to(target);
      auto fResult = follySkipperPos(follyS);

      EXPECT_EQ(bResult, fResult)
          << "Seed=" << seed << " target=" << target
          << " bskip=" << optionalToString(bResult)
          << " folly=" << optionalToString(fResult);

      target += static_cast<int64_t>(rng() % 50) + 1;
    }
  }
}

TEST(SkipperParity, InterleavedDeleteAndSkipTo) {
  // Interleave deletions between skipTo calls — single threaded, deterministic
  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  for (int64_t i = 0; i < 500; ++i) {
    bskip.add(i);
    follyAcc.add(i);
  }

  LargeNodeList::Skipper bskipS(bskip);
  folly::ConcurrentSkipList<int64_t>::Accessor::Skipper follyS(follyAcc);

  // skipTo(50), then delete 51-149, then skipTo(100)
  auto bR = bskipS.skipTo(static_cast<int64_t>(50));
  follyS.to(50);
  EXPECT_EQ(bR, follySkipperPos(follyS));

  for (int64_t i = 51; i < 150; ++i) {
    bskip.remove(i);
    follyAcc.remove(i);
  }

  bR = bskipS.skipTo(static_cast<int64_t>(100));
  follyS.to(100);
  EXPECT_EQ(bR, follySkipperPos(follyS))
      << "After delete 51-149, skipTo(100): bskip=" << optionalToString(bR)
      << " folly=" << optionalToString(follySkipperPos(follyS));

  // Delete more, keep going
  for (int64_t i = 200; i < 350; ++i) {
    bskip.remove(i);
    follyAcc.remove(i);
  }

  bR = bskipS.skipTo(static_cast<int64_t>(250));
  follyS.to(250);
  EXPECT_EQ(bR, follySkipperPos(follyS)) << "After delete 200-349, skipTo(250)";

  bR = bskipS.skipTo(static_cast<int64_t>(400));
  follyS.to(400);
  EXPECT_EQ(bR, follySkipperPos(follyS));
}

TEST(SkipperParity, EmptyList) {
  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  LargeNodeList::Skipper bskipS(bskip);
  folly::ConcurrentSkipList<int64_t>::Accessor::Skipper follyS(follyAcc);

  auto bResult = bskipS.skipTo(static_cast<int64_t>(0));
  follyS.to(0);
  EXPECT_EQ(bResult, follySkipperPos(follyS));
}

TEST(SkipperParity, SingleElement) {
  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  bskip.add(500);
  follyAcc.add(500);

  LargeNodeList::Skipper bskipS(bskip);
  folly::ConcurrentSkipList<int64_t>::Accessor::Skipper follyS(follyAcc);

  // Before element
  auto bR = bskipS.skipTo(static_cast<int64_t>(100));
  follyS.to(100);
  EXPECT_EQ(bR, follySkipperPos(follyS));

  // At element
  bR = bskipS.skipTo(static_cast<int64_t>(500));
  follyS.to(500);
  EXPECT_EQ(bR, follySkipperPos(follyS));

  // Past element
  bR = bskipS.skipTo(static_cast<int64_t>(999));
  follyS.to(999);
  EXPECT_EQ(bR, follySkipperPos(follyS));
}

TEST(SkipperParity, AllDeletedList) {
  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  for (int64_t i = 0; i < 100; ++i) {
    bskip.add(i);
    follyAcc.add(i);
  }
  for (int64_t i = 0; i < 100; ++i) {
    bskip.remove(i);
    follyAcc.remove(i);
  }

  LargeNodeList::Skipper bskipS(bskip);
  folly::ConcurrentSkipList<int64_t>::Accessor::Skipper follyS(follyAcc);

  auto bR = bskipS.skipTo(static_cast<int64_t>(50));
  follyS.to(50);
  EXPECT_EQ(bR, follySkipperPos(follyS));
}

TEST(SkipperParity, RandomForwardWorkload) {
  std::mt19937_64 rng(99999);
  constexpr int64_t kKeySpace = 1000;

  LargeNodeList bskip;
  auto follyList = folly::ConcurrentSkipList<int64_t>::createInstance(10);
  folly::ConcurrentSkipList<int64_t>::Accessor follyAcc(follyList);

  for (int64_t i = 0; i < kKeySpace; ++i) {
    bskip.add(i);
    follyAcc.add(i);
  }

  for (int i = 0; i < kKeySpace / 3; ++i) {
    int64_t key = static_cast<int64_t>(rng() % kKeySpace);
    bskip.remove(key);
    follyAcc.remove(key);
  }

  LargeNodeList::Skipper bskipS(bskip);
  folly::ConcurrentSkipList<int64_t>::Accessor::Skipper follyS(follyAcc);

  std::vector<int64_t> targets;
  targets.reserve(500);
  for (int i = 0; i < 500; ++i) {
    targets.push_back(static_cast<int64_t>(rng() % (kKeySpace + 50)));
  }
  std::sort(targets.begin(), targets.end());

  for (size_t i = 0; i < targets.size(); ++i) {
    auto bResult = bskipS.skipTo(targets[i]);
    follyS.to(targets[i]);
    auto fResult = follySkipperPos(follyS);
    EXPECT_EQ(bResult, fResult)
        << "Parity failure at op=" << i << " target=" << targets[i];
  }
}

// ============================================================================
// Default config tests (B=16, P=16)
// ============================================================================

TEST(ConcurrentBSkipListProd, BasicInsertAndFind) {
  DefaultList list;

  for (int64_t i = 0; i < 10000; ++i) {
    list.add(i);
  }

  EXPECT_EQ(list.size(), 10000);
  for (int64_t i = 0; i < 10000; ++i) {
    EXPECT_TRUE(list.contains(i));
  }
}

TEST(ConcurrentBSkipListProd, ConcurrentInsert) {
  DefaultList list;

  constexpr int kNumThreads = 8;
  constexpr int kKeysPerThread = 5000;

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&list, t]() {
      for (int i = 0; i < kKeysPerThread; ++i) {
        list.add(static_cast<int64_t>(t * kKeysPerThread + i));
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(list.size(), kNumThreads * kKeysPerThread);
}

TEST(ConcurrentBSkipListProd, ConcurrentMixedOps) {
  DefaultList list;

  for (int64_t i = 0; i < 5000; ++i) {
    list.add(i);
  }

  std::atomic<bool> stop{false};

  std::vector<std::thread> threads;

  threads.reserve(4);
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&list, &stop, t]() {
      int64_t key = 5000 + t * 100000;
      while (!stop.load()) {
        list.add(key++);
      }
    });
  }

  threads.reserve(4);
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&list, &stop]() {
      while (!stop.load()) {
        for (int64_t i = 0; i < 5000; ++i) {
          list.contains(i);
        }
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  for (auto& t : threads) {
    t.join();
  }
}

TEST(ConcurrentBSkipListProd, SkipperBasic) {
  DefaultList list;

  for (int64_t i = 0; i < 10000; ++i) {
    list.add(i);
  }

  DefaultList::Skipper skipper(list);

  auto pos = skipper.skipTo(static_cast<int64_t>(5000));
  ASSERT_TRUE(pos);
  EXPECT_EQ(*pos, 5000);

  pos = skipper.advance();
  ASSERT_TRUE(pos);
  EXPECT_EQ(*pos, 5001);

  pos = skipper.skipTo(static_cast<int64_t>(9999));
  ASSERT_TRUE(pos);
  EXPECT_EQ(*pos, 9999);

  pos = skipper.advance();
  EXPECT_FALSE(pos);
}

TEST(ConcurrentBSkipListProd, DeleteBasic) {
  DefaultList list;

  for (int64_t i = 0; i < 1000; ++i) {
    list.add(i);
  }

  for (int64_t i = 0; i < 500; ++i) {
    EXPECT_TRUE(list.remove(i));
  }

  EXPECT_EQ(list.size(), 500);

  for (int64_t i = 500; i < 1000; ++i) {
    EXPECT_TRUE(list.contains(i));
  }
}

TEST(ConcurrentBSkipListProd, StressSkipperDuringWrites) {
  DefaultList list;

  for (int64_t i = 0; i < 5000; ++i) {
    list.add(i);
  }

  std::atomic<bool> stop{false};
  std::atomic<int64_t> anomalies{0};

  std::vector<std::thread> threads;

  // Writers
  threads.reserve(4);
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&list, &stop, t]() {
      std::mt19937 rng(42 + t);
      while (!stop.load()) {
        int64_t key = rng() % 10000;
        if (rng() % 2 == 0) {
          list.add(key);
        } else {
          list.remove(key);
        }
      }
    });
  }

  // Skipper readers
  threads.reserve(4);
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&list, &stop, &anomalies]() {
      while (!stop.load()) {
        DefaultList::Skipper skipper(list);
        int64_t prev = -1;
        for (auto pos = skipper.current(); pos; pos = skipper.advance()) {
          if (*pos <= prev) {
            anomalies.fetch_add(1);
          }
          prev = *pos;
        }
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(2));
  stop.store(true);
  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(anomalies.load(), 0);
}

// ============================================================================
// Concurrency stress tests
// ============================================================================

TEST(ConcurrentBSkipListConcurrency, HighContentionReads) {
  LargeNodeList list;

  for (int64_t i = 0; i < 1000; ++i) {
    list.add(i);
  }

  constexpr int kNumThreads = 16;
  std::atomic<int> errors{0};

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&list, &errors]() {
      for (int iter = 0; iter < 1000; ++iter) {
        for (int64_t i = 0; i < 1000; ++i) {
          if (!list.contains(i)) {
            errors.fetch_add(1);
          }
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(errors.load(), 0);
}

TEST(ConcurrentBSkipListConcurrency, ConcurrentWritesSameKeys) {
  LargeNodeList list;

  constexpr int kNumThreads = 8;
  constexpr int kNumKeys = 1000;

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&list]() {
      for (int iter = 0; iter < 100; ++iter) {
        for (int64_t i = 0; i < kNumKeys; ++i) {
          list.add(i);
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(list.size(), kNumKeys);
}

TEST(ConcurrentBSkipListConcurrency, NoFalseNegatives) {
  LargeNodeList list;

  constexpr int kNumKeys = 10000;

  for (int64_t i = 0; i < kNumKeys; ++i) {
    list.add(i);
  }

  constexpr int kNumReaders = 8;
  constexpr int kNumWriters = 4;
  std::atomic<bool> stop{false};
  std::atomic<int64_t> falseNegatives{0};
  std::atomic<int64_t> totalReads{0};

  std::vector<std::thread> threads;

  // Readers
  threads.reserve(kNumReaders);
  for (int t = 0; t < kNumReaders; ++t) {
    threads.emplace_back([&list, &stop, &falseNegatives, &totalReads]() {
      while (!stop.load()) {
        for (int64_t i = 0; i < kNumKeys; ++i) {
          if (!list.contains(i)) {
            falseNegatives.fetch_add(1);
          }
          totalReads.fetch_add(1);
        }
      }
    });
  }

  // Writers: insert ONLY new keys (above kNumKeys)
  threads.reserve(kNumWriters);
  for (int t = 0; t < kNumWriters; ++t) {
    threads.emplace_back([&list, &stop, t]() {
      int64_t key = kNumKeys + t * 100000;
      while (!stop.load()) {
        list.add(key++);
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(2));
  stop.store(true);
  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(falseNegatives.load(), 0)
      << "Had " << falseNegatives.load() << " false negatives out of "
      << totalReads.load() << " reads";
}

TEST(ConcurrentBSkipListConcurrency, ConcurrentInsertNoDataLoss) {
  DefaultList list;

  constexpr int kNumThreads = 8;
  constexpr int kKeysPerThread = 5000;

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&list, t]() {
      for (int i = 0; i < kKeysPerThread; ++i) {
        list.add(static_cast<int64_t>(t * kKeysPerThread + i));
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(list.size(), kNumThreads * kKeysPerThread);

  // Verify every key can be found
  for (int t = 0; t < kNumThreads; ++t) {
    for (int i = 0; i < kKeysPerThread; ++i) {
      int64_t key = t * kKeysPerThread + i;
      EXPECT_TRUE(list.contains(key)) << "Missing key " << key;
    }
  }

  // Verify sorted iteration matches
  std::vector<int64_t> iterated;
  scanWithForEachElement(list, [&](int64_t key) { iterated.push_back(key); });
  EXPECT_EQ(iterated.size(), kNumThreads * kKeysPerThread);
  for (size_t i = 1; i < iterated.size(); ++i) {
    EXPECT_LT(iterated[i - 1], iterated[i]);
  }
}

TEST(ConcurrentBSkipList, SequentialScanTraversal) {
  LargeNodeList list;

  for (int64_t i = 0; i < 1000; ++i) {
    list.add(i);
  }

  std::vector<int64_t> visited;
  scanWithForEachElement(list, [&visited](int64_t k) { visited.push_back(k); });

  EXPECT_EQ(visited.size(), 1000);
  for (int64_t i = 0; i < 1000; ++i) {
    EXPECT_EQ(visited[i], i);
  }
}

TEST(ConcurrentBSkipList, SequentialScanTraversalWithDeletes) {
  LargeNodeList list;

  for (int64_t i = 0; i < 100; ++i) {
    list.add(i);
  }

  // Delete even keys
  for (int64_t i = 0; i < 100; i += 2) {
    list.remove(i);
  }

  std::vector<int64_t> visited;
  scanWithForEachElement(list, [&visited](int64_t k) { visited.push_back(k); });

  EXPECT_EQ(visited.size(), 50);
  for (auto k : visited) {
    EXPECT_EQ(k % 2, 1);
  }
}

// ============================================================================
// AddOrUpdate tests
// ============================================================================

TEST(ConcurrentBSkipList, AddOrUpdateBasic) {
  LargeNodeList list;

  // Add keys 0-99
  for (int64_t i = 0; i < 100; ++i) {
    list.add(i);
  }

  // AddOrUpdate on existing key — updater should be called
  int64_t updatedKey = -1;
  auto outcome = list.addOrUpdate(
      int64_t(50), [&updatedKey](const int64_t& val) { updatedKey = val; });

  EXPECT_FALSE(outcome);
  EXPECT_EQ(updatedKey, 50);

  // AddOrUpdate on non-existing key — should insert
  outcome = list.addOrUpdate(int64_t(200), [](const int64_t&) {
    FAIL() << "Updater should not be called on new insert";
  });

  EXPECT_TRUE(outcome);
  EXPECT_TRUE(list.contains(200));
}

// ============================================================================
// Tombstone tests
// ============================================================================

TEST(BSkipTombstone, HighRankTombstone_B32) {
  using List32 = TestList<int64_t, 32, 16>;
  List32 list;

  for (int64_t i = 0; i < 64; ++i) {
    list.add(i);
  }

  // Delete high-rank keys
  for (int64_t i = 30; i < 32; ++i) {
    EXPECT_TRUE(list.remove(i));
    EXPECT_FALSE(list.contains(i));
  }

  // Verify remaining
  for (int64_t i = 0; i < 30; ++i) {
    EXPECT_TRUE(list.contains(i));
  }

  // Verify iteration skips tombstones
  std::vector<int64_t> iterated;
  scanWithForEachElement(list, [&](int64_t key) { iterated.push_back(key); });
  for (auto k : iterated) {
    EXPECT_TRUE(k < 30 || k >= 32);
  }
}

TEST(BSkipTombstone, IterationWithHeavyTombstones) {
  DefaultList list;

  constexpr int kTotal = 5000;
  for (int64_t i = 0; i < kTotal; ++i) {
    list.add(i);
  }

  // Delete 80% of keys
  for (int64_t i = 0; i < kTotal; ++i) {
    if (i % 5 != 0) {
      list.remove(i);
    }
  }

  // Iterate — should only see keys divisible by 5
  DefaultList::Skipper skipper(list);
  int count = 0;
  int64_t prev = -1;
  for (auto pos = skipper.current(); pos; pos = skipper.advance()) {
    EXPECT_EQ(*pos % 5, 0);
    EXPECT_GT(*pos, prev);
    prev = *pos;
    ++count;
  }
  EXPECT_EQ(count, kTotal / 5);
}

// ============================================================================
// Memory safety tests
// ============================================================================

TEST(BSkipMemory, SkipperKeepsListAlive) {
  DefaultList list;

  for (int64_t i = 0; i < 1000; ++i) {
    list.add(i);
  }

  // Create skipper and verify it works
  DefaultList::Skipper skipper(list);
  auto pos = skipper.skipTo(static_cast<int64_t>(500));
  ASSERT_TRUE(pos);
  EXPECT_EQ(*pos, 500);

  pos = skipper.advance();
  ASSERT_TRUE(pos);
  EXPECT_EQ(*pos, 501);
}

// ============================================================================
// Cross-validation: ConcurrentBSkipList iteration completeness
// ============================================================================

TEST(ConcurrentBSkipListIteration, SkipToAllElements) {
  DefaultList list;
  std::vector<int64_t> inserted;
  for (int64_t i = 1; i <= 1000; ++i) {
    list.add(i);
    inserted.push_back(i);
  }

  DefaultList::Skipper skipper(list);
  std::vector<int64_t> found;
  for (int64_t target : inserted) {
    auto pos = skipper.skipTo(target);
    ASSERT_TRUE(pos) << "skipTo(" << target << ") returned nullopt";
    EXPECT_EQ(*pos, target);
    found.push_back(*pos);
  }
  EXPECT_EQ(found, inserted);
}

TEST(ConcurrentBSkipListIteration, AdvanceAllElements) {
  DefaultList list;
  std::vector<int64_t> inserted;
  for (int64_t i = 1; i <= 1000; ++i) {
    list.add(i);
    inserted.push_back(i);
  }

  DefaultList::Skipper skipper(list);
  std::vector<int64_t> found;
  for (auto pos = skipper.current(); pos; pos = skipper.advance()) {
    found.push_back(*pos);
  }
  EXPECT_EQ(found, inserted);
}

TEST(ConcurrentBSkipListIteration, TwoIdenticalListsSameIteration) {
  DefaultList list1;
  DefaultList list2;

  for (int64_t i = 1; i <= 10000; ++i) {
    list1.add(i);
    list2.add(i);
  }

  DefaultList::Skipper skipper1(list1);
  DefaultList::Skipper skipper2(list2);

  int count = 0;
  auto pos1 = skipper1.current();
  auto pos2 = skipper2.current();
  while (pos1 && pos2) {
    EXPECT_EQ(*pos1, *pos2) << "Mismatch at element " << count;
    pos1 = skipper1.advance();
    pos2 = skipper2.advance();
    count++;
  }
  EXPECT_FALSE(pos1) << "List 1 has extra elements";
  EXPECT_FALSE(pos2) << "List 2 has extra elements";
  EXPECT_EQ(count, 10000);
}

// ============================================================================
// High thread count stress
// ============================================================================

// High thread count stress: 8 writers + 16 readers using thread-safe Skipper.
TEST(BSkipConcurrency, HighThreadCountStress) {
  DefaultList list;

  constexpr int kNumWriterThreads = 8;
  constexpr int kNumReaderThreads = 16;
  constexpr int kKeysPerWriter = 5000;

  std::atomic<bool> stop{false};
  std::atomic<int64_t> readErrors{0};

  std::vector<std::thread> threads;

  // Writers
  threads.reserve(kNumWriterThreads + kNumReaderThreads);
  for (int t = 0; t < kNumWriterThreads; ++t) {
    threads.emplace_back([&list, t]() {
      for (int i = 0; i < kKeysPerWriter; ++i) {
        list.add(static_cast<int64_t>(t * kKeysPerWriter + i));
      }
    });
  }

  // Readers using thread-safe Skipper + contains
  for (int t = 0; t < kNumReaderThreads; ++t) {
    threads.emplace_back([&list, &stop, &readErrors]() {
      while (!stop.load()) {
        DefaultList::Skipper skipper(list);
        auto val = skipper.current();
        while (val.has_value()) {
          if (!list.contains(*val)) {
            readErrors.fetch_add(1);
          }
          val = skipper.advance();
        }
      }
    });
  }

  // Let writers finish, then stop readers
  for (int t = 0; t < kNumWriterThreads; ++t) {
    threads[t].join();
  }
  stop.store(true);
  for (int t = kNumWriterThreads; t < static_cast<int>(threads.size()); ++t) {
    threads[t].join();
  }

  EXPECT_EQ(list.size(), kNumWriterThreads * kKeysPerWriter);
  EXPECT_EQ(readErrors.load(), 0);
}

TEST(BSkipConcurrency, MutualExclusionDelete) {
  DefaultList list;

  constexpr int kNumKeys = 10000;
  for (int64_t i = 0; i < kNumKeys; ++i) {
    list.add(i);
  }

  // Multiple threads try to delete the same keys concurrently.
  // Each key should only be successfully deleted once.
  std::atomic<int64_t> totalDeleted{0};

  constexpr int kNumThreads = 8;
  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&list, &totalDeleted]() {
      for (int64_t i = 0; i < kNumKeys; ++i) {
        if (list.remove(i)) {
          totalDeleted.fetch_add(1);
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(totalDeleted.load(), kNumKeys);
  EXPECT_EQ(list.size(), 0);
}

// ============================================================================
// uint32_t key type tests
// ============================================================================

TEST(ConcurrentBSkipList, Uint32Keys) {
  ConcurrentBSkipList<uint32_t> list;

  // Start from 1 because 0 == minSentinel for uint32_t
  for (uint32_t i = 1; i <= 10000; ++i) {
    list.add(i);
  }

  EXPECT_EQ(list.size(), 10000);

  for (uint32_t i = 1; i <= 10000; ++i) {
    EXPECT_TRUE(list.contains(i));
  }
}

// ============================================================================
// AddOrUpdate extended tests
// ============================================================================

TEST(ConcurrentBSkipList, AddOrUpdateTombstonedKey) {
  LargeNodeList list;

  for (int64_t i = 0; i < 100; ++i) {
    list.add(i);
  }

  // Delete key 50
  EXPECT_TRUE(list.remove(50));
  EXPECT_FALSE(list.contains(50));
  EXPECT_EQ(list.size(), 99);

  // addOrUpdate should reinsert the tombstoned key
  auto reviveOutcome = list.addOrUpdate(int64_t(50), [](const int64_t&) {
    FAIL() << "Updater should not be called on tombstone reinsert";
  });

  EXPECT_TRUE(reviveOutcome);
  EXPECT_TRUE(list.contains(50));
  EXPECT_EQ(list.size(), 100);
}

TEST(ConcurrentBSkipList, AddOrUpdateConcurrent) {
  LargeNodeList list;

  constexpr int kNumKeys = 1000;
  for (int64_t i = 0; i < kNumKeys; ++i) {
    list.add(i);
  }

  constexpr int kNumThreads = 8;
  std::atomic<bool> stop{false};

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&list, &stop, t]() {
      std::mt19937 rng(42 + t);
      while (!stop.load()) {
        int64_t key = rng() % kNumKeys;
        list.addOrUpdate(key, [](const int64_t&) {
          // no-op update
        });
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  for (auto& t : threads) {
    t.join();
  }

  // All original keys should still be present
  for (int64_t i = 0; i < kNumKeys; ++i) {
    EXPECT_TRUE(list.contains(i))
        << "Key " << i << " missing after concurrent addOrUpdate";
  }
}

TEST(ConcurrentBSkipList, AddOrUpdateConcurrentWithRemove) {
  LargeNodeList list;

  constexpr int kNumKeys = 512;
  for (int64_t i = 0; i < kNumKeys; ++i) {
    list.add(i);
  }

  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  threads.reserve(8);

  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&list, &stop, t]() {
      std::mt19937 rng(100 + t);
      while (!stop.load(std::memory_order_relaxed)) {
        int64_t key = rng() % kNumKeys;
        list.addOrUpdate(key, [](const int64_t&) {
          // no-op update
        });
      }
    });
  }

  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&list, &stop, t]() {
      std::mt19937 rng(200 + t);
      while (!stop.load(std::memory_order_relaxed)) {
        int64_t key = rng() % kNumKeys;
        list.remove(key);
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true, std::memory_order_relaxed);
  for (auto& t : threads) {
    t.join();
  }

  int64_t prev = std::numeric_limits<int64_t>::min();
  size_t iterCount = 0;
  scanWithForEachElement(list, [&](int64_t key) {
    EXPECT_LT(prev, key);
    EXPECT_GE(key, 0);
    EXPECT_LT(key, kNumKeys);
    prev = key;
    ++iterCount;
  });
  EXPECT_EQ(iterCount, list.size());
}

TEST(ConcurrentBSkipList, ConcurrentSplitSameNodeRange) {
  using SmallSplitList = TestList<int64_t, 4, 2>;

  SmallSplitList list;
  constexpr int kThreads = 8;
  constexpr int kKeysPerThread = 128;
  std::atomic<bool> start{false};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&list, &start, t]() {
      while (!start.load(std::memory_order_acquire)) {
      }
      for (int i = 0; i < kKeysPerThread; ++i) {
        int64_t key = i * kThreads + t + 1;
        EXPECT_TRUE(list.add(key));
      }
    });
  }

  start.store(true, std::memory_order_release);
  for (auto& t : threads) {
    t.join();
  }

  constexpr int64_t kTotalKeys = kThreads * kKeysPerThread;
  EXPECT_EQ(list.size(), static_cast<size_t>(kTotalKeys));
  for (int64_t key = 1; key <= kTotalKeys; ++key) {
    EXPECT_TRUE(list.contains(key)) << "Missing key " << key;
  }

  int64_t expected = 1;
  scanWithForEachElement(list, [&](int64_t key) {
    EXPECT_EQ(key, expected++);
  });
  EXPECT_EQ(expected, kTotalKeys + 1);
}

TEST(ConcurrentBSkipList, SkipperScanConcurrentWrites) {
  LargeNodeList list;

  for (int64_t i = 0; i < 1000; ++i) {
    list.add(i);
  }

  std::atomic<bool> stop{false};
  std::atomic<int> anomalies{0};

  // Writer thread
  std::thread writer([&list, &stop]() {
    int64_t key = 1000;
    while (!stop.load()) {
      list.add(key++);
    }
  });

  // Reader thread using a full skipper scan
  std::thread reader([&list, &stop, &anomalies]() {
    while (!stop.load()) {
      int64_t prev = -1;
      scanWithSkipper(list, [&prev, &anomalies](int64_t k) {
        if (k <= prev) {
          anomalies.fetch_add(1);
        }
        prev = k;
      });
    }
  });

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  writer.join();
  reader.join();

  EXPECT_EQ(anomalies.load(), 0) << "skipper scan returned out-of-order keys";
}

// ============================================================================
// OLC contention stress test
// ============================================================================

TEST(BSkipConcurrency, OLCContentionStress) {
  DefaultList list;

  constexpr int kInitialKeys = 10000;
  for (int64_t i = 0; i < kInitialKeys; ++i) {
    list.add(i);
  }

  constexpr int kNumWriters = 4;
  constexpr int kNumReaders = 8;
  std::atomic<bool> stop{false};
  std::atomic<int64_t> corruptedResults{0};

  std::vector<std::thread> threads;

  // Writers: mix of insert and delete to cause version bumps
  threads.reserve(kNumWriters);
  for (int t = 0; t < kNumWriters; ++t) {
    threads.emplace_back([&list, &stop, t]() {
      std::mt19937 rng(42 + t);
      int64_t nextKey = kInitialKeys + t * 100000;
      while (!stop.load()) {
        if (rng() % 3 == 0) {
          // Delete a random existing key
          list.remove(static_cast<int64_t>(rng() % kInitialKeys));
        } else {
          // Insert new keys
          list.add(nextKey++);
        }
      }
    });
  }

  // Readers: skipTo random targets, verify returned keys are valid
  threads.reserve(kNumReaders);
  for (int t = 0; t < kNumReaders; ++t) {
    threads.emplace_back([&list, &stop, &corruptedResults, t]() {
      std::mt19937 rng(100 + t);
      while (!stop.load()) {
        DefaultList::Skipper skipper(list);
        int64_t prev = -1;
        for (int i = 0; i < 100; ++i) {
          int64_t target = static_cast<int64_t>(rng() % (kInitialKeys * 2));
          if (target <= prev) {
            target = prev + 1;
          }
          auto pos = skipper.skipTo(target);
          if (pos) {
            if (*pos < target) {
              corruptedResults.fetch_add(1);
            }
            if (*pos <= prev && prev >= 0) {
              corruptedResults.fetch_add(1);
            }
            prev = *pos;
          } else {
            break;
          }
        }
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(2));
  stop.store(true);
  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(corruptedResults.load(), 0)
      << "OLC returned corrupted results under contention";
}

// ============================================================================
// OLC correctness regression tests
// ============================================================================

// Regression test for split-node visibility: OLC readers must not
// observe a partially-constructed node during splitAndInsert. The split node
// must have its version set to odd (write-in-progress) before being linked
// into the next_ chain.
TEST(BSkipConcurrency, SplitNodeOlcVisibility) {
  DefaultList list;

  // Pre-populate to force splits during concurrent inserts
  for (int64_t i = 0; i < 1000; i += 2) {
    list.add(i);
  }

  constexpr int kWriters = 4;
  constexpr int kReaders = 8;
  std::atomic<bool> stop{false};
  std::atomic<int64_t> olcErrors{0};

  std::vector<std::thread> threads;
  threads.reserve(kWriters + kReaders);

  // Writers: insert odd keys to trigger splits in existing leaves
  for (int t = 0; t < kWriters; ++t) {
    threads.emplace_back([&list, t]() {
      for (int64_t i = 1 + t * 2; i < 1000; i += kWriters * 2) {
        list.add(i);
      }
    });
  }

  // Readers: skipTo across the range, verify results are valid
  for (int t = 0; t < kReaders; ++t) {
    threads.emplace_back([&list, &stop, &olcErrors, t]() {
      std::mt19937 rng(200 + t);
      while (!stop.load()) {
        DefaultList::Skipper skipper(list);
        for (int64_t target = 0; target < 1000; target += 5) {
          auto pos = skipper.skipTo(target);
          if (pos) {
            if (*pos < target) {
              olcErrors.fetch_add(1);
            }
          }
        }
      }
    });
  }

  // Let writers finish
  for (int t = 0; t < kWriters; ++t) {
    threads[t].join();
  }
  stop.store(true);
  for (size_t t = kWriters; t < threads.size(); ++t) {
    threads[t].join();
  }

  EXPECT_EQ(olcErrors.load(), 0)
      << "OLC reader saw invalid data from a split node";
  EXPECT_EQ(list.size(), 1000);
}

// Regression test: heavy concurrent inserts (causing many splits) with
// concurrent skipTo readers. Verifies that stale OLC path entries and
// split-node visibility don't cause incorrect results.
TEST(BSkipConcurrency, HeavySplitWithSkipTo) {
  DefaultList list;

  constexpr int kWriters = 4;
  constexpr int kReaders = 4;
  constexpr int kKeysPerWriter = 10000;
  std::atomic<bool> stop{false};
  std::atomic<int64_t> errors{0};

  std::vector<std::thread> threads;
  threads.reserve(kWriters + kReaders);

  for (int t = 0; t < kWriters; ++t) {
    threads.emplace_back([&list, t]() {
      for (int64_t i = 0; i < kKeysPerWriter; ++i) {
        list.add(static_cast<int64_t>(t * kKeysPerWriter + i));
      }
    });
  }

  for (int t = 0; t < kReaders; ++t) {
    threads.emplace_back([&list, &stop, &errors, t]() {
      std::mt19937 rng(300 + t);
      while (!stop.load()) {
        DefaultList::Skipper skipper(list);
        int64_t prev = -1;
        for (int i = 0; i < 200; ++i) {
          int64_t target = prev + 1 + static_cast<int64_t>(rng() % 100);
          auto pos = skipper.skipTo(target);
          if (pos) {
            if (*pos < target) {
              errors.fetch_add(1);
            }
            prev = *pos;
          } else {
            break;
          }
        }
      }
    });
  }

  for (int t = 0; t < kWriters; ++t) {
    threads[t].join();
  }
  stop.store(true);
  for (size_t t = kWriters; t < threads.size(); ++t) {
    threads[t].join();
  }

  EXPECT_EQ(errors.load(), 0)
      << "OLC returned below-target result during heavy splits";
}

// ============================================================================
// Custom comparator tests — verify Comp plumbing with std::greater
// ============================================================================

// Custom comparator (std::greater) lives in the Policy now that Comp moved
// off the primary template.
struct DescPolicy : ConcurrentBSkipDefaultPolicy<int64_t> {
  using Comp = std::greater<int64_t>;
};
using DescList = ConcurrentBSkipList<
    int64_t,
    /*PayloadType=*/void,
    /*B=*/16,
    bskip_detail::kDefaultReadPolicy<int64_t>,
    LeafStoragePolicy::Separate,
    DescPolicy>;

TEST(ConcurrentBSkipList, DescendingOrderInsertAndContains) {
  DescList list;

  for (int64_t i = 0; i < 1000; ++i) {
    list.add(i);
  }
  EXPECT_EQ(list.size(), 1000);

  for (int64_t i = 0; i < 1000; ++i) {
    EXPECT_TRUE(list.contains(i));
  }
  EXPECT_FALSE(list.contains(1000));
}

TEST(ConcurrentBSkipList, DescendingOrderIteration) {
  DescList list;

  for (int64_t i = 0; i < 100; ++i) {
    list.add(i);
  }

  // With std::greater, iteration should yield keys in descending order.
  int64_t prev = std::numeric_limits<int64_t>::max();
  size_t count = 0;
  scanWithForEachElement(list, [&](int64_t val) {
    EXPECT_LT(val, prev) << "Expected descending order";
    prev = val;
    ++count;
  });
  EXPECT_EQ(count, 100);
}

TEST(ConcurrentBSkipList, DescendingOrderSkipper) {
  DescList list;

  for (int64_t i = 0; i < 100; ++i) {
    list.add(i);
  }

  DescList::Skipper skipper(list);

  // skipTo(50) should return 50 (it's >= 50 in greater-ordering, meaning <= 50
  // numerically).
  auto result = skipper.skipTo(50);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 50);

  // advance should give us 49 (next in descending order).
  result = skipper.advance();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 49);
}

TEST(ConcurrentBSkipList, DescendingOrderRemove) {
  DescList list;

  for (int64_t i = 0; i < 100; ++i) {
    list.add(i);
  }
  EXPECT_EQ(list.size(), 100);

  EXPECT_TRUE(list.remove(50));
  EXPECT_FALSE(list.contains(50));
  EXPECT_EQ(list.size(), 99);

  // Re-insert should work.
  EXPECT_TRUE(list.add(50));
  EXPECT_TRUE(list.contains(50));
  EXPECT_EQ(list.size(), 100);
}

// ============================================================================
// Heterogeneous lookup tests
// ============================================================================

struct TransparentComp {
  using is_transparent = void;

  bool operator()(int64_t a, int64_t b) const { return a < b; }
  bool operator()(int32_t a, int64_t b) const {
    return static_cast<int64_t>(a) < b;
  }
  bool operator()(int64_t a, int32_t b) const {
    return a < static_cast<int64_t>(b);
  }
};

struct HeteroPolicy : ConcurrentBSkipDefaultPolicy<int64_t> {
  using Comp = TransparentComp;
};
using HeteroList = ConcurrentBSkipList<
    int64_t,
    /*PayloadType=*/void,
    /*B=*/16,
    bskip_detail::kDefaultReadPolicy<int64_t>,
    LeafStoragePolicy::Separate,
    HeteroPolicy>;

TEST(ConcurrentBSkipListHetero, ContainsWithDifferentType) {
  HeteroList list;

  for (int64_t i = 0; i < 1000; ++i) {
    list.add(i);
  }

  // Lookup using int32_t (heterogeneous)
  for (int32_t i = 0; i < 1000; ++i) {
    EXPECT_TRUE(list.contains(i)) << "Hetero contains failed for " << i;
  }
  EXPECT_FALSE(list.contains(int32_t{1000}));
  EXPECT_FALSE(list.contains(int32_t{-1}));
}

TEST(ConcurrentBSkipListHetero, FindWithDifferentType) {
  HeteroList list;

  for (int64_t i = 0; i < 1000; ++i) {
    list.add(i * 3);
  }

  // Find using int32_t
  auto result = list.find(int32_t{150});
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 150);

  result = list.find(int32_t{151});
  EXPECT_FALSE(result.has_value());
}

TEST(ConcurrentBSkipListHetero, ContainsWithTombstones) {
  HeteroList list;

  for (int64_t i = 0; i < 100; ++i) {
    list.add(i);
  }

  // Delete some keys, then verify heterogeneous lookup respects tombstones
  for (int64_t i = 0; i < 50; ++i) {
    list.remove(i);
  }

  for (int32_t i = 0; i < 50; ++i) {
    EXPECT_FALSE(list.contains(i)) << "Tombstoned key " << i << " found";
  }
  for (int32_t i = 50; i < 100; ++i) {
    EXPECT_TRUE(list.contains(i)) << "Live key " << i << " not found";
  }
}

TEST(ConcurrentBSkipListHetero, FindReturnsCorrectValue) {
  HeteroList list;

  list.add(int64_t{42});
  list.add(int64_t{100});
  list.add(int64_t{999});

  auto r1 = list.find(int32_t{42});
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(*r1, 42);

  auto r2 = list.find(int32_t{43});
  EXPECT_FALSE(r2.has_value());

  auto r3 = list.find(int32_t{999});
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(*r3, 999);
}

// ============================================================================
// Sentinel rejection tests
// ============================================================================

// Sentinel keys (numeric_limits min/max) are caller UB — DCHECK catches them.
// DCHECK compiles out under NDEBUG, so these only hold in debug builds.
#ifndef NDEBUG
TEST(ConcurrentBSkipListDeathTest, SentinelInsertDChecks) {
  LargeNodeList list;
  EXPECT_DEATH(list.add(std::numeric_limits<int64_t>::min()), "sentinel keys");
  EXPECT_DEATH(list.add(std::numeric_limits<int64_t>::max()), "sentinel keys");
}

TEST(ConcurrentBSkipListDeathTest, SentinelRemoveDChecks) {
  LargeNodeList list;
  list.add(int64_t{42});
  EXPECT_DEATH(
      list.remove(std::numeric_limits<int64_t>::min()), "sentinel keys");
  EXPECT_DEATH(
      list.remove(std::numeric_limits<int64_t>::max()), "sentinel keys");
}

TEST(ConcurrentBSkipListDeathTest, SentinelFindDChecks) {
  LargeNodeList list;
  EXPECT_DEATH(
      list.contains(std::numeric_limits<int64_t>::min()), "sentinel keys");
  EXPECT_DEATH(
      list.contains(std::numeric_limits<int64_t>::max()), "sentinel keys");
}
#endif // NDEBUG

// ============================================================================
// descendToLeafLocked path coverage — exercises insertOrUpdateExisting
// when foundKey=true at level > 0 (forces descent to leaf)
// ============================================================================

TEST(ConcurrentBSkipList, AddOrUpdateExistingPromotedKey) {
  // Use LargeNodeList (B=64, P=2) — high promotion probability means keys
  // are more likely to appear at level > 0, triggering the descent path
  // in insertOrUpdateExisting.
  LargeNodeList list;

  // Insert enough keys to get promoted keys at internal levels
  for (int64_t i = 0; i < 10000; ++i) {
    list.add(i);
  }
  EXPECT_EQ(list.size(), 10000);

  // addOrUpdate on existing keys — some will be found at level > 0
  // and trigger descendToLeafLocked
  int updateCount = 0;
  for (int64_t i = 0; i < 10000; ++i) {
    auto outcome = list.addOrUpdate(i, [](const int64_t&) {});
    if (!outcome) {
      ++updateCount;
    }
  }
  // All keys already exist, so all should be updates (not inserts)
  EXPECT_EQ(updateCount, 10000);
  EXPECT_EQ(list.size(), 10000);
}

TEST(ConcurrentBSkipList, AddOrUpdateTombstonedPromotedKey) {
  LargeNodeList list;

  for (int64_t i = 0; i < 5000; ++i) {
    list.add(i);
  }

  // Delete half — tombstone them
  for (int64_t i = 0; i < 2500; ++i) {
    list.remove(i);
  }
  EXPECT_EQ(list.size(), 2500);

  // Re-insert via addOrUpdate — some tombstoned keys will be found at
  // level > 0, triggering tombstone revival through descendToLeafLocked
  for (int64_t i = 0; i < 2500; ++i) {
    auto outcome = list.addOrUpdate(i, [](const int64_t&) {
      FAIL() << "Updater called on tombstoned key — should revive instead";
    });
    EXPECT_TRUE(outcome) << "Key " << i << " should be revived from tombstone";
  }
  EXPECT_EQ(list.size(), 5000);

  // Verify all keys present
  for (int64_t i = 0; i < 5000; ++i) {
    EXPECT_TRUE(list.contains(i)) << "Key " << i << " missing after revival";
  }
}

// Same tests with DefaultList config to catch config-dependent issues
TEST(ConcurrentBSkipList, AddOrUpdateExistingPromotedKeyDefaultConfig) {
  DefaultList list;

  for (int64_t i = 0; i < 5000; ++i) {
    list.add(i);
  }

  int updateCount = 0;
  for (int64_t i = 0; i < 5000; ++i) {
    auto outcome = list.addOrUpdate(i, [](const int64_t&) {});
    if (!outcome) {
      ++updateCount;
    }
  }
  EXPECT_EQ(updateCount, 5000);
  EXPECT_EQ(list.size(), 5000);
}

TEST(ConcurrentBSkipList, SkipperScanUnderConcurrentChurn) {
  DefaultList list;

  for (int64_t i = 0; i < 5000; ++i) {
    list.add(i);
  }

  // Writer thread churns keys above the stable range while readers repeatedly
  // walk the list.
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    int64_t base = 5000;
    while (!stop.load()) {
      list.add(base);
      list.remove(base);
      ++base;
    }
  });

  for (int iter = 0; iter < 50; ++iter) {
    std::vector<int64_t> visited;
    visited.reserve(6000);
    scanWithSkipper(list, [&](int64_t k) { visited.push_back(k); });
    // Should see at least the original 5000 keys (writer adds/removes
    // above that range), and they should be in sorted order.
    EXPECT_GE(visited.size(), 4999u);
    for (size_t i = 1; i < visited.size(); ++i) {
      EXPECT_LT(visited[i - 1], visited[i]) << "Out of order at index " << i;
    }
  }

  stop.store(true);
  writer.join();
}

// ============================================================================
// Skipper tier escalation — heavy writes force OLC retries and fallback
// to locked traversal (tiers 3 and 4)
// ============================================================================

TEST(ConcurrentBSkipList, SkipperTierEscalation) {
  DefaultList list;

  constexpr int kKeys = 10000;
  for (int64_t i = 0; i < kKeys; ++i) {
    list.add(i * 2); // even keys only
  }

  // Writers continuously mutate to invalidate OLC versions
  std::atomic<bool> stop{false};
  constexpr int kWriters = 4;
  std::vector<std::thread> writers;
  writers.reserve(kWriters);
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w] {
      int64_t base = kKeys * 2 + w * 10000;
      while (!stop.load()) {
        list.add(base);
        list.remove(base);
        ++base;
      }
    });
  }

  // Skipper doing large jumps — can't resolve in tier 1, must escalate
  for (int iter = 0; iter < 20; ++iter) {
    DefaultList::Skipper skipper(list);
    int64_t prev = -1;
    for (int64_t target = 0; target < kKeys * 2; target += 200) {
      auto pos = skipper.skipTo(target);
      if (pos) {
        EXPECT_GE(*pos, target);
        EXPECT_GT(*pos, prev);
        prev = *pos;
      }
    }
  }

  stop.store(true);
  for (auto& w : writers) {
    w.join();
  }
}

// ============================================================================
// Concurrent stress tests — C5 priority items
// ============================================================================

// skipTo + concurrent delete: Skipper doing skipTo while another thread
// tombstones keys the skipper is about to visit. Verifies skipTo still
// returns consistent, monotonically increasing results.
TEST(ConcurrentBSkipList, SkipToConcurrentDelete) {
  DefaultList list;

  constexpr int64_t kKeys = 20000;
  for (int64_t i = 0; i < kKeys; ++i) {
    list.add(i);
  }

  std::atomic<bool> stop{false};

  // Deleters: tombstone keys throughout the range
  constexpr int kDeleters = 4;
  std::vector<std::thread> deleters;
  deleters.reserve(kDeleters);
  for (int d = 0; d < kDeleters; ++d) {
    deleters.emplace_back([&, d] {
      std::mt19937_64 rng(42 + d);
      while (!stop.load()) {
        int64_t key = static_cast<int64_t>(rng() % kKeys);
        list.remove(key);
      }
    });
  }

  // Skipper threads: do skipTo across the range, verify correctness.
  // We check that every result satisfies *pos >= target (the skipTo contract).
  // Strict monotonicity (*pos > prev) is NOT checked: when concurrent deletes
  // tombstone many consecutive keys, a previous skipTo can jump ahead (e.g.,
  // skipTo(100) returns 250 because 100-249 are tombstoned). The Skipper is
  // forward-only, so skipTo(200) stays at 250 (target < current position).
  constexpr int kSkipperThreads = 4;
  std::atomic<int> violations{0};
  std::vector<std::thread> skippers;
  skippers.reserve(kSkipperThreads);
  for (int s = 0; s < kSkipperThreads; ++s) {
    skippers.emplace_back([&] {
      for (int iter = 0; iter < 50; ++iter) {
        DefaultList::Skipper skipper(list);
        for (int64_t target = 0; target < kKeys; target += 100) {
          auto pos = skipper.skipTo(target);
          if (pos) {
            if (*pos < target) {
              violations.fetch_add(1);
            }
          }
        }
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  for (auto& t : deleters) {
    t.join();
  }
  for (auto& t : skippers) {
    t.join();
  }

  EXPECT_EQ(violations.load(), 0) << "skipTo returned a value below the target";
}

// Full scan + heavy concurrent insert/delete: multiple writers churn keys above
// the iteration range while readers repeatedly walk the list. Writers operate
// above kInitialKeys so the stable keys [0, kInitialKeys) should always
// appear in sorted order.
TEST(ConcurrentBSkipList, SkipperScanHeavyWriteLoad) {
  DefaultList list;

  constexpr int64_t kInitialKeys = 5000;
  for (int64_t i = 0; i < kInitialKeys; ++i) {
    list.add(i);
  }

  std::atomic<bool> stop{false};

  // Heavy writers: churn keys above the stable range.
  constexpr int kWriters = 6;
  std::vector<std::thread> writers;
  writers.reserve(kWriters);
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w] {
      int64_t base = kInitialKeys + w * 10000;
      while (!stop.load()) {
        list.add(base);
        list.remove(base);
        ++base;
      }
    });
  }

  // Run full scans many times — verify sorted order invariant
  std::atomic<int> orderViolations{0};
  constexpr int kReaders = 4;
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&] {
      for (int iter = 0; iter < 100; ++iter) {
        std::vector<int64_t> visited;
        visited.reserve(kInitialKeys * 2);
        scanWithSkipper(list, [&](int64_t k) { visited.push_back(k); });
        // Stable keys should always be present
        EXPECT_GE(visited.size(), static_cast<size_t>(kInitialKeys - 1));
        for (size_t i = 1; i < visited.size(); ++i) {
          if (visited[i - 1] >= visited[i]) {
            orderViolations.fetch_add(1);
            break;
          }
        }
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  for (auto& t : writers) {
    t.join();
  }
  for (auto& t : readers) {
    t.join();
  }

  EXPECT_EQ(orderViolations.load(), 0)
      << "skipper scan returned out-of-order results under heavy writes";
}

// addOrUpdate + concurrent readers: concurrent addOrUpdate while Skippers
// and contains() are running.
TEST(ConcurrentBSkipList, AddOrUpdateConcurrentReaders) {
  DefaultList list;

  constexpr int64_t kKeys = 10000;
  for (int64_t i = 0; i < kKeys; ++i) {
    list.add(i);
  }

  std::atomic<bool> stop{false};

  // Updater threads: addOrUpdate existing keys (modify in place)
  constexpr int kUpdaters = 4;
  std::vector<std::thread> updaters;
  updaters.reserve(kUpdaters);
  for (int u = 0; u < kUpdaters; ++u) {
    updaters.emplace_back([&, u] {
      std::mt19937_64 rng(200 + u);
      while (!stop.load()) {
        int64_t key = static_cast<int64_t>(rng() % kKeys);
        list.addOrUpdate(key, [](const int64_t& /* existing */) {
          // no-op update — just exercises the lock path
        });
      }
    });
  }

  // Contains readers
  std::atomic<int> containsFails{0};
  constexpr int kContainsReaders = 4;
  std::vector<std::thread> containsReaders;
  containsReaders.reserve(kContainsReaders);
  for (int r = 0; r < kContainsReaders; ++r) {
    containsReaders.emplace_back([&] {
      while (!stop.load()) {
        for (int64_t i = 0; i < kKeys; ++i) {
          if (!list.contains(i)) {
            containsFails.fetch_add(1);
          }
        }
      }
    });
  }

  // Skipper readers: verify monotonicity
  std::atomic<int> skipperViolations{0};
  constexpr int kSkipperReaders = 2;
  std::vector<std::thread> skipperReaders;
  skipperReaders.reserve(kSkipperReaders);
  for (int s = 0; s < kSkipperReaders; ++s) {
    skipperReaders.emplace_back([&] {
      while (!stop.load()) {
        DefaultList::Skipper skipper(list);
        int64_t prev = -1;
        for (int64_t target = 0; target < kKeys; target += 50) {
          auto pos = skipper.skipTo(target);
          if (pos) {
            if (*pos < target || *pos <= prev) {
              skipperViolations.fetch_add(1);
            }
            prev = *pos;
          }
        }
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  for (auto& t : updaters) {
    t.join();
  }
  for (auto& t : containsReaders) {
    t.join();
  }
  for (auto& t : skipperReaders) {
    t.join();
  }

  EXPECT_EQ(containsFails.load(), 0)
      << "contains() returned false for a key that was never removed";
  EXPECT_EQ(skipperViolations.load(), 0)
      << "skipTo returned non-monotonic results during addOrUpdate";
}

// Large-scale concurrent delete + re-insert: tombstone churn — delete
// half the keys, re-insert them, verify size and iteration order.
TEST(ConcurrentBSkipList, TombstoneChurnConcurrent) {
  DefaultList list;

  constexpr int64_t kKeys = 10000;
  for (int64_t i = 0; i < kKeys; ++i) {
    list.add(i);
  }

  std::atomic<bool> stop{false};

  // Churners: delete and re-insert random keys
  constexpr int kChurners = 6;
  std::vector<std::thread> churners;
  churners.reserve(kChurners);
  for (int c = 0; c < kChurners; ++c) {
    churners.emplace_back([&, c] {
      std::mt19937_64 rng(300 + c);
      while (!stop.load()) {
        int64_t key = static_cast<int64_t>(rng() % kKeys);
        list.remove(key);
        list.add(key);
      }
    });
  }

  // Readers: verify iteration order is always sorted
  std::atomic<int> orderViolations{0};
  constexpr int kReaders = 2;
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&] {
      while (!stop.load()) {
        int64_t prev = -1;
        scanWithSkipper(list, [&](int64_t k) {
          if (k <= prev) {
            orderViolations.fetch_add(1);
          }
          prev = k;
        });
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  for (auto& t : churners) {
    t.join();
  }
  for (auto& t : readers) {
    t.join();
  }

  EXPECT_EQ(orderViolations.load(), 0)
      << "Iteration order violated during tombstone churn";

  // After all churners stop, re-insert all keys and verify
  for (int64_t i = 0; i < kKeys; ++i) {
    list.add(i);
  }

  EXPECT_EQ(list.size(), kKeys);

  int64_t prev = -1;
  size_t finalCount = scanWithForEachElement(list, [&](int64_t k) {
    EXPECT_GT(k, prev) << "Final iteration out of order";
    prev = k;
  });
  EXPECT_EQ(finalCount, static_cast<size_t>(kKeys));
}

// ============================================================================
// Descending-order heterogeneous lookup
// ============================================================================

struct DescTransparentComp {
  using is_transparent = void;

  bool operator()(int64_t a, int64_t b) const { return a > b; }
  bool operator()(int32_t a, int64_t b) const {
    return static_cast<int64_t>(a) > b;
  }
  bool operator()(int64_t a, int32_t b) const {
    return a > static_cast<int64_t>(b);
  }
};

struct DescHeteroPolicy : ConcurrentBSkipDefaultPolicy<int64_t> {
  using Comp = DescTransparentComp;
};
using DescHeteroList = ConcurrentBSkipList<
    int64_t,
    /*PayloadType=*/void,
    /*B=*/16,
    bskip_detail::kDefaultReadPolicy<int64_t>,
    LeafStoragePolicy::Separate,
    DescHeteroPolicy>;

TEST(ConcurrentBSkipListHetero, DescendingContains) {
  DescHeteroList list;

  for (int64_t i = 0; i < 500; ++i) {
    list.add(i);
  }

  // Heterogeneous lookup with int32_t on a descending-order list
  for (int32_t i = 0; i < 500; ++i) {
    EXPECT_TRUE(list.contains(i)) << "Missing key " << i;
  }
  EXPECT_FALSE(list.contains(int32_t{500}));
}

TEST(ConcurrentBSkipListHetero, DescendingFind) {
  DescHeteroList list;

  for (int64_t i = 0; i < 500; ++i) {
    list.add(i * 7);
  }

  auto r = list.find(int32_t{42});
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 42);

  r = list.find(int32_t{43});
  EXPECT_FALSE(r.has_value());
}

TEST(ConcurrentBSkipListHetero, DescendingIterationOrder) {
  DescHeteroList list;

  for (int64_t i = 0; i < 100; ++i) {
    list.add(i);
  }

  // Descending comparator → iteration should yield 99, 98, ..., 0
  int64_t prev = 100;
  size_t count = scanWithForEachElement(list, [&](int64_t value) {
    EXPECT_LT(value, prev);
    prev = value;
  });
  EXPECT_EQ(count, 100);
}

// ============================================================================
// Regression tests
// ============================================================================

// Internal node OLC visibility — skipTo during inserts that modify
// internal nodes. Version brackets ensure OLC readers
// see consistent internal node state.
TEST(ConcurrentBSkipList, InternalNodeOlcVisibility) {
  DefaultList list;

  constexpr int64_t kInitial = 5000;
  for (int64_t i = 0; i < kInitial; ++i) {
    list.add(i * 2); // even keys
  }

  std::atomic<bool> stop{false};
  constexpr int kWriters = 4;
  std::vector<std::thread> writers;
  writers.reserve(kWriters);
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w] {
      int64_t base = kInitial * 2 + w * 100000;
      while (!stop.load()) {
        list.add(base);
        ++base;
      }
    });
  }

  std::atomic<int> violations{0};
  constexpr int kReaders = 4;
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&] {
      for (int iter = 0; iter < 100; ++iter) {
        DefaultList::Skipper skipper(list);
        int64_t prev = -1;
        for (int64_t target = 0; target < kInitial * 2; target += 50) {
          auto pos = skipper.skipTo(target);
          if (pos) {
            if (*pos < target) {
              violations.fetch_add(1);
            }
            if (*pos <= prev) {
              violations.fetch_add(1);
            }
            prev = *pos;
          }
        }
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  for (auto& w : writers) {
    w.join();
  }
  for (auto& r : readers) {
    r.join();
  }

  EXPECT_EQ(violations.load(), 0)
      << "skipTo returned result below target or non-monotonic during "
         "concurrent inserts";
}

// Full-scan no-duplicates — concurrent inserts during a skipper walk.
TEST(ConcurrentBSkipList, SkipperScanNoDuplicates) {
  DefaultList list;

  constexpr int64_t kInitial = 5000;
  for (int64_t i = 0; i < kInitial; ++i) {
    list.add(i);
  }

  std::atomic<bool> stop{false};
  constexpr int kWriters = 4;
  std::vector<std::thread> writers;
  writers.reserve(kWriters);
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w] {
      int64_t base = kInitial + w * 100000;
      while (!stop.load()) {
        list.add(base);
        list.remove(base);
        ++base;
      }
    });
  }

  std::atomic<int> dupViolations{0};
  constexpr int kReaders = 4;
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&] {
      for (int iter = 0; iter < 200; ++iter) {
        std::vector<int64_t> visited;
        visited.reserve(kInitial * 2);
        scanWithSkipper(list, [&](int64_t k) { visited.push_back(k); });
        for (size_t i = 1; i < visited.size(); ++i) {
          if (visited[i] <= visited[i - 1]) {
            dupViolations.fetch_add(1);
            break;
          }
        }
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  for (auto& w : writers) {
    w.join();
  }
  for (auto& r : readers) {
    r.join();
  }

  EXPECT_EQ(dupViolations.load(), 0)
      << "skipper scan produced duplicate or out-of-order keys";
}

// Full-scan split stress with B=4 — small fanout forces frequent splits.
TEST(ConcurrentBSkipList, SkipperScanSplitStressB4) {
  using SmallList = TestList<int64_t, 4, 4>;
  SmallList list;

  constexpr int64_t kInitial = 2000;
  for (int64_t i = 0; i < kInitial; ++i) {
    list.add(i);
  }

  std::atomic<bool> stop{false};
  std::vector<std::thread> writers;
  writers.reserve(4);
  for (int w = 0; w < 4; ++w) {
    writers.emplace_back([&, w] {
      int64_t base = kInitial + w * 100000;
      while (!stop.load()) {
        list.add(base++);
      }
    });
  }

  std::atomic<int> violations{0};
  std::vector<std::thread> readers;
  readers.reserve(4);
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&] {
      for (int iter = 0; iter < 200; ++iter) {
        std::vector<int64_t> visited;
        visited.reserve(kInitial * 4);
        scanWithSkipper(list, [&](int64_t k) { visited.push_back(k); });
        for (size_t i = 1; i < visited.size(); ++i) {
          if (visited[i] <= visited[i - 1]) {
            violations.fetch_add(1);
            break;
          }
        }
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  for (auto& w : writers) {
    w.join();
  }
  for (auto& r : readers) {
    r.join();
  }

  EXPECT_EQ(violations.load(), 0)
      << "skipper scan with B=4 produced duplicates during splits";
}

// 2D: Large keys (sizeof(T) > 8) — exercises KeyReadPolicy::Locked path.
// A 16-byte key cannot be atomically read, so all reader paths use shared
// locks instead of seqlock validation.
struct LargeKeyPolicy : ConcurrentBSkipDefaultPolicy<LargeKey> {
  using Comp = LargeKeyComp;
};
using LargeKeyList = ConcurrentBSkipList<
    LargeKey,
    /*PayloadType=*/void,
    /*B=*/16,
    bskip_detail::kDefaultReadPolicy<LargeKey>,
    LeafStoragePolicy::Separate,
    LargeKeyPolicy>;

using SmallLeafLargeKeyList = ConcurrentBSkipList<
    LargeKey,
    /*PayloadType=*/void,
    /*B=*/4,
    bskip_detail::kDefaultReadPolicy<LargeKey>,
    LeafStoragePolicy::Separate,
    LargeKeyPolicy>;

struct OptimisticLargeKeyPolicy : ConcurrentBSkipDefaultPolicy<LargeKey> {
  using Comp = LargeKeyComp;
  using Hash = OptimisticLargeKeyHash;
};
using OptimisticLargeKeyList = ConcurrentBSkipList<
    LargeKey,
    /*PayloadType=*/void,
    /*B=*/16,
    KeyReadPolicy::RelaxedAtomic,
    LeafStoragePolicy::Separate,
    OptimisticLargeKeyPolicy>;

struct CheckedNonTrivialKeyPolicy
    : ConcurrentBSkipDefaultPolicy<CheckedNonTrivialKey> {
  using Comp = CheckedNonTrivialKeyComp;
};
using CheckedNonTrivialKeyList = ConcurrentBSkipList<
    CheckedNonTrivialKey,
    uint64_t,
    /*B=*/16,
    bskip_detail::kDefaultReadPolicy<CheckedNonTrivialKey>,
    LeafStoragePolicy::Separate,
    CheckedNonTrivialKeyPolicy>;

TEST(ConcurrentBSkipList, LargeKeyBasicOperations) {
  LargeKeyList list;

  for (int64_t i = 0; i < 500; ++i) {
    list.add({i, i * 10});
  }
  EXPECT_EQ(list.size(), 500);

  for (int64_t i = 0; i < 500; ++i) {
    EXPECT_TRUE(list.contains({i, i * 10}));
  }
  EXPECT_FALSE(list.contains(LargeKey{999, 999}));

  // Skipper
  LargeKeyList::Skipper skipper(list);
  auto pos = skipper.skipTo(LargeKey{100, 1000});
  ASSERT_TRUE(pos.has_value());
  EXPECT_EQ(pos->a, 100);

  std::vector<LargeKey> visited;
  scanWithForEachElement(list, [&](const LargeKey& k) {
    visited.push_back(k);
  });
  EXPECT_EQ(visited.size(), 500);
  for (size_t i = 1; i < visited.size(); ++i) {
    EXPECT_TRUE(LargeKeyComp{}(visited[i - 1], visited[i]));
  }
}

TEST(ConcurrentBSkipList, LargeKeySkipperAdvanceCrossesLeafBoundary) {
  SmallLeafLargeKeyList list;

  for (int64_t i = 0; i < 12; ++i) {
    list.add({i, i * 10});
  }

  SmallLeafLargeKeyList::Skipper skipper(list);
  auto pos = skipper.skipTo(LargeKey{2, 20});
  ASSERT_TRUE(pos.has_value());
  EXPECT_EQ(pos->a, 2);

  pos = skipper.advance();
  ASSERT_TRUE(pos.has_value());
  EXPECT_EQ(pos->a, 3);

  pos = skipper.advance();
  ASSERT_TRUE(pos.has_value());
  EXPECT_EQ(pos->a, 4);
}

TEST(ConcurrentBSkipList, LargeKeySkipperDoesNotRewindOnLowerTarget) {
  LargeKeyList list;

  for (int64_t i = 0; i < 10; ++i) {
    list.add({i, i * 10});
  }

  LargeKeyList::Skipper skipper(list);
  auto pos = skipper.skipTo(LargeKey{5, 50});
  ASSERT_TRUE(pos.has_value());
  EXPECT_EQ(pos->a, 5);

  pos = skipper.skipTo(LargeKey{3, 30});
  ASSERT_TRUE(pos.has_value());
  EXPECT_EQ(pos->a, 5);
}

TEST(ConcurrentBSkipList, LargeKeySkipperDeletedCurrentDoesNotRewind) {
  LargeKeyList list;

  for (int64_t i = 0; i < 10; ++i) {
    list.add({i, i * 10});
  }

  LargeKeyList::Skipper skipper(list);
  auto pos = skipper.skipTo(LargeKey{5, 50});
  ASSERT_TRUE(pos.has_value());
  EXPECT_EQ(pos->a, 5);

  EXPECT_TRUE(list.remove(LargeKey{5, 50}));

  // skipTo(target < lastKey_) returns the cached lastKey_ (monotonicity
  // guarantee). On platforms where LargeKey is hardware-atomic, the optimistic
  // path returns lastKey_ directly; on platforms without lock-free 16-byte
  // atomics, the locked path scans forward and may find the next live key.
  pos = skipper.skipTo(LargeKey{4, 40});
  ASSERT_TRUE(pos.has_value());
  EXPECT_GE(pos->a, 5);
}

TEST(ConcurrentBSkipList, CurrentDoesNotAdvanceOnOptimisticInvalidation) {
  OptimisticLargeKeyList list;
  list.add(LargeKey{42, 420});
  list.add(LargeKey{100, 1000});

  std::atomic<bool> stop{false};
  std::atomic<int64_t> wrongCurrent{0};

  std::thread writer([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      EXPECT_FALSE(list.addOrUpdate(LargeKey{100, 1000}, [](const LargeKey&) {
      }));
    }
  });

  std::thread reader([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      OptimisticLargeKeyList::Skipper skipper(list);
      auto pos = skipper.skipTo(LargeKey{42, 420});
      if (!pos || pos->a != 42) {
        wrongCurrent.fetch_add(1, std::memory_order_relaxed);
        continue;
      }

      pos = skipper.current();
      if (!pos || pos->a != 42) {
        wrongCurrent.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  std::this_thread::sleep_for( // NOLINT(facebook-hte-BadCall-sleep_for)
      std::chrono::seconds(2));
  stop.store(true, std::memory_order_relaxed);
  writer.join();
  reader.join();

  EXPECT_EQ(wrongCurrent.load(std::memory_order_relaxed), 0);
}

TEST(
    ConcurrentBSkipList,
    DeterministicSplitPublicationAdjacentProbeUsesAcquireLoad) {
  HookedAdjacentProbeList list;
  for (int64_t key : {10, 20, 30, 40, 50}) {
    EXPECT_TRUE(list.add(key));
  }

  HookedAdjacentProbeList::Skipper skipper(list);
  auto start = skipper.skipTo(int64_t{20});
  ASSERT_TRUE(start.has_value());
  EXPECT_EQ(*start, 20);

  BSkipHookState hookState;
  ScopedBSkipTestHook scopedHook(hookState);

  std::thread writer([&] { EXPECT_TRUE(list.add(15)); });

  bool sawSplitPublication = false;
  {
    std::unique_lock<std::mutex> lock(hookState.mutex);
    sawSplitPublication =
        hookState.cv.wait_for(lock, std::chrono::seconds(5), [&] {
          return hookState.splitPublished;
        });
  }
  EXPECT_TRUE(sawSplitPublication);

  auto pos = skipper.skipTo(int64_t{30});
  EXPECT_TRUE(pos.has_value());
  if (pos) {
    EXPECT_EQ(*pos, 30);
  }
  EXPECT_TRUE(
      hookState.sawProbeNextLoadAcquire.load(std::memory_order_relaxed));
  EXPECT_TRUE(
      hookState.sawProbeNextValidateAcquire.load(std::memory_order_relaxed));
  writer.join();
}

TEST(ConcurrentBSkipList, CachedOlcHorizontalTraversalUsesAcquireLoad) {
  HookedOlcTraversalList list;
  for (int64_t i = 1; i <= 5000; ++i) {
    EXPECT_TRUE(list.add(i));
  }

  HookedOlcTraversalList::Skipper skipper(list);
  auto start = skipper.skipTo(int64_t{8});
  ASSERT_TRUE(start.has_value());
  EXPECT_EQ(*start, 8);

  BSkipHookState hookState;
  ScopedBSkipTestHook scopedHook(hookState);

  auto pos = skipper.skipTo(int64_t{4900});
  EXPECT_TRUE(pos.has_value());
  if (pos) {
    EXPECT_EQ(*pos, 4900);
  }
  EXPECT_TRUE(hookState.sawOlcLoadAcquire.load(std::memory_order_relaxed));
}

TEST(ConcurrentBSkipList, RelaxedAtomicLeafReadsValidateAfterKeyLoad) {
  HookedRelaxedAtomicList list;
  for (uint32_t key : {10, 20, 30, 40}) {
    EXPECT_TRUE(list.add(key));
  }

  BSkipHookState hookState;
  ScopedBSkipTestHook scopedHook(hookState);

  HookedRelaxedAtomicList::Skipper skipper(list);
  auto pos = skipper.skipTo(uint32_t{30});
  ASSERT_TRUE(pos.has_value());
  EXPECT_EQ(*pos, 30u);
  EXPECT_TRUE(hookState.sawLeafKeyPostLoadValidateAcquire.load(
      std::memory_order_relaxed));
}

TEST(ConcurrentBSkipList, LockedOnlySkipperAvoidsInvalidCopies) {
  CheckedNonTrivialKey::resetCounters();
  CheckedNonTrivialKeyList list;

  for (int64_t id = 1; id <= 8; ++id) {
    list.add(CheckedNonTrivialKey{id}, uint64_t{0});
  }

  std::atomic<bool> stop{false};
  std::atomic<int64_t> invalidReturned{0};
  std::atomic<int64_t> lowerBoundViolations{0};
  std::atomic<int64_t> updateMisses{0};
  std::atomic<int64_t> missingResults{0};

  std::thread writer([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      for (int64_t id = 1; id <= 8; ++id) {
        if (!list.updatePayload(
                CheckedNonTrivialKey{id},
                [](const auto& key, uint64_t& generation) {
                  CheckedNonTrivialKey::widenRaceWindow();
                  EXPECT_TRUE(key.valid());
                  ++generation;
                })) {
          updateMisses.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
  });

  std::thread reader([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      CheckedNonTrivialKeyList::Skipper skipper(list);
      for (int64_t id = 1; id <= 8; ++id) {
        auto pos = skipper.skipTo(CheckedNonTrivialKey{id});
        if (!pos) {
          missingResults.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        if (!pos->valid()) {
          invalidReturned.fetch_add(1, std::memory_order_relaxed);
        }
        if (pos->id < id) {
          lowerBoundViolations.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
  });

  std::this_thread::sleep_for( // NOLINT(facebook-hte-BadCall-sleep_for)
      std::chrono::seconds(2));
  stop.store(true, std::memory_order_relaxed);
  writer.join();
  reader.join();

  EXPECT_EQ(updateMisses.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(missingResults.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(invalidReturned.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(lowerBoundViolations.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(
      CheckedNonTrivialKey::invalidCopies.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(
      CheckedNonTrivialKey::invalidDestructions.load(std::memory_order_relaxed),
      0);
}

// 2E: Multiple concurrent Skippers + writers on the same list.
TEST(ConcurrentBSkipList, MultipleConcurrentSkippers) {
  DefaultList list;

  constexpr int64_t kKeys = 10000;
  for (int64_t i = 0; i < kKeys; ++i) {
    list.add(i);
  }

  std::atomic<bool> stop{false};
  std::vector<std::thread> writers;
  writers.reserve(2);
  for (int w = 0; w < 2; ++w) {
    writers.emplace_back([&, w] {
      int64_t base = kKeys + w * 100000;
      while (!stop.load()) {
        list.add(base);
        list.remove(base);
        ++base;
      }
    });
  }

  std::atomic<int> violations{0};
  constexpr int kSkippers = 4;
  std::vector<std::thread> skipperThreads;
  skipperThreads.reserve(kSkippers);
  for (int s = 0; s < kSkippers; ++s) {
    skipperThreads.emplace_back([&, s] {
      for (int iter = 0; iter < 100; ++iter) {
        DefaultList::Skipper skipper(list);
        int64_t prev = -1;
        int64_t stride = 10 + s * 5;
        for (int64_t target = 0; target < kKeys; target += stride) {
          auto pos = skipper.skipTo(target);
          if (pos) {
            if (*pos < target || *pos <= prev) {
              violations.fetch_add(1);
            }
            prev = *pos;
          }
        }
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  for (auto& w : writers) {
    w.join();
  }
  for (auto& t : skipperThreads) {
    t.join();
  }

  EXPECT_EQ(violations.load(), 0)
      << "Multiple concurrent skippers saw non-monotonic results";
}

// 2F: Concurrent update() + Skipper — update modifies elements while
// Skippers read. Skipper should never see a value outside the valid range.
TEST(ConcurrentBSkipList, ConcurrentUpdateAndSkipper) {
  DefaultList list;

  constexpr int64_t kKeys = 5000;
  for (int64_t i = 0; i < kKeys; ++i) {
    list.add(i);
  }

  std::atomic<bool> stop{false};

  // Updater thread: cycles values through add/remove
  std::thread updater([&] {
    while (!stop.load()) {
      for (int64_t i = 0; i < kKeys && !stop.load(); i += 10) {
        list.addOrUpdate(i, [](const int64_t& val) {
          // Existing keys are observed in-place, but remain immutable.
          (void)val;
        });
      }
    }
  });

  std::atomic<int> violations{0};
  constexpr int kReaders = 4;
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&] {
      for (int iter = 0; iter < 200; ++iter) {
        DefaultList::Skipper skipper(list);
        int64_t prev = -1;
        for (int64_t target = 0; target < kKeys; target += 20) {
          auto pos = skipper.skipTo(target);
          if (pos) {
            if (*pos < target || *pos <= prev) {
              violations.fetch_add(1);
            }
            prev = *pos;
          }
        }
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  updater.join();
  for (auto& r : readers) {
    r.join();
  }

  EXPECT_EQ(violations.load(), 0)
      << "Skipper saw invalid value during concurrent updates";
}

// 2G: B=2 split stress — forces a split on every insert. 8 threads inserting.
// Under TSAN, catches lock ordering violations and data races.
TEST(ConcurrentBSkipList, B2SplitStress) {
  using TinyList = TestList<int64_t, 2, 2>;
  TinyList list;

  constexpr int kThreads = 8;
  constexpr int64_t kPerThread = 500;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int64_t i = 0; i < kPerThread; ++i) {
        list.add(t * kPerThread + i);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(list.size(), static_cast<size_t>(kThreads * kPerThread));
  for (int64_t i = 0; i < kThreads * kPerThread; ++i) {
    EXPECT_TRUE(list.contains(i)) << "Missing key " << i;
  }

  // Verify sorted iteration
  std::vector<int64_t> visited;
  scanWithForEachElement(list, [&](int64_t k) { visited.push_back(k); });
  EXPECT_EQ(visited.size(), static_cast<size_t>(kThreads * kPerThread));
  for (size_t i = 1; i < visited.size(); ++i) {
    EXPECT_LT(visited[i - 1], visited[i]);
  }
}

// ============================================================================
// Sentinel boundary tests — stress the edges where sentinels matter.
// Use B=4 to force splits with few elements.
// ============================================================================

using TinyList = TestList<int64_t, 4, 4, 3>;

TEST(SentinelBoundary, EmptyListIteration) {
  TinyList list;
  EXPECT_EQ(list.size(), 0);
  EXPECT_TRUE(list.empty());
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(scanWithForEachElement(list, [](int64_t) {}), 0);

  TinyList::Skipper skipper(list);
  EXPECT_FALSE(skipper.good());
  EXPECT_EQ(skipper.skipTo(int64_t{0}), std::nullopt);
  EXPECT_EQ(skipper.advance(), std::nullopt);
  EXPECT_EQ(skipper.current(), std::nullopt);
}

TEST(SentinelBoundary, SingleElementAllPaths) {
  TinyList list;
  list.add(42);
  EXPECT_EQ(list.size(), 1);

  // contains / find
  EXPECT_TRUE(list.contains(42));
  EXPECT_EQ(list.find(42), 42);
  EXPECT_FALSE(list.contains(41));
  EXPECT_FALSE(list.contains(43));

  std::vector<int64_t> feKeys;
  size_t count = scanWithForEachElement(list, [&](int64_t k) {
    feKeys.push_back(k);
  });
  EXPECT_EQ(count, 1);
  EXPECT_EQ(feKeys, std::vector<int64_t>{42});

  // Skipper
  TinyList::Skipper skipper(list);
  EXPECT_TRUE(skipper.good());
  auto r = skipper.skipTo(int64_t{42});
  EXPECT_TRUE(r.has_value());
  EXPECT_EQ(*r, 42);
  EXPECT_EQ(skipper.advance(), std::nullopt);
  EXPECT_FALSE(skipper.good());

  // Remove and verify empty
  EXPECT_TRUE(list.remove(42));
  EXPECT_EQ(list.size(), 0);
  EXPECT_FALSE(list.contains(42));
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(scanWithForEachElement(list, [](int64_t) {}), 0);
}

TEST(SentinelBoundary, SkipToLastElement) {
  TinyList list;
  for (auto k : {int64_t{10}, int64_t{20}, int64_t{30}}) {
    list.add(k);
  }

  TinyList::Skipper skipper(list);
  auto r = skipper.skipTo(int64_t{30});
  EXPECT_TRUE(r.has_value());
  EXPECT_EQ(*r, 30);

  // Past last element
  r = skipper.skipTo(int64_t{31});
  EXPECT_FALSE(r.has_value());
  EXPECT_FALSE(skipper.good());

  // New skipper, skip directly past end
  TinyList::Skipper skipper2(list);
  r = skipper2.skipTo(std::numeric_limits<int64_t>::max() - 1);
  EXPECT_FALSE(r.has_value());
  EXPECT_FALSE(skipper2.good());
}

TEST(SentinelBoundary, SkipToBeyondEnd) {
  DefaultList list;
  for (int64_t i = 1; i <= 100; ++i) {
    list.add(i);
  }

  DefaultList::Skipper skipper(list);
  // Skip to last
  auto r = skipper.skipTo(int64_t{100});
  EXPECT_TRUE(r.has_value());
  EXPECT_EQ(*r, 100);

  // Skip past
  r = skipper.skipTo(int64_t{101});
  EXPECT_FALSE(r.has_value());
  EXPECT_FALSE(skipper.good());

  // Skip way past
  DefaultList::Skipper skipper2(list);
  r = skipper2.skipTo(int64_t{999999});
  EXPECT_FALSE(r.has_value());
  EXPECT_FALSE(skipper2.good());
}

TEST(SentinelBoundary, IterateAcrossSplits) {
  TinyList list; // B=4, so 5th insert forces a split
  std::vector<int64_t> expected;
  for (int64_t i = 1; i <= 20; ++i) {
    list.add(i * 10);
    expected.push_back(i * 10);
  }

  std::vector<int64_t> feKeys;
  scanWithForEachElement(list, [&](int64_t k) { feKeys.push_back(k); });
  EXPECT_EQ(feKeys, expected);

  // Verify no sentinel values leaked
  for (auto k : feKeys) {
    EXPECT_NE(k, std::numeric_limits<int64_t>::min());
    EXPECT_NE(k, std::numeric_limits<int64_t>::max());
  }
}

TEST(SentinelBoundary, SequentialScanMatchesHelper) {
  DefaultList list;
  std::mt19937 rng(777);
  for (int i = 0; i < 5000; ++i) {
    list.add(static_cast<int64_t>(rng() % 100000));
  }

  std::vector<int64_t> feKeys;
  scanWithForEachElement(list, [&](int64_t k) { feKeys.push_back(k); });
  EXPECT_EQ(feKeys.size(), list.size());
}

TEST(SentinelBoundary, SkipperForwardToEveryKey) {
  DefaultList list;
  std::set<int64_t> keySet;
  std::mt19937 rng(888);
  while (keySet.size() < 1000) {
    keySet.insert(static_cast<int64_t>(rng() % 100000) + 1);
  }
  std::vector<int64_t> keys(keySet.begin(), keySet.end());
  for (auto k : keys) {
    list.add(k);
  }

  DefaultList::Skipper skipper(list);
  for (auto k : keys) {
    auto r = skipper.skipTo(k);
    ASSERT_TRUE(r.has_value()) << "skipTo(" << k << ") failed";
    EXPECT_EQ(*r, k);
  }

  // Past last key
  auto r = skipper.skipTo(std::numeric_limits<int64_t>::max() - 1);
  EXPECT_FALSE(r.has_value());
  EXPECT_FALSE(skipper.good());
}

TEST(SentinelBoundary, InsertDeleteNearSentinelValues) {
  DefaultList list;
  constexpr int64_t nearMin = std::numeric_limits<int64_t>::min() + 1;
  constexpr int64_t nearMax = std::numeric_limits<int64_t>::max() - 1;

  list.add(nearMin);
  list.add(nearMax);
  list.add(int64_t{0});

  EXPECT_TRUE(list.contains(nearMin));
  EXPECT_TRUE(list.contains(nearMax));
  EXPECT_TRUE(list.contains(0));
  EXPECT_EQ(list.size(), 3);

  std::vector<int64_t> keys;
  scanWithForEachElement(list, [&](int64_t k) { keys.push_back(k); });
  EXPECT_EQ(keys.size(), 3);
  EXPECT_EQ(keys[0], nearMin);
  EXPECT_EQ(keys[2], nearMax);

  // Skipper
  DefaultList::Skipper skipper(list);
  auto r = skipper.skipTo(nearMin);
  EXPECT_TRUE(r.has_value());
  EXPECT_EQ(*r, nearMin);
  r = skipper.skipTo(nearMax);
  EXPECT_TRUE(r.has_value());
  EXPECT_EQ(*r, nearMax);

  // Delete near-sentinel keys
  EXPECT_TRUE(list.remove(nearMin));
  EXPECT_TRUE(list.remove(nearMax));
  EXPECT_FALSE(list.contains(nearMin));
  EXPECT_FALSE(list.contains(nearMax));
  EXPECT_EQ(list.size(), 1);
}

TEST(SentinelBoundary, ConcurrentSkipperAtEnd) {
  DefaultList list;
  for (int64_t i = 1; i <= 1000; ++i) {
    list.add(i);
  }

  constexpr int64_t kMinSent = std::numeric_limits<int64_t>::min();
  constexpr int64_t kMaxSent = std::numeric_limits<int64_t>::max();

  std::atomic<bool> stop{false};
  std::atomic<int64_t> sentinelLeaks{0};

  std::vector<std::thread> threads;
  threads.reserve(6);
  // Readers: create skippers and skipTo near/past end
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&list, &stop, &sentinelLeaks]() {
      while (!stop.load()) {
        DefaultList::Skipper skipper(list);
        auto r = skipper.skipTo(int64_t{999});
        if (r.has_value() && (*r == kMinSent || *r == kMaxSent)) {
          sentinelLeaks.fetch_add(1);
        }
        // skipTo past all original keys — may find concurrent inserts
        r = skipper.skipTo(int64_t{1001});
        if (r.has_value() && (*r == kMinSent || *r == kMaxSent)) {
          sentinelLeaks.fetch_add(1);
        }
      }
    });
  }
  // Writers: insert/delete at the high end
  for (int t = 0; t < 2; ++t) {
    threads.emplace_back([&list, &stop, t]() {
      int64_t key = 2000 + t * 10000;
      while (!stop.load()) {
        list.add(key);
        list.remove(key);
        key++;
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  for (auto& t : threads) {
    t.join();
  }
  EXPECT_EQ(sentinelLeaks.load(), 0);
}

TEST(SentinelBoundary, TinyListB4) {
  TinyList list; // B=4, kMaxHeight=3

  // 2 elements — fits in one node
  list.add(10);
  list.add(20);
  EXPECT_TRUE(list.contains(10));
  EXPECT_TRUE(list.contains(20));
  EXPECT_EQ(list.size(), 2);

  // 5 elements — forces a split
  list.add(30);
  list.add(40);
  list.add(50);
  EXPECT_EQ(list.size(), 5);

  std::vector<int64_t> keys;
  scanWithForEachElement(list, [&](int64_t k) { keys.push_back(k); });
  EXPECT_EQ(keys, (std::vector<int64_t>{10, 20, 30, 40, 50}));

  // Delete all
  for (auto k : keys) {
    EXPECT_TRUE(list.remove(k));
  }
  EXPECT_EQ(list.size(), 0);
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(scanWithForEachElement(list, [](int64_t) {}), 0);

  // Re-insert after delete
  list.add(99);
  EXPECT_TRUE(list.contains(99));
  EXPECT_EQ(list.size(), 1);
}

TEST(SentinelBoundary, SkipperScanDuringInsertAtEnd) {
  DefaultList list;
  for (int64_t i = 1; i <= 1000; ++i) {
    list.add(i);
  }

  std::atomic<bool> stop{false};
  std::atomic<int64_t> sentinelLeaks{0};

  constexpr int64_t kMinSentinel = std::numeric_limits<int64_t>::min();
  constexpr int64_t kMaxSentinel = std::numeric_limits<int64_t>::max();

  // Reader: full scan with skipper, check no sentinel values
  std::thread reader([&]() {
    while (!stop.load()) {
      scanWithSkipper(list, [&](int64_t k) {
        if (k == kMinSentinel || k == kMaxSentinel) {
          sentinelLeaks.fetch_add(1);
        }
      });
    }
  });

  // Writer: insert at high end
  std::thread writer([&]() {
    int64_t key = 100000;
    while (!stop.load()) {
      list.add(key++);
    }
  });

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  reader.join();
  writer.join();
  EXPECT_EQ(sentinelLeaks.load(), 0);
}

// ============================================================================
// Lazy level growth tests — verify correctness when internal levels are
// created on demand during insertion.
// ============================================================================

// Use P=2 (high promotion probability) to force level growth with fewer keys.
using HighPromoList = TestList<int64_t, 4, 2, 5>;

TEST(LazyGrowth, GrowthOnPromotion) {
  HighPromoList list; // P=2, B=4 — most keys promote, small nodes split fast

  // Insert enough keys to force growth to multiple levels.
  // With P=2, ~50% of keys promote to level 1, ~25% to level 2, etc.
  for (int64_t i = 1; i <= 100; ++i) {
    list.add(i);
  }

  // Verify all keys are findable
  for (int64_t i = 1; i <= 100; ++i) {
    EXPECT_TRUE(list.contains(i)) << "Missing key " << i;
  }

  std::vector<int64_t> feKeys;
  scanWithForEachElement(list, [&](int64_t k) { feKeys.push_back(k); });
  EXPECT_EQ(feKeys.size(), 100);

  // Verify Skipper finds all keys
  HighPromoList::Skipper skipper(list);
  for (int64_t i = 1; i <= 100; ++i) {
    auto r = skipper.skipTo(i);
    ASSERT_TRUE(r.has_value()) << "Skipper missed key " << i;
    EXPECT_EQ(*r, i);
  }
}

TEST(LazyGrowth, ConcurrentGrowth) {
  DefaultList list;

  // 4 writer threads inserting disjoint key ranges simultaneously.
  // Some keys will promote and trigger concurrent ensureHeightAbove calls.
  std::vector<std::thread> threads;
  threads.reserve(4);
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&list, t]() {
      for (int64_t i = 0; i < 10000; ++i) {
        list.add(static_cast<int64_t>(t * 100000 + i));
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // Verify all keys exist
  for (int t = 0; t < 4; ++t) {
    for (int64_t i = 0; i < 10000; ++i) {
      EXPECT_TRUE(list.contains(static_cast<int64_t>(t * 100000 + i)));
    }
  }
  EXPECT_EQ(list.size(), 40000);
}

TEST(LazyGrowth, ReaderDuringGrowth) {
  DefaultList list;

  // Pre-populate with some keys so readers have something to find.
  for (int64_t i = 1; i <= 100; ++i) {
    list.add(i);
  }

  std::atomic<bool> stop{false};
  std::atomic<int64_t> missedKeys{0};

  // Readers: verify pre-populated keys are always findable
  std::vector<std::thread> threads;
  threads.reserve(5);
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&list, &stop, &missedKeys]() {
      while (!stop.load()) {
        for (int64_t i = 1; i <= 100; ++i) {
          if (!list.contains(i)) {
            missedKeys.fetch_add(1);
          }
        }
      }
    });
  }

  // Writer: insert keys that force tree growth
  threads.emplace_back([&list, &stop]() {
    int64_t key = 1000;
    while (!stop.load()) {
      list.add(key++);
    }
  });

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  for (auto& t : threads) {
    t.join();
  }
  EXPECT_EQ(missedKeys.load(), 0);
}

TEST(LazyGrowth, SkipperAcrossGrowth) {
  DefaultList list;

  // Start with a few keys — tree is at height 1 (level 0 only for small N)
  for (int64_t i = 1; i <= 10; ++i) {
    list.add(i * 10);
  }

  // Create Skipper at current (small) height
  DefaultList::Skipper skipper(list);

  // Skipper should find all keys
  for (int64_t i = 1; i <= 10; ++i) {
    auto r = skipper.skipTo(i * 10);
    ASSERT_TRUE(r.has_value()) << "Skipper missed key " << i * 10;
    EXPECT_EQ(*r, i * 10);
  }

  // Now grow the tree by inserting many more keys
  for (int64_t i = 200; i <= 10000; ++i) {
    list.add(i);
  }

  // Skipper (with old cached height) should still work correctly
  auto r = skipper.skipTo(int64_t{5000});
  EXPECT_TRUE(r.has_value());
  EXPECT_EQ(*r, 5000);

  r = skipper.skipTo(int64_t{9999});
  EXPECT_TRUE(r.has_value());
  EXPECT_EQ(*r, 9999);
}

TEST(LazyGrowth, GrowthToMaxHeight) {
  // kPromotionProbInverse=2 with enough keys guarantees growth to kMaxHeight
  HighPromoList list;

  // With kPromotionProbInverse=2, kMaxHeight=5: need ~2^5 = 32 keys to likely
  // reach level 4. Insert 1000 to be sure.
  for (int64_t i = 1; i <= 1000; ++i) {
    list.add(i);
  }

  // All keys findable
  for (int64_t i = 1; i <= 1000; ++i) {
    EXPECT_TRUE(list.contains(i));
  }

  // Verify iteration produces sorted unique output
  std::vector<int64_t> keys;
  scanWithForEachElement(list, [&](int64_t k) { keys.push_back(k); });
  EXPECT_EQ(keys.size(), 1000);
  for (size_t i = 1; i < keys.size(); ++i) {
    EXPECT_LT(keys[i - 1], keys[i]);
  }
}

TEST(LazyGrowth, DestructorFreesGrown) {
  // Just verify no crashes/leaks (ASAN will catch leaks).
  {
    DefaultList list;
    for (int64_t i = 1; i <= 10000; ++i) {
      list.add(i);
    }
    // Destructor runs here — must free heap-allocated internal sentinels
    // plus all user data nodes.
  }
  // If we get here without ASAN complaints, we're good.
}

// ============================================================================
// Large key concurrent tests (sizeof > 8, all reads through shared locks)
// ============================================================================

TEST(ConcurrentBSkipList, ConcurrentReadsAndWrites) {
  LargeKeyList list;

  constexpr int kStable = 2000;
  constexpr int kTransient = 2000;
  for (int i = 0; i < kStable; ++i) {
    list.add(LargeKey{static_cast<int64_t>(i * 2), 0});
  }
  for (int i = 0; i < kTransient; ++i) {
    list.add(LargeKey{static_cast<int64_t>(i * 2 + 1), 0});
  }

  std::vector<LargeKey> stableKeys;
  stableKeys.reserve(kStable);
  for (int i = 0; i < kStable; ++i) {
    stableKeys.push_back(LargeKey{static_cast<int64_t>(i * 2), 0});
  }
  std::sort(stableKeys.begin(), stableKeys.end(), LargeKeyComp{});

  std::atomic<bool> stop{false};
  std::atomic<int64_t> missed{0};
  std::vector<std::thread> threads;
  threads.reserve(4);

  for (int w = 0; w < 2; ++w) {
    threads.emplace_back([&list, &stop, w]() {
      std::mt19937 rng(42 + w);
      while (!stop.load(std::memory_order_relaxed)) {
        int key = (rng() % kTransient) * 2 + 1;
        if (rng() % 2 == 0) {
          list.remove(LargeKey{static_cast<int64_t>(key), 0});
        } else {
          list.add(LargeKey{static_cast<int64_t>(key), 0});
        }
      }
    });
  }

  for (int r = 0; r < 2; ++r) {
    threads.emplace_back([&list, &stableKeys, &stop, &missed]() {
      while (!stop.load(std::memory_order_relaxed)) {
        LargeKeyList::Skipper skipper(list);
        for (const auto& target : stableKeys) {
          auto pos = skipper.skipTo(target);
          if (!pos || !(*pos == target)) {
            missed.fetch_add(1);
          }
        }
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(2));
  stop.store(true);
  for (auto& t : threads) {
    t.join();
  }
  EXPECT_EQ(missed.load(), 0)
      << "Stable keys missed " << missed.load()
      << " times during concurrent writes";
}

TEST(ConcurrentBSkipList, SequentialScanCompleteness) {
  LargeKeyList list;
  std::set<LargeKey, LargeKeyComp> expected;
  for (int i = 1; i <= 500; ++i) {
    LargeKey k{static_cast<int64_t>(i), static_cast<int64_t>(i * 3)};
    list.add(k);
    expected.insert(k);
  }

  std::set<LargeKey, LargeKeyComp> got;
  scanWithForEachElement(list, [&](const LargeKey& k) { got.insert(k); });
  EXPECT_EQ(got, expected);
}

// ============================================================================
// Concurrent forEachElement tests
// ============================================================================

TEST(ConcurrentBSkipList, ForEachElementConcurrentInserts) {
  DefaultList list;

  constexpr int64_t kStableKeys = 5000;
  for (int64_t i = 0; i < kStableKeys; ++i) {
    list.add(i);
  }

  std::atomic<bool> stop{false};

  constexpr int kWriters = 4;
  std::vector<std::thread> writers;
  writers.reserve(kWriters);
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w] {
      int64_t base = kStableKeys + w * 100000;
      while (!stop.load(std::memory_order_relaxed)) {
        list.add(base++);
      }
    });
  }

  std::atomic<int> orderViolations{0};
  std::atomic<int> sentinelLeaks{0};
  constexpr int64_t kMinSentinel = std::numeric_limits<int64_t>::min();
  constexpr int64_t kMaxSentinel = std::numeric_limits<int64_t>::max();

  constexpr int kReaders = 4;
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&] {
      for (int iter = 0; iter < 200; ++iter) {
        int64_t prev = -1;
        size_t stableCount = 0;
        list.forEachElement([&](int64_t k) {
          if (k == kMinSentinel || k == kMaxSentinel) {
            sentinelLeaks.fetch_add(1);
          }
          if (k <= prev) {
            orderViolations.fetch_add(1);
          }
          prev = k;
          if (k < kStableKeys) {
            ++stableCount;
          }
        });
        EXPECT_GE(stableCount, static_cast<size_t>(kStableKeys - 1));
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  for (auto& w : writers) {
    w.join();
  }
  for (auto& r : readers) {
    r.join();
  }

  EXPECT_EQ(orderViolations.load(), 0)
      << "forEachElement returned out-of-order keys during concurrent inserts";
  EXPECT_EQ(sentinelLeaks.load(), 0) << "forEachElement leaked sentinel values";
}

TEST(ConcurrentBSkipList, ForEachElementConcurrentTombstoneChurn) {
  DefaultList list;

  constexpr int64_t kKeys = 10000;
  for (int64_t i = 0; i < kKeys; ++i) {
    list.add(i);
  }

  std::atomic<bool> stop{false};

  constexpr int kChurners = 4;
  std::vector<std::thread> churners;
  churners.reserve(kChurners);
  for (int c = 0; c < kChurners; ++c) {
    churners.emplace_back([&, c] {
      std::mt19937_64 rng(500 + c);
      while (!stop.load(std::memory_order_relaxed)) {
        int64_t key = static_cast<int64_t>(rng() % kKeys);
        list.remove(key);
        list.add(key);
      }
    });
  }

  std::atomic<int> orderViolations{0};
  constexpr int kReaders = 4;
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&] {
      for (int iter = 0; iter < 200; ++iter) {
        int64_t prev = -1;
        list.forEachElement([&](int64_t k) {
          if (k <= prev) {
            orderViolations.fetch_add(1);
          }
          prev = k;
        });
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  for (auto& c : churners) {
    c.join();
  }
  for (auto& r : readers) {
    r.join();
  }

  EXPECT_EQ(orderViolations.load(), 0)
      << "forEachElement returned out-of-order keys during tombstone churn";
}

TEST(ConcurrentBSkipList, ForEachElementConcurrentSplitStressB4) {
  using SmallList = TestList<int64_t, 4, 4>;
  SmallList list;

  constexpr int64_t kInitial = 2000;
  for (int64_t i = 0; i < kInitial; ++i) {
    list.add(i);
  }

  std::atomic<bool> stop{false};

  constexpr int kWriters = 4;
  std::vector<std::thread> writers;
  writers.reserve(kWriters);
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w] {
      int64_t base = kInitial + w * 100000;
      while (!stop.load(std::memory_order_relaxed)) {
        list.add(base++);
      }
    });
  }

  std::atomic<int> orderViolations{0};
  constexpr int kReaders = 4;
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&] {
      for (int iter = 0; iter < 200; ++iter) {
        int64_t prev = -1;
        list.forEachElement([&](int64_t k) {
          if (k <= prev) {
            orderViolations.fetch_add(1);
          }
          prev = k;
        });
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  for (auto& w : writers) {
    w.join();
  }
  for (auto& r : readers) {
    r.join();
  }

  EXPECT_EQ(orderViolations.load(), 0)
      << "forEachElement with B=4 returned out-of-order keys during splits";
}

} // namespace
} // namespace folly

// ============================================================================
// Torn-read regression test: proves KeysAtomicallyCopyable=true is unsafe
// for keys > 8 bytes. Uses a 16-byte key with non-standard sort order
// (reverse on first field) to amplify torn-read effects.
// ============================================================================
namespace folly::bskip_torn_read_test {

struct RevKey {
  static constexpr int64_t kChecksumSalt = 0x5a5a5a5a5a5a5a5aULL;

  int64_t a;
  int64_t b;

  static constexpr int64_t checksumFor(int64_t a) { return a ^ kChecksumSalt; }

  constexpr RevKey() : a(0), b(checksumFor(0)) {}
  constexpr explicit RevKey(int64_t a_) : a(a_), b(checksumFor(a_)) {}
  constexpr RevKey(int64_t a_, int64_t b_) : a(a_), b(b_) {}

  bool operator==(const RevKey& o) const { return a == o.a; }
  bool operator!=(const RevKey& o) const { return !(*this == o); }

  bool valid() const { return b == checksumFor(a); }
};

static_assert(sizeof(RevKey) == 16);
static_assert(std::is_trivially_copyable_v<RevKey>);

struct RevKeyComp {
  bool operator()(const RevKey& x, const RevKey& y) const { return x.a > y.a; }
};

} // namespace folly::bskip_torn_read_test

namespace std {
template <>
struct numeric_limits<folly::bskip_torn_read_test::RevKey> {
  static constexpr bool is_specialized = true;
  static constexpr folly::bskip_torn_read_test::RevKey min() {
    return folly::bskip_torn_read_test::RevKey(
        std::numeric_limits<int64_t>::max(),
        std::numeric_limits<int64_t>::min());
  }
  static constexpr folly::bskip_torn_read_test::RevKey max() {
    return folly::bskip_torn_read_test::RevKey(
        std::numeric_limits<int64_t>::min(),
        std::numeric_limits<int64_t>::max());
  }
};
template <>
struct hash<folly::bskip_torn_read_test::RevKey> {
  size_t operator()(const folly::bskip_torn_read_test::RevKey& k) const {
    return hash<int64_t>{}(k.a);
  }
};
} // namespace std

namespace folly::bskip_torn_read_test {

// Default KeyReadPolicy::Locked for 16-byte keys. All reads use shared
// locks.
struct RevKeyPolicy : ConcurrentBSkipDefaultPolicy<RevKey> {
  using Comp = RevKeyComp;
};
using SafeList = ConcurrentBSkipList<
    RevKey,
    /*PayloadType=*/void,
    /*B=*/16,
    bskip_detail::kDefaultReadPolicy<RevKey>,
    LeafStoragePolicy::Separate,
    RevKeyPolicy>;

struct RevKeySplitHeavyPolicy : RevKeyPolicy {
  static constexpr uint8_t kPromotionProbInverse = 2;
  static constexpr uint8_t kMaxHeight = 8;
};
using SafeSplitHeavyList = ConcurrentBSkipList<
    RevKey,
    /*PayloadType=*/void,
    /*B=*/4,
    bskip_detail::kDefaultReadPolicy<RevKey>,
    LeafStoragePolicy::Separate,
    RevKeySplitHeavyPolicy>;

struct StressResult {
  int64_t missed = 0;
  int64_t corrupted = 0;
};

template <typename ListType>
int64_t runConcurrentSkipperStress(int seconds) {
  ListType list;

  constexpr int kStable = 5000;
  constexpr int kTransient = 5000;
  // Even = stable, odd = transient
  for (int i = 2; i < (kStable + kTransient) * 2 + 2; i += 2) {
    list.add(RevKey(i));
  }
  for (int i = 3; i < (kStable + kTransient) * 2 + 2; i += 2) {
    list.add(RevKey(i));
  }

  std::vector<RevKey> stableKeys;
  stableKeys.reserve(kStable + kTransient);
  for (int i = 2; i < (kStable + kTransient) * 2 + 2; i += 2) {
    stableKeys.emplace_back(i);
  }
  std::sort(stableKeys.begin(), stableKeys.end(), RevKeyComp{});

  std::atomic<bool> stop{false};
  std::atomic<int64_t> missed{0};
  std::vector<std::thread> threads;
  threads.reserve(8);

  for (int w = 0; w < 4; ++w) {
    threads.emplace_back([&list, &stop, w]() {
      std::mt19937 rng(42 + w);
      while (!stop.load(std::memory_order_relaxed)) {
        int key = (rng() % ((kStable + kTransient) * 2)) + 2;
        key = key | 1;
        if (rng() % 2 == 0) {
          list.remove(RevKey(key));
        } else {
          list.add(RevKey(key));
        }
      }
    });
  }

  for (int r = 0; r < 4; ++r) {
    threads.emplace_back([&list, &stableKeys, &stop, &missed]() {
      while (!stop.load(std::memory_order_relaxed)) {
        typename ListType::Skipper skipper(list);
        for (const auto& target : stableKeys) {
          auto pos = skipper.skipTo(target);
          if (!pos || *pos != target) {
            missed.fetch_add(1);
          }
        }
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  stop.store(true);
  for (auto& t : threads) {
    t.join();
  }
  return missed.load();
}

template <typename ListType>
StressResult runSplitHeavySkipperStress(int seconds) {
  ListType list;

  // Under unoptimized + !NDEBUG + TSAN, the single-threaded setup loop alone
  // can crawl toward the CI timeout at kStable=20000 with P=2 aggressive height
  // growth forcing many splits per insert. Scale it down under sanitizers;
  // non-sanitizer runs keep full coverage.
  constexpr int kStable = folly::kIsSanitizeThread ? 2000 : 20000;
  constexpr int kTransient = 200000;
  for (int i = 0; i < kStable; ++i) {
    list.add(RevKey(2 + i * 2));
  }

  std::vector<RevKey> stableKeys;
  stableKeys.reserve(kStable);
  for (int i = 0; i < kStable; ++i) {
    stableKeys.emplace_back(2 + i * 2);
  }
  std::sort(stableKeys.begin(), stableKeys.end(), RevKeyComp{});

  std::atomic<int> nextOdd{3};
  std::atomic<bool> stop{false};
  std::atomic<int64_t> missed{0};
  std::atomic<int64_t> corrupted{0};

  std::vector<std::thread> threads;
  threads.reserve(folly::kIsSanitizeThread ? 4 : 12);

  for (int w = 0; w < (folly::kIsSanitizeThread ? 2 : 4); ++w) {
    threads.emplace_back([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        int key = nextOdd.fetch_add(2, std::memory_order_relaxed);
        if (key >= 3 + kTransient * 2) {
          break;
        }
        list.add(RevKey(key));
      }
    });
  }

  for (int r = 0; r < (folly::kIsSanitizeThread ? 2 : 8); ++r) {
    threads.emplace_back([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        typename ListType::Skipper skipper(list);
        for (const auto& target : stableKeys) {
          auto pos = skipper.skipTo(target);
          if (!pos || *pos != target) {
            missed.fetch_add(1, std::memory_order_relaxed);
          } else if (!pos->valid()) {
            corrupted.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
    });
  }

  std::this_thread::sleep_for( // NOLINT(facebook-hte-BadCall-sleep_for)
      std::chrono::seconds(seconds));
  stop.store(true, std::memory_order_relaxed);
  for (auto& t : threads) {
    t.join();
  }
  return {
      missed.load(std::memory_order_relaxed),
      corrupted.load(std::memory_order_relaxed)};
}

static_assert(sizeof(RevKey) > 8);

TEST(ConcurrentBSkipList, SafeLockedReadsZeroMisses) {
  // KeyReadPolicy::Locked (the default for sizeof > 8): all reads use
  // shared locks. Must produce zero misses under concurrent writes.
  auto missed = runConcurrentSkipperStress<SafeList>(3);
  LOG(INFO) << "Safe (KeyReadPolicy::Locked, 16B key): missed=" << missed;
  EXPECT_EQ(missed, 0);
}

TEST(ConcurrentBSkipList, SafeLockedSplitHeavyReadsZeroMisses) {
  auto result = runSplitHeavySkipperStress<SafeSplitHeavyList>(
      folly::kIsSanitizeThread ? 2 : 3);
  LOG(INFO) << "Safe split-heavy (KeyReadPolicy::Locked, 16B key): missed="
            << result.missed << " corrupted=" << result.corrupted;
  EXPECT_EQ(result.missed, 0);
  EXPECT_EQ(result.corrupted, 0);
}

} // namespace folly::bskip_torn_read_test

namespace folly::bskip_versioned_large_key_test {

struct VersionedSplitHeavyPolicy : ConcurrentBSkipDefaultPolicy<VersionedKey> {
  static constexpr uint8_t kPromotionProbInverse = 2;
  static constexpr uint8_t kMaxHeight = 8;
};
using SafeVersionedSplitHeavyList = ConcurrentBSkipList<
    VersionedKey,
    /*PayloadType=*/void,
    /*B=*/4,
    KeyReadPolicy::Locked,
    LeafStoragePolicy::Separate,
    VersionedSplitHeavyPolicy>;

struct OptimisticVersionedSplitHeavyPolicy : VersionedSplitHeavyPolicy {
  using Hash = OptimisticVersionedKeyHash;
};
using OptimisticVersionedSplitHeavyList = ConcurrentBSkipList<
    VersionedKey,
    /*PayloadType=*/void,
    /*B=*/4,
    KeyReadPolicy::RelaxedAtomic,
    LeafStoragePolicy::Separate,
    OptimisticVersionedSplitHeavyPolicy>;

struct VersionedKeyStressResult {
  int64_t missed = 0;
  int64_t lowerBoundViolations = 0;
  int64_t mismatchedFields = 0;
};

VersionedKey makeVersionedKey(int64_t v) {
  return VersionedKey{v, v};
}

template <typename ListType>
VersionedKeyStressResult runVersionedSplitHeavySkipperStress(int seconds) {
  ListType list;

  // Under unoptimized + !NDEBUG + TSAN, the single-threaded setup loop alone
  // can crawl toward the CI timeout at kStable=20000 with P=2 aggressive height
  // growth forcing many splits per insert. Scale it down under sanitizers;
  // non-sanitizer runs keep full coverage.
  constexpr int kStable = folly::kIsSanitizeThread ? 2000 : 20000;
  constexpr int kTransient = 400000;
  for (int i = 0; i < kStable; ++i) {
    list.add(makeVersionedKey(2 + i * 2));
  }

  std::vector<VersionedKey> stableKeys;
  stableKeys.reserve(kStable);
  for (int i = 0; i < kStable; ++i) {
    stableKeys.push_back(makeVersionedKey(2 + i * 2));
  }
  std::sort(stableKeys.begin(), stableKeys.end());

  // VersionedKey sorts by descending timestamp. Driving the transient
  // sequence downward keeps every new key on the current right edge, so
  // repeated inserts keep splitting the same tip instead of diffusing across
  // the tree.
  std::atomic<int64_t> nextOdd{1};
  std::atomic<bool> stop{false};
  std::atomic<int64_t> missed{0};
  std::atomic<int64_t> lowerBoundViolations{0};
  std::atomic<int64_t> mismatchedFields{0};

  std::vector<std::thread> threads;
  threads.reserve(folly::kIsSanitizeThread ? 4 : 32);

  for (int w = 0; w < (folly::kIsSanitizeThread ? 2 : 4); ++w) {
    threads.emplace_back([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        int64_t key = nextOdd.fetch_sub(2, std::memory_order_relaxed);
        if (key <= 1 - static_cast<int64_t>(kTransient) * 2) {
          break;
        }
        list.add(makeVersionedKey(key));
      }
    });
  }

  for (int r = 0; r < (folly::kIsSanitizeThread ? 2 : 8); ++r) {
    threads.emplace_back([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        typename ListType::Skipper skipper(list);
        for (const auto& target : stableKeys) {
          auto pos = skipper.skipTo(target);
          if (!pos || *pos != target) {
            missed.fetch_add(1, std::memory_order_relaxed);
            if (pos && *pos < target) {
              lowerBoundViolations.fetch_add(1, std::memory_order_relaxed);
            }
          }
          if (pos && pos->timestamp != pos->id) {
            mismatchedFields.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
    });
  }

  std::this_thread::sleep_for( // NOLINT(facebook-hte-BadCall-sleep_for)
      std::chrono::seconds(seconds));
  stop.store(true, std::memory_order_relaxed);
  for (auto& t : threads) {
    t.join();
  }
  return {
      missed.load(std::memory_order_relaxed),
      lowerBoundViolations.load(std::memory_order_relaxed),
      mismatchedFields.load(std::memory_order_relaxed)};
}

TEST(
    ConcurrentBSkipList, SafeLockedVersionedLargeKeySplitHeavyReadsZeroMisses) {
  auto result =
      runVersionedSplitHeavySkipperStress<SafeVersionedSplitHeavyList>(
          folly::kIsSanitizeThread ? 2 : 3);
  LOG(INFO)
      << "Safe split-heavy versioned 16B key (KeyReadPolicy::Locked): missed="
      << result.missed
      << " lowerBoundViolations=" << result.lowerBoundViolations
      << " mismatchedFields=" << result.mismatchedFields;
  EXPECT_EQ(result.missed, 0);
  EXPECT_EQ(result.lowerBoundViolations, 0);
  EXPECT_EQ(result.mismatchedFields, 0);
}

TEST(
    ConcurrentBSkipList, OptimisticVersionedLargeKeySplitHeavyReadsZeroMisses) {
  auto result =
      runVersionedSplitHeavySkipperStress<OptimisticVersionedSplitHeavyList>(
          folly::kIsSanitizeThread ? 2 : 3);
  LOG(INFO)
      << "Optimistic split-heavy versioned 16B key (RelaxedAtomic): missed="
      << result.missed
      << " lowerBoundViolations=" << result.lowerBoundViolations
      << " mismatchedFields=" << result.mismatchedFields;
  EXPECT_EQ(result.missed, 0);
  EXPECT_EQ(result.lowerBoundViolations, 0);
  EXPECT_EQ(result.mismatchedFields, 0);
}

} // namespace folly::bskip_versioned_large_key_test

// ============================================================================
// Skipper monotonicity regression tests
//
// These tests verify that the Skipper's public API (skipTo, advance, current)
// never returns a key smaller than a previously returned key, even when:
//   (a) skipTo is called with a target below the current position,
//   (b) concurrent inserts shift elements within leaf nodes,
//   (c) concurrent deletes tombstone the current key.
// ============================================================================
namespace folly::bskip_monotonicity_test {
namespace {

using MonoList = ConcurrentBSkipList<int64_t>;

TEST(SkipperMonotonicity, SkipToBackwardTarget_ReturnsAtLeastLastKey) {
  MonoList list;
  for (int64_t i = 0; i < 1000; ++i) {
    list.add(i * 2);
  }

  MonoList::Skipper skipper(list);
  auto pos = skipper.skipTo(int64_t{500});
  ASSERT_TRUE(pos);
  EXPECT_EQ(*pos, 500);

  // skipTo with a target below the current position must not regress.
  pos = skipper.skipTo(int64_t{100});
  ASSERT_TRUE(pos);
  EXPECT_GE(*pos, 500)
      << "skipTo(100) after skipTo(500) must not return a key below 500";

  // current() must agree.
  auto cur = skipper.current();
  ASSERT_TRUE(cur);
  EXPECT_GE(*cur, 500);

  // advance() must go strictly forward from wherever we are.
  auto next = skipper.advance();
  ASSERT_TRUE(next);
  EXPECT_GT(*next, *cur);
}

TEST(SkipperMonotonicity, SkipToSameTarget_Idempotent) {
  MonoList list;
  for (int64_t i = 0; i < 500; ++i) {
    list.add(i);
  }

  MonoList::Skipper skipper(list);
  auto pos1 = skipper.skipTo(int64_t{200});
  ASSERT_TRUE(pos1);
  EXPECT_EQ(*pos1, 200);

  // Repeating the same skipTo must return the same value.
  auto pos2 = skipper.skipTo(int64_t{200});
  ASSERT_TRUE(pos2);
  EXPECT_EQ(*pos2, 200);
}

TEST(SkipperMonotonicity, InterleavedOps_NeverRegress) {
  MonoList list;
  for (int64_t i = 0; i < 2000; ++i) {
    list.add(i);
  }

  MonoList::Skipper skipper(list);
  int64_t highWaterMark = -1;

  // Advancing ops (advance, skipTo) must return strictly greater.
  // Non-advancing ops (current) must return >= (same value is fine).
  auto checkAdvance = [&](std::optional<int64_t> val, const char* op) {
    if (val) {
      EXPECT_GT(*val, highWaterMark)
          << op << " returned " << *val
          << " which is not greater than high water mark " << highWaterMark;
      highWaterMark = *val;
    }
  };

  auto checkNonRegress = [&](std::optional<int64_t> val, const char* op) {
    if (val) {
      EXPECT_GE(*val, highWaterMark)
          << op << " returned " << *val << " which is below high water mark "
          << highWaterMark;
      if (*val > highWaterMark) {
        highWaterMark = *val;
      }
    }
  };

  checkAdvance(skipper.current(), "initial current");
  checkAdvance(skipper.advance(), "advance");
  checkAdvance(skipper.skipTo(int64_t{100}), "skipTo(100)");
  checkAdvance(skipper.advance(), "advance");
  checkAdvance(skipper.advance(), "advance");
  checkAdvance(skipper.skipTo(int64_t{500}), "skipTo(500)");
  checkNonRegress(skipper.current(), "current after skipTo(500)");

  // Now go backward — must not regress.
  auto pos = skipper.skipTo(int64_t{50});
  ASSERT_TRUE(pos);
  EXPECT_GE(*pos, 500)
      << "skipTo(50) after skipTo(500) must not return below 500";
  highWaterMark = std::max(highWaterMark, *pos);

  checkAdvance(skipper.advance(), "advance after backward skipTo");
  checkAdvance(skipper.skipTo(int64_t{1000}), "skipTo(1000)");
  checkAdvance(skipper.advance(), "final advance");
}

TEST(SkipperMonotonicity, ConcurrentInsert_SkipToNeverRegresses) {
  MonoList list;
  constexpr int64_t kRange = 20000;
  // Populate even keys so writers can insert odd keys into existing leaves,
  // maximizing the chance of within-leaf element shifts.
  for (int64_t i = 0; i < kRange; i += 2) {
    list.add(i);
  }

  std::atomic<bool> stop{false};
  std::atomic<int> regressions{0};

  constexpr int kWriters = 4;
  std::vector<std::thread> writers;
  writers.reserve(kWriters);
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w] {
      std::mt19937_64 rng(100 + w);
      while (!stop.load(std::memory_order_relaxed)) {
        int64_t key = static_cast<int64_t>((rng() % kRange) | 1);
        list.add(key);
      }
    });
  }

  constexpr int kReaders = 4;
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&] {
      for (int iter = 0; iter < 200 && !stop.load(std::memory_order_relaxed);
           ++iter) {
        MonoList::Skipper skipper(list);
        int64_t prev = -1;
        for (int64_t target = 0; target < kRange; target += 10) {
          auto pos = skipper.skipTo(target);
          if (pos) {
            if (*pos < prev) {
              regressions.fetch_add(1, std::memory_order_relaxed);
            }
            prev = *pos;
          }
        }
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(2));
  stop.store(true);
  for (auto& t : writers) {
    t.join();
  }
  for (auto& t : readers) {
    t.join();
  }

  EXPECT_EQ(regressions.load(), 0)
      << "skipTo returned a key smaller than a previously returned key";
}

TEST(SkipperMonotonicity, ConcurrentInsert_AdvanceNeverRegresses) {
  MonoList list;
  constexpr int64_t kRange = 10000;
  for (int64_t i = 0; i < kRange; i += 2) {
    list.add(i);
  }

  std::atomic<bool> stop{false};
  std::atomic<int> regressions{0};

  constexpr int kWriters = 4;
  std::vector<std::thread> writers;
  writers.reserve(kWriters);
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w] {
      std::mt19937_64 rng(200 + w);
      while (!stop.load(std::memory_order_relaxed)) {
        int64_t key = static_cast<int64_t>((rng() % kRange) | 1);
        list.add(key);
      }
    });
  }

  constexpr int kReaders = 4;
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&] {
      for (int iter = 0; iter < 200 && !stop.load(std::memory_order_relaxed);
           ++iter) {
        MonoList::Skipper skipper(list);
        int64_t prev = -1;
        for (auto pos = skipper.current(); pos; pos = skipper.advance()) {
          if (*pos <= prev) {
            regressions.fetch_add(1, std::memory_order_relaxed);
          }
          prev = *pos;
        }
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(2));
  stop.store(true);
  for (auto& t : writers) {
    t.join();
  }
  for (auto& t : readers) {
    t.join();
  }

  EXPECT_EQ(regressions.load(), 0)
      << "advance returned a key not greater than the previous key";
}

TEST(SkipperMonotonicity, ConcurrentChurn_MixedOpsNeverRegress) {
  MonoList list;
  constexpr int64_t kRange = 20000;
  for (int64_t i = 0; i < kRange; ++i) {
    list.add(i);
  }

  std::atomic<bool> stop{false};
  std::atomic<int> regressions{0};

  constexpr int kChurners = 4;
  std::vector<std::thread> churners;
  churners.reserve(kChurners);
  for (int c = 0; c < kChurners; ++c) {
    churners.emplace_back([&, c] {
      std::mt19937_64 rng(300 + c);
      while (!stop.load(std::memory_order_relaxed)) {
        int64_t key = static_cast<int64_t>(rng() % kRange);
        if (rng() % 2 == 0) {
          list.add(key);
        } else {
          list.remove(key);
        }
      }
    });
  }

  constexpr int kReaders = 4;
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&, r] {
      std::mt19937_64 rng(400 + r);
      for (int iter = 0; iter < 100 && !stop.load(std::memory_order_relaxed);
           ++iter) {
        MonoList::Skipper skipper(list);
        int64_t highWaterMark = -1;

        // Interleave skipTo, advance, and current.
        for (int step = 0; step < 500; ++step) {
          std::optional<int64_t> val;
          int op = rng() % 3;
          if (op == 0) {
            int64_t target = static_cast<int64_t>(rng() % kRange);
            val = skipper.skipTo(target);
          } else if (op == 1) {
            val = skipper.advance();
          } else {
            val = skipper.current();
          }

          if (val) {
            if (*val < highWaterMark) {
              regressions.fetch_add(1, std::memory_order_relaxed);
            }
            if (*val > highWaterMark) {
              highWaterMark = *val;
            }
          }
        }
      }
    });
  }

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(2));
  stop.store(true);
  for (auto& t : churners) {
    t.join();
  }
  for (auto& t : readers) {
    t.join();
  }

  EXPECT_EQ(regressions.load(), 0)
      << "mixed skipTo/advance/current returned a regressing key";
}

} // namespace
} // namespace folly::bskip_monotonicity_test

namespace folly::bskip_payload_test {
namespace {

struct WidePayload {
  uint64_t value;
  uint64_t checksum;
  uint64_t echoedValue;
  uint64_t echoedChecksum;

  bool operator==(const WidePayload& other) const {
    return value == other.value && checksum == other.checksum &&
        echoedValue == other.echoedValue &&
        echoedChecksum == other.echoedChecksum;
  }
};

static_assert(sizeof(WidePayload) == 32);
static_assert(std::is_trivially_copyable_v<WidePayload>);

WidePayload makeWidePayload(uint32_t key, uint64_t generation) {
  uint64_t value = static_cast<uint64_t>(key) * 17 + generation;
  uint64_t checksum = ~value;
  return {value, checksum, value, checksum};
}

bool isExpectedWidePayload(uint32_t key, const WidePayload& payload) {
  uint64_t base = static_cast<uint64_t>(key) * 17;
  return payload.value >= base && payload.value < base + 4 &&
      payload.checksum == ~payload.value &&
      payload.echoedValue == payload.value &&
      payload.echoedChecksum == payload.checksum;
}

using BSkipPayload = folly::ConcurrentBSkipList<uint32_t, uint64_t>;

using BSkipPayloadInline = folly::ConcurrentBSkipList<
    uint32_t,
    uint64_t,
    /*B=*/16,
    folly::KeyReadPolicy::RelaxedAtomic,
    folly::LeafStoragePolicy::Inline>;

using BSkipPayloadWide = folly::ConcurrentBSkipList<
    uint32_t,
    WidePayload,
    /*B=*/16,
    folly::KeyReadPolicy::Locked>;

using BSkipPayloadWideInline = folly::ConcurrentBSkipList<
    uint32_t,
    WidePayload,
    /*B=*/16,
    folly::KeyReadPolicy::Locked,
    folly::LeafStoragePolicy::Inline>;

using BSkipDefaultPayloadMap = folly::ConcurrentBSkipMap<uint32_t, uint64_t>;
using BSkipDefaultInlinePayloadMap =
    folly::ConcurrentBSkipInlineMap<uint32_t, uint64_t>;

struct VersionedAliasPolicy
    : folly::ConcurrentBSkipDefaultPolicy<
          VersionedKey,
          OptimisticVersionedKeyHash> {
  // 16B key: explicit opt-in. operator< is safe on stale values.
  static constexpr folly::KeyReadPolicy kReadPolicy =
      folly::KeyReadPolicy::RelaxedAtomic;
};

using BSkipVersionedKeySet =
    folly::ConcurrentBSkipSet<VersionedKey, VersionedAliasPolicy>;

static_assert(BSkipPayload::kHasPayload);
static_assert(std::is_same_v<BSkipPayload::payload_type, uint64_t>);
static_assert(BSkipPayloadInline::kHasPayload);
static_assert(BSkipPayloadInline::kInlinePayloadStorage);
static_assert(BSkipPayload::kReadPolicy != folly::KeyReadPolicy::Locked);
static_assert(BSkipPayloadInline::kReadPolicy != folly::KeyReadPolicy::Locked);
static_assert(BSkipPayloadWide::kReadPolicy == folly::KeyReadPolicy::Locked);
static_assert(
    BSkipPayloadWideInline::kReadPolicy == folly::KeyReadPolicy::Locked);
static_assert(
    BSkipDefaultPayloadMap::kReadPolicy == folly::KeyReadPolicy::RelaxedAtomic);
static_assert(
    !BSkipDefaultPayloadMap::kInlinePayloadStorage &&
    BSkipDefaultPayloadMap::kHasPayload);
static_assert(BSkipDefaultInlinePayloadMap::kInlinePayloadStorage);
static_assert(
    BSkipVersionedKeySet::kReadPolicy == folly::KeyReadPolicy::RelaxedAtomic);
static_assert(BSkipPayloadWide::kReadPolicy == folly::KeyReadPolicy::Locked);

TEST(ConcurrentBSkipList, PolicyAliasesProvideExpectedContainerShapes) {
  BSkipDefaultPayloadMap map;
  EXPECT_TRUE(map.add(7, uint64_t{49}));
  auto found = map.find(7);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->first, 7u);
  EXPECT_EQ(found->second, 49u);

  BSkipDefaultInlinePayloadMap inlineMap;
  EXPECT_TRUE(inlineMap.add(9, uint64_t{63}));
  auto inlineFound = inlineMap.find(9);
  ASSERT_TRUE(inlineFound.has_value());
  EXPECT_EQ(inlineFound->first, 9u);
  EXPECT_EQ(inlineFound->second, 63u);

  BSkipVersionedKeySet versionedSet;
  VersionedKey key{11, 22};
  EXPECT_TRUE(versionedSet.add(key));
  EXPECT_TRUE(versionedSet.contains(key));
}

template <typename ListType>
void expectConcurrentOptimisticPayloadReadsStayConsistent() {
  ListType list;
  std::atomic<bool> stop{false};
  std::atomic<int> mismatches{0};
  std::atomic<int> misses{0};
  std::atomic<int> countMismatches{0};

  constexpr uint32_t kNumKeys = 2000;

  for (uint32_t i = 2; i < kNumKeys; ++i) {
    list.add(i, static_cast<uint64_t>(i) * 7);
  }

  std::vector<std::thread> writers;
  writers.reserve(4);
  for (int t = 0; t < 4; ++t) {
    writers.emplace_back([&, t] {
      while (!stop.load(std::memory_order_relaxed)) {
        for (uint32_t i = 2; i < kNumKeys; ++i) {
          uint64_t payload = static_cast<uint64_t>(i) * 7 + t;
          list.addOrUpdate(
              i, [payload](const uint32_t&, std::optional<uint64_t>& p) {
                p = payload;
              });
        }
      }
    });
  }

  std::thread skipperReader([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      typename ListType::Skipper skipper(list);
      for (uint32_t i = 2; i < kNumKeys; ++i) {
        auto key = skipper.skipTo(i);
        if (!key || *key != i) {
          misses.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        uint64_t payload = skipper.currPayload();
        uint64_t base = static_cast<uint64_t>(*key) * 7;
        if (payload < base || payload >= base + 4) {
          mismatches.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
  });

  std::thread scanReader([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      size_t count = 0;
      list.forEachElement([&](uint32_t key, uint64_t payload) {
        ++count;
        uint64_t base = static_cast<uint64_t>(key) * 7;
        if (payload < base || payload >= base + 4) {
          mismatches.fetch_add(1, std::memory_order_relaxed);
        }
      });
      if (count != kNumKeys - 2) {
        countMismatches.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  std::this_thread::sleep_for( // NOLINT(facebook-hte-BadCall-sleep_for)
      std::chrono::seconds(2));
  stop.store(true, std::memory_order_relaxed);

  for (auto& t : writers) {
    t.join();
  }
  skipperReader.join();
  scanReader.join();

  EXPECT_EQ(misses.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(mismatches.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(countMismatches.load(std::memory_order_relaxed), 0);
}

template <typename ListType>
void expectConcurrentWideOptimisticPayloadReadsStayConsistent() {
  ListType list;
  std::atomic<bool> stop{false};
  std::atomic<int> mismatches{0};
  std::atomic<int> misses{0};
  std::atomic<int> countMismatches{0};

  constexpr uint32_t kNumKeys = 2000;

  for (uint32_t i = 2; i < kNumKeys; ++i) {
    list.add(i, makeWidePayload(i, 0));
  }

  std::vector<std::thread> writers;
  writers.reserve(4);
  for (int t = 0; t < 4; ++t) {
    writers.emplace_back([&, t] {
      while (!stop.load(std::memory_order_relaxed)) {
        for (uint32_t i = 2; i < kNumKeys; ++i) {
          WidePayload payload = makeWidePayload(i, t);
          list.addOrUpdate(
              i, [payload](const uint32_t&, std::optional<WidePayload>& p) {
                p = payload;
              });
        }
      }
    });
  }

  std::thread skipperReader([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      typename ListType::Skipper skipper(list);
      for (uint32_t i = 2; i < kNumKeys; ++i) {
        auto key = skipper.skipTo(i);
        if (!key || *key != i) {
          misses.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        WidePayload payload = skipper.currPayload();
        if (!isExpectedWidePayload(*key, payload)) {
          mismatches.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
  });

  std::thread scanReader([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      size_t count = 0;
      list.forEachElement([&](uint32_t key, WidePayload payload) {
        ++count;
        if (!isExpectedWidePayload(key, payload)) {
          mismatches.fetch_add(1, std::memory_order_relaxed);
        }
      });
      if (count != kNumKeys - 2) {
        countMismatches.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  std::this_thread::sleep_for( // NOLINT(facebook-hte-BadCall-sleep_for)
      std::chrono::seconds(2));
  stop.store(true, std::memory_order_relaxed);

  for (auto& t : writers) {
    t.join();
  }
  skipperReader.join();
  scanReader.join();

  EXPECT_EQ(misses.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(mismatches.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(countMismatches.load(std::memory_order_relaxed), 0);
}

TEST(BSkipPayload, AddAndFindWithPayload) {
  BSkipPayload list;
  EXPECT_TRUE(list.add(10, uint64_t{100}));
  EXPECT_TRUE(list.add(20, uint64_t{200}));
  EXPECT_TRUE(list.add(30, uint64_t{300}));

  auto r = list.find(20);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->first, 20);
  EXPECT_EQ(r->second, 200);

  EXPECT_FALSE(list.find(99).has_value());
}

TEST(BSkipPayload, AddWithoutPayloadDefaultsToZero) {
  BSkipPayload list;
  list.add(5);

  auto r = list.find(5);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->first, 5);
  EXPECT_EQ(r->second, 0);
}

TEST(BSkipPayload, AddOrUpdateWithPayload) {
  BSkipPayload list;
  EXPECT_TRUE(list.add(10, uint64_t{100}));

  auto outcome = list.addOrUpdate(
      10, [](const uint32_t& /*key*/, std::optional<uint64_t>& payload) {
        payload = 777;
      });
  EXPECT_FALSE(outcome);

  auto r = list.find(10);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->second, 777);
}

TEST(ConcurrentBSkipList, CountAndEraseProvideCSLStyleAliases) {
  DefaultList list;

  EXPECT_TRUE(list.add(10));
  EXPECT_FALSE(list.add(10));
  EXPECT_EQ(list.count(10), 1);
  EXPECT_EQ(list.count(11), 0);
  EXPECT_EQ(list.erase(11), 0);
  EXPECT_TRUE(list.remove(10));
  EXPECT_TRUE(list.add(10));
  EXPECT_EQ(list.erase(10), 1);
  EXPECT_TRUE(list.empty());
}

TEST(BSkipPayload, ReviveWithoutPayloadClearsDeletedPayload) {
  BSkipPayload list;
  EXPECT_TRUE(list.add(10, uint64_t{100}));
  EXPECT_TRUE(list.remove(10));

  EXPECT_TRUE(list.add(10));
  auto revived = list.find(10);
  ASSERT_TRUE(revived.has_value());
  EXPECT_EQ(revived->first, 10);
  EXPECT_EQ(revived->second, 0);

  EXPECT_TRUE(list.remove(10));
  int updaterCalls = 0;
  // Updater receives an empty optional on the Revive path; leaving it empty
  // resets the slot to a default-constructed payload.
  auto outcome = list.addOrUpdate(
      10, [&](const uint32_t&, std::optional<uint64_t>&) { ++updaterCalls; });
  EXPECT_TRUE(outcome);
  EXPECT_EQ(updaterCalls, 1);

  revived = list.find(10);
  ASSERT_TRUE(revived.has_value());
  EXPECT_EQ(revived->second, 0);
}

TEST(BSkipPayload, UpdatePayloadModifiesPayload) {
  BSkipPayload list;
  list.add(42, uint64_t{100});

  bool updated = list.updatePayload(
      42, [](const uint32_t& /*key*/, uint64_t& payload) { payload += 50; });
  EXPECT_TRUE(updated);

  auto r = list.find(42);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->second, 150);
}

TEST(BSkipPayload, RemoveAndGetPayloadReturnsDeletedPayload) {
  BSkipPayload list;
  EXPECT_FALSE(list.removeAndGetPayload(7).has_value());
  EXPECT_TRUE(list.add(7, uint64_t{123}));

  auto payload = list.removeAndGetPayload(7);
  ASSERT_TRUE(payload.has_value());
  EXPECT_EQ(*payload, 123);
  EXPECT_FALSE(list.find(7).has_value());
  EXPECT_FALSE(list.removeAndGetPayload(7).has_value());
}

// Sentinel keys are caller UB checked by DCHECK in updatePayload().
#ifndef NDEBUG
TEST(BSkipPayloadDeathTest, UpdatePayloadSentinelDChecks) {
  BSkipPayload list;
  list.add(42, uint64_t{100});

  EXPECT_DEATH(
      list.updatePayload(
          std::numeric_limits<uint32_t>::min(),
          [](const uint32_t&, uint64_t&) {}),
      "sentinel keys");
  EXPECT_DEATH(
      list.updatePayload(
          std::numeric_limits<uint32_t>::max(),
          [](const uint32_t&, uint64_t&) {}),
      "sentinel keys");
}
#endif // NDEBUG

TEST(BSkipPayload, RemoveAndRevivePreservesPayload) {
  BSkipPayload list;
  list.add(10, uint64_t{100});
  EXPECT_TRUE(list.remove(10));
  EXPECT_FALSE(list.find(10).has_value());

  list.add(10, uint64_t{200});
  auto r = list.find(10);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->second, 200);
}

TEST(BSkipPayload, SplitPreservesKeyPayloadAssociation) {
  BSkipPayload list;
  for (uint32_t i = 2; i < 200; ++i) {
    list.add(i, static_cast<uint64_t>(i) * 7);
  }

  for (uint32_t i = 2; i < 200; ++i) {
    auto r = list.find(i);
    ASSERT_TRUE(r.has_value()) << "key " << i << " not found";
    EXPECT_EQ(r->first, i);
    EXPECT_EQ(r->second, static_cast<uint64_t>(i) * 7)
        << "payload mismatch at key " << i;
  }
}

TEST(BSkipPayload, ForEachElementVisitsPayloads) {
  BSkipPayload list;
  for (uint32_t i = 2; i <= 50; ++i) {
    list.add(i, static_cast<uint64_t>(i) * 11);
  }

  std::vector<std::pair<uint32_t, uint64_t>> collected;
  list.forEachElement([&](uint32_t key, uint64_t payload) {
    collected.emplace_back(key, payload);
  });

  EXPECT_EQ(collected.size(), 49u);
  for (const auto& [key, payload] : collected) {
    EXPECT_EQ(payload, static_cast<uint64_t>(key) * 11);
  }
}

TEST(BSkipPayload, SkipperReturnsKeysOnly) {
  BSkipPayload list;
  for (uint32_t i = 2; i <= 100; ++i) {
    list.add(i, static_cast<uint64_t>(i) * 3);
  }

  BSkipPayload::Skipper skipper(list);
  auto r = skipper.skipTo(50);
  ASSERT_TRUE(r.has_value());
  EXPECT_GE(*r, 50u);

  auto found = list.find(*r);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->second, static_cast<uint64_t>(*r) * 3);
}

TEST(BSkipPayload, SkipperExposesCurrentPayloadLocation) {
  BSkipPayload list;
  for (uint32_t i = 2; i <= 100; ++i) {
    list.add(i, static_cast<uint64_t>(i) * 13);
  }

  BSkipPayload::Skipper skipper(list);
  auto key = skipper.skipTo(50);
  ASSERT_TRUE(key.has_value());
  EXPECT_GE(*key, 50u);
  EXPECT_NE(skipper.currentLeaf(), nullptr);
  EXPECT_EQ(skipper.peekCurrentPayload(), skipper.currPayload());

  auto found = list.find(*key);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(skipper.peekCurrentPayload(), found->second);

  EXPECT_LT(skipper.currentSlot(), 255);
}

TEST(BSkipPayload, ConcurrentWriteReadConsistency) {
  BSkipPayload list;
  std::atomic<bool> stop{false};
  std::atomic<int> mismatches{0};

  constexpr int kNumKeys = 10000;

  std::vector<std::thread> writers;
  writers.reserve(4);
  for (int t = 0; t < 4; ++t) {
    writers.emplace_back([&, t] {
      for (uint32_t i = 2; i < kNumKeys; ++i) {
        uint32_t key = i;
        uint64_t payload = static_cast<uint64_t>(key) * 7 + t;
        list.addOrUpdate(
            key, [payload](const uint32_t&, std::optional<uint64_t>& p) {
              p = payload;
            });
      }
    });
  }

  std::vector<std::thread> readers;
  readers.reserve(4);
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        for (uint32_t i = 2; i < kNumKeys; ++i) {
          auto r = list.find(i);
          if (!r) {
            continue;
          }
          uint32_t key = r->first;
          uint64_t payload = r->second;
          uint64_t base = static_cast<uint64_t>(key) * 7;
          if (payload < base || payload >= base + 4) {
            mismatches.fetch_add(1);
          }
        }
      }
    });
  }

  for (auto& t : writers) {
    t.join();
  }
  stop.store(true);
  for (auto& t : readers) {
    t.join();
  }

  EXPECT_EQ(mismatches.load(), 0)
      << "payload torn read: key and payload were from different slots";
}

TEST(BSkipPayload, OptimisticSkipperAndScanKeepPayloadsConsistent) {
  expectConcurrentOptimisticPayloadReadsStayConsistent<BSkipPayload>();
}

TEST(BSkipPayloadWide, AddAndFindWithPayload) {
  BSkipPayloadWide list;
  EXPECT_TRUE(list.add(10, makeWidePayload(10, 1)));
  EXPECT_TRUE(list.add(20, makeWidePayload(20, 2)));

  auto r = list.find(20);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->first, 20);
  EXPECT_EQ(r->second, makeWidePayload(20, 2));
}

TEST(BSkipPayloadWide, OptimisticSkipperAndScanKeepPayloadsConsistent) {
  expectConcurrentWideOptimisticPayloadReadsStayConsistent<BSkipPayloadWide>();
}

TEST(BSkipPayloadInline, UpdatePayloadModifiesPayload) {
  BSkipPayloadInline list;
  list.add(42, uint64_t{100});

  bool updated =
      list.updatePayload(42, [](const uint32_t& key, uint64_t& payload) {
        EXPECT_EQ(key, 42);
        payload += 50;
      });
  EXPECT_TRUE(updated);

  auto r = list.find(42);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->first, 42);
  EXPECT_EQ(r->second, 150);
}

TEST(BSkipPayloadInline, RemoveAndGetPayloadReturnsDeletedPayload) {
  BSkipPayloadInline list;
  EXPECT_FALSE(list.removeAndGetPayload(7).has_value());
  EXPECT_TRUE(list.add(7, uint64_t{123}));

  auto payload = list.removeAndGetPayload(7);
  ASSERT_TRUE(payload.has_value());
  EXPECT_EQ(*payload, 123);
  EXPECT_FALSE(list.find(7).has_value());
  EXPECT_FALSE(list.removeAndGetPayload(7).has_value());
}

TEST(BSkipPayloadInline, SplitPreservesKeyPayloadAssociation) {
  BSkipPayloadInline list;
  for (uint32_t i = 2; i < 200; ++i) {
    list.add(i, static_cast<uint64_t>(i) * 9);
  }

  for (uint32_t i = 2; i < 200; ++i) {
    auto r = list.find(i);
    ASSERT_TRUE(r.has_value()) << "key " << i << " not found";
    EXPECT_EQ(r->first, i);
    EXPECT_EQ(r->second, static_cast<uint64_t>(i) * 9)
        << "payload mismatch at key " << i;
  }
}

TEST(BSkipPayloadInline, OptimisticSkipperAndScanKeepPayloadsConsistent) {
  expectConcurrentOptimisticPayloadReadsStayConsistent<BSkipPayloadInline>();
}

TEST(BSkipPayloadWideInline, OptimisticSkipperAndScanKeepPayloadsConsistent) {
  expectConcurrentWideOptimisticPayloadReadsStayConsistent<
      BSkipPayloadWideInline>();
}

TEST(BSkipPayload, PayloadVoidInstantiationUnchanged) {
  using BSkipNoPayload = folly::ConcurrentBSkipList<uint32_t>;
  static_assert(!BSkipNoPayload::kHasPayload);
  static_assert(std::is_same_v<BSkipNoPayload::payload_type, void>);

  BSkipNoPayload list;
  list.add(10);
  auto r = list.find(10);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 10);
}

} // namespace
} // namespace folly::bskip_payload_test
