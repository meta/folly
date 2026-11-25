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

#include <folly/container/EvictingCacheMap.h>

#include <memory>
#include <new>
#include <set>
#include <utility>

#include <folly/portability/GTest.h>

using namespace folly;

TEST(EvictingCacheMap, SetReleasesNodeWhenIndexInsertThrows) {
  struct Key {
    explicit Key(int id, bool failAfterCopy = false)
        : id(id), failAfterCopy(failAfterCopy) {}

    Key(const Key& other)
        : id(other.id),
          failAfterCopy(other.failAfterCopy),
          failOnHash(other.failAfterCopy) {}

    Key& operator=(const Key& other) {
      id = other.id;
      failAfterCopy = other.failAfterCopy;
      failOnHash = other.failAfterCopy;
      return *this;
    }

    bool operator==(const Key& other) const { return id == other.id; }

    int id;
    bool failAfterCopy;
    bool failOnHash{false};
  };
  struct Hash {
    size_t operator()(const Key& key) const {
      if (key.failOnHash) {
        throw std::bad_alloc();
      }
      return key.id;
    }
  };

  EvictingCacheMap<Key, std::shared_ptr<int>, Hash> map(1);
  map.set(Key{1}, std::make_shared<int>(1));
  auto value = std::make_shared<int>(2);
  std::weak_ptr<int> weakValue = value;

  // Lookup hashes the original key. Index insertion hashes the node's copy.
  EXPECT_THROW(map.set(Key{2, true}, std::move(value)), std::bad_alloc);
  EXPECT_TRUE(weakValue.expired());
  ASSERT_EQ(map.size(), 1);
  EXPECT_TRUE(map.exists(Key{1}));
  EXPECT_FALSE(map.exists(Key{2}));

  map.set(Key{2}, std::make_shared<int>(2));
  ASSERT_EQ(map.size(), 1);
  EXPECT_FALSE(map.exists(Key{1}));
  EXPECT_TRUE(map.exists(Key{2}));
}

TEST(EvictingCacheMap, SanityTest) {
  EvictingCacheMap<int, int> map(0);

  EXPECT_EQ(0, map.size());
  EXPECT_TRUE(map.empty());
  EXPECT_FALSE(map.exists(1));
  map.set(1, 1);
  EXPECT_EQ(1, map.size());
  EXPECT_FALSE(map.empty());
  EXPECT_EQ(1, map.get(1));
  EXPECT_TRUE(map.exists(1));
  map.set(1, 2);
  EXPECT_EQ(1, map.size());
  EXPECT_FALSE(map.empty());
  EXPECT_EQ(2, map.get(1));
  EXPECT_TRUE(map.exists(1));
  map.erase(1);
  EXPECT_EQ(0, map.size());
  EXPECT_TRUE(map.empty());
  EXPECT_FALSE(map.exists(1));

  EXPECT_EQ(0, map.size());
  EXPECT_TRUE(map.empty());
  EXPECT_FALSE(map.exists(1));
  map.set(1, 1);
  EXPECT_EQ(1, map.size());
  EXPECT_FALSE(map.empty());
  EXPECT_EQ(1, map.get(1));
  EXPECT_TRUE(map.exists(1));
  map.set(1, 2);
  EXPECT_EQ(1, map.size());
  EXPECT_FALSE(map.empty());
  EXPECT_EQ(2, map.get(1));
  EXPECT_TRUE(map.exists(1));

  EXPECT_FALSE(map.exists(2));
  map.set(2, 1);
  EXPECT_TRUE(map.exists(2));
  EXPECT_EQ(2, map.size());
  EXPECT_FALSE(map.empty());
  EXPECT_EQ(1, map.get(2));
  map.set(2, 2);
  EXPECT_EQ(2, map.size());
  EXPECT_FALSE(map.empty());
  EXPECT_EQ(2, map.get(2));
  EXPECT_TRUE(map.exists(2));
  map.erase(2);
  EXPECT_EQ(1, map.size());
  EXPECT_FALSE(map.empty());
  EXPECT_FALSE(map.exists(2));
  map.erase(1);
  EXPECT_EQ(0, map.size());
  EXPECT_TRUE(map.empty());
  EXPECT_FALSE(map.exists(1));
}

TEST(EvictingCacheMap, PruneTest) {
  EvictingCacheMap<int, int> map(0);
  EXPECT_EQ(0, map.size());
  EXPECT_TRUE(map.empty());
  for (int i = 0; i < 100; i++) {
    EXPECT_FALSE(map.exists(i));
  }

  for (int i = 0; i < 100; i++) {
    map.set(i, i);
    EXPECT_EQ(i + 1, map.size());
    EXPECT_FALSE(map.empty());
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i));
  }

  map.prune(1000000);
  EXPECT_EQ(0, map.size());
  EXPECT_TRUE(map.empty());
  for (int i = 0; i < 100; i++) {
    EXPECT_FALSE(map.exists(i));
  }

  for (int i = 0; i < 100; i++) {
    map.set(i, i);
    EXPECT_EQ(i + 1, map.size());
    EXPECT_FALSE(map.empty());
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i));
  }

  map.prune(100);
  EXPECT_EQ(0, map.size());
  EXPECT_TRUE(map.empty());
  for (int i = 0; i < 100; i++) {
    EXPECT_FALSE(map.exists(i));
  }

  for (int i = 0; i < 100; i++) {
    map.set(i, i);
    EXPECT_EQ(i + 1, map.size());
    EXPECT_FALSE(map.empty());
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i));
  }

  map.prune(99);
  EXPECT_EQ(1, map.size());
  EXPECT_FALSE(map.empty());
  for (int i = 0; i < 99; i++) {
    EXPECT_FALSE(map.exists(i));
  }
  EXPECT_TRUE(map.exists(99));
  EXPECT_EQ(99, map.get(99));

  map.prune(100);
  EXPECT_EQ(0, map.size());
  EXPECT_TRUE(map.empty());
  for (int i = 0; i < 100; i++) {
    EXPECT_FALSE(map.exists(i));
  }

  for (int i = 0; i < 100; i++) {
    map.set(i, i);
    EXPECT_EQ(i + 1, map.size());
    EXPECT_FALSE(map.empty());
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i));
  }

  map.prune(90);
  EXPECT_EQ(10, map.size());
  EXPECT_FALSE(map.empty());
  for (int i = 0; i < 90; i++) {
    EXPECT_FALSE(map.exists(i));
  }
  for (int i = 90; i < 100; i++) {
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i));
  }
}

TEST(EvictingCacheMap, PruneHookTest) {
  EvictingCacheMap<int, int> map(0);
  EXPECT_EQ(0, map.size());
  EXPECT_TRUE(map.empty());
  for (int i = 0; i < 100; i++) {
    EXPECT_FALSE(map.exists(i));
  }

  int sum = 0;
  auto pruneCb = [&](int&& k, int&& v) {
    EXPECT_EQ(k, v);
    sum += k;
  };

  EXPECT_FALSE(map.getPruneHook());
  map.setPruneHook(pruneCb);
  EXPECT_TRUE(map.getPruneHook());
  {
    int v = 42;
    map.getPruneHook()(42, std::move(v));
  }
  EXPECT_EQ(42, sum);
  sum = 0;

  for (int i = 0; i < 100; i++) {
    map.set(i, i);
    EXPECT_EQ(i + 1, map.size());
    EXPECT_FALSE(map.empty());
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i));
  }

  map.prune(1000000);
  EXPECT_EQ(0, map.size());
  EXPECT_TRUE(map.empty());
  for (int i = 0; i < 100; i++) {
    EXPECT_FALSE(map.exists(i));
  }
  EXPECT_EQ((99 * 100) / 2, sum);
  sum = 0;

  for (int i = 0; i < 100; i++) {
    map.set(i, i);
    EXPECT_EQ(i + 1, map.size());
    EXPECT_FALSE(map.empty());
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i));
  }

  map.prune(100);
  EXPECT_EQ(0, map.size());
  EXPECT_TRUE(map.empty());
  for (int i = 0; i < 100; i++) {
    EXPECT_FALSE(map.exists(i));
  }
  EXPECT_EQ((99 * 100) / 2, sum);
  sum = 0;

  for (int i = 0; i < 100; i++) {
    map.set(i, i);
    EXPECT_EQ(i + 1, map.size());
    EXPECT_FALSE(map.empty());
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i));
  }

  map.prune(99);
  EXPECT_EQ(1, map.size());
  EXPECT_FALSE(map.empty());
  for (int i = 0; i < 99; i++) {
    EXPECT_FALSE(map.exists(i));
  }
  EXPECT_TRUE(map.exists(99));
  EXPECT_EQ(99, map.get(99));

  EXPECT_EQ((98 * 99) / 2, sum);
  sum = 0;

  map.prune(100);
  EXPECT_EQ(0, map.size());
  EXPECT_TRUE(map.empty());
  for (int i = 0; i < 100; i++) {
    EXPECT_FALSE(map.exists(i));
  }

  EXPECT_EQ(99, sum);
  sum = 0;

  for (int i = 0; i < 100; i++) {
    map.set(i, i);
    EXPECT_EQ(i + 1, map.size());
    EXPECT_FALSE(map.empty());
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i));
  }

  map.prune(90);
  EXPECT_EQ(10, map.size());
  EXPECT_FALSE(map.empty());
  for (int i = 0; i < 90; i++) {
    EXPECT_FALSE(map.exists(i));
  }
  for (int i = 90; i < 100; i++) {
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i));
  }
  EXPECT_EQ((89 * 90) / 2, sum);
  sum = 0;

  // Erase does not call prune hook (NOTE: possible source of usage bugs)
  map.erase(99);

  EXPECT_EQ(0, sum);

  // But can provide your own hook
  map.erase(98, pruneCb);

  EXPECT_EQ(98, sum);
  sum = 0;

  // And with iterator
  auto it1 = map.find(96);
  auto it2 = map.find(97);

  auto it3 = map.erase(it2, pruneCb);
  EXPECT_EQ(it1, it3);
  EXPECT_EQ(97, sum);
  sum = 0;

  // Destructor does not call prune hook (NOTE: possibly source of usage bugs)
  map.~EvictingCacheMap<int, int>();
  // Re-enter clean state
  new (&map) EvictingCacheMap<int, int>(0);

  EXPECT_EQ(0, sum);
}

TEST(EvictingCacheMap, SetMaxSize) {
  EvictingCacheMap<int, int> map(100, 20);
  for (int i = 0; i < 90; i++) {
    map.set(i, i);
    EXPECT_TRUE(map.exists(i));
  }

  EXPECT_EQ(90, map.size());
  map.setMaxSize(50);
  EXPECT_EQ(map.size(), 50);

  for (int i = 0; i < 90; i++) {
    map.set(i, i);
    EXPECT_TRUE(map.exists(i));
  }
  EXPECT_EQ(40, map.size());
  map.setMaxSize(0);
  EXPECT_EQ(40, map.size());
  map.setMaxSize(10);
  EXPECT_EQ(10, map.size());
}

TEST(EvictingCacheMap, SetClearSize) {
  EvictingCacheMap<int, int> map(100, 20);
  for (int i = 0; i < 90; i++) {
    map.set(i, i);
    EXPECT_TRUE(map.exists(i));
  }

  EXPECT_EQ(90, map.size());
  map.setClearSize(40);
  map.setMaxSize(50);
  EXPECT_EQ(map.size(), 50);

  for (int i = 0; i < 90; i++) {
    map.set(i, i);
    EXPECT_TRUE(map.exists(i));
  }
  EXPECT_EQ(20, map.size());
  map.setMaxSize(0);
  EXPECT_EQ(20, map.size());
  map.setMaxSize(10);
  EXPECT_EQ(0, map.size());
}

TEST(EvictingCacheMap, DestructorInvocationTest) {
  struct SumInt {
    SumInt(int val_, int* ref_) : val(val_), ref(ref_) {}
    ~SumInt() { *ref += val; }

    SumInt(SumInt const&) = delete;
    SumInt& operator=(SumInt const&) = delete;

    SumInt(SumInt&& other) : val(std::exchange(other.val, 0)), ref(other.ref) {}
    SumInt& operator=(SumInt&& other) {
      std::swap(val, other.val);
      std::swap(ref, other.ref);
      return *this;
    }

    int val;
    int* ref;
  };

  int sum;
  EvictingCacheMap<int, SumInt> map(0);

  EXPECT_EQ(0, map.size());
  EXPECT_TRUE(map.empty());
  for (int i = 0; i < 100; i++) {
    EXPECT_FALSE(map.exists(i));
  }

  for (int i = 0; i < 100; i++) {
    map.set(i, SumInt(i, &sum));
    EXPECT_EQ(i + 1, map.size());
    EXPECT_FALSE(map.empty());
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i).val);
  }

  sum = 0;
  map.prune(1000000);
  EXPECT_EQ(0, map.size());
  EXPECT_TRUE(map.empty());
  for (int i = 0; i < 100; i++) {
    EXPECT_FALSE(map.exists(i));
  }
  EXPECT_EQ((99 * 100) / 2, sum);

  for (int i = 0; i < 100; i++) {
    map.set(i, SumInt(i, &sum));
    EXPECT_EQ(i + 1, map.size());
    EXPECT_FALSE(map.empty());
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i).val);
  }

  sum = 0;
  map.prune(100);
  EXPECT_EQ(0, map.size());
  EXPECT_TRUE(map.empty());
  for (int i = 0; i < 100; i++) {
    EXPECT_FALSE(map.exists(i));
  }
  EXPECT_EQ((99 * 100) / 2, sum);

  for (int i = 0; i < 100; i++) {
    map.set(i, SumInt(i, &sum));
    EXPECT_EQ(i + 1, map.size());
    EXPECT_FALSE(map.empty());
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i).val);
  }

  sum = 0;
  map.prune(99);
  EXPECT_EQ(1, map.size());
  EXPECT_FALSE(map.empty());
  for (int i = 0; i < 99; i++) {
    EXPECT_FALSE(map.exists(i));
  }
  EXPECT_TRUE(map.exists(99));
  EXPECT_EQ(99, map.get(99).val);

  EXPECT_EQ((98 * 99) / 2, sum);

  sum = 0;
  map.prune(100);
  EXPECT_EQ(0, map.size());
  EXPECT_TRUE(map.empty());
  for (int i = 0; i < 100; i++) {
    EXPECT_FALSE(map.exists(i));
  }

  EXPECT_EQ(99, sum);
  for (int i = 0; i < 100; i++) {
    map.set(i, SumInt(i, &sum));
    EXPECT_EQ(i + 1, map.size());
    EXPECT_FALSE(map.empty());
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i).val);
  }

  sum = 0;
  map.prune(90);
  EXPECT_EQ(10, map.size());
  EXPECT_FALSE(map.empty());
  for (int i = 0; i < 90; i++) {
    EXPECT_FALSE(map.exists(i));
  }
  for (int i = 90; i < 100; i++) {
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i).val);
  }
  EXPECT_EQ((89 * 90) / 2, sum);

  sum = 0;
  for (int i = 0; i < 90; i++) {
    auto pair = map.insert(i, SumInt(i + 1, &sum));
    EXPECT_EQ(i + 1, pair.first->second.val);
    EXPECT_TRUE(pair.second);
    EXPECT_TRUE(map.exists(i));
  }
  EXPECT_EQ(0, sum);
  for (int i = 90; i < 100; i++) {
    auto pair = map.insert(i, SumInt(i + 1, &sum));
    EXPECT_EQ(i, pair.first->second.val);
    EXPECT_FALSE(pair.second);
    EXPECT_TRUE(map.exists(i));
  }
  EXPECT_EQ((10 * 191) / 2, sum);
  sum = 0;
  map.prune(100);
  EXPECT_EQ((90 * 91) / 2 + (10 * 189) / 2, sum);

  sum = 0;
  map.set(3, SumInt(3, &sum));
  map.set(2, SumInt(2, &sum));
  map.set(1, SumInt(1, &sum));
  EXPECT_EQ(0, sum);
  EXPECT_EQ(2, map.erase(map.find(1))->second.val);
  EXPECT_EQ(1, sum);
  EXPECT_EQ(map.end(), map.erase(map.findWithoutPromotion(3)));
  EXPECT_EQ(4, sum);
  map.prune(1);
  EXPECT_EQ(6, sum);
}

TEST(EvictingCacheMap, LruSanityTest) {
  EvictingCacheMap<int, int> map(10);
  EXPECT_EQ(0, map.size());
  EXPECT_TRUE(map.empty());
  for (int i = 0; i < 100; i++) {
    EXPECT_FALSE(map.exists(i));
  }

  for (int i = 0; i < 100; i++) {
    map.set(i, i);
    EXPECT_GE(10, map.size());
    EXPECT_FALSE(map.empty());
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i));
  }

  EXPECT_EQ(10, map.size());
  EXPECT_FALSE(map.empty());
  for (int i = 0; i < 90; i++) {
    EXPECT_FALSE(map.exists(i));
  }
  for (int i = 90; i < 100; i++) {
    EXPECT_TRUE(map.exists(i));
  }
}

TEST(EvictingCacheMap, LruPromotionTest) {
  EvictingCacheMap<int, int> map(10);
  EXPECT_EQ(0, map.size());
  EXPECT_TRUE(map.empty());
  for (int i = 0; i < 100; i++) {
    EXPECT_FALSE(map.exists(i));
  }

  for (int i = 0; i < 100; i++) {
    map.set(i, i);
    EXPECT_GE(10, map.size());
    EXPECT_FALSE(map.empty());
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i));
    for (int j = 0; j < std::min(i + 1, 9); j++) {
      EXPECT_TRUE(map.exists(j));
      EXPECT_EQ(j, map.get(j));
    }
  }

  EXPECT_EQ(10, map.size());
  EXPECT_FALSE(map.empty());
  for (int i = 0; i < 9; i++) {
    EXPECT_TRUE(map.exists(i));
  }
  EXPECT_TRUE(map.exists(99));
  for (int i = 10; i < 99; i++) {
    EXPECT_FALSE(map.exists(i));
  }
}

TEST(EvictingCacheMap, LruNoPromotionTest) {
  EvictingCacheMap<int, int> map(10);
  EXPECT_EQ(0, map.size());
  EXPECT_TRUE(map.empty());
  for (int i = 0; i < 100; i++) {
    EXPECT_FALSE(map.exists(i));
  }

  for (int i = 0; i < 100; i++) {
    map.set(i, i);
    EXPECT_GE(10, map.size());
    EXPECT_FALSE(map.empty());
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i));
    for (int j = 0; j < std::min(i + 1, 9); j++) {
      if (map.exists(j)) {
        EXPECT_EQ(j, map.getWithoutPromotion(j));
      }
    }
  }

  EXPECT_EQ(10, map.size());
  EXPECT_FALSE(map.empty());
  for (int i = 0; i < 90; i++) {
    EXPECT_FALSE(map.exists(i));
  }
  for (int i = 90; i < 100; i++) {
    EXPECT_TRUE(map.exists(i));
  }
}

TEST(EvictingCacheMap, IteratorSanityTest) {
  const int nItems = 1000;
  EvictingCacheMap<int, int> map(nItems);
  EXPECT_TRUE(map.begin() == map.end());
  for (int i = 0; i < nItems; i++) {
    EXPECT_FALSE(map.exists(i));
    map.set(i, i * 2);
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i * 2, map.get(i));
  }

  std::set<int> seen;
  for (auto& it : map) {
    EXPECT_FALSE(seen.contains(it.first));
    seen.insert(it.first);
    EXPECT_EQ(it.first * 2, it.second);
  }
  EXPECT_EQ(nItems, seen.size());
}

TEST(EvictingCacheMap, FindTest) {
  const int nItems = 1000;
  EvictingCacheMap<int, int> map(nItems);
  for (int i = 0; i < nItems; i++) {
    map.set(i * 2, i * 2);
    EXPECT_TRUE(map.exists(i * 2));
    EXPECT_EQ(i * 2, map.get(i * 2));
  }
  for (int i = 0; i < nItems * 2; i++) {
    if (i % 2 == 0) {
      auto it = map.find(i);
      EXPECT_FALSE(it == map.end());
      EXPECT_EQ(i, it->first);
      EXPECT_EQ(i, it->second);
    } else {
      EXPECT_TRUE(map.find(i) == map.end());
    }
  }
  for (int i = nItems * 2 - 1; i >= 0; i--) {
    if (i % 2 == 0) {
      auto it = map.find(i);
      EXPECT_FALSE(it == map.end());
      EXPECT_EQ(i, it->first);
      EXPECT_EQ(i, it->second);
    } else {
      EXPECT_TRUE(map.find(i) == map.end());
    }
  }
  EXPECT_EQ(0, map.begin()->first);
}

TEST(EvictingCacheMap, FindWithoutPromotionTest) {
  const int nItems = 1000;
  EvictingCacheMap<int, int> map(nItems);
  for (int i = 0; i < nItems; i++) {
    map.set(i * 2, i * 2);
    EXPECT_TRUE(map.exists(i * 2));
    EXPECT_EQ(i * 2, map.get(i * 2));
  }
  for (int i = nItems * 2 - 1; i >= 0; i--) {
    if (i % 2 == 0) {
      auto it = map.findWithoutPromotion(i);
      EXPECT_FALSE(it == map.end());
      EXPECT_EQ(i, it->first);
      EXPECT_EQ(i, it->second);
    } else {
      EXPECT_TRUE(map.findWithoutPromotion(i) == map.end());
    }
  }
  EXPECT_EQ((nItems - 1) * 2, map.begin()->first);
}

TEST(EvictingCacheMap, IteratorOrderingTest) {
  const int nItems = 1000;
  EvictingCacheMap<int, int> map(nItems);
  for (int i = 0; i < nItems; i++) {
    map.set(i, i);
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i));
  }

  int expected = nItems - 1;
  for (auto it = map.begin(); it != map.end(); ++it) {
    EXPECT_EQ(expected, it->first);
    expected--;
  }

  expected = 0;
  for (auto it = map.rbegin(); it != map.rend(); ++it) {
    EXPECT_EQ(expected, it->first);
    expected++;
  }

  {
    auto it = map.end();
    expected = 0;
    EXPECT_TRUE(it != map.begin());
    do {
      --it;
      EXPECT_EQ(expected, it->first);
      expected++;
    } while (it != map.begin());
    EXPECT_EQ(nItems, expected);
  }

  {
    auto it = map.rend();
    expected = nItems - 1;
    do {
      --it;
      EXPECT_EQ(expected, it->first);
      expected--;
    } while (it != map.rbegin());
    EXPECT_EQ(-1, expected);
  }
}

TEST(EvictingCacheMap, MoveTest) {
  const int nItems = 1000;
  EvictingCacheMap<int, int> map(nItems);
  for (int i = 0; i < nItems; i++) {
    map.set(i, i);
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i));
  }

  // Move to empty
  EvictingCacheMap<int, int> map2 = std::move(map);
  EXPECT_TRUE(map.empty());
  for (int i = 0; i < nItems; i++) {
    EXPECT_TRUE(map2.exists(i));
    EXPECT_EQ(i, map2.get(i));
  }

  // Move to non-empty
  EvictingCacheMap<int, int> map3(1);
  map3.set(1, 1);
  EXPECT_EQ(1, map3.size());
  map3 = std::move(map2);
  EXPECT_TRUE(map2.empty());
  EXPECT_EQ(nItems, map3.size());
}

TEST(EvictingCacheMap, CustomKeyEqual) {
  const int nItems = 100;
  struct Eq {
    bool operator()(const int& a, const int& b) const {
      return (a % mod) == (b % mod);
    }
    int mod;
  };
  struct Hash {
    size_t operator()(const int& a) const { return std::hash<int>()(a % mod); }
    int mod;
  };
  EvictingCacheMap<int, int, Hash, Eq> map(
      nItems, 1 /* clearSize */, Hash{nItems}, Eq{nItems});
  for (int i = 0; i < nItems; i++) {
    map.set(i, i);
    EXPECT_TRUE(map.exists(i));
    EXPECT_EQ(i, map.get(i));
    EXPECT_TRUE(map.exists(i + nItems));
    EXPECT_EQ(i, map.get(i + nItems));
  }
}

TEST(EvictingCacheMap, InvalidHashPartlyUsable) {
  // Some uses of EvictingCacheMap only use constructor+destructor in a header
  // file where the hasher is invalid. (Only fully defined in cpp file.)
  struct BadHash {};
  struct BadEq {};
  EvictingCacheMap<int, int, BadHash, BadEq> map{42};
  // Also move ctor and operator
  EvictingCacheMap<int, int, BadHash, BadEq> map2{std::move(map)};
  map = std::move(map2);
}

TEST(EvictingCacheMap, IteratorConversion) {
  using type = EvictingCacheMap<int, int>;
  using i = type::iterator;
  using ci = type::const_iterator;
  using ri = type::reverse_iterator;
  using cri = type::const_reverse_iterator;

  EXPECT_TRUE((std::is_convertible<i, i>::value));
  EXPECT_TRUE((std::is_convertible<i, ci>::value));
  EXPECT_FALSE((std::is_convertible<ci, i>::value));
  EXPECT_TRUE((std::is_convertible<ci, ci>::value));

  EXPECT_TRUE((std::is_convertible<ri, ri>::value));
  EXPECT_TRUE((std::is_convertible<ri, cri>::value));
  EXPECT_FALSE((std::is_convertible<cri, ri>::value));
  EXPECT_TRUE((std::is_convertible<cri, cri>::value));
}

TEST(EvictingCacheMap, HeterogeneousAccess) {
  constexpr std::array pieces{
      std::pair{"one"_sp, 1},
      std::pair{"two"_sp, 2},
      std::pair{"three"_sp, 3},
  };
  constexpr std::array charstars{
      std::pair{"four", 4},
      std::pair{"five", 5},
      std::pair{"six", 6},
      std::pair{"seven", 7},
  };

  EvictingCacheMap<std::string, int> map(0);
  for (auto&& [key, value] : pieces) {
    auto [_, inserted] = map.insert(key, value);
    EXPECT_TRUE(inserted);
  }
  for (auto&& [key, value] : charstars) {
    map.set(key, value);
  }

  for (auto&& [key, value] : pieces) {
    auto exists = map.exists(key);
    EXPECT_TRUE(exists);
    auto iter = map.find(key);
    EXPECT_TRUE(iter != map.end());
    EXPECT_EQ(iter->second, value);
    iter = map.findWithoutPromotion(key);
    EXPECT_TRUE(iter != map.end());
    EXPECT_EQ(iter->second, value);
  }
  for (auto&& [key, value] : charstars) {
    auto result = map.get(key);
    EXPECT_EQ(result, value);
    result = map.getWithoutPromotion(key);
    EXPECT_EQ(result, value);
  }

  for (auto&& [key, _] : pieces) {
    auto erased = map.erase(key);
    EXPECT_TRUE(erased);
    erased = map.erase(key);
    EXPECT_FALSE(erased);
  }
  for (auto&& [key, _] : charstars) {
    map.erase(map.findWithoutPromotion(key));
  }
  EXPECT_TRUE(map.empty());
}

TEST(EvictingCacheMap, ApproximateEntryMemUsage) {
  // Entry (without weight) should be
  // * two pointers for LRU list
  // * sizeof(key) and sizeof(value)
  // * roughly 1.5 pointers for F14 index
  // And sizeof(std::unique_ptr<char[]>) should usually be size of raw pointer
  EXPECT_EQ(sizeof(std::unique_ptr<char[]>), sizeof(char*));
  EXPECT_LE(
      (EvictingCacheMap<uint64_t, std::unique_ptr<char[]>>::
           kApproximateEntryMemUsage),
      48U);
}

TEST(EvictingCacheMap, PiecewiseConstructTest) {
  EvictingCacheMap<uint64_t, std::string> map(1);
  uint64_t i = 1;
  auto [iter, inserted] = map.try_emplace(i, "test");
  EXPECT_TRUE(inserted);
  EXPECT_EQ(iter->second, "test");
}

TEST(EvictingCacheMap, ZeroClearSizeClampingTest) {
  // Test that clearSize = 0 gets clamped to 1
  constexpr size_t kMaxSize = 5;
  constexpr size_t kClearSize = 0;
  EvictingCacheMap<int, int> map(kMaxSize, kClearSize);

  // Fill the cache to maxSize
  for (int i = 0; i < 5; i++) {
    map.set(i, i);
    EXPECT_TRUE(map.exists(i));
  }
  EXPECT_EQ(5, map.size());

  // Adding one more should trigger eviction
  // Even though clearSize was set to 0, it should be clamped to 1
  map.set(5, 5);
  EXPECT_EQ(5, map.size()); // Size should still be maxSize
  EXPECT_TRUE(map.exists(5)); // New element should exist
  EXPECT_FALSE(map.exists(0)); // Oldest element should be evicted

  // Verify that exactly 1 element was cleared (not 0)
  for (int i = 1; i < 6; i++) {
    EXPECT_TRUE(map.exists(i));
  }
}

TEST(EvictingCacheMap, SetClearSizeZeroClampingTest) {
  constexpr size_t kMaxSize = 10;
  constexpr size_t kClearSize = 5;
  EvictingCacheMap<int, int> map(kMaxSize, kClearSize);

  // Fill cache
  for (int i = 0; i < 10; i++) {
    map.set(i, i);
  }
  EXPECT_EQ(10, map.size());

  // Set clearSize to 0 - should be clamped to 1
  map.setClearSize(0);

  // Trigger eviction by reducing maxSize
  map.setMaxSize(5);
  EXPECT_EQ(5, map.size());

  // Add more elements to test that clearSize=1 is being used
  map.set(10, 10);
  EXPECT_EQ(5, map.size());
}
