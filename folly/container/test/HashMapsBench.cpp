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

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <glog/logging.h>
#include <folly/Benchmark.h>
#include <folly/BenchmarkUtil.h>
#include <folly/Conv.h>
#include <folly/Function.h>
#include <folly/Random.h>
#include <folly/hash/Hash.h>
#include <folly/init/Init.h>
#include <folly/lang/Align.h>
#include <folly/lang/Exception.h>
#include <folly/portability/GFlags.h>

#include <folly/container/F14Map.h>

using namespace folly;

// Depending the max load factor and the rehashing policy, the map size could
// affect benchmark results quite a bit. For example, a map that's just rehashed
// could have a lower probability of collisions, but the rehash time would get
// counted toward the operation that triggered the rehash.
DEFINE_int32(
    map_size_min,
    11,
    "min number of entries to benchmark each map with (inclusive)");
DEFINE_int32(
    map_size_max,
    100000,
    "max number of entries to benchmark each map with (inclusive)");
DEFINE_int32(
    map_size_step, 32, "multiplier for each benchmark between iterations");

DEFINE_bool(
    map_locality, false, "Run the fixed-work F14 locality matrix instead.");
DEFINE_uint32(
    map_locality_small_entries,
    128,
    "Entries for non-collision hot/eviction fixtures; collisions use fixed 6/64.");
DEFINE_uint32(
    map_locality_evict_iters,
    128,
    "Eviction lookups, a positive multiple of 128.");
DEFINE_uint32(map_locality_seed, 42, "Fixed locality query-shuffle seed.");
DEFINE_uint32(
    map_locality_large_llc_factor,
    2,
    "Minimum large header-line footprint / LLC.");
DEFINE_uint64(
    map_locality_hot_max_bytes, 16384, "Hot map+query allocation budget.");
DEFINE_uint64(
    map_locality_max_bytes,
    uint64_t{8} << 30,
    "Per-process active map+query+reserved eviction-buffer budget, not RSS.");

//////// Key related preparation ////////

namespace {
static const std::string kPadding("0123456789012345678901234567890123");
struct NonSSOString : public std::string {
  template <typename... Args>
  explicit NonSSOString(Args&&... args)
      : std::string(std::string(std::forward<Args>(args)...) + kPadding) {}

  NonSSOString(NonSSOString const&) = default;
  NonSSOString& operator=(NonSSOString const&) = default;
  NonSSOString(NonSSOString&&) = default;
  NonSSOString& operator=(NonSSOString&&) = default;
};
} // namespace

namespace std {
template <>
struct hash<NonSSOString> {
  size_t operator()(const NonSSOString& s) const { return hash<string>()(s); }
};

#ifdef __GLIBCXX__
template <>
struct __is_fast_hash<hash<NonSSOString>> : public std::false_type {};

static_assert(
    __cache_default<string, hash<string>>::value ==
        __cache_default<NonSSOString, hash<NonSSOString>>::value,
    "To draw a fair comparison, the policy of whether to cache hash codes "
    "for NonSSOString keys should be the same as for std::string keys.");
#endif
} // namespace std

namespace folly {
template <typename K>
struct IsAvalanchingHasher<std::hash<NonSSOString>, K> : std::true_type {};
} // namespace folly

const int kSalt = 0x619abc7e;

template <typename K>
K keyGen(uint32_t key) {
  return folly::to<K>(hash::jenkins_rev_mix32(key ^ kSalt));
}

template <>
NonSSOString keyGen<NonSSOString>(uint32_t key) {
  return NonSSOString(to<std::string>(key));
}

template <class K>
std::vector<K>& keyList(int /*max*/ = 0) {
  static std::vector<K> keys;
  return keys;
}

template <class K>
void prepare(size_t max) {
  auto& keys = keyList<K>();
  if (keys.size() < max) {
    for (auto key = keys.size(); key < max; ++key) {
      keys.push_back(keyGen<K>(key));
    }
  }
}

template <class K>
const K& key(int index) {
  return keyList<K>()[index];
}

template <typename TArray>
TArray value(int i) {
  // quick and dirty hack, since we don't test for correctness, ignore any
  // narrowing that might happen.
  return {static_cast<typename TArray::value_type>(i)};
}

template <template <class, class> class Map, class K, class V, class Test>
void benchmarkFilledMap(int runs, int size, const Test& test, int div = 1) {
  BenchmarkSuspender braces;
  Map<K, V> map(size);
  prepare<K>(size);
  for (int i = 0; i < size; i += div) {
    auto& k = key<K>(i);
    map.insert(std::pair<K, V>(k, value<V>(i * 3)));
  }
  folly::makeUnpredictable(map);
  braces.dismissing([&] {
    for (int r = 0; r < runs; ++r) {
      test(map);
    }
  });
}

static auto& getRNG() {
  static std::mt19937 rng{42};
  return rng;
}

template <template <class, class> class Map, class K, class V, class Test>
void benchmarkManyFilledMapsByKey(
    int runs, int size, const Test& test, int div = 1) {
  BenchmarkSuspender braces;
  Map<K, V> maps[64];
  std::vector<K> toInsert;
  prepare<K>(size);
  for (int i = 0; i < size; ++i) {
    auto& k = key<K>(i);
    toInsert.push_back(k);
  }
  for (int i = 0; i * div < size; ++i) {
    maps[i % 64].insert(std::pair<K, V>(toInsert[i], value<V>(i * 3)));
  }
  std::shuffle(toInsert.begin(), toInsert.end(), getRNG());
  folly::makeUnpredictable(maps);
  folly::makeUnpredictable(toInsert);
  braces.dismissing([&] {
    for (int r = 0; r < runs; ++r) {
      for (int i = 0; i < size; i += div) {
        test(maps[i % 64], toInsert[i]);
      }
    }
  });
}

template <template <class, class> class Map, class K, class V, class Test>
void benchmarkFilledMapByKey(
    int runs, int size, const Test& test, int div = 1) {
  BenchmarkSuspender braces;
  Map<K, V> map(size);
  std::vector<K> toInsert;
  prepare<K>(size);
  for (int i = 0; i < size; ++i) {
    auto& k = key<K>(i);
    toInsert.push_back(k);
  }
  for (int i = 0; i * div < size; ++i) {
    map.insert(std::pair<K, V>(toInsert[i], value<V>(i * 3)));
  }
  std::shuffle(toInsert.begin(), toInsert.end(), getRNG());
  folly::makeUnpredictable(map);
  folly::makeUnpredictable(toInsert);
  braces.dismissing([&] {
    for (int r = 0; r < runs; ++r) {
      for (int i = 0; i < size; i += div) {
        test(map, toInsert[i]);
      }
    }
  });
}

template <
    template <class, class> class Map,
    class K,
    class V,
    class... Args,
    class Test>
void benchmarkFromEmptyArgs(int runs, int size, Test test, Args... args) {
  BenchmarkSuspender braces;
  prepare<K>(size);
  for (int r = 0; r < runs; ++r) {
    Map<K, V> map{args...};
    folly::makeUnpredictable(map);
    braces.dismissing([&] {
      for (int i = 0; i < size; ++i) {
        test(map, i);
      }
    });
  }
}

template <template <class, class> class Map, class K, class V>
void benchmarkInsert(int runs, int size) {
  benchmarkFromEmptyArgs<Map, K, V>(
      runs,
      size,
      [](Map<K, V>& m, int i) {
        m.insert(std::pair<K, V>(key<K>(i), value<V>(i)));
      },
      unsigned(size));
}

template <template <class, class> class Map, class K, class V>
void benchmarkInsertGrow(int runs, int size) {
  benchmarkFromEmptyArgs<Map, K, V>(runs, size, [](Map<K, V>& m, int i) {
    m.insert(std::pair<K, V>(key<K>(i), value<V>(i)));
  });
}

template <template <class, class> class Map, class K, class V>
void benchmarkInsertSqBr(int runs, int size) {
  benchmarkFromEmptyArgs<Map, K, V>(runs, size, [](Map<K, V>& m, int i) {
    m[key<K>(i)] = value<V>(i);
  });
}

template <template <class, class> class Map, class K, class V>
void benchmarkFind(int runs, int size) {
  int x = 0;
  benchmarkFilledMapByKey<Map, K, V>(
      runs,
      size,
      [&](Map<K, V>& m, const K& key) {
        auto found = m.find(key);
        if (found != m.end()) {
          x ^= found->second[0];
          folly::doNotOptimizeAway(x);
        }
      },
      2);
}

template <template <class, class> class Map, class K, class V>
void benchmarkManyFind(int runs, int size) {
  int x = 0;
  benchmarkManyFilledMapsByKey<Map, K, V>(
      runs,
      size,
      [&](Map<K, V>& m, const K& key) {
        auto found = m.find(key);
        if (found != m.end()) {
          x ^= found->second[0];
          folly::doNotOptimizeAway(x);
        }
      },
      2);
}

template <template <class, class> class Map, class K, class V>
void benchmarkSqBrFind(int runs, int size) {
  int x = 0;
  benchmarkFilledMapByKey<Map, K, V>(
      runs,
      size,
      [&](Map<K, V>& m, const K& key) {
        x ^= m[key][0];
        folly::doNotOptimizeAway(x);
      },
      2);
}

template <template <class, class> class Map, class K, class V>
void benchmarkErase(int runs, int size) {
  for (int i = 0; i < runs; ++i) {
    benchmarkFilledMapByKey<Map, K, V>(
        1, size, [&](Map<K, V>& m, const K& key) { m.erase(key); }, 2);
  }
}

template <template <class, class> class Map, class K, class V>
void benchmarkDtor(int runs, int size) {
  for (int i = 0; i < runs; ++i) {
    benchmarkFilledMap<Map, K, V>(
        1,
        size,
        [&](Map<K, V>& m) {
          Map<K, V> toDestruct;
          swap(m, toDestruct);
        },
        2);
  }
}

template <template <class, class> class Map, class K, class V>
void benchmarkClear(int runs, int size) {
  for (int i = 0; i < runs; ++i) {
    benchmarkFilledMap<Map, K, V>(1, size, [&](Map<K, V>& m) { m.clear(); }, 1);
  }
}

template <template <class, class> class Map, class K, class V>
void benchmarkCopyCtor(int runs, int size) {
  Map<K, V> n;
  for (int i = 0; i < runs; ++i) {
    benchmarkFilledMap<Map, K, V>(1, size, [&](Map<K, V>& m) { n = m; }, 1);
    folly::doNotOptimizeAway(n);
  }
}

template <template <class, class> class Map, class K, class V>
void benchmarkIter(int runs, int size) {
  int x = 0;
  benchmarkFilledMap<Map, K, V>(runs, size, [&](Map<K, V>& m) {
    for (auto& entry : m) {
      x ^= entry.second[0];
    }
  });
  folly::doNotOptimizeAway(x);
}

template <template <class, class> class Map, class K, class V>
void benchmarkSparseIter(int runs, int size) {
  int x = 0;
  benchmarkFilledMap<Map, K, V>(
      runs,
      size,
      [&](Map<K, V>& m) {
        for (auto& entry : m) {
          x ^= entry.second[0];
        }
      },
      8);
  folly::doNotOptimizeAway(x);
}

template <template <class, class> class Map, class K, class V>
void benchmarkCtorWithCapacity(int runs, int size) {
  for (int ii = 0; ii < runs; ++ii) {
    Map<K, V> map(size);
    folly::doNotOptimizeAway(map);
  }
}

template <class K, class V>
using unord = std::unordered_map<K, V>;
template <class K, class V>
using f14val = F14ValueMap<K, V>;
template <class K, class V>
using f14node = F14NodeMap<K, V>;
template <class K, class V>
using f14vec = F14VectorMap<K, V>;

void runAllHashMapTests() {
  using std::map;
  using std::string;
  std::vector<string> testOrder;
  map<size_t,
      map<string,
          map<string, map<string, map<string, std::function<int(int)>>>>>>
      tests;

#define Z(test, map, key, value_size)                                          \
  for (auto size = FLAGS_map_size_min; size <= FLAGS_map_size_max;             \
       size *= FLAGS_map_size_step) {                                          \
    auto value = fmt::format("a[{}]", #value_size);                            \
    tests[size][#test][#key][value][#map] = [=](int iters) {                   \
      benchmark##test<map, key, std::array<uint8_t, value_size>>(iters, size); \
      return iters;                                                            \
    };                                                                         \
  }

#define Y(test, map)             \
  Z(test, map, uint64_t, 1)      \
  Z(test, map, std::string, 1)   \
  Z(test, map, NonSSOString, 1)  \
  Z(test, map, uint64_t, 128)    \
  Z(test, map, std::string, 128) \
  Z(test, map, NonSSOString, 128)

#define X(test)               \
  testOrder.push_back(#test); \
  Y(test, unord)              \
  Y(test, f14val)             \
  Y(test, f14node)            \
  Y(test, f14vec)

  X(Insert);
  X(InsertSqBr);
  X(InsertGrow);
  X(Find);
  X(ManyFind);
  X(SqBrFind);
  X(Erase);
  X(Iter);
  X(SparseIter);
  X(Clear);
  X(Dtor);
  X(CopyCtor);
  X(CtorWithCapacity);

#undef X
#undef Y
#undef Z
  for (auto& size : tests) {
    for (auto& test : testOrder) {
      for (auto& key : size.second[test]) {
        for (auto& value : key.second) {
          bool isBaseline = true;
          for (auto& map : value.second) {
            addBenchmark(
                __FILE__,
                fmt::format(
                    "{}{} {:>8}<{}, {}>[{}]",
                    isBaseline ? "" : "%",
                    test,
                    map.first,
                    key.first,
                    value.first,
                    size.first),
                std::move(map.second));
            isBaseline = false;
          }
          addBenchmark(__FILE__, "-", [](int iters) { return iters; });
        }
      }
    }
  }
}

namespace {

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

using std::literals::operator""sv;

enum class LookupLocality { hot, eviction, large };
enum class LookupPattern {
  hit,
  miss,
  mixed,
  no_tag,
  false_tag,
  overflow_no_tag,
  overflow_false_tag,
  overflow_last_hit,
};

struct LookupConfig {
  std::string name;
  LookupLocality locality;
  LookupPattern pattern;
};

constexpr std::array kLookupLocalities{
    std::pair{LookupLocality::hot, "Hot"sv},
    std::pair{LookupLocality::eviction, "Eviction"sv},
    std::pair{LookupLocality::large, "Large"sv},
};
constexpr std::array kLookupPatterns{
    std::pair{LookupPattern::hit, "hit"sv},
    std::pair{LookupPattern::miss, "miss"sv},
    std::pair{LookupPattern::mixed, "mixed"sv},
    std::pair{LookupPattern::no_tag, "no_tag"sv},
    std::pair{LookupPattern::false_tag, "false_tag"sv},
    std::pair{LookupPattern::overflow_no_tag, "overflow_no_tag"sv},
    std::pair{LookupPattern::overflow_false_tag, "overflow_false_tag"sv},
    std::pair{LookupPattern::overflow_last_hit, "overflow_last_hit"sv},
};

bool noTagPattern(LookupPattern pattern) {
  return pattern == LookupPattern::no_tag ||
      pattern == LookupPattern::overflow_no_tag;
}

bool overflowPattern(LookupPattern pattern) {
  return pattern == LookupPattern::overflow_no_tag ||
      pattern == LookupPattern::overflow_false_tag ||
      pattern == LookupPattern::overflow_last_hit;
}

bool collisionPattern(LookupPattern pattern) {
  return noTagPattern(pattern) || pattern == LookupPattern::false_tag ||
      overflowPattern(pattern);
}

size_t maxProbe(std::vector<size_t> const& histo) {
  for (size_t i = histo.size(); i != 0; --i) {
    if (histo[i - 1] != 0) {
      return i - 1;
    }
  }
  return 0;
}

struct LookupCollisionHash {
  // Intentionally non-avalanching; opt out of remixing to preserve fixture
  // tags.
  using folly_is_avalanching = std::true_type;
  size_t operator()(uint64_t key) const {
    return static_cast<size_t>(key & 255) << (sizeof(size_t) * 8 - 8);
  }
};

struct LookupCountEqual {
  size_t* calls;
  bool operator()(uint64_t a, uint64_t b) const {
    ++*calls;
    return a == b;
  }
};

size_t localityEntries(LookupConfig const& config, size_t llc) {
  if (collisionPattern(config.pattern)) {
    return overflowPattern(config.pattern) ? 64 : 6;
  }
  if (config.locality != LookupLocality::large) {
    return FLAGS_map_locality_small_entries;
  }
  auto const factor = FLAGS_map_locality_large_llc_factor;
  if (llc == 0 || llc > std::numeric_limits<size_t>::max() / factor) {
    throw_exception<std::length_error>("Invalid large locality footprint");
  }
  auto const bytes = llc * factor;
  auto const lines = bytes / hardware_constructive_interference_size +
      (bytes % hardware_constructive_interference_size != 0);
  if (lines > std::numeric_limits<uint32_t>::max() / 32) {
    throw_exception<std::length_error>("Too many locality entries");
  }
  // At most 16 slots per chunk; size the allocated header-line arena beyond
  // LLC.
  return lines * 16;
}

uint64_t localityKey(size_t id, bool collision, uint8_t tag = 17) {
  return collision
      ? (uint64_t{id} << 8) | tag
      : keyGen<uint64_t>(static_cast<uint32_t>(id));
}

template <typename V>
V localityValue(size_t id) {
  V result{};
  result[0] = static_cast<uint8_t>(id * 3);
  return result;
}

bool localityHit(LookupPattern pattern, size_t i) {
  return pattern == LookupPattern::hit ||
      pattern == LookupPattern::overflow_last_hit ||
      (pattern == LookupPattern::mixed && i % 2 == 0);
}

template <typename Map>
struct LookupFixture {
  Map map;
  std::vector<uint64_t> queries;
  size_t entries = 0;
  size_t expected_hits = 0;
  uint64_t expected_sum = 0;
  uint64_t trace_hash = 0;
};

template <typename Map>
LookupFixture<Map> makeLookupFixture(LookupConfig const& config, size_t llc) {
  LookupFixture<Map> fixture;
  fixture.entries = localityEntries(config, llc);
  auto const collision = collisionPattern(config.pattern);
  auto const query_count =
      config.locality == LookupLocality::large ? fixture.entries : size_t{128};
  auto const estimate =
      sizeof(typename Map::value_type) + 128 + sizeof(uint64_t);
  auto const limit = std::min<uint64_t>(
      FLAGS_map_locality_max_bytes, std::numeric_limits<size_t>::max());
  if (fixture.entries == 0 ||
      fixture.entries > std::numeric_limits<uint32_t>::max() / 2 ||
      fixture.entries > limit / estimate) {
    throw_exception<std::length_error>(
        "Locality fixture exceeds entry or memory budget");
  }
  fixture.map.reserve(collision ? 128 : fixture.entries);
  for (size_t i = 0; i < fixture.entries; ++i) {
    fixture.map.emplace(
        localityKey(i, collision), localityValue<typename Map::mapped_type>(i));
  }
  fixture.queries.reserve(query_count);
  for (size_t i = 0; i < query_count; ++i) {
    auto const hit = localityHit(config.pattern, i);
    auto const id = config.pattern == LookupPattern::overflow_last_hit
        ? fixture.entries - 1
        : i % fixture.entries + (hit ? 0 : fixture.entries);
    // Tags 17 and 33 have the same probe delta modulo the 16-chunk fixture.
    auto const tag = noTagPattern(config.pattern) ? 33 : 17;
    fixture.queries.push_back(localityKey(id, collision, tag));
    fixture.expected_hits += hit;
    if (hit) {
      fixture.expected_sum +=
          uint64_t{static_cast<uint8_t>((id % fixture.entries) * 3)} + 1;
    }
  }
  std::mt19937 rng{FLAGS_map_locality_seed};
  std::shuffle(fixture.queries.begin(), fixture.queries.end(), rng);
  uint64_t sum = 0;
  size_t hits = 0;
  for (auto key : fixture.queries) {
    auto const found = fixture.map.find(key);
    if (found != fixture.map.end()) {
      ++hits;
      sum += uint64_t{found->second[0]} + 1;
    }
    fixture.trace_hash = hash::hash_combine(fixture.trace_hash, key);
  }
  if (hits != fixture.expected_hits || sum != fixture.expected_sum) {
    throw_exception<std::runtime_error>("Locality query oracle failed");
  }
  return fixture;
}

template <template <class...> class Map, typename V>
void checkCollisionFixture(
    LookupConfig const& config,
    LookupFixture<Map<uint64_t, V, LookupCollisionHash>> const& fixture) {
  size_t calls = 0;
  Map<uint64_t, V, LookupCollisionHash, LookupCountEqual> shadow(
      0, LookupCollisionHash{}, LookupCountEqual{&calls});
  shadow.reserve(128);
  for (size_t i = 0; i < fixture.entries; ++i) {
    shadow.emplace(localityKey(i, true), localityValue<V>(i));
  }
  auto const stats = fixture.map.computeStats();
  auto const shadow_stats = shadow.computeStats();
  if (stats.chunkCount != 16 ||
      stats.chunkOccupancyHisto != shadow_stats.chunkOccupancyHisto ||
      stats.chunkOutboundOverflowHisto !=
          shadow_stats.chunkOutboundOverflowHisto ||
      (overflowPattern(config.pattern)
           ? maxProbe(stats.keyProbeLengthHisto) <= 1
           : maxProbe(stats.keyProbeLengthHisto) != 1)) {
    throw_exception<std::runtime_error>(
        "Locality collision fixture oracle failed");
  }
  auto const no_tag = noTagPattern(config.pattern);
  for (auto key : fixture.queries) {
    calls = 0;
    auto const found = shadow.find(key);
    if (calls != (no_tag ? 0 : fixture.entries) ||
        (found != shadow.end()) !=
            (config.pattern == LookupPattern::overflow_last_hit)) {
      throw_exception<std::runtime_error>("Locality candidate oracle failed");
    }
  }
}

template <typename Map>
FOLLY_NOINLINE uint64_t localityFind(Map const& map, uint64_t key) {
  std::atomic_signal_fence(std::memory_order_seq_cst);
  auto const found = map.find(key);
  return found == map.end() ? 0 : uint64_t{found->second[0]} + 1;
}

FOLLY_NOINLINE uint64_t localityEmpty(uint64_t key) {
  std::atomic_signal_fence(std::memory_order_seq_cst);
  doNotOptimizeAway(key);
  return 1;
}

template <typename Find>
uint64_t localitySweep(std::vector<uint64_t> const& queries, Find const& find) {
  uint64_t sum = 0;
  for (auto key : queries) {
    sum += find(key);
  }
  return sum;
}

template <typename Map, typename Check>
unsigned runLookupLocality(
    LookupConfig const& config, unsigned requested_iters, Check const& check) {
  BenchmarkSuspender suspend;
  auto const llc = bm_llc_size();
  if (config.locality != LookupLocality::hot && llc == 0) {
    throw_exception<std::runtime_error>(
        "Unable to determine LLC for locality run");
  }
  auto fixture = makeLookupFixture<Map>(config, llc);
  check(fixture);
  auto const stats = fixture.map.computeStats();
  auto const query_bytes = fixture.queries.capacity() * sizeof(uint64_t);
  auto const map_bytes = fixture.map.getAllocatedMemorySize() + sizeof(Map);
  auto const fixture_bytes = uint64_t{map_bytes} + query_bytes;
  if (llc > FLAGS_map_locality_max_bytes / 2) {
    throw_exception<std::length_error>(
        "Eviction reserve exceeds locality memory budget");
  }
  auto const eviction_bytes = uint64_t{llc} * 2;
  if (fixture_bytes > FLAGS_map_locality_max_bytes - eviction_bytes ||
      (config.locality == LookupLocality::hot &&
       fixture_bytes > FLAGS_map_locality_hot_max_bytes) ||
      (config.locality == LookupLocality::large &&
       stats.chunkCount < llc * FLAGS_map_locality_large_llc_factor /
               hardware_constructive_interference_size)) {
    throw_exception<std::length_error>(
        "Locality fixture footprint is outside its budget");
  }
  auto const wanted = config.locality == LookupLocality::eviction
      ? FLAGS_map_locality_evict_iters
      : requested_iters;
  auto const passes =
      wanted / fixture.queries.size() + (wanted % fixture.queries.size() != 0);
  if (passes == 0 ||
      passes > std::numeric_limits<unsigned>::max() / fixture.queries.size()) {
    throw_exception<std::length_error>("Invalid locality lookup count");
  }
  auto const operations = passes * fixture.queries.size();
  LOG(INFO) << fmt::format(
      "LookupLocality {} seed={} entries={} llc_bytes={} map_bytes={} query_bytes={} "
      "header_line_bytes={} lookups={} hits={} trace_hash={} reserved_eviction_bytes_per_process={} "
      "paired_fixture_and_eviction_bytes_estimate={}",
      config.name,
      FLAGS_map_locality_seed,
      fixture.entries,
      llc,
      map_bytes,
      query_bytes,
      stats.chunkCount * hardware_constructive_interference_size,
      operations,
      passes * fixture.expected_hits,
      fixture.trace_hash,
      eviction_bytes,
      2 * (fixture_bytes + eviction_bytes));
  makeUnpredictable(fixture.map);
  makeUnpredictable(fixture.queries);
  auto const find = [&](uint64_t key) {
    return localityFind(fixture.map, key);
  };
  doNotOptimizeAway(localitySweep(fixture.queries, find));
  uint64_t sum = 0;
  if (config.locality == LookupLocality::eviction) {
    for (size_t p = 0; p < passes; ++p) {
      for (auto key : fixture.queries) {
        bm_llc_evict(key);
        suspend.dismissing([&] { sum += find(key); });
      }
    }
  } else {
    suspend.dismissing([&] {
      for (size_t p = 0; p < passes; ++p) {
        sum += localitySweep(fixture.queries, find);
      }
    });
  }
  doNotOptimizeAway(sum);
  if (sum != passes * fixture.expected_sum) {
    throw_exception<std::runtime_error>("Timed locality checksum mismatch");
  }
  return static_cast<unsigned>(operations);
}

unsigned runLocalityControl(bool eviction, unsigned requested_iters) {
  BenchmarkSuspender suspend;
  if (bm_llc_size() > FLAGS_map_locality_max_bytes / 2) {
    throw_exception<std::length_error>(
        "Eviction control exceeds locality memory budget");
  }
  auto const operations =
      eviction ? FLAGS_map_locality_evict_iters : requested_iters;
  uint64_t sum = 0;
  if (eviction) {
    for (unsigned i = 0; i < operations; ++i) {
      bm_llc_evict(i);
      suspend.dismissing([&] { sum += localityEmpty(i); });
    }
  } else {
    suspend.dismissing([&] {
      for (unsigned i = 0; i < operations; ++i) {
        sum += localityEmpty(i);
      }
    });
  }
  doNotOptimizeAway(sum);
  return operations;
}

template <template <class...> class Map, size_t Bytes>
void addLocalityMap(std::string_view map_name) {
  using V = std::array<uint8_t, Bytes>;
  using Regular = Map<uint64_t, V, f14::DefaultHasher<uint64_t>>;
  using Collision = Map<uint64_t, V, LookupCollisionHash>;
  for (auto const& [locality, locality_name] : kLookupLocalities) {
    for (auto const& [pattern, pattern_name] : kLookupPatterns) {
      if (locality == LookupLocality::large && collisionPattern(pattern)) {
        continue;
      }
      LookupConfig config{
          fmt::format(
              "Lookup{}_{}_<u64,a[{}]>_{}",
              locality_name,
              map_name,
              Bytes,
              pattern_name),
          locality,
          pattern};
      addBenchmark(__FILE__, config.name, [config](unsigned iters) {
        if (collisionPattern(config.pattern)) {
          return runLookupLocality<Collision>(
              config, iters, [&](auto const& fixture) {
                checkCollisionFixture<Map, V>(config, fixture);
              });
        }
        return runLookupLocality<Regular>(config, iters, [](auto const&) {});
      });
    }
  }
}

void runLookupLocalityBenchmarks() {
  if (FLAGS_map_locality_small_entries < 32 ||
      FLAGS_map_locality_small_entries >
          std::numeric_limits<uint32_t>::max() / 2 ||
      FLAGS_map_locality_evict_iters == 0 ||
      FLAGS_map_locality_evict_iters % 128 != 0 ||
      FLAGS_map_locality_large_llc_factor < 2 ||
      FLAGS_map_locality_large_llc_factor > 16) {
    throw_exception<std::invalid_argument>(
        "Invalid locality benchmark configuration");
  }
  folly::gflags::SetCommandLineOptionWithMode(
      "bm_mode", "best-of", folly::gflags::SET_FLAG_IF_DEFAULT);
  folly::gflags::SetCommandLineOptionWithMode(
      "bm_profile", "true", folly::gflags::SET_FLAG_IF_DEFAULT);
  folly::gflags::SetCommandLineOptionWithMode(
      "bm_profile_iters", "65536", folly::gflags::SET_FLAG_IF_DEFAULT);
  folly::gflags::SetCommandLineOptionWithMode(
      "bm_regex", "^Lookup", folly::gflags::SET_FLAG_IF_DEFAULT);
  std::string mode;
  std::string profile;
  std::string profile_iters;
  folly::gflags::GetCommandLineOption("bm_mode", &mode);
  folly::gflags::GetCommandLineOption("bm_profile", &profile);
  folly::gflags::GetCommandLineOption("bm_profile_iters", &profile_iters);
  auto const count = to<int64_t>(profile_iters);
  if (mode != "best-of" || profile != "true" || count <= 0 ||
      static_cast<uint64_t>(count) > std::numeric_limits<unsigned>::max()) {
    throw_exception<std::invalid_argument>(
        "Locality benchmarks require valid fixed profiling mode");
  }
  LOG(INFO)
      << "Fixed-work profiles; repeat paired process/worker runs for confidence. "
      << "Hot/eviction share fixtures. Large cases cover the full trace. "
      << "Eviction applies LLC pressure, not guaranteed DRAM access. "
      << "Wrapper controls are diagnostic, not subtracted.";
  addLocalityMap<F14ValueMap, 1>("f14val");
  addLocalityMap<F14ValueMap, 17>("f14val");
  addLocalityMap<F14NodeMap, 1>("f14node");
  addLocalityMap<F14NodeMap, 17>("f14node");
  addLocalityMap<F14VectorMap, 1>("f14vec");
  addLocalityMap<F14VectorMap, 17>("f14vec");
  addBenchmark(__FILE__, "LookupHot_wrapper_control", [](unsigned n) {
    return runLocalityControl(false, n);
  });
  addBenchmark(__FILE__, "LookupEviction_wrapper_control", [](unsigned n) {
    return runLocalityControl(true, n);
  });
}

#else

void runLookupLocalityBenchmarks() {
  throw_exception<std::runtime_error>(
      "F14 locality benchmarks are unsupported without vector intrinsics");
}

#endif

} // namespace

// Pointer-chase over a randomly permuted cycle of n cache lines. Not a map
// benchmark: it's a baseline for the raw memory latency of the host running
// this binary, so that the Find/ManyFind results above at large map sizes
// (which are dominated by cache/DRAM latency, not by map logic) can be
// compared across hosts or architectures on equal footing.
namespace {
void pointerChase(size_t iters, size_t n) {
  BenchmarkSuspender braces;
  std::vector<size_t> order(n);
  for (size_t i = 0; i < n; ++i) {
    order[i] = i;
  }
  std::shuffle(order.begin(), order.end(), getRNG());
  struct alignas(64) Line {
    size_t next;
    char pad[56];
  };
  std::vector<Line> lines(n);
  for (size_t i = 0; i < n; ++i) {
    lines[order[i]].next = order[(i + 1) % n];
  }
  size_t cur = 0;
  braces.dismissing([&] {
    while (iters-- > 0) {
      cur = lines[cur].next;
    }
  });
  doNotOptimizeAway(cur);
}
} // namespace

BENCHMARK_NAMED_PARAM(pointerChase, 1K, 1000)
BENCHMARK_NAMED_PARAM(pointerChase, 32K, 32000)
BENCHMARK_NAMED_PARAM(pointerChase, 1M, 1000000)
BENCHMARK_NAMED_PARAM(pointerChase, 16M, 16000000)

// find() on a small map, evicting the LLC before every lookup with
// bm_llc_evict() so each one starts genuinely cold, without needing a huge
// resident table the way the Find/ManyFind benchmarks above do. The value
// type is sized so kChunkStride isn't a power of two, matching the common
// case for real map value types; useful for evaluating changes to F14's
// per-chunk prefetching. Run with `--bm_profile --bm_profile_iters=<N>`:
// bm_llc_evict() dominates wall-clock time enough that the default
// adaptive/best-of calibration misreports the timed (dismissed) portion as
// zero, even though it's measured correctly.
namespace {
void findColdCache(size_t iters, size_t n) {
  using V = std::array<uint8_t, 17>;
  BenchmarkSuspender braces;
  prepare<uint64_t>(n);
  F14ValueMap<uint64_t, V> m(n);
  for (size_t i = 0; i < n; ++i) {
    m.emplace(key<uint64_t>(static_cast<int>(i)), V{});
  }
  std::uniform_int_distribution<int> dist(0, static_cast<int>(n) - 1);
  int x = 0;
  while (iters-- > 0) {
    auto const& k = key<uint64_t>(dist(getRNG()));
    bm_llc_evict(iters);
    braces.dismissing([&] {
      auto it = m.find(k);
      if (it != m.end()) {
        x ^= it->second[0];
      }
    });
  }
  doNotOptimizeAway(x);
}
} // namespace

BENCHMARK_NAMED_PARAM(findColdCache, 1K, 1000)

// Inserting the entries of a map, in iteration order, into a map with the same
// chunk count makes consecutive insertions land in the same chunk, so each one
// loads the tags just stored by the previous one. This is slow unless the store
// can be forwarded to the load.
namespace {
void insertFromMap(size_t iters, size_t n, bool shuffle) {
  BenchmarkSuspender braces;
  prepare<uint64_t>(n);
  F14ValueMap<uint64_t, uint64_t> src(n);
  for (size_t i = 0; i < n; ++i) {
    src.emplace(key<uint64_t>(static_cast<int>(i)), i);
  }
  std::vector<std::pair<uint64_t, uint64_t>> entries(src.begin(), src.end());
  if (shuffle) {
    std::shuffle(entries.begin(), entries.end(), getRNG());
  }
  while (iters-- > 0) {
    F14ValueMap<uint64_t, uint64_t> m(n);
    braces.dismissing([&] {
      for (auto const& kv : entries) {
        m.insert(kv);
      }
    });
  }
}
} // namespace

BENCHMARK_NAMED_PARAM(insertFromMap, 1K_shuffled, 1000, true)
BENCHMARK_RELATIVE_NAMED_PARAM(insertFromMap, 1K_iteration_order, 1000, false)

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  folly::gflags::SetCommandLineOptionWithMode(
      "bm_max_iters", "100000", folly::gflags::SET_FLAG_IF_DEFAULT);
  folly::gflags::SetCommandLineOptionWithMode(
      "bm_min_iters", "10000", folly::gflags::SET_FLAG_IF_DEFAULT);
  folly::gflags::SetCommandLineOptionWithMode(
      "bm_max_secs", "1", folly::gflags::SET_FLAG_IF_DEFAULT);
  if (FLAGS_map_locality) {
    try {
      runLookupLocalityBenchmarks();
      runBenchmarks();
      return 0;
    } catch (std::exception const& error) {
      LOG(ERROR) << error.what();
      return 1;
    }
  }
  LOG(INFO) << "Preparing benchmark...";
  runAllHashMapTests();
  LOG(INFO) << "Running benchmark, which could take tens of minutes...";
  runBenchmarks();
  return 0;
}
