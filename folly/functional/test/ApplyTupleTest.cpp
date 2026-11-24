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

#include <folly/functional/ApplyTuple.h>

#include <algorithm>
#include <utility>

#include <folly/Overload.h>
#include <folly/portability/GTest.h>

TEST(ApplyTuple, UncurryCopyMove) {
  std::string separator = "================================\n";
  auto formatRow = folly::uncurry([=](std::string a, std::string b) {
    // capture separator by copy
    return separator + a + "\n" + b + "\n" + separator;
  });
  auto row = std::make_tuple("hello", "world");
  auto expected = separator + "hello\nworld\n" + separator;
  EXPECT_EQ(expected, formatRow(row));
  auto formatRowCopy = formatRow;
  EXPECT_EQ(expected, formatRowCopy(row));
  auto formatRowMove = std::move(formatRow);
  EXPECT_EQ(expected, formatRowMove(row));

  // capture value moved out from formatRow
  EXPECT_NE(expected, formatRow(row));
}

TEST(ApplyTuple, Uncurry) {
  EXPECT_EQ(42, folly::uncurry([](int x, int y) {
              return x * y;
            })(std::pair<int, int>(6, 7)));
  EXPECT_EQ(42, folly::uncurry([](int&& x, int&& y) {
              return x * y;
            })(std::pair<int&&, int&&>(6, 7)));
  EXPECT_EQ(42, folly::uncurry([](int&& x, int&& y) {
              return x * y;
            })(std::pair<int&&, int&&>(6, 7)));

  std::string long1 = "a long string exceeding small string size";
  std::string long2 = "and here is another one!";
  std::string expected = long1 + long2;

  auto cat = folly::uncurry([](std::string a, std::string b) {
    return std::move(a) + std::move(b);
  });

  EXPECT_EQ(expected, cat(std::make_pair(long1, long2)));
  EXPECT_FALSE(long1.empty());
  EXPECT_FALSE(long2.empty());
  EXPECT_EQ(expected, cat(std::tie(long1, long2)));
  EXPECT_FALSE(long1.empty());
  EXPECT_FALSE(long2.empty());
  EXPECT_EQ(
      expected, cat(std::forward_as_tuple(std::move(long1), std::move(long2))));
  EXPECT_TRUE(long1.empty());
  EXPECT_TRUE(long2.empty());
}

TEST(ApplyTuple, UncurryStdFind) {
  std::vector<std::pair<int, int>> v{{1, 9}, {2, 8}, {3, 7}, {4, 6}, {5, 5}};
  EXPECT_EQ(
      3, std::count_if(v.begin(), v.end(), folly::uncurry([](int a, int b) {
                         return b % a == 0;
                       })));
}

namespace {
struct S {
  template <typename... Args>
  explicit S(Args&&... args) : tuple_(std::forward<Args>(args)...) {}

  std::tuple<int, double, std::string> tuple_;
};
} // namespace

TEST(MakeIndexSequenceFromTuple, Basic) {
  using folly::index_sequence_for_tuple;
  using OneElementTuple = std::tuple<int>;
  using TwoElementTuple = std::tuple<int>;

  EXPECT_TRUE((std::is_same<
               index_sequence_for_tuple<OneElementTuple>,
               std::index_sequence<0>>::value));
  EXPECT_TRUE((std::is_same<
               index_sequence_for_tuple<const OneElementTuple>,
               std::index_sequence<0>>::value));

  EXPECT_TRUE((std::is_same<
               index_sequence_for_tuple<TwoElementTuple>,
               std::index_sequence<0>>::value));
  EXPECT_TRUE((std::is_same<
               index_sequence_for_tuple<const TwoElementTuple>,
               std::index_sequence<0>>::value));
}

TEST(ApplyResult, Basic) {
  {
    auto f = [](auto) -> int { return {}; };
    using F = decltype(f);
    EXPECT_TRUE(
        (std::is_same<folly::apply_result_t<F, std::tuple<int>>, int>{}));
  }

  {
    auto f = folly::overload(
        [](int) {},
        [](double) -> double { return {}; },
        [](int, int) -> int { return {}; });
    using F = decltype(f);

    EXPECT_TRUE(
        (std::is_same<folly::apply_result_t<F, std::tuple<int>>, void>::value));
    EXPECT_TRUE((
        std::is_same<folly::apply_result_t<F, std::tuple<double>>, double>::
            value));
    EXPECT_TRUE((
        std::is_same<folly::apply_result_t<F, std::tuple<int, int>>, int>::
            value));
  }
}

TEST(IsApplicable, Basic) {
  {
    auto f = [] {};
    using F = decltype(f);
    EXPECT_TRUE((folly::is_applicable_v<F, std::tuple<>>));
    EXPECT_FALSE((folly::is_applicable_v<F, std::tuple<int>>));
  }
  {
    auto f = folly::overload([](int) {}, [](double) -> double { return {}; });
    using F = decltype(f);
    EXPECT_TRUE((folly::is_applicable_v<F, std::tuple<double>>));
    EXPECT_TRUE((folly::is_applicable_v<F, std::tuple<int>>));
    EXPECT_FALSE((folly::is_applicable_v<F, std::tuple<>>));
    EXPECT_FALSE((folly::is_applicable_v<F, std::tuple<int, double>>));
  }
}

TEST(IsNothrowApplicable, Basic) {
  {
    auto f = []() noexcept {};
    using F = decltype(f);
    EXPECT_TRUE((folly::is_nothrow_applicable_v<F, std::tuple<>>));
    EXPECT_FALSE((folly::is_nothrow_applicable_v<F, std::tuple<int>>));
  }
  {
    auto f = folly::overload(
        [](int) noexcept {}, [](double) -> double { return {}; });
    using F = decltype(f);
    EXPECT_FALSE((folly::is_nothrow_applicable_v<F, std::tuple<double>>));
    EXPECT_TRUE((folly::is_nothrow_applicable_v<F, std::tuple<int>>));
    EXPECT_FALSE((folly::is_nothrow_applicable_v<F, std::tuple<>>));
    EXPECT_FALSE((folly::is_nothrow_applicable_v<F, std::tuple<int, double>>));
  }
}

TEST(IsApplicableR, Basic) {
  {
    auto f = []() -> int { return {}; };
    using F = decltype(f);
    EXPECT_TRUE((folly::is_applicable_r_v<double, F, std::tuple<>>));
    EXPECT_FALSE((folly::is_applicable_r_v<double, F, std::tuple<int>>));
  }
  {
    auto f = folly::overload(
        [](int) noexcept {}, [](double) -> double { return {}; });
    using F = decltype(f);
    EXPECT_TRUE((folly::is_applicable_r_v<float, F, std::tuple<double>>));
    EXPECT_TRUE((folly::is_applicable_r_v<void, F, std::tuple<int>>));
    EXPECT_FALSE((folly::is_applicable_r_v<void, F, std::tuple<>>));
    EXPECT_FALSE(
        (folly::is_applicable_r_v<double, F, std::tuple<int, double>>));
  }
}

TEST(IsNothrowApplicableR, Basic) {
  {
    auto f = []() noexcept -> int { return {}; };
    using F = decltype(f);
    EXPECT_TRUE((folly::is_nothrow_applicable_r_v<double, F, std::tuple<>>));
    EXPECT_FALSE(
        (folly::is_nothrow_applicable_r_v<double, F, std::tuple<int>>));
  }
  {
    auto f = folly::overload(
        [](int) noexcept {}, [](double) -> double { return {}; });
    using F = decltype(f);
    EXPECT_FALSE(
        (folly::is_nothrow_applicable_r_v<float, F, std::tuple<double>>));
    EXPECT_TRUE((folly::is_nothrow_applicable_r_v<void, F, std::tuple<int>>));
    EXPECT_FALSE((folly::is_nothrow_applicable_r_v<void, F, std::tuple<>>));
    EXPECT_FALSE(
        (folly::is_nothrow_applicable_r_v<double, F, std::tuple<int, double>>));
  }
}

TEST(ForwardTuple, Basic) {
  auto tuple = std::make_tuple(1, 2.0);

  EXPECT_TRUE((std::is_same<
               decltype(folly::forward_tuple(tuple)),
               std::tuple<int&, double&>>::value));
  EXPECT_EQ(folly::forward_tuple(tuple), tuple);
  EXPECT_TRUE((std::is_same<
               decltype(folly::forward_tuple(std::as_const(tuple))),
               std::tuple<const int&, const double&>>::value));
  EXPECT_EQ(folly::forward_tuple(std::as_const(tuple)), tuple);

  EXPECT_TRUE((std::is_same<
               decltype(folly::forward_tuple(std::move(tuple))),
               std::tuple<int&&, double&&>>::value));
  EXPECT_EQ(folly::forward_tuple(std::move(tuple)), tuple);
  EXPECT_TRUE(
      (std::is_same<
          // NOLINTNEXTLINE(performance-move-const-arg)
          decltype(folly::forward_tuple(std::move(std::as_const(tuple)))),
          std::tuple<const int&&, const double&&>>::value));
  // NOLINTNEXTLINE(performance-move-const-arg)
  EXPECT_EQ(folly::forward_tuple(std::move(std::as_const(tuple))), tuple);

  auto integer = 1;
  auto floating_point = 2.0;
  auto ref_tuple = std::forward_as_tuple(integer, std::move(floating_point));

  EXPECT_TRUE((std::is_same<
               decltype(folly::forward_tuple(ref_tuple)),
               std::tuple<int&, double&>>::value));

  EXPECT_TRUE((std::is_same<
               decltype(folly::forward_tuple(std::move(ref_tuple))),
               std::tuple<int&, double&&>>::value));

  EXPECT_TRUE(
      (std::is_same<
          decltype(std::tuple_cat(
              folly::forward_tuple(tuple),
              folly::forward_tuple(std::move(tuple)))),
          std::tuple<int&, double&, int&&, double&&>>::value));
}
