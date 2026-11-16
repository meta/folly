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

// Benchmark ConcurrentBSkipList against folly::ConcurrentSkipList.
// Uses 100K random keys from a 1M universe.
// Run: buck run @mode/opt
// fbsource//xplat/folly/test:concurrent_b_skip_list_benchmark

#include <folly/ConcurrentBSkipList.h>

#include <algorithm>
#include <memory>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include <folly/Benchmark.h>
#include <folly/ConcurrentSkipList.h>
#include <folly/init/Init.h>
#include <folly/portability/GFlags.h>
#include <folly/test/ConcurrentBSkipListTestKeys.h>

using namespace folly;
using folly::bskip_test::AoS16BKey24;
using folly::bskip_test::Bench16B;

struct OptimisticBench16BHash {
  size_t operator()(Bench16B key16b) const {
    return std::hash<Bench16B>{}(key16b);
  }
};

struct Bench16BOptimisticPolicy
    : ConcurrentBSkipDefaultPolicy<Bench16B, OptimisticBench16BHash> {
  // 16B key: explicit opt-in to RelaxedAtomic. operator< is safe on
  // stale-but-complete values (pure int64 comparison, no pointers).
  static constexpr KeyReadPolicy kReadPolicy = KeyReadPolicy::RelaxedAtomic;
};

// B=32 for uint32_t keys (128B leaf = 2 cache lines).
using BSkip32 = ConcurrentBSkipList<uint32_t, /*PayloadType=*/void, /*B=*/32>;

// SoA: uint32_t key + uint64_t payload, B=16 (64B keys = 1 cache line).
using BSkip32SoA16 = ConcurrentBSkipMap<uint32_t, uint64_t>;

// SoA: uint32_t key + uint64_t payload, B=32 (128B keys = 2 cache lines).
using BSkip32SoA32 = ConcurrentBSkipList<uint32_t, uint64_t, /*B=*/32>;

// AoS stand-in: 12-byte packed key (uint32 + uint64 hitword), like
// InlineHitElement. B=16 to match current Hybrid.
struct __attribute__((packed)) AoSKey12 {
  uint32_t key{};
  uint64_t hitword{0};

  auto operator<=>(const AoSKey12& o) const {
    if (key < o.key) {
      return std::strong_ordering::less;
    }
    if (o.key < key) {
      return std::strong_ordering::greater;
    }
    return std::strong_ordering::equal;
  }
  bool operator==(const AoSKey12& o) const { return key == o.key; }
  static AoSKey12 min_val() {
    return {std::numeric_limits<uint32_t>::min(), 0};
  }
  static AoSKey12 max_val() {
    return {std::numeric_limits<uint32_t>::max(), 0};
  }
};
static_assert(sizeof(AoSKey12) == 12);

namespace std {
template <>
struct numeric_limits<AoSKey12> {
  static constexpr bool is_specialized = true;
  static AoSKey12 min() { return AoSKey12::min_val(); }
  static AoSKey12 max() { return AoSKey12::max_val(); }
};
template <>
struct hash<AoSKey12> {
  size_t operator()(const AoSKey12& k) const {
    return std::hash<uint64_t>{}(static_cast<uint64_t>(k.key));
  }
};
} // namespace std

// AoS B=16: 12B × 16 = 192B key array (3 cache lines). Matches current Hybrid.
using BSkipAoS16 = ConcurrentBSkipList<AoSKey12>;

// AoS Key16B B=24: 24B × 24 = 576B (9 cache lines). Current Hybrid for Key16B.
using BSkipAoS16B24 =
    ConcurrentBSkipList<AoS16BKey24, /*PayloadType=*/void, /*B=*/24>;

// SoA Key16B B=16: 16B × 16 = 256B key array (4 cache lines).
using BSkipSoA16B16 =
    ConcurrentBSkipMap<Bench16B, uint64_t, Bench16BOptimisticPolicy>;

// SoA Key16B B=24: 16B × 24 = 384B key array (6 cache lines).
using BSkipSoA16B24 = ConcurrentBSkipList<
    Bench16B,
    uint64_t,
    /*B=*/24,
    KeyReadPolicy::RelaxedAtomic,
    LeafStoragePolicy::Separate,
    Bench16BOptimisticPolicy>;

// SoA Key16B B=32: 16B × 32 = 512B key array (8 cache lines).
using BSkipSoA16B32 = ConcurrentBSkipList<
    Bench16B,
    uint64_t,
    /*B=*/32,
    KeyReadPolicy::RelaxedAtomic,
    LeafStoragePolicy::Separate,
    Bench16BOptimisticPolicy>;

using FollySL = ConcurrentSkipList<uint32_t>;
using FollySL16B = ConcurrentSkipList<Bench16B>;
struct Bench16BLockedPolicy
    : ConcurrentBSkipDefaultPolicy<Bench16B, OptimisticBench16BHash> {
  static constexpr KeyReadPolicy kReadPolicy = KeyReadPolicy::Locked;
};
using Locked16BBSkip = ConcurrentBSkipSet<Bench16B, Bench16BLockedPolicy>;
using Optimistic16BBSkip =
    ConcurrentBSkipSet<Bench16B, Bench16BOptimisticPolicy>;

static Bench16B makeKey16B(uint32_t key) {
  return {static_cast<int64_t>(key), static_cast<int64_t>(key)};
}

static Bench16B makeTransientKey16B(int64_t key) {
  return {key, key};
}

// ============================================================================
// Setup: 100K random keys from [1, 1M] (~10% selectivity).
// ============================================================================

static constexpr uint32_t kUniverse = 1'000'000;
static constexpr uint32_t kListSize = 100'000;

// NOLINTBEGIN(facebook-avoid-non-const-global-variables)
static std::vector<uint32_t> gKeys; // sorted random keys
static std::vector<uint32_t> gShuffledKeys; // same keys, random order
static std::vector<uint32_t> gSkipTargets10;
static std::vector<uint32_t> gSkipTargets100;
static std::vector<Bench16B> gKey16BKeys; // same keys as gKeys, Key16B order
static std::vector<Bench16B> gKey16BShuffledKeys; // random insert order
static std::vector<Bench16B> gKey16BSkipTargets10;
static std::vector<Bench16B> gKey16BSkipTargets100;

static std::unique_ptr<BSkip32> gBSkip;
static std::unique_ptr<BSkip32SoA16> gBSkipSoA16;
static std::unique_ptr<BSkip32SoA32> gBSkipSoA32;
static std::unique_ptr<BSkipAoS16> gBSkipAoS16;
static std::unique_ptr<BSkipAoS16B24> gBSkipAoS16B;
static std::unique_ptr<BSkipSoA16B16> gBSkipSoA16B16;
static std::unique_ptr<BSkipSoA16B24> gBSkipSoA16B24;
static std::unique_ptr<BSkipSoA16B32> gBSkipSoA16B32;
static std::shared_ptr<FollySL> gFolly;
static std::shared_ptr<FollySL16B> gFollyKey16B;
static std::unique_ptr<Locked16BBSkip> gLocked16BBSkip;
static std::unique_ptr<Optimistic16BBSkip> gOptimistic16BBSkip;
// NOLINTEND(facebook-avoid-non-const-global-variables)

static void initData() {
  static bool done = false;
  if (done) {
    return;
  }
  done = true;

  std::mt19937 rng(42);
  std::set<uint32_t> keySet;
  while (static_cast<uint32_t>(keySet.size()) < kListSize) {
    keySet.insert((rng() % kUniverse) + 1); // [1, kUniverse]
  }
  gKeys.assign(keySet.begin(), keySet.end());

  for (size_t i = 0; i < gKeys.size(); i += 10) {
    gSkipTargets10.push_back(gKeys[i]);
  }
  for (size_t i = 0; i < gKeys.size(); i += 100) {
    gSkipTargets100.push_back(gKeys[i]);
  }

  // Random insertion order for the construction benchmarks.
  gShuffledKeys = gKeys;
  std::shuffle(gShuffledKeys.begin(), gShuffledKeys.end(), rng);
  auto& shuffled = gShuffledKeys;

  gKey16BKeys.reserve(gKeys.size());
  for (auto key : gKeys) {
    gKey16BKeys.push_back(makeKey16B(key));
  }
  std::sort(gKey16BKeys.begin(), gKey16BKeys.end());
  for (size_t i = 0; i < gKey16BKeys.size(); i += 10) {
    gKey16BSkipTargets10.push_back(gKey16BKeys[i]);
  }
  for (size_t i = 0; i < gKey16BKeys.size(); i += 100) {
    gKey16BSkipTargets100.push_back(gKey16BKeys[i]);
  }

  gKey16BShuffledKeys.reserve(gShuffledKeys.size());
  for (auto key : gShuffledKeys) {
    gKey16BShuffledKeys.push_back(makeKey16B(key));
  }

  gBSkip = std::make_unique<BSkip32>();
  gBSkipSoA16 = std::make_unique<BSkip32SoA16>();
  gBSkipSoA32 = std::make_unique<BSkip32SoA32>();
  gBSkipAoS16 = std::make_unique<BSkipAoS16>();
  gBSkipAoS16B = std::make_unique<BSkipAoS16B24>();
  gBSkipSoA16B16 = std::make_unique<BSkipSoA16B16>();
  gBSkipSoA16B24 = std::make_unique<BSkipSoA16B24>();
  gBSkipSoA16B32 = std::make_unique<BSkipSoA16B32>();
  gFolly = FollySL::createInstance(10);
  gFollyKey16B = FollySL16B::createInstance(10);
  gLocked16BBSkip = std::make_unique<Locked16BBSkip>();
  gOptimistic16BBSkip = std::make_unique<Optimistic16BBSkip>();
  {
    FollySL::Accessor acc(gFolly);
    FollySL16B::Accessor key16bAcc(gFollyKey16B);
    for (auto k : shuffled) {
      gBSkip->add(k);
      uint64_t payload = static_cast<uint64_t>(k) * 7;
      gBSkipSoA16->add(k, payload);
      gBSkipSoA32->add(k, payload);
      gBSkipAoS16->add(AoSKey12{k, payload});
      acc.add(k);
    }
    for (const auto& key16b : gKey16BShuffledKeys) {
      uint64_t payload = static_cast<uint64_t>(key16b.id) * 7;
      key16bAcc.add(key16b);
      gLocked16BBSkip->add(key16b);
      gOptimistic16BBSkip->add(key16b);
      gBSkipAoS16B->add(AoS16BKey24{key16b, payload});
      gBSkipSoA16B16->add(key16b, payload);
      gBSkipSoA16B24->add(key16b, payload);
      gBSkipSoA16B32->add(key16b, payload);
    }
  }
}

// Drive a folly::ConcurrentSkipList Skipper through `targets` via
// Skipper::to(). The list is wrapped in a shared_ptr/Accessor in the
// benchmark globals, so the SkipList type must be passed explicitly while
// `list` is whatever Skipper's constructor accepts. Inlined so each BENCHMARK
// body keeps the same machine code as the open-coded original.
template <typename SkipListT, typename ListHandle, typename Targets>
FOLLY_ALWAYS_INLINE void runFollySkipperTo(
    const ListHandle& list, const Targets& targets) {
  typename SkipListT::Skipper skipper(list);
  for (const auto& t : targets) {
    skipper.to(t);
  }
}

// Drive a ConcurrentBSkipList Skipper through `targets` via Skipper::skipTo().
template <typename ListT, typename Targets>
FOLLY_ALWAYS_INLINE void runBSkipSkipperSkipTo(
    ListT& list, const Targets& targets) {
  typename ListT::Skipper skipper(list);
  for (const auto& t : targets) {
    skipper.skipTo(t);
  }
}

// ============================================================================
// SkipTo stride=10
// ============================================================================

BENCHMARK(Folly_SkipTo10, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    runFollySkipperTo<FollySL>(gFolly, gSkipTargets10);
  }
}

BENCHMARK_RELATIVE(BSkip_SkipTo10, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    runBSkipSkipperSkipTo(*gBSkip, gSkipTargets10);
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// Key16B SkipTo stride=10
// ============================================================================

BENCHMARK(Folly_Key16B_SkipTo10, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    runFollySkipperTo<FollySL16B>(gFollyKey16B, gKey16BSkipTargets10);
  }
}

BENCHMARK_RELATIVE(Key16B_Locked_SkipTo10, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    runBSkipSkipperSkipTo(*gLocked16BBSkip, gKey16BSkipTargets10);
  }
}

BENCHMARK_RELATIVE(Key16B_Optimistic_SkipTo10, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    runBSkipSkipperSkipTo(*gOptimistic16BBSkip, gKey16BSkipTargets10);
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// SkipTo stride=100
// ============================================================================

BENCHMARK(Folly_SkipTo100, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    runFollySkipperTo<FollySL>(gFolly, gSkipTargets100);
  }
}

BENCHMARK_RELATIVE(BSkip_SkipTo100, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    runBSkipSkipperSkipTo(*gBSkip, gSkipTargets100);
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// Key16B SkipTo stride=100
// ============================================================================

BENCHMARK(Folly_Key16B_SkipTo100, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    runFollySkipperTo<FollySL16B>(gFollyKey16B, gKey16BSkipTargets100);
  }
}

BENCHMARK_RELATIVE(Key16B_Locked_SkipTo100, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    runBSkipSkipperSkipTo(*gLocked16BBSkip, gKey16BSkipTargets100);
  }
}

BENCHMARK_RELATIVE(Key16B_Optimistic_SkipTo100, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    runBSkipSkipperSkipTo(*gOptimistic16BBSkip, gKey16BSkipTargets100);
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// Contains
// ============================================================================

BENCHMARK(Folly_Contains, iters) {
  initData();
  FollySL::Accessor acc(gFolly);
  for (size_t i = 0; i < iters; ++i) {
    doNotOptimizeAway(acc.contains(gKeys[i % gKeys.size()]));
  }
}

BENCHMARK_RELATIVE(BSkip_Contains, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    doNotOptimizeAway(gBSkip->contains(gKeys[i % gKeys.size()]));
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// Key16B Contains
// ============================================================================

BENCHMARK(Folly_Key16B_Contains, iters) {
  initData();
  FollySL16B::Accessor acc(gFollyKey16B);
  for (size_t i = 0; i < iters; ++i) {
    doNotOptimizeAway(acc.contains(gKey16BKeys[i % gKey16BKeys.size()]));
  }
}

BENCHMARK_RELATIVE(Key16B_Locked_Contains, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    doNotOptimizeAway(
        gLocked16BBSkip->contains(gKey16BKeys[i % gKey16BKeys.size()]));
  }
}

BENCHMARK_RELATIVE(Key16B_Optimistic_Contains, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    doNotOptimizeAway(
        gOptimistic16BBSkip->contains(gKey16BKeys[i % gKey16BKeys.size()]));
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// Iterate (full scan)
// ============================================================================

// Folly-style iteration: increment Skipper while good(), accumulating
// project(data()). SkipListT must be passed explicitly because `list` is a
// shared_ptr/Accessor wrapper around it.
template <typename SkipListT, typename ListHandle, typename Project>
FOLLY_ALWAYS_INLINE uint64_t
follyIterateSum(const ListHandle& list, Project&& project) {
  uint64_t sum = 0;
  typename SkipListT::Skipper skipper(list);
  while (skipper.good()) {
    sum += project(skipper.data());
    ++skipper;
  }
  return sum;
}

// BSkip-style iteration: cursor walks via current()/advance(), accumulating
// project(*pos).
template <typename ListT, typename Project>
FOLLY_ALWAYS_INLINE uint64_t bskipIterateSum(ListT& list, Project&& project) {
  uint64_t sum = 0;
  typename ListT::Skipper skipper(list);
  for (auto pos = skipper.current(); pos; pos = skipper.advance()) {
    sum += project(*pos);
  }
  return sum;
}

constexpr auto kIdentity = [](auto k) { return uint64_t{k}; };
constexpr auto kKey16BToInt = [](const Bench16B& d) {
  return static_cast<uint64_t>(d.id);
};

BENCHMARK(Folly_Iterate, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    doNotOptimizeAway(follyIterateSum<FollySL>(gFolly, kIdentity));
  }
}

BENCHMARK_RELATIVE(BSkip_Iterate, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    doNotOptimizeAway(bskipIterateSum(*gBSkip, kIdentity));
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// Key16B Iterate (full scan)
// ============================================================================

BENCHMARK(Folly_Key16B_Iterate, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    doNotOptimizeAway(follyIterateSum<FollySL16B>(gFollyKey16B, kKey16BToInt));
  }
}

BENCHMARK_RELATIVE(Key16B_Locked_Iterate, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    doNotOptimizeAway(bskipIterateSum(*gLocked16BBSkip, kKey16BToInt));
  }
}

BENCHMARK_RELATIVE(Key16B_Optimistic_Iterate, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    doNotOptimizeAway(bskipIterateSum(*gOptimistic16BBSkip, kKey16BToInt));
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// ForEachElement (full scan)
// ============================================================================

BENCHMARK(BSkip_ForEachElement, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    uint64_t sum = 0;
    gBSkip->forEachElement([&sum](uint32_t key) { sum += key; });
    doNotOptimizeAway(sum);
  }
}

BENCHMARK(Folly_Key16B_ForEachElement, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    uint64_t sum = 0;
    FollySL16B::Skipper skipper(gFollyKey16B);
    while (skipper.good()) {
      sum += static_cast<uint64_t>(skipper.data().id);
      ++skipper;
    }
    doNotOptimizeAway(sum);
  }
}

BENCHMARK_RELATIVE(Key16B_Locked_ForEachElement, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    uint64_t sum = 0;
    gLocked16BBSkip->forEachElement([&sum](const Bench16B& key16b) {
      sum += static_cast<uint64_t>(key16b.id);
    });
    doNotOptimizeAway(sum);
  }
}

BENCHMARK_RELATIVE(Key16B_Optimistic_ForEachElement, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    uint64_t sum = 0;
    gOptimistic16BBSkip->forEachElement([&sum](const Bench16B& key16b) {
      sum += static_cast<uint64_t>(key16b.id);
    });
    doNotOptimizeAway(sum);
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// Insert (random sparse keys into fresh list)
// ============================================================================

BENCHMARK(Folly_Insert, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    auto list = FollySL::createInstance(10);
    FollySL::Accessor acc(list);
    for (auto k : gShuffledKeys) {
      acc.add(k);
    }
    doNotOptimizeAway(acc.size());
  }
}

BENCHMARK_RELATIVE(BSkip_Insert, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    BSkip32 list;
    for (auto k : gShuffledKeys) {
      list.add(k);
    }
    doNotOptimizeAway(list.size());
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// Key16B Insert (random sparse keys into fresh list)
// ============================================================================

BENCHMARK(Folly_Key16B_Insert, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    const auto list = FollySL16B::createInstance(10);
    FollySL16B::Accessor acc(list);
    for (const auto& key16b : gKey16BShuffledKeys) {
      acc.add(key16b);
    }
    doNotOptimizeAway(acc.size());
  }
}

BENCHMARK_RELATIVE(Key16B_Locked_Insert, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    Locked16BBSkip list;
    for (const auto& key16b : gKey16BShuffledKeys) {
      list.add(key16b);
    }
    doNotOptimizeAway(list.size());
  }
}

BENCHMARK_RELATIVE(Key16B_Optimistic_Insert, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    Optimistic16BBSkip list;
    for (const auto& key16b : gKey16BShuffledKeys) {
      list.add(key16b);
    }
    doNotOptimizeAway(list.size());
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// Dense stride-1: 90% fill, sequential scan through every key in order.
// ============================================================================

// NOLINTBEGIN(facebook-avoid-non-const-global-variables)
static std::vector<uint32_t> gDenseKeys; // 90K keys from [1..100K] (90% fill)
static std::vector<uint32_t> gDenseTargets; // every key (stride=1)
static std::unique_ptr<BSkip32> gDenseBSkip;
static std::shared_ptr<FollySL> gDenseFolly;
static std::shared_ptr<FollySL16B> gDenseFollyKey16B;
static std::vector<Bench16B> gDenseKey16BKeys;
static std::vector<Bench16B> gDenseKey16BTargets;
static std::unique_ptr<Locked16BBSkip> gDenseLocked16BBSkip;
static std::unique_ptr<Optimistic16BBSkip> gDenseOptimistic16BBSkip;
// NOLINTEND(facebook-avoid-non-const-global-variables)

static void initDenseData() {
  static bool done = false;
  if (done) {
    return;
  }
  done = true;

  // 90% fill: 90K keys from a 100K universe
  static constexpr uint32_t kDenseUniverse = 100'000;
  static constexpr uint32_t kDenseSize = 90'000;

  std::mt19937 rng(123);
  std::set<uint32_t> keySet;
  while (static_cast<uint32_t>(keySet.size()) < kDenseSize) {
    keySet.insert((rng() % kDenseUniverse) + 1);
  }
  gDenseKeys.assign(keySet.begin(), keySet.end());

  // Targets = every key in the universe (stride-1 sequential scan).
  for (uint32_t i = 1; i <= kDenseUniverse; ++i) {
    gDenseTargets.push_back(i);
    gDenseKey16BTargets.push_back(makeKey16B(i));
  }

  // Insert in random order
  auto shuffled = gDenseKeys;
  std::shuffle(shuffled.begin(), shuffled.end(), rng);
  gDenseKey16BKeys.reserve(gDenseKeys.size());
  for (auto key : gDenseKeys) {
    gDenseKey16BKeys.push_back(makeKey16B(key));
  }

  gDenseBSkip = std::make_unique<BSkip32>();
  gDenseFolly = FollySL::createInstance(10);
  gDenseFollyKey16B = FollySL16B::createInstance(10);
  gDenseLocked16BBSkip = std::make_unique<Locked16BBSkip>();
  gDenseOptimistic16BBSkip = std::make_unique<Optimistic16BBSkip>();
  {
    FollySL::Accessor acc(gDenseFolly);
    FollySL16B::Accessor key16bAcc(gDenseFollyKey16B);
    for (auto k : shuffled) {
      gDenseBSkip->add(k);
      acc.add(k);
      auto key16b = makeKey16B(k);
      key16bAcc.add(key16b);
      gDenseLocked16BBSkip->add(key16b);
      gDenseOptimistic16BBSkip->add(key16b);
    }
  }
}

// Stride-1 through the set's own keys: every skipTo advances to the
// next element. This is the absolute worst case for BSkip's scan-from-0
// because Phase 0 (current key >= target) always misses.
BENCHMARK(Folly_SkipTo1_Keys, iters) {
  initDenseData();
  for (size_t i = 0; i < iters; ++i) {
    FollySL::Skipper skipper(gDenseFolly);
    for (auto t : gDenseKeys) {
      skipper.to(t);
    }
  }
}

BENCHMARK_RELATIVE(BSkip_SkipTo1_Keys, iters) {
  initDenseData();
  for (size_t i = 0; i < iters; ++i) {
    BSkip32::Skipper skipper(*gDenseBSkip);
    for (auto t : gDenseKeys) {
      skipper.skipTo(t);
    }
  }
}

BENCHMARK_DRAW_LINE();

BENCHMARK(Folly_Key16B_SkipTo1_Keys, iters) {
  initDenseData();
  for (size_t i = 0; i < iters; ++i) {
    FollySL16B::Skipper skipper(gDenseFollyKey16B);
    for (const auto& t : gDenseKey16BKeys) {
      skipper.to(t);
    }
  }
}

BENCHMARK_RELATIVE(Key16B_Locked_SkipTo1_Keys, iters) {
  initDenseData();
  for (size_t i = 0; i < iters; ++i) {
    Locked16BBSkip::Skipper skipper(*gDenseLocked16BBSkip);
    for (const auto& t : gDenseKey16BKeys) {
      skipper.skipTo(t);
    }
  }
}

BENCHMARK_RELATIVE(Key16B_Optimistic_SkipTo1_Keys, iters) {
  initDenseData();
  for (size_t i = 0; i < iters; ++i) {
    Optimistic16BBSkip::Skipper skipper(*gDenseOptimistic16BBSkip);
    for (const auto& t : gDenseKey16BKeys) {
      skipper.skipTo(t);
    }
  }
}

BENCHMARK_DRAW_LINE();

// Stride-1 through the full key universe. 10% of targets are misses.
BENCHMARK(Folly_SkipTo1_Universe, iters) {
  initDenseData();
  for (size_t i = 0; i < iters; ++i) {
    FollySL::Skipper skipper(gDenseFolly);
    for (auto t : gDenseTargets) {
      skipper.to(t);
    }
  }
}

BENCHMARK_RELATIVE(BSkip_SkipTo1_Universe, iters) {
  initDenseData();
  for (size_t i = 0; i < iters; ++i) {
    BSkip32::Skipper skipper(*gDenseBSkip);
    for (auto t : gDenseTargets) {
      skipper.skipTo(t);
    }
  }
}

BENCHMARK_DRAW_LINE();

BENCHMARK(Folly_Key16B_SkipTo1_Universe, iters) {
  initDenseData();
  for (size_t i = 0; i < iters; ++i) {
    FollySL16B::Skipper skipper(gDenseFollyKey16B);
    for (const auto& t : gDenseKey16BTargets) {
      skipper.to(t);
    }
  }
}

BENCHMARK_RELATIVE(Key16B_Locked_SkipTo1_Universe, iters) {
  initDenseData();
  for (size_t i = 0; i < iters; ++i) {
    Locked16BBSkip::Skipper skipper(*gDenseLocked16BBSkip);
    for (const auto& t : gDenseKey16BTargets) {
      skipper.skipTo(t);
    }
  }
}

BENCHMARK_RELATIVE(Key16B_Optimistic_SkipTo1_Universe, iters) {
  initDenseData();
  for (size_t i = 0; i < iters; ++i) {
    Optimistic16BBSkip::Skipper skipper(*gDenseOptimistic16BBSkip);
    for (const auto& t : gDenseKey16BTargets) {
      skipper.skipTo(t);
    }
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// Multi-threaded: N readers + M writers
// ============================================================================

template <int NumReaders, int NumWriters>
void bmConcurrentReadWrite(size_t iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    BSkip32 list;
    for (auto k : gShuffledKeys) {
      list.add(k);
    }

    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    threads.reserve(NumWriters + NumReaders);

    for (int w = 0; w < NumWriters; ++w) {
      threads.emplace_back([&list, &stop, w]() {
        uint32_t key = kUniverse + 1 + w * 100000;
        while (!stop.load(std::memory_order_relaxed)) {
          list.add(key++);
        }
      });
    }

    for (int r = 0; r < NumReaders; ++r) {
      threads.emplace_back([&list, &stop, r]() {
        while (!stop.load(std::memory_order_relaxed)) {
          BSkip32::Skipper skipper(list);
          for (size_t j = r; j < gSkipTargets10.size(); j += NumReaders) {
            skipper.skipTo(gSkipTargets10[j]);
          }
        }
      });
    }

    /* sleep override */
    std::this_thread::sleep_for( // NOLINT(facebook-hte-BadCall-sleep_for)
        std::chrono::milliseconds(100));
    stop.store(true);
    for (auto& t : threads) {
      t.join();
    }
    doNotOptimizeAway(list.size());
  }
}

BENCHMARK(BSkip_4R_0W, iters) {
  bmConcurrentReadWrite<4, 0>(iters);
}
BENCHMARK(BSkip_4R_1W, iters) {
  bmConcurrentReadWrite<4, 1>(iters);
}
BENCHMARK(BSkip_8R_2W, iters) {
  bmConcurrentReadWrite<8, 2>(iters);
}
BENCHMARK(BSkip_8R_4W, iters) {
  bmConcurrentReadWrite<8, 4>(iters);
}

BENCHMARK_DRAW_LINE();

template <typename ListType, int NumReaders, int NumWriters>
void bmConcurrentKey16BReadWrite(size_t iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    ListType list;
    for (const auto& key16b : gKey16BShuffledKeys) {
      list.add(key16b);
    }

    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    threads.reserve(NumWriters + NumReaders);

    for (int w = 0; w < NumWriters; ++w) {
      threads.emplace_back([&list, &stop, w]() {
        int64_t key = -1 - static_cast<int64_t>(w) * 100000;
        while (!stop.load(std::memory_order_relaxed)) {
          list.add(makeTransientKey16B(key--));
        }
      });
    }

    for (int r = 0; r < NumReaders; ++r) {
      threads.emplace_back([&list, &stop, r]() {
        while (!stop.load(std::memory_order_relaxed)) {
          typename ListType::Skipper skipper(list);
          for (size_t j = r; j < gKey16BSkipTargets10.size(); j += NumReaders) {
            skipper.skipTo(gKey16BSkipTargets10[j]);
          }
        }
      });
    }

    std::this_thread::sleep_for( // NOLINT(facebook-hte-BadCall-sleep_for)
        std::chrono::milliseconds(100));
    stop.store(true);
    for (auto& t : threads) {
      t.join();
    }
    doNotOptimizeAway(list.size());
  }
}

template <int NumReaders, int NumWriters>
void bmConcurrentFollyKey16BReadWrite(size_t iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    auto list = FollySL16B::createInstance(10);
    {
      FollySL16B::Accessor acc(list);
      for (const auto& key16b : gKey16BShuffledKeys) {
        acc.add(key16b);
      }
    }

    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    threads.reserve(NumWriters + NumReaders);

    for (int w = 0; w < NumWriters; ++w) {
      threads.emplace_back([&list, &stop, w]() {
        FollySL16B::Accessor acc(list);
        int64_t key = -1 - static_cast<int64_t>(w) * 100000;
        while (!stop.load(std::memory_order_relaxed)) {
          acc.add(makeTransientKey16B(key--));
        }
      });
    }

    for (int r = 0; r < NumReaders; ++r) {
      threads.emplace_back([&list, &stop, r]() {
        while (!stop.load(std::memory_order_relaxed)) {
          FollySL16B::Skipper skipper(list);
          for (size_t j = r; j < gKey16BSkipTargets10.size(); j += NumReaders) {
            skipper.to(gKey16BSkipTargets10[j]);
          }
        }
      });
    }

    std::this_thread::sleep_for( // NOLINT(facebook-hte-BadCall-sleep_for)
        std::chrono::milliseconds(100));
    stop.store(true);
    for (auto& t : threads) {
      t.join();
    }
    FollySL16B::Accessor acc(list);
    doNotOptimizeAway(acc.size());
  }
}

BENCHMARK(Folly_Key16B_4R_0W, iters) {
  bmConcurrentFollyKey16BReadWrite<4, 0>(iters);
}
BENCHMARK_RELATIVE(Key16B_Locked_4R_0W, iters) {
  bmConcurrentKey16BReadWrite<Locked16BBSkip, 4, 0>(iters);
}
BENCHMARK_RELATIVE(Key16B_Optimistic_4R_0W, iters) {
  bmConcurrentKey16BReadWrite<Optimistic16BBSkip, 4, 0>(iters);
}
BENCHMARK(Folly_Key16B_4R_1W, iters) {
  bmConcurrentFollyKey16BReadWrite<4, 1>(iters);
}
BENCHMARK_RELATIVE(Key16B_Locked_4R_1W, iters) {
  bmConcurrentKey16BReadWrite<Locked16BBSkip, 4, 1>(iters);
}
BENCHMARK_RELATIVE(Key16B_Optimistic_4R_1W, iters) {
  bmConcurrentKey16BReadWrite<Optimistic16BBSkip, 4, 1>(iters);
}
BENCHMARK(Folly_Key16B_8R_2W, iters) {
  bmConcurrentFollyKey16BReadWrite<8, 2>(iters);
}
BENCHMARK_RELATIVE(Key16B_Locked_8R_2W, iters) {
  bmConcurrentKey16BReadWrite<Locked16BBSkip, 8, 2>(iters);
}
BENCHMARK_RELATIVE(Key16B_Optimistic_8R_2W, iters) {
  bmConcurrentKey16BReadWrite<Optimistic16BBSkip, 8, 2>(iters);
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// Construction: empty list construction/destruction cost
// ============================================================================

BENCHMARK(BSkip_Construction, iters) {
  for (size_t i = 0; i < iters; ++i) {
    BSkip32 list;
    doNotOptimizeAway(&list);
  }
}

BENCHMARK_DRAW_LINE();

BENCHMARK(Folly_Key16B_SmallListInsert10, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    auto list = FollySL16B::createInstance(10);
    FollySL16B::Accessor acc(list);
    for (uint32_t j = 0; j < 10; ++j) {
      acc.add(gKey16BShuffledKeys[j]);
    }
    doNotOptimizeAway(acc.size());
  }
}

BENCHMARK_RELATIVE(Key16B_Locked_SmallListInsert10, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    Locked16BBSkip list;
    for (uint32_t j = 0; j < 10; ++j) {
      list.add(gKey16BShuffledKeys[j]);
    }
    doNotOptimizeAway(list.size());
  }
}

BENCHMARK_RELATIVE(Key16B_Optimistic_SmallListInsert10, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    Optimistic16BBSkip list;
    for (uint32_t j = 0; j < 10; ++j) {
      list.add(gKey16BShuffledKeys[j]);
    }
    doNotOptimizeAway(list.size());
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// SmallList: insert only 10 keys (tiny posting list overhead benchmark)
// ============================================================================

BENCHMARK(BSkip_SmallListInsert10, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    BSkip32 list;
    for (uint32_t j = 0; j < 10; ++j) {
      list.add(gShuffledKeys[j]);
    }
    doNotOptimizeAway(list.size());
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// AoS vs SoA: SkipTo stride=10 (key scan hot path)
// ============================================================================

BENCHMARK(AoS_B16_SkipTo10, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    BSkipAoS16::Skipper skipper(*gBSkipAoS16);
    for (auto t : gSkipTargets10) {
      skipper.skipTo(AoSKey12{t, 0});
    }
  }
}

BENCHMARK_RELATIVE(SoA_B16_SkipTo10, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    BSkip32SoA16::Skipper skipper(*gBSkipSoA16);
    for (auto t : gSkipTargets10) {
      skipper.skipTo(t);
    }
  }
}

BENCHMARK_RELATIVE(SoA_B32_SkipTo10, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    BSkip32SoA32::Skipper skipper(*gBSkipSoA32);
    for (auto t : gSkipTargets10) {
      skipper.skipTo(t);
    }
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// AoS vs SoA: SkipTo stride=100
// ============================================================================

BENCHMARK(AoS_B16_SkipTo100, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    BSkipAoS16::Skipper skipper(*gBSkipAoS16);
    for (auto t : gSkipTargets100) {
      skipper.skipTo(AoSKey12{t, 0});
    }
  }
}

BENCHMARK_RELATIVE(SoA_B16_SkipTo100, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    BSkip32SoA16::Skipper skipper(*gBSkipSoA16);
    for (auto t : gSkipTargets100) {
      skipper.skipTo(t);
    }
  }
}

BENCHMARK_RELATIVE(SoA_B32_SkipTo100, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    BSkip32SoA32::Skipper skipper(*gBSkipSoA32);
    for (auto t : gSkipTargets100) {
      skipper.skipTo(t);
    }
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// AoS vs SoA: Find (point lookup including payload read)
// ============================================================================

BENCHMARK(AoS_B16_Find, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    for (size_t j = 0; j < 1000; ++j) {
      auto r = gBSkipAoS16->find(AoSKey12{gKeys[j], 0});
      doNotOptimizeAway(r);
    }
  }
}

BENCHMARK_RELATIVE(SoA_B16_Find, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    for (size_t j = 0; j < 1000; ++j) {
      auto r = gBSkipSoA16->find(gKeys[j]);
      doNotOptimizeAway(r);
    }
  }
}

BENCHMARK_RELATIVE(SoA_B32_Find, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    for (size_t j = 0; j < 1000; ++j) {
      auto r = gBSkipSoA32->find(gKeys[j]);
      doNotOptimizeAway(r);
    }
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// AoS vs SoA: Insert (key + payload shift cost)
// ============================================================================

BENCHMARK(AoS_B16_Insert10K, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    BSkipAoS16 list;
    for (uint32_t j = 0; j < 10000; ++j) {
      uint32_t k = gShuffledKeys[j];
      list.add(AoSKey12{k, static_cast<uint64_t>(k) * 7});
    }
    doNotOptimizeAway(list.size());
  }
}

BENCHMARK_RELATIVE(SoA_B16_Insert10K, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    BSkip32SoA16 list;
    for (uint32_t j = 0; j < 10000; ++j) {
      uint32_t k = gShuffledKeys[j];
      list.add(k, static_cast<uint64_t>(k) * 7);
    }
    doNotOptimizeAway(list.size());
  }
}

BENCHMARK_RELATIVE(SoA_B32_Insert10K, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    BSkip32SoA32 list;
    for (uint32_t j = 0; j < 10000; ++j) {
      uint32_t k = gShuffledKeys[j];
      list.add(k, static_cast<uint64_t>(k) * 7);
    }
    doNotOptimizeAway(list.size());
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// AoS vs SoA: ForEach (full scan)
// ============================================================================

BENCHMARK(AoS_B16_ForEach, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    size_t count = 0;
    gBSkipAoS16->forEachElement([&](const AoSKey12& key) {
      doNotOptimizeAway(key.hitword);
      ++count;
    });
    doNotOptimizeAway(count);
  }
}

BENCHMARK_RELATIVE(SoA_B16_ForEach, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    size_t count = 0;
    gBSkipSoA16->forEachElement([&](uint32_t key, uint64_t payload) {
      doNotOptimizeAway(key);
      doNotOptimizeAway(payload);
      ++count;
    });
    doNotOptimizeAway(count);
  }
}

BENCHMARK_RELATIVE(SoA_B32_ForEach, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    size_t count = 0;
    gBSkipSoA32->forEachElement([&](uint32_t key, uint64_t payload) {
      doNotOptimizeAway(key);
      doNotOptimizeAway(payload);
      ++count;
    });
    doNotOptimizeAway(count);
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// Key16B AoS vs SoA: SkipTo stride=10
// ============================================================================

BENCHMARK(Key16B_AoS_B24_SkipTo10, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    BSkipAoS16B24::Skipper skipper(*gBSkipAoS16B);
    for (const auto& t : gKey16BSkipTargets10) {
      skipper.skipTo(AoS16BKey24{t, 0});
    }
  }
}

BENCHMARK_RELATIVE(Key16B_SoA_B16_SkipTo10, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    BSkipSoA16B16::Skipper skipper(*gBSkipSoA16B16);
    for (const auto& t : gKey16BSkipTargets10) {
      skipper.skipTo(t);
    }
  }
}

BENCHMARK_RELATIVE(Key16B_SoA_B24_SkipTo10, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    BSkipSoA16B24::Skipper skipper(*gBSkipSoA16B24);
    for (const auto& t : gKey16BSkipTargets10) {
      skipper.skipTo(t);
    }
  }
}

BENCHMARK_RELATIVE(Key16B_SoA_B32_SkipTo10, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    BSkipSoA16B32::Skipper skipper(*gBSkipSoA16B32);
    for (const auto& t : gKey16BSkipTargets10) {
      skipper.skipTo(t);
    }
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// Key16B AoS vs SoA: Find
// ============================================================================

BENCHMARK(Key16B_AoS_B24_Find, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    for (size_t j = 0; j < 1000; ++j) {
      auto r = gBSkipAoS16B->find(AoS16BKey24{gKey16BKeys[j], 0});
      doNotOptimizeAway(r);
    }
  }
}

BENCHMARK_RELATIVE(Key16B_SoA_B16_Find, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    for (size_t j = 0; j < 1000; ++j) {
      auto r = gBSkipSoA16B16->find(gKey16BKeys[j]);
      doNotOptimizeAway(r);
    }
  }
}

BENCHMARK_RELATIVE(Key16B_SoA_B24_Find, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    for (size_t j = 0; j < 1000; ++j) {
      auto r = gBSkipSoA16B24->find(gKey16BKeys[j]);
      doNotOptimizeAway(r);
    }
  }
}

BENCHMARK_RELATIVE(Key16B_SoA_B32_Find, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    for (size_t j = 0; j < 1000; ++j) {
      auto r = gBSkipSoA16B32->find(gKey16BKeys[j]);
      doNotOptimizeAway(r);
    }
  }
}

BENCHMARK_DRAW_LINE();

// ============================================================================
// Key16B AoS vs SoA: Insert
// ============================================================================

BENCHMARK(Key16B_AoS_B24_Insert10K, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    BSkipAoS16B24 list;
    for (uint32_t j = 0; j < 10000; ++j) {
      list.add(
          AoS16BKey24{gKey16BShuffledKeys[j], static_cast<uint64_t>(j) * 7});
    }
    doNotOptimizeAway(list.size());
  }
}

BENCHMARK_RELATIVE(Key16B_SoA_B16_Insert10K, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    BSkipSoA16B16 list;
    for (uint32_t j = 0; j < 10000; ++j) {
      list.add(gKey16BShuffledKeys[j], static_cast<uint64_t>(j) * 7);
    }
    doNotOptimizeAway(list.size());
  }
}

BENCHMARK_RELATIVE(Key16B_SoA_B24_Insert10K, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    BSkipSoA16B24 list;
    for (uint32_t j = 0; j < 10000; ++j) {
      list.add(gKey16BShuffledKeys[j], static_cast<uint64_t>(j) * 7);
    }
    doNotOptimizeAway(list.size());
  }
}

BENCHMARK_RELATIVE(Key16B_SoA_B32_Insert10K, iters) {
  initData();
  for (size_t i = 0; i < iters; ++i) {
    BSkipSoA16B32 list;
    for (uint32_t j = 0; j < 10000; ++j) {
      list.add(gKey16BShuffledKeys[j], static_cast<uint64_t>(j) * 7);
    }
    doNotOptimizeAway(list.size());
  }
}

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  runBenchmarks();
  return 0;
}
