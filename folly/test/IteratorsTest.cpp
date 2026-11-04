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

#include <folly/detail/Iterators.h>

#include <map>
#include <vector>

#include <folly/container/Array.h>
#include <folly/portability/GTest.h>
#include <folly/portability/SysTypes.h>

using namespace folly::detail;

namespace {
struct IntArrayIterator
    : IteratorFacade<IntArrayIterator, int const, std::forward_iterator_tag> {
  explicit IntArrayIterator(int const* a) : a_(a) {}
  void increment() { ++a_; }
  int const& dereference() const { return *a_; }
  bool equal(IntArrayIterator const& rhs) const { return rhs.a_ == a_; }
  int const* a_;
};
} // namespace

TEST(IteratorsTest, IterFacadeHasCorrectTraits) {
  using TR = std::iterator_traits<IntArrayIterator>;
  static_assert(std::is_same<TR::value_type, int const>::value);
  static_assert(std::is_same<TR::reference, int const&>::value);
  static_assert(std::is_same<TR::pointer, int const*>::value);
  static_assert(
      std::is_same<TR::iterator_category, std::forward_iterator_tag>::value);
  static_assert(std::is_same<TR::difference_type, ssize_t>::value);
}

TEST(IteratorsTest, SimpleIteratorFacade) {
  static const auto kArray = folly::make_array(42, 43, 44);
  IntArrayIterator end(kArray.data() + kArray.size());
  IntArrayIterator iter(kArray.data());
  EXPECT_NE(iter, end);
  EXPECT_EQ(42, *iter);
  ++iter;
  EXPECT_NE(iter, end);
  EXPECT_EQ(43, *iter);
  ++iter;
  EXPECT_NE(iter, end);
  EXPECT_EQ(44, *iter);
  ++iter;
  EXPECT_EQ(iter, end);
}

namespace {
// Simple iterator adaptor: wraps an int pointer.
struct IntPointerIter
    : IteratorAdaptor<
          IntPointerIter,
          int const*,
          int const,
          std::forward_iterator_tag> {
  using Super = IteratorAdaptor<
      IntPointerIter,
      int const*,
      int const,
      std::forward_iterator_tag>;
  explicit IntPointerIter(int const* ptr) : Super(ptr) {}
};
} // namespace

TEST(IteratorsTest, IterAdaptorHasCorrectTraits) {
  using TR = std::iterator_traits<IntPointerIter>;
  static_assert(std::is_same<TR::value_type, int const>::value);
  static_assert(std::is_same<TR::reference, int const&>::value);
  static_assert(std::is_same<TR::pointer, int const*>::value);
  static_assert(
      std::is_same<TR::iterator_category, std::forward_iterator_tag>::value);
  static_assert(std::is_same<TR::difference_type, ssize_t>::value);
}

TEST(IteratorsTest, IterAdaptorWithPointer) {
  static const auto kArray = folly::make_array(42, 43, 44);
  IntPointerIter end(kArray.data() + kArray.size());
  IntPointerIter iter(kArray.data());
  EXPECT_NE(iter, end);
  EXPECT_EQ(42, *iter);
  ++iter;
  EXPECT_NE(iter, end);
  EXPECT_EQ(43, *iter);
  ++iter;
  EXPECT_NE(iter, end);
  EXPECT_EQ(44, *iter);
  ++iter;
  EXPECT_EQ(iter, end);
}

namespace {
// More complex case: wrap a map iterator, but these provide either the key or
// value.
struct IntMapKeyIter
    : IteratorAdaptor<
          IntMapKeyIter,
          std::map<int, int>::iterator,
          int const,
          std::forward_iterator_tag> {
  using Super = IteratorAdaptor<
      IntMapKeyIter,
      std::map<int, int>::iterator,
      int const,
      std::forward_iterator_tag>;
  explicit IntMapKeyIter(std::map<int, int>::iterator iter) : Super(iter) {}
  int const& dereference() const { return base()->first; }
};

struct IntMapValueIter
    : IteratorAdaptor<
          IntMapValueIter,
          std::map<int, int>::iterator,
          int,
          std::forward_iterator_tag> {
  using Super = IteratorAdaptor<
      IntMapValueIter,
      std::map<int, int>::iterator,
      int,
      std::forward_iterator_tag>;
  explicit IntMapValueIter(std::map<int, int>::iterator iter) : Super(iter) {}
  int& dereference() const { return base()->second; }
};

} // namespace

TEST(IteratorsTest, IterAdaptorOfOtherIter) {
  std::map<int, int> m{{2, 42}, {3, 43}, {4, 44}};

  IntMapKeyIter keyEnd(m.end());
  IntMapKeyIter keyIter(m.begin());
  EXPECT_NE(keyIter, keyEnd);
  EXPECT_EQ(2, *keyIter);
  ++keyIter;
  EXPECT_NE(keyIter, keyEnd);
  EXPECT_EQ(3, *keyIter);
  ++keyIter;
  EXPECT_NE(keyIter, keyEnd);
  EXPECT_EQ(4, *keyIter);
  ++keyIter;
  EXPECT_EQ(keyIter, keyEnd);

  IntMapValueIter valueEnd(m.end());
  IntMapValueIter valueIter(m.begin());
  EXPECT_NE(valueIter, valueEnd);
  EXPECT_EQ(42, *valueIter);
  ++valueIter;
  EXPECT_NE(valueIter, valueEnd);
  EXPECT_EQ(43, *valueIter);
  ++valueIter;
  EXPECT_NE(valueIter, valueEnd);
  EXPECT_EQ(44, *valueIter);
  ++valueIter;
  EXPECT_EQ(valueIter, valueEnd);
}

namespace {
struct IntMapValueIterConst
    : IteratorAdaptor<
          IntMapValueIterConst,
          std::map<int, int>::const_iterator,
          int const,
          std::forward_iterator_tag> {
  using Super = IteratorAdaptor<
      IntMapValueIterConst,
      std::map<int, int>::const_iterator,
      int const,
      std::forward_iterator_tag>;
  explicit IntMapValueIterConst(std::map<int, int>::const_iterator iter)
      : Super(iter) {}
  /* implicit */ IntMapValueIterConst(IntMapValueIter const& rhs)
      : IntMapValueIterConst(rhs.base()) {}
  int const& dereference() const { return base()->second; }
};
} // namespace

TEST(IteratorsTest, MixedConstAndNonconstIters) {
  std::map<int, int> m{{2, 42}, {3, 43}, {4, 44}};
  IntMapValueIterConst cend(m.cend());
  IntMapValueIter valueIter(m.begin());
  EXPECT_NE(valueIter, cend);
  ++valueIter;
  ++valueIter;
  ++valueIter;
  EXPECT_EQ(valueIter, cend);
}

namespace {
// Random-access adaptor whose iteration methods are private.
class IntVectorIter
    : public IteratorAdaptor<
          IntVectorIter,
          std::vector<int>::iterator,
          int,
          std::random_access_iterator_tag> {
  using Super = IteratorAdaptor<
      IntVectorIter,
      std::vector<int>::iterator,
      int,
      std::random_access_iterator_tag>;
  friend Super::Super;

 public:
  IntVectorIter() = default;
  explicit IntVectorIter(std::vector<int>::iterator iter) : Super(iter) {}

 private:
  int& dereference() const { return *base_reference(); }
};

// Iterator whose reference type is a proxy.
struct Proxy {
  int* p;
  int get() const { return *p; }
};

class ProxyIter
    : public IteratorFacade<
          ProxyIter,
          int,
          std::random_access_iterator_tag,
          Proxy,
          long> {
 public:
  explicit ProxyIter(int* p) : p_(p) {}

 private:
  friend IteratorFacade;

  Proxy dereference() const { return Proxy{p_}; }
  bool equal(ProxyIter const& rhs) const { return p_ == rhs.p_; }
  void increment() { ++p_; }
  void decrement() { --p_; }
  void advance(long n) { p_ += n; }
  long distance_to(ProxyIter const& rhs) const { return rhs.p_ - p_; }

  int* p_;
};
} // namespace

TEST(IteratorsTest, RandomAccessIterAdaptor) {
  using TR = std::iterator_traits<IntVectorIter>;
  static_assert(std::is_same_v<TR::reference, int&>);
  static_assert(std::is_same_v<TR::pointer, int*>);
  static_assert(
      std::is_same_v<TR::iterator_category, std::random_access_iterator_tag>);
  static_assert(std::random_access_iterator<IntVectorIter>);

  std::vector<int> v{40, 41, 42, 43, 44};
  IntVectorIter begin(v.begin());
  IntVectorIter end(v.end());
  EXPECT_EQ(5, end - begin);
  EXPECT_EQ(-5, begin - end);
  EXPECT_EQ(42, begin[2]);
  EXPECT_EQ(43, *(begin + 3));
  EXPECT_EQ(43, *(3 + begin));
  EXPECT_EQ(44, *(end - 1));
  EXPECT_TRUE(begin < end);
  EXPECT_TRUE(begin <= begin);
  EXPECT_TRUE(end > begin);
  EXPECT_TRUE(end >= end);
  EXPECT_FALSE(end < begin);
  auto it = begin;
  it += 4;
  EXPECT_EQ(44, *it);
  it -= 2;
  EXPECT_EQ(42, *it);
  EXPECT_EQ(42, *it--);
  EXPECT_EQ(41, *it);
  *it = 7;
  EXPECT_EQ(7, v[1]);
}

TEST(IteratorsTest, ProxyReference) {
  using TR = std::iterator_traits<ProxyIter>;
  static_assert(std::is_same_v<TR::reference, Proxy>);
  static_assert(std::is_same_v<TR::difference_type, long>);

  int a[] = {1, 2, 3};
  ProxyIter begin(a);
  ProxyIter end(a + 3);
  EXPECT_EQ(3, end - begin);
  EXPECT_EQ(2, (*++begin).get());
  EXPECT_EQ(2, begin->get());
  EXPECT_EQ(3, begin[1].get());
  EXPECT_TRUE(begin < end);
}
