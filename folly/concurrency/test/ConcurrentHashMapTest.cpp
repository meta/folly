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

#include <folly/concurrency/ConcurrentHashMap.h>

#include <atomic>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

#include <folly/Traits.h>
#include <folly/container/test/TrackingTypes.h>
#include <folly/hash/Hash.h>
#include <folly/portability/GFlags.h>
#include <folly/portability/GTest.h>
#include <folly/synchronization/Latch.h>
#include <folly/test/DeterministicSchedule.h>

using namespace folly::test;
using namespace folly;
using namespace std;

DEFINE_int64(seed, 0, "Seed for random number generators");

template <typename T>
class ConcurrentHashMapTest : public ::testing::Test {};
TYPED_TEST_SUITE_P(ConcurrentHashMapTest);

template <template <
    typename,
    typename,
    uint8_t,
    typename,
    typename,
    typename,
    template <typename> class,
    class> class Impl>
struct MapFactory {
  template <
      typename KeyType,
      typename ValueType,
      typename HashFn = std::hash<KeyType>,
      typename KeyEqual = std::equal_to<KeyType>,
      typename Allocator = std::allocator<uint8_t>,
      uint8_t ShardBits = 8,
      template <typename> class Atom = std::atomic,
      class Mutex = std::mutex>
  using MapT = ConcurrentHashMap<
      KeyType,
      ValueType,
      HashFn,
      KeyEqual,
      Allocator,
      ShardBits,
      Atom,
      Mutex,
      Impl>;
};

#define CHM typename TypeParam::template MapT

TYPED_TEST_P(ConcurrentHashMapTest, MapTest) {
  CHM<uint64_t, uint64_t> foomap(3);
  EXPECT_TRUE(foomap.empty());
  EXPECT_EQ(foomap.find(1), foomap.cend());
  auto r = foomap.insert(1, 0);
  EXPECT_TRUE(r.second);
  auto r2 = foomap.insert(1, 0);
  EXPECT_EQ(r.first->second, 0);
  EXPECT_EQ(r.first->first, 1);
  EXPECT_EQ(r2.first->second, 0);
  EXPECT_EQ(r2.first->first, 1);
  EXPECT_EQ(r.first, r2.first);
  EXPECT_TRUE(r.second);
  EXPECT_FALSE(r2.second);
  EXPECT_FALSE(foomap.empty());
  EXPECT_TRUE(foomap.insert(std::make_pair(2, 0)).second);
  EXPECT_TRUE(foomap.insert_or_assign(2, 0).second);
  EXPECT_EQ(foomap.size(), 2);
  EXPECT_TRUE(foomap.assign_if_equal(2, 0, 3));
  EXPECT_FALSE(foomap.assign_if(2, 4, [](auto&&) { return false; }));
  EXPECT_TRUE(foomap.insert(3, 0).second);
  EXPECT_FALSE(foomap.erase_if_equal(3, 1));
  EXPECT_TRUE(foomap.erase_if_equal(3, 0));
  EXPECT_TRUE(foomap.insert(3, 0).second);
  EXPECT_NE(foomap.find(1), foomap.cend());
  EXPECT_NE(foomap.find(2), foomap.cend());
  EXPECT_EQ(foomap.find(2)->second, 3);
  EXPECT_EQ(foomap[2], 3);
  EXPECT_EQ(foomap[20], 0);
  EXPECT_EQ(foomap.at(20), 0);
  EXPECT_FALSE(foomap.insert(1, 0).second);
  auto l = foomap.find(1);
  foomap.erase(l);
  EXPECT_FALSE(foomap.erase(1));
  EXPECT_EQ(foomap.find(1), foomap.cend());
  auto res = foomap.find(2);
  EXPECT_NE(res, foomap.cend());
  EXPECT_EQ(3, res->second);
  EXPECT_FALSE(foomap.empty());
  foomap.clear();
  EXPECT_TRUE(foomap.empty());
  EXPECT_TRUE(foomap.insert(3, 0).second);
  EXPECT_FALSE(foomap.empty());
  foomap.erase(3);
  EXPECT_TRUE(foomap.empty());
}

TYPED_TEST_P(ConcurrentHashMapTest, MaxSizeTest) {
  CHM<uint64_t, uint64_t> foomap(2, 16);
  bool insert_failed = false;
  for (int i = 0; i < 32; i++) {
    auto res = foomap.insert(0, 0);
    if (!res.second) {
      insert_failed = true;
    }
  }
  EXPECT_TRUE(insert_failed);
}

// nextPowTwo(9) == 16 and nextPowTwo(8) == 8, so a naive independent rounding
// of size and max_size would make max_size_ (8) end up smaller than size_
// (16). max_size is a lower bound like size, so it should be raised to match
// rather than left inconsistent -- verified behaviorally here, since neither
// is directly queryable: the map must still be able to hold up to 16
// elements (matching the rounded size, not the smaller un-clamped max_size)
// before insert starts failing.
TYPED_TEST_P(ConcurrentHashMapTest, MaxSizeBelowSizeIsClampedTest) {
  CHM<uint64_t, uint64_t> foomap(9, 8);
  for (uint64_t i = 0; i < 16; i++) {
    EXPECT_TRUE(foomap.insert(i, 0).second);
  }
}

TYPED_TEST_P(ConcurrentHashMapTest, MoveTest) {
  CHM<uint64_t, uint64_t> foomap(2, 16);
  auto other = std::move(foomap);
  auto other2 = std::move(other);
  other = std::move(other2);
}

struct foo {
  static int moved;
  static int copied;
  foo(foo&&) noexcept { moved++; }
  foo& operator=(foo&&) {
    moved++;
    return *this;
  }
  foo& operator=(const foo&) {
    copied++;
    return *this;
  }
  foo(const foo&) { copied++; }
  foo() {}
};
int foo::moved{0};
int foo::copied{0};

TYPED_TEST_P(ConcurrentHashMapTest, EmplaceTest) {
  CHM<uint64_t, foo> foomap(200);
  foo bar; // Make sure to test copy
  foomap.insert(1, bar);
  EXPECT_EQ(foo::moved, 0);
  EXPECT_EQ(foo::copied, 1);
  foo::copied = 0;
  // The difference between emplace and try_emplace:
  // If insertion fails, try_emplace does not move its argument
  foomap.try_emplace(1, foo());
  EXPECT_EQ(foo::moved, 0);
  EXPECT_EQ(foo::copied, 0);
  foomap.emplace(1, foo());
  EXPECT_EQ(foo::moved, 1);
  EXPECT_EQ(foo::copied, 0);
  // Reset the counters. Repeated tests are allowed
  foo::moved = 0;
  foo::copied = 0;
}

TYPED_TEST_P(ConcurrentHashMapTest, MapInsertIteratorValueTest) {
  CHM<uint64_t, uint64_t> foomap(2);
  for (uint64_t i = 0; i < 1 << 16; i++) {
    auto ret = foomap.insert(i, i + 1);
    EXPECT_TRUE(ret.second);
    EXPECT_EQ(ret.first->second, i + 1);
  }
}

TYPED_TEST_P(ConcurrentHashMapTest, MapResizeTest) {
  CHM<uint64_t, uint64_t> foomap(2);
  EXPECT_EQ(foomap.find(1), foomap.cend());
  EXPECT_TRUE(foomap.insert(1, 0).second);
  EXPECT_TRUE(foomap.insert(2, 0).second);
  EXPECT_TRUE(foomap.insert(3, 0).second);
  EXPECT_TRUE(foomap.insert(4, 0).second);
  foomap.reserve(512);
  EXPECT_NE(foomap.find(1), foomap.cend());
  EXPECT_NE(foomap.find(2), foomap.cend());
  EXPECT_FALSE(foomap.insert(1, 0).second);
  EXPECT_TRUE(foomap.erase(1));
  EXPECT_EQ(foomap.find(1), foomap.cend());
  auto res = foomap.find(2);
  EXPECT_NE(res, foomap.cend());
  if (res != foomap.cend()) {
    EXPECT_EQ(0, res->second);
  }
  foomap.reserve(0);
}

TYPED_TEST_P(ConcurrentHashMapTest, ReserveTest) {
  CHM<uint64_t, uint64_t> foomap;
  int64_t insert_count = 0;
  int64_t actual_count = 0;

  for (int64_t i = 0; i < 1000; i++) {
    if (i % 100 == 0) {
      foomap.reserve(i + 100);
    }
    foomap.insert(std::make_pair(i, i));
    insert_count++;
  }

  for (auto it = foomap.begin(); it != foomap.cend(); ++it) {
    actual_count++;
  }

  EXPECT_EQ(insert_count, actual_count);
}

// Ensure we can insert objects without copy constructors.
TYPED_TEST_P(ConcurrentHashMapTest, MapNoCopiesTest) {
  struct Uncopyable {
    int i_;
    Uncopyable(int i) { i_ = i; }
    Uncopyable(const Uncopyable& that) = delete;
    bool operator==(const Uncopyable& o) const { return i_ == o.i_; }
  };
  struct Hasher {
    size_t operator()(const Uncopyable&) const { return 0; }
  };
  CHM<Uncopyable, Uncopyable, Hasher> foomap(2);
  EXPECT_TRUE(foomap.try_emplace(1, 1).second);
  EXPECT_TRUE(foomap.try_emplace(2, 2).second);
  auto res = foomap.find(2);
  EXPECT_NE(res, foomap.cend());

  EXPECT_TRUE(foomap.try_emplace(3, 3).second);

  auto res2 = foomap.find(2);
  EXPECT_NE(res2, foomap.cend());
  EXPECT_EQ(&(res->second), &(res2->second));
}

TYPED_TEST_P(ConcurrentHashMapTest, MapMovableKeysTest) {
  struct Movable {
    int i_;
    Movable(int i) { i_ = i; }
    Movable(const Movable&) = delete;
    Movable(Movable&& o) {
      i_ = o.i_;
      o.i_ = 0;
    }
    bool operator==(const Movable& o) const { return i_ == o.i_; }
  };
  struct Hasher {
    size_t operator()(const Movable&) const { return 0; }
  };
  CHM<Movable, Movable, Hasher> foomap(2);
  EXPECT_TRUE(foomap.insert(std::make_pair(Movable(10), Movable(1))).second);
  EXPECT_TRUE(foomap.assign(Movable(10), Movable(2)));
  EXPECT_TRUE(foomap.insert(Movable(11), Movable(1)).second);
  EXPECT_TRUE(foomap.emplace(Movable(12), Movable(1)).second);
  EXPECT_TRUE(foomap.insert_or_assign(Movable(10), Movable(3)).second);
  EXPECT_TRUE(foomap.assign_if_equal(Movable(10), Movable(3), Movable(4)));
  EXPECT_TRUE(foomap.assign_if(Movable(10), Movable(5), [](auto&&) {
    return true;
  }));
  EXPECT_FALSE(foomap.try_emplace(Movable(10), Movable(3)).second);
  EXPECT_TRUE(foomap.try_emplace(Movable(13), Movable(3)).second);
}

TYPED_TEST_P(ConcurrentHashMapTest, MapUpdateTest) {
  CHM<uint64_t, uint64_t> foomap(2);
  EXPECT_TRUE(foomap.insert(1, 10).second);
  EXPECT_TRUE(bool(foomap.assign(1, 11)));
  auto res = foomap.find(1);
  EXPECT_NE(res, foomap.cend());
  EXPECT_EQ(11, res->second);
}

TYPED_TEST_P(ConcurrentHashMapTest, MapIterateTest2) {
  CHM<uint64_t, uint64_t> foomap(2);
  auto begin = foomap.cbegin();
  auto end = foomap.cend();
  EXPECT_EQ(begin, end);
}

TYPED_TEST_P(ConcurrentHashMapTest, MapIterateTest) {
  CHM<uint64_t, uint64_t> foomap(2);
  EXPECT_EQ(foomap.cbegin(), foomap.cend());
  EXPECT_TRUE(foomap.insert(1, 1).second);
  EXPECT_TRUE(foomap.insert(2, 2).second);
  auto iter = foomap.cbegin();
  EXPECT_NE(iter, foomap.cend());
  EXPECT_EQ(iter->first, 1);
  EXPECT_EQ(iter->second, 1);
  ++iter;
  EXPECT_NE(iter, foomap.cend());
  EXPECT_EQ(iter->first, 2);
  EXPECT_EQ(iter->second, 2);
  ++iter;
  EXPECT_EQ(iter, foomap.cend());

  int count = 0;
  for (auto it = foomap.cbegin(); it != foomap.cend(); ++it) {
    count++;
  }
  EXPECT_EQ(count, 2);
}

TYPED_TEST_P(ConcurrentHashMapTest, MoveIterateAssignIterate) {
  using Map = CHM<int, int>;
  Map tmp;
  Map map{std::move(tmp)};

  map.insert(0, 0);
  ++map.cbegin();
  CHM<int, int> other;
  other.insert(0, 0);
  map = std::move(other);
  ++map.cbegin();
}

TYPED_TEST_P(ConcurrentHashMapTest, EraseTest) {
  CHM<uint64_t, uint64_t> foomap(3);
  foomap.insert(1, 0);
  auto f1 = foomap.find(1);
  EXPECT_EQ(1, foomap.erase(1));
  foomap.erase(f1);
}

TYPED_TEST_P(ConcurrentHashMapTest, EraseIfEqualTest) {
  CHM<uint64_t, uint64_t> foomap(3);
  foomap.insert(1, 0);
  EXPECT_FALSE(foomap.erase_if_equal(1, 1));
  auto f1 = foomap.find(1);
  EXPECT_EQ(0, f1->second);
  EXPECT_TRUE(foomap.erase_if_equal(1, 0));
  EXPECT_EQ(foomap.find(1), foomap.cend());
}

TYPED_TEST_P(ConcurrentHashMapTest, EraseIfTest) {
  CHM<uint64_t, uint64_t> foomap(3);
  foomap.insert(1, 0);
  EXPECT_FALSE(foomap.erase_key_if(1, [](const uint64_t& value) {
    return value == 1;
  }));
  auto f1 = foomap.find(1);
  EXPECT_EQ(0, f1->second);
  EXPECT_TRUE(foomap.erase_key_if(1, [](const uint64_t& value) {
    return value == 0;
  }));
  EXPECT_EQ(foomap.find(1), foomap.cend());

  CHM<std::string, std::weak_ptr<uint64_t>> barmap(3);
  auto shared = std::make_shared<uint64_t>(123);
  barmap.insert("test", shared);
  EXPECT_FALSE(barmap.erase_key_if(
      "test",
      [](const std::weak_ptr<uint64_t>& ptr) { return ptr.expired(); }));
  EXPECT_EQ(*barmap.find("test")->second.lock(), 123);
  shared.reset();
  EXPECT_TRUE(barmap.erase_key_if(
      "test",
      [](const std::weak_ptr<uint64_t>& ptr) { return ptr.expired(); }));
  EXPECT_EQ(barmap.find("test"), barmap.cend());
}

TYPED_TEST_P(ConcurrentHashMapTest, CopyIterator) {
  CHM<int, int> map;
  map.insert(0, 0);
  for (auto cit = map.cbegin(); cit != map.cend(); ++cit) {
    std::pair<int const, int> const ckv{0, 0};
    EXPECT_EQ(*cit, ckv);
  }
}

TYPED_TEST_P(ConcurrentHashMapTest, EraseInIterateTest) {
  CHM<uint64_t, uint64_t> foomap(3);
  for (uint64_t k = 0; k < 10; ++k) {
    foomap.insert(k, k);
  }
  EXPECT_EQ(10, foomap.size());
  for (auto it = foomap.cbegin(); it != foomap.cend();) {
    if (it->second > 3) {
      it = foomap.erase(it);
    } else {
      ++it;
    }
  }
  EXPECT_EQ(4, foomap.size());
  for (auto it = foomap.cbegin(); it != foomap.cend(); ++it) {
    EXPECT_GE(3, it->second);
  }
}

TYPED_TEST_P(ConcurrentHashMapTest, AssignIfTest) {
  CHM<uint64_t, uint64_t> foomap(3);
  foomap.insert(1, 0);

  bool canAssignFlag = false;
  EXPECT_FALSE(
      foomap.assign_if(1, 1, [canAssignFlag](auto&&) { return canAssignFlag; })
          .has_value());

  canAssignFlag = true;
  auto f1 = foomap.assign_if(1, 2, [canAssignFlag](auto&&) {
    return canAssignFlag;
  });
  EXPECT_TRUE(f1.has_value());
  EXPECT_EQ(2, f1.value()->second);

  // Assign based on the current value.
  auto f2 = foomap.assign_if(1, 3, [](auto&& val) { return val == 2; });
  EXPECT_TRUE(f2.has_value());
  EXPECT_EQ(3, f2.value()->second);
}

TYPED_TEST_P(ConcurrentHashMapTest, InsertOrAssignIfTest) {
  CHM<uint64_t, uint64_t> foomap(3);

  bool canAssignFlag = false;
  auto r = foomap.insert_or_assign_if(0, 0, [canAssignFlag](auto&&) {
    return canAssignFlag;
  });
  EXPECT_TRUE(r.second);
  EXPECT_EQ(0, r.first->second);
  EXPECT_EQ(0, foomap.find(0)->second);

  canAssignFlag = true;
  r = foomap.insert_or_assign_if(1, 0, [canAssignFlag](auto&&) {
    return canAssignFlag;
  });
  EXPECT_TRUE(r.second);
  EXPECT_EQ(0, r.first->second);
  EXPECT_EQ(0, foomap.find(1)->second);

  // assign_if test equivalent
  canAssignFlag = false;
  r = foomap.insert_or_assign_if(1, 1, [canAssignFlag](auto&&) {
    return canAssignFlag;
  });
  EXPECT_FALSE(r.second);
  EXPECT_EQ(0, r.first->second);
  EXPECT_EQ(0, foomap.find(1)->second);

  canAssignFlag = true;
  r = foomap.insert_or_assign_if(1, 2, [canAssignFlag](auto&&) {
    return canAssignFlag;
  });
  EXPECT_TRUE(r.second);
  EXPECT_EQ(2, r.first->second);
  EXPECT_EQ(2, foomap.find(1)->second);

  // Assign based on the current value.
  r = foomap.insert_or_assign_if(1, 3, [](auto&& val) { return val == 2; });
  EXPECT_TRUE(r.second);
  EXPECT_EQ(3, r.first->second);
  EXPECT_EQ(3, foomap.find(1)->second);

  r = foomap.insert_or_assign_if(1, 4, [](auto&& val) { return val == 2; });
  EXPECT_FALSE(r.second);
  EXPECT_EQ(3, r.first->second);
  EXPECT_EQ(3, foomap.find(1)->second);
}

// TODO: hazptrs must support DeterministicSchedule

#define Atom std::atomic // DeterministicAtomic
#define Mutex std::mutex // DeterministicMutex
#define lib std // DeterministicSchedule
#define join t.join() // DeterministicSchedule::join(t)
// #define Atom DeterministicAtomic
// #define Mutex DeterministicMutex
// #define lib DeterministicSchedule
// #define join DeterministicSchedule::join(t)

TYPED_TEST_P(ConcurrentHashMapTest, UpdateStressTest) {
  DeterministicSchedule sched(DeterministicSchedule::uniform(FLAGS_seed));

  // size must match iters for this test.
  unsigned size = 128 * 128;
  unsigned iters = size;
  CHM<unsigned long,
      unsigned long,
      std::hash<unsigned long>,
      std::equal_to<unsigned long>,
      std::allocator<uint8_t>,
      8,
      Atom,
      Mutex>
      m(2);

  for (uint32_t i = 0; i < size; i++) {
    m.insert(i, i);
  }
  std::vector<std::thread> threads;
  unsigned int num_threads = 32;
  threads.reserve(num_threads);
  for (uint32_t t = 0; t < num_threads; t++) {
    threads.push_back(lib::thread([&, t]() {
      int offset = (iters * t / num_threads);
      for (uint32_t i = 0; i < iters / num_threads; i++) {
        unsigned long k = folly::hash::jenkins_rev_mix32((i + offset));
        k = k % (iters / num_threads) + offset;
        unsigned long val = 3;
        {
          auto res = m.find(k);
          EXPECT_NE(res, m.cend());
          EXPECT_EQ(k, res->second);
          auto r = m.assign(k, res->second);
          EXPECT_TRUE(r);
        }
        {
          auto res = m.find(k);
          EXPECT_NE(res, m.cend());
          EXPECT_EQ(k, res->second);
        }
        // Another random insertion to force table resizes
        val = size + i + offset;
        EXPECT_TRUE(m.insert(val, val).second);
      }
    }));
  }
  for (auto& t : threads) {
    join;
  }
}

TYPED_TEST_P(ConcurrentHashMapTest, EraseStressTest) {
  DeterministicSchedule sched(DeterministicSchedule::uniform(FLAGS_seed));

  unsigned size = 2;
  unsigned iters = size * 128 * 2;
  CHM<unsigned long,
      unsigned long,
      std::hash<unsigned long>,
      std::equal_to<unsigned long>,
      std::allocator<uint8_t>,
      8,
      Atom,
      Mutex>
      m(2);

  for (uint32_t i = 0; i < size; i++) {
    unsigned long k = folly::hash::jenkins_rev_mix32(i);
    m.insert(k, k);
  }
  std::vector<std::thread> threads;
  unsigned int num_threads = 32;
  threads.reserve(num_threads);
  for (uint32_t t = 0; t < num_threads; t++) {
    threads.push_back(lib::thread([&, t]() {
      int offset = (iters * t / num_threads);
      for (uint32_t i = 0; i < iters / num_threads; i++) {
        unsigned long k = folly::hash::jenkins_rev_mix32((i + offset));
        auto res = m.insert(k, k).second;
        if (res) {
          if (i % 2 == 0) {
            res = m.erase(k);
          } else {
            res = m.erase_if_equal(k, k);
          }
          if (!res) {
            printf("Faulre to erase thread %i val %li\n", t, k);
            exit(0);
          }
          EXPECT_TRUE(res);
        }
        res = m.insert(k, k).second;
        if (res) {
          res = bool(m.assign(k, k));
          if (!res) {
            printf("Thread %i update fail %li res%i\n", t, k, res);
            exit(0);
          }
          EXPECT_TRUE(res);
          auto result = m.find(k);
          if (result == m.cend()) {
            printf("Thread %i lookup fail %li\n", t, k);
            exit(0);
          }
          EXPECT_EQ(k, result->second);
        }
      }
    }));
  }
  for (auto& t : threads) {
    join;
  }
}

TYPED_TEST_P(ConcurrentHashMapTest, TryEmplaceEraseStressTest) {
  DeterministicSchedule sched(DeterministicSchedule::uniform(FLAGS_seed));
  std::atomic<int> iterations{10000};
  std::vector<std::thread> threads;
  unsigned int num_threads = 32;
  threads.reserve(num_threads);
  CHM<int, int> map;
  for (uint32_t t = 0; t < num_threads; t++) {
    threads.push_back(lib::thread([&]() {
      while (--iterations >= 0) {
        auto it = map.try_emplace(1, 101);
        map.erase(1);
        EXPECT_EQ(it.first->second, 101);
      }
    }));
  }
  for (auto& t : threads) {
    join;
  }
}

TYPED_TEST_P(ConcurrentHashMapTest, InsertOrAssignStressTest) {
  DeterministicSchedule sched(DeterministicSchedule::uniform(FLAGS_seed));
  std::atomic<int> iterations{10000};
  std::vector<std::thread> threads;
  unsigned int num_threads = 32;
  threads.reserve(num_threads);
  CHM<int, int> map;
  for (uint32_t t = 0; t < num_threads; t++) {
    threads.push_back(lib::thread([&]() {
      int i = 0;
      while (--iterations >= 0) {
        auto res = map.insert_or_assign(0, ++i);
        ASSERT_TRUE(res.second);
        auto v = res.first->second;
        ASSERT_EQ(v, i);
      }
    }));
  }
  for (auto& t : threads) {
    join;
  }
}

TYPED_TEST_P(ConcurrentHashMapTest, IterateStressTest) {
  DeterministicSchedule sched(DeterministicSchedule::uniform(FLAGS_seed));

  unsigned size = 2;
  unsigned iters = size * 128 * 2;
  CHM<unsigned long,
      unsigned long,
      std::hash<unsigned long>,
      std::equal_to<unsigned long>,
      std::allocator<uint8_t>,
      8,
      Atom,
      Mutex>
      m(2);

  for (uint32_t i = 0; i < size; i++) {
    unsigned long k = folly::hash::jenkins_rev_mix32(i);
    m.insert(k, k);
  }
  for (uint32_t i = 0; i < 10; i++) {
    m.insert(i, i);
  }
  std::vector<std::thread> threads;
  unsigned int num_threads = 32;
  for (uint32_t t = 0; t < num_threads; t++) {
    threads.push_back(lib::thread([&, t]() {
      int offset = (iters * t / num_threads);
      for (uint32_t i = 0; i < iters / num_threads; i++) {
        unsigned long k = folly::hash::jenkins_rev_mix32((i + offset));
        auto res = m.insert(k, k).second;
        if (res) {
          if (i % 2 == 0) {
            res = m.erase(k);
          } else {
            res = m.erase_if_equal(k, k);
          }
          EXPECT_TRUE(res);
        }
        int count = 0;
        for (auto it = m.cbegin(); it != m.cend(); ++it) {
          if (it->first < 10) {
            count++;
          }
        }
        EXPECT_EQ(count, 10);
      }
    }));
  }
  for (auto& t : threads) {
    join;
  }
}

TYPED_TEST_P(ConcurrentHashMapTest, insertStressTest) {
  DeterministicSchedule sched(DeterministicSchedule::uniform(FLAGS_seed));

  unsigned size = 2;
  unsigned iters = size * 64 * 4;
  CHM<unsigned long,
      unsigned long,
      std::hash<unsigned long>,
      std::equal_to<unsigned long>,
      std::allocator<uint8_t>,
      8,
      Atom,
      Mutex>
      m(2);

  EXPECT_TRUE(m.insert(0, 0).second);
  EXPECT_FALSE(m.insert(0, 0).second);
  std::vector<std::thread> threads;
  unsigned int num_threads = 32;
  for (uint32_t t = 0; t < num_threads; t++) {
    threads.push_back(lib::thread([&, t]() {
      int offset = (iters * t / num_threads);
      for (uint32_t i = 0; i < iters / num_threads; i++) {
        auto var = offset + i + 1;
        EXPECT_TRUE(m.insert(var, var).second);
        EXPECT_FALSE(m.insert(0, 0).second);
      }
    }));
  }
  for (auto& t : threads) {
    join;
  }
}

TYPED_TEST_P(ConcurrentHashMapTest, assignStressTest) {
  DeterministicSchedule sched(DeterministicSchedule::uniform(FLAGS_seed));

  unsigned size = 2;
  unsigned iters = size * 64 * 4;
  struct big_value {
    uint64_t v1;
    uint64_t v2;
    uint64_t v3;
    uint64_t v4;
    uint64_t v5;
    uint64_t v6;
    uint64_t v7;
    uint64_t v8;
    void set(uint64_t v) { v1 = v2 = v3 = v4 = v5 = v6 = v7 = v8 = v; }
    void check() const {
      auto v = v1;
      EXPECT_EQ(v, v8);
      EXPECT_EQ(v, v7);
      EXPECT_EQ(v, v6);
      EXPECT_EQ(v, v5);
      EXPECT_EQ(v, v4);
      EXPECT_EQ(v, v3);
      EXPECT_EQ(v, v2);
    }
  };
  CHM<unsigned long,
      big_value,
      std::hash<unsigned long>,
      std::equal_to<unsigned long>,
      std::allocator<uint8_t>,
      8,
      Atom,
      Mutex>
      m(2);

  for (uint32_t i = 0; i < iters; i++) {
    big_value a;
    a.set(i);
    m.insert(i, a);
  }

  std::vector<std::thread> threads;
  unsigned int num_threads = 32;
  for (uint32_t t = 0; t < num_threads; t++) {
    threads.push_back(lib::thread([&]() {
      for (uint32_t i = 0; i < iters; i++) {
        auto res = m.find(i);
        EXPECT_NE(res, m.cend());
        res->second.check();
        big_value b;
        b.set(res->second.v1 + 1);
        m.assign(i, b);
      }
    }));
  }
  for (auto& t : threads) {
    join;
  }
}

TYPED_TEST_P(ConcurrentHashMapTest, RefcountTest) {
  struct badhash {
    size_t operator()(uint64_t) const { return 0; }
  };
  CHM<uint64_t,
      uint64_t,
      badhash,
      std::equal_to<uint64_t>,
      std::allocator<uint8_t>,
      0>
      foomap(3);
  foomap.insert(0, 0);
  foomap.insert(1, 1);
  foomap.insert(2, 2);
  for (int32_t i = 0; i < 300; ++i) {
    foomap.insert_or_assign(1, i);
  }
}

struct Wrapper {
  explicit Wrapper(bool& del_) : del(del_) {}
  ~Wrapper() { del = true; }

  bool& del;
};

TYPED_TEST_P(ConcurrentHashMapTest, Deletion) {
  bool del{false};

  {
    CHM<int, std::shared_ptr<Wrapper>> map;

    map.insert(0, std::make_shared<Wrapper>(del));
  }

  folly::hazptr_cleanup();

  EXPECT_TRUE(del);
}

TYPED_TEST_P(ConcurrentHashMapTest, DeletionWithErase) {
  bool del{false};

  {
    CHM<int, std::shared_ptr<Wrapper>> map;

    map.insert(0, std::make_shared<Wrapper>(del));
    map.erase(0);
  }

  folly::hazptr_cleanup();

  EXPECT_TRUE(del);
}

TYPED_TEST_P(ConcurrentHashMapTest, DeletionWithIterator) {
  bool del{false};

  {
    CHM<int, std::shared_ptr<Wrapper>> map;

    map.insert(0, std::make_shared<Wrapper>(del));
    auto it = map.find(0);
    map.erase(it);
  }

  folly::hazptr_cleanup();

  EXPECT_TRUE(del);
}

TYPED_TEST_P(ConcurrentHashMapTest, DeletionWithForLoop) {
  bool del{false};

  {
    CHM<int, std::shared_ptr<Wrapper>> map;

    map.insert(0, std::make_shared<Wrapper>(del));
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
      EXPECT_EQ(it->first, 0);
    }
  }

  folly::hazptr_cleanup();

  EXPECT_TRUE(del);
}

TYPED_TEST_P(ConcurrentHashMapTest, DeletionMultiple) {
  bool del1{false}, del2{false};

  {
    CHM<int, std::shared_ptr<Wrapper>> map;

    map.insert(0, std::make_shared<Wrapper>(del1));
    map.insert(1, std::make_shared<Wrapper>(del2));
  }

  folly::hazptr_cleanup();

  EXPECT_TRUE(del1);
  EXPECT_TRUE(del2);
}

TYPED_TEST_P(ConcurrentHashMapTest, DeletionAssigned) {
  bool del1{false}, del2{false};

  {
    CHM<int, std::shared_ptr<Wrapper>> map;

    map.insert(0, std::make_shared<Wrapper>(del1));
    map.insert_or_assign(0, std::make_shared<Wrapper>(del2));
  }

  folly::hazptr_cleanup();

  EXPECT_TRUE(del1);
  EXPECT_TRUE(del2);
}

TYPED_TEST_P(ConcurrentHashMapTest, DeletionMultipleMaps) {
  bool del1{false}, del2{false};

  {
    CHM<int, std::shared_ptr<Wrapper>> map1;
    CHM<int, std::shared_ptr<Wrapper>> map2;

    map1.insert(0, std::make_shared<Wrapper>(del1));
    map2.insert(0, std::make_shared<Wrapper>(del2));
  }

  folly::hazptr_cleanup();

  EXPECT_TRUE(del1);
  EXPECT_TRUE(del2);
}

TYPED_TEST_P(ConcurrentHashMapTest, ForEachLoop) {
  CHM<int, int> map;
  map.insert(1, 2);
  size_t iters = 0;
  for (const auto& kv : map) {
    EXPECT_EQ(kv.first, 1);
    EXPECT_EQ(kv.second, 2);
    ++iters;
  }
  EXPECT_EQ(iters, 1);
}

template <typename T>
struct FooBase {
  typename T::ConstIterator it;
  explicit FooBase(typename T::ConstIterator&& it_) : it(std::move(it_)) {}
  FooBase(FooBase&&) = default;
  FooBase& operator=(FooBase&&) = default;
};

TYPED_TEST_P(ConcurrentHashMapTest, IteratorMove) {
  using Foo = FooBase<CHM<int, int>>;
  CHM<int, int> map;
  int k = 111;
  int v = 999999;
  map.insert(k, v);
  Foo foo(map.find(k));
  ASSERT_EQ(foo.it->second, v);
  Foo foo2(map.find(0));
  foo2 = std::move(foo);
  ASSERT_EQ(foo2.it->second, v);
}

TYPED_TEST_P(ConcurrentHashMapTest, IteratorLoop) {
  CHM<std::string, int> map;
  static constexpr size_t kNum = 4000;
  for (size_t i = 0; i < kNum; ++i) {
    map.insert(to<std::string>(i), i);
  }

  size_t count = 0;
  for (auto it = map.begin(); it != map.end(); ++it) {
    ASSERT_LT(count, kNum);
    ++count;
  }

  EXPECT_EQ(count, kNum);
}

namespace {
template <typename T, typename Arg>
using detector_find = decltype(std::declval<T>().find(std::declval<Arg>()));

template <typename T, typename Arg>
using detector_erase = decltype(std::declval<T>().erase(std::declval<Arg>()));
} // namespace

TYPED_TEST_P(ConcurrentHashMapTest, HeterogeneousLookup) {
  using Hasher = folly::transparent<folly::hasher<folly::StringPiece>>;
  using KeyEqual = folly::transparent<std::equal_to<folly::StringPiece>>;
  using M = CHM<std::string, bool, Hasher, KeyEqual>;

  constexpr auto hello = "hello"_sp;
  constexpr auto buddy = "buddy"_sp;
  constexpr auto world = "world"_sp;

  M map;
  map.emplace(hello, true);
  map.emplace(world, false);

  auto checks = [hello, buddy](auto& ref) {
    // find
    EXPECT_TRUE(ref.end() == ref.find(buddy));
    EXPECT_EQ(hello, ref.find(hello)->first);

    // at
    EXPECT_TRUE(ref.at(hello));
    EXPECT_THROW(ref.at(buddy), std::out_of_range);

    // invocability checks
    static_assert(
        !is_detected_v<detector_find, decltype(ref), int>,
        "there shouldn't be a find() overload for this string map with an int param");
  };

  checks(map);
  checks(std::as_const(map));
}

TYPED_TEST_P(ConcurrentHashMapTest, ExplicitHeterogeneousTemplateArguments) {
  using Hasher = folly::transparent<folly::hasher<folly::StringPiece>>;
  using KeyEqual = folly::transparent<std::equal_to<folly::StringPiece>>;
  CHM<std::string, bool, Hasher, KeyEqual> map;
  constexpr auto key = "hello"_sp;

  auto inserted = map.template insert<StringPiece, 0>(
      std::pair<StringPiece, bool>{key, true});
  EXPECT_TRUE(inserted.second);
  EXPECT_EQ(key, inserted.first->first);
  auto found = std::as_const(map).template find<StringPiece, 0>(key);
  ASSERT_NE(found, map.end());
  EXPECT_TRUE(found->second);
  EXPECT_TRUE((std::as_const(map).template at<StringPiece, 0>(key)));
  EXPECT_TRUE((map.template operator[]<StringPiece, 0>(key)));

  EXPECT_EQ(0, (map.template erase_if_equal<StringPiece, 0>(key, false)));
  EXPECT_EQ(1, (map.template erase_if_equal<StringPiece, 0>(key, true)));
  map.emplace(key, true);
  EXPECT_EQ(1, (map.template erase<StringPiece, 0>(key)));
  map.emplace(key, true);
  auto predicate = [](bool value) { return value; };
  EXPECT_EQ(
      1,
      (map.template erase_key_if<StringPiece, decltype(predicate), 0>(
          key, std::move(predicate))));
  EXPECT_TRUE(map.empty());
}

TYPED_TEST_P(ConcurrentHashMapTest, HeterogeneousInsert) {
  using Hasher = folly::transparent<folly::hasher<folly::StringPiece>>;
  using KeyEqual = folly::transparent<std::equal_to<folly::StringPiece>>;
  using P = std::pair<StringPiece, std::string>;
  using CP = std::pair<const StringPiece, std::string>;

  CHM<std::string, std::string, Hasher, KeyEqual> map;
  P p{"foo", "hello"};
  StringPiece foo{"foo"};
  StringPiece bar{"bar"};

  map.insert("foo", "hello");
  map.insert(foo, "hello");
  // TODO(T31574848): the list-initialization below does not work on libstdc++
  // versions (e.g., GCC < 6) with no implementation of N4387 ("perfect
  // initialization" for pairs and tuples).
  //   StringPiece sp{"foo"};
  //   map.insert({sp, "hello"});
  map.insert({"foo", "hello"});
  map.insert(P("foo", "hello"));
  map.insert(CP("foo", "hello"));
  map.insert(std::move(p));
  map.insert_or_assign("foo", "hello");
  map.insert_or_assign(StringPiece{"foo"}, "hello");

  map.erase(StringPiece{"foo"});
  map.erase(foo);
  map.erase("");
  EXPECT_TRUE(map.empty());

  map.insert("foo", "hello");
  map.insert("bar", "world");
  map.erase_if_equal(StringPiece{"foo"}, "hello");
  map.erase_key_if(bar, [](const std::string& s) { return s == "world"; });
  map.erase("");
  EXPECT_TRUE(map.empty());

  map.insert("foo", "baz");
  EXPECT_TRUE(map.assign(foo, "hello2"));
  auto mbIt = map.assign_if_equal("foo", "hello2", "hello");
  EXPECT_TRUE(mbIt);
  EXPECT_EQ(mbIt.value()->second, "hello");
  EXPECT_EQ(map[foo], "hello");
  auto mbIt2 = map.assign_if("foo", "hello2", [](auto&& val) {
    return val == "hello";
  });
  EXPECT_TRUE(mbIt);
  EXPECT_EQ(mbIt2.value()->second, "hello2");
  EXPECT_EQ(map[foo], "hello2");
  auto it = map.find(foo);
  map.erase(it);
  EXPECT_TRUE(map.empty());

  map.try_emplace(foo);
  map.try_emplace(foo, "hello");
  map.try_emplace(StringPiece{"foo"}, "hello");
  map.try_emplace(foo, "hello");
  map.try_emplace(foo);
  map.try_emplace("foo");
  map.try_emplace("foo", "hello");
  map.try_emplace("bar", /* count */ 20, 'x');
  EXPECT_EQ(map[bar], std::string(20, 'x'));

  map.emplace(StringPiece{"foo"}, "hello");
  map.emplace("foo", "hello");

  // invocability checks
  static_assert(
      !is_detected_v<detector_erase, decltype(map), int>,
      "there shouldn't be an erase() overload for this string map with an int param");
}

TYPED_TEST_P(ConcurrentHashMapTest, InsertOrAssignIterator) {
  CHM<int, int> map;
  auto [itr1, insert1] = map.insert_or_assign(1, 1);
  auto [itr2, insert2] = map.insert_or_assign(1, 2);
  auto itr3 = map.find(1);
  EXPECT_EQ(itr3->second, 2);
  EXPECT_EQ(itr2->second, 2);
}

TYPED_TEST_P(ConcurrentHashMapTest, EraseClonedNonCopyable) {
  // Using a non-copyable value type to use the node structure with an
  // extra level of indirection to key-value items.
  using Value = std::unique_ptr<int>;
  // [TODO] Fix the SIMD version to pass this test, then change the
  // map type to CHM.
  ConcurrentHashMap<int, Value> map;
  int cloned = 32; // The item that will end up being cloned.
  for (int i = 0; i < cloned; i++) {
    map.try_emplace(256 * i, std::make_unique<int>(0));
  }
  auto [iter, _] = map.try_emplace(256 * cloned, std::make_unique<int>(0));
  // Add more items to cause rehash that clones the item.
  int num = 10000;
  for (int i = cloned + 1; i < num; i++) {
    map.try_emplace(256 * i, std::make_unique<int>(0));
  }
  // Erase items to invoke hazard pointer asynchronous reclamation.
  for (int i = 0; i < num; i++) {
    map.erase(256 * i);
  }
  // The cloned node and the associated key-value item should still be
  // protected by iter from being reclaimed.
  EXPECT_EQ(iter->first, 256 * cloned);
}

TYPED_TEST_P(ConcurrentHashMapTest, ConcurrentInsertClear) {
  DeterministicSchedule sched(DeterministicSchedule::uniform(FLAGS_seed));

  /* 8192 and 8x / 9x multipliers are values that tend to reproduce
   * race condition (fixed by this change) more frequently.
   * Test keeps CHM size limited to chm_max_size and clear() if it exceeds.
   * Key space for CHM is limited to chm_max_key and exceeds max size by
   * small margin.
   * Test imitates serial traversal over key space by multiple threads,
   * triggering clear() by multiple threads at once.
   */
  constexpr unsigned long chm_base_size = 8192;
  constexpr unsigned long chm_max_size = chm_base_size * 8;
  constexpr unsigned long chm_max_key = chm_base_size * 9;
  CHM<unsigned long,
      unsigned long,
      std::hash<unsigned long>,
      std::equal_to<unsigned long>,
      std::allocator<uint8_t>,
      8,
      Atom,
      Mutex>
      m(chm_base_size);

  std::vector<std::thread> threads;
  /* 32 threads and 50k rounds are a compromise between trying to create
   * race conditions in insert()/clear(), and finishing test in reasonable
   * time */
  constexpr int load_divisor = folly::kIsSanitizeThread ? 4 : 1;
  constexpr size_t num_threads = 32 / load_divisor;
  constexpr size_t rounds_per_thread = 50000 / load_divisor;
  threads.reserve(num_threads);
  for (size_t t = 0; t < num_threads; t++) {
    threads.emplace_back([&, t]() {
      for (size_t i = 0; i < rounds_per_thread; ++i) {
        /* Code simulates traversal over whole key space by all threads
         * combined, with each thread contributing to semi-unique set of keys.
         * Each thread is assigned its own key sequence,
         * with thread_X traversing values X, 32+X, 32*2+X, 32*3+X, ...
         * 32*N+X mod chm_max_key.
         */
        const unsigned long k = (i * num_threads + t) % chm_max_key;
        if (m.size() >= chm_max_size) {
          m.clear();
        }
        m.insert_or_assign(k, k);
      }
    });
  }

  for (auto& t : threads) {
    join;
  }
}

struct Wrong : std::exception {
  char const* what() const noexcept { return "wrong!"; }
};
struct ValueMaybeThrow {
  int value = 0;
  explicit ValueMaybeThrow(int i) : value{i} {
    if (!i) {
      throw Wrong();
    }
  }
};

TYPED_TEST_P(ConcurrentHashMapTest, ValueMaybeThrowInsertSeparate) {
  CHM<int, ValueMaybeThrow> map;
  EXPECT_THROW(map.insert(0, 0), Wrong);
  EXPECT_FALSE(get_ptr(map, 0));
}

TYPED_TEST_P(ConcurrentHashMapTest, ValueMaybeThrowTryEmplaceFail) {
  CHM<int, ValueMaybeThrow> map;
  map.try_emplace(0, 1);
  map.try_emplace(0, 0);
  EXPECT_TRUE(get_ptr(map, 0));
  EXPECT_EQ(1, get_ptr(map, 0)->value);
}

TYPED_TEST_P(ConcurrentHashMapTest, ValueMaybeThrowTryEmplaceSucc) {
  CHM<int, ValueMaybeThrow> map;
  EXPECT_THROW(map.try_emplace(0, 0), Wrong);
  EXPECT_FALSE(get_ptr(map, 0));
}

TYPED_TEST_P(ConcurrentHashMapTest, ValueMaybeThrowEmplace) {
  CHM<int, ValueMaybeThrow> map;
  EXPECT_THROW(map.emplace(0, 0), Wrong);
  EXPECT_FALSE(get_ptr(map, 0));
}

TYPED_TEST_P(ConcurrentHashMapTest, ValueMaybeThrowInsertOrAssignInsert) {
  CHM<int, ValueMaybeThrow> map;
  EXPECT_THROW(map.insert_or_assign(0, 0), Wrong);
  EXPECT_FALSE(get_ptr(map, 0));
}

TYPED_TEST_P(ConcurrentHashMapTest, ValueMaybeThrowInsertOrAssignAssign) {
  CHM<int, ValueMaybeThrow> map;
  map.emplace(0, 1);
  EXPECT_THROW(map.insert_or_assign(0, 0), Wrong);
  ASSERT_TRUE(get_ptr(map, 0));
  EXPECT_EQ(1, get_ptr(map, 0)->value);
}

TYPED_TEST_P(ConcurrentHashMapTest, ValueMaybeThrowAssignAbsent) {
  CHM<int, ValueMaybeThrow> map;
  map.assign(0, 0);
  EXPECT_FALSE(get_ptr(map, 0));
}

TYPED_TEST_P(ConcurrentHashMapTest, ValueMaybeThrowAssignPresent) {
  CHM<int, ValueMaybeThrow> map;
  map.emplace(0, 1);
  EXPECT_THROW(map.assign(0, 0), Wrong);
  ASSERT_TRUE(get_ptr(map, 0));
  EXPECT_EQ(1, get_ptr(map, 0)->value);
}

REGISTER_TYPED_TEST_SUITE_P(
    ConcurrentHashMapTest,
    MapTest,
    MaxSizeTest,
    MaxSizeBelowSizeIsClampedTest,
    MoveTest,
    EmplaceTest,
    MapResizeTest,
    ReserveTest,
    MapNoCopiesTest,
    MapMovableKeysTest,
    MapUpdateTest,
    MapIterateTest2,
    MapIterateTest,
    MoveIterateAssignIterate,
    MapInsertIteratorValueTest,
    CopyIterator,
    Deletion,
    DeletionAssigned,
    DeletionMultiple,
    DeletionMultipleMaps,
    DeletionWithErase,
    DeletionWithForLoop,
    DeletionWithIterator,
    EraseIfEqualTest,
    EraseIfTest,
    EraseInIterateTest,
    AssignIfTest,
    InsertOrAssignIfTest,
    EraseStressTest,
    EraseTest,
    ForEachLoop,
    TryEmplaceEraseStressTest,
    InsertOrAssignStressTest,
    IterateStressTest,
    RefcountTest,
    UpdateStressTest,
    assignStressTest,
    insertStressTest,
    IteratorMove,
    IteratorLoop,
    HeterogeneousLookup,
    ExplicitHeterogeneousTemplateArguments,
    HeterogeneousInsert,
    InsertOrAssignIterator,
    EraseClonedNonCopyable,
    ConcurrentInsertClear,
    ValueMaybeThrowInsertSeparate,
    ValueMaybeThrowTryEmplaceFail,
    ValueMaybeThrowTryEmplaceSucc,
    ValueMaybeThrowEmplace,
    ValueMaybeThrowInsertOrAssignInsert,
    ValueMaybeThrowInsertOrAssignAssign,
    ValueMaybeThrowAssignAbsent,
    ValueMaybeThrowAssignPresent);

using folly::detail::concurrenthashmap::bucket::BucketTable;

#if (                                                        \
    FOLLY_SSE_PREREQ(4, 2) ||                                \
    (FOLLY_AARCH64 && FOLLY_F14_CRC_INTRINSIC_AVAILABLE)) && \
    FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
using folly::detail::concurrenthashmap::simd::SIMDTable;
using MapFactoryTypes =
    ::testing::Types<MapFactory<BucketTable>, MapFactory<SIMDTable>>;
#else
using MapFactoryTypes = ::testing::Types<MapFactory<BucketTable>>;
#endif

INSTANTIATE_TYPED_TEST_SUITE_P(
    MapFactoryTypesInstantiation, ConcurrentHashMapTest, MapFactoryTypes);
