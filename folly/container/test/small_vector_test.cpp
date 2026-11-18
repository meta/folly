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

#include <folly/small_vector.h>

#include <algorithm>
#include <array>
#include <forward_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include <folly/Conv.h>
#include <folly/Traits.h>
#include <folly/container/Iterator.h>
#include <folly/memory/UninitializedMemoryHacks.h>
#include <folly/portability/GTest.h>
#include <folly/sorted_vector_types.h>

using folly::small_vector;

using folly::small_vector_policy::policy_in_situ_only;
using folly::small_vector_policy::policy_size_type;

#if FOLLY_X64 || FOLLY_PPC64 || FOLLY_AARCH64 || FOLLY_RISCV64

template <typename...>
struct same_;
template <typename T>
struct same_<T, T> {};

auto s0 = same_<
    folly::small_vector_policy::
        merge<policy_size_type<uint32_t>, policy_size_type<uint8_t>>::size_type,
    uint8_t>{};

// Fast access. Heap only small vector
static_assert(
    sizeof(small_vector<int, 0>) == 16,
    "Object size is not what we expect for small_vector<int, 0>");

static_assert(
    sizeof(small_vector<int, 0, policy_size_type<uint32_t>>) ==
        4 /* size_ */ + 8,
    "Object size is not what we expect for small_vector<int, 0, uint32_t>");

static_assert(
    sizeof(small_vector<double, 0, policy_size_type<uint8_t>>) == 9,
    "Object size is not what we expect for small_vector<double, 0, uint32_t>");

static_assert(
    sizeof(small_vector<
           std::pair<int64_t, int64_t>,
           0,
           policy_size_type<uint32_t>>) == 12,
    "Object size is not what we expect for small_vector<pair<int64_t, int64_t>, 0, uint32_t>");

static_assert(
    sizeof(small_vector<int>) == 16,
    "Object size is not what we expect for small_vector<int>");
static_assert(
    sizeof(small_vector<int32_t, 2>) == 16,
    "Object size is not what we expect for "
    "small_vector<int32_t,2>");
static_assert(
    sizeof(small_vector<int, 10>) == 10 * sizeof(int) + sizeof(std::size_t),
    "Object size is not what we expect for small_vector<int,10>");

static_assert(
    sizeof(small_vector<int32_t, 1, policy_size_type<uint32_t>>) == 8 + 4,
    "small_vector<int32_t,1,uint32_t> is wrong size");

// Types sized so that inline storage == HeapPtrWithCapacity size for a given
// policy.  HeapPtrWithCapacity is packed: pointer(8) + InternalSizeType.
// When the two are equal, capacity should be stored inline at zero cost.
template <size_t N>
using SizedType = std::array<char, N>;

// 16 bytes == HeapPtrWithCapacity with default size_t policy (8+8).
static_assert(sizeof(SizedType<16>) == 16);
static_assert(
    sizeof(small_vector<SizedType<16>, 1>) == 16 + sizeof(std::size_t),
    "small_vector<SizedType<16>,1> should be 24 bytes — inline capacity fits "
    "within the union at no extra cost");

// 12 bytes == HeapPtrWithCapacity with uint32_t policy (8+4).
static_assert(sizeof(SizedType<12>) == 12);
static_assert(
    sizeof(small_vector<SizedType<12>, 1, policy_size_type<uint32_t>>) ==
        12 + sizeof(uint32_t),
    "small_vector<SizedType<12>,1,uint32_t> should be 16 bytes — inline "
    "capacity fits within the union at no extra cost");

// 10 bytes == HeapPtrWithCapacity with uint16_t policy (8+2).
static_assert(sizeof(SizedType<10>) == 10);
static_assert(
    sizeof(small_vector<SizedType<10>, 1, policy_size_type<uint16_t>>) ==
        10 + sizeof(uint16_t),
    "small_vector<SizedType<10>,1,uint16_t> should be 12 bytes — inline "
    "capacity fits within the union at no extra cost");

// Extra 2 bytes needed for alignment.
static_assert(
    sizeof(small_vector<int32_t, 1, policy_size_type<uint16_t>>) == 8 + 2 + 2,
    "small_vector<int32_t,1,uint16_t> is wrong size");
static_assert(
    alignof(small_vector<int32_t, 1, policy_size_type<uint16_t>>) >= 4,
    "small_vector not aligned correctly");

// Extra 3 bytes needed for alignment.
static_assert(
    sizeof(small_vector<int32_t, 1, policy_size_type<uint8_t>>) == 8 + 1 + 3,
    "small_vector<int32_t,1,uint8_t> is wrong size");
static_assert(
    alignof(small_vector<int32_t, 1, policy_size_type<uint8_t>>) >= 4,
    "small_vector not aligned correctly");

static_assert(
    sizeof(small_vector<int16_t, 4, policy_size_type<uint16_t>>) == 10,
    "Sizeof unexpectedly large");

#endif

static_assert(
    !std::is_trivially_copyable<std::unique_ptr<int>>::value,
    "std::unique_ptr<> is trivially copyable");

static_assert(
    alignof(small_vector<folly::aligned_storage_t<32, 32>, 4>) == 32,
    "small_vector not aligned correctly");

namespace {

template <typename Key, typename Value, size_t N>
using noheap_sorted_vector_map = folly::sorted_vector_map<
    Key,
    Value,
    std::less<Key>,
    std::allocator<std::pair<Key, Value>>,
    void,
    folly::small_vector<std::pair<Key, Value>, N, policy_in_situ_only<true>>>;

template <typename T, size_t N>
using noheap_sorted_vector_set = folly::sorted_vector_set<
    T,
    std::less<T>,
    std::allocator<T>,
    void,
    folly::small_vector<T, N, policy_in_situ_only<true>>>;

struct NontrivialType {
  static int ctored;
  explicit NontrivialType() : a(0) {}

  /* implicit */ NontrivialType(int a_) : a(a_) { ++ctored; }

  NontrivialType(NontrivialType const& /* s */) { ++ctored; }

  NontrivialType& operator=(NontrivialType const& o) {
    a = o.a;
    return *this;
  }

  int32_t a;
};
static_assert(
    !std::is_trivially_copyable<NontrivialType>::value,
    "NontrivialType is trivially copyable");

int NontrivialType::ctored = 0;

struct TestException {};

int throwCounter = 1;
void MaybeThrow() {
  if (!--throwCounter) {
    throw TestException();
  }
}

const int kMagic = 0xdeadbeef;
struct Thrower {
  static int alive;

  Thrower() : magic(kMagic) {
    EXPECT_EQ(magic, kMagic);
    MaybeThrow();
    ++alive;
  }
  Thrower(Thrower const& other) : magic(other.magic) {
    EXPECT_EQ(magic, kMagic);
    MaybeThrow();
    ++alive;
  }
  ~Thrower() noexcept {
    EXPECT_EQ(magic, kMagic);
    magic = 0;
    --alive;
  }

  Thrower& operator=(Thrower const& /* other */) {
    EXPECT_EQ(magic, kMagic);
    MaybeThrow();
    return *this;
  }

  // This is just to try to make sure we don't get our member
  // functions called on uninitialized memory.
  int magic;
};

int Thrower::alive = 0;

// Type that counts how many exist and doesn't support copy
// construction.
struct NoncopyableCounter {
  static int alive;
  NoncopyableCounter() { ++alive; }
  ~NoncopyableCounter() { --alive; }
  NoncopyableCounter(NoncopyableCounter&&) noexcept { ++alive; }
  NoncopyableCounter(NoncopyableCounter const&) = delete;
  NoncopyableCounter& operator=(NoncopyableCounter const&) const = delete;
  NoncopyableCounter& operator=(NoncopyableCounter&&) { return *this; }
};
int NoncopyableCounter::alive = 0;

static_assert(
    !std::is_trivially_copyable<NoncopyableCounter>::value,
    "NoncopyableCounter is trivially copyable");

// Check that throws don't break the basic guarantee for some cases.
// Uses the method for testing exception safety described at
// http://www.boost.org/community/exception_safety.html, to force all
// throwing code paths to occur.
template <int N>
struct TestBasicGuarantee {
  folly::small_vector<Thrower, N> vec;
  int const prepopulate;

  explicit TestBasicGuarantee(int prepopulate_) : prepopulate(prepopulate_) {
    throwCounter = 1000;
    for (int i = 0; i < prepopulate; ++i) {
      vec.emplace_back();
    }
  }

  ~TestBasicGuarantee() { throwCounter = 1000; }

  template <class Operation>
  void operator()(int insertCount, Operation const& op) {
    bool done = false;

    std::unique_ptr<folly::small_vector<Thrower, N>> workingVec;
    for (int counter = 1; !done; ++counter) {
      throwCounter = 1000;
      workingVec = std::make_unique<folly::small_vector<Thrower, N>>(vec);
      throwCounter = counter;
      EXPECT_EQ(Thrower::alive, prepopulate * 2);
      try {
        op(*workingVec);
        done = true;
      } catch (...) {
        // Note that the size of the vector can change if we were
        // inserting somewhere other than the end (it's a basic only
        // guarantee).  All we're testing here is that we have the
        // right amount of uninitialized vs initialized memory.
        EXPECT_EQ(Thrower::alive, workingVec->size() + vec.size());
        continue;
      }

      // If things succeeded.
      EXPECT_EQ(workingVec->size(), prepopulate + insertCount);
      EXPECT_EQ(Thrower::alive, prepopulate * 2 + insertCount);
    }
  }
};

} // namespace

template <int N>
void testBasicGuarantee() {
  for (int prepop = 1; prepop < 30; ++prepop) {
    (TestBasicGuarantee<N>(prepop))( // parens or a mildly vexing parse :(
        1,
        [&](folly::small_vector<Thrower, N>& v) { v.emplace_back(); });

    EXPECT_EQ(Thrower::alive, 0);

    (TestBasicGuarantee<N>(prepop))(1, [&](folly::small_vector<Thrower, N>& v) {
      v.insert(v.begin(), Thrower());
    });

    EXPECT_EQ(Thrower::alive, 0);

    (TestBasicGuarantee<N>(prepop))(1, [&](folly::small_vector<Thrower, N>& v) {
      v.insert(v.begin() + 1, Thrower());
    });

    EXPECT_EQ(Thrower::alive, 0);
  }

  TestBasicGuarantee<N>(4)(3, [&](folly::small_vector<Thrower, N>& v) {
    std::vector<Thrower> b;
    b.emplace_back();
    b.emplace_back();
    b.emplace_back();

    /*
     * Apparently if you do the following initializer_list instead
     * of the above push_back's, and one of the Throwers throws,
     * g++4.6 doesn't destruct the previous ones.  Heh.
     */
    // b = { Thrower(), Thrower(), Thrower() };
    v.insert(v.begin() + 1, b.begin(), b.end());
  });

  TestBasicGuarantee<N>(2)(6, [&](folly::small_vector<Thrower, N>& v) {
    std::vector<Thrower> b;
    for (int i = 0; i < 6; ++i) {
      b.emplace_back();
    }

    v.insert(v.begin() + 1, b.begin(), b.end());
  });
}

TEST(smallVector, BasicGuarantee) {
  testBasicGuarantee<3>();

  EXPECT_EQ(Thrower::alive, 0);
  try {
    throwCounter = 4;
    folly::small_vector<Thrower, 1> p(14, Thrower());
  } catch (...) {
  }
  EXPECT_EQ(Thrower::alive, 0);

  // Heap only small_vector
  testBasicGuarantee<0>();
}

// Run this with.
// MALLOC_CONF=prof_leak:true
// LD_PRELOAD=${JEMALLOC_PATH}/lib/libjemalloc.so.2
// LD_PRELOAD="$LD_PRELOAD:"${UNWIND_PATH}/lib/libunwind.so.7
TEST(smallVector, leakTest) {
  for (int j = 0; j < 1000; ++j) {
    folly::small_vector<int, 10> someVec(300);
    for (int i = 0; i < 10000; ++i) {
      someVec.push_back(12);
    }
  }

  for (int j = 0; j < 1000; ++j) {
    folly::small_vector<int, 0> someVec(300);
    for (int i = 0; i < 10000; ++i) {
      someVec.push_back(12);
    }
  }
}

TEST(smallVector, leakTestWithTracking) {
  constexpr size_t size = 97;
  constexpr auto count = folly::annotate_object_count_leaked_uncollected;
  auto const base = count();
  small_vector<int> vec;
  vec.resize(size);
  EXPECT_EQ(size, vec.size());
  EXPECT_EQ(base + size_t(folly::kIsSanitizeAddress), count());
  vec.resize(0);
  EXPECT_EQ(0, vec.size());
  EXPECT_EQ(base + size_t(folly::kIsSanitizeAddress), count());
  vec.shrink_to_fit();
  EXPECT_LT(vec.capacity(), size);
  EXPECT_EQ(base, count());
}

TEST(smallVector, leakTestWithLeakedObject) {
  {
    // this case does not actually need leak-tracking
    using vec_t = folly::small_vector<int, 1>;
    constexpr size_t size = 97;
    static auto& vec = *new vec_t();
    vec.resize(size);
    EXPECT_EQ(size, vec.size());
  }

  {
    // this case does need leak-tracking
    using policy_t = folly::small_vector_policy::policy_size_type<uint32_t>;
    using vec_t = folly::small_vector<int, 1, policy_t>;
    constexpr size_t size = 97;
    static auto& vec = *new vec_t();
    vec.resize(size);
    EXPECT_EQ(size, vec.size());
  }
}

TEST(smallVector, InsertTrivial) {
  folly::small_vector<int> someVec(3, 3);
  someVec.insert(someVec.begin(), 12, 12);
  EXPECT_EQ(someVec.size(), 15);
  for (size_t i = 0; i < someVec.size(); ++i) {
    if (i < 12) {
      EXPECT_EQ(someVec[i], 12);
    } else {
      EXPECT_EQ(someVec[i], 3);
    }
  }

  // Make sure we insert a larger range so we can test placement new
  // and move inserts
  auto oldSize = someVec.size();
  someVec.insert(someVec.begin() + 1, 30, 30);
  EXPECT_EQ(someVec.size(), oldSize + 30);
  EXPECT_EQ(someVec[0], 12);
  EXPECT_EQ(someVec[1], 30);
  EXPECT_EQ(someVec[31], 12);
}

TEST(smallVector, InsertNontrivial) {
  folly::small_vector<std::string> v1(6, "asd"), v2(7, "wat");
  v1.insert(v1.begin() + 1, v2.begin(), v2.end());
  EXPECT_TRUE(v1.size() == 6 + 7);
  EXPECT_EQ(v1.front(), "asd");
  EXPECT_EQ(v1[1], "wat");

  // Insert without default constructor
  class TestClass {
   public:
    // explicit TestClass() = default;
    explicit TestClass(std::string s_) : s(s_) {}
    std::string s;
  };
  folly::small_vector<TestClass> v3(5, TestClass("asd"));
  folly::small_vector<TestClass> v4(10, TestClass("wat"));
  v3.insert(v3.begin() + 1, v4.begin(), v4.end());
  EXPECT_TRUE(v3.size() == 5 + 10);
  EXPECT_EQ(v3[0].s, "asd");
  EXPECT_EQ(v3[1].s, "wat");
  EXPECT_EQ(v3[10].s, "wat");
  EXPECT_EQ(v3[11].s, "asd");
}

TEST(smallVector, InsertFromBidirectionalList) {
  folly::small_vector<std::string> v(6, "asd");
  std::list<std::string> l(6, "wat");
  v.insert(v.end(), l.begin(), l.end());
  EXPECT_EQ(v[0], "asd");
  EXPECT_EQ(v[5], "asd");
  EXPECT_EQ(v[6], "wat");
  EXPECT_EQ(v[11], "wat");
}

template <int N>
void testSwap() {
  folly::small_vector<int, N> somethingVec, emptyVec;
  somethingVec.push_back(1);
  somethingVec.push_back(2);
  somethingVec.push_back(3);
  somethingVec.push_back(4);

  // Swapping intern'd with intern'd.
  auto vec = somethingVec;
  EXPECT_TRUE(vec == somethingVec);
  EXPECT_FALSE(vec == emptyVec);
  EXPECT_FALSE(somethingVec == emptyVec);

  // Swapping a heap vector with an intern vector.
  folly::small_vector<int, N> junkVec;
  junkVec.assign(12, 12);
  EXPECT_EQ(junkVec.size(), 12);
  for (auto i : junkVec) {
    EXPECT_EQ(i, 12);
  }
  swap(junkVec, vec);
  EXPECT_TRUE(junkVec == somethingVec);
  EXPECT_EQ(vec.size(), 12);
  for (auto i : vec) {
    EXPECT_EQ(i, 12);
  }

  // Swapping two heap vectors.
  folly::small_vector<int, N> moreJunk(15, 15);
  EXPECT_EQ(moreJunk.size(), 15);
  for (auto i : moreJunk) {
    EXPECT_EQ(i, 15);
  }
  swap(vec, moreJunk);
  EXPECT_EQ(moreJunk.size(), 12);
  for (auto i : moreJunk) {
    EXPECT_EQ(i, 12);
  }
  EXPECT_EQ(vec.size(), 15);
  for (auto i : vec) {
    EXPECT_EQ(i, 15);
  }
}

TEST(smallVector, Swap) {
  testSwap<10>();

  // heap only small_vector
  testSwap<0>();

  // Making a vector heap, then smaller than another non-heap vector,
  // then swapping.
  folly::small_vector<int, 5> shrinker, other(4, 10);
  shrinker = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  shrinker.erase(shrinker.begin() + 2, shrinker.end());
  EXPECT_LT(shrinker.size(), other.size());
  swap(shrinker, other);
  EXPECT_EQ(shrinker.size(), 4);
  EXPECT_TRUE(std::all_of(shrinker.begin(), shrinker.end(), [](int i) {
    return i == 10;
  }));
  EXPECT_TRUE((other == small_vector<int, 5>{0, 1}));
}

template <int N>
void testEmplace() {
  NontrivialType::ctored = 0;

  folly::small_vector<NontrivialType, N> vec;
  vec.reserve(1024);
  {
    auto& emplaced = vec.emplace_back(12);
    EXPECT_EQ(NontrivialType::ctored, 1);
    EXPECT_EQ(vec.front().a, 12);
    EXPECT_TRUE(std::addressof(emplaced) == std::addressof(vec.back()));
  }
  {
    auto& emplaced = vec.emplace_back(13);
    EXPECT_EQ(vec.front().a, 12);
    EXPECT_EQ(vec.back().a, 13);
    EXPECT_EQ(NontrivialType::ctored, 2);
    EXPECT_TRUE(std::addressof(emplaced) == std::addressof(vec.back()));
  }

  NontrivialType::ctored = 0;
  for (int i = 0; i < 120; ++i) {
    auto& emplaced = vec.emplace_back(i);
    EXPECT_TRUE(std::addressof(emplaced) == std::addressof(vec.back()));
  }
  EXPECT_EQ(NontrivialType::ctored, 120);
  EXPECT_EQ(vec[0].a, 12);
  EXPECT_EQ(vec[1].a, 13);
  EXPECT_EQ(vec.back().a, 119);

  // We implement emplace() with a temporary (see the implementation
  // for a comment about why), so this should make 2 ctor calls.
  NontrivialType::ctored = 0;
  vec.emplace(vec.begin(), 12);
  EXPECT_EQ(NontrivialType::ctored, 2);
}

TEST(smallVector, Emplace) {
  testEmplace<1>();
  // heap only
  testEmplace<0>();
}

TEST(smallVector, Erase) {
  folly::small_vector<int, 4> notherVec = {1, 2, 3, 4, 5};
  EXPECT_EQ(notherVec.front(), 1);
  EXPECT_EQ(notherVec.size(), 5);
  notherVec.erase(notherVec.begin());
  EXPECT_EQ(notherVec.front(), 2);
  EXPECT_EQ(notherVec.size(), 4);
  EXPECT_EQ(notherVec[2], 4);
  EXPECT_EQ(notherVec[3], 5);
  notherVec.erase(notherVec.begin() + 2);
  EXPECT_EQ(notherVec.size(), 3);
  EXPECT_EQ(notherVec[2], 5);

  folly::small_vector<int, 2> vec2 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  vec2.erase(vec2.begin() + 1, vec2.end() - 1);
  folly::small_vector<int, 2> expected = {1, 10};
  EXPECT_TRUE(vec2 == expected);

  folly::small_vector<std::string, 3> v(102, "ASD");
  v.resize(1024, "D");
  EXPECT_EQ(v.size(), 1024);
  EXPECT_EQ(v.back(), "D");
  EXPECT_EQ(v.front(), "ASD");
  v.resize(1);
  EXPECT_EQ(v.front(), "ASD");
  EXPECT_EQ(v.size(), 1);
  v.resize(0);
  EXPECT_TRUE(v.empty());

  // test heap only
  folly::small_vector<int, 0> vec3 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  vec3.erase(vec3.begin() + 1, vec3.end() - 1);
  folly::small_vector<int, 0> expected3 = {1, 10};
  EXPECT_TRUE(vec3 == expected3);
}

template <int N>
void testGrowShrinkGrow() {
  folly::small_vector<NontrivialType, N> vec = {1, 2, 3, 4, 5};
  std::generate_n(std::back_inserter(vec), 102, std::rand);

  auto capacity = vec.capacity();

  auto oldSize = vec.size();
  for (size_t i = 0; i < oldSize; ++i) {
    vec.erase(vec.begin() + (std::rand() % vec.size()));
    EXPECT_EQ(vec.capacity(), capacity);
  }
  EXPECT_TRUE(vec.empty());

  EXPECT_EQ(vec.capacity(), capacity);
  std::generate_n(std::back_inserter(vec), 102, std::rand);
  EXPECT_EQ(vec.capacity(), capacity);

  std::generate_n(std::back_inserter(vec), 4096, std::rand);
  EXPECT_GT(vec.capacity(), capacity);

  vec.resize(10);
  vec.shrink_to_fit();
  EXPECT_LT(vec.capacity(), capacity);
  auto cap = vec.capacity();
  vec.resize(4);
  vec.shrink_to_fit();
  if (N > 4) {
    EXPECT_EQ(vec.capacity(), N); // in situ size
  } else {
    EXPECT_LT(vec.capacity(), cap); // on heap
  }
}

TEST(smallVector, GrowShrinkGrow) {
  testGrowShrinkGrow<7>();

  testGrowShrinkGrow<0>();
}

TEST(smallVector, ShrinkToFitMoveOnly) {
  folly::small_vector<std::unique_ptr<int>> vec;
  vec.reserve(100);
  for (int i = 0; i < 3; ++i) {
    vec.push_back(std::make_unique<int>(i));
  }
  vec.shrink_to_fit();
  EXPECT_LT(vec.capacity(), 100);
  ASSERT_EQ(vec.size(), 3);
  for (int i = 0; i < 3; ++i) {
    ASSERT_NE(vec[i], nullptr);
    EXPECT_EQ(*vec[i], i);
  }
}

TEST(smallVector, Iteration) {
  folly::small_vector<std::string, 3> vec = {"foo", "bar"};
  vec.push_back("blah");
  vec.push_back("blah2");
  vec.push_back("blah3");
  vec.erase(vec.begin() + 2);

  std::vector<std::string> otherVec;
  for (auto& s : vec) {
    otherVec.push_back(s);
  }
  EXPECT_EQ(otherVec.size(), vec.size());
  if (otherVec.size() == vec.size()) {
    EXPECT_TRUE(std::equal(otherVec.begin(), otherVec.end(), vec.begin()));
  }

  std::reverse(otherVec.begin(), otherVec.end());
  auto oit = otherVec.begin();
  auto rit = vec.crbegin();
  for (; rit != vec.crend(); ++oit, ++rit) {
    EXPECT_EQ(*oit, *rit);
  }
}

TEST(smallVector, NonCopyableType) {
  folly::small_vector<NontrivialType, 2> vec;

  for (int i = 0; i < 10; ++i) {
    vec.emplace(vec.begin(), 13);
  }
  EXPECT_EQ(vec.size(), 10);
  auto vec2 = std::move(vec);
  EXPECT_EQ(vec.size(), 0);
  EXPECT_EQ(vec2.size(), 10);
  vec2.clear();

  folly::small_vector<NoncopyableCounter, 3> vec3;
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(vec3.size(), i);
    EXPECT_EQ(NoncopyableCounter::alive, i);
    vec3.insert(vec3.begin(), NoncopyableCounter());
  }
  EXPECT_EQ(vec3.size(), 10);
  EXPECT_EQ(NoncopyableCounter::alive, 10);

  vec3.insert(vec3.begin() + 3, NoncopyableCounter());
  EXPECT_EQ(NoncopyableCounter::alive, 11);
  auto vec4 = std::move(vec3);
  EXPECT_EQ(NoncopyableCounter::alive, 11);
  vec4.resize(30);
  EXPECT_EQ(NoncopyableCounter::alive, 30);
  vec4.erase(vec4.begin(), vec4.end());
  EXPECT_EQ(vec4.size(), 0);
  EXPECT_EQ(NoncopyableCounter::alive, 0);

  {
    folly::small_vector<NontrivialType, 0> vec0;

    for (int i = 0; i < 10; ++i) {
      vec0.emplace(vec0.begin(), 13);
    }
    EXPECT_EQ(vec0.size(), 10);
    auto vec02 = std::move(vec0);
    EXPECT_EQ(vec0.size(), 0);
    EXPECT_EQ(vec02.size(), 10);
    vec02.clear();
  }
}

template <int N>
void testMoveConstructor() {
  folly::small_vector<std::string, N> v1;
  v1.push_back("asd");
  v1.push_back("bsd");
  auto v2 = std::move(v1);
  EXPECT_EQ(v2.size(), 2);
  EXPECT_EQ(v2[0], "asd");
  EXPECT_EQ(v2[1], "bsd");

  v1 = std::move(v2);
  EXPECT_EQ(v1.size(), 2);
  EXPECT_EQ(v1[0], "asd");
  EXPECT_EQ(v1[1], "bsd");
}

TEST(smallVector, MoveConstructor) {
  testMoveConstructor<10>();
  testMoveConstructor<0>();
}

TEST(smallVector, NoHeap) {
  using Vector =
      folly::small_vector<std::string, 10, policy_in_situ_only<true>>;

  Vector v;
  static_assert(v.max_size() == 10, "max_size is incorrect");

  for (int i = 0; i < 10; ++i) {
    v.push_back(folly::to<std::string>(i));
    EXPECT_EQ(v.size(), i + 1);
  }

  bool caught = false;
  try {
    v.insert(v.begin(), "ha");
  } catch (const std::length_error&) {
    caught = true;
  }
  EXPECT_TRUE(caught);

  // Check max_size works right with various policy combinations.
  folly::small_vector<std::string, 32, policy_size_type<uint32_t>> v4;
  EXPECT_EQ(v4.max_size(), (uint32_t(1) << 31) - 1);

  /*
   * Test that even when we ask for a small number inlined it'll still
   * inline at least as much as it takes to store the value_type
   * pointer.
   */
  folly::small_vector<char, 1, policy_in_situ_only<true>> notsosmall;
  static_assert(
      notsosmall.max_size() == sizeof(char*), "max_size is incorrect");
  caught = false;
  try {
    notsosmall.push_back(12);
    notsosmall.push_back(13);
    notsosmall.push_back(14);
  } catch (const std::length_error&) {
    caught = true;
  }
  EXPECT_FALSE(caught);
}

TEST(smallVector, MaxSize) {
  folly::small_vector<int, 2, policy_size_type<uint8_t>> vec;
  EXPECT_EQ(vec.max_size(), 127);
  vec.reserve(127);
  folly::small_vector<int, 2, policy_size_type<uint16_t>> vec2;
  EXPECT_EQ(vec2.max_size(), (1 << 15) - 1);
  vec2.reserve((1 << 15) - 1);

  folly::small_vector<int, 0, policy_size_type<uint8_t>> vec3;
  EXPECT_EQ(vec3.max_size(), 255);
  vec3.reserve(255);
}

TEST(smallVector, AllHeap) {
  // Use something bigger than the pointer so it can't get inlined.
  struct SomeObj {
    double a, b, c, d, e;
    int val;
    SomeObj(int val_) : val(val_) {}
    bool operator==(SomeObj const& o) const { return o.val == val; }
  };

  folly::small_vector<SomeObj, 0> vec = {1};
  EXPECT_EQ(vec.size(), 1);
  if (!vec.empty()) {
    EXPECT_TRUE(vec[0] == 1);
  }
  vec.insert(vec.begin(), {0, 1, 2, 3});
  EXPECT_EQ(vec.size(), 5);
  EXPECT_TRUE((vec == folly::small_vector<SomeObj, 0>{0, 1, 2, 3, 1}));
}
template <int N>
void testBasic() {
  using Vector = folly::small_vector<int, N, policy_size_type<uint32_t>>;

  Vector a;

  a.push_back(12);
  EXPECT_EQ(a.front(), 12);
  EXPECT_EQ(a.size(), 1);
  a.push_back(13);
  EXPECT_EQ(a.size(), 2);
  EXPECT_EQ(a.front(), 12);
  EXPECT_EQ(a.back(), 13);

  a.emplace(a.end(), 32);
  EXPECT_EQ(a.back(), 32);

  a.emplace(a.begin(), 12);
  EXPECT_EQ(a.front(), 12);
  EXPECT_EQ(a.back(), 32);
  a.erase(a.end() - 1);
  EXPECT_EQ(a.back(), 13);

  a.push_back(12);
  EXPECT_EQ(a.back(), 12);
  a.pop_back();
  EXPECT_EQ(a.back(), 13);

  const int s = 12;
  a.push_back(s); // lvalue reference

  Vector b, c;
  b = a;
  EXPECT_TRUE(b == a);
  c = std::move(b);
  EXPECT_TRUE(c == a);
  EXPECT_TRUE(c != b && b != a);

  EXPECT_GT(c.size(), 0);
  c.resize(1);
  EXPECT_EQ(c.size(), 1);

  Vector intCtor(12);
}

TEST(smallVector, Basic) {
  testBasic<3>();
  testBasic<0>();
}

TEST(smallVector, Capacity) {
  folly::small_vector<uint64_t, 1> vec;
  EXPECT_EQ(vec.size(), 0);
  EXPECT_EQ(vec.capacity(), 1);

  vec.push_back(0);
  EXPECT_EQ(vec.size(), 1);
  EXPECT_EQ(vec.capacity(), 1);

  vec.push_back(1);
  EXPECT_EQ(vec.size(), 2);
  EXPECT_GT(vec.capacity(), 1);

  folly::small_vector<uint64_t, 2> vec2;
  EXPECT_EQ(vec2.size(), 0);
  EXPECT_EQ(vec2.capacity(), 2);

  vec2.push_back(0);
  vec2.push_back(1);
  EXPECT_EQ(vec2.size(), 2);
  EXPECT_EQ(vec2.capacity(), 2);

  vec2.push_back(2);
  EXPECT_EQ(vec2.size(), 3);
  EXPECT_GT(vec2.capacity(), 2);

  // Test capacity heapifying logic
  folly::small_vector<unsigned char, 1> vec3;
  const size_t hc_size = 100000;
  for (size_t i = 0; i < hc_size; ++i) {
    auto v = (unsigned char)i;
    vec3.push_back(v);
    EXPECT_EQ(vec3[i], v);
    EXPECT_EQ(vec3.size(), i + 1);
    EXPECT_GT(vec3.capacity(), i);
  }
  for (auto i = hc_size; i > 0; --i) {
    auto v = (unsigned char)(i - 1);
    EXPECT_EQ(vec3.back(), v);
    vec3.pop_back();
    EXPECT_EQ(vec3.size(), i - 1);
  }
}

TEST(smallVector, HeapAllocationSize) {
  // Inline: no heap allocation.
  folly::small_vector<int, 2> vec;
  EXPECT_EQ(vec.heap_allocation_size(), 0);
  vec.push_back(1);
  EXPECT_EQ(vec.heap_allocation_size(), 0);
  vec.push_back(2);
  EXPECT_EQ(vec.heap_allocation_size(), 0);

  // Heap: allocation size >= capacity * sizeof(element).
  vec.push_back(3);
  EXPECT_GE(vec.heap_allocation_size(), vec.capacity() * sizeof(int));

  // Larger heap allocation.
  vec.reserve(1000);
  EXPECT_GE(vec.heap_allocation_size(), 1000 * sizeof(int));
}

template <int N>
void testSelfResize() {
  for (int i = 1; i < 33; ++i) {
    folly::small_vector<int, N> vec(i, 42);
    auto newSize = vec.capacity() + 1;
    vec.resize(newSize, vec[0]);
    EXPECT_EQ(vec.size(), newSize);
    for (auto value : vec) {
      EXPECT_EQ(value, 42);
    }
  }
}

TEST(smallVector, SelfResize) {
  testSelfResize<1>();
  testSelfResize<0>();
}

template <int N>
void testSelfPushBack() {
  for (int i = 1; i < 33; ++i) {
    folly::small_vector<std::string, N> vec;
    for (int j = 0; j < i; ++j) {
      vec.push_back("abc");
    }
    EXPECT_EQ(vec.size(), i);
    vec.push_back(std::move(vec[0]));
    EXPECT_EQ(vec.size(), i + 1);

    EXPECT_EQ(vec[i], "abc");
  }
}

TEST(smallVector, SelfPushBack) {
  testSelfPushBack<1>();
  testSelfPushBack<0>();
}

template <int N>
void testSelfEmplaceBack() {
  for (int i = 1; i < 33; ++i) {
    folly::small_vector<std::string, N> vec;
    for (int j = 0; j < i; ++j) {
      vec.emplace_back("abc");
    }
    EXPECT_EQ(vec.size(), i);
    vec.emplace_back(std::move(vec[0]));
    EXPECT_EQ(vec.size(), i + 1);

    EXPECT_EQ(vec[i], "abc");
  }
}

TEST(smallVector, SelfEmplaceBack) {
  testSelfEmplaceBack<1>();
  testSelfEmplaceBack<0>();
}

template <int N>
void testSelfInsert() {
  // end insert
  for (int i = 1; i < 33; ++i) {
    folly::small_vector<std::string, N> vec;
    for (int j = 0; j < i; ++j) {
      vec.push_back("abc");
    }
    EXPECT_EQ(vec.size(), i);
    vec.insert(vec.end(), std::move(vec[0]));
    EXPECT_EQ(vec.size(), i + 1);

    EXPECT_EQ(vec[i], "abc");
    EXPECT_EQ(vec[vec.size() - 1], "abc");
  }

  // middle insert
  for (int i = 2; i < 33; ++i) {
    folly::small_vector<std::string, N> vec;
    for (int j = 0; j < i; ++j) {
      vec.push_back("abc");
    }
    EXPECT_EQ(vec.size(), i);
    vec.insert(vec.end() - 1, std::move(vec[0]));
    EXPECT_EQ(vec.size(), i + 1);

    EXPECT_EQ(vec[i - 1], "abc");
    EXPECT_EQ(vec[i], "abc");
  }

  // range insert
  for (int i = 2; i < 33; ++i) {
    folly::small_vector<std::string, N> vec;
    // reserve 2 * i space so we don't grow and invalidate references.
    vec.reserve(2 * i);
    for (int j = 0; j < i; ++j) {
      vec.push_back("abc");
    }
    EXPECT_EQ(vec.size(), i);
    vec.insert(vec.end() - 1, vec.begin(), vec.end() - 1);
    EXPECT_EQ(vec.size(), 2 * i - 1);

    for (auto const& val : vec) {
      EXPECT_EQ(val, "abc");
    }
  }
}

TEST(smallVector, SelfInsert) {
  testSelfInsert<1>();

  testSelfInsert<0>();
}

struct CheckedInt {
  static const int DEFAULT_VALUE = (int)0xdeadbeef;
  CheckedInt() : value(DEFAULT_VALUE) {}
  explicit CheckedInt(int value_) : value(value_) {}
  CheckedInt(const CheckedInt& rhs, int) : value(rhs.value) {}
  CheckedInt(const CheckedInt& rhs) : value(rhs.value) {}
  CheckedInt(CheckedInt&& rhs) noexcept : value(rhs.value) {
    rhs.value = DEFAULT_VALUE;
  }
  CheckedInt& operator=(const CheckedInt& rhs) {
    value = rhs.value;
    return *this;
  }
  CheckedInt& operator=(CheckedInt&& rhs) noexcept {
    value = rhs.value;
    rhs.value = DEFAULT_VALUE;
    return *this;
  }
  ~CheckedInt() {}
  int value;
};

TEST(smallVector, ForwardingEmplaceInsideVector) {
  folly::small_vector<CheckedInt> v;
  v.push_back(CheckedInt(1));
  for (int i = 1; i < 20; ++i) {
    v.emplace_back(v[0], 42);
    ASSERT_EQ(1, v.back().value);
  }
}

TEST(smallVector, LVEmplaceInsideVector) {
  folly::small_vector<CheckedInt> v;
  v.push_back(CheckedInt(1));
  for (int i = 1; i < 20; ++i) {
    v.emplace_back(v[0]);
    ASSERT_EQ(1, v.back().value);
  }
}

TEST(smallVector, CLVEmplaceInsideVector) {
  folly::small_vector<CheckedInt> v;
  const folly::small_vector<CheckedInt>& cv = v;
  v.push_back(CheckedInt(1));
  for (int i = 1; i < 20; ++i) {
    v.emplace_back(cv[0]);
    ASSERT_EQ(1, v.back().value);
  }
}

TEST(smallVector, RVEmplaceInsideVector) {
  folly::small_vector<CheckedInt> v;
  v.push_back(CheckedInt(0));
  for (int i = 1; i < 20; ++i) {
    v[0] = CheckedInt(1);
    v.emplace_back(std::move(v[0]));
    ASSERT_EQ(1, v.back().value);
  }
}

TEST(smallVector, LVPushValueInsideVector) {
  folly::small_vector<CheckedInt> v;
  v.push_back(CheckedInt(1));
  for (int i = 1; i < 20; ++i) {
    v.push_back(v[0]);
    ASSERT_EQ(1, v.back().value);
  }
}

TEST(smallVector, RVPushValueInsideVector) {
  folly::small_vector<CheckedInt> v;
  v.push_back(CheckedInt(0));
  for (int i = 1; i < 20; ++i) {
    v[0] = CheckedInt(1);
    v.push_back(v[0]);
    ASSERT_EQ(1, v.back().value);
  }
}

TEST(smallVector, EmplaceIterCtor) {
  std::vector<int*> v{new int(1), new int(2)};
  std::vector<std::unique_ptr<int>> uv(v.begin(), v.end());

  std::vector<int*> w{new int(1), new int(2)};
  small_vector<std::unique_ptr<int>> uw(w.begin(), w.end());
}

TEST(smallVector, InputIterator) {
  std::vector<int> expected{125, 320, 512, 750, 333};
  std::string values = "125 320 512 750 333";
  std::istringstream is1(values);
  std::istringstream is2(values);

  std::vector<int> stdV{
      std::istream_iterator<int>(is1), std::istream_iterator<int>()};
  ASSERT_EQ(stdV.size(), expected.size());
  for (size_t i = 0; i < expected.size(); i++) {
    ASSERT_EQ(stdV[i], expected[i]);
  }

  small_vector<int> smallV{
      std::istream_iterator<int>(is2), std::istream_iterator<int>()};
  ASSERT_EQ(smallV.size(), expected.size());
  for (size_t i = 0; i < expected.size(); i++) {
    ASSERT_EQ(smallV[i], expected[i]);
  }
}

TEST(smallVector, InsertFromInputIterator) {
  small_vector<int, 2> v{1, 2, 9};
  std::istringstream is("3 4 5");
  v.insert(
      v.begin() + 2,
      std::istream_iterator<int>(is),
      std::istream_iterator<int>());
  const std::vector<int> expected{1, 2, 3, 4, 5, 9};
  ASSERT_EQ(v.size(), expected.size());
  for (size_t i = 0; i < expected.size(); i++) {
    EXPECT_EQ(v[i], expected[i]);
  }
}

TEST(smallVector, InsertFromForwardIterator) {
  small_vector<int, 2> v{1, 2, 9};
  const std::forward_list<int> src{3, 4, 5};
  v.insert(v.begin() + 2, src.begin(), src.end());
  const std::vector<int> expected{1, 2, 3, 4, 5, 9};
  ASSERT_EQ(v.size(), expected.size());
  for (size_t i = 0; i < expected.size(); i++) {
    EXPECT_EQ(v[i], expected[i]);
  }
}

TEST(smallVector, NoCopyCtor) {
  struct Tester {
    Tester() = default;
    Tester(const Tester&) = delete;
    [[maybe_unused]] Tester(Tester&&) = default;

    int field = 42;
  };

  small_vector<Tester> test(10);
  ASSERT_EQ(test.size(), 10);
  for (const auto& element : test) {
    EXPECT_EQ(element.field, 42);
  }
}

TEST(smallVector, ZeroInitializable) {
  small_vector<int> test(10);
  ASSERT_EQ(test.size(), 10);
  for (const auto& element : test) {
    EXPECT_EQ(element, 0);
  }
}

TEST(smallVector, InsertMoreThanGrowth) {
  small_vector<int, 10> test;
  test.insert(test.end(), 30, 0);
  for (auto element : test) {
    EXPECT_EQ(element, 0);
  }
}

TEST(smallVector, EmplaceBackExponentialGrowth) {
  small_vector<std::pair<int, int>> test;
  std::vector<size_t> capacities;
  capacities.push_back(test.capacity());
  for (int i = 0; i < 10000; ++i) {
    test.emplace_back(0, 0);
    if (test.capacity() != capacities.back()) {
      capacities.push_back(test.capacity());
    }
  }
  EXPECT_LE(capacities.size(), 25);
}

TEST(smallVector, InsertExponentialGrowth) {
  small_vector<std::pair<int, int>> test;
  std::vector<size_t> capacities;
  capacities.push_back(test.capacity());
  for (int i = 0; i < 10000; ++i) {
    test.insert(test.begin(), std::make_pair(0, 0));
    if (test.capacity() != capacities.back()) {
      capacities.push_back(test.capacity());
    }
  }
  EXPECT_LE(capacities.size(), 25);
}

TEST(smallVector, InsertNExponentialGrowth) {
  small_vector<int> test;
  std::vector<size_t> capacities;
  capacities.push_back(test.capacity());
  for (int i = 0; i < 10000; ++i) {
    test.insert(test.begin(), 100, 0);
    if (test.capacity() != capacities.back()) {
      capacities.push_back(test.capacity());
    }
  }
  EXPECT_LE(capacities.size(), 25);
}

namespace {
struct Counts {
  size_t copyCount{0};
  size_t moveCount{0};
};

class Counter {
  Counts* counts;

 public:
  explicit Counter(Counts& counts_) : counts(&counts_) {}
  Counter(Counter const& other) noexcept : counts(other.counts) {
    ++counts->copyCount;
  }
  Counter(Counter&& other) noexcept : counts(other.counts) {
    ++counts->moveCount;
  }
  Counter& operator=(Counter const& rhs) noexcept {
    EXPECT_EQ(counts, rhs.counts);
    ++counts->copyCount;
    return *this;
  }
  [[maybe_unused]] Counter& operator=(Counter&& rhs) noexcept {
    EXPECT_EQ(counts, rhs.counts);
    ++counts->moveCount;
    return *this;
  }
};
} // namespace

TEST(smallVector, EmplaceBackEfficiency) {
  small_vector<Counter, 2> test;
  Counts counts;
  for (size_t i = 1; i <= test.capacity(); ++i) {
    test.emplace_back(counts);
    EXPECT_EQ(0, counts.copyCount);
    EXPECT_EQ(0, counts.moveCount);
  }
  EXPECT_EQ(test.size(), test.capacity());
  test.emplace_back(counts);
  // Every element except the last has to be moved to the new position
  EXPECT_EQ(0, counts.copyCount);
  EXPECT_EQ(test.size() - 1, counts.moveCount);
  EXPECT_LT(test.size(), test.capacity());
}

TEST(smallVector, RVPushBackEfficiency) {
  small_vector<Counter, 2> test;
  Counts counts;
  for (size_t i = 1; i <= test.capacity(); ++i) {
    test.push_back(Counter(counts));
    // 1 copy for each push_back()
    EXPECT_EQ(0, counts.copyCount);
    EXPECT_EQ(i, counts.moveCount);
  }
  EXPECT_EQ(test.size(), test.capacity());
  test.push_back(Counter(counts));
  // 1 move for each push_back()
  // Every element except the last has to be moved to the new position
  EXPECT_EQ(0, counts.copyCount);
  EXPECT_EQ(test.size() + test.size() - 1, counts.moveCount);
  EXPECT_LT(test.size(), test.capacity());
}

TEST(smallVector, CLVPushBackEfficiency) {
  small_vector<Counter, 2> test;
  Counts counts;
  Counter const counter(counts);
  for (size_t i = 1; i <= test.capacity(); ++i) {
    test.push_back(counter);
    // 1 copy for each push_back()
    EXPECT_EQ(i, counts.copyCount);
    EXPECT_EQ(0, counts.moveCount);
  }
  EXPECT_EQ(test.size(), test.capacity());
  test.push_back(counter);
  // 1 copy for each push_back()
  EXPECT_EQ(test.size(), counts.copyCount);
  // Every element except the last has to be moved to the new position
  EXPECT_EQ(test.size() - 1, counts.moveCount);
  EXPECT_LT(test.size(), test.capacity());
}

TEST(smallVector, StorageForSortedVectorMap) {
  folly::small_sorted_vector_map<int32_t, int32_t, 2> test;
  test.insert(std::make_pair(10, 10));
  EXPECT_EQ(test.size(), 1);
  test.insert(std::make_pair(10, 10));
  EXPECT_EQ(test.size(), 1);
  test.insert(std::make_pair(20, 10));
  EXPECT_EQ(test.size(), 2);
  test.insert(std::make_pair(30, 10));
  EXPECT_EQ(test.size(), 3);
}

TEST(smallVector, NoHeapStorageForSortedVectorMap) {
  noheap_sorted_vector_map<int32_t, int32_t, 2> test;
  test.insert(std::make_pair(10, 10));
  EXPECT_EQ(test.size(), 1);
  test.insert(std::make_pair(10, 10));
  EXPECT_EQ(test.size(), 1);
  test.insert(std::make_pair(20, 10));
  EXPECT_EQ(test.size(), 2);
  EXPECT_THROW(test.insert(std::make_pair(30, 10)), std::length_error);
  EXPECT_EQ(test.size(), 2);
}

TEST(smallVector, StorageForSortedVectorSet) {
  folly::small_sorted_vector_set<int32_t, 2> test;
  test.insert(10);
  EXPECT_EQ(test.size(), 1);
  test.insert(10);
  EXPECT_EQ(test.size(), 1);
  test.insert(20);
  EXPECT_EQ(test.size(), 2);
  test.insert(30);
  EXPECT_EQ(test.size(), 3);
}

TEST(smallVector, NoHeapStorageForSortedVectorSet) {
  noheap_sorted_vector_set<int32_t, 2> test;
  test.insert(10);
  EXPECT_EQ(test.size(), 1);
  test.insert(10);
  EXPECT_EQ(test.size(), 1);
  test.insert(20);
  EXPECT_EQ(test.size(), 2);
  EXPECT_THROW(test.insert(30), std::length_error);
  EXPECT_EQ(test.size(), 2);
}

TEST(smallVector, SelfMoveAssignmentForVectorOfPair) {
  folly::small_vector<std::pair<int, int>, 2> test;
  test.emplace_back(13, 2);
  EXPECT_EQ(test.size(), 1);
  EXPECT_EQ(test[0].first, 13);
  test = std::move(std::move(test)); // suppress self-move warning
  EXPECT_EQ(test.size(), 1);
  EXPECT_EQ(test[0].first, 13);
}

TEST(smallVector, SelfCopyAssignmentForVectorOfPair) {
  folly::small_vector<std::pair<int, int>, 2> test;
  test.emplace_back(13, 2);
  EXPECT_EQ(test.size(), 1);
  EXPECT_EQ(test[0].first, 13);
  test = static_cast<decltype(test)&>(test); // suppress self-assign warning
  EXPECT_EQ(test.size(), 1);
  EXPECT_EQ(test[0].first, 13);
}

namespace {
struct NonAssignableType {
  int const i_{};
};
} // namespace

TEST(smallVector, PopBackNonAssignableType) {
  small_vector<NonAssignableType> v;
  v.emplace_back();
  EXPECT_EQ(1, v.size());
  v.pop_back();
  EXPECT_EQ(0, v.size());
}

TEST(smallVector, erase) {
  small_vector<int> v(3);
  std::iota(v.begin(), v.end(), 1);
  v.push_back(2);
  erase(v, 2);
  ASSERT_EQ(2u, v.size());
  EXPECT_EQ(1u, v[0]);
  EXPECT_EQ(3u, v[1]);
}

TEST(smallVector, eraseIf) {
  small_vector<int> v(6);
  std::iota(v.begin(), v.end(), 1);
  erase_if(v, [](const auto& x) { return x % 2 == 0; });
  ASSERT_EQ(3u, v.size());
  EXPECT_EQ(1u, v[0]);
  EXPECT_EQ(3u, v[1]);
  EXPECT_EQ(5u, v[2]);
}

namespace {

class NonTrivialInt {
 public:
  NonTrivialInt() {}
  /* implicit */ NonTrivialInt(int value)
      : value_(std::make_shared<int>(value)) {}

  operator int() const { return *value_; }

 private:
  std::shared_ptr<const int> value_;
};

// Move and copy constructor and assignment have several special cases depending
// on relative sizes, so test all combinations.
template <class T, size_t N>
void testMoveAndCopy() {
  const auto fill = [](auto& v, size_t n) {
    v.resize(n);
    std::iota(v.begin(), v.end(), 0);
  };
  const auto verify = [](const auto& v, size_t n) {
    ASSERT_EQ(v.size(), n);
    for (size_t i = 0; i < n; ++i) {
      EXPECT_EQ(v[i], i);
    }
  };

  using Vec = small_vector<T, N>;
  SCOPED_TRACE(fmt::format("N = {}", N));

  const size_t kMinCapacity = Vec{}.capacity();

  for (size_t from = 0; from < 16; ++from) {
    SCOPED_TRACE(fmt::format("from = {}", from));

    {
      SCOPED_TRACE("Move-construction");
      Vec a;
      fill(a, from);
      const auto aCapacity = a.capacity();
      Vec b = std::move(a);
      verify(b, from);
      EXPECT_EQ(b.capacity(), aCapacity);
      verify(a, 0);
      EXPECT_EQ(a.capacity(), kMinCapacity);
    }
    {
      SCOPED_TRACE("Copy-construction");
      Vec a;
      fill(a, from);
      Vec b = a;
      verify(b, from);
      verify(a, from);
    }

    for (size_t to = 0; to < 16; ++to) {
      SCOPED_TRACE(fmt::format("to = {}", to));
      {
        SCOPED_TRACE("Move-assignment");
        Vec a;
        fill(a, from);
        Vec b;
        fill(b, to);

        const auto aCapacity = a.capacity();
        b = std::move(a);
        verify(b, from);
        EXPECT_EQ(b.capacity(), aCapacity);
        verify(a, 0);
        EXPECT_EQ(a.capacity(), kMinCapacity);
      }
      {
        SCOPED_TRACE("Copy-assignment");
        Vec a;
        fill(a, from);
        Vec b;
        fill(b, to);

        b = a;
        verify(b, from);
        verify(a, from);
      }
      {
        SCOPED_TRACE("swap");
        Vec a;
        fill(a, from);
        Vec b;
        fill(b, to);

        const auto aCapacity = a.capacity();
        const auto bCapacity = b.capacity();
        swap(a, b);
        verify(b, from);
        EXPECT_EQ(b.capacity(), aCapacity);
        verify(a, to);
        EXPECT_EQ(a.capacity(), bCapacity);
      }
    }
  }
}

} // namespace

TEST(smallVector, MoveAndCopyTrivial) {
  testMoveAndCopy<int, 0>();
  // Capacity does not fit inline.
  testMoveAndCopy<int, 2>();
  // Capacity does fits inline.
  testMoveAndCopy<int, 4>();
}

TEST(smallVector, MoveAndCopyNonTrivial) {
  testMoveAndCopy<NonTrivialInt, 0>();
  testMoveAndCopy<NonTrivialInt, 4>();
}

TEST(smallVector, PolicyMaxSizeExceeded) {
  struct Obj {
    char a[350];
  };

  small_vector<Obj, 10, policy_size_type<uint8_t>> v;

  EXPECT_THROW(
      for (size_t i = 0; i < 0x100; ++i) { v.push_back(Obj()); },
      std::length_error);
}

TEST(smallVector, Hashable) {
  small_vector<int> v0{{0, 1}};
  small_vector<int> v1{{1, 2}};
  std::unordered_set<small_vector<int>> s;
  s.insert(v0);
  EXPECT_NE(s.end(), s.find(v0));
  EXPECT_EQ(s.end(), s.find(v1));
}

TEST(smallVector, overflowConstruct) {
  EXPECT_THROW(
      folly::small_vector<std::string>(SIZE_MAX / sizeof(std::string) + 1),
      std::length_error);
}

TEST(smallVector, overflowResize) {
  folly::small_vector<std::string> vec;
  EXPECT_THROW(
      vec.resize(SIZE_MAX / sizeof(std::string) + 1), std::length_error);
}

TEST(smallVector, overflowAssign) {
  folly::small_vector<std::string> vec;
  EXPECT_THROW(
      vec.assign(SIZE_MAX / sizeof(std::string) + 1, "hello"),
      std::length_error);
}

TEST(smallVector, assignZeroElementsNoInline) {
  folly::small_vector<int, 0> v;
  v.assign(0, 42);
  EXPECT_TRUE(v.empty());
}

namespace {
struct MaybeThrowOnCopy {
  std::unique_ptr<bool> throwOnCopy;

  explicit MaybeThrowOnCopy(bool t) : throwOnCopy(std::make_unique<bool>(t)) {}
  MaybeThrowOnCopy(const MaybeThrowOnCopy& other)
      : throwOnCopy(std::make_unique<bool>(other.throwOnCopy)) {
    if (*other.throwOnCopy) {
      throw std::runtime_error{"test"};
    }
  }
};
} // namespace

TEST(smallVector, rangeConstructorForwardIteratorThrows) {
  std::array<MaybeThrowOnCopy, 3> arr = {
      MaybeThrowOnCopy{false}, MaybeThrowOnCopy{true}, MaybeThrowOnCopy{false}};

  using ForwardIterator =
      folly::index_iterator<std::array<MaybeThrowOnCopy, 3>>;

  auto first = ForwardIterator{arr, 0};
  auto last = ForwardIterator{arr, arr.size()};

  using SV1 = small_vector<MaybeThrowOnCopy, 1>;
  using SV3 = small_vector<MaybeThrowOnCopy, 3>;

  EXPECT_THROW(SV1(first, last), std::runtime_error);
  EXPECT_THROW(SV3(first, last), std::runtime_error);
}

TEST(smallVector, rangeConstructorInputIteratorThrows) {
  std::array<MaybeThrowOnCopy, 3> arr = {
      MaybeThrowOnCopy{false}, MaybeThrowOnCopy{true}, MaybeThrowOnCopy{false}};

  class InputIterator
      : public folly::index_iterator<std::array<MaybeThrowOnCopy, 3>> {
   public:
    using difference_type = std::ptrdiff_t;
    using value_type = MaybeThrowOnCopy const;
    using reference = MaybeThrowOnCopy const&;
    using pointer = MaybeThrowOnCopy const*;
    using iterator_category = std::input_iterator_tag;

    using index_iterator<std::array<MaybeThrowOnCopy, 3>>::index_iterator;
  };

  auto first = InputIterator{arr, 0};
  auto last = InputIterator{arr, arr.size()};

  using SV1 = small_vector<MaybeThrowOnCopy, 1>;
  using SV3 = small_vector<MaybeThrowOnCopy, 3>;

  EXPECT_THROW(SV1(first, last), std::runtime_error);
  EXPECT_THROW(SV3(first, last), std::runtime_error);
}

TEST(smallVector, comparisons) {
  folly::small_vector<int, 3> vec1 = {1, 2, 3, 4, 5};
  folly::small_vector<int, 3> vec2 = {1, 2, 3, 4, 5};
  EXPECT_EQ(vec1, vec2);
  EXPECT_FALSE(vec1 < vec2);
  EXPECT_TRUE(vec1 <= vec2);
  EXPECT_FALSE(vec1 > vec2);
  EXPECT_TRUE(vec1 >= vec2);

#if FOLLY_CPLUSPLUS >= 202002L && defined(__cpp_lib_three_way_comparison)
  EXPECT_EQ(vec1 <=> vec2, std::strong_ordering::equal);
#endif
  vec1.pop_back();
  EXPECT_NE(vec1, vec2);
  EXPECT_TRUE(vec1 < vec2);
  EXPECT_TRUE(vec1 <= vec2);
  EXPECT_FALSE(vec1 > vec2);
  EXPECT_FALSE(vec1 >= vec2);

#if FOLLY_CPLUSPLUS >= 202002L && defined(__cpp_lib_three_way_comparison)
  EXPECT_EQ(vec1 <=> vec2, std::strong_ordering::less);
  EXPECT_EQ(vec2 <=> vec1, std::strong_ordering::greater);
#endif
}

namespace {
struct LessOnly {
  int v;
  friend bool operator==(LessOnly a, LessOnly b) { return a.v == b.v; }
  friend bool operator<(LessOnly a, LessOnly b) { return a.v < b.v; }
};
} // namespace

TEST(smallVector, comparisonsWithoutThreeWay) {
  folly::small_vector<LessOnly, 2> vec1 = {{1}, {2}};
  folly::small_vector<LessOnly, 2> vec2 = {{1}, {3}};
  EXPECT_TRUE(vec1 != vec2);
  EXPECT_FALSE(vec1 != vec1);
  EXPECT_TRUE(vec1 < vec2);
  EXPECT_TRUE(vec1 <= vec2);
  EXPECT_TRUE(vec1 <= vec1);
  EXPECT_FALSE(vec1 > vec2);
  EXPECT_TRUE(vec2 > vec1);
  EXPECT_FALSE(vec1 >= vec2);
  EXPECT_TRUE(vec1 >= vec1);
}

struct NontrivialImmovable {
  NontrivialImmovable() {}
  NontrivialImmovable(const NontrivialImmovable&) = default;
  NontrivialImmovable(NontrivialImmovable&&) = delete;
  ~NontrivialImmovable() = default;

  // Make it non trivial to copy
  NontrivialImmovable& operator=(const NontrivialImmovable&) { return *this; }
};

static_assert(!std::is_trivially_copyable_v<NontrivialImmovable>);

struct TrivialImmovable {
  TrivialImmovable() {}
  TrivialImmovable(const TrivialImmovable&) = default;
  TrivialImmovable(TrivialImmovable&&) = delete;
};

static_assert(std::is_trivially_copyable_v<TrivialImmovable>);

struct TrivialNonCopyableNorMovable {
  TrivialNonCopyableNorMovable() {}
  TrivialNonCopyableNorMovable(const TrivialNonCopyableNorMovable&) = delete;
  TrivialNonCopyableNorMovable(TrivialNonCopyableNorMovable&&) = delete;
};

static_assert(std::is_trivially_copyable_v<TrivialNonCopyableNorMovable>);

TEST(smallVector, ImmovableTypes) {
  // Immovable types can be used to create small_vectors as long as no use to
  // resizing operations is present. We need to make sure that creation of the
  // small_vector with the sized constructor works whether or not they are
  // trivially copyable.
  {
    folly::small_vector<NontrivialImmovable> sv{10};
  }
  {
    folly::small_vector<TrivialImmovable> sv{10};
  }
  {
    folly::small_vector<TrivialNonCopyableNorMovable> sv{10};
  }
  SUCCEED();
}

namespace {
template <typename C, typename... A>
using detect_resize_without_initialization =
    decltype(folly::resizeWithoutInitialization(
        std::declval<C&>(), std::declval<A>()...));
} // namespace

static_assert(
    folly::is_detected_v<
        detect_resize_without_initialization,
        folly::small_vector<int, 4>,
        std::size_t>,
    "resizeWithoutInitialization must be available for trivial elements");
static_assert(
    !folly::is_detected_v<
        detect_resize_without_initialization,
        folly::small_vector<std::string, 4>,
        std::size_t>,
    "resizeWithoutInitialization must be unavailable for non-trivially-destructible elements");
static_assert(
    folly::is_detected_v<
        detect_resize_without_initialization,
        std::vector<int>,
        std::size_t>,
    "std::vector must remain supported");

TEST(smallVector, resizeWithoutInitializationGrowsPreservingPrefix) {
  folly::small_vector<int, 4> sv{1, 2, 3};
  sv.resize_without_initialization(6);
  EXPECT_EQ(sv.size(), 6u);
  EXPECT_GE(sv.capacity(), 6u);
  for (std::size_t i = 3; i < sv.size(); ++i) {
    sv[i] = static_cast<int>(i * 10);
  }
  const folly::small_vector<int, 4> expected{1, 2, 3, 30, 40, 50};
  EXPECT_EQ(sv, expected);
}

TEST(smallVector, resizeWithoutInitializationSpillsToHeapPreservingPrefix) {
  folly::small_vector<int, 2> sv{7, 8};
  sv.resize_without_initialization(100);
  EXPECT_EQ(sv.size(), 100u);
  EXPECT_GE(sv.capacity(), 100u);
  const folly::small_vector<int, 2> prefix{sv[0], sv[1]};
  const folly::small_vector<int, 2> expectedPrefix{7, 8};
  EXPECT_EQ(prefix, expectedPrefix);
}

TEST(smallVector, resizeWithoutInitializationShrinks) {
  folly::small_vector<int, 4> sv{1, 2, 3, 4, 5};
  sv.resize_without_initialization(2);
  const folly::small_vector<int, 4> expected{1, 2};
  EXPECT_EQ(sv, expected);
}

TEST(smallVector, resizeWithoutInitializationFreeFunction) {
  folly::small_vector<uint64_t, 2> sv{5};
  folly::resizeWithoutInitialization(sv, 32);
  EXPECT_EQ(sv.size(), 32u);
  EXPECT_EQ(sv[0], 5u);
}
