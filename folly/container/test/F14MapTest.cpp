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

#include <folly/container/F14Map.h>

#include <algorithm>
#include <list>
#include <random>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>

#include <glog/logging.h>

#include <folly/Benchmark.h>
#include <folly/Conv.h>
#include <folly/FBString.h>
#include <folly/Portability.h>
#include <folly/container/test/F14TestUtil.h>
#include <folly/container/test/TrackingTypes.h>
#include <folly/hash/Hash.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <folly/test/TestUtils.h>

using namespace folly;
using namespace folly::f14;
using namespace folly::string_piece_literals;
using namespace folly::test;

template <typename A, typename B>
using detect_op_eq = decltype(FOLLY_DECLVAL(A) == FOLLY_DECLVAL(B));

static constexpr bool kFallback = folly::f14::detail::getF14IntrinsicsMode() ==
    folly::f14::detail::F14IntrinsicsMode::None;

template <typename T>
void runSanityChecks(T const& t) {
  (void)t;
#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
  F14TableStats::compute(t);
#endif
}

template <
    template <typename, typename, typename, typename, typename> class TMap>
void testCustomSwap() {
  using std::swap;

  TMap<
      int,
      int,
      DefaultHasher<int>,
      DefaultKeyEqual<int>,
      SwapTrackingAlloc<std::pair<int const, int>>>
      m0, m1;
  resetTracking();
  swap(m0, m1);

  EXPECT_EQ(0, Tracked<0>::counts().dist(Counts{0, 0, 0, 0}));
}

TEST(F14Map, customSwap) {
  testCustomSwap<F14ValueMap>();
  testCustomSwap<F14NodeMap>();
  testCustomSwap<F14VectorMap>();
  testCustomSwap<F14FastMap>();
}

template <
    template <typename, typename, typename, typename, typename> class TMap,
    typename K,
    typename V>
void runAllocatedMemorySizeTest() {
  using A = SwapTrackingAlloc<std::pair<const K, V>>;

  resetTracking();
  {
    TMap<K, V, DefaultHasher<K>, DefaultKeyEqual<K>, A> m;

    // if F14 intrinsics are not available then we fall back to using
    // std::unordered_map underneath, but in that case the allocation
    // info is only best effort
    if (!kFallback) {
      EXPECT_EQ(testAllocatedMemorySize(), 0);
      EXPECT_EQ(m.getAllocatedMemorySize(), 0);
    }
    auto emptyMapAllocatedMemorySize = testAllocatedMemorySize();
    auto emptyMapAllocatedBlockCount = testAllocatedBlockCount();

    for (size_t i = 0; i < 1000; ++i) {
      m.insert(std::make_pair(to<K>(i), V{}));
      m.erase(to<K>(i / 10 + 2));
      if (!kFallback) {
        EXPECT_EQ(testAllocatedMemorySize(), m.getAllocatedMemorySize());
      }
      EXPECT_GE(m.getAllocatedMemorySize(), sizeof(std::pair<K, V>) * m.size());
      std::size_t size = 0;
      std::size_t count = 0;
      m.visitAllocationClasses([&](std::size_t, std::size_t) mutable {});
      m.visitAllocationClasses([&](std::size_t bytes, std::size_t n) {
        size += bytes * n;
        count += n;
      });
      if (!kFallback) {
        EXPECT_EQ(testAllocatedMemorySize(), size);
        EXPECT_EQ(testAllocatedBlockCount(), count);
      }
    }

    m = decltype(m){};
    EXPECT_EQ(testAllocatedMemorySize(), emptyMapAllocatedMemorySize);
    EXPECT_EQ(testAllocatedBlockCount(), emptyMapAllocatedBlockCount);

    m.reserve(5);
    EXPECT_GT(testAllocatedMemorySize(), 0);
    m = {};
    if (!kFallback) {
      EXPECT_GT(testAllocatedMemorySize(), 0);
    }
  }
  EXPECT_EQ(testAllocatedMemorySize(), 0);
  EXPECT_EQ(testAllocatedBlockCount(), 0);
}

template <typename K, typename V>
void runAllocatedMemorySizeTests() {
  runAllocatedMemorySizeTest<F14ValueMap, K, V>();
  runAllocatedMemorySizeTest<F14NodeMap, K, V>();
  runAllocatedMemorySizeTest<F14VectorMap, K, V>();
  runAllocatedMemorySizeTest<F14FastMap, K, V>();
}

TEST(F14Map, getAllocatedMemorySize) {
  runAllocatedMemorySizeTests<bool, bool>();
  runAllocatedMemorySizeTests<int, int>();
  runAllocatedMemorySizeTests<bool, std::string>();
  runAllocatedMemorySizeTests<double, std::string>();
  runAllocatedMemorySizeTests<std::string, int>();
  runAllocatedMemorySizeTests<std::string, std::string>();
  runAllocatedMemorySizeTests<fbstring, long>();
}

template <typename M>
void runVisitContiguousRangesTest(int n) {
  M map;

  for (int i = 0; i < n; ++i) {
    makeUnpredictable(i);
    map[i] = i;
    map.erase(i / 2);
  }

  std::unordered_map<uintptr_t, bool> visited;
  for (auto& entry : map) {
    visited[reinterpret_cast<uintptr_t>(&entry)] = false;
  }

  map.visitContiguousRanges([&](auto b, auto e) {
    for (auto i = b; i != e; ++i) {
      auto iter = visited.find(reinterpret_cast<uintptr_t>(i));
      ASSERT_TRUE(iter != visited.end());
      EXPECT_FALSE(iter->second);
      iter->second = true;
    }
  });

  // ensure no entries were skipped
  for (auto& e : visited) {
    EXPECT_TRUE(e.second);
  }
}

template <typename M>
void runVisitContiguousRangesTest() {
  runVisitContiguousRangesTest<M>(0); // empty
  runVisitContiguousRangesTest<M>(5); // single chunk
  runVisitContiguousRangesTest<M>(1000); // many chunks
}

TEST(F14ValueMap, visitContiguousRanges) {
  runVisitContiguousRangesTest<F14ValueMap<int, int>>();
}

TEST(F14NodeMap, visitContiguousRanges) {
  runVisitContiguousRangesTest<F14NodeMap<int, int>>();
}

TEST(F14VectorMap, visitContiguousRanges) {
  runVisitContiguousRangesTest<F14VectorMap<int, int>>();
}

TEST(F14FastMap, visitContiguousRanges) {
  runVisitContiguousRangesTest<F14FastMap<int, int>>();
}

#if FOLLY_HAS_MEMORY_RESOURCE
TEST(F14Map, pmrEmpty) {
  pmr::F14ValueMap<int, int> m1;
  pmr::F14NodeMap<int, int> m2;
  pmr::F14VectorMap<int, int> m3;
  pmr::F14FastMap<int, int> m4;
  EXPECT_TRUE(m1.empty() && m2.empty() && m3.empty() && m4.empty());
}
#endif

namespace {
struct NestedHash {
  template <typename N>
  std::size_t operator()(N const& v) const;
};

template <template <class...> class TMap>
struct Nested {
  std::unique_ptr<TMap<Nested, int, NestedHash>> map_;

  explicit Nested(int depth)
      : map_(std::make_unique<TMap<Nested, int, NestedHash>>()) {
    if (depth > 0) {
      map_->emplace(Nested{depth - 1}, 0);
    }
  }
};

template <typename N>
std::size_t NestedHash::operator()(N const& v) const {
  std::size_t rv = 0;
  for (auto& kv : *v.map_) {
    rv += Hash{}(operator()(kv.first), kv.second);
  }
  return Hash{}(rv);
}

template <template <class...> class TMap>
bool operator==(Nested<TMap> const& lhs, Nested<TMap> const& rhs) {
  return *lhs.map_ == *rhs.map_;
}

template <template <class...> class TMap>
bool operator!=(Nested<TMap> const& lhs, Nested<TMap> const& rhs) {
  return !(lhs == rhs);
}

template <template <class...> class TMap>
void testNestedMapEquality() {
  auto n1 = Nested<TMap>(100);
  auto n2 = Nested<TMap>(100);
  auto n3 = Nested<TMap>(99);
  EXPECT_TRUE(n1 == n1);
  EXPECT_TRUE(n1 == n2);
  EXPECT_FALSE(n1 == n3);
  EXPECT_FALSE(n1 != n1);
  EXPECT_FALSE(n1 != n2);
  EXPECT_TRUE(n1 != n3);
}

template <template <class...> class TMap>
void testEqualityRefinement() {
  TMap<std::pair<int, int>, int, HashFirst, EqualFirst> m1;
  TMap<std::pair<int, int>, int, HashFirst, EqualFirst> m2;
  m1[std::make_pair(0, 0)] = 0;
  m1[std::make_pair(1, 1)] = 1;
  EXPECT_FALSE(m1.insert(std::make_pair(std::make_pair(0, 2), 0)).second);
  EXPECT_EQ(m1.size(), 2);
  EXPECT_EQ(m1.count(std::make_pair(0, 10)), 1);
  for (auto& kv : m1) {
    m2.emplace(std::make_pair(kv.first.first, kv.first.second + 1), kv.second);
  }
  EXPECT_EQ(m1.size(), m2.size());
  for (auto& kv : m1) {
    EXPECT_EQ(m2.count(kv.first), 1);
  }
  EXPECT_FALSE(m1 == m2);
  EXPECT_TRUE(m1 != m2);
}
} // namespace

TEST(F14Map, nestedMapEquality) {
  testNestedMapEquality<F14ValueMap>();
  testNestedMapEquality<F14NodeMap>();
  testNestedMapEquality<F14VectorMap>();
  testNestedMapEquality<F14FastMap>();
}

TEST(F14Map, equalityRefinement) {
  testEqualityRefinement<F14ValueMap>();
  testEqualityRefinement<F14NodeMap>();
  testEqualityRefinement<F14VectorMap>();
  testEqualityRefinement<F14FastMap>();
}

namespace {
std::string s(char const* p) {
  return p;
}
} // namespace

template <typename T>
void runSimple() {
  T h;

  EXPECT_EQ(h.size(), 0);
  h.reserve(0);
  std::vector<std::pair<std::string const, std::string>> v(
      {{"abc", "first"}, {"abc", "second"}});
  h.insert(v.begin(), v.begin());
  EXPECT_EQ(h.size(), 0);
  if (!kFallback) {
    EXPECT_EQ(h.bucket_count(), 0);
  }
  h.insert(v.begin(), v.end());
  EXPECT_EQ(h.size(), 1);
  EXPECT_EQ(h["abc"], s("first"));
  h = T{};
  if (!kFallback) {
    EXPECT_EQ(h.bucket_count(), 0);
  }

  h.insert(std::make_pair(s("abc"), s("ABC")));
  EXPECT_TRUE(h.find(s("def")) == h.end());
  EXPECT_FALSE(h.find(s("abc")) == h.end());
  EXPECT_EQ(h[s("abc")], s("ABC"));
  h[s("ghi")] = s("GHI");
  EXPECT_EQ(h.size(), 2);
  h.erase(h.find(s("abc")));
  EXPECT_EQ(h.size(), 1);

  T h2(std::move(h));
  EXPECT_EQ(h.size(), 0);
  EXPECT_TRUE(h.begin() == h.end());
  EXPECT_EQ(h2.size(), 1);

  EXPECT_TRUE(h2.find(s("abc")) == h2.end());
  EXPECT_EQ(h2.begin()->first, s("ghi"));
  {
    auto i = h2.begin();
    EXPECT_FALSE(i == h2.end());
    ++i;
    EXPECT_TRUE(i == h2.end());
  }

  T h3;
  h3.try_emplace(s("xxx"));
  h3.insert_or_assign(s("yyy"), s("YYY"));
  h3 = std::move(h2);
  EXPECT_EQ(h2.size(), 0);
  EXPECT_EQ(h3.size(), 1);
  EXPECT_TRUE(h3.find(s("xxx")) == h3.end());

  for (uint64_t i = 0; i < 1000; ++i) {
    h[to<std::string>(i * i * i)] = s("x");
    EXPECT_EQ(h.size(), i + 1);
  }
  {
    using std::swap;
    swap(h, h2);
  }
  for (uint64_t i = 0; i < 1000; ++i) {
    EXPECT_TRUE(h2.find(to<std::string>(i * i * i)) != h2.end());
    EXPECT_EQ(
        h2.find(to<std::string>(i * i * i))->first, to<std::string>(i * i * i));
    EXPECT_TRUE(h2.find(to<std::string>(i * i * i + 2)) == h2.end());
  }

  T h4{h2};
  EXPECT_EQ(h2.size(), 1000);
  EXPECT_EQ(h4.size(), 1000);

  T h5{std::move(h2)};
  T h6;
  h6 = h4;
  T h7 = h4;
  T h8({{s("abc"), s("ABC")}, {s("def"), s("DEF")}});
  T h9({{s("abc"), s("ABD")}, {s("def"), s("DEF")}});
  EXPECT_EQ(h8.size(), 2);
  EXPECT_EQ(h8.count(s("abc")), 1);
  EXPECT_EQ(h8.count(s("xyz")), 0);
  EXPECT_TRUE(h8.contains(s("abc")));
  EXPECT_FALSE(h8.contains(s("xyz")));

  EXPECT_TRUE(h7 != h8);
  EXPECT_TRUE(h8 != h9);

  h8 = std::move(h7);
  // h2 and h7 are moved from, h4, h5, h6, and h8 should be identical

  EXPECT_TRUE(h4 == h8);

  EXPECT_TRUE(h2.empty());
  EXPECT_TRUE(h7.empty());
  for (uint64_t i = 0; i < 1000; ++i) {
    auto k = to<std::string>(i * i * i);
    EXPECT_EQ(h4.count(k), 1);
    EXPECT_EQ(h5.count(k), 1);
    EXPECT_EQ(h6.count(k), 1);
    EXPECT_EQ(h8.count(k), 1);
    EXPECT_TRUE(h4.contains(k));
    EXPECT_TRUE(h5.contains(k));
    EXPECT_TRUE(h6.contains(k));
    EXPECT_TRUE(h8.contains(k));
  }

  EXPECT_TRUE(h2 == h7);
  EXPECT_TRUE(h4 != h7);

  EXPECT_EQ(h3.at(s("ghi")), s("GHI"));
  EXPECT_THROW(h3.at(s("abc")), std::out_of_range);

  h8.clear();
  h8.emplace(s("abc"), s("ABC"));
  EXPECT_GE(h8.bucket_count(), 1);
  h8 = {};
  if (!kFallback) {
    EXPECT_GE(h8.bucket_count(), 1);
  }
  h9 = {{s("abc"), s("ABD")}, {s("def"), s("DEF")}};
  EXPECT_TRUE(h8.empty());
  EXPECT_EQ(h9.size(), 2);

  auto expectH8 = [&h8](T& ref) { EXPECT_EQ(&ref, &h8); };
  expectH8((h8 = h2));
  expectH8((h8 = std::move(h2)));
  expectH8((h8 = {}));

  runSanityChecks(h);
  runSanityChecks(h2);
  runSanityChecks(h3);
  runSanityChecks(h4);
  runSanityChecks(h5);
  runSanityChecks(h6);
  runSanityChecks(h7);
  runSanityChecks(h8);
  runSanityChecks(h9);
}

template <typename T>
void runEraseWhileIterating() {
  constexpr int kNumElements = 1000;

  // mul and kNumElements should be relatively prime
  for (int mul : {1, 3, 17, 137, kNumElements - 1}) {
    for (int interval : {1, 3, 5, kNumElements / 2}) {
      T h;
      for (auto i = 0; i < kNumElements; ++i) {
        EXPECT_TRUE(h.emplace((i * mul) % kNumElements, i).second);
      }

      int sum = 0;
      for (auto it = h.begin(); it != h.end();) {
        sum += it->second;
        if (it->first % interval == 0) {
          it = h.erase(it);
        } else {
          ++it;
        }
      }
      EXPECT_EQ(kNumElements * (kNumElements - 1) / 2, sum);
    }
  }
}

struct EraseRangeCase {
  int size;
  std::size_t firstPos;
  std::size_t lastPos;
};

template <typename T>
void checkEraseRange(EraseRangeCase c) {
  using Value = std::pair<typename T::key_type, typename T::mapped_type>;
  T h;
  for (int k = 0; k < c.size; ++k) {
    h.emplace(k, k);
  }
  auto first = std::next(h.cbegin(), c.firstPos);
  auto last = std::next(h.cbegin(), c.lastPos);
  std::vector<Value> expectedBefore(h.cbegin(), first);
  std::vector<Value> expectedAfter(last, h.cend());

  auto result = h.erase(first, last);

  EXPECT_EQ(c.firstPos, std::distance(h.begin(), result));
  // Order within each half may change, but elements must not cross between the
  // halves, so that iterating while erasing visits every element exactly once.
  EXPECT_THAT(
      std::vector<Value>(h.begin(), result),
      ::testing::UnorderedElementsAreArray(expectedBefore));
  EXPECT_THAT(
      std::vector<Value>(result, h.end()),
      ::testing::UnorderedElementsAreArray(expectedAfter));
}

template <typename T>
void runEraseRange() {
  constexpr EraseRangeCase kCases[] = {
      {0, 0, 0},
      {5, 0, 0},
      {5, 5, 5},
      {5, 0, 5},
      {5, 0, 2},
      {5, 3, 5},
      {5, 1, 4},
      {5, 2, 3},
      {5, 4, 5},
  };
  for (auto c : kCases) {
    checkEraseRange<T>(c);
  }

  std::mt19937 rng(0);
  for (int i = 0; i < 1000; ++i) {
    int size = std::uniform_int_distribution<int>(0, 100)(rng);
    std::uniform_int_distribution<std::size_t> pos(0, size);
    auto firstPos = pos(rng);
    auto lastPos = pos(rng);
    if (firstPos > lastPos) {
      std::swap(firstPos, lastPos);
    }
    checkEraseRange<T>({size, firstPos, lastPos});
  }
}

template <typename T>
void runRehash() {
  unsigned n = 10000;
  T h;
  auto b = h.bucket_count();
  for (unsigned i = 0; i < n; ++i) {
    h.insert(std::make_pair(to<std::string>(i), s("")));
    if (b != h.bucket_count()) {
      runSanityChecks(h);
      b = h.bucket_count();
    }
  }
  EXPECT_EQ(h.size(), n);
  runSanityChecks(h);
}

// T should be a map from uint64_t to Tracked<1> that uses SwapTrackingAlloc
template <typename T>
void runRandom() {
  using R = std::unordered_map<uint64_t, Tracked<2>>;

  resetTracking();

  std::mt19937_64 gen(0);
  std::uniform_int_distribution<> pctDist(0, 100);
  std::uniform_int_distribution<uint64_t> bitsBitsDist(1, 6);
  {
    T t0;
    T t1;
    R r0;
    R r1;
    std::size_t rollbacks = 0;
    std::size_t resizingSmallRollbacks = 0;
    std::size_t resizingLargeRollbacks = 0;

    for (std::size_t reps = 0; reps < 100000 ||
         (!kFallback &&
          (rollbacks < 10 || resizingSmallRollbacks < 1 ||
           resizingLargeRollbacks < 1));
         ++reps) {
      if (!kFallback && pctDist(gen) < 20) {
        // 10% chance allocator will fail after 0 to 3 more allocations.
        // Skip rollback tests for fallback impl because gcc 4.9's
        // libstdc++ doesn't pass them.
        limitTestAllocations(gen() & 3);
      } else {
        unlimitTestAllocations();
      }
      bool leakCheckOnly = false;

      // discardBits will be from 0 to 62
      auto discardBits = (uint64_t{1} << bitsBitsDist(gen)) - 2;
      auto k = gen() >> discardBits;
      auto v = gen();
      auto pct = pctDist(gen);

      try {
        EXPECT_EQ(t0.empty(), r0.empty());
        EXPECT_EQ(t0.size(), r0.size());
        EXPECT_EQ(t0.size() + t1.size(), Tracked<1>::counts().liveCount());
        EXPECT_EQ(r0.size() + r1.size(), Tracked<2>::counts().liveCount());
        if (pct < 15) {
          // insert
          auto t = t0.insert(std::make_pair(k, v));
          auto r = r0.insert(std::make_pair(k, v));
          EXPECT_EQ(t.first->first, r.first->first);
          EXPECT_EQ(t.first->second.val_, r.first->second.val_);
          EXPECT_EQ(t.second, r.second);
        } else if (pct < 25) {
          // emplace
          auto t = t0.emplace(k, v);
          auto r = r0.emplace(k, v);
          EXPECT_EQ(t.first->first, r.first->first);
          EXPECT_EQ(t.first->second.val_, r.first->second.val_);
          EXPECT_EQ(t.second, r.second);
        } else if (pct < 30) {
          // bulk insert
          leakCheckOnly = true;
          t0.insert(t1.begin(), t1.end());
          r0.insert(r1.begin(), r1.end());
        } else if (pct < 40) {
          // erase by key
          auto t = t0.erase(k);
          auto r = r0.erase(k);
          EXPECT_EQ(t, r);
        } else if (pct < 47) {
          // erase by iterator
          if (t0.size() > 0) {
            auto r = r0.find(k);
            if (r == r0.end()) {
              r = r0.begin();
            }
            k = r->first;
            auto t = t0.find(k);
            t = t0.erase(t);
            if (t != t0.end()) {
              EXPECT_NE(t->first, k);
            }
            r = r0.erase(r);
            if (r != r0.end()) {
              EXPECT_NE(r->first, k);
            }
          }
        } else if (pct < 50) {
          // bulk erase
          if (t0.size() > 0) {
            auto r = r0.find(k);
            if (r == r0.end()) {
              r = r0.begin();
            }
            k = r->first;
            auto t = t0.find(k);
            auto firstt = t;
            auto lastt = ++t;
            t = t0.erase(firstt, lastt);
            if (t != t0.end()) {
              EXPECT_NE(t->first, k);
            }
            auto firstr = r;
            auto lastr = ++r;
            r = r0.erase(firstr, lastr);
            if (r != r0.end()) {
              EXPECT_NE(r->first, k);
            }
          }
        } else if (pct < 58) {
          // find
          auto t = t0.find(k);
          auto r = r0.find(k);
          EXPECT_EQ((t == t0.end()), (r == r0.end()));
          if (t != t0.end() && r != r0.end()) {
            EXPECT_EQ(t->first, r->first);
            EXPECT_EQ(t->second.val_, r->second.val_);
          }
          EXPECT_EQ(t0.count(k), r0.count(k));
          // TODO: When std::unordered_map supports c++20:
          // EXPECT_EQ(t0.contains(k), r0.contains(k));
        } else if (pct < 60) {
          // equal_range
          auto t = t0.equal_range(k);
          auto r = r0.equal_range(k);
          EXPECT_EQ((t.first == t.second), (r.first == r.second));
          if (t.first != t.second && r.first != r.second) {
            EXPECT_EQ(t.first->first, r.first->first);
            EXPECT_EQ(t.first->second.val_, r.first->second.val_);
            t.first++;
            r.first++;
            EXPECT_TRUE(t.first == t.second);
            EXPECT_TRUE(r.first == r.second);
          }
        } else if (pct < 65) {
          // iterate
          uint64_t t = 0;
          for (auto& e : t0) {
            t += e.first * 37 + e.second.val_ + 1000;
          }
          uint64_t r = 0;
          for (auto& e : r0) {
            r += e.first * 37 + e.second.val_ + 1000;
          }
          EXPECT_EQ(t, r);
        } else if (pct < 69) {
          // swap
          using std::swap;
          swap(t0, t1);
          swap(r0, r1);
        } else if (pct < 70) {
          // swap
          t0.swap(t1);
          r0.swap(r1);
        } else if (pct < 72) {
          // default construct
          t0.~T();
          new (&t0) T();
          r0.~R();
          new (&r0) R();
        } else if (pct < 74) {
          // default construct with capacity
          std::size_t capacity = k & 0xffff;
          T t(capacity);
          t0 = std::move(t);
          R r(capacity);
          r0 = std::move(r);
        } else if (pct < 80) {
          // bulk iterator construct
          t0 = T{t1.begin(), t1.end()};
          r0 = R{r1.begin(), r1.end()};
        } else if (pct < 82) {
          // initializer list construct
          auto k2 = gen() >> discardBits;
          auto v2 = gen();
          T t({{k, v}, {k2, v}, {k2, v2}});
          t0 = std::move(t);
          R r({{k, v}, {k2, v}, {k2, v2}});
          r0 = std::move(r);
        } else if (pct < 85) {
          // copy construct
          T t(t1);
          t0 = std::move(t);
          R r(r1);
          r0 = std::move(r);
        } else if (pct < 88) {
          // copy construct
          T t(t1, t1.get_allocator());
          t0 = std::move(t);
          R r(r1, r1.get_allocator());
          r0 = std::move(r);
        } else if (pct < 89) {
          // move construct
          t0.~T();
          new (&t0) T(std::move(t1));
          r0.~R();
          new (&r0) R(std::move(r1));
        } else if (pct < 90) {
          // move construct
          t0.~T();
          auto ta = t1.get_allocator();
          new (&t0) T(std::move(t1), ta);
          r0.~R();
          auto ra = r1.get_allocator();
          new (&r0) R(std::move(r1), ra);
        } else if (pct < 94) {
          // copy assign
          leakCheckOnly = true;
          t0 = t1;
          r0 = r1;
        } else if (pct < 96) {
          // move assign
          t0 = std::move(t1);
          r0 = std::move(r1);
        } else if (pct < 98) {
          // operator==
          EXPECT_EQ((t0 == t1), (r0 == r1));
        } else if (pct < 99) {
          // clear
          runSanityChecks(t0);
          t0.clear();
          r0.clear();
        } else if (pct < 100) {
          // reserve
          auto scale = std::uniform_int_distribution<>(0, 8)(gen);
          auto delta = std::uniform_int_distribution<>(-2, 2)(gen);
          std::ptrdiff_t target = (t0.size() * scale) / 4 + delta;
          if (target >= 0) {
            t0.reserve(static_cast<std::size_t>(target));
            r0.reserve(static_cast<std::size_t>(target));
          }
        }
      } catch (std::bad_alloc const&) {
        ++rollbacks;

        runSanityChecks(t0);

        if (leakCheckOnly) {
          unlimitTestAllocations();
          t0.clear();
          for (auto&& kv : r0) {
            t0[kv.first] = kv.second.val_;
          }
        }

        if (t0.bucket_count() == t0.size() && t0.size() > 0) {
          if (t0.size() < 10) {
            ++resizingSmallRollbacks;
          } else {
            ++resizingLargeRollbacks;
          }
        }

        assert(t0.size() == r0.size());
        for (auto&& kv : r0) {
          auto t = t0.find(kv.first);
          EXPECT_TRUE(
              t != t0.end() && t->first == kv.first &&
              t->second.val_ == kv.second.val_);
        }
      }
    }
  }

  EXPECT_EQ(testAllocatedMemorySize(), 0);
}

TEST(F14ValueMap, simple) {
  runSimple<F14ValueMap<std::string, std::string>>();
}

TEST(F14NodeMap, simple) {
  runSimple<F14NodeMap<std::string, std::string>>();
}

TEST(F14VectorMap, simple) {
  runSimple<F14VectorMap<std::string, std::string>>();
}

TEST(F14FastMap, simple) {
  runSimple<F14FastMap<std::string, std::string>>();
}

#if FOLLY_HAS_MEMORY_RESOURCE
TEST(F14ValueMap, pmrSimple) {
  runSimple<pmr::F14ValueMap<std::string, std::string>>();
}

TEST(F14NodeMap, pmrSimple) {
  runSimple<pmr::F14NodeMap<std::string, std::string>>();
}

TEST(F14VectorMap, pmrSimple) {
  runSimple<pmr::F14VectorMap<std::string, std::string>>();
}

TEST(F14FastMap, pmrSimple) {
  runSimple<pmr::F14FastMap<std::string, std::string>>();
}
#endif

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
TEST(F14VectorMap, reverseIterator) {
  using TMap = F14VectorMap<uint64_t, uint64_t>;
  auto populate = [](TMap& h, uint64_t lo, uint64_t hi) {
    for (auto i = lo; i < hi; ++i) {
      h.emplace(i, i);
    }
  };
  auto verify = [](TMap const& h, uint64_t lo, uint64_t hi) {
    EXPECT_NE(h.end(), h.find(lo));
    uint64_t val = lo;
    for (auto rit = h.rbegin() + lo; rit != h.rend(); ++rit) {
      EXPECT_EQ(val, rit->first);
      EXPECT_EQ(val, rit->second);
      val++;
    }
    EXPECT_EQ(hi, val);
  };
  TMap h;
  size_t prevSize = 0;
  size_t newSize = 1;
  // verify iteration order across rehashes, copies, and moves
  while (newSize < 10'000) {
    populate(h, prevSize, newSize);
    verify(h, 0, newSize);
    verify(h, newSize / 2, newSize);

    TMap h2{h};
    verify(h2, 0, newSize);

    h = std::move(h2);
    verify(h, 0, newSize);
    prevSize = newSize;
    newSize *= 10;
  }
}

TEST(F14VectorMap, OrderPreservingReinsertionView) {
  F14VectorMap<int, int> m1;
  for (size_t i = 0; i < 5; ++i) {
    m1.emplace(i, i);
  }

  F14VectorMap<int, int> m2;
  for (const auto& kv : order_preserving_reinsertion_view(m1)) {
    m2.insert(kv);
  }

  EXPECT_EQ(asVector(m1), asVector(m2));
}

template <typename T>
inline constexpr bool has_data_v = requires(T& v) { v.data(); };

TEST(F14VectorMap, Data) {
  using TMap = F14VectorMap<int, int>;
  TMap m;
  for (int i = 0; i < 5; ++i) {
    m.emplace(i, 10 * i);
  }

  // Storage order matches [rbegin, rend), the raw values_ range.
  auto* p = m.data();
  static_assert(std::is_same_v<decltype(p), TMap::value_type*>);
  EXPECT_EQ(p, m.rbegin());
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(i, p[i].first);
    EXPECT_EQ(10 * i, p[i].second);
  }

  // Mapped values are mutable through the pointer.
  p[2].second = 42;
  EXPECT_EQ(42, m.find(2)->second);

  // Const overload.
  TMap const& cm = m;
  auto* cp = cm.data();
  static_assert(std::is_same_v<decltype(cp), TMap::value_type const*>);
  EXPECT_EQ(cp, p);

  // Empty.
  TMap e;
  EXPECT_EQ(e.data(), e.rbegin());

  static_assert(!has_data_v<F14ValueMap<int, int>>);
  static_assert(!has_data_v<F14NodeMap<int, int>>);
  static_assert(!has_data_v<F14FastMap<int, int>>);
  static_assert(!has_data_v<F14FastMap<int, std::string>>);
  static_assert(has_data_v<pmr::F14VectorMap<int, int>>);
}

template <typename T>
inline constexpr bool has_as_span_v = requires(T& v) { v.as_span(); };

TEST(F14VectorMap, AsSpan) {
  using TMap = F14VectorMap<int, int>;
  TMap m;
  for (int i = 0; i < 5; ++i) {
    m.emplace(i, 10 * i);
  }

  // as_span() is data() and size() bundled into a span.
  auto s = m.as_span();
  static_assert(std::is_same_v<decltype(s), std::span<TMap::value_type>>);
  EXPECT_EQ(s.data(), m.data());
  EXPECT_EQ(s.size(), m.size());

  // Const overload.
  TMap const& cm = m;
  auto cs = cm.as_span();
  static_assert(
      std::is_same_v<decltype(cs), std::span<TMap::value_type const>>);
  EXPECT_EQ(cs.data(), cm.data());
  EXPECT_EQ(cs.size(), cm.size());

  // Empty.
  TMap e;
  EXPECT_EQ(e.as_span().data(), e.data());
  EXPECT_EQ(e.as_span().size(), e.size());

  // Only the vector map exposes storage; value, node, and fast maps do not,
  // including fast maps that happen to be vector-backed (large value type).
  static_assert(!has_as_span_v<F14ValueMap<int, int>>);
  static_assert(!has_as_span_v<F14NodeMap<int, int>>);
  static_assert(!has_as_span_v<F14FastMap<int, int>>);
  static_assert(!has_as_span_v<F14FastMap<int, std::string>>);
  static_assert(has_as_span_v<pmr::F14VectorMap<int, int>>);
}
#endif

TEST(F14ValueMap, eraseWhileIterating) {
  runEraseWhileIterating<F14ValueMap<int, int>>();
}

TEST(F14NodeMap, eraseWhileIterating) {
  runEraseWhileIterating<F14NodeMap<int, int>>();
}

TEST(F14VectorMap, eraseWhileIterating) {
  runEraseWhileIterating<F14VectorMap<int, int>>();
}

TEST(F14FastMap, eraseWhileIterating) {
  runEraseWhileIterating<F14FastMap<int, int>>();
}

TEST(F14ValueMap, eraseRange) {
  runEraseRange<F14ValueMap<int, int>>();
}

TEST(F14NodeMap, eraseRange) {
  runEraseRange<F14NodeMap<int, int>>();
}

TEST(F14VectorMap, eraseRange) {
  runEraseRange<F14VectorMap<int, int>>();
}

TEST(F14FastMap, eraseRange) {
  runEraseRange<F14FastMap<int, int>>();
}

// Validates the erase range test facility against the standard container.
TEST(StdUnorderedMap, eraseRange) {
  runEraseRange<std::unordered_map<int, int>>();
}

TEST(F14ValueMap, rehash) {
  runRehash<F14ValueMap<std::string, std::string>>();
}

TEST(F14NodeMap, rehash) {
  runRehash<F14NodeMap<std::string, std::string>>();
}

TEST(F14VectorMap, rehash) {
  runRehash<F14VectorMap<std::string, std::string>>();
}

TEST(F14FastMap, rehash) {
  runRehash<F14VectorMap<std::string, std::string>>();
}

template <typename T>
void runPrehash() {
  T h;

  EXPECT_EQ(h.size(), 0);

  h.insert(std::make_pair(s("abc"), s("ABC")));
  EXPECT_TRUE(h.find(s("def")) == h.end());
  EXPECT_FALSE(h.find(s("abc")) == h.end());

  auto t1 = h.prehash(s("def"));
  F14HashToken t2;
  t2 = h.prehash(s("abc"));
  h.prefetch(t2);
  EXPECT_TRUE(h.find(t1, s("def")) == h.end());
  EXPECT_FALSE(h.find(t2, s("abc")) == h.end());
  h.prefetch(t1);

  {
    auto const key3 = s("def");
    auto hv3 = h.hash_function()(key3);
    auto t3 = h.prehash(key3, hv3);
    EXPECT_EQ(t3, h.prehash(key3));
    EXPECT_TRUE(h.find(t3, key3) == h.end());
  }
  {
    auto const key3 = s("abc");
    auto hv3 = h.hash_function()(key3);
    auto t3 = h.prehash(key3, hv3);
    EXPECT_EQ(t3, h.prehash(key3));
    EXPECT_FALSE(h.find(t3, key3) == h.end());
  }
}
TEST(F14ValueMap, prehash) {
  runPrehash<F14ValueMap<std::string, std::string>>();
}

TEST(F14NodeMap, prehash) {
  runPrehash<F14NodeMap<std::string, std::string>>();
}

TEST(F14VectorMap, prehash) {
  runPrehash<F14VectorMap<std::string, std::string>>();
}

TEST(F14FastMap, prehash) {
  runPrehash<F14FastMap<std::string, std::string>>();
}

TEST(F14ValueMap, random) {
  runRandom<F14ValueMap<
      uint64_t,
      Tracked<1>,
      std::hash<uint64_t>,
      std::equal_to<uint64_t>,
      SwapTrackingAlloc<std::pair<uint64_t const, Tracked<1>>>>>();
}

TEST(F14NodeMap, random) {
  runRandom<F14NodeMap<
      uint64_t,
      Tracked<1>,
      std::hash<uint64_t>,
      std::equal_to<uint64_t>,
      SwapTrackingAlloc<std::pair<uint64_t const, Tracked<1>>>>>();
}

TEST(F14VectorMap, random) {
  runRandom<F14VectorMap<
      uint64_t,
      Tracked<1>,
      std::hash<uint64_t>,
      std::equal_to<uint64_t>,
      SwapTrackingAlloc<std::pair<uint64_t const, Tracked<1>>>>>();
}

TEST(F14FastMap, random) {
  runRandom<F14FastMap<
      uint64_t,
      Tracked<1>,
      std::hash<uint64_t>,
      std::equal_to<uint64_t>,
      SwapTrackingAlloc<std::pair<uint64_t const, Tracked<1>>>>>();
}

TEST(F14ValueMap, growStats) {
  F14ValueMap<uint64_t, uint64_t> h;
  for (unsigned i = 1; i <= 3072; ++i) {
    h[i]++;
  }
  // F14ValueMap just before rehash
  runSanityChecks(h);
  h[0]++;
  // F14ValueMap just after rehash
  runSanityChecks(h);
}

TEST(F14ValueMap, steadyStateStats) {
  // 10k keys, 14% probability of insert, 90% chance of erase, so the
  // table should converge to 1400 size without triggering the rehash
  // that would occur at 1536.
  F14ValueMap<uint64_t, uint64_t> h;
  std::mt19937_64 gen(0);
  std::uniform_int_distribution<> dist(0, 10000);
  for (std::size_t i = 0; i < 100000; ++i) {
    auto key = dist(gen);
    if (dist(gen) < 1400) {
      h.insert_or_assign(key, i);
    } else {
      h.erase(key);
    }
    if (((i + 1) % 10000) == 0) {
#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
      auto stats = F14TableStats::compute(h);
      // Verify that average miss probe length is bounded despite continued
      // erase + reuse.  p99 of the average across 10M random steps is 4.69,
      // average is 2.96.
      EXPECT_LT(f14::expectedProbe(stats.missProbeLengthHisto), 10.0);
#endif
    }
  }
  // F14ValueMap at steady state
  runSanityChecks(h);
}

TEST(F14VectorMap, steadyStateStats) {
  // 10k keys, 14% probability of insert, 90% chance of erase, so the
  // table should converge to 1400 size without triggering the rehash
  // that would occur at 1536.
  F14VectorMap<std::string, uint64_t> h;
  std::mt19937_64 gen(0);
  std::uniform_int_distribution<> dist(0, 10000);
  for (std::size_t i = 0; i < 100000; ++i) {
    auto key = "0123456789ABCDEFGHIJKLMNOPQ" + to<std::string>(dist(gen));
    if (dist(gen) < 1400) {
      h.insert_or_assign(key, i);
    } else {
      h.erase(key);
    }
    if (((i + 1) % 10000) == 0) {
#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
      auto stats = F14TableStats::compute(h);
      // Verify that average miss probe length is bounded despite continued
      // erase + reuse.  p99 of the average across 10M random steps is 4.69,
      // average is 2.96.
      EXPECT_LT(f14::expectedProbe(stats.missProbeLengthHisto), 10.0);
#endif
    }
  }
  // F14ValueMap at steady state
  runSanityChecks(h);
}

TEST(Tracked, baseline) {
  Tracked<0> a0;

  {
    resetTracking();
    Tracked<0> b0{a0};
    EXPECT_EQ(a0.val_, b0.val_);
    EXPECT_EQ(sumCounts(), (Counts{1, 0, 0, 0}));
    EXPECT_EQ(Tracked<0>::counts(), (Counts{1, 0, 0, 0}));
  }
  {
    resetTracking();
    Tracked<0> b0{std::move(a0)};
    EXPECT_EQ(a0.val_, b0.val_);
    EXPECT_EQ(sumCounts(), (Counts{0, 1, 0, 0}));
    EXPECT_EQ(Tracked<0>::counts(), (Counts{0, 1, 0, 0}));
  }
  {
    resetTracking();
    Tracked<1> b1{a0};
    EXPECT_EQ(a0.val_, b1.val_);
    EXPECT_EQ(sumCounts(), (Counts{0, 0, 1, 0}));
    EXPECT_EQ(Tracked<1>::counts(), (Counts{0, 0, 1, 0}));
  }
  {
    resetTracking();
    Tracked<1> b1{std::move(a0)};
    EXPECT_EQ(a0.val_, b1.val_);
    EXPECT_EQ(sumCounts(), (Counts{0, 0, 0, 1}));
    EXPECT_EQ(Tracked<1>::counts(), (Counts{0, 0, 0, 1}));
  }
  {
    Tracked<0> b0;
    resetTracking();
    b0 = a0;
    EXPECT_EQ(a0.val_, b0.val_);
    EXPECT_EQ(sumCounts(), (Counts{0, 0, 0, 0, 1, 0}));
    EXPECT_EQ(Tracked<0>::counts(), (Counts{0, 0, 0, 0, 1, 0}));
  }
  {
    Tracked<0> b0;
    resetTracking();
    b0 = std::move(a0);
    EXPECT_EQ(a0.val_, b0.val_);
    EXPECT_EQ(sumCounts(), (Counts{0, 0, 0, 0, 0, 1}));
    EXPECT_EQ(Tracked<0>::counts(), (Counts{0, 0, 0, 0, 0, 1}));
  }
  {
    Tracked<1> b1;
    resetTracking();
    b1 = a0;
    EXPECT_EQ(a0.val_, b1.val_);
    EXPECT_EQ(sumCounts(), (Counts{0, 0, 1, 0, 0, 1, 0, 1}));
    EXPECT_EQ(Tracked<1>::counts(), (Counts{0, 0, 1, 0, 0, 1, 0, 1}));
  }
  {
    Tracked<1> b1;
    resetTracking();
    b1 = std::move(a0);
    EXPECT_EQ(a0.val_, b1.val_);
    EXPECT_EQ(sumCounts(), (Counts{0, 0, 0, 1, 0, 1, 0, 1}));
    EXPECT_EQ(Tracked<1>::counts(), (Counts{0, 0, 0, 1, 0, 1, 0, 1}));
  }
}

// M should be a map from Tracked<0> to Tracked<1>.  F should take a map
// and a pair const& or pair&& and cause it to be inserted
template <typename M, typename F>
void runInsertCases(
    std::string const& name, F const& insertFunc, uint64_t expectedDist = 0) {
  static_assert(std::is_same<typename M::key_type, Tracked<0>>::value);
  static_assert(std::is_same<typename M::mapped_type, Tracked<1>>::value);
  {
    typename M::value_type p{0, 0};
    M m;
    resetTracking();
    insertFunc(m, p);
    // fresh key, value_type const& ->
    // copy is expected
    EXPECT_EQ(
        Tracked<0>::counts().dist(Counts{1, 0, 0, 0}) +
            Tracked<1>::counts().dist(Counts{1, 0, 0, 0}),
        expectedDist)
        << name << "\n0 -> " << Tracked<0>::counts() << "\n1 -> "
        << Tracked<1>::counts();
  }
  {
    typename M::value_type p{0, 0};
    M m;
    resetTracking();
    insertFunc(m, std::move(p));
    // fresh key, value_type&& ->
    // key copy is unfortunate but required
    EXPECT_EQ(
        Tracked<0>::counts().dist(Counts{1, 0, 0, 0}) +
            Tracked<1>::counts().dist(Counts{0, 1, 0, 0}),
        expectedDist)
        << name << "\n0 -> " << Tracked<0>::counts() << "\n1 -> "
        << Tracked<1>::counts();
  }
  {
    std::pair<Tracked<0>, Tracked<1>> p{0, 0};
    M m;
    resetTracking();
    insertFunc(m, p);
    // fresh key, pair<key_type,mapped_type> const& ->
    // 1 copy is required
    EXPECT_EQ(
        Tracked<0>::counts().dist(Counts{1, 0, 0, 0}) +
            Tracked<1>::counts().dist(Counts{1, 0, 0, 0}),
        expectedDist)
        << name << "\n0 -> " << Tracked<0>::counts() << "\n1 -> "
        << Tracked<1>::counts();
  }
  {
    std::pair<Tracked<0>, Tracked<1>> p{0, 0};
    M m;
    resetTracking();
    insertFunc(m, std::move(p));
    // fresh key, pair<key_type,mapped_type>&& ->
    // this is the happy path for insert(make_pair(.., ..))
    EXPECT_EQ(
        Tracked<0>::counts().dist(Counts{0, 1, 0, 0}) +
            Tracked<1>::counts().dist(Counts{0, 1, 0, 0}),
        expectedDist)
        << name << "\n0 -> " << Tracked<0>::counts() << "\n1 -> "
        << Tracked<1>::counts();
  }
  {
    std::pair<Tracked<2>, Tracked<3>> p{0, 0};
    M m;
    resetTracking();
    insertFunc(m, p);
    // fresh key, convertible const& ->
    //   key_type ops: Tracked<0>::counts
    //   mapped_type ops: Tracked<1>::counts
    //   key_src ops: Tracked<2>::counts
    //   mapped_src ops: Tracked<3>::counts;

    // There are three strategies that could be optimal for particular
    // ratios of cost:
    //
    // - convert key and value in place to final position, destroy if
    //   insert fails. This is the strategy used by std::unordered_map
    //   and FBHashMap
    //
    // - convert key and default value in place to final position,
    //   convert value only if insert succeeds.  Nobody uses this strategy
    //
    // - convert key to a temporary, move key and convert value if
    //   insert succeeds.  This is the strategy used by F14 and what is
    //   EXPECT_EQ here.

    // The expectedDist * 3 is just a hack for the emplace-pieces-by-value
    // test, whose test harness copies the original pair and then uses
    // move conversion instead of copy conversion.
    EXPECT_EQ(
        Tracked<0>::counts().dist(Counts{0, 1, 1, 0}) +
            Tracked<1>::counts().dist(Counts{0, 0, 1, 0}) +
            Tracked<2>::counts().dist(Counts{0, 0, 0, 0}) +
            Tracked<3>::counts().dist(Counts{0, 0, 0, 0}),
        expectedDist * 3);
  }
  {
    std::pair<Tracked<2>, Tracked<3>> p{0, 0};
    M m;
    resetTracking();
    insertFunc(m, std::move(p));
    // fresh key, convertible&& ->
    //   key_type ops: Tracked<0>::counts
    //   mapped_type ops: Tracked<1>::counts
    //   key_src ops: Tracked<2>::counts
    //   mapped_src ops: Tracked<3>::counts;
    EXPECT_EQ(
        Tracked<0>::counts().dist(Counts{0, 1, 0, 1}) +
            Tracked<1>::counts().dist(Counts{0, 0, 0, 1}) +
            Tracked<2>::counts().dist(Counts{0, 0, 0, 0}) +
            Tracked<3>::counts().dist(Counts{0, 0, 0, 0}),
        expectedDist);
  }
  if (!kFallback) {
    typename M::value_type p{0, 0};
    M m;
    m[0] = 0;
    resetTracking();
    insertFunc(m, p);
    // duplicate key, value_type const&
    EXPECT_EQ(
        Tracked<0>::counts().dist(Counts{0, 0, 0, 0}) +
            Tracked<1>::counts().dist(Counts{0, 0, 0, 0}),
        expectedDist);
  }
  if (!kFallback) {
    typename M::value_type p{0, 0};
    M m;
    m[0] = 0;
    resetTracking();
    insertFunc(m, std::move(p));
    // duplicate key, value_type&&
    EXPECT_EQ(
        Tracked<0>::counts().dist(Counts{0, 0, 0, 0}) +
            Tracked<1>::counts().dist(Counts{0, 0, 0, 0}),
        expectedDist);
  }
  if (!kFallback) {
    std::pair<Tracked<0>, Tracked<1>> p{0, 0};
    M m;
    m[0] = 0;
    resetTracking();
    insertFunc(m, p);
    // duplicate key, pair<key_type,mapped_type> const&
    EXPECT_EQ(
        Tracked<0>::counts().dist(Counts{0, 0, 0, 0}) +
            Tracked<1>::counts().dist(Counts{0, 0, 0, 0}),
        expectedDist);
  }
  if (!kFallback) {
    std::pair<Tracked<0>, Tracked<1>> p{0, 0};
    M m;
    m[0] = 0;
    resetTracking();
    insertFunc(m, std::move(p));
    // duplicate key, pair<key_type,mapped_type>&&
    EXPECT_EQ(
        Tracked<0>::counts().dist(Counts{0, 0, 0, 0}) +
            Tracked<1>::counts().dist(Counts{0, 0, 0, 0}),
        expectedDist);
  }
  if (!kFallback) {
    std::pair<Tracked<2>, Tracked<3>> p{0, 0};
    M m;
    m[0] = 0;
    resetTracking();
    insertFunc(m, p);
    // duplicate key, convertible const& ->
    //   key_type ops: Tracked<0>::counts
    //   mapped_type ops: Tracked<1>::counts
    //   key_src ops: Tracked<2>::counts
    //   mapped_src ops: Tracked<3>::counts;
    EXPECT_EQ(
        Tracked<0>::counts().dist(Counts{0, 0, 1, 0}) +
            Tracked<1>::counts().dist(Counts{0, 0, 0, 0}) +
            Tracked<2>::counts().dist(Counts{0, 0, 0, 0}) +
            Tracked<3>::counts().dist(Counts{0, 0, 0, 0}),
        expectedDist * 2);
  }
  if (!kFallback) {
    std::pair<Tracked<2>, Tracked<3>> p{0, 0};
    M m;
    m[0] = 0;
    resetTracking();
    insertFunc(m, std::move(p));
    // duplicate key, convertible&& ->
    //   key_type ops: Tracked<0>::counts
    //   mapped_type ops: Tracked<1>::counts
    //   key_src ops: Tracked<2>::counts
    //   mapped_src ops: Tracked<3>::counts;
    EXPECT_EQ(
        Tracked<0>::counts().dist(Counts{0, 0, 0, 1}) +
            Tracked<1>::counts().dist(Counts{0, 0, 0, 0}) +
            Tracked<2>::counts().dist(Counts{0, 0, 0, 0}) +
            Tracked<3>::counts().dist(Counts{0, 0, 0, 0}),
        expectedDist);
  }
}

struct DoInsert {
  template <typename M, typename P>
  void operator()(M& m, P&& p) const {
    m.insert(std::forward<P>(p));
  }
};

struct DoEmplace1 {
  template <typename M, typename P>
  void operator()(M& m, P&& p) const {
    m.emplace(std::forward<P>(p));
  }
};

struct DoEmplace2 {
  template <typename M, typename U1, typename U2>
  void operator()(M& m, std::pair<U1, U2> const& p) const {
    m.emplace(p.first, p.second);
  }

  template <typename M, typename U1, typename U2>
  void operator()(M& m, std::pair<U1, U2>&& p) const {
    m.emplace(std::move(p.first), std::move(p.second));
  }
};

struct DoEmplace3 {
  template <typename M, typename U1, typename U2>
  void operator()(M& m, std::pair<U1, U2> const& p) const {
    m.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(p.first),
        std::forward_as_tuple(p.second));
  }

  template <typename M, typename U1, typename U2>
  void operator()(M& m, std::pair<U1, U2>&& p) const {
    m.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(std::move(p.first)),
        std::forward_as_tuple(std::move(p.second)));
  }
};

// Simulates use of piecewise_construct without proper use of
// forward_as_tuple.  This code doesn't yield the normal pattern, but
// it should have exactly 1 additional move or copy of the key and 1
// additional move or copy of the mapped value.
struct DoEmplace3Value {
  template <typename M, typename U1, typename U2>
  void operator()(M& m, std::pair<U1, U2> const& p) const {
    m.emplace(
        std::piecewise_construct,
        std::tuple<U1>{p.first},
        std::tuple<U2>{p.second});
  }

  template <typename M, typename U1, typename U2>
  void operator()(M& m, std::pair<U1, U2>&& p) const {
    m.emplace(
        std::piecewise_construct,
        std::tuple<U1>{std::move(p.first)},
        std::tuple<U2>{std::move(p.second)});
  }
};

template <typename M>
void runInsertAndEmplace(std::string const& name) {
  runInsertCases<M>(name + " insert", DoInsert{});
  runInsertCases<M>(name + " emplace pair", DoEmplace1{});
  runInsertCases<M>(name + " emplace k,v", DoEmplace2{});
  runInsertCases<M>(name + " emplace pieces", DoEmplace3{});
  runInsertCases<M>(name + " emplace pieces by value", DoEmplace3Value{}, 2);

  // Calling the default pair constructor via emplace is valid, but not
  // very useful in real life.  Verify that it works.
  M m;
  typename M::key_type k;
  EXPECT_EQ(m.count(k), 0);
  EXPECT_FALSE(m.contains(k));
  m.emplace();
  EXPECT_EQ(m.count(k), 1);
  EXPECT_TRUE(m.contains(k));
  m.emplace();
  EXPECT_EQ(m.count(k), 1);
  EXPECT_TRUE(m.contains(k));
}

TEST(F14ValueMap, destructuring) {
  runInsertAndEmplace<F14ValueMap<Tracked<0>, Tracked<1>>>("f14value");
}

TEST(F14NodeMap, destructuring) {
  runInsertAndEmplace<F14NodeMap<Tracked<0>, Tracked<1>>>("f14node");
}

TEST(F14VectorMap, destructuring) {
  runInsertAndEmplace<F14VectorMap<Tracked<0>, Tracked<1>>>("f14vector");
}

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

TEST(F14VectorMap, destructuringErase) {
  using M = F14VectorMap<Tracked<0>, Tracked<1>>;
  typename M::value_type p1{0, 0};
  typename M::value_type p2{2, 2};
  M m;
  m.insert(p1);
  m.insert(p2);

  resetTracking();
  m.erase(p1.first);
  LOG(INFO) << "erase -> " << "key_type ops " << Tracked<0>::counts()
            << ", mapped_type ops " << Tracked<1>::counts();
  // deleting p1 will cause p2 to be moved to the front of the values array
  EXPECT_EQ(
      Tracked<0>::counts().dist(Counts{0, 1, 0, 0}) +
          Tracked<1>::counts().dist(Counts{0, 1, 0, 0}),
      0);
}

TEST(F14ValueMap, maxSize) {
  F14ValueMap<int, int> m;
  EXPECT_EQ(
      m.max_size(),
      std::min(
          folly::f14::detail::SizeAndChunkShift::kMaxSize,
          std::allocator_traits<decltype(m)::allocator_type>::max_size(
              m.get_allocator())));
}

TEST(F14NodeMap, maxSize) {
  F14NodeMap<int, int> m;
  EXPECT_EQ(
      m.max_size(),
      std::min(
          folly::f14::detail::SizeAndChunkShift::kMaxSize,
          std::allocator_traits<decltype(m)::allocator_type>::max_size(
              m.get_allocator())));
}

TEST(F14VectorMap, vectorMaxSize) {
  F14VectorMap<int, int> m;
  EXPECT_EQ(
      m.max_size(),
      std::min(
          {folly::f14::detail::SizeAndChunkShift::kMaxSize,
           std::size_t{std::numeric_limits<uint32_t>::max()},
           std::allocator_traits<decltype(m)::allocator_type>::max_size(
               m.get_allocator())}));
}

#endif

template <typename M>
void runMoveOnlyTest() {
  M t0;
  t0[10] = 20;
  t0.emplace(30, 40);
  t0.insert(std::make_pair(50, 60));
  M t1{std::move(t0)};
  EXPECT_TRUE(t0.empty());
  M t2;
  EXPECT_TRUE(t2.empty());
  t2 = std::move(t1);
  EXPECT_EQ(t2.size(), 3);
}

TEST(F14ValueMap, moveOnly) {
  runMoveOnlyTest<F14ValueMap<MoveOnlyTestInt, int>>();
  runMoveOnlyTest<F14ValueMap<int, MoveOnlyTestInt>>();
  runMoveOnlyTest<F14ValueMap<MoveOnlyTestInt, MoveOnlyTestInt>>();
}

TEST(F14NodeMap, moveOnly) {
  runMoveOnlyTest<F14NodeMap<MoveOnlyTestInt, int>>();
  runMoveOnlyTest<F14NodeMap<int, MoveOnlyTestInt>>();
  runMoveOnlyTest<F14NodeMap<MoveOnlyTestInt, MoveOnlyTestInt>>();
}

TEST(F14VectorMap, moveOnly) {
  runMoveOnlyTest<F14VectorMap<MoveOnlyTestInt, int>>();
  runMoveOnlyTest<F14VectorMap<int, MoveOnlyTestInt>>();
  runMoveOnlyTest<F14VectorMap<MoveOnlyTestInt, MoveOnlyTestInt>>();
}

TEST(F14FastMap, moveOnly) {
  runMoveOnlyTest<F14FastMap<MoveOnlyTestInt, int>>();
  runMoveOnlyTest<F14FastMap<int, MoveOnlyTestInt>>();
  runMoveOnlyTest<F14FastMap<MoveOnlyTestInt, MoveOnlyTestInt>>();
}

template <typename M>
void runEraseIntoTest() {
  M t0;
  M t1;

  auto emplaceIntoT0 = [&t0](auto&& key, auto&& value) {
    EXPECT_FALSE(key.destroyed);
    t0.emplace(std::move(key), std::move(value));
  };
  auto emplaceIntoT0Mut =
      [&](typename M::key_type&& key, typename M::mapped_type&& value) mutable {
        emplaceIntoT0(std::move(key), std::move(value));
      };

  t0.emplace(10, 0);
  t1.emplace(20, 0);
  t1.eraseInto(t1.begin(), emplaceIntoT0);
  EXPECT_TRUE(t1.empty());
  EXPECT_EQ(t0.size(), 2);
  EXPECT_TRUE(t0.find(10) != t0.end());
  EXPECT_TRUE(t0.find(20) != t0.end());

  t1.emplace(20, 0);
  t1.emplace(30, 0);
  t1.emplace(40, 0);
  t1.eraseInto(t1.begin(), t1.end(), emplaceIntoT0Mut);
  EXPECT_TRUE(t1.empty());
  EXPECT_EQ(t0.size(), 4);
  EXPECT_TRUE(t0.find(30) != t0.end());
  EXPECT_TRUE(t0.find(40) != t0.end());

  t1.emplace(50, 0);
  size_t erased = t1.eraseInto(t1.find(50)->first, emplaceIntoT0);
  EXPECT_EQ(erased, 1);
  EXPECT_TRUE(t1.empty());
  EXPECT_EQ(t0.size(), 5);
  EXPECT_TRUE(t0.find(50) != t0.end());

  typename M::key_type key{60};
  erased = t1.eraseInto(key, emplaceIntoT0Mut);
  EXPECT_EQ(erased, 0);
  EXPECT_EQ(t0.size(), 5);
}

TEST(F14ValueMap, eraseInto) {
  runEraseIntoTest<F14ValueMap<MoveOnlyTestInt, MoveOnlyTestInt>>();
}

TEST(F14NodeMap, eraseInto) {
  runEraseIntoTest<F14NodeMap<MoveOnlyTestInt, MoveOnlyTestInt>>();
}

TEST(F14VectorMap, eraseInto) {
  runEraseIntoTest<F14VectorMap<MoveOnlyTestInt, MoveOnlyTestInt>>();
}

TEST(F14FastMap, eraseInto) {
  runEraseIntoTest<F14FastMap<MoveOnlyTestInt, MoveOnlyTestInt>>();
}

template <typename M>
void runEraseIntoEmptyFromEraseTest() {
  M m;
  m.emplace(0, 0);
  m.erase(0);

  EXPECT_GT(m.bucket_count(), 0);
  EXPECT_TRUE(m.empty());

  m.eraseInto(m.begin(), m.end(), [](auto&&, auto&&) {});

  EXPECT_GT(m.bucket_count(), 0);
  EXPECT_TRUE(m.empty());
}

TEST(F14ValueMap, eraseIntoEmptyFromErase) {
  runEraseIntoEmptyFromEraseTest<
      F14ValueMap<MoveOnlyTestInt, MoveOnlyTestInt>>();
}

TEST(F14NodeMap, eraseIntoEmptyFromErase) {
  runEraseIntoEmptyFromEraseTest<
      F14NodeMap<MoveOnlyTestInt, MoveOnlyTestInt>>();
}

TEST(F14VectorMap, eraseIntoEmptyFromErase) {
  runEraseIntoEmptyFromEraseTest<
      F14VectorMap<MoveOnlyTestInt, MoveOnlyTestInt>>();
}

TEST(F14FastMap, eraseIntoEmptyFromErase) {
  runEraseIntoEmptyFromEraseTest<
      F14FastMap<MoveOnlyTestInt, MoveOnlyTestInt>>();
}

template <typename M>
void runEraseIntoEmptyFromReserveTest() {
  M m;
  m.reserve(1);

  EXPECT_GT(m.bucket_count(), 0);
  EXPECT_TRUE(m.empty());

  m.eraseInto(m.begin(), m.end(), [](auto&&, auto&&) {});

  EXPECT_GT(m.bucket_count(), 0);
  EXPECT_TRUE(m.empty());
}

TEST(F14ValueMap, eraseIntoEmptyFromReserve) {
  runEraseIntoEmptyFromReserveTest<
      F14ValueMap<MoveOnlyTestInt, MoveOnlyTestInt>>();
}

TEST(F14NodeMap, eraseIntoEmptyFromReserve) {
  runEraseIntoEmptyFromReserveTest<
      F14NodeMap<MoveOnlyTestInt, MoveOnlyTestInt>>();
}

TEST(F14VectorMap, eraseIntoEmptyFromReserve) {
  runEraseIntoEmptyFromReserveTest<
      F14VectorMap<MoveOnlyTestInt, MoveOnlyTestInt>>();
}

TEST(F14FastMap, eraseIntoEmptyFromReserve) {
  runEraseIntoEmptyFromReserveTest<
      F14FastMap<MoveOnlyTestInt, MoveOnlyTestInt>>();
}

template <typename M>
void runPermissiveConstructorTest() {
  M t;
  M const& ct{t};

  for (int i = 10; i <= 100; i += 10) {
    t[i] = i;
  }
  t.erase(10);
  EXPECT_EQ(t.size(), 9);
  t.erase(t.find(20));
  EXPECT_EQ(t.size(), 8);
  t.erase(ct.find(30));
  EXPECT_EQ(t.size(), 7);
  t.eraseInto(40, [](auto&&, auto&&) {});
  EXPECT_EQ(t.size(), 6);
  t.eraseInto(t.find(50), [](auto&&, auto&&) {});
  EXPECT_EQ(t.size(), 5);
  t.eraseInto(ct.find(60), [](auto&&, auto&&) {});
  EXPECT_EQ(t.size(), 4);
}

TEST(F14ValueMap, permissiveConstructor) {
  runPermissiveConstructorTest<F14ValueMap<
      PermissiveConstructorTestInt,
      PermissiveConstructorTestInt>>();
}

TEST(F14NodeMap, permissiveConstructor) {
  runPermissiveConstructorTest<
      F14NodeMap<PermissiveConstructorTestInt, PermissiveConstructorTestInt>>();
}

TEST(F14VectorMap, permissiveConstructor) {
  runPermissiveConstructorTest<F14VectorMap<
      PermissiveConstructorTestInt,
      PermissiveConstructorTestInt>>();
}

TEST(F14FastMap, permissiveConstructor) {
  runPermissiveConstructorTest<
      F14FastMap<PermissiveConstructorTestInt, PermissiveConstructorTestInt>>();
}

TEST(F14ValueMap, heterogeneousLookup) {
  using Hasher = folly::transparent<folly::hasher<folly::StringPiece>>;
  using KeyEqual = folly::transparent<std::equal_to<folly::StringPiece>>;

  constexpr auto hello = "hello"_sp;
  constexpr auto buddy = "buddy"_sp;
  constexpr auto world = "world"_sp;

  F14ValueMap<std::string, bool, Hasher, KeyEqual> map;
  map.emplace(hello, true);
  map.emplace(world, false);

  auto checks = [hello, buddy](auto& ref) {
    // count
    EXPECT_EQ(0, ref.count(buddy));
    EXPECT_EQ(1, ref.count(hello));

    // find
    EXPECT_TRUE(ref.end() == ref.find(buddy));
    EXPECT_EQ(hello, ref.find(hello)->first);

    const auto buddyHashToken = ref.prehash(buddy);
    const auto helloHashToken = ref.prehash(hello);

    EXPECT_TRUE(
        buddyHashToken == ref.prehash(buddy, ref.hash_function()(buddy)));
    EXPECT_TRUE(
        helloHashToken == ref.prehash(hello, ref.hash_function()(hello)));
#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
    EXPECT_FALSE(
        buddyHashToken == ref.prehash(hello, ref.hash_function()(hello)));
#endif

    // prehash + find
    EXPECT_TRUE(ref.end() == ref.find(buddyHashToken, buddy));
    EXPECT_EQ(hello, ref.find(helloHashToken, hello)->first);

    // contains
    EXPECT_FALSE(ref.contains(buddy));
    EXPECT_TRUE(ref.contains(hello));

    // contains with prehash
    EXPECT_FALSE(ref.contains(buddyHashToken, buddy));
    EXPECT_TRUE(ref.contains(helloHashToken, hello));

    // equal_range
    EXPECT_TRUE(std::make_pair(ref.end(), ref.end()) == ref.equal_range(buddy));
    EXPECT_TRUE(
        std::make_pair(ref.find(hello), ++ref.find(hello)) ==
        ref.equal_range(hello));
  };

  checks(map);
  checks(std::as_const(map));
}

template <typename M>
void runStatefulFunctorTest() {
  bool ranHasher = false;
  bool ranEqual = false;
  bool ranAlloc = false;
  bool ranDealloc = false;

  auto hasher = [&](int x) {
    ranHasher = true;
    return x;
  };
  auto equal = [&](int x, int y) {
    ranEqual = true;
    return x == y;
  };
  auto alloc = [&](std::size_t n) {
    ranAlloc = true;
    return std::malloc(n);
  };
  auto dealloc = [&](void* p, std::size_t) {
    ranDealloc = true;
    std::free(p);
  };

  {
    M map(0, hasher, equal, {alloc, dealloc});
    map[10]++;
    map[10]++;
    EXPECT_EQ(map[10], 2);

    M map2(map);
    M map3(std::move(map));
    map = map2;
    map2.clear();
    map2 = std::move(map3);
  }
  EXPECT_TRUE(ranHasher);
  EXPECT_TRUE(ranEqual);
  EXPECT_TRUE(ranAlloc);
  EXPECT_TRUE(ranDealloc);
}

TEST(F14ValueMap, statefulFunctors) {
  runStatefulFunctorTest<F14ValueMap<
      int,
      int,
      GenericHasher<int>,
      GenericEqual<int>,
      GenericAlloc<std::pair<int const, int>>>>();
}

TEST(F14NodeMap, statefulFunctors) {
  runStatefulFunctorTest<F14NodeMap<
      int,
      int,
      GenericHasher<int>,
      GenericEqual<int>,
      GenericAlloc<std::pair<int const, int>>>>();
}

TEST(F14VectorMap, statefulFunctors) {
  runStatefulFunctorTest<F14VectorMap<
      int,
      int,
      GenericHasher<int>,
      GenericEqual<int>,
      GenericAlloc<std::pair<int const, int>>>>();
}

TEST(F14FastMap, statefulFunctors) {
  runStatefulFunctorTest<F14FastMap<
      int,
      int,
      GenericHasher<int>,
      GenericEqual<int>,
      GenericAlloc<std::pair<int const, int>>>>();
}

template <typename M>
void runHeterogeneousInsertTest() {
  M map;

  resetTracking();
  EXPECT_EQ(map.count(10), 0);
  EXPECT_FALSE(map.contains(10));
  EXPECT_EQ(Tracked<1>::counts().dist(Counts{0, 0, 0, 0}), 0)
      << Tracked<1>::counts();

  resetTracking();
  map[10] = 20;
  EXPECT_EQ(Tracked<1>::counts().dist(Counts{0, 0, 0, 1}), 0)
      << Tracked<1>::counts();

  resetTracking();
  std::pair<int, int> p(10, 30);
  std::vector<std::pair<int, int>> v({p});
  map[10] = 30;
  map.insert(std::pair<int, int>(10, 30));
  map.insert(std::pair<int const, int>(10, 30));
  map.insert(p);
  map.insert(v.begin(), v.end());
  map.insert(
      std::make_move_iterator(v.begin()), std::make_move_iterator(v.end()));
  map.insert_or_assign(10, 40);
  EXPECT_EQ(Tracked<1>::counts().dist(Counts{0, 0, 0, 0}), 0)
      << Tracked<1>::counts();

  resetTracking();
  map.emplace(10, 30);
  map.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(10),
      std::forward_as_tuple(30));
  map.emplace(p);
  map.try_emplace(10, 30);
  map.try_emplace(10);
  EXPECT_EQ(Tracked<1>::counts().dist(Counts{0, 0, 0, 0}), 0)
      << Tracked<1>::counts();

  resetTracking();
  map.erase(10);
  EXPECT_EQ(map.size(), 0);
  EXPECT_EQ(Tracked<1>::counts().dist(Counts{0, 0, 0, 0}), 0)
      << Tracked<1>::counts();

  map.emplace(10, 40);
  resetTracking();
  map.erase(map.find(10));
  EXPECT_EQ(map.size(), 0);
  EXPECT_EQ(Tracked<1>::counts().dist(Counts{0, 0, 0, 0}), 0)
      << Tracked<1>::counts();

  map.emplace(10, 40);
  resetTracking();
  map.eraseInto(10, [](auto&&, auto&&) {});
  EXPECT_EQ(map.size(), 0);
  EXPECT_EQ(Tracked<1>::counts().dist(Counts{0, 0, 0, 0}), 0)
      << Tracked<1>::counts();

  const auto t = map.prehash(10);
  resetTracking();
  map.try_emplace_token(t, 10, 40);
  EXPECT_TRUE(map.contains(t, 10));
  EXPECT_EQ(Tracked<1>::counts().dist(Counts{0, 0, 0, 1}), 0)
      << Tracked<1>::counts();
  resetTracking();
  map.erase(map.find(t, 10));
  EXPECT_EQ(map.size(), 0);
  EXPECT_EQ(Tracked<1>::counts().dist(Counts{0, 0, 0, 0}), 0)
      << Tracked<1>::counts();
}

template <typename M>
void runHeterogeneousInsertStringTest() {
  using P = std::pair<StringPiece, std::string>;
  using CP = std::pair<const StringPiece, std::string>;

  M map;
  P p{"foo", "hello"};
  std::vector<P> v{p};
  StringPiece foo{"foo"};

  map.insert(P("foo", "hello"));
  // TODO(T31574848): the list-initialization below does not work on libstdc++
  // versions (e.g., GCC < 6) with no implementation of N4387 ("perfect
  // initialization" for pairs and tuples).
  //   StringPiece sp{"foo"};
  //   map.insert({sp, "hello"});
  map.insert({"foo", "hello"});
  map.insert(CP("foo", "hello"));
  map.insert(p);
  map.insert(v.begin(), v.end());
  map.insert(
      std::make_move_iterator(v.begin()), std::make_move_iterator(v.end()));
  map.insert_or_assign("foo", "hello");
  map.insert_or_assign(StringPiece{"foo"}, "hello");
  EXPECT_EQ(map["foo"], "hello");

  map.emplace(StringPiece{"foo"}, "hello");
  map.emplace("foo", "hello");
  map.emplace(p);
  map.emplace();
  map.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(StringPiece{"foo"}),
      std::forward_as_tuple(/* count */ 20, 'x'));
  map.try_emplace(StringPiece{"foo"}, "hello");
  map.try_emplace(foo, "hello");
  map.try_emplace(foo);
  map.try_emplace("foo");
  map.try_emplace("foo", "hello");
  map.try_emplace("foo", /* count */ 20, 'x');

  map.erase(StringPiece{"foo"});
  map.erase(foo);
  map.erase("");
  EXPECT_TRUE(map.empty());

  map.try_emplace(foo);
  map.erase(map.find(foo));
  map.try_emplace(foo);
  typename M::const_iterator it = map.find(foo);
  map.erase(it);
}

TEST(F14ValueMap, heterogeneousInsert) {
  runHeterogeneousInsertTest<F14ValueMap<
      Tracked<1>,
      int,
      TransparentTrackedHash<1>,
      TransparentTrackedEqual<1>>>();
  runHeterogeneousInsertStringTest<F14ValueMap<
      std::string,
      std::string,
      transparent<hasher<StringPiece>>,
      transparent<DefaultKeyEqual<StringPiece>>>>();
  runHeterogeneousInsertStringTest<F14ValueMap<std::string, std::string>>();
}

TEST(F14NodeMap, heterogeneousInsert) {
  runHeterogeneousInsertTest<F14NodeMap<
      Tracked<1>,
      int,
      TransparentTrackedHash<1>,
      TransparentTrackedEqual<1>>>();
  runHeterogeneousInsertStringTest<F14NodeMap<
      std::string,
      std::string,
      transparent<hasher<StringPiece>>,
      transparent<DefaultKeyEqual<StringPiece>>>>();
  runHeterogeneousInsertStringTest<F14NodeMap<std::string, std::string>>();
}

TEST(F14VectorMap, heterogeneousInsert) {
  runHeterogeneousInsertTest<F14VectorMap<
      Tracked<1>,
      int,
      TransparentTrackedHash<1>,
      TransparentTrackedEqual<1>>>();
  runHeterogeneousInsertStringTest<F14VectorMap<
      std::string,
      std::string,
      transparent<hasher<StringPiece>>,
      transparent<DefaultKeyEqual<StringPiece>>>>();
  runHeterogeneousInsertStringTest<F14VectorMap<std::string, std::string>>();
}

TEST(F14FastMap, heterogeneousInsert) {
  runHeterogeneousInsertTest<F14FastMap<
      Tracked<1>,
      int,
      TransparentTrackedHash<1>,
      TransparentTrackedEqual<1>>>();
  runHeterogeneousInsertStringTest<F14FastMap<
      std::string,
      std::string,
      transparent<hasher<StringPiece>>,
      transparent<DefaultKeyEqual<StringPiece>>>>();
  runHeterogeneousInsertStringTest<F14FastMap<std::string, std::string>>();
}

namespace {

// std::is_convertible is not transitive :( Problem scenario: B<T> is
// implicitly convertible to A, so hasher that takes A can be used as a
// transparent hasher for a map with key of type B<T>. C is implicitly
// convertible to any B<T>, but we have to disable heterogeneous find
// for C.  There is no way to infer the T of the intermediate type so C
// can't be used to explicitly construct A.

struct A {
  int value;

  friend bool operator==(A const&, A const&) = default;
};

struct AHasher {
  std::size_t operator()(A const& v) const { return v.value; }
};

template <typename T>
struct B {
  int value;

  explicit B(int v) : value(v) {}

  /* implicit */ B(A const& v) : value(v.value) {}

  /* implicit */ operator A() const { return A{value}; }
};

struct C {
  int value;

  template <typename T>
  /* implicit */ operator B<T>() const {
    return B<T>{value};
  }
};
} // namespace

TEST(F14FastMap, disabledDoubleTransparent) {
  static_assert(std::is_convertible<B<char>, A>::value);
  static_assert(std::is_convertible<C, B<char>>::value);
  static_assert(!std::is_convertible<C, A>::value);

  F14FastMap<
      B<char>,
      int,
      folly::transparent<AHasher>,
      folly::transparent<std::equal_to<A>>>
      map;
  map[A{10}] = 10;

  EXPECT_TRUE(map.find(C{10}) != map.end());
  EXPECT_TRUE(map.find(C{20}) == map.end());
}

template <typename M>
void runRandomInsertOrderTest() {
  if (FOLLY_F14_PERTURB_INSERTION_ORDER) {
    std::string prev;
    bool diffFound = false;
    for (int tries = 0; tries < 100; ++tries) {
      M m;
      for (char x = '0'; x <= '7'; ++x) {
        m.try_emplace(x);
      }
      m.reserve(10);
      auto it = m.try_emplace('8').first;
      auto addr = &*it;
      m.try_emplace('9');
      EXPECT_TRUE(it == m.find('8'));
      EXPECT_TRUE(addr = &*m.find('8'));
      std::string s;
      for (auto&& e : m) {
        s.push_back(e.first);
      }
      LOG(INFO) << s << "\n";
      if (prev.empty()) {
        prev = s;
        continue;
      }
      if (prev != s) {
        diffFound = true;
        break;
      }
    }
    EXPECT_TRUE(diffFound) << "no randomness found in insert order";
  }
}

TEST(F14Map, randomInsertOrder) {
  runRandomInsertOrderTest<F14ValueMap<char, char>>();
  runRandomInsertOrderTest<F14FastMap<char, char>>();
  runRandomInsertOrderTest<F14FastMap<char, std::string>>();
}

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

template <typename M>
void runContinuousCapacityTest(std::size_t minSize, std::size_t maxSize) {
  using K = typename M::key_type;
  for (std::size_t n = minSize; n <= maxSize; ++n) {
    M m1;
    m1.reserve(n);
    auto cap = m1.bucket_count();
    double ratio = cap * 1.0 / n;
    // worst case scenario is that rehash just occurred and capacityScale
    // is 5*2^12
    EXPECT_TRUE(ratio < 1 + 1.0 / (5 << 12))
        << ratio << ", " << cap << ", " << n;
    m1[0];
    M m2;
    m2 = m1;
    EXPECT_LE(m2.bucket_count(), 2);
    for (K i = 1; i < n; ++i) {
      folly::makeUnpredictable(i);
      m1[i];
    }
    EXPECT_EQ(m1.bucket_count(), cap);
    M m3 = m1;
    EXPECT_EQ(m3.bucket_count(), cap);
    for (K i = n; i <= cap; ++i) {
      folly::makeUnpredictable(i);
      m1[i];
    }
    EXPECT_GT(m1.bucket_count(), cap);
    EXPECT_LE(m1.bucket_count(), 3 * cap);

    M m4;
    for (K i = 0; i < n; ++i) {
      folly::makeUnpredictable(i);
      m4[i];
    }
    // reserve(0) works like shrink_to_fit.  Note that tight fit (1/8
    // waste bound) only applies for vector policy or single-chunk, which
    // might not apply to m1.  m3 should already have been optimally sized.
    m1.reserve(0);
    m3.reserve(0);
    m4.reserve(0);
    EXPECT_GT(m1.load_factor(), 0.5);
    EXPECT_GE(m3.load_factor(), 0.875);
    EXPECT_EQ(m3.bucket_count(), cap);
    EXPECT_GE(m4.load_factor(), 0.875);
  }
}

TEST(F14Map, continuousCapacitySmall0) {
  runContinuousCapacityTest<F14NodeMap<std::size_t, std::string>>(1, 14);
}

TEST(F14Map, continuousCapacitySmall1) {
  runContinuousCapacityTest<F14ValueMap<std::size_t, std::string>>(1, 14);
}

TEST(F14Map, continuousCapacitySmall2) {
  runContinuousCapacityTest<F14VectorMap<std::size_t, std::string>>(1, 100);
}

TEST(F14Map, continuousCapacitySmall3) {
  runContinuousCapacityTest<F14FastMap<std::size_t, std::string>>(1, 14);
}

TEST(F14Map, continuousCapacityBig0) {
  runContinuousCapacityTest<F14VectorMap<std::size_t, std::string>>(
      1000000 - 1, 1000000 - 1);
}

TEST(F14Map, continuousCapacityBig1) {
  runContinuousCapacityTest<F14VectorMap<std::size_t, std::string>>(
      1000000, 1000000);
}

TEST(F14Map, continuousCapacityBig2) {
  runContinuousCapacityTest<F14VectorMap<std::size_t, std::string>>(
      1000000 + 1, 1000000 + 1);
}

TEST(F14Map, continuousCapacityBig3) {
  runContinuousCapacityTest<F14VectorMap<std::size_t, std::string>>(
      1000000 + 2, 1000000 + 2);
}

TEST(F14Map, continuousCapacityF12) {
  runContinuousCapacityTest<F14VectorMap<uint16_t, uint16_t>>(0xfff0, 0xfffe);
}

#endif

template <template <class...> class TMap>
void testContainsWithPrecomputedHash() {
  TMap<int, int> m{};
  const auto key{1};
  m.insert({key, 1});
  const auto hashToken = m.prehash(key);
  EXPECT_TRUE(m.contains(hashToken, key));
  const auto otherKey{2};
  const auto hashTokenNotFound = m.prehash(otherKey);
  EXPECT_FALSE(m.contains(hashTokenNotFound, otherKey));

  m.prefetch(hashToken);
  m.prefetch(hashTokenNotFound);
}

TEST(F14Map, containsWithPrecomputedHash) {
  testContainsWithPrecomputedHash<F14ValueMap>();
  testContainsWithPrecomputedHash<F14VectorMap>();
  testContainsWithPrecomputedHash<F14NodeMap>();
  testContainsWithPrecomputedHash<F14FastMap>();
}

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
template <template <class...> class TMap>
void testFindHashedKey() {
  TMap<std::string, int> m{};
  std::string key{"hello"};
  m.insert({key, 1});

  F14HashedKey<std::string> hashedKey{key};
  EXPECT_NE(m.find(hashedKey), m.end());

  std::string otherKey{"folly"};
  F14HashedKey<std::string> hashedKeyNotFound{otherKey};
  EXPECT_EQ(m.find(hashedKeyNotFound), m.end());
}

TEST(F14Map, findHashedKey) {
  testFindHashedKey<F14ValueMap>();
  testFindHashedKey<F14VectorMap>();
  testFindHashedKey<F14NodeMap>();
  testFindHashedKey<F14FastMap>();
}

template <template <class...> class TMap>
void testFindHashedKeyTransparent() {
  struct Hasher {
    using is_transparent = void;
    size_t operator()(int key) const { return key; }
  };
  struct Equal {
    using is_transparent = void;
    bool operator()(int a, int b) const { return a == b; }
  };

  TMap<int, int, Hasher, Equal> m{};
  int key{0};
  m.insert({key, 1});

  F14HashedKey<int, Hasher, Equal> hashedKey{key};
  EXPECT_NE(m.find(hashedKey), m.end());

  int otherKey{1};
  F14HashedKey<int, Hasher, Equal> hashedKeyNotFound{otherKey};
  EXPECT_EQ(m.find(hashedKeyNotFound), m.end());
}

TEST(F14Map, findHashedKeyTransparent) {
  testFindHashedKeyTransparent<F14ValueMap>();
  testFindHashedKeyTransparent<F14VectorMap>();
  testFindHashedKeyTransparent<F14NodeMap>();
  testFindHashedKeyTransparent<F14FastMap>();
}

template <template <class...> class TMap>
void testContainsHashedKey() {
  TMap<std::string, int> m{};
  std::string key{"hello"};
  m.insert({key, 1});

  F14HashedKey<std::string> hashedKey{key};
  EXPECT_TRUE(m.contains(hashedKey));

  std::string otherKey{"folly"};
  F14HashedKey<std::string> hashedKeyNotFound{otherKey};
  EXPECT_FALSE(m.contains(hashedKeyNotFound));
}

TEST(F14Map, containsHashedKey) {
  testContainsHashedKey<F14ValueMap>();
  testContainsHashedKey<F14VectorMap>();
  testContainsHashedKey<F14NodeMap>();
  testContainsHashedKey<F14FastMap>();
}

template <template <class...> class TMap>
void testContainsHeterogeneousHashedKey() {
  TMap<std::string, int> m{};
  std::string key{"hello"};
  m.insert({key, 1});

  F14HashedKey<std::string_view> hashedKey{key};
  EXPECT_TRUE(m.contains(hashedKey));

  std::string otherKey{"folly"};
  F14HashedKey<std::string_view> hashedKeyNotFound{otherKey};
  EXPECT_FALSE(m.contains(hashedKeyNotFound));
}

TEST(F14Map, containsHeterogeneousHashedKey) {
  testContainsHeterogeneousHashedKey<F14ValueMap>();
  testContainsHeterogeneousHashedKey<F14VectorMap>();
  testContainsHeterogeneousHashedKey<F14NodeMap>();
  testContainsHeterogeneousHashedKey<F14FastMap>();
}

template <template <class...> class TMap>
void testInsertOrAssignHashedKey() {
  TMap<std::string, int> m{};
  std::string key{"hello"};
  m.insert({key, 1});

  F14HashedKey<std::string> hashedKey{key};
  EXPECT_FALSE(m.insert_or_assign(hashedKey, 2).second);

  std::string otherKey{"folly"};
  F14HashedKey<std::string> hashedKeyNotFound{otherKey};
  EXPECT_TRUE(m.insert_or_assign(hashedKeyNotFound, 3).second);
}

TEST(F14Map, insertOrAssignHashedKey) {
  testInsertOrAssignHashedKey<F14ValueMap>();
  testInsertOrAssignHashedKey<F14VectorMap>();
  testInsertOrAssignHashedKey<F14NodeMap>();
  testInsertOrAssignHashedKey<F14FastMap>();
}

template <template <class...> class TMap>
void testInsertOrAssignRValueHashedKey() {
  TMap<std::string, int> m{};
  std::string key{"hello"};
  m.insert({key, 1});

  F14HashedKey<std::string> hashedKey{key};
  EXPECT_FALSE(m.insert_or_assign(std::move(hashedKey), 2).second);

  std::string otherKey{"folly"};
  F14HashedKey<std::string> hashedKeyNotFound{otherKey};
  EXPECT_TRUE(m.insert_or_assign(std::move(hashedKeyNotFound), 3).second);
}

TEST(F14Map, insertOrAssignRValueHashedKey) {
  testInsertOrAssignRValueHashedKey<F14ValueMap>();
  testInsertOrAssignRValueHashedKey<F14VectorMap>();
  testInsertOrAssignRValueHashedKey<F14NodeMap>();
  testInsertOrAssignRValueHashedKey<F14FastMap>();
}

template <template <class...> class TMap>
void testContainsWithPrecomputedHashKeyWrapperTransparent() {
  struct Key {
    int num{};
    Key(double, int num_) : num{num_} {}
  };
  static_assert(!std::is_constructible_v<Key, int>);
  static_assert(!std::is_constructible_v<int, Key>);
  static_assert(!folly::is_detected_v<detect_op_eq, Key, Key>);
  static_assert(!folly::is_detected_v<detect_op_eq, Key, int>);
  static_assert(!folly::is_detected_v<detect_op_eq, int, Key>);
  struct KeyHash {
    using is_transparent = void;
    size_t operator()(Key key) const { return key.num; }
    size_t operator()(int key) const { return key; }
  };
  struct KeyEqual {
    using is_transparent = void;
    bool operator()(Key a, Key b) const { return a.num == b.num; }
    bool operator()(Key a, int b) const { return a.num == b; }
    bool operator()(int a, Key b) const { return a == b.num; }
  };
  using Map = TMap<Key, const char*, KeyHash, KeyEqual>;
  using HKey = typename Map::hashed_key_type;
  static_assert(std::is_same_v<HKey, F14HashedKey<Key, KeyHash, KeyEqual>>);

  int num = 3;
  Key key{0., num};
  HKey hkey{key};

  Map m{};
  EXPECT_FALSE(m.count(key));
  EXPECT_FALSE(m.count(num));
  EXPECT_FALSE(m.count(hkey));

  m.insert({key, "hello"});
  EXPECT_TRUE(m.count(key));
  EXPECT_TRUE(m.count(num));
  EXPECT_TRUE(m.count(hkey));
  EXPECT_STREQ("hello", m.find(key)->second);
  EXPECT_STREQ("hello", m.find(num)->second);
  EXPECT_STREQ("hello", m.find(hkey)->second);
  m.clear();

  m.insert({hkey, "world"});
  EXPECT_TRUE(m.count(key));
  EXPECT_TRUE(m.count(num));
  EXPECT_TRUE(m.count(hkey));
  EXPECT_STREQ("world", m.find(key)->second);
  EXPECT_STREQ("world", m.find(num)->second);
  EXPECT_STREQ("world", m.find(hkey)->second);
  m.clear();
}

TEST(F14Map, containsWithPrecomputedHashKeyWrapperTransparent) {
  testContainsWithPrecomputedHashKeyWrapperTransparent<F14ValueMap>();
  testContainsWithPrecomputedHashKeyWrapperTransparent<F14VectorMap>();
  testContainsWithPrecomputedHashKeyWrapperTransparent<F14NodeMap>();
  testContainsWithPrecomputedHashKeyWrapperTransparent<F14FastMap>();
}
#endif // FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

template <template <class...> class TMap>
void testEraseIf() {
  TMap<int, int> m{{1, 1}, {2, 2}, {3, 3}, {4, 4}};
  const auto isEvenKey = [](const auto& p) { return p.first % 2 == 0; };
  EXPECT_EQ(2u, erase_if(m, isEvenKey));
  ASSERT_EQ(2u, m.size());
  EXPECT_TRUE(m.contains(1));
  EXPECT_TRUE(m.contains(3));
}

TEST(F14Map, eraseIf) {
  testEraseIf<F14ValueMap>();
  testEraseIf<F14VectorMap>();
  testEraseIf<F14NodeMap>();
  testEraseIf<F14FastMap>();
}

namespace {
template <std::size_t N>
struct DivideBy {
  // this is a lie for testing purposes
  using folly_is_avalanching = std::true_type;

  std::size_t operator()(std::size_t v) const { return v / N; }
};
} // namespace

template <template <class...> class TMap>
void testCopyAfterRemovedCollisions() {
  // Insert 11 things into chunks 0, 1, and 2, 15 into chunk 3, then
  // remove all but the last one from chunk 1 and see if we can find that
  // one in a copy of the map.
  TMap<std::size_t, bool, DivideBy<16>> map;
  map.reserve(48);
  for (std::size_t k = 0; k < 11; ++k) {
    map[k] = true;
    map[k + 16] = true;
    map[k + 32] = true;
  }
  for (std::size_t k = 0; k < 14; ++k) {
    map[k + 48] = true;
  }
  map[14 + 48] = true;
  for (std::size_t k = 0; k < 14; ++k) {
    map.erase(k + 48);
  }
  auto copy = map;
  EXPECT_EQ(copy.count(14 + 48), 1);
}

TEST(F14Map, copyAfterRemovedCollisions) {
  testCopyAfterRemovedCollisions<F14ValueMap>();
  testCopyAfterRemovedCollisions<F14VectorMap>();
  testCopyAfterRemovedCollisions<F14NodeMap>();
  testCopyAfterRemovedCollisions<F14FastMap>();
}

TEST(F14Map, bulkInsertAutoReserve) {
  constexpr std::size_t kRangeSize = 1000;
  using Value = std::pair<int, int>;

  std::vector<Value> randomAccessInput(kRangeSize, Value{1, 1});
  std::list<Value> forwardInput(
      randomAccessInput.begin(), randomAccessInput.end());

  F14ValueMap<int, int> automatic(
      randomAccessInput.begin(), randomAccessInput.end());
  F14ValueMap<int, int> explicitCapacity(
      randomAccessInput.begin(), randomAccessInput.end(), 8);
  F14ValueMap<int, int> forward(forwardInput.begin(), forwardInput.end());
  F14ValueMap<int, int> inserted;
  inserted.insert(randomAccessInput.begin(), randomAccessInput.end());

  EXPECT_EQ(automatic.size(), 1);
  EXPECT_EQ(explicitCapacity.size(), 1);
  EXPECT_EQ(forward.size(), 1);
  EXPECT_EQ(inserted.size(), 1);
  if (!kFallback) {
    EXPECT_GE(automatic.bucket_count(), kRangeSize);
    EXPECT_GE(explicitCapacity.bucket_count(), 8);
    EXPECT_LT(explicitCapacity.bucket_count(), automatic.bucket_count());
    EXPECT_LT(forward.bucket_count(), automatic.bucket_count());
    EXPECT_GE(inserted.bucket_count(), kRangeSize);
  }
}

template <template <class...> class TMap>
void testIterDeductionGuide() {
  TMap<int, double> source({{1, 2.0}, {3, 4.0}});

  TMap dest1(source.begin(), source.end());
  static_assert(std::is_same_v<decltype(dest1), decltype(source)>);
  EXPECT_EQ(dest1, source);

  TMap dest2(source.begin(), source.end(), 2);
  static_assert(std::is_same_v<decltype(dest2), decltype(source)>);
  EXPECT_EQ(dest2, source);

  TMap dest3(source.begin(), source.end(), 2, f14::DefaultHasher<int>{});
  static_assert(std::is_same_v<decltype(dest3), decltype(source)>);
  EXPECT_EQ(dest3, source);

  TMap dest4(
      source.begin(),
      source.end(),
      2,
      f14::DefaultHasher<int>{},
      f14::DefaultKeyEqual<int>{});
  static_assert(std::is_same_v<decltype(dest4), decltype(source)>);
  EXPECT_EQ(dest4, source);

  TMap dest5(
      source.begin(),
      source.end(),
      2,
      f14::DefaultHasher<int>{},
      f14::DefaultKeyEqual<int>{},
      f14::DefaultAlloc<std::pair<const int, double>>{});
  static_assert(std::is_same_v<decltype(dest5), decltype(source)>);
  EXPECT_EQ(dest5, source);

  TMap dest6(
      source.begin(),
      source.end(),
      2,
      f14::DefaultAlloc<std::pair<const int, double>>{});
  static_assert(std::is_same_v<decltype(dest6), decltype(source)>);
  EXPECT_EQ(dest6, source);

  TMap dest7(
      source.begin(),
      source.end(),
      2,
      f14::DefaultHasher<int>{},
      f14::DefaultAlloc<std::pair<const int, double>>{});
  static_assert(std::is_same_v<decltype(dest7), decltype(source)>);
  EXPECT_EQ(dest7, source);
}

TEST(F14Map, iterDeductionGuide) {
  testIterDeductionGuide<F14ValueMap>();
  testIterDeductionGuide<F14NodeMap>();
  testIterDeductionGuide<F14VectorMap>();
  testIterDeductionGuide<F14FastMap>();
}

template <template <class...> class TMap>
void testInitializerListDeductionGuide() {
  TMap<int, double> source({{1, 2.0}, {3, 4.0}});

#if !defined(__GNUC__) || __GNUC__ > 12 || defined(__clang__)
  // some versions of gcc, including until at least gcc v12, fail here
  TMap dest1{std::pair{1, 2.0}, {3, 4.0}};
  static_assert(std::is_same_v<decltype(dest1), decltype(source)>);
  EXPECT_EQ(dest1, source);
#endif

  TMap dest2({std::pair{1, 2.0}, {3, 4.0}});
  static_assert(std::is_same_v<decltype(dest2), decltype(source)>);
  EXPECT_EQ(dest2, source);

  TMap dest3({std::pair{1, 2.0}, {3, 4.0}}, 2);
  static_assert(std::is_same_v<decltype(dest3), decltype(source)>);
  EXPECT_EQ(dest3, source);

  TMap dest4({std::pair{1, 2.0}, {3, 4.0}}, 2, f14::DefaultHasher<int>{});
  static_assert(std::is_same_v<decltype(dest4), decltype(source)>);
  EXPECT_EQ(dest4, source);

  TMap dest5(
      {std::pair{1, 2.0}, {3, 4.0}},
      2,
      f14::DefaultHasher<int>{},
      f14::DefaultKeyEqual<int>{});
  static_assert(std::is_same_v<decltype(dest5), decltype(source)>);
  EXPECT_EQ(dest5, source);

  TMap dest6(
      {std::pair{1, 2.0}, {3, 4.0}},
      2,
      f14::DefaultHasher<int>{},
      f14::DefaultKeyEqual<int>{},
      f14::DefaultAlloc<std::pair<const int, double>>{});
  static_assert(std::is_same_v<decltype(dest6), decltype(source)>);
  EXPECT_EQ(dest6, source);

  TMap dest7(
      {std::pair{1, 2.0}, {3, 4.0}},
      2,
      f14::DefaultAlloc<std::pair<const int, double>>{});
  static_assert(std::is_same_v<decltype(dest7), decltype(source)>);
  EXPECT_EQ(dest7, source);

  TMap dest8(
      {std::pair{1, 2.0}, {3, 4.0}},
      2,
      f14::DefaultHasher<int>{},
      f14::DefaultAlloc<std::pair<const int, double>>{});
  static_assert(std::is_same_v<decltype(dest8), decltype(source)>);
  EXPECT_EQ(dest8, source);
}

TEST(F14Map, initializerListDeductionGuide) {
  testInitializerListDeductionGuide<F14ValueMap>();
  testInitializerListDeductionGuide<F14NodeMap>();
  testInitializerListDeductionGuide<F14VectorMap>();
  testInitializerListDeductionGuide<F14FastMap>();
}

namespace {

struct TracedMovable {
 public:
  TracedMovable() = default;

  TracedMovable& operator=(TracedMovable&& other) noexcept {
    if (this != &other) {
      n = std::exchange(other.n, 0);
    }
    return *this;
  }

  TracedMovable(TracedMovable&& other) noexcept {
    n = std::exchange(other.n, 0);
  }

  int32_t n{10};
};

} // namespace

template <template <class...> class TMap>
void testInsertOrAssignUnchangedIfNoInsert() {
  TMap<int32_t, TracedMovable> m;

  m[0] = TracedMovable{};
  EXPECT_EQ(m[0].n, 10);

  m.insert_or_assign(0, TracedMovable{});
  EXPECT_EQ(m[0].n, 10);
}

TEST(F14Map, insertOrAssignUnchangedIfNoInsert) {
  testInsertOrAssignUnchangedIfNoInsert<F14ValueMap>();
  testInsertOrAssignUnchangedIfNoInsert<F14NodeMap>();
  testInsertOrAssignUnchangedIfNoInsert<F14VectorMap>();
  testInsertOrAssignUnchangedIfNoInsert<F14FastMap>();
}

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

template <typename M>
void runSimpleShrinkToFitTest(float expectedLoadFactor) {
  using K = typename M::key_type;
  for (int n = 1; n <= 1000; ++n) {
    M m;
    for (K k = 0; k < n; ++k) {
      m[k];
    }
    m.reserve(0); // reserve(0) works like shrink_to_fit
    EXPECT_GE(m.load_factor(), expectedLoadFactor);
  }
}

TEST(F14Map, shrinkToFit) {
  runSimpleShrinkToFitTest<F14NodeMap<int, int>>(0.5);
  runSimpleShrinkToFitTest<F14ValueMap<int, int>>(0.5);
  runSimpleShrinkToFitTest<F14VectorMap<int, int>>(0.875);
}

template <typename M>
void runDefactoShrinkToFitTest(float expectedLoadFactor) {
  for (int n = 1; n <= 1000; n += (n + 9) / 10) {
    M m1;
    for (int k = 0; k < n; ++k) {
      m1[k];
    }
    for (int j = 0; j <= n; ++j) {
      auto m2 = m1;
      // any argument < size is a "defacto" shrink_to_fit
      m2.reserve(m2.size() - j);
      EXPECT_GE(m2.load_factor(), expectedLoadFactor);
    }
  }
}

TEST(F14Map, defactoShrinkToFit) {
  runDefactoShrinkToFitTest<F14NodeMap<int, int>>(0.5);
  runDefactoShrinkToFitTest<F14ValueMap<int, int>>(0.5);
  runDefactoShrinkToFitTest<F14VectorMap<int, int>>(0.875);
}

template <typename M>
void runInitialReserveTest(float expectedLoadFactor) {
  auto initBucketsCtor = [](int initBuckets) { return M(initBuckets); };
  auto defaultCtorAndReserve = [](int initBuckets) {
    M m;
    m.reserve(initBuckets);
    return m;
  };

  auto fill = [](M& m, int n) {
    using K = typename M::key_type;
    auto initBuckets = m.bucket_count();
    for (K k = 0; k < n; ++k) {
      m[k];
      EXPECT_EQ(m.bucket_count(), initBuckets);
    }
  };

  using MakeFuncs = std::initializer_list<std::function<M(int)>>;
  for (const auto& make : MakeFuncs{initBucketsCtor, defaultCtorAndReserve}) {
    for (int n = 1; n <= 1000; ++n) {
      auto m = make(n);
      fill(m, n);
      EXPECT_GE(m.load_factor(), expectedLoadFactor);
    }
  }
}

TEST(F14Map, initialReserve) {
  runInitialReserveTest<F14NodeMap<int, int>>(0.5);
  runInitialReserveTest<F14ValueMap<int, int>>(0.5);
  runInitialReserveTest<F14VectorMap<int, int>>(0.875);
}

template <typename M>
void runReserveMoreTest(int n) {
  constexpr int kIters = 1000;
  M m;
  int k = 0;
  for (int i = 0; i < kIters; ++i) {
    auto bc = m.bucket_count();
    m.reserve(m.size() + n);
    EXPECT_GE(m.bucket_count(), bc); // should never shrink
    for (int j = 0; j < n; ++j) {
      bc = m.bucket_count();
      m[k++];
      EXPECT_EQ(m.bucket_count(), bc);
    }
  }
}

TEST(F14Map, reserveMoreNeverShrinks) {
  runReserveMoreTest<F14NodeMap<int, int>>(1);
  runReserveMoreTest<F14ValueMap<int, int>>(1);
  runReserveMoreTest<F14VectorMap<int, int>>(1);
  runReserveMoreTest<F14NodeMap<int, int>>(10);
  runReserveMoreTest<F14ValueMap<int, int>>(10);
  runReserveMoreTest<F14VectorMap<int, int>>(10);
}

TEST(F14Map, reserveBadAlloc) {
  SKIP_IF(
      std::numeric_limits<size_t>::max() <=
      std::numeric_limits<uint32_t>::max());
  EXPECT_THROW(
      (F14VectorMap<int, int>().reserve(
          std::size_t{std::numeric_limits<uint32_t>::max()} + 1)),
      std::bad_alloc);
}

#endif

TEST(F14Map, InsertOrAssignShouldNotMoveTheData) {
  F14FastMap<int, std::vector<int>> map;
  std::vector<int> data = {1, 2, 3};
  map.insert_or_assign(0, data);
  map.insert_or_assign(0, data);
  EXPECT_EQ(data.size(), 3);
}

template <template <typename, typename> class TMap>
void testInsertRange() {
  using Map = TMap<int, int>;
  Map dest{{0, 0}, {1, 1}};
  const Map source{{1, -1}, {2, 2}};
  const Map expected{{0, 0}, {1, 1}, {2, 2}};

  dest.insert_range(source);
  EXPECT_EQ(expected, dest);

  dest.insert_range(source);
  EXPECT_EQ(expected, dest);
}

TEST(F14Map, insertRange) {
  testInsertRange<F14ValueMap>();
  testInsertRange<F14NodeMap>();
  testInsertRange<F14VectorMap>();
  testInsertRange<F14FastMap>();
}
