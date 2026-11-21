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

#include <folly/container/detail/Util.h>

#include <glog/logging.h>

#include <folly/Optional.h>
#include <folly/Range.h>
#include <folly/container/testing/TrackingTypes.h>
#include <folly/portability/GTest.h>

using namespace folly::test;

namespace folly {

TEST(Tracked, baseline) {
  // this is a test that Tracked works like we expect
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

// F should take a templatized func and a pair const& or pair&& and
// call the function using callWithExtractedKey
template <typename F>
void runKeyExtractCases(
    std::string const& name, F const& func, uint64_t expectedDist = 0) {
  Optional<std::pair<Tracked<0> const, Tracked<1>>> sink;
  auto sinkFunc = [&sink](Tracked<0> const& key, auto&&... args) {
    if (!sink.hasValue() || sink.value().first != key) {
      sink.emplace(std::forward<decltype(args)>(args)...);
    }
  };

  {
    std::pair<Tracked<0> const, Tracked<1>> p{0, 0};
    sink.reset();
    resetTracking();
    func(sinkFunc, p);
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
    std::pair<Tracked<0> const, Tracked<1>> p{0, 0};
    sink.reset();
    resetTracking();
    func(sinkFunc, std::move(p));
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
    sink.reset();
    resetTracking();
    func(sinkFunc, p);
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
    sink.reset();
    resetTracking();
    func(sinkFunc, std::move(p));
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
    sink.reset();
    resetTracking();
    func(sinkFunc, p);
    // fresh key, convertible const& ->
    //   key_type ops: Tracked<0>::counts
    //   mapped_type ops: Tracked<1>::counts
    //   key_src ops: Tracked<2>::counts
    //   mapped_src ops: Tracked<3>::counts();

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
    sink.reset();
    resetTracking();
    func(sinkFunc, std::move(p));
    // fresh key, convertible&& ->
    //   key_type ops: Tracked<0>::counts
    //   mapped_type ops: Tracked<1>::counts
    //   key_src ops: Tracked<2>::counts
    //   mapped_src ops: Tracked<3>::counts();
    EXPECT_EQ(
        Tracked<0>::counts().dist(Counts{0, 1, 0, 1}) +
            Tracked<1>::counts().dist(Counts{0, 0, 0, 1}) +
            Tracked<2>::counts().dist(Counts{0, 0, 0, 0}) +
            Tracked<3>::counts().dist(Counts{0, 0, 0, 0}),
        expectedDist);
  }
  {
    std::pair<Tracked<0> const, Tracked<1>> p{0, 0};
    sink.reset();
    sink.emplace(0, 0);
    resetTracking();
    func(sinkFunc, p);
    // duplicate key, value_type const&
    EXPECT_EQ(
        Tracked<0>::counts().dist(Counts{0, 0, 0, 0}) +
            Tracked<1>::counts().dist(Counts{0, 0, 0, 0}),
        expectedDist);
  }
  {
    std::pair<Tracked<0> const, Tracked<1>> p{0, 0};
    sink.reset();
    sink.emplace(0, 0);
    resetTracking();
    func(sinkFunc, std::move(p));
    // duplicate key, value_type&&
    EXPECT_EQ(
        Tracked<0>::counts().dist(Counts{0, 0, 0, 0}) +
            Tracked<1>::counts().dist(Counts{0, 0, 0, 0}),
        expectedDist);
  }
  {
    std::pair<Tracked<0>, Tracked<1>> p{0, 0};
    sink.reset();
    sink.emplace(0, 0);
    resetTracking();
    func(sinkFunc, p);
    // duplicate key, pair<key_type,mapped_type> const&
    EXPECT_EQ(
        Tracked<0>::counts().dist(Counts{0, 0, 0, 0}) +
            Tracked<1>::counts().dist(Counts{0, 0, 0, 0}),
        expectedDist);
  }
  {
    std::pair<Tracked<0>, Tracked<1>> p{0, 0};
    sink.reset();
    sink.emplace(0, 0);
    resetTracking();
    func(sinkFunc, std::move(p));
    // duplicate key, pair<key_type,mapped_type>&&
    EXPECT_EQ(
        Tracked<0>::counts().dist(Counts{0, 0, 0, 0}) +
            Tracked<1>::counts().dist(Counts{0, 0, 0, 0}),
        expectedDist);
  }
  {
    std::pair<Tracked<2>, Tracked<3>> p{0, 0};
    sink.reset();
    sink.emplace(0, 0);
    resetTracking();
    func(sinkFunc, p);
    // duplicate key, convertible const& ->
    //   key_type ops: Tracked<0>::counts
    //   mapped_type ops: Tracked<1>::counts
    //   key_src ops: Tracked<2>::counts
    //   mapped_src ops: Tracked<3>::counts();
    EXPECT_EQ(
        Tracked<0>::counts().dist(Counts{0, 0, 1, 0}) +
            Tracked<1>::counts().dist(Counts{0, 0, 0, 0}) +
            Tracked<2>::counts().dist(Counts{0, 0, 0, 0}) +
            Tracked<3>::counts().dist(Counts{0, 0, 0, 0}),
        expectedDist * 2);
  }
  {
    std::pair<Tracked<2>, Tracked<3>> p{0, 0};
    sink.reset();
    sink.emplace(0, 0);
    resetTracking();
    func(sinkFunc, std::move(p));
    // duplicate key, convertible&& ->
    //   key_type ops: Tracked<0>::counts
    //   mapped_type ops: Tracked<1>::counts
    //   key_src ops: Tracked<2>::counts
    //   mapped_src ops: Tracked<3>::counts();
    EXPECT_EQ(
        Tracked<0>::counts().dist(Counts{0, 0, 0, 1}) +
            Tracked<1>::counts().dist(Counts{0, 0, 0, 0}) +
            Tracked<2>::counts().dist(Counts{0, 0, 0, 0}) +
            Tracked<3>::counts().dist(Counts{0, 0, 0, 0}),
        expectedDist);
  }
}

struct DoEmplace1 {
  template <typename F, typename P>
  void operator()(F&& f, P&& p) const {
    std::allocator<char> a;
    detail::callWithExtractedKey<Tracked<0>, Tracked<1>>(
        a, std::forward<F>(f), std::forward<P>(p));
  }
};

struct DoEmplace2 {
  template <typename F, typename U1, typename U2>
  void operator()(F&& f, std::pair<U1, U2> const& p) const {
    std::allocator<char> a;
    detail::callWithExtractedKey<Tracked<0>, Tracked<1>>(
        a, std::forward<F>(f), p.first, p.second);
  }

  template <typename F, typename U1, typename U2>
  void operator()(F&& f, std::pair<U1, U2>&& p) const {
    std::allocator<char> a;
    detail::callWithExtractedKey<Tracked<0>, Tracked<1>>(
        a, std::forward<F>(f), std::move(p.first), std::move(p.second));
  }
};

struct DoEmplace3 {
  template <typename F, typename U1, typename U2>
  void operator()(F&& f, std::pair<U1, U2> const& p) const {
    std::allocator<char> a;
    detail::callWithExtractedKey<Tracked<0>, Tracked<1>>(
        a,
        std::forward<F>(f),
        std::piecewise_construct,
        std::forward_as_tuple(p.first),
        std::forward_as_tuple(p.second));
  }

  template <typename F, typename U1, typename U2>
  void operator()(F&& f, std::pair<U1, U2>&& p) const {
    std::allocator<char> a;
    detail::callWithExtractedKey<Tracked<0>, Tracked<1>>(
        a,
        std::forward<F>(f),
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
  template <typename F, typename U1, typename U2>
  void operator()(F&& f, std::pair<U1, U2> const& p) const {
    std::allocator<char> a;
    detail::callWithExtractedKey<Tracked<0>, Tracked<1>>(
        a,
        std::forward<F>(f),
        std::piecewise_construct,
        std::tuple<U1>{p.first},
        std::tuple<U2>{p.second});
  }

  template <typename F, typename U1, typename U2>
  void operator()(F&& f, std::pair<U1, U2>&& p) const {
    std::allocator<char> a;
    detail::callWithExtractedKey<Tracked<0>, Tracked<1>>(
        a,
        std::forward<F>(f),
        std::piecewise_construct,
        std::tuple<U1>{std::move(p.first)},
        std::tuple<U2>{std::move(p.second)});
  }
};

TEST(Util, callWithExtractedKey) {
  runKeyExtractCases("emplace pair", DoEmplace1{});
  runKeyExtractCases("emplace k,v", DoEmplace2{});
  runKeyExtractCases("emplace pieces", DoEmplace3{});
  runKeyExtractCases("emplace pieces by value", DoEmplace3Value{}, 2);

  // Calling the default pair constructor via emplace is valid, but not
  // very useful in real life.  Verify that it works.
  std::allocator<char> a;
  detail::callWithExtractedKey<Tracked<0>, Tracked<1>>(
      a, [](Tracked<0> const& key, auto&&... args) {
        EXPECT_TRUE(key == 0);
        std::pair<Tracked<0> const, Tracked<1>> p(
            std::forward<decltype(args)>(args)...);
        EXPECT_TRUE(p.first == 0);
        EXPECT_TRUE(p.second == 0);
      });
}

TEST(Util, callWithConstructedKey) {
  Optional<Tracked<0>> sink;
  auto sinkFunc = [&](Tracked<0> const& key, auto&&... args) {
    if (!sink.hasValue()) {
      sink.emplace(std::forward<decltype(args)>(args)...);
    } else {
      EXPECT_TRUE(sink.value() == key);
    }
  };
  std::allocator<char> a;

  {
    Tracked<0> k1{0};
    Tracked<0> k2{0};
    uint64_t k3 = 0;
    sink.reset();
    resetTracking();
    detail::callWithConstructedKey<Tracked<0>>(a, sinkFunc, k1);
    // copy is expected on successful emplace
    EXPECT_EQ(Tracked<0>::counts().dist(Counts{1, 0, 0, 0}), 0);

    resetTracking();
    detail::callWithConstructedKey<Tracked<0>>(a, sinkFunc, k2);
    // no copies or moves on failing emplace with value_type
    EXPECT_EQ(Tracked<0>::counts().dist(Counts{0, 0, 0, 0}), 0);

    resetTracking();
    detail::callWithConstructedKey<Tracked<0>>(a, sinkFunc, k3);
    // copy convert expected for failing emplace with wrong type
    EXPECT_EQ(Tracked<0>::counts().dist(Counts{0, 0, 1, 0}), 0);

    sink.reset();
    resetTracking();
    detail::callWithConstructedKey<Tracked<0>>(a, sinkFunc, k3);
    // copy convert + move expected for successful emplace with wrong type
    EXPECT_EQ(Tracked<0>::counts().dist(Counts{0, 1, 1, 0}), 0);
  }
  {
    Tracked<0> k1{0};
    Tracked<0> k2{0};
    uint64_t k3 = 0;
    sink.reset();
    resetTracking();
    detail::callWithConstructedKey<Tracked<0>>(a, sinkFunc, std::move(k1));
    // move is expected on successful emplace
    EXPECT_EQ(Tracked<0>::counts().dist(Counts{0, 1, 0, 0}), 0);

    resetTracking();
    detail::callWithConstructedKey<Tracked<0>>(a, sinkFunc, std::move(k2));
    // no copies or moves on failing emplace with value_type
    EXPECT_EQ(Tracked<0>::counts().dist(Counts{0, 0, 0, 0}), 0);

    resetTracking();
    detail::callWithConstructedKey<Tracked<0>>(a, sinkFunc, std::move(k3));
    // move convert expected for failing emplace with wrong type
    EXPECT_EQ(Tracked<0>::counts().dist(Counts{0, 0, 0, 1}), 0);

    sink.reset();
    resetTracking();
    detail::callWithConstructedKey<Tracked<0>>(a, sinkFunc, std::move(k3));
    // move convert + move expected for successful emplace with wrong type
    EXPECT_EQ(Tracked<0>::counts().dist(Counts{0, 1, 0, 1}), 0);
  }

  // Calling the default pair constructor via emplace is valid, but not
  // very useful in real life.  Verify that it works.
  sink.reset();
  detail::callWithConstructedKey<Tracked<0>>(a, sinkFunc);
  EXPECT_TRUE(sink.has_value());
}

// We're deliberately allowing only a subset of the desired heterogeneous
// string behaviors with this functor so that we can verify that
// conversions will still be applied if the heterogeneous test fails.
template <typename T>
using IsStringPiece = std::is_same<T, StringPiece>;

template <typename KeyType, typename Arg1, typename Arg2>
struct ExpectArgTypes {
  int which{0};
  KeyType const* expectedAddr{nullptr};

  template <typename K, typename... Args>
  void operator()(K const&, Args&&... args) {
    // avoid static_assert to ensure we don't affect SFINAE
    EXPECT_TRUE((std::is_same<K, KeyType>::value)) << which;
    using T = std::tuple<Args&&...>;
    EXPECT_EQ(std::tuple_size<T>::value, 2) << which;
    EXPECT_TRUE((std::is_same<std::tuple_element_t<0, T>, Arg1>::value))
        << which;
    EXPECT_TRUE((std::is_same<std::tuple_element_t<1, T>, Arg2>::value))
        << which;

    auto t = std::forward_as_tuple(std::forward<Args>(args)...);
    EXPECT_TRUE(expectedAddr == nullptr || expectedAddr == &std::get<0>(t))
        << which;
  }
};

TEST(Util, callWithExtractedHeterogeneousKey) {
  std::allocator<char> a;
  std::string str{"key"};
  StringPiece sp{"key"};
  char const* ptr{"key"};
  auto strPair = std::make_pair(str, 0);
  std::pair<std::string&, int> strLRefPair(str, 0);
  std::pair<std::string&&, int> strRRefPair(std::move(str), 0);
  auto spPair = std::make_pair(sp, 0);
  auto ptrPair = std::make_pair(ptr, 0);

  // none of the std::move below actually get consumed

  detail::callWithExtractedKey<std::string, int, IsStringPiece>(
      a, ExpectArgTypes<std::string, std::string&, int&&>{0, &str}, str, 0);
  detail::callWithExtractedKey<std::string, int, IsStringPiece>(
      a,
      ExpectArgTypes<std::string, std::string const&, int const&>{
          1, &strPair.first},
      strPair);
  detail::callWithExtractedKey<std::string, int, IsStringPiece>(
      a,
      ExpectArgTypes<std::string, std::string&, int&&>{2, &str},
      std::move(strLRefPair));
  detail::callWithExtractedKey<std::string, int, IsStringPiece>(
      a,
      ExpectArgTypes<std::string, std::string&, int const&>{10, &str},
      strLRefPair);

  detail::callWithExtractedKey<std::string, int, IsStringPiece>(
      a,
      ExpectArgTypes<std::string, std::string&&, int&&>{3, &str},
      std::move(str),
      0);
  detail::callWithExtractedKey<std::string, int, IsStringPiece>(
      a,
      ExpectArgTypes<std::string, std::string&&, int&&>{4, &strPair.first},
      std::move(strPair));
  detail::callWithExtractedKey<std::string, int, IsStringPiece>(
      a,
      ExpectArgTypes<std::string, std::string&, int const&>{5, &str},
      strRRefPair);
  detail::callWithExtractedKey<std::string, int, IsStringPiece>(
      a,
      ExpectArgTypes<std::string, std::string&&, int&&>{11, &str},
      std::move(strRRefPair));

  detail::callWithExtractedKey<std::string, int, IsStringPiece>(
      a, ExpectArgTypes<StringPiece, StringPiece&, int&&>{6, &sp}, sp, 0);
  detail::callWithExtractedKey<std::string, int, IsStringPiece>(
      a,
      ExpectArgTypes<StringPiece, StringPiece const&, int const&>{
          7, &spPair.first},
      spPair);

  detail::callWithExtractedKey<std::string, int, IsStringPiece>(
      a, ExpectArgTypes<std::string, std::string&&, int&&>{8}, ptr, 0);
  detail::callWithExtractedKey<std::string, int, IsStringPiece>(
      a, ExpectArgTypes<std::string, std::string&&, int const&>{9}, ptrPair);
}

TEST(Util, callWithConstructedHeterogeneousKey) {
  std::allocator<char> a;
  std::string str{"key"};
  StringPiece sp{"key"};
  char const* ptr{"key"};
  detail::callWithConstructedKey<std::string, IsStringPiece>(
      a,
      [](auto const& key, auto&&... args) {
        // avoid static_assert to ensure we don't affect SFINAE
        EXPECT_TRUE((std::is_same<decltype(key), std::string const&>::value));
        using T = std::tuple<decltype(args)&&...>;
        EXPECT_EQ(std::tuple_size<T>::value, 1);
        EXPECT_TRUE(
            (std::is_same<std::tuple_element_t<0, T>, std::string&>::value));
      },
      str);
  detail::callWithConstructedKey<std::string, IsStringPiece>(
      a,
      [](auto const& key, auto&&... args) {
        EXPECT_TRUE((std::is_same<decltype(key), std::string const&>::value));
        using T = std::tuple<decltype(args)&&...>;
        EXPECT_EQ(std::tuple_size<T>::value, 1);
        EXPECT_TRUE(
            (std::is_same<std::tuple_element_t<0, T>, std::string&&>::value));
      },
      std::move(str));
  detail::callWithConstructedKey<std::string, IsStringPiece>(
      a,
      [](auto const& key, auto&&... args) {
        EXPECT_TRUE((std::is_same<decltype(key), StringPiece const&>::value));
        using T = std::tuple<decltype(args)&&...>;
        EXPECT_EQ(std::tuple_size<T>::value, 1);
        EXPECT_TRUE(
            (std::is_same<std::tuple_element_t<0, T>, StringPiece&>::value));
      },
      sp);
  detail::callWithConstructedKey<std::string, IsStringPiece>(
      a,
      [](auto const& key, auto&&... args) {
        // avoid static_assert to ensure we don't affect SFINAE
        EXPECT_TRUE((std::is_same<decltype(key), std::string const&>::value));
        using T = std::tuple<decltype(args)&&...>;
        EXPECT_EQ(std::tuple_size<T>::value, 1);
        EXPECT_TRUE(
            (std::is_same<std::tuple_element_t<0, T>, std::string&&>::value));
        auto t = std::forward_as_tuple(std::forward<decltype(args)>(args)...);
        EXPECT_EQ(&key, &std::get<0>(t));
      },
      ptr);
}

} // namespace folly
