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

#include <folly/FBVector.h>

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <numeric>
#include <vector>

#include <ranges>
#include <folly/FBString.h>
#include <folly/Random.h>
#include <folly/Traits.h>
#include <folly/container/irange.h>
#include <folly/portability/GTest.h>
#include <folly/test/FBVectorTestUtil.h>

using namespace std;
using namespace folly;
using namespace folly::test::detail;

using IntFBVector = fbvector<int>;
using FBStringFBVector = fbvector<fbstring>;

#define VECTOR IntFBVector
#include <folly/container/test/FBVectorTests.cpp.h> // nolint
#undef VECTOR
#define VECTOR FBStringFBVector
#include <folly/container/test/FBVectorTests.cpp.h> // nolint
#undef VECTOR

TEST(fbvector, clause233613Ambiguity) {
  fbvector<int> v(10, 20);
  EXPECT_EQ(v.size(), 10);
  for (const auto& element : v) {
    EXPECT_EQ(element, 20);
  }
}

TEST(fbvector, clause2336111Ambiguity) {
  fbvector<int> v;
  v.assign(10, 20);
  EXPECT_EQ(v.size(), 10);
  for (const auto& element : v) {
    EXPECT_EQ(element, 20);
  }
}

TEST(fbvector, clause233626) {
  fbvector<int> v;
  auto const n = random(0U, 10000U);
  v.reserve(n);
  auto const n1 = random(0U, 10000U);
  auto const obj = randomObject<int>();
  v.assign(n1, obj);
  v.shrink_to_fit();
  // Nothing to verify except that the call made it through
}

TEST(fbvector, clause23364Ambiguity) {
  fbvector<int> v;
  fbvector<int>::const_iterator it = v.end();
  v.insert(it, 10, 20);
  EXPECT_EQ(v.size(), 10);
  for (auto i : v) {
    EXPECT_EQ(i, 20);
  }
}

TEST(fbvector, composition) {
  fbvector<fbvector<double>> matrix(100, fbvector<double>(100));
}

TEST(fbvector, worksWithStdString) {
  fbvector<std::string> v(10, "hello");
  EXPECT_EQ(v.size(), 10);
  v.push_back("world");
}

namespace {
struct UserDefinedType {
  int whatevs_;
};
} // namespace

FOLLY_ASSUME_FBVECTOR_COMPATIBLE(UserDefinedType)

TEST(fbvector, worksWithUserDefinedType) {
  fbvector<UserDefinedType> v(10);
  EXPECT_EQ(v.size(), 10);
  v.push_back(UserDefinedType());
}

namespace {
struct SelfReferentialUserDefinedType {
  // SelfReferentialUserDefinedType is incomplete here,
  // but fbvector should still be able to compile.
  fbvector<SelfReferentialUserDefinedType> children;
};
} // namespace

TEST(fbvector, selfReferentialType) {
  SelfReferentialUserDefinedType root;
  root.children.push_back(SelfReferentialUserDefinedType{});
  EXPECT_EQ(root.children.size(), 1);
}

TEST(fbvector, moveConstruction) {
  fbvector<int> v1(100, 100);
  fbvector<int> v2;
  EXPECT_EQ(v1.size(), 100);
  EXPECT_EQ(v1.front(), 100);
  EXPECT_EQ(v2.size(), 0);
  v2 = std::move(v1);
  EXPECT_EQ(v1.size(), 0);
  EXPECT_EQ(v2.size(), 100);
  EXPECT_EQ(v2.front(), 100);

  v1.assign(100, 100);
  auto other = std::move(v1);
  EXPECT_EQ(v1.size(), 0);
  EXPECT_EQ(other.size(), 100);
  EXPECT_EQ(other.front(), 100);
}

TEST(fbvector, emplace) {
  fbvector<std::string> s(12, "asd");
  EXPECT_EQ(s.size(), 12);
  EXPECT_EQ(s.front(), "asd");
  const auto& emplaced = s.emplace_back("funk");
  EXPECT_EQ(emplaced, "funk");
  EXPECT_EQ(s.back(), "funk");
  EXPECT_EQ(std::addressof(emplaced), std::addressof(s.back()));
}

TEST(fbvector, initializerLists) {
  fbvector<int> vec = {1, 2, 3};
  EXPECT_EQ(vec.size(), 3);
  EXPECT_EQ(vec[0], 1);
  EXPECT_EQ(vec[1], 2);
  EXPECT_EQ(vec[2], 3);

  vec = {0, 0, 12, 16};
  EXPECT_EQ(vec.size(), 4);
  EXPECT_EQ(vec[0], 0);
  EXPECT_EQ(vec[1], 0);
  EXPECT_EQ(vec[2], 12);
  EXPECT_EQ(vec[3], 16);

  vec.insert(vec.begin() + 1, {23, 23});
  EXPECT_EQ(vec.size(), 6);
  EXPECT_EQ(vec[0], 0);
  EXPECT_EQ(vec[1], 23);
  EXPECT_EQ(vec[2], 23);
  EXPECT_EQ(vec[3], 0);
  EXPECT_EQ(vec[4], 12);
  EXPECT_EQ(vec[5], 16);
}

TEST(fbvector, uniquePtr) {
  fbvector<std::unique_ptr<int>> v(12);
  std::unique_ptr<int> p(new int(12));
  v.push_back(std::move(p));
  EXPECT_EQ(*v.back(), 12);

  v[0] = std::move(p);
  EXPECT_FALSE(v[0].get());
  v[0] = std::make_unique<int>(32);
  std::unique_ptr<int> somePtr;
  v.insert(v.begin(), std::move(somePtr));
  EXPECT_EQ(*v[1], 32);
}

TEST(FBVector, task858056) {
  fbvector<fbstring> cycle;
  cycle.push_back("foo");
  cycle.push_back("bar");
  cycle.push_back("baz");
  fbstring message("Cycle detected: ");
  for (const auto& node_name : cycle | views::reverse) {
    message += "[";
    message += node_name;
    message += "] ";
  }
  EXPECT_EQ("Cycle detected: [baz] [bar] [foo] ", message);
}

TEST(FBVector, moveIterator) {
  fbvector<int> base = {0, 1, 2};

  auto cp1 = base;
  fbvector<int> fbvi1(
      std::make_move_iterator(cp1.begin()), std::make_move_iterator(cp1.end()));
  EXPECT_EQ(fbvi1, base);

  auto cp2 = base;
  fbvector<int> fbvi2;
  fbvi2.assign(
      std::make_move_iterator(cp2.begin()), std::make_move_iterator(cp2.end()));
  EXPECT_EQ(fbvi2, base);

  auto cp3 = base;
  fbvector<int> fbvi3;
  fbvi3.insert(
      fbvi3.end(),
      std::make_move_iterator(cp3.begin()),
      std::make_move_iterator(cp3.end()));
  EXPECT_EQ(fbvi3, base);
}

TEST(FBVector, reserveConsistency) {
  struct S {
    int64_t a, b, c, d;
  };

  fbvector<S> fb1;
  for (size_t i = 0; i < 1000; ++i) {
    fb1.reserve(1);
    EXPECT_EQ(fb1.size(), 0);
    fb1.shrink_to_fit();
  }
}

TEST(FBVector, vectorOfMaps) {
  fbvector<std::map<std::string, std::string>> v;

  v.push_back(std::map<std::string, std::string>());
  v.push_back(std::map<std::string, std::string>());

  EXPECT_EQ(2, v.size());

  v[1]["hello"] = "world";
  EXPECT_EQ(0, v[0].size());
  EXPECT_EQ(1, v[1].size());

  v[0]["foo"] = "bar";
  EXPECT_EQ(1, v[0].size());
  EXPECT_EQ(1, v[1].size());
}

TEST(FBVector, shrinkToFitAfterClear) {
  fbvector<int> fb1;
  fb1.push_back(42);
  fb1.push_back(1337);
  fb1.clear();
  fb1.shrink_to_fit();
  EXPECT_EQ(fb1.size(), 0);
  EXPECT_EQ(fb1.capacity(), 0);
}

TEST(FBVector, zeroLen) {
  fbvector<int> fb1(0);
  fbvector<int> fb2(0, 10);
  fbvector<int> fb3(std::move(fb1));
  fbvector<int> fb4;
  fb4 = std::move(fb2);
  fbvector<int> fb5 = fb3;
  fbvector<int> fb6;
  fb6 = fb4;
  std::initializer_list<int> il = {};
  fb6 = il;
  fbvector<int> fb7(fb6.begin(), fb6.end());
}

TEST(FBVector, deductionGuides) {
  fbvector<int> v(3);

  fbvector x(v.begin(), v.end());
  EXPECT_TRUE((std::is_same_v<fbvector<int>, decltype(x)>));

  fbvector y{v.begin(), v.end()};
  EXPECT_TRUE((std::is_same_v<fbvector<fbvector<int>::iterator>, decltype(y)>));
}

TEST(FBVector, erase) {
  fbvector<int> v(3);
  std::iota(v.begin(), v.end(), 1);
  v.push_back(2);
  erase(v, 2);
  ASSERT_EQ(2u, v.size());
  EXPECT_EQ(1u, v[0]);
  EXPECT_EQ(3u, v[1]);
}

TEST(FBVector, eraseIf) {
  fbvector<int> v(6);
  std::iota(v.begin(), v.end(), 1);
  erase_if(v, [](const auto& x) { return x % 2 == 0; });
  ASSERT_EQ(3u, v.size());
  EXPECT_EQ(1u, v[0]);
  EXPECT_EQ(3u, v[1]);
  EXPECT_EQ(5u, v[2]);
}

TEST(FBVector, overflowConstruct) {
  EXPECT_THROW(
      folly::fbvector<std::string>(SIZE_MAX / sizeof(std::string) + 1),
      std::length_error);
}

TEST(FBVector, overflowResize) {
  folly::fbvector<std::string> vec;
  EXPECT_THROW(vec.resize(SIZE_MAX / sizeof(string) + 1), std::length_error);
}

TEST(FBVector, overflowAssign) {
  folly::fbvector<std::string> vec;
  EXPECT_THROW(
      vec.assign(SIZE_MAX / sizeof(std::string) + 1, "hello"),
      std::length_error);
}

namespace {
struct alignas(128) AlignedTrivial {
  int value = 0;
};

struct alignas(256) AlignedNontrivial {
  int value = 0;
  std::unique_ptr<int> owned = std::make_unique<int>(42);
};

template <typename T>
class FBVectorAlignment : public testing::Test {};

using AlignedTypes = testing::Types<AlignedTrivial, AlignedNontrivial>;
TYPED_TEST_SUITE(FBVectorAlignment, AlignedTypes);

TYPED_TEST(FBVectorAlignment, PreservesAlignmentAcrossCapacityChanges) {
  // Retain allocations so repeated attempts do not merely reuse one lucky address.
  std::vector<fbvector<TypeParam>> allocations;
  for (size_t attempt = 0; attempt < 32; ++attempt) {
    SCOPED_TRACE(attempt);
    fbvector<TypeParam> values;
    values.reserve(5 + attempt);
    ASSERT_EQ(0, reinterpret_cast<uintptr_t>(values.data()) % alignof(TypeParam));
    values.resize(5);
    for (size_t i = 0; i < values.size(); ++i) {
      values[i].value = static_cast<int>(i + 1);
    }

    values.reserve(64 + attempt);
    ASSERT_EQ(0, reinterpret_cast<uintptr_t>(values.data()) % alignof(TypeParam));
    values.resize(values.capacity());
    values.emplace_back();
    ASSERT_EQ(0, reinterpret_cast<uintptr_t>(values.data()) % alignof(TypeParam));

    values.resize(33);
    values.shrink_to_fit();
    ASSERT_EQ(0, reinterpret_cast<uintptr_t>(values.data()) % alignof(TypeParam));
    for (size_t i = 0; i < 5; ++i) {
      EXPECT_EQ(static_cast<int>(i + 1), values[i].value);
    }
    allocations.push_back(std::move(values));
  }
}
} // namespace

#ifndef _MSC_VER
TEST(FBVector, zeroInit) {
  // This is a higher-level version of TEST(Traits, zeroInit).
  struct S1 {
    int i_;
  };
  struct S3 {
    int S1::* mp_;
  };
  folly::fbvector<S3> vec(4);
  vec.resize(10);
  EXPECT_EQ(vec[0].mp_, nullptr);
  EXPECT_EQ(vec[8].mp_, nullptr);
}
#endif
