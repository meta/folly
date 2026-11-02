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

#include <folly/container/Iterator.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <deque>
#include <functional>
#include <istream>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <numeric>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <folly/portability/GTest.h>

class IteratorTest : public testing::Test {};

TEST_F(IteratorTest, iterator_has_known_distance_v) {
  EXPECT_FALSE((folly::iterator_has_known_distance_v<int*, int const*>));
  EXPECT_TRUE((folly::iterator_has_known_distance_v<int*, int*>));
}

TEST_F(IteratorTest, range_has_known_distance_v) {
  EXPECT_FALSE(folly::range_has_known_distance_v<std::list<int>&>);
  EXPECT_TRUE(folly::range_has_known_distance_v<std::vector<int>&>);
}

TEST_F(IteratorTest, iterator_category_t) {
  EXPECT_TRUE(( //
      std::is_same_v<
          folly::iterator_category_t<
              std::iterator<std::input_iterator_tag, int>>,
          std::input_iterator_tag>));
  EXPECT_FALSE(( //
      std::is_same_v<
          folly::iterator_category_t<
              std::iterator<std::input_iterator_tag, int>>,
          std::output_iterator_tag>));
}

TEST_F(IteratorTest, iterator_concept_t) {
  auto ints_iota = std::views::iota(0, 1);
  using iota_iterator = decltype(ints_iota.begin());
  using istream_iterator = std::ranges::iterator_t<
      std::ranges::basic_istream_view<int, char, std::char_traits<char>>>;

  EXPECT_TRUE(( //
      std::is_same_v<
          folly::iterator_concept_t<int*>,
          std::contiguous_iterator_tag>));
  EXPECT_TRUE(( //
      std::is_same_v<
          folly::iterator_concept_t<const int*>,
          std::contiguous_iterator_tag>));
  EXPECT_TRUE(( //
      std::is_same_v<
          folly::iterator_concept_t<std::vector<int>::iterator>,
          std::contiguous_iterator_tag>));
  EXPECT_TRUE(( //
      std::is_same_v<
          folly::iterator_concept_t<std::reverse_iterator<int*>>,
          std::random_access_iterator_tag>));
  EXPECT_TRUE(( //
      std::is_same_v<
          folly::iterator_concept_t<folly::reverse_iterator<int*>>,
          std::random_access_iterator_tag>));
  EXPECT_TRUE(( //
      std::is_same_v<
          folly::iterator_concept_t<iota_iterator>,
          std::random_access_iterator_tag>));
  EXPECT_TRUE(( //
      std::is_same_v<
          folly::iterator_concept_t<std::list<int>::iterator>,
          std::bidirectional_iterator_tag>));
  EXPECT_TRUE(( //
      std::is_same_v<
          folly::iterator_concept_t<istream_iterator>,
          std::input_iterator_tag>));
  EXPECT_TRUE(( //
      std::is_same_v<
          folly::iterator_concept_t<
              std::iterator<std::forward_iterator_tag, int>>,
          std::forward_iterator_tag>));
}

TEST_F(IteratorTest, iterator_category_matches_v) {
  EXPECT_TRUE(( //
      folly::iterator_category_matches_v<
          std::iterator<std::input_iterator_tag, int>,
          std::input_iterator_tag>));
  EXPECT_FALSE(( //
      folly::iterator_category_matches_v<
          std::iterator<std::input_iterator_tag, int>,
          std::output_iterator_tag>));
  EXPECT_FALSE(( //
      folly::iterator_category_matches_v<int, std::input_iterator_tag>));
}

TEST_F(IteratorTest, iterator_value_type_t) {
  EXPECT_TRUE(( //
      std::is_same_v<
          std::map<int, double>::value_type,
          folly::iterator_value_type_t<std::map<int, double>::iterator>>));
  EXPECT_TRUE(( //
      std::is_same_v<
          std::map<std::reference_wrapper<int>, double&>::value_type,
          folly::iterator_value_type_t<
              std::map<std::reference_wrapper<int>, double&>::iterator>>));
  EXPECT_FALSE(( //
      std::is_same_v<
          std::map<int, float>::value_type,
          folly::iterator_value_type_t<std::map<int, double>::iterator>>));
}

TEST_F(IteratorTest, iterator_key_type_t) {
  EXPECT_TRUE(( //
      std::is_same_v<
          std::map<int, double>::key_type,
          folly::iterator_key_type_t<std::map<int, double>::iterator>>));
  EXPECT_TRUE(( //
      std::is_same_v<
          std::map<std::reference_wrapper<int>, double&>::key_type,
          folly::iterator_key_type_t<
              std::map<std::reference_wrapper<int>, double&>::iterator>>));
  EXPECT_TRUE(( //
      std::is_same_v<
          int,
          folly::iterator_key_type_t<std::iterator<
              std::input_iterator_tag,
              std::pair<const int&, double>>>>));
  EXPECT_FALSE(( //
      std::is_same_v<
          std::map<char, double>::key_type,
          folly::iterator_key_type_t<std::map<int, double>::iterator>>));
}

TEST_F(IteratorTest, iterator_mapped_type_t) {
  EXPECT_TRUE(( //
      std::is_same_v<
          std::map<int, double>::mapped_type,
          folly::iterator_mapped_type_t<std::map<int, double>::iterator>>));
  EXPECT_TRUE(( //
      std::is_same_v<
          std::map<std::reference_wrapper<int>, double&>::mapped_type,
          folly::iterator_mapped_type_t<
              std::map<std::reference_wrapper<int>, double&>::iterator>>));
  EXPECT_FALSE(( //
      std::is_same_v<
          std::map<int, float>::mapped_type,
          folly::iterator_mapped_type_t<std::map<int, double>::iterator>>));
}

namespace {
/**
 * Container type used for unit tests.
 */
template <typename T>
using Container = std::deque<T>;

// Constructor and assignment operator call counters for struct Object.
std::size_t gDefaultCtrCnt;
std::size_t gCopyCtrCnt;
std::size_t gMoveCtrCnt;
std::size_t gExplicitCtrCnt;
std::size_t gMultiargCtrCnt;
std::size_t gCopyOpCnt;
std::size_t gMoveOpCnt;
std::size_t gConvertOpCnt;

/**
 * Class that increases various counters to keep track of how objects have
 * been constructed or assigned to, to verify iterator behavior.
 */
struct Object {
  Object() { ++gDefaultCtrCnt; }
  Object(const Object&) { ++gCopyCtrCnt; }
  Object(Object&&) noexcept { ++gMoveCtrCnt; }
  explicit Object(int) { ++gExplicitCtrCnt; }
  explicit Object(int, int) { ++gMultiargCtrCnt; }
  Object& operator=(const Object&) {
    ++gCopyOpCnt;
    return *this;
  }
  Object& operator=(Object&&) noexcept {
    ++gMoveOpCnt;
    return *this;
  }
  [[maybe_unused]] Object& operator=(int) noexcept {
    ++gConvertOpCnt;
    return *this;
  }
};

/**
 * Reset all call counters to 0.
 */
void init_counters() {
  gDefaultCtrCnt = gCopyCtrCnt = gMoveCtrCnt = gExplicitCtrCnt =
      gMultiargCtrCnt = gCopyOpCnt = gMoveOpCnt = gConvertOpCnt = 0;
}

/**
 * Test for iterator copy and move.
 */
template <typename Iterator>
void copy_and_move_test(Container<int>& q, Iterator it) {
  assert(q.empty());
  const auto it2(it); // copy construct
  it = it2; // copy assign from const
  FOLLY_PUSH_WARNING
  FOLLY_CLANG_DISABLE_WARNING("-Wself-assign-overloaded")
  it = it; // self assign
  FOLLY_POP_WARNING
  auto it3(std::move(it)); // move construct
  it = std::move(it3); // move assign
  // Make sure iterator still works.
  it = 4711; // emplace
  EXPECT_EQ(q, Container<int>{4711});
}

/**
 * Test for emplacement with perfect forwarding.
 */
template <typename Iterator>
void emplace_test(Container<Object>& q, Iterator it) {
  using folly::make_emplace_args;
  assert(q.empty());
  init_counters();
  it = Object{}; // default construct + move construct
  Object obj; // default construct
  it = obj; // copy construct
  it = std::move(obj); // move construct
  const Object obj2; // default construct
  it = obj2; // copy construct from const
  it = std::move(obj2); // copy construct (const defeats move)
  it = 0; // explicit construct
  it = make_emplace_args(0, 0); // explicit multiarg construct
  it = std::make_pair(0, 0); // implicit multiarg construct
  it = std::make_tuple(0, 0); // implicit multiarg construct
  auto args = make_emplace_args(Object{}); // default construct + move construct
  it = args; // copy construct
  it = const_cast<const decltype(args)&>(args); // copy construct from const
  it = std::move(args); // move construct
  auto args2 = std::make_tuple(Object{}); // default construct + move construct
  it = args2; // (implicit multiarg) copy construct
  it = std::move(args2); // (implicit multiarg) move construct
  auto args3 = std::make_pair(0, 0);
  it = args3; // implicit multiarg construct
  it = std::move(args3); // implicit multiarg construct
  ASSERT_EQ(q.size(), 16);
  EXPECT_EQ(gDefaultCtrCnt, 5);
  EXPECT_EQ(gCopyCtrCnt, 6);
  EXPECT_EQ(gMoveCtrCnt, 6);
  EXPECT_EQ(gExplicitCtrCnt, 1);
  EXPECT_EQ(gMultiargCtrCnt, 5);
  EXPECT_EQ(gCopyOpCnt, 0);
  EXPECT_EQ(gMoveOpCnt, 0);
  EXPECT_EQ(gConvertOpCnt, 0);
}
} // namespace

using namespace folly;

/**
 * Basic tests for folly::emplace_iterator.
 */
TEST(EmplaceIterator, EmplacerTest) {
  {
    Container<int> q;
    copy_and_move_test(q, emplacer(q, q.begin()));
  }
  {
    Container<Object> q;
    emplace_test(q, emplacer(q, q.begin()));
  }
  {
    Container<int> q;
    auto it = emplacer(q, q.begin());
    it = 0;
    it = 1;
    it = 2;
    it = emplacer(q, q.begin());
    it = 3;
    it = 4;
    EXPECT_EQ(q, Container<int>({3, 4, 0, 1, 2}));
  }
}

/**
 * Basic tests for folly::front_emplace_iterator.
 */
TEST(EmplaceIterator, FrontEmplacerTest) {
  {
    Container<int> q;
    copy_and_move_test(q, front_emplacer(q));
  }
  {
    Container<Object> q;
    emplace_test(q, front_emplacer(q));
  }
  {
    Container<int> q;
    auto it = front_emplacer(q);
    it = 0;
    it = 1;
    it = 2;
    it = front_emplacer(q);
    it = 3;
    it = 4;
    EXPECT_EQ(q, Container<int>({4, 3, 2, 1, 0}));
  }
}

/**
 * Basic tests for folly::back_emplace_iterator.
 */
TEST(EmplaceIterator, BackEmplacerTest) {
  {
    Container<int> q;
    copy_and_move_test(q, back_emplacer(q));
  }
  {
    Container<Object> q;
    emplace_test(q, back_emplacer(q));
  }
  {
    Container<int> q;
    auto it = back_emplacer(q);
    it = 0;
    it = 1;
    it = 2;
    it = back_emplacer(q);
    it = 3;
    it = 4;
    EXPECT_EQ(q, Container<int>({0, 1, 2, 3, 4}));
  }
}

/**
 * Basic tests for folly::hint_emplace_iterator.
 */
TEST(EmplaceIterator, HintEmplacerTest) {
  {
    init_counters();
    std::map<int, Object> m;
    auto it = hint_emplacer(m, m.end());
    it = make_emplace_args(
        std::piecewise_construct,
        std::forward_as_tuple(0),
        std::forward_as_tuple(0));
    it = make_emplace_args(
        std::piecewise_construct,
        std::forward_as_tuple(1),
        std::forward_as_tuple(0, 0));
    it = make_emplace_args(
        std::piecewise_construct,
        std::forward_as_tuple(2),
        std::forward_as_tuple(Object{}));
    ASSERT_EQ(m.size(), 3);
    EXPECT_EQ(gDefaultCtrCnt, 1);
    EXPECT_EQ(gCopyCtrCnt, 0);
    EXPECT_EQ(gMoveCtrCnt, 1);
    EXPECT_EQ(gExplicitCtrCnt, 1);
    EXPECT_EQ(gMultiargCtrCnt, 1);
    EXPECT_EQ(gCopyOpCnt, 0);
    EXPECT_EQ(gMoveOpCnt, 0);
    EXPECT_EQ(gConvertOpCnt, 0);
  }
  {
    struct O {
      explicit O(int i_) : i(i_) {}
      bool operator<(const O& other) const { return i < other.i; }
      bool operator==(const O& other) const { return i == other.i; }
      int i;
    };
    std::vector<int> v1 = {0, 1, 2, 3, 4};
    std::vector<int> v2 = {0, 2, 4};
    std::set<O> diff;
    std::set_difference(
        v1.begin(),
        v1.end(),
        v2.begin(),
        v2.end(),
        hint_emplacer(diff, diff.end()));
    std::set<O> expected = {O(1), O(3)};
    ASSERT_EQ(diff, expected);
  }
}

/**
 * Test std::copy() with explicit conversion. This would not compile with a
 * std::back_insert_iterator, because the constructor of Object that takes a
 * single int is explicit.
 */
TEST(EmplaceIterator, Copy) {
  init_counters();
  Container<int> in({0, 1, 2});
  Container<Object> out;
  std::copy(in.begin(), in.end(), back_emplacer(out));
  EXPECT_EQ(3, out.size());
  EXPECT_EQ(gDefaultCtrCnt, 0);
  EXPECT_EQ(gCopyCtrCnt, 0);
  EXPECT_EQ(gMoveCtrCnt, 0);
  EXPECT_EQ(gExplicitCtrCnt, 3);
  EXPECT_EQ(gMultiargCtrCnt, 0);
  EXPECT_EQ(gCopyOpCnt, 0);
  EXPECT_EQ(gMoveOpCnt, 0);
  EXPECT_EQ(gConvertOpCnt, 0);
}

/**
 * Test std::transform() with multi-argument constructors. This would require
 * a temporary Object with std::back_insert_iterator.
 */
TEST(EmplaceIterator, Transform) {
  init_counters();
  Container<int> in({0, 1, 2});
  Container<Object> out;
  std::transform(in.begin(), in.end(), back_emplacer(out), [](int i) {
    return make_emplace_args(i, i);
  });
  EXPECT_EQ(3, out.size());
  EXPECT_EQ(gDefaultCtrCnt, 0);
  EXPECT_EQ(gCopyCtrCnt, 0);
  EXPECT_EQ(gMoveCtrCnt, 0);
  EXPECT_EQ(gExplicitCtrCnt, 0);
  EXPECT_EQ(gMultiargCtrCnt, 3);
  EXPECT_EQ(gCopyOpCnt, 0);
  EXPECT_EQ(gMoveOpCnt, 0);
  EXPECT_EQ(gConvertOpCnt, 0);
}

/**
 * Test multi-argument store and forward.
 */
TEST(EmplaceIterator, EmplaceArgs) {
  Object o1;
  const Object o2;
  Object& o3 = o1;
  const Object& o4 = o3;
  Object o5;

  {
    // Test copy construction.
    auto args = make_emplace_args(0, o1, o2, o3, o4, Object{}, std::cref(o2));
    init_counters();
    auto args2 = args;
    EXPECT_EQ(gDefaultCtrCnt, 0);
    EXPECT_EQ(gCopyCtrCnt, 5);
    EXPECT_EQ(gMoveCtrCnt, 0);
    EXPECT_EQ(gExplicitCtrCnt, 0);
    EXPECT_EQ(gMultiargCtrCnt, 0);
    EXPECT_EQ(gCopyOpCnt, 0);
    EXPECT_EQ(gMoveOpCnt, 0);
    EXPECT_EQ(gConvertOpCnt, 0);

    // Test copy assignment.
    init_counters();
    args = args2;
    EXPECT_EQ(gDefaultCtrCnt, 0);
    EXPECT_EQ(gCopyCtrCnt, 0);
    EXPECT_EQ(gMoveCtrCnt, 0);
    EXPECT_EQ(gExplicitCtrCnt, 0);
    EXPECT_EQ(gMultiargCtrCnt, 0);
    EXPECT_EQ(gCopyOpCnt, 5);
    EXPECT_EQ(gMoveOpCnt, 0);
    EXPECT_EQ(gConvertOpCnt, 0);
  }

  {
    // Test RVO.
    init_counters();
    auto args = make_emplace_args(
        0, o1, o2, o3, o4, Object{}, std::cref(o2), rref(std::move(o5)));
    EXPECT_EQ(gDefaultCtrCnt, 1);
    EXPECT_EQ(gCopyCtrCnt, 4);
    EXPECT_EQ(gMoveCtrCnt, 1);
    EXPECT_EQ(gExplicitCtrCnt, 0);
    EXPECT_EQ(gMultiargCtrCnt, 0);
    EXPECT_EQ(gCopyOpCnt, 0);
    EXPECT_EQ(gMoveOpCnt, 0);
    EXPECT_EQ(gConvertOpCnt, 0);

    // Test move construction.
    init_counters();
    auto args2 = std::move(args);
    EXPECT_EQ(gDefaultCtrCnt, 0);
    EXPECT_EQ(gCopyCtrCnt, 0);
    EXPECT_EQ(gMoveCtrCnt, 5);
    EXPECT_EQ(gExplicitCtrCnt, 0);
    EXPECT_EQ(gMultiargCtrCnt, 0);
    EXPECT_EQ(gCopyOpCnt, 0);
    EXPECT_EQ(gMoveOpCnt, 0);
    EXPECT_EQ(gConvertOpCnt, 0);

    // Test move assignment.
    init_counters();
    args = std::move(args2);
    EXPECT_EQ(gDefaultCtrCnt, 0);
    EXPECT_EQ(gCopyCtrCnt, 0);
    EXPECT_EQ(gMoveCtrCnt, 0);
    EXPECT_EQ(gExplicitCtrCnt, 0);
    EXPECT_EQ(gMultiargCtrCnt, 0);
    EXPECT_EQ(gCopyOpCnt, 0);
    EXPECT_EQ(gMoveOpCnt, 5);
    EXPECT_EQ(gConvertOpCnt, 0);

    // Make sure arguments are stored correctly. lvalues by reference, rvalues
    // by (moved) copy. Rvalues cannot be stored by reference because they may
    // refer to an expired temporary by the time they are accessed.
    static_assert(
        std::is_same<
            int,
            std::tuple_element_t<0, decltype(args)::storage_type>>::value);
    static_assert(
        std::is_same<
            Object,
            std::tuple_element_t<1, decltype(args)::storage_type>>::value);
    static_assert(
        std::is_same<
            Object,
            std::tuple_element_t<2, decltype(args)::storage_type>>::value);
    static_assert(
        std::is_same<
            Object,
            std::tuple_element_t<3, decltype(args)::storage_type>>::value);
    static_assert(
        std::is_same<
            Object,
            std::tuple_element_t<4, decltype(args)::storage_type>>::value);
    static_assert(
        std::is_same<
            Object,
            std::tuple_element_t<5, decltype(args)::storage_type>>::value);
    static_assert(
        std::is_same<
            std::reference_wrapper<const Object>,
            std::tuple_element_t<6, decltype(args)::storage_type>>::value);
    static_assert(
        std::is_same<
            rvalue_reference_wrapper<Object>,
            std::tuple_element_t<7, decltype(args)::storage_type>>::value);

    // Check whether args.get() restores the original argument type for
    // rvalue references to emplace_args.
    static_assert(
        std::is_same<int&&, decltype(get_emplace_arg<0>(std::move(args)))>::
            value);
    static_assert(
        std::is_same<Object&, decltype(get_emplace_arg<1>(std::move(args)))>::
            value);
    static_assert(
        std::is_same<
            const Object&,
            decltype(get_emplace_arg<2>(std::move(args)))>::value);
    static_assert(
        std::is_same<Object&, decltype(get_emplace_arg<3>(std::move(args)))>::
            value);
    static_assert(
        std::is_same<
            const Object&,
            decltype(get_emplace_arg<4>(std::move(args)))>::value);
    static_assert(
        std::is_same<Object&&, decltype(get_emplace_arg<5>(std::move(args)))>::
            value);
    static_assert(
        std::is_same<
            const Object&,
            decltype(get_emplace_arg<6>(std::move(args)))>::value);
    static_assert(
        std::is_same<Object&&, decltype(get_emplace_arg<7>(std::move(args)))>::
            value);

    // lvalue references to emplace_args should behave mostly like std::tuples.
    // Note that get_emplace_arg<7>(args) does not compile, because
    // folly::rvalue_reference_wrappers can only be unwrapped through an rvalue
    // reference.
    static_assert(
        std::is_same<int&, decltype(get_emplace_arg<0>(args))>::value);
    static_assert(
        std::is_same<Object&, decltype(get_emplace_arg<1>(args))>::value);
    static_assert(
        std::is_same<Object&, decltype(get_emplace_arg<2>(args))>::value);
    static_assert(
        std::is_same<Object&, decltype(get_emplace_arg<3>(args))>::value);
    static_assert(
        std::is_same<Object&, decltype(get_emplace_arg<4>(args))>::value);
    static_assert(
        std::is_same<Object&, decltype(get_emplace_arg<5>(args))>::value);
    static_assert(
        std::is_same<const Object&, decltype(get_emplace_arg<6>(args))>::value);
  }
}

/**
 * Test implicit unpacking.
 */
TEST(EmplaceIterator, ImplicitUnpack) {
  static std::size_t multiCtrCnt;
  static std::size_t pairCtrCnt;
  static std::size_t tupleCtrCnt;

  struct Object2 {
    Object2(int, int) { ++multiCtrCnt; }
    explicit Object2(const std::pair<int, int>&) { ++pairCtrCnt; }
    explicit Object2(const std::tuple<int, int>&) { ++tupleCtrCnt; }
  };

  auto test = [](auto&& it, bool expectUnpack) {
    multiCtrCnt = pairCtrCnt = tupleCtrCnt = 0;
    it = std::make_pair(0, 0);
    it = std::make_tuple(0, 0);
    if (expectUnpack) {
      EXPECT_EQ(multiCtrCnt, 2);
      EXPECT_EQ(pairCtrCnt, 0);
      EXPECT_EQ(tupleCtrCnt, 0);
    } else {
      EXPECT_EQ(multiCtrCnt, 0);
      EXPECT_EQ(pairCtrCnt, 1);
      EXPECT_EQ(tupleCtrCnt, 1);
    }
  };

  Container<Object2> q;

  test(emplacer(q, q.begin()), true);
  test(emplacer<false>(q, q.begin()), false);
  test(front_emplacer(q), true);
  test(front_emplacer<false>(q), false);
  test(back_emplacer(q), true);
  test(back_emplacer<false>(q), false);
}

// IndexIterator -------------

namespace index_iterator_type_tests {
namespace {

struct WeirdContainer {
  std::vector<bool> body_;

  using value_type = bool;
  using size_type = std::uint32_t;
  using difference_type = std::int32_t;

  auto operator[](size_type idx) { return body_[idx]; }
  auto operator[](size_type idx) const { return body_[idx]; }
};

// iterator concepts ------------------------
#if defined(__cpp_lib_concepts)

using vit = folly::index_iterator<std::vector<int>>;
using cvit = folly::index_iterator<const std::vector<int>>;
using vit_weird = folly::index_iterator<WeirdContainer>;
using cvit_weird = folly::index_iterator<const WeirdContainer>;

static_assert(std::random_access_iterator<vit>);
static_assert(std::random_access_iterator<cvit>);
static_assert(std::random_access_iterator<vit_weird>);
static_assert(std::random_access_iterator<cvit_weird>);

#endif

// dependent type computations ------------------------
struct ref_type {};
struct cref_type {};

struct just_operator {
  using value_type = int;
  ref_type operator[](std::size_t) { return {}; }
  cref_type operator[](std::size_t) const { return {}; }
};

struct has_size_type : just_operator {
  using size_type = std::uint32_t;
};

struct has_difference_type : just_operator {
  using difference_type = std::int32_t;
};

struct has_both_size_and_difference_type : just_operator {
  using size_type = std::uint32_t;
  using difference_type = std::int32_t;
};

struct no_operator {
  using value_type = int;

  friend ref_type tag_invoke(
      folly::cpo_t<index_iterator_access_at>, no_operator&, std::size_t) {
    return {};
  }

  friend cref_type tag_invoke(
      folly::cpo_t<index_iterator_access_at>, const no_operator&, std::size_t) {
    return {};
  }

  [[maybe_unused]] int operator[](std::size_t);
  [[maybe_unused]] int operator[](std::size_t) const;
};

struct no_operator_private {
  using value_type = int;

 private:
  friend ref_type tag_invoke(
      folly::cpo_t<index_iterator_access_at>,
      no_operator_private&,
      std::size_t) {
    return {};
  }

  friend cref_type tag_invoke(
      folly::cpo_t<index_iterator_access_at>,
      const no_operator_private&,
      std::size_t) {
    return {};
  }
};

template <typename T>
using all_types = std::tuple<
    typename std::iterator_traits<folly::index_iterator<T>>::value_type,
    typename std::iterator_traits<folly::index_iterator<T>>::reference,
    typename std::iterator_traits<folly::index_iterator<T>>::difference_type,
    typename folly::index_iterator<T>::size_type>;

// gives better error messages than std::is_same
template <typename T>
void is_same_test(T, T) {}

} // namespace

TEST(IndexIterator, Types) {
  is_same_test(
      all_types<just_operator>{},
      std::tuple<int, ref_type, std::ptrdiff_t, std::size_t>{});
  is_same_test(
      all_types<const just_operator>{},
      std::tuple<int, cref_type, std::ptrdiff_t, std::size_t>{});
  is_same_test(
      all_types<has_size_type>{},
      std::tuple<int, ref_type, std::ptrdiff_t, std::uint32_t>{});
  is_same_test(
      all_types<const has_size_type>{},
      std::tuple<int, cref_type, std::ptrdiff_t, std::uint32_t>{});
  is_same_test(
      all_types<has_difference_type>{},
      std::tuple<int, ref_type, std::int32_t, std::size_t>{});
  is_same_test(
      all_types<const has_difference_type>{},
      std::tuple<int, cref_type, std::int32_t, std::size_t>{});
  is_same_test(
      all_types<has_both_size_and_difference_type>{},
      std::tuple<int, ref_type, std::int32_t, std::uint32_t>{});
  is_same_test(
      all_types<const has_both_size_and_difference_type>{},
      std::tuple<int, cref_type, std::int32_t, std::uint32_t>{});
  is_same_test(
      all_types<no_operator>{},
      std::tuple<int, ref_type, std::ptrdiff_t, std::size_t>{});
  is_same_test(
      all_types<const no_operator>{},
      std::tuple<int, cref_type, std::ptrdiff_t, std::size_t>{});
  is_same_test(
      all_types<no_operator_private>{},
      std::tuple<int, ref_type, std::ptrdiff_t, std::size_t>{});
  is_same_test(
      all_types<const no_operator_private>{},
      std::tuple<int, cref_type, std::ptrdiff_t, std::size_t>{});
}

} // namespace index_iterator_type_tests

TEST(IndexIterator, Sort) {
  std::vector<int> v(100, 0);
  std::iota(v.rbegin(), v.rend(), 0);

  std::sort(
      index_iterator<std::vector<int>>{v, 0},
      index_iterator<std::vector<int>>{v, v.size()});

  std::vector<int> expected(100, 0);
  std::iota(expected.begin(), expected.end(), 0);
  ASSERT_EQ(expected, v);
}

namespace {

constexpr bool accessTest() {
  std::array<int, 2> a{0, 1};
  const std::array<int, 2> b{0, 1};

  const index_iterator<std::array<int, 2>> mutI{a, 0};
  const index_iterator<const std::array<int, 2>> constI{b, 0};

  if (*mutI != 0 || *constI != 0) {
    return false;
  }

  if (mutI[1] != 1 || constI[1] != 1) {
    return false;
  }

  return true;
}

static_assert(accessTest());

template <typename I>
constexpr bool mutationsTest(I i0, I i1) {
  auto tmp = i0;
  if (++tmp != i1) {
    return false;
  }
  tmp = i0;
  if (tmp++ != i0) {
    return false;
  }
  if (tmp != i1) {
    return false;
  }

  tmp = i1;
  if (--tmp != i0) {
    return false;
  }
  tmp = i1;
  if (tmp-- != i1) {
    return false;
  }
  if (tmp != i0) {
    return false;
  }

  return true;
}

static constexpr std::array<int, 2> kA{0, 1};

static constexpr index_iterator<const std::array<int, 2>> kI0{kA, 0};
static constexpr index_iterator<const std::array<int, 2>> kI1{kA, 1};

static_assert(kI0 == kI0);
static_assert(kI0 != kI1);
static_assert(kI0 < kI1);
static_assert(kI0 <= kI0);
static_assert(kI0 <= kI1);
static_assert(kI1 >= kI1);
static_assert(kI1 >= kI0);
static_assert(kI1 > kI0);

static_assert(mutationsTest(kI0, kI1));
static_assert(kI0 + 1 == kI1);
static_assert(kI1 - 1 == kI0);
static_assert(kI1 - kI0 == 1);
static_assert(kI0 - kI1 == -1);

struct IndexedVector {
  std::vector<int> v_;

  std::pair<int, int&> operator[](std::size_t i) {
    return {static_cast<int>(i), v_[i]};
  }

  std::pair<int, const int&> operator[](std::size_t i) const {
    return {static_cast<int>(i), v_[i]};
  }

  using value_type = std::pair<int, int>;

  using iterator = folly::index_iterator<IndexedVector>;
  using const_iterator = folly::index_iterator<const IndexedVector>;

  iterator begin() { return {*this, 0}; }
  const_iterator begin() const { return cbegin(); }
  const_iterator cbegin() const { return {*this, 0}; }

  iterator end() { return {*this, v_.size()}; }
  const_iterator end() const { return cend(); }
  const_iterator cend() const { return {*this, v_.size()}; }
};

struct CustomMapWithIndexedIterator {
 public:
  using size_type = std::uint32_t;
  using value_type = std::pair<int, int>;

 public:
  using iterator = folly::index_iterator<CustomMapWithIndexedIterator>;
  using const_iterator =
      folly::index_iterator<const CustomMapWithIndexedIterator>;

  iterator begin() { return {*this, 0}; }
  const_iterator cbegin() const { return {*this, 0}; }

  std::vector<int> keys;
  std::vector<int> values;

 private:
  friend std::pair<int, int> tag_invoke(
      folly::cpo_t<index_iterator_access_at>,
      const CustomMapWithIndexedIterator& self,
      size_type idx) {
    return {self.keys[idx], self.values[idx]};
  }
};

} // namespace

TEST(IndexIterator, UseProxyReferences) {
  IndexedVector iv{{0, 1, 2, 3}};
  const IndexedVector& civ = iv;

  using it = decltype(iv.begin());
  using cit = decltype(civ.begin());

  using it_ref_t = typename std::iterator_traits<it>::reference;
  using it_cref_t = typename std::iterator_traits<cit>::reference;

  static_assert(std::is_same<it_ref_t, std::pair<int, int&>>::value);
  static_assert(std::is_same<it_cref_t, std::pair<int, const int&>>::value);

  static_assert(std::is_same<decltype(it{}->first), int>::value);
  static_assert(std::is_same<decltype(it{}->second), int&>::value);
  static_assert(std::is_same<decltype(cit{}->first), int>::value);
  static_assert(std::is_same<decltype(cit{}->second), const int&>::value);

  ASSERT_EQ(4, (std::count_if(civ.begin(), civ.end(), [](auto&& pair) {
              return pair.first == pair.second;
            })));
  ASSERT_EQ(4, (std::count_if(iv.begin(), iv.end(), [](auto&& pair) {
              return pair.first == pair.second;
            })));
  cit conversion = iv.begin() + 1;
  ASSERT_EQ(&iv, conversion.get_container());
  ASSERT_EQ(1, conversion.get_index());

  static_assert(!std::is_convertible<cit, it>::value);

  for (auto&& x : iv) {
    x.second = -1;
  }

  std::vector<int> expected{-1, -1, -1, -1};
  ASSERT_EQ(expected, iv.v_);

  // testing pointer proxies
  for (auto f = iv.begin(); f != iv.end(); ++f) {
    f->second = 1;
  }

  expected = {1, 1, 1, 1};
  ASSERT_EQ(expected, iv.v_);
}

TEST(IndexIterator, OperatorArrowForNonProxies) {
  using v_t = std::vector<std::array<int, 2>>;
  using iterator = folly::index_iterator<v_t>;

  static_assert(std::is_same<iterator::pointer, v_t::pointer>::value);

  v_t v;
  v.resize(3);
  iterator f{v, 0};
  iterator l{v, v.size()};

  f->at(0) = 1;
  (f + 1)->at(0) = 2;
  (f + 2)->at(0) = 3;

  v_t expected{{1, 0}, {2, 0}, {3, 0}};

  ASSERT_EQ(expected, v);
}

TEST(IndexIterator, NoOperatorIntegration) {
  CustomMapWithIndexedIterator m{.keys{1, 2, 3}, .values{4, 5, 6}};

  auto it = m.begin();
  ASSERT_EQ((std::pair{1, 4}), it[0]);
  ASSERT_EQ((std::pair{2, 5}), it[1]);
  ASSERT_EQ((std::pair{3, 6}), it[2]);

  auto cit = m.cbegin();
  ASSERT_EQ((std::pair{1, 4}), cit[0]);
  ASSERT_EQ((std::pair{2, 5}), cit[1]);
  ASSERT_EQ((std::pair{3, 6}), cit[2]);
}
// reverse_iterator -------------

namespace reverse_iterator_type_tests {
namespace {

template <typename Iter>
concept has_arrow = requires(const Iter i) { i.operator->(); };

template <typename Iter>
void validate_traits_against_std_type() {
  using folly_rev = folly::reverse_iterator<Iter>;
  using std_rev = std::reverse_iterator<Iter>;

  static_assert(
      std::same_as<
          typename folly_rev::iterator_type,
          typename std_rev::iterator_type>);
  static_assert(
      std::same_as<
          typename folly_rev::iterator_category,
          typename std_rev::iterator_category>);
  static_assert(
      std::same_as<
          typename folly_rev::iterator_concept,
          typename std_rev::iterator_concept>);
  static_assert(
      std::same_as<
          typename folly_rev::value_type,
          typename std_rev::value_type>);
  static_assert(
      std::same_as<
          typename folly_rev::difference_type,
          typename std_rev::difference_type>);
  static_assert(
      std::same_as<typename folly_rev::pointer, typename std_rev::pointer>);
  static_assert(
      std::same_as<typename folly_rev::reference, typename std_rev::reference>);
  static_assert(has_arrow<folly_rev> == has_arrow<std_rev>);
  static_assert(
      std::sized_sentinel_for<folly_rev, folly_rev> ==
      std::sized_sentinel_for<std_rev, std_rev>);
  static_assert(
      std::three_way_comparable<folly_rev> ==
      std::three_way_comparable<std_rev>);
  static_assert(
      std::totally_ordered<folly_rev> == std::totally_ordered<std_rev>);
  static_assert(
      std::same_as<
          std::iter_rvalue_reference_t<folly_rev>,
          std::iter_rvalue_reference_t<std_rev>>);
  static_assert(
      std::bidirectional_iterator<folly_rev> ==
      std::bidirectional_iterator<std_rev>);
  static_assert(
      std::random_access_iterator<folly_rev> ==
      std::random_access_iterator<std_rev>);
  static_assert(
      std::contiguous_iterator<folly_rev> == std::contiguous_iterator<std_rev>);
}

struct recursive_node {
  using reverse_iterator = folly::reverse_iterator<recursive_node*>;
};

} // namespace

TEST(ReverseIterator, Types) {
  std::vector<int> ints;
  std::list<int> ints_list;

  auto ints_iota = std::views::iota(0, 1);
  auto ints_transformed = ints | std::views::transform([](int i) { return i; });
  auto ints_filtered =
      ints_list | std::views::filter([](int i) { return i > 0; });

  validate_traits_against_std_type<int*>();
  validate_traits_against_std_type<const int*>();
  validate_traits_against_std_type<std::array<int, 4>::iterator>();
  validate_traits_against_std_type<std::vector<int>::iterator>();
  validate_traits_against_std_type<std::vector<int>::const_iterator>();
  validate_traits_against_std_type<std::vector<bool>::iterator>();
  validate_traits_against_std_type<std::deque<int>::iterator>();
  validate_traits_against_std_type<std::list<int>::iterator>();
  validate_traits_against_std_type<std::set<int>::iterator>();
  validate_traits_against_std_type<std::map<int, int>::iterator>();
  validate_traits_against_std_type<std::string::iterator>();
  validate_traits_against_std_type<std::string_view::iterator>();
  validate_traits_against_std_type<std::span<int>::iterator>();
  validate_traits_against_std_type<std::reverse_iterator<int*>>();
  validate_traits_against_std_type<folly::index_iterator<std::vector<int>>>();
  validate_traits_against_std_type<decltype(ints_iota.begin())>();
  validate_traits_against_std_type<decltype(ints_transformed.begin())>();
  validate_traits_against_std_type<decltype(ints_filtered.begin())>();
}

TEST(ReverseIterator, ConstructionAndAssignment) {
  using rev = folly::reverse_iterator<int*>;
  using crev = folly::reverse_iterator<const int*>;

  static_assert(std::is_trivially_copy_constructible_v<rev>);
  static_assert(std::is_trivially_destructible_v<rev>);

  static_assert(!std::is_convertible_v<int*, rev>);
  static_assert(std::is_convertible_v<rev, crev>);
  static_assert(!std::is_convertible_v<crev, rev>);
  static_assert(!std::is_assignable_v<rev&, crev>);

  static_assert(
      std::same_as<
          decltype(folly::reverse_iterator(std::declval<int*>())),
          rev>);
  static_assert(
      std::same_as<
          decltype(folly::reverse_iterator(std::declval<const rev&>())),
          rev>);

  constexpr auto checks = [] {
    int ints[] = {1, 2, 3};
    int* const p = ints + 1;

    // the standard requires the default constructor to value-initialize
    const rev value_initialized;
    if (std::to_address(value_initialized.base()) != nullptr) {
      return false;
    }

    const rev it{p};
    if (it.base() != p || *it != ints[0]) {
      return false;
    }

    const rev copied = it;
    if (copied.base() != p || *copied != ints[0]) {
      return false;
    }

    const crev converted = it;
    static_assert(std::same_as<decltype(*converted), const int&>);
    if (converted.base() != p || *converted != ints[0]) {
      return false;
    }

    rev copy_assigned;
    copy_assigned = it;
    if (copy_assigned.base() != p || *copy_assigned != ints[0]) {
      return false;
    }

    crev convert_assigned;
    convert_assigned = it;
    if (convert_assigned.base() != p || *convert_assigned != ints[0]) {
      return false;
    }

    return true;
  };
  static_assert(checks());
  EXPECT_TRUE(checks());
}

TEST(ReverseIterator, Access) {
  constexpr auto checks = [] {
    int ints[] = {1, 2, 3};
    const folly::reverse_iterator<int*> it{std::end(ints)};

    static_assert(std::same_as<decltype(*it), int&>);
    static_assert(std::same_as<decltype(it[0]), int&>);

    if (&*it != &ints[2]) {
      return false;
    }
    if (std::to_address(it) != &ints[2]) {
      return false;
    }
    if (*it != ints[2]) {
      return false;
    }
    if (&it[0] != &ints[2]) {
      return false;
    }
    if (&it[2] != &ints[0]) {
      return false;
    }

    *it = 7;
    if (ints[2] != 7) {
      return false;
    }

    it[2] = 8;
    if (ints[0] != 8) {
      return false;
    }

    return true;
  };
  static_assert(checks());
  EXPECT_TRUE(checks());

  {
    std::list<std::string> strs = {"a", "bc"};
    const folly::reverse_iterator<std::list<std::string>::iterator> it{
        strs.end()};

    static_assert(std::same_as<decltype(*it), std::string&>);
    EXPECT_EQ(&strs.back(), std::to_address(it));
    EXPECT_EQ(&strs.back(), it.operator->());
    EXPECT_EQ(2u, it->size());

    it->push_back('z');
    EXPECT_EQ("bcz", strs.back());
  }

  {
    std::vector<bool> bits = {false, true};
    const folly::reverse_iterator<std::vector<bool>::iterator> it{bits.end()};

    static_assert(std::same_as<decltype(*it), std::vector<bool>::reference>);
    static_assert(std::same_as<decltype(it[0]), std::vector<bool>::reference>);
    EXPECT_EQ(bits[1], *it);
    EXPECT_EQ(bits[0], it[1]);

    *it = false;
    EXPECT_FALSE(bits[1]);

    it[1] = true;
    EXPECT_TRUE(bits[0]);
  }
}

TEST(ReverseIterator, Arithmetic) {
  constexpr auto checks = [] {
    using rev = folly::reverse_iterator<int*>;

    int ints[] = {1, 2, 3};
    int* const first = std::begin(ints);
    int* const last = std::end(ints);

    rev it{last};

    // each step checks what the operator returned and where it left `it`
    if ((++it).base() != last - 1 || it.base() != last - 1) {
      return false;
    }
    if ((it++).base() != last - 1 || it.base() != last - 2) {
      return false;
    }
    if ((--it).base() != last - 1 || it.base() != last - 1) {
      return false;
    }
    if ((it--).base() != last - 1 || it.base() != last) {
      return false;
    }

    if ((it + 3).base() != first || it.base() != last) {
      return false;
    }
    if ((3 + it).base() != first || it.base() != last) {
      return false;
    }
    if ((it += 3).base() != first || it.base() != first) {
      return false;
    }
    if ((it - 3).base() != last || it.base() != first) {
      return false;
    }
    if ((it -= 3).base() != last || it.base() != last) {
      return false;
    }

    if (rev{first} - rev{last} != 3) {
      return false;
    }

    const folly::reverse_iterator<const int*> const_rbegin{last};
    if (rev{first} - const_rbegin != 3) {
      return false;
    }
    if (const_rbegin - rev{first} != -3) {
      return false;
    }

    auto ints_iota = std::views::iota(0, 3);
    folly::reverse_iterator<decltype(ints_iota.begin())> iota_it{
        ints_iota.end()};
    if (*iota_it != 2 || *++iota_it != 1 || iota_it[1] != 0) {
      return false;
    }

    return true;
  };
  static_assert(checks());
  EXPECT_TRUE(checks());

  {
    std::vector<int> vec = {1, 2, 3};
    const folly::reverse_iterator<std::vector<int>::iterator> vec_rend{
        vec.begin()};
    const folly::reverse_iterator<std::vector<int>::const_iterator> vec_rbegin{
        vec.cend()};

    EXPECT_EQ(3, vec_rend - vec_rbegin);

    using rev_list = folly::reverse_iterator<std::list<int>::iterator>;
    static_assert(std::sentinel_for<rev_list, rev_list>);
    static_assert(!std::sized_sentinel_for<rev_list, rev_list>);
  }
}

TEST(ReverseIterator, Comparisons) {
  using rev = folly::reverse_iterator<int*>;
  using crev = folly::reverse_iterator<const int*>;

  static_assert(std::totally_ordered_with<rev, crev>);

  static constexpr auto validate = [](const auto& lo, const auto& hi) {
    return lo == lo && !(lo == hi) && lo != hi && //
        lo < hi && !(hi < lo) && hi > lo && !(lo > hi) && //
        lo <= hi && !(hi <= lo) && hi >= lo && !(lo >= hi) && //
        (lo <=> hi) == std::strong_ordering::less &&
        (hi <=> lo) == std::strong_ordering::greater &&
        (lo <=> lo) == std::strong_ordering::equal;
  };

  // reverse iteration runs rbegin -> rend, so rbegin has to order first even
  // though its base is the larger pointer
  static constexpr int ints[] = {1, 2, 3};
  constexpr crev rbegin{std::end(ints)};
  constexpr crev rend{std::begin(ints)};

  static_assert(rbegin < rend);
  static_assert(validate(rbegin, rend));
  EXPECT_TRUE(validate(rbegin, rend));

  // mismatched types need a mutable array, so they need their own scope
  constexpr auto mixed = [] {
    int mut[] = {1, 2, 3};
    return validate(rev{std::end(mut)}, crev{std::begin(mut)});
  };
  static_assert(mixed());
  EXPECT_TRUE(mixed());
}

TEST(ReverseIterator, RangesHooks) {
  constexpr auto checks = [] {
    using rev = folly::reverse_iterator<int*>;

    int ints[] = {1, 2, 3};
    const rev rbegin{std::end(ints)};
    const rev next{std::end(ints) - 1};

    static_assert(
        std::same_as<decltype(std::ranges::iter_move(rbegin)), int&&>);
    if (std::ranges::iter_move(rbegin) != ints[2]) {
      return false;
    }

    std::ranges::iter_swap(rbegin, next);
    if (ints[2] != 2 || ints[1] != 3) {
      return false;
    }

    return true;
  };
  static_assert(checks());
  EXPECT_TRUE(checks());
}

TEST(ReverseIterator, IncompleteValueType) {
  static_assert(std::random_access_iterator<recursive_node::reverse_iterator>);

  constexpr auto checks = [] {
    recursive_node nodes[2];
    const recursive_node::reverse_iterator rbegin{std::end(nodes)};
    return &*rbegin == &nodes[1];
  };
  static_assert(checks());
  EXPECT_TRUE(checks());
}

} // namespace reverse_iterator_type_tests
