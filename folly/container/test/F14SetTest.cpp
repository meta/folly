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

#include <folly/Portability.h>

// Allow tests for keys that throw in copy/move constructors. This
// warning has to be disabled before the templates are defined in the
// header to have any effect.
FOLLY_GNU_DISABLE_WARNING("-Wdeprecated-declarations")

// clang-format off:
#include <folly/container/F14Set.h>
// clang-format on

#include <list>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <glog/logging.h>

#include <folly/Benchmark.h>
#include <folly/Conv.h>
#include <folly/FBString.h>
#include <folly/container/test/F14TestUtil.h>
#include <folly/container/test/IteratorTestUtil.h>
#include <folly/container/test/TrackingTypes.h>
#include <folly/lang/Keep.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <folly/test/TestUtils.h>

using namespace folly;
using namespace folly::f14;
using namespace folly::string_piece_literals;
using namespace folly::test;

extern "C" FOLLY_KEEP int check_std_unordered_set_int_accumulate(
    std::unordered_set<int> const& set) {
  return std::accumulate(set.begin(), set.end(), 0);
}
extern "C" FOLLY_KEEP int check_folly_f14_node_set_int_accumulate(
    folly::F14NodeSet<int> const& set) {
  return std::accumulate(set.begin(), set.end(), 0);
}
extern "C" FOLLY_KEEP int check_folly_f14_vector_set_int_accumulate(
    folly::F14VectorSet<int> const& set) {
  return std::accumulate(set.begin(), set.end(), 0);
}
extern "C" FOLLY_KEEP int check_folly_f14_value_set_int_accumulate(
    folly::F14ValueSet<int> const& set) {
  return std::accumulate(set.begin(), set.end(), 0);
}

extern "C" FOLLY_KEEP size_t
check_std_unordered_set_int_count(std::unordered_set<int> const& set, int key) {
  return set.count(key);
}
extern "C" FOLLY_KEEP size_t
check_folly_node_set_int_count(folly::F14NodeSet<int> const& set, int key) {
  return set.count(key);
}
extern "C" FOLLY_KEEP size_t
check_folly_vector_set_int_count(folly::F14VectorSet<int> const& set, int key) {
  return set.count(key);
}
extern "C" FOLLY_KEEP size_t
check_folly_value_set_int_count(folly::F14ValueSet<int> const& set, int key) {
  return set.count(key);
}

static constexpr bool kFallback = folly::f14::detail::getF14IntrinsicsMode() ==
    folly::f14::detail::F14IntrinsicsMode::None;

template <typename T>
void runSanityChecks(T const& t) {
  (void)t;
#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
  F14TableStats::compute(t);
#endif
}

template <template <typename, typename, typename, typename> class TSet>
void testCustomSwap() {
  using std::swap;

  TSet<int, DefaultHasher<int>, DefaultKeyEqual<int>, SwapTrackingAlloc<int>>
      m0, m1;
  resetTracking();
  swap(m0, m1);

  EXPECT_EQ(0, Tracked<0>::counts().dist(Counts{0, 0, 0, 0}));
}

TEST(F14Set, customSwap) {
  testCustomSwap<F14ValueSet>();
  testCustomSwap<F14NodeSet>();
  testCustomSwap<F14VectorSet>();
  testCustomSwap<F14FastSet>();
}

namespace {
template <
    template <typename, typename, typename, typename> class TSet,
    typename K>
void runAllocatedMemorySizeTest() {
  using A = SwapTrackingAlloc<K>;

  resetTracking();
  {
    TSet<K, DefaultHasher<K>, DefaultKeyEqual<K>, A> s;

    // if F14 intrinsics are not available then we fall back to using
    // std::unordered_set underneath, but in that case the allocation
    // info is only best effort
    if (!kFallback) {
      EXPECT_EQ(testAllocatedMemorySize(), 0);
      EXPECT_EQ(s.getAllocatedMemorySize(), 0);
    }
    auto emptySetAllocatedMemorySize = testAllocatedMemorySize();
    auto emptySetAllocatedBlockCount = testAllocatedBlockCount();

    for (size_t i = 0; i < 1000; ++i) {
      s.insert(to<K>(i));
      s.erase(to<K>(i / 10 + 2));
      if (!kFallback) {
        EXPECT_EQ(testAllocatedMemorySize(), s.getAllocatedMemorySize());
      }
      EXPECT_GE(s.getAllocatedMemorySize(), sizeof(K) * s.size());
      std::size_t size = 0;
      std::size_t count = 0;
      s.visitAllocationClasses([&](std::size_t, std::size_t) mutable {});
      s.visitAllocationClasses([&](std::size_t bytes, std::size_t n) {
        size += bytes * n;
        count += n;
      });
      if (!kFallback) {
        EXPECT_EQ(testAllocatedMemorySize(), size);
        EXPECT_EQ(testAllocatedBlockCount(), count);
      }
    }

    s = decltype(s){};
    EXPECT_EQ(testAllocatedMemorySize(), emptySetAllocatedMemorySize);
    EXPECT_EQ(testAllocatedBlockCount(), emptySetAllocatedBlockCount);

    s.reserve(5);
    EXPECT_GT(testAllocatedMemorySize(), 0);
    s = {};
    if (!kFallback) {
      EXPECT_GT(testAllocatedMemorySize(), 0);
    }
  }
  EXPECT_EQ(testAllocatedMemorySize(), 0);
  EXPECT_EQ(testAllocatedBlockCount(), 0);
}

template <typename K>
void runAllocatedMemorySizeTests() {
  runAllocatedMemorySizeTest<F14ValueSet, K>();
  runAllocatedMemorySizeTest<F14NodeSet, K>();
  runAllocatedMemorySizeTest<F14VectorSet, K>();
  runAllocatedMemorySizeTest<F14FastSet, K>();
}
} // namespace

TEST(F14Set, getAllocatedMemorySize) {
  runAllocatedMemorySizeTests<bool>();
  runAllocatedMemorySizeTests<int>();
  runAllocatedMemorySizeTests<long>();
  runAllocatedMemorySizeTests<double>();
  runAllocatedMemorySizeTests<std::string>();
  runAllocatedMemorySizeTests<fbstring>();
}

template <typename S>
void runVisitContiguousRangesTest(int n) {
  S set;

  for (int i = 0; i < n; ++i) {
    makeUnpredictable(i);
    set.insert(i);
    set.erase(i / 2);
  }

  std::unordered_map<uintptr_t, bool> visited;
  for (auto& entry : set) {
    visited[reinterpret_cast<uintptr_t>(&entry)] = false;
  }

  set.visitContiguousRanges([&](auto b, auto e) {
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

template <typename S>
void runVisitContiguousRangesTest() {
  runVisitContiguousRangesTest<S>(0); // empty
  runVisitContiguousRangesTest<S>(5); // single chunk
  runVisitContiguousRangesTest<S>(1000); // many chunks
}

TEST(F14ValueSet, visitContiguousRanges) {
  runVisitContiguousRangesTest<F14ValueSet<int>>();
}

TEST(F14NodeSet, visitContiguousRanges) {
  runVisitContiguousRangesTest<F14NodeSet<int>>();
}

TEST(F14VectorSet, visitContiguousRanges) {
  runVisitContiguousRangesTest<F14VectorSet<int>>();
}

TEST(F14FastSet, visitContiguousRanges) {
  runVisitContiguousRangesTest<F14FastSet<int>>();
}

#if FOLLY_HAS_MEMORY_RESOURCE
TEST(F14Set, pmrEmpty) {
  pmr::F14ValueSet<int> s1;
  pmr::F14NodeSet<int> s2;
  pmr::F14VectorSet<int> s3;
  pmr::F14FastSet<int> s4;
  EXPECT_TRUE(s1.empty() && s2.empty() && s3.empty() && s4.empty());
}
#endif

namespace {
struct NestedHash {
  template <typename N>
  std::size_t operator()(N const& v) const;
};

template <template <class...> class TSet>
struct Nested {
  std::unique_ptr<TSet<Nested, NestedHash>> set_;

  explicit Nested(int depth)
      : set_(std::make_unique<TSet<Nested, NestedHash>>()) {
    if (depth > 0) {
      set_->emplace(depth - 1);
    }
  }
};

template <typename N>
std::size_t NestedHash::operator()(N const& v) const {
  std::size_t rv = 0;
  for (auto& k : *v.set_) {
    rv += operator()(k);
  }
  return Hash{}(rv);
}

template <template <class...> class TSet>
bool operator==(Nested<TSet> const& lhs, Nested<TSet> const& rhs) {
  return *lhs.set_ == *rhs.set_;
}

template <template <class...> class TSet>
bool operator!=(Nested<TSet> const& lhs, Nested<TSet> const& rhs) {
  return !(lhs == rhs);
}

template <template <class...> class TSet>
void testNestedSetEquality() {
  auto n1 = Nested<TSet>(100);
  auto n2 = Nested<TSet>(100);
  auto n3 = Nested<TSet>(99);
  EXPECT_TRUE(n1 == n1);
  EXPECT_TRUE(n1 == n2);
  EXPECT_FALSE(n1 == n3);
  EXPECT_FALSE(n1 != n1);
  EXPECT_FALSE(n1 != n2);
  EXPECT_TRUE(n1 != n3);
}

template <template <class...> class TSet>
void testEqualityRefinement() {
  TSet<std::pair<int, int>, HashFirst, EqualFirst> s1;
  TSet<std::pair<int, int>, HashFirst, EqualFirst> s2;
  s1.insert(std::make_pair(0, 0));
  s1.insert(std::make_pair(1, 1));
  EXPECT_FALSE(s1.insert(std::make_pair(0, 2)).second);
  EXPECT_EQ(s1.size(), 2);
  EXPECT_EQ(s1.count(std::make_pair(0, 10)), 1);
  for (auto& k : s1) {
    s2.emplace(k.first, k.second + 1);
  }
  EXPECT_EQ(s1.size(), s2.size());
  for (auto& k : s1) {
    EXPECT_EQ(s2.count(k), 1);
  }
  EXPECT_FALSE(s1 == s2);
  EXPECT_TRUE(s1 != s2);
}
} // namespace

TEST(F14Set, nestedSetEquality) {
  testNestedSetEquality<F14ValueSet>();
  testNestedSetEquality<F14NodeSet>();
  testNestedSetEquality<F14VectorSet>();
  testNestedSetEquality<F14FastSet>();
}

TEST(F14Set, equalityRefinement) {
  testEqualityRefinement<F14ValueSet>();
  testEqualityRefinement<F14NodeSet>();
  testEqualityRefinement<F14VectorSet>();
  testEqualityRefinement<F14FastSet>();
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
  std::vector<std::string> v({"abc", "abc"});
  h.insert(v.begin(), v.begin());
  EXPECT_EQ(h.size(), 0);
  if (!kFallback) {
    EXPECT_EQ(h.bucket_count(), 0);
  }
  h.insert(v.begin(), v.end());
  EXPECT_EQ(h.size(), 1);
  h = T{};
  if (!kFallback) {
    EXPECT_EQ(h.bucket_count(), 0);
  }

  h.insert(s("abc"));
  EXPECT_TRUE(h.find(s("def")) == h.end());
  EXPECT_FALSE(h.find(s("abc")) == h.end());
  h.insert(s("ghi"));
  EXPECT_EQ(h.size(), 2);
  h.erase(h.find(s("abc")));
  EXPECT_EQ(h.size(), 1);

  T h2(std::move(h));
  EXPECT_EQ(h.size(), 0);
  EXPECT_TRUE(h.begin() == h.end());
  EXPECT_EQ(h2.size(), 1);

  EXPECT_TRUE(h2.find(s("abc")) == h2.end());
  EXPECT_EQ(*h2.begin(), s("ghi"));
  {
    auto i = h2.begin();
    EXPECT_FALSE(i == h2.end());
    ++i;
    EXPECT_TRUE(i == h2.end());
  }

  T h3;
  h3.insert(s("xxx"));
  h3.insert(s("yyy"));
  h3 = std::move(h2);
  EXPECT_EQ(h2.size(), 0);
  EXPECT_EQ(h3.size(), 1);
  EXPECT_TRUE(h3.find(s("xxx")) == h3.end());

  for (uint64_t i = 0; i < 1000; ++i) {
    h.insert(std::move(to<std::string>(i * i * i)));
    EXPECT_EQ(h.size(), i + 1);
  }
  {
    using std::swap;
    swap(h, h2);
  }
  for (uint64_t i = 0; i < 1000; ++i) {
    EXPECT_TRUE(h2.find(to<std::string>(i * i * i)) != h2.end());
    EXPECT_EQ(*h2.find(to<std::string>(i * i * i)), to<std::string>(i * i * i));
    EXPECT_TRUE(h2.find(to<std::string>(i * i * i + 2)) == h2.end());
  }

  T h4{h2};
  EXPECT_EQ(h2.size(), 1000);
  EXPECT_EQ(h4.size(), 1000);

  T h5{std::move(h2)};
  T h6;
  h6 = h4;
  T h7 = h4;

  T h8({s("abc"), s("def")});
  T h9({s("abd"), s("def")});
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

  h8.clear();
  h8.emplace(s("abc"));
  if (!kFallback) {
    EXPECT_GT(h8.bucket_count(), 1);
  }
  h8 = {};
  if (!kFallback) {
    EXPECT_GT(h8.bucket_count(), 1);
  }
  h9 = {s("abc"), s("def")};
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
}

template <typename T>
void runEraseWhileIterating() {
  constexpr int kNumElements = 1000;

  // mul and kNumElements should be relatively prime
  for (int mul : {1, 3, 17, 137, kNumElements - 1}) {
    for (int interval : {1, 3, 5, kNumElements / 2}) {
      T h;
      for (auto i = 0; i < kNumElements; ++i) {
        EXPECT_TRUE(h.emplace((i * mul) % kNumElements).second);
      }

      int sum = 0;
      for (auto it = h.begin(); it != h.end();) {
        sum += *it;
        if (*it % interval == 0) {
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
  using Value = typename T::value_type;
  T h;
  for (int k = 0; k < c.size; ++k) {
    h.emplace(k);
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
  for (unsigned i = 0; i < n; ++i) {
    h.insert(to<std::string>(i));
  }
  EXPECT_EQ(h.size(), n);
  runSanityChecks(h);
}

// T should be a set of uint64_t
template <typename T>
void runRandom() {
  using R = std::unordered_set<uint64_t>;

  std::mt19937_64 gen(0);
  std::uniform_int_distribution<> pctDist(0, 100);
  std::uniform_int_distribution<uint64_t> bitsBitsDist(1, 6);
  T t0;
  T t1;
  R r0;
  R r1;

  for (std::size_t reps = 0; reps < 100000; ++reps) {
    // discardBits will be from 0 to 62
    auto discardBits = (uint64_t{1} << bitsBitsDist(gen)) - 2;
    auto k = gen() >> discardBits;
    auto pct = pctDist(gen);

    EXPECT_EQ(t0.size(), r0.size());
    if (pct < 15) {
      // insert
      auto t = t0.insert(k);
      auto r = r0.insert(k);
      EXPECT_EQ(t.second, r.second);
      EXPECT_EQ(*t.first, *r.first);
    } else if (pct < 25) {
      // emplace
      auto t = t0.emplace(k);
      auto r = r0.emplace(k);
      EXPECT_EQ(t.second, r.second);
      EXPECT_EQ(*t.first, *r.first);
    } else if (pct < 30) {
      // bulk insert
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
        k = *r;
        auto t = t0.find(k);
        t = t0.erase(t);
        if (t != t0.end()) {
          EXPECT_NE(*t, k);
        }
        r = r0.erase(r);
        if (r != r0.end()) {
          EXPECT_NE(*r, k);
        }
      }
    } else if (pct < 50) {
      // bulk erase
      if (t0.size() > 0) {
        auto r = r0.find(k);
        if (r == r0.end()) {
          r = r0.begin();
        }
        k = *r;
        auto t = t0.find(k);
        auto firstt = t;
        auto lastt = ++t;
        t = t0.erase(firstt, lastt);
        if (t != t0.end()) {
          EXPECT_NE(*t, k);
        }
        auto firstr = r;
        auto lastr = ++r;
        r = r0.erase(firstr, lastr);
        if (r != r0.end()) {
          EXPECT_NE(*r, k);
        }
      }
    } else if (pct < 58) {
      // find
      auto t = t0.find(k);
      auto r = r0.find(k);
      EXPECT_EQ((t == t0.end()), (r == r0.end()));
      if (t != t0.end() && r != r0.end()) {
        EXPECT_EQ(*t, *r);
      }
      EXPECT_EQ(t0.count(k), r0.count(k));
      // TODO: When std::unordered_set supports c++20:
      // EXPECT_EQ(t0.contains(k), r0.contains(k));
    } else if (pct < 60) {
      // equal_range
      auto t = t0.equal_range(k);
      auto r = r0.equal_range(k);
      EXPECT_EQ((t.first == t.second), (r.first == r.second));
      if (t.first != t.second && r.first != r.second) {
        EXPECT_EQ(*t.first, *r.first);
        t.first++;
        r.first++;
        EXPECT_TRUE(t.first == t.second);
        EXPECT_TRUE(r.first == r.second);
      }
    } else if (pct < 65) {
      // iterate
      uint64_t t = 0;
      for (auto& e : t0) {
        t += e + 1000;
      }
      uint64_t r = 0;
      for (auto& e : r0) {
        r += e + 1000;
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
      t0.~T();
      new (&t0) T(capacity);
      r0.~R();
      new (&r0) R(capacity);
    } else if (pct < 80) {
      // bulk iterator construct
      t0.~T();
      new (&t0) T(r1.begin(), r1.end());
      r0.~R();
      new (&r0) R(r1.begin(), r1.end());
    } else if (pct < 82) {
      // initializer list construct
      auto k2 = gen() >> discardBits;
      t0.~T();
      new (&t0) T({k, k, k2});
      r0.~R();
      new (&r0) R({k, k, k2});
    } else if (pct < 88) {
      // copy construct
      t0.~T();
      new (&t0) T(t1);
      r0.~R();
      new (&r0) R(r1);
    } else if (pct < 90) {
      // move construct
      t0.~T();
      new (&t0) T(std::move(t1));
      r0.~R();
      new (&r0) R(std::move(r1));
    } else if (pct < 94) {
      // copy assign
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
  }
}

TEST(F14ValueSet, simple) {
  runSimple<F14ValueSet<std::string>>();
}

TEST(F14NodeSet, simple) {
  runSimple<F14NodeSet<std::string>>();
}

TEST(F14VectorSet, simple) {
  runSimple<F14VectorSet<std::string>>();
}

TEST(F14FastSet, simple) {
  // F14FastSet internally uses a conditional typedef. Verify it compiles.
  runRandom<F14FastSet<uint64_t>>();
  runSimple<F14FastSet<std::string>>();
}

#if FOLLY_HAS_MEMORY_RESOURCE
TEST(F14ValueSet, pmrSimple) {
  runSimple<pmr::F14ValueSet<std::string>>();
}

TEST(F14NodeSet, pmrSimple) {
  runSimple<pmr::F14NodeSet<std::string>>();
}

TEST(F14VectorSet, pmrSimple) {
  runSimple<pmr::F14VectorSet<std::string>>();
}

TEST(F14FastSet, pmrSimple) {
  // F14FastSet internally uses a conditional typedef. Verify it compiles.
  runRandom<pmr::F14FastSet<uint64_t>>();
  runSimple<pmr::F14FastSet<std::string>>();
}
#endif

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

TEST(F14Set, ContainerSize) {
  {
    F14ValueSet<int> set;
    set.insert(10);
    EXPECT_EQ(sizeof(set), 3 * sizeof(void*));
    if (alignof(folly::max_align_t) == 16) {
      // chunks will be allocated as 2 max_align_t-s
      EXPECT_EQ(set.getAllocatedMemorySize(), 32);
    } else {
      // chunks will be allocated using aligned_malloc with the true size
      EXPECT_EQ(set.getAllocatedMemorySize(), 24);
    }
  }
  {
    F14NodeSet<int> set;
    set.insert(10);
    EXPECT_EQ(sizeof(set), 3 * sizeof(void*));
    if (alignof(folly::max_align_t) == 16) {
      // chunks will be allocated as 2 max_align_t-s
      EXPECT_EQ(set.getAllocatedMemorySize(), 36);
    } else {
      // chunks will be allocated using aligned_malloc with the true size
      EXPECT_EQ(set.getAllocatedMemorySize(), 20 + 2 * sizeof(void*));
    }
  }
  {
    F14VectorSet<int> set;
    set.insert(10);
    EXPECT_EQ(sizeof(set), 8 + 2 * sizeof(void*));
    EXPECT_EQ(set.getAllocatedMemorySize(), 32);
  }
}

TEST(F14VectorMap, reverseIterator) {
  using TSet = F14VectorSet<uint64_t>;
  auto populate = [](TSet& h, uint64_t lo, uint64_t hi) {
    for (auto i = lo; i < hi; ++i) {
      h.insert(i);
    }
  };
  auto verify = [](TSet const& h, uint64_t lo, uint64_t hi) {
    EXPECT_NE(h.end(), h.find(lo));
    uint64_t val = lo;
    for (auto rit = h.rbegin() + lo; rit != h.rend(); ++rit) {
      EXPECT_EQ(val, *rit);
      val++;
    }
    EXPECT_EQ(hi, val);
  };

  TSet h;
  size_t prevSize = 0;
  size_t newSize = 1;
  // verify iteration order across rehashes, copies, and moves
  while (newSize < 10'000) {
    populate(h, prevSize, newSize);
    verify(h, 0, newSize);
    verify(h, newSize / 2, newSize);

    TSet h2{h};
    verify(h2, 0, newSize);

    h = std::move(h2);
    verify(h, 0, newSize);
    prevSize = newSize;
    newSize *= 10;
  }
}

TEST(F14VectorSet, iterators) {
  using namespace folly::iterator_test_utils;
  using TSet = F14VectorSet<int>;

  check_traits_match<TSet::iterator, std::reverse_iterator<const int*>>();
  check_traits_match<TSet::reverse_iterator, const int*>();
  static_assert(std::is_same_v<TSet::iterator, TSet::const_iterator>);
  static_assert(
      std::is_same_v<TSet::reverse_iterator, TSet::const_reverse_iterator>);
  static_assert(std::is_trivially_copyable_v<TSet::iterator>);

  TSet s;
  for (int i = 0; i < 5; ++i) {
    s.insert(i);
  }
  const int* first = s.data();
  const int* last = first + s.size();
  const std::reverse_iterator ref_first{last};
  const std::reverse_iterator ref_last{first};

  EXPECT_TRUE(same_access(s.begin(), ref_first, ref_last));
  EXPECT_TRUE(same_arithmetic(s.begin(), ref_first, ref_last));
  EXPECT_TRUE(
      same_differences(s.begin(), s.end(), s.cbegin(), ref_first, ref_last));
  EXPECT_TRUE(same_comparisons(s.begin(), s.end(), ref_first, ref_last));

  EXPECT_TRUE(same_access(s.rbegin(), first, last));
  EXPECT_TRUE(same_arithmetic(s.rbegin(), first, last));
  EXPECT_TRUE(same_differences(s.rbegin(), s.rend(), s.crbegin(), first, last));
  EXPECT_TRUE(same_comparisons(s.rbegin(), s.rend(), first, last));

  TSet other{1};
  EXPECT_DEBUG_DEATH((void)(s.begin() == other.begin()), "");
}

TEST(F14VectorSet, OrderPreservingReinsertionView) {
  F14VectorSet<int> s1;
  for (size_t i = 0; i < 5; ++i) {
    s1.emplace(i);
  }

  F14VectorSet<int> s2;
  for (const auto& k : order_preserving_reinsertion_view(s1)) {
    s2.insert(k);
  }

  EXPECT_EQ(asVector(s1), asVector(s2));
}

template <typename T>
inline constexpr bool has_data_v = requires(T& v) { v.data(); };

TEST(F14VectorSet, Data) {
  using TSet = F14VectorSet<int>;
  TSet s;
  for (int i = 0; i < 5; ++i) {
    s.emplace(i);
  }

  // Storage order matches [rbegin, rend), the raw values_ range. Elements
  // are const even through a non-const set, like set iterators.
  auto* p = s.data();
  static_assert(std::is_same_v<decltype(p), int const*>);
  EXPECT_EQ(p, std::to_address(s.rbegin()));
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(i, p[i]);
  }

  TSet const& cs = s;
  EXPECT_EQ(cs.data(), p);

  TSet e;
  EXPECT_EQ(e.data(), std::to_address(e.rbegin()));

  static_assert(!has_data_v<F14ValueSet<int>>);
  static_assert(!has_data_v<F14NodeSet<int>>);
  static_assert(!has_data_v<F14FastSet<int>>);
  static_assert(!has_data_v<F14FastSet<std::string>>);
  static_assert(has_data_v<pmr::F14VectorSet<int>>);
}

template <typename T>
inline constexpr bool has_as_span_v = requires(T& v) { v.as_span(); };

TEST(F14VectorSet, AsSpan) {
  using TSet = F14VectorSet<int>;
  TSet s;
  for (int i = 0; i < 5; ++i) {
    s.emplace(i);
  }

  // as_span() is data() and size() bundled into a span.
  auto sp = s.as_span();
  static_assert(std::is_same_v<decltype(sp), std::span<int const>>);
  EXPECT_EQ(sp.data(), s.data());
  EXPECT_EQ(sp.size(), s.size());

  TSet const& cs = s;
  auto csp = cs.as_span();
  EXPECT_EQ(csp.data(), cs.data());
  EXPECT_EQ(csp.size(), cs.size());

  TSet e;
  EXPECT_TRUE(e.as_span().empty());

  static_assert(!has_as_span_v<F14ValueSet<int>>);
  static_assert(!has_as_span_v<F14NodeSet<int>>);
  static_assert(!has_as_span_v<F14FastSet<int>>);
  static_assert(!has_as_span_v<F14FastSet<std::string>>);
  static_assert(has_as_span_v<pmr::F14VectorSet<int>>);
}

#endif

TEST(F14ValueSet, eraseWhileIterating) {
  runEraseWhileIterating<F14ValueSet<int>>();
}

TEST(F14NodeSet, eraseWhileIterating) {
  runEraseWhileIterating<F14NodeSet<int>>();
}

TEST(F14VectorSet, eraseWhileIterating) {
  runEraseWhileIterating<F14VectorSet<int>>();
}

TEST(F14ValueSet, eraseRange) {
  runEraseRange<F14ValueSet<int>>();
}

TEST(F14NodeSet, eraseRange) {
  runEraseRange<F14NodeSet<int>>();
}

TEST(F14VectorSet, eraseRange) {
  runEraseRange<F14VectorSet<int>>();
}

// Validates the erase range test facility against the standard container.
TEST(StdUnorderedSet, eraseRange) {
  runEraseRange<std::unordered_set<int>>();
}

TEST(F14ValueSet, rehash) {
  runRehash<F14ValueSet<std::string>>();
}

TEST(F14NodeSet, rehash) {
  runRehash<F14NodeSet<std::string>>();
}

TEST(F14VectorSet, rehash) {
  runRehash<F14VectorSet<std::string>>();
}

TEST(F14ValueSet, random) {
  runRandom<F14ValueSet<uint64_t>>();
}

TEST(F14NodeSet, random) {
  runRandom<F14NodeSet<uint64_t>>();
}

TEST(F14VectorSet, random) {
  runRandom<F14VectorSet<uint64_t>>();
}

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

TEST(F14ValueSet, growStats) {
  F14ValueSet<uint64_t> h;
  for (unsigned i = 1; i <= 3072; ++i) {
    h.insert(i);
  }
  // F14ValueSet just before rehash
  runSanityChecks(h);
  h.insert(0);
  // F14ValueSet just after rehash
  runSanityChecks(h);
}

TEST(F14ValueSet, steadyStateStats) {
  // 10k keys, 14% probability of insert, 90% chance of erase, so the
  // table should converge to 1400 size without triggering the rehash
  // that would occur at 1536.
  F14ValueSet<uint64_t> h;
  std::mt19937 gen(0);
  std::uniform_int_distribution<> dist(0, 10000);
  for (std::size_t i = 0; i < 100000; ++i) {
    auto key = dist(gen);
    if (dist(gen) < 1400) {
      h.insert(key);
    } else {
      h.erase(key);
    }
    if (((i + 1) % 10000) == 0) {
      auto stats = F14TableStats::compute(h);
      // Verify that average miss probe length is bounded despite continued
      // erase + reuse.  p99 of the average across 10M random steps is 4.69,
      // average is 2.96.
      EXPECT_LT(f14::expectedProbe(stats.missProbeLengthHisto), 10.0);
    }
  }
  // F14ValueSet at steady state
  runSanityChecks(h);
}

#endif

// S should be a set of Tracked<0>.  F should take a set
// and a key_type const& or key_type&& and cause it to be inserted
template <typename S, typename F>
void runInsertCases(std::string const& /* name */, F const& insertFunc) {
  static_assert(std::is_same<typename S::value_type, Tracked<0>>::value);
  {
    typename S::value_type k{0};
    S s;
    resetTracking();
    insertFunc(s, k);
    // fresh key, value_type const& ->
    // copy is expected
    EXPECT_EQ(Tracked<0>::counts().dist(Counts{1, 0, 0, 0}), 0);
  }
  {
    typename S::value_type k{0};
    S s;
    resetTracking();
    insertFunc(s, std::move(k));
    // fresh key, value_type&& ->
    // move is expected
    EXPECT_EQ(Tracked<0>::counts().dist(Counts{0, 1, 0, 0}), 0);
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

template <typename S>
void runInsertAndEmplace() {
  {
    typename S::value_type k1{0};
    typename S::value_type k2{0};
    S s;
    resetTracking();
    EXPECT_TRUE(s.insert(k1).second);
    // copy is expected on successful insert
    EXPECT_EQ(Tracked<0>::counts().dist(Counts{1, 0, 0, 0}), 0);

    resetTracking();
    EXPECT_FALSE(s.insert(k2).second);
    // no copies or moves on failing insert
    EXPECT_EQ(Tracked<0>::counts().dist(Counts{0, 0, 0, 0}), 0);
  }
  {
    typename S::value_type k1{0};
    typename S::value_type k2{0};
    S s;
    resetTracking();
    EXPECT_TRUE(s.insert(std::move(k1)).second);
    // move is expected on successful insert
    EXPECT_EQ(Tracked<0>::counts().dist(Counts{0, 1, 0, 0}), 0);

    resetTracking();
    EXPECT_FALSE(s.insert(std::move(k2)).second);
    // no copies or moves on failing insert
    EXPECT_EQ(Tracked<0>::counts().dist(Counts{0, 0, 0, 0}), 0);
  }
  {
    typename S::value_type k1{0};
    typename S::value_type k2{0};
    uint64_t k3 = 0;
    S s;
    resetTracking();
    EXPECT_TRUE(s.emplace(k1).second);
    // copy is expected on successful emplace
    EXPECT_EQ(Tracked<0>::counts().dist(Counts{1, 0, 0, 0}), 0)
        << Tracked<0>::counts();

    resetTracking();
    EXPECT_FALSE(s.emplace(k2).second);
    if (!kFallback) {
      // no copies or moves on failing emplace with value_type
      EXPECT_EQ(Tracked<0>::counts().dist(Counts{0, 0, 0, 0}), 0)
          << Tracked<0>::counts();
    }

    resetTracking();
    EXPECT_FALSE(s.emplace(k3).second);
    if (!kFallback) {
      // copy convert expected for failing emplace with wrong type
      EXPECT_EQ(Tracked<0>::counts().dist(Counts{0, 0, 1, 0}), 0)
          << Tracked<0>::counts();
    }

    s.clear();
    resetTracking();
    EXPECT_TRUE(s.emplace(k3).second);
    // copy convert + move expected for successful emplace with wrong type
    EXPECT_EQ(Tracked<0>::counts().dist(Counts{0, 1, 1, 0}), 0)
        << Tracked<0>::counts();
  }
  {
    typename S::value_type k1{0};
    typename S::value_type k2{0};
    uint64_t k3 = 0;
    S s;
    resetTracking();
    EXPECT_TRUE(s.emplace(std::move(k1)).second);
    // move is expected on successful emplace
    EXPECT_EQ(Tracked<0>::counts().dist(Counts{0, 1, 0, 0}), 0)
        << Tracked<0>::counts();

    resetTracking();
    EXPECT_FALSE(s.emplace(std::move(k2)).second);
    if (!kFallback) {
      // no copies or moves on failing emplace with value_type
      EXPECT_EQ(Tracked<0>::counts().dist(Counts{0, 0, 0, 0}), 0)
          << Tracked<0>::counts();
    }

    resetTracking();
    EXPECT_FALSE(s.emplace(std::move(k3)).second);
    if (!kFallback) {
      // move convert expected for failing emplace with wrong type
      EXPECT_EQ(Tracked<0>::counts().dist(Counts{0, 0, 0, 1}), 0)
          << Tracked<0>::counts();
    }

    s.clear();
    resetTracking();
    EXPECT_TRUE(s.emplace(std::move(k3)).second);
    // move convert + move expected for successful emplace with wrong type
    EXPECT_EQ(Tracked<0>::counts().dist(Counts{0, 1, 0, 1}), 0)
        << Tracked<0>::counts();
  }

  // Calling the default pair constructor via emplace is valid, but not
  // very useful in real life.  Verify that it works.
  S s;
  typename S::value_type k;
  EXPECT_EQ(s.count(k), 0);
  EXPECT_FALSE(s.contains(k));
  s.emplace();
  EXPECT_EQ(s.count(k), 1);
  EXPECT_TRUE(s.contains(k));
  s.emplace();
  EXPECT_EQ(s.count(k), 1);
  EXPECT_TRUE(s.contains(k));
}

TEST(F14ValueSet, destructuring) {
  runInsertAndEmplace<F14ValueSet<Tracked<0>>>();
}

TEST(F14NodeSet, destructuring) {
  runInsertAndEmplace<F14NodeSet<Tracked<0>>>();
}

TEST(F14VectorSet, destructuring) {
  runInsertAndEmplace<F14VectorSet<Tracked<0>>>();
}

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
TEST(F14ValueSet, maxSize) {
  F14ValueSet<int> s;
  EXPECT_EQ(
      s.max_size(),
      std::min(
          folly::f14::detail::SizeAndChunkShift::kMaxSize,
          std::allocator_traits<decltype(s)::allocator_type>::max_size(
              s.get_allocator())));
}

TEST(F14NodeSet, maxSize) {
  F14NodeSet<int> s;
  EXPECT_EQ(
      s.max_size(),
      std::min(
          folly::f14::detail::SizeAndChunkShift::kMaxSize,
          std::allocator_traits<decltype(s)::allocator_type>::max_size(
              s.get_allocator())));
}

TEST(F14VectorSet, maxSize) {
  F14VectorSet<int> s;
  EXPECT_EQ(
      s.max_size(),
      std::min(
          {folly::f14::detail::SizeAndChunkShift::kMaxSize,
           std::size_t{std::numeric_limits<uint32_t>::max()},
           std::allocator_traits<decltype(s)::allocator_type>::max_size(
               s.get_allocator())}));
}
#endif

template <typename S>
void runMoveOnlyTest() {
  S t0;
  t0.emplace(10);
  t0.insert(20);
  S t1{std::move(t0)};
  EXPECT_TRUE(t0.empty());
  S t2;
  EXPECT_TRUE(t2.empty());
  t2 = std::move(t1);
  EXPECT_EQ(t2.size(), 2);
}

TEST(F14ValueSet, moveOnly) {
  runMoveOnlyTest<F14ValueSet<MoveOnlyTestInt>>();
}

TEST(F14NodeSet, moveOnly) {
  runMoveOnlyTest<F14NodeSet<MoveOnlyTestInt>>();
}

TEST(F14VectorSet, moveOnly) {
  runMoveOnlyTest<F14VectorSet<MoveOnlyTestInt>>();
}

TEST(F14FastSet, moveOnly) {
  runMoveOnlyTest<F14FastSet<MoveOnlyTestInt>>();
}

template <typename S>
void runEraseIntoTest() {
  S t0;
  S t1;

  auto insertIntoT0 = [&t0](auto&& value) {
    EXPECT_FALSE(value.destroyed);
    t0.emplace(std::move(value));
  };
  auto insertIntoT0Mut = [&](typename S::value_type&& value) mutable {
    insertIntoT0(std::move(value));
  };

  t0.insert(10);
  t1.insert(20);
  t1.eraseInto(t1.begin(), insertIntoT0);
  EXPECT_TRUE(t1.empty());
  EXPECT_EQ(t0.size(), 2);
  EXPECT_TRUE(t0.find(10) != t0.end());
  EXPECT_TRUE(t0.find(20) != t0.end());

  t1.insert(20);
  t1.eraseInto(t1.cbegin(), insertIntoT0);
  EXPECT_TRUE(t1.empty());

  t1.insert(20);
  t1.insert(30);
  t1.insert(40);
  t1.eraseInto(t1.begin(), t1.end(), insertIntoT0Mut);
  EXPECT_TRUE(t1.empty());
  EXPECT_EQ(t0.size(), 4);
  EXPECT_TRUE(t0.find(30) != t0.end());
  EXPECT_TRUE(t0.find(40) != t0.end());

  t1.insert(50);
  size_t erased = t1.eraseInto(*t1.find(50), insertIntoT0);
  EXPECT_EQ(erased, 1);
  EXPECT_TRUE(t1.empty());
  EXPECT_EQ(t0.size(), 5);
  EXPECT_TRUE(t0.find(50) != t0.end());

  typename S::value_type key{60};
  erased = t1.eraseInto(key, insertIntoT0Mut);
  EXPECT_EQ(erased, 0);
  EXPECT_EQ(t0.size(), 5);
}

TEST(F14ValueSet, eraseInto) {
  runEraseIntoTest<F14ValueSet<MoveOnlyTestInt>>();
}

TEST(F14NodeSet, eraseInto) {
  runEraseIntoTest<F14NodeSet<MoveOnlyTestInt>>();
}

TEST(F14VectorSet, eraseInto) {
  runEraseIntoTest<F14VectorSet<MoveOnlyTestInt>>();
}

TEST(F14FastSet, eraseInto) {
  runEraseIntoTest<F14FastSet<MoveOnlyTestInt>>();
}

TEST(F14ValueSet, heterogeneous) {
  // note: std::string is implicitly convertible to but not from StringPiece
  using Hasher = transparent<hasher<StringPiece>>;
  using KeyEqual = transparent<std::equal_to<StringPiece>>;

  constexpr auto hello = "hello"_sp;
  constexpr auto buddy = "buddy"_sp;
  constexpr auto world = "world"_sp;

  F14ValueSet<std::string, Hasher, KeyEqual> set;
  set.emplace(hello);
  set.emplace(world);

  auto checks = [hello, buddy](auto& ref) {
    // count
    EXPECT_EQ(0, ref.count(buddy));
    EXPECT_EQ(1, ref.count(hello));

    // find
    EXPECT_TRUE(ref.end() == ref.find(buddy));
    EXPECT_EQ(hello, *ref.find(hello));

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
    EXPECT_EQ(hello, *ref.find(helloHashToken, hello));

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

  checks(set);
  checks(std::as_const(set));
}

template <typename S>
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
    S set(0, hasher, equal, {alloc, dealloc});
    set.insert(10);
    set.insert(10);
    EXPECT_EQ(set.size(), 1);

    S set2(set);
    S set3(std::move(set));
    set = set2;
    set2.clear();
    set2 = std::move(set3);
  }
  EXPECT_TRUE(ranHasher);
  EXPECT_TRUE(ranEqual);
  EXPECT_TRUE(ranAlloc);
  EXPECT_TRUE(ranDealloc);
}

TEST(F14ValueSet, statefulFunctors) {
  runStatefulFunctorTest<F14ValueSet<
      int,
      GenericHasher<int>,
      GenericEqual<int>,
      GenericAlloc<int>>>();
}

TEST(F14NodeSet, statefulFunctors) {
  runStatefulFunctorTest<F14NodeSet<
      int,
      GenericHasher<int>,
      GenericEqual<int>,
      GenericAlloc<int>>>();
}

TEST(F14VectorSet, statefulFunctors) {
  runStatefulFunctorTest<F14VectorSet<
      int,
      GenericHasher<int>,
      GenericEqual<int>,
      GenericAlloc<int>>>();
}

TEST(F14FastSet, statefulFunctors) {
  runStatefulFunctorTest<F14FastSet<
      int,
      GenericHasher<int>,
      GenericEqual<int>,
      GenericAlloc<int>>>();
}

template <typename S>
void runHeterogeneousInsertTest() {
  S set;

  resetTracking();
  EXPECT_EQ(set.count(10), 0);
  EXPECT_FALSE(set.contains(10));
  EXPECT_EQ(Tracked<1>::counts().dist(Counts{0, 0, 0, 0}), 0)
      << Tracked<1>::counts;

  resetTracking();
  set.insert(10);
  EXPECT_EQ(Tracked<1>::counts().dist(Counts{0, 0, 0, 1}), 0)
      << Tracked<1>::counts;

  resetTracking();
  int k = 10;
  std::vector<int> v({10});
  set.insert(10);
  set.insert(k);
  set.insert(v.begin(), v.end());
  set.insert(
      std::make_move_iterator(v.begin()), std::make_move_iterator(v.end()));
  set.emplace(10);
  set.emplace(k);
  EXPECT_EQ(Tracked<1>::counts().dist(Counts{0, 0, 0, 0}), 0)
      << Tracked<1>::counts;

  resetTracking();
  set.erase(20);
  EXPECT_EQ(set.size(), 1);
  EXPECT_EQ(Tracked<1>::counts().dist(Counts{0, 0, 0, 0}), 0)
      << Tracked<1>::counts;

  resetTracking();
  set.erase(10);
  EXPECT_EQ(set.size(), 0);
  EXPECT_EQ(Tracked<1>::counts().dist(Counts{0, 0, 0, 0}), 0)
      << Tracked<1>::counts;

  set.insert(10);
  resetTracking();
  set.erase(set.find(10));
  EXPECT_EQ(set.size(), 0);
  EXPECT_EQ(Tracked<1>::counts().dist(Counts{0, 0, 0, 0}), 0)
      << Tracked<1>::counts;

  set.insert(10);
  resetTracking();
  set.eraseInto(10, [](auto&&) {});
  EXPECT_EQ(Tracked<1>::counts().dist(Counts{0, 0, 0, 0}), 0)
      << Tracked<1>::counts;
}

template <typename S>
void runHeterogeneousInsertStringTest() {
  S set;
  StringPiece k{"foo"};
  std::vector<StringPiece> v{k};

  set.insert(k);
  set.insert("foo");
  set.insert(StringPiece{"foo"});
  set.insert(v.begin(), v.end());
  set.insert(
      std::make_move_iterator(v.begin()), std::make_move_iterator(v.end()));

  set.emplace();
  set.emplace(k);
  set.emplace("foo");
  set.emplace(StringPiece("foo"));

  set.erase("");
  set.erase(k);
  set.erase(StringPiece{"foo"});
  EXPECT_TRUE(set.empty());

  set.insert(k);
  set.erase(set.find(k));
  set.insert(k);
  typename S::const_iterator it = set.find(k);
  set.erase(it);
}

TEST(F14ValueSet, heterogeneousInsert) {
  runHeterogeneousInsertTest<F14ValueSet<
      Tracked<1>,
      TransparentTrackedHash<1>,
      TransparentTrackedEqual<1>>>();
  runHeterogeneousInsertStringTest<F14ValueSet<
      std::string,
      transparent<hasher<StringPiece>>,
      transparent<DefaultKeyEqual<StringPiece>>>>();
  runHeterogeneousInsertStringTest<F14ValueSet<std::string>>();
}

TEST(F14NodeSet, heterogeneousInsert) {
  runHeterogeneousInsertTest<F14NodeSet<
      Tracked<1>,
      TransparentTrackedHash<1>,
      TransparentTrackedEqual<1>>>();
  runHeterogeneousInsertStringTest<F14NodeSet<
      std::string,
      transparent<hasher<StringPiece>>,
      transparent<DefaultKeyEqual<StringPiece>>>>();
  runHeterogeneousInsertStringTest<F14NodeSet<std::string>>();
}

TEST(F14VectorSet, heterogeneousInsert) {
  runHeterogeneousInsertTest<F14VectorSet<
      Tracked<1>,
      TransparentTrackedHash<1>,
      TransparentTrackedEqual<1>>>();
  runHeterogeneousInsertStringTest<F14VectorSet<
      std::string,
      transparent<hasher<StringPiece>>,
      transparent<DefaultKeyEqual<StringPiece>>>>();
  runHeterogeneousInsertStringTest<F14VectorSet<std::string>>();
}

TEST(F14FastSet, heterogeneousInsert) {
  runHeterogeneousInsertTest<F14FastSet<
      Tracked<1>,
      TransparentTrackedHash<1>,
      TransparentTrackedEqual<1>>>();
  runHeterogeneousInsertStringTest<F14FastSet<
      std::string,
      transparent<hasher<StringPiece>>,
      transparent<DefaultKeyEqual<StringPiece>>>>();
  runHeterogeneousInsertStringTest<F14FastSet<std::string>>();
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

TEST(F14FastSet, disabledDoubleTransparent) {
  static_assert(std::is_convertible<B<char>, A>::value);
  static_assert(std::is_convertible<C, B<char>>::value);
  static_assert(!std::is_convertible<C, A>::value);

  F14FastSet<B<char>, transparent<AHasher>, transparent<std::equal_to<A>>> set;
  set.emplace(A{10});

  EXPECT_TRUE(set.find(C{10}) != set.end());
  EXPECT_TRUE(set.find(C{20}) == set.end());
}

namespace {
struct CharArrayHasher {
  template <std::size_t N>
  std::size_t operator()(std::array<char, N> const& value) const {
    return Hash{}(StringPiece{value.data(), &value.data()[value.size()]});
  }
};

template <
    template <typename, typename, typename, typename> class S,
    std::size_t N>
struct RunAllValueSizeTests {
  void operator()() const {
    using Key = std::array<char, N>;
    static_assert(sizeof(Key) == N);
    S<Key, CharArrayHasher, std::equal_to<Key>, std::allocator<Key>> set;

    for (int i = 0; i < 100; ++i) {
      Key key{{static_cast<char>(i)}};
      set.insert(key);
    }
    while (!set.empty()) {
      set.erase(set.begin());
    }

    RunAllValueSizeTests<S, N - 1>{}();
  }
};

template <template <typename, typename, typename, typename> class S>
struct RunAllValueSizeTests<S, 0> {
  void operator()() const {}
};
} // namespace

TEST(F14ValueSet, valueSize) {
  RunAllValueSizeTests<F14ValueSet, 32>{}();
}

template <typename S, typename F>
void runRandomInsertOrderTest(F&& func) {
  if (FOLLY_F14_PERTURB_INSERTION_ORDER) {
    std::string prev;
    bool diffFound = false;
    for (int tries = 0; tries < 100; ++tries) {
      S set;
      for (char x = '0'; x <= '9'; ++x) {
        set.emplace(func(x));
      }
      std::string s;
      for (auto&& e : set) {
        s += e;
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

TEST(F14Set, randomInsertOrder) {
  runRandomInsertOrderTest<F14ValueSet<char>>([](char x) { return x; });
  runRandomInsertOrderTest<F14FastSet<char>>([](char x) { return x; });
  runRandomInsertOrderTest<F14FastSet<std::string>>([](char x) {
    return std::string{std::size_t{1}, x};
  });
}

template <template <class...> class TSet>
void testContainsWithPrecomputedHash() {
  TSet<int> m{};
  const auto key{1};
  m.insert(key);
  const auto hashToken = m.prehash(key);
  EXPECT_TRUE(m.contains(hashToken, key));
  const auto otherKey{2};
  const auto hashTokenNotFound = m.prehash(otherKey);
  EXPECT_FALSE(m.contains(hashTokenNotFound, otherKey));
  m.prefetch(hashToken);
  m.prefetch(hashTokenNotFound);
}

TEST(F14Set, containsWithPrecomputedHash) {
  testContainsWithPrecomputedHash<F14ValueSet>();
  testContainsWithPrecomputedHash<F14NodeSet>();
  testContainsWithPrecomputedHash<F14VectorSet>();
  testContainsWithPrecomputedHash<F14FastSet>();
}

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
template <template <class...> class TSet>
void testFindHashedKey() {
  TSet<std::string> s{};
  std::string key{"hello"};
  s.insert(key);

  F14HashedKey<std::string> hashedKey{key};
  EXPECT_NE(s.find(hashedKey), s.end());

  std::string otherKey{"folly"};
  F14HashedKey<std::string> hashedKeyNotFound{otherKey};
  EXPECT_EQ(s.find(hashedKeyNotFound), s.end());
}

TEST(F14Set, findHashedKey) {
  testFindHashedKey<F14ValueSet>();
  testFindHashedKey<F14NodeSet>();
  testFindHashedKey<F14VectorSet>();
  testFindHashedKey<F14FastSet>();
}

template <template <class...> class TSet>
void testContainsHashedKey() {
  TSet<std::string> s{};
  std::string key{"hello"};
  s.insert(key);

  F14HashedKey<std::string> hashedKey{key};
  EXPECT_TRUE(s.contains(hashedKey));

  std::string otherKey{"folly"};
  F14HashedKey<std::string> hashedKeyNotFound{otherKey};
  EXPECT_FALSE(s.contains(hashedKeyNotFound));
}

TEST(F14Set, containsHashedKey) {
  testContainsHashedKey<F14ValueSet>();
  testContainsHashedKey<F14NodeSet>();
  testContainsHashedKey<F14VectorSet>();
  testContainsHashedKey<F14FastSet>();
}

#endif // FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

template <template <class...> class TSet>
void testEraseIf() {
  TSet<int> s{1, 2, 3, 4};
  const auto isEvenKey = [](const auto& key) { return key % 2 == 0; };
  EXPECT_EQ(2u, erase_if(s, isEvenKey));
  ASSERT_EQ(2u, s.size());
  EXPECT_TRUE(s.contains(1));
  EXPECT_TRUE(s.contains(3));
}

TEST(F14Set, eraseIf) {
  testEraseIf<F14ValueSet>();
  testEraseIf<F14FastSet>();
  testEraseIf<F14VectorSet>();
  testEraseIf<F14NodeSet>();
}

template <template <class...> class TSet>
void testExceptionOnInsert() {
  TSet<ThrowOnCopyTestInt> m{};
  ThrowOnCopyTestInt key;
  EXPECT_THROW(m.insert(key), std::exception);
  EXPECT_TRUE(m.empty());
}

TEST(F14Set, ExceptionOnInsert) {
  testExceptionOnInsert<F14ValueSet>();
  testExceptionOnInsert<F14NodeSet>();
  testExceptionOnInsert<F14VectorSet>();
  testExceptionOnInsert<F14FastSet>();
}

TEST(F14Set, bulkInsertAutoReserve) {
  constexpr std::size_t kRangeSize = 1000;

  std::vector<int> randomAccessInput(kRangeSize, 1);
  std::list<int> forwardInput(
      randomAccessInput.begin(), randomAccessInput.end());

  F14ValueSet<int> automatic(
      randomAccessInput.begin(), randomAccessInput.end());
  F14ValueSet<int> explicitCapacity(
      randomAccessInput.begin(), randomAccessInput.end(), 8);
  F14ValueSet<int> forward(forwardInput.begin(), forwardInput.end());
  F14ValueSet<int> inserted;
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

template <template <class...> class TSet>
void testIterDeductionGuide() {
  TSet<int> source({1, 2});

  TSet dest1(source.begin(), source.end());
  static_assert(std::is_same_v<decltype(dest1), decltype(source)>);
  EXPECT_EQ(dest1, source);

  TSet dest2(source.begin(), source.end(), 2);
  static_assert(std::is_same_v<decltype(dest2), decltype(source)>);
  EXPECT_EQ(dest2, source);

  TSet dest3(source.begin(), source.end(), 2, f14::DefaultHasher<int>{});
  static_assert(std::is_same_v<decltype(dest3), decltype(source)>);
  EXPECT_EQ(dest3, source);

  TSet dest4(
      source.begin(),
      source.end(),
      2,
      f14::DefaultHasher<int>{},
      f14::DefaultKeyEqual<int>{});
  static_assert(std::is_same_v<decltype(dest4), decltype(source)>);
  EXPECT_EQ(dest4, source);

  TSet dest5(
      source.begin(),
      source.end(),
      2,
      f14::DefaultHasher<int>{},
      f14::DefaultKeyEqual<int>{},
      f14::DefaultAlloc<int>{});
  static_assert(std::is_same_v<decltype(dest5), decltype(source)>);
  EXPECT_EQ(dest5, source);

  TSet dest6(source.begin(), source.end(), 2, f14::DefaultAlloc<int>{});
  static_assert(std::is_same_v<decltype(dest6), decltype(source)>);
  EXPECT_EQ(dest6, source);

  TSet dest7(
      source.begin(),
      source.end(),
      2,
      f14::DefaultHasher<int>{},
      f14::DefaultAlloc<int>{});
  static_assert(std::is_same_v<decltype(dest7), decltype(source)>);
  EXPECT_EQ(dest7, source);
}

TEST(F14Set, iterDeductionGuide) {
  testIterDeductionGuide<F14ValueSet>();
  testIterDeductionGuide<F14NodeSet>();
  testIterDeductionGuide<F14VectorSet>();
  testIterDeductionGuide<F14FastSet>();
}

template <template <class...> class TSet>
void testInitializerListDeductionGuide() {
  TSet<int> source({1, 2});

  TSet dest1({1, 2}, 2);
  static_assert(std::is_same_v<decltype(dest1), decltype(source)>);
  EXPECT_EQ(dest1, source);

  TSet dest2({1, 2}, 2, f14::DefaultHasher<int>{});
  static_assert(std::is_same_v<decltype(dest2), decltype(source)>);
  EXPECT_EQ(dest2, source);

  TSet dest3({1, 2}, 2, f14::DefaultHasher<int>{}, f14::DefaultKeyEqual<int>{});
  static_assert(std::is_same_v<decltype(dest3), decltype(source)>);
  EXPECT_EQ(dest3, source);

  TSet dest4(
      {1, 2},
      2,
      f14::DefaultHasher<int>{},
      f14::DefaultKeyEqual<int>{},
      f14::DefaultAlloc<int>{});
  static_assert(std::is_same_v<decltype(dest4), decltype(source)>);
  EXPECT_EQ(dest4, source);

  TSet dest5({1, 2}, 2, f14::DefaultAlloc<int>{});
  static_assert(std::is_same_v<decltype(dest5), decltype(source)>);
  EXPECT_EQ(dest5, source);

  TSet dest6({1, 2}, 2, f14::DefaultHasher<int>{}, f14::DefaultAlloc<int>{});
  static_assert(std::is_same_v<decltype(dest6), decltype(source)>);
  EXPECT_EQ(dest6, source);
}

TEST(F14Set, initializerListDeductionGuide) {
  testInitializerListDeductionGuide<F14ValueSet>();
  testInitializerListDeductionGuide<F14NodeSet>();
  testInitializerListDeductionGuide<F14VectorSet>();
  testInitializerListDeductionGuide<F14FastSet>();
}

template <template <typename> class TSet>
void testInsertRange() {
  using Set = TSet<int>;
  Set dest{0, 1};
  const Set source{1, 2};
  const Set expected{0, 1, 2};

  dest.insert_range(source);
  EXPECT_EQ(expected, dest);

  dest.insert_range(source);
  EXPECT_EQ(expected, dest);
}

TEST(F14Set, insertRange) {
  testInsertRange<F14ValueSet>();
  testInsertRange<F14NodeSet>();
  testInsertRange<F14VectorSet>();
  testInsertRange<F14FastSet>();
}
