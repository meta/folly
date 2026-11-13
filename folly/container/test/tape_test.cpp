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

#include <folly/container/tape.h>

#include <compare>
#include <forward_list>
#include <iterator>
#include <limits>
#include <vector>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <folly/small_vector.h>

namespace {

template <typename I>
struct InputIter {
  I wrapped;

  using value_type = typename std::iterator_traits<I>::value_type;
  using reference = typename std::iterator_traits<I>::reference;
  using difference_type = typename std::iterator_traits<I>::difference_type;
  using pointer = typename std::iterator_traits<I>::pointer;
  using iterator_category = std::input_iterator_tag;

  InputIter& operator++() {
    ++wrapped;
    return *this;
  }

  reference operator*() const { return *wrapped; }

  auto operator++(int) { operator++(); }

  friend bool operator==(const InputIter& x, const InputIter& y) {
    return x.wrapped == y.wrapped;
  }
  friend bool operator!=(const InputIter& x, const InputIter& y) {
    return !(x == y);
  }
};

template <typename T>
struct InputRange {
  std::vector<T> c_;
  using iterator = InputIter<typename std::vector<T>::const_iterator>;

  iterator begin() const { return iterator{c_.begin()}; }
  iterator end() const { return iterator{c_.end()}; }
};

static_assert(!std::forward_iterator<InputIter<int*>>);
static_assert(std::input_iterator<InputIter<int*>>);

template <typename T>
auto asRange(const std::vector<T>& c, std::input_iterator_tag) {
  return InputRange<T>{c};
}

template <typename T>
auto asRange(const std::vector<T>& c, std::forward_iterator_tag) {
  return std::forward_list<T>{c.begin(), c.end()};
}

template <typename T>
auto asRange(const std::vector<T>& c, std::random_access_iterator_tag) {
  return c;
}

} // namespace

// tape types

static_assert(
    std::is_same_v<folly::StringPiece, folly::string_tape::reference>);
static_assert(
    std::is_same_v<folly::StringPiece, folly::string_tape::value_type>);
static_assert( //
    std::is_same_v<
        folly::Range<const int*>,
        folly::tape<std::vector<int>>::reference>);
static_assert( //
    std::is_same_v<
        folly::Range<const int*>,
        folly::tape<std::vector<int>>::value_type>);
static_assert( //
    std::is_same_v<
        folly::Range<const int*>,
        folly::tape<folly::small_vector<int, 4>>::reference>);

static_assert(std::ranges::random_access_range<folly::string_tape>);

TEST(Tape, SmokeTest) {
  folly::string_tape st;

  st.push_back("abc");
  st.push_back("def");

  ASSERT_EQ("abc", st[0]);
  ASSERT_EQ("def", st[1]);
  ASSERT_EQ("abc", st.front());
  ASSERT_EQ("def", st.back());
}

TEST(Tape, IntTape) {
  folly::tape<std::vector<int>> t;
  t.push_back({1, 2});
  t.emplace_back(std::vector{3});

  ASSERT_EQ(1, t[0][0]);
  ASSERT_EQ(2, t[0][1]);
  ASSERT_EQ(3, t[1][0]);
}

TEST(Tape, Count) {
  folly::string_tape st{"abc", "def", "bla", "abc"};

  ASSERT_EQ(2U, (std::count(st.begin(), st.end(), "abc")));
  ASSERT_EQ(2U, (std::count(st.cbegin(), st.cend(), "abc")));
  ASSERT_EQ(2U, (std::count(st.rbegin(), st.rend(), "abc")));
  ASSERT_EQ(2U, (std::count(st.crbegin(), st.crend(), "abc")));
}

TEST(Tape, At) {
  folly::string_tape st{"ab", "cde", "f"};
  ASSERT_EQ("ab", st.at(0));
  ASSERT_EQ("cde", st.at(1));
  ASSERT_EQ("f", st.at(2));
  ASSERT_THROW((void)st.at(3), std::out_of_range);
}

TEST(Tape, Move) {
  folly::string_tape st0{"ab", "cde", "f"};
  folly::string_tape st1{st0};

  folly::string_tape move_constuctor{std::move(st0)};
  folly::string_tape move_assign;
  move_assign = std::move(st1);

  ASSERT_EQ(move_constuctor, move_assign);
  ASSERT_THAT(move_constuctor, testing::ElementsAre("ab", "cde", "f"));

  // moved out should be valid but unspecified.
  // we had a bug where 0s marker was missing.
  // The test is over specified.
  ASSERT_EQ(0U, st0.size());
  ASSERT_EQ(0U, st1.size());

  ASSERT_TRUE(st0.begin() == st0.end());
  ASSERT_TRUE(st1.begin() == st1.end());

  st0.push_back("ab");
  st1.push_back("ab");

  ASSERT_THAT(st0, testing::ElementsAre("ab"));
  ASSERT_THAT(st1, testing::ElementsAre("ab"));

  // self-move should be valid but unspecified.
  // not many generic choices, though: either no-op or clear or reset
  // std::vector<int> self-move is:
  //   reset under libstdc++ and libc++
  //   no-op under microsoft-stl
  // the current implementation is reset
  move_assign = std::move(std::move(move_assign)); // suppress self-move warning
  ASSERT_EQ(0U, move_assign.size());
}

TEST(Tape, TwoItersConstructor) {
  auto tst = [&](auto tag) {
    {
      const std::vector<std::vector<int>> a{{1, 2}, {3, 4, 5}};

      auto input = asRange(a, tag);
      folly::tape<std::vector<int>> t{input.begin(), input.end()};
      ASSERT_EQ(2, t.size());
      ASSERT_EQ(folly::crange(a[0]), t[0]);
      ASSERT_EQ(folly::crange(a[1]), t[1]);
    }
    {
      const std::vector<std::vector<int>> empty;
      auto input = asRange(empty, tag);
      folly::tape<std::vector<int>> t{input.begin(), input.end()};
      ASSERT_EQ(0, t.size());
      ASSERT_TRUE(t.empty());
    }
    {
      const std::vector<std::vector<int>> emptyRow{{1, 2}, {}, {3, 4, 5}};
      auto input = asRange(emptyRow, tag);
      folly::tape<std::vector<int>> t{input.begin(), input.end()};
      ASSERT_EQ(3, t.size());
      ASSERT_EQ(folly::crange(emptyRow[0]), t[0]);
      ASSERT_EQ(folly::crange(emptyRow[1]), t[1]);
      ASSERT_EQ(folly::crange(emptyRow[2]), t[2]);
    }
  };

  tst(std::input_iterator_tag{});
  tst(std::forward_iterator_tag{});
  tst(std::random_access_iterator_tag{});
}

TEST(Tape, InitListNotStrings) {
  std::vector<int> a[] = {{1, 2}, {3, 4, 5}, {6, 7}};

  folly::tape<std::vector<int>> t{a[0], a[1], a[2]};
  ASSERT_EQ(3, t.size());
  ASSERT_EQ(folly::crange(a[0]), t[0]);
  ASSERT_EQ(folly::crange(a[1]), t[1]);
  ASSERT_EQ(folly::crange(a[2]), t[2]);
}

TEST(Tape, Ordering) {
  const folly::string_tape st1{"abc", "def"};
  const folly::string_tape st2{"abcd", "ef"};

  ASSERT_EQ(st1, st1);
  ASSERT_NE(st1, st2);

  ASSERT_LT(st1, st2);
  ASSERT_LE(st1, st2);
  ASSERT_LE(st1, st1);

  ASSERT_GT(st2, st1);
  ASSERT_GE(st2, st1);
  ASSERT_GE(st1, st1);
  static_assert(std::same_as<decltype(st1 <=> st2), std::strong_ordering>);
  EXPECT_EQ(st1 <=> st2, std::strong_ordering::less);
  EXPECT_EQ(st2 <=> st1, std::strong_ordering::greater);
  EXPECT_EQ(st1 <=> st1, std::strong_ordering::equal);
  EXPECT_TRUE((folly::string_tape{"abc"} <=> st1) < 0);
  EXPECT_TRUE((folly::string_tape{} <=> st1) < 0);
}

TEST(Tape, ThreeWayComparisonWeakElements) {
  struct WeakValue {
    int value;
    bool operator==(const WeakValue&) const = default;
    std::weak_ordering operator<=>(const WeakValue& other) const {
      return value <=> other.value;
    }
  };
  static_assert(std::three_way_comparable<WeakValue>);
  const folly::tape<std::vector<WeakValue>> a{
      std::vector<WeakValue>{{1}, {2}}, std::vector<WeakValue>{{3}}};
  const folly::tape<std::vector<WeakValue>> b{
      std::vector<WeakValue>{{1}, {2}}, std::vector<WeakValue>{{4}}};
  const folly::tape<std::vector<WeakValue>> prefix{
      std::vector<WeakValue>{{1}, {2}}};
  static_assert(std::same_as<decltype(a <=> b), std::weak_ordering>);
  EXPECT_EQ(a <=> b, std::weak_ordering::less);
  EXPECT_EQ(b <=> a, std::weak_ordering::greater);
  EXPECT_EQ(a <=> a, std::weak_ordering::equivalent);
  EXPECT_TRUE((prefix <=> a) < 0);
  EXPECT_LT(a, b);
  EXPECT_LE(a, a);
  EXPECT_GT(b, a);
  EXPECT_GE(a, a);
  EXPECT_NE(a, b);
}

TEST(Tape, NoLegacyOrderingFallback) {
  struct LegacyValue {
    int value;
    bool operator==(const LegacyValue&) const = default;
    bool operator<(const LegacyValue& other) const {
      return value < other.value;
    }
  };
  using LegacyTape = folly::tape<std::vector<LegacyValue>>;
  static_assert(!std::three_way_comparable<LegacyTape>);
  const LegacyTape values{std::vector<LegacyValue>{{1}}};
  EXPECT_EQ(values.size(), 1);
  EXPECT_EQ(values.front()[0], LegacyValue{1});
  EXPECT_LT(values.front()[0], LegacyValue{2});
}

TEST(Tape, ThreeWayComparisonUnordered) {
  const folly::tape<std::vector<double>> a{
      std::vector<double>{std::numeric_limits<double>::quiet_NaN(), 1}};
  const folly::tape<std::vector<double>> b{std::vector<double>{0, 2}};
  static_assert(std::same_as<decltype(a <=> b), std::partial_ordering>);
  EXPECT_EQ(a <=> b, std::partial_ordering::unordered);
  EXPECT_EQ(b <=> a, std::partial_ordering::unordered);
  EXPECT_FALSE(a < b);
  EXPECT_FALSE(a <= b);
  EXPECT_FALSE(a > b);
  EXPECT_FALSE(a >= b);
}

TEST(Tape, ThreeWayComparisonUnsignedByteOrdering) {
  const folly::string_tape high{"\x80"};
  const folly::string_tape low{"\x7f"};
  EXPECT_EQ(high <=> low, std::strong_ordering::greater);
  EXPECT_GT(high, low);
  const folly::tape<std::vector<signed char>> signedHigh{
      std::vector<signed char>{static_cast<signed char>(0x80)}};
  const folly::tape<std::vector<signed char>> signedLow{
      std::vector<signed char>{0x7f}};
  EXPECT_EQ(signedHigh <=> signedLow, std::strong_ordering::greater);
  EXPECT_GT(signedHigh, signedLow);
}

TEST(Tape, RecordBuilder) {
  folly::string_tape st;

  {
    auto builder = st.new_record_builder();
    ASSERT_TRUE(builder.empty());
    ASSERT_EQ(0U, builder.size());
    builder.push_back('a');
    builder.push_back('b');
    ASSERT_FALSE(builder.empty());
    ASSERT_EQ(2U, builder.size());

    ASSERT_EQ('b', builder.back());
    ASSERT_EQ('b', std::as_const(builder).back());

    ASSERT_EQ(std::string_view(builder.begin(), builder.end()), "ab");
    ASSERT_EQ(std::string_view(builder.cbegin(), builder.cend()), "ab");
    *builder.begin() = 'c';
    builder.commit();
  }
  {
    auto builder = st.new_record_builder();
    ASSERT_EQ(builder.emplace_back('d'), 'd');
    builder[0] = 'e';
    ASSERT_EQ(std::as_const(builder)[0], 'e');
    builder.commit();
  }
  {
    auto builder = st.new_record_builder();
    builder.emplace_back(); // test emplace with default constructor
    builder.at(0) = 'b';
    ASSERT_EQ(std::as_const(builder).at(0), 'b');
    builder[0] = 'a';
    ASSERT_EQ(std::as_const(builder)[0], 'a');
    builder.commit();
  }
  {
    auto builder = st.new_record_builder();
    constexpr std::string_view s = "abc";
    std::copy(s.begin(), s.end(), builder.back_inserter());
    builder.commit();
  }
  {
    auto builder = st.new_record_builder();
    builder.push_back('0');
    builder.push_back('1');
    // Default abort - not committed
  }
  {
    // calling abort
    auto builder = st.new_record_builder();
    ASSERT_EQ(0U, builder.size());
    builder.push_back('a');
    builder.push_back('b');
    ASSERT_EQ(2U, builder.size());
    builder.abort();
    ASSERT_EQ(0U, builder.size());
    builder.push_back('a');
    builder.push_back('b');
    builder.commit();
  }
  {
    // amending record
    ASSERT_EQ(st.back(), "ab");
    auto builder = st.last_record_builder();
    builder.push_back('c');
    builder[0] = '0';
    builder.commit();
    ASSERT_EQ(st.back(), "0bc");
  }
  {
    // not committing last record
    ASSERT_EQ(st.size(), 5);

    {
      auto builder = st.new_record_builder();
      builder.push_back('a');
      builder.push_back('b');
      builder.commit();
    }

    ASSERT_EQ(st.size(), 6);
    (void)st.last_record_builder(); // destructor will auto abort.

    ASSERT_EQ(st.size(), 5);
  }

  ASSERT_EQ("cb", st[0]);
  ASSERT_EQ("e", st[1]);
  ASSERT_EQ("a", st[2]);
  ASSERT_EQ("abc", st[3]);
  ASSERT_EQ("0bc", st[4]);
  ASSERT_EQ(10U, st.size_flat());
  ASSERT_EQ(5U, st.size());

  // checking that pop back is still ok.

  st.pop_back();
  ASSERT_EQ("cb", st[0]);
  ASSERT_EQ("e", st[1]);
  ASSERT_EQ("a", st[2]);
  ASSERT_EQ("abc", st[3]);
  ASSERT_EQ(7U, st.size_flat());
  ASSERT_EQ(4U, st.size());

  st.pop_back();
  ASSERT_EQ("cb", st[0]);
  ASSERT_EQ("e", st[1]);
  ASSERT_EQ("a", st[2]);
  ASSERT_EQ(4U, st.size_flat());
  ASSERT_EQ(3U, st.size());

  st.pop_back();
  ASSERT_EQ("cb", st[0]);
  ASSERT_EQ("e", st[1]);
  ASSERT_EQ(2U, st.size());
  ASSERT_EQ(3U, st.size_flat());

  st.pop_back();
  ASSERT_EQ("cb", st[0]);
  ASSERT_EQ(1U, st.size());
  ASSERT_EQ(2U, st.size_flat());

  st.pop_back();
  ASSERT_EQ(0U, st.size());
  ASSERT_EQ(0U, st.size_flat());
}

TEST(Tape, PushBack) {
  folly::tape<std::vector<int>> t;

  auto tst = [&](auto tag) mutable {
    t.clear();
    ASSERT_EQ(0, t.size());

    std::vector<int> a{1, 2, 3}, b{4, 5}, c;
    t.push_back(asRange(a, tag));
    ASSERT_EQ(1, t.size());
    ASSERT_EQ(folly::crange(a), t[0]);

    t.push_back(asRange(b, tag));
    ASSERT_EQ(2, t.size());
    ASSERT_EQ(folly::crange(a), t[0]);
    ASSERT_EQ(folly::crange(b), t[1]);

    t.push_back(c);
    ASSERT_EQ(3, t.size());
    ASSERT_EQ(folly::crange(a), t[0]);
    ASSERT_EQ(folly::crange(b), t[1]);
    ASSERT_EQ(folly::crange(c), t[2]);
  };

  tst(std::input_iterator_tag{});
  tst(std::forward_iterator_tag{});
  tst(std::random_access_iterator_tag{});
}

TEST(Tape, PushBackStr) {
  folly::string_tape st;

  st.push_back("abc");
  st.push_back({'d', 'e'});

  constexpr std::string_view c = "fgh";
  st.push_back(c);
  constexpr std::string_view d = "ijklm";
  st.push_back(d.begin(), d.end());
  ASSERT_THAT(st, testing::ElementsAre("abc", "de", "fgh", "ijklm"));
}

TEST(Tape, EmptyRecord) {
  folly::tape<std::vector<int>> t;

  t.push_back({});
  t.push_back({});

  ASSERT_EQ(2U, t.size());
  ASSERT_TRUE(t[0].empty());
  ASSERT_TRUE(t[1].empty());
}

TEST(Tape, Resize) {
  folly::string_tape st{"ab", "abc", "abcd", "de"};

  st.resize(6U);
  ASSERT_THAT(st, testing::ElementsAre("ab", "abc", "abcd", "de", "", ""));

  st.resize(2U);
  ASSERT_THAT(st, testing::ElementsAre("ab", "abc"));

  st.resize(2U);
  ASSERT_THAT(st, testing::ElementsAre("ab", "abc"));

  st.push_back("abcd");
  ASSERT_THAT(st, testing::ElementsAre("ab", "abc", "abcd"));

  st.resize(0U);
  ASSERT_TRUE(st.empty());

  st.push_back("ab");
  ASSERT_THAT(st, testing::ElementsAre("ab"));
}

TEST(Tape, EmptyStrings) {
  folly::string_tape st{"a"};
  st.push_back("");
  ASSERT_THAT(st, testing::ElementsAre("a", ""));

  st.emplace_back();

  ASSERT_THAT(st, testing::ElementsAre("a", "", ""));

  st.erase(st.begin() + 1, st.end());

  ASSERT_THAT(st, testing::ElementsAre("a"));
  st.emplace_back();
  st.emplace_back("bd");
  ASSERT_THAT(st, testing::ElementsAre("a", "", "bd"));
}

TEST(Tape, InsertOne) {
  auto tst = [](auto tag) {
    folly::tape<std::vector<int>> t;

    std::vector<int> a{1, 2, 3}, b{4, 5}, c{6, 7, 8}, d{};

    {
      auto pos = t.insert(t.end(), asRange(a, tag));
      ASSERT_EQ(pos - t.begin(), 0);
      ASSERT_THAT(t, testing::ElementsAre(a));
    }
    {
      auto pos = t.insert(t.end(), asRange(c, tag));

      ASSERT_EQ(pos - t.begin(), 1);
      ASSERT_THAT(t, testing::ElementsAre(a, c));
    }
    {
      auto pos = t.insert(t.begin() + 1, asRange(b, tag));

      ASSERT_EQ(pos - t.begin(), 1);
      ASSERT_THAT(t, testing::ElementsAre(a, b, c));
    }
    {
      auto pos = t.insert(t.begin() + 2, asRange(d, tag));

      ASSERT_EQ(pos - t.begin(), 2);
      ASSERT_THAT(t, testing::ElementsAre(a, b, d, c));
    }
  };

  tst(std::input_iterator_tag{});
  tst(std::forward_iterator_tag{});
  tst(std::random_access_iterator_tag{});
}

TEST(Tape, InsertOneStr) {
  folly::string_tape st;
  auto pos = st.insert(st.end(), "ab");
  ASSERT_EQ(pos - st.begin(), 0);
  ASSERT_THAT(st, testing::ElementsAre("ab"));

  {
    std::string s("cde");
    pos = st.insert(st.end(), s.begin(), s.end());
    ASSERT_EQ(pos - st.begin(), 1);
    ASSERT_THAT(st, testing::ElementsAre("ab", "cde"));
  }

  {
    pos = st.insert(st.begin() + 1, {'0', '1'});
    ASSERT_EQ(pos - st.begin(), 1);
    ASSERT_THAT(st, testing::ElementsAre("ab", "01", "cde"));
  }

  {
    std::forward_list<char> in{'1', '2', '3'};
    pos = st.insert(st.begin(), in);
    ASSERT_EQ(pos - st.begin(), 0);
    ASSERT_THAT(st, testing::ElementsAre("123", "ab", "01", "cde"));
  }
}

TEST(Tape, EraseOne) {
  folly::string_tape st{"ab", "abc", "abcd", "de"};
  folly::string_tape::iterator pos = st.erase(st.cbegin() + 1);

  ASSERT_EQ(1, pos - st.begin());
  ASSERT_THAT(st, testing::ElementsAre("ab", "abcd", "de"));

  // tape is still functional
  st.push_back("ef");
  ASSERT_THAT(st, testing::ElementsAre("ab", "abcd", "de", "ef"));

  ASSERT_EQ(st.erase(st.cbegin()) - st.cbegin(), 0);
  ASSERT_THAT(st, testing::ElementsAre("abcd", "de", "ef"));

  ASSERT_EQ(st.erase(st.begin() + 1) - st.cbegin(), 1);
  ASSERT_THAT(st, testing::ElementsAre("abcd", "ef"));

  ASSERT_EQ(st.erase(st.cend() - 1) - st.cbegin(), 1);
  ASSERT_THAT(st, testing::ElementsAre("abcd"));

  ASSERT_EQ(st.erase(st.cbegin()) - st.cbegin(), 0);
  ASSERT_TRUE(st.empty());
}

TEST(Tape, EraseRange) {
  folly::string_tape st{"ab", "abc", "abcd", "de"};

  folly::string_tape::iterator pos = st.erase(st.cbegin() + 1, st.cend() - 1);
  ASSERT_THAT(st, testing::ElementsAre("ab", "de"));
  ASSERT_EQ(pos - st.begin(), 1);

  pos = st.erase(st.end(), st.end());
  ASSERT_THAT(st, testing::ElementsAre("ab", "de"));
  ASSERT_EQ(pos - st.begin(), 2);
}

TEST(Tape, Iteration) {
  folly::string_tape st{"0", "10", "100", "1000"};
  ASSERT_THAT(st, testing::ElementsAre("0", "10", "100", "1000"));

  folly::string_tape st2(st.rbegin(), st.rend());
  ASSERT_THAT(st2, testing::ElementsAre("1000", "100", "10", "0"));
}

TEST(Tape, ReserveShrink) {
  // There is a limit to what we can test since reserve/shrink
  // have very little guarantees.

  folly::string_tape st;
  st.reserve(2, 7);

  st.push_back("abc");
  const char* beforePushSecond = st[0].data();

  st.push_back("defg");
  const char* afterPushSecond = st[0].data();
  ASSERT_EQ(beforePushSecond, afterPushSecond);

  st.shrink_to_fit();
  const char* afterShrink = st[0].data();
  ASSERT_EQ(beforePushSecond, afterShrink);
}

TEST(Tape, TapeOfTapes) {
  folly::tape<folly::string_tape> strings;

  auto builder = strings.new_record_builder();
  builder.push_back("ab");
  builder.push_back("abc");
  builder.commit();

  builder.push_back("bc");
  builder.push_back("bcd");
  builder.push_back("bcde");
  builder.commit();

  ASSERT_EQ(strings.size(), 2);
  ASSERT_THAT(strings[0], testing::ElementsAre("ab", "abc"));
  ASSERT_THAT(strings[1], testing::ElementsAre("bc", "bcd", "bcde"));
}
