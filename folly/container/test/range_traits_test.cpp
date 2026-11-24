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

#include <folly/container/range_traits.h>

#include <array>
#include <list>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <span>

#include <folly/portability/GTest.h>

struct RangeTraitsTest : testing::Test {};

namespace {

template <typename T>
void takeByCopyListInitialization(T);

template <typename T>
concept implicitly_default_constructible = requires {
  takeByCopyListInitialization<T>({});
};

struct FromRangeConstructible {
  template <typename R>
  explicit FromRangeConstructible(folly::from_range_t, const R& range)
      : size{std::size(range)} {}

  std::size_t size;
};

} // namespace

TEST_F(RangeTraitsTest, from_range) {
  static_assert(
      std::is_same_v<decltype(folly::from_range), const folly::from_range_t>);
  static_assert(std::is_trivially_copyable_v<folly::from_range_t>);
  static_assert(std::is_default_constructible_v<folly::from_range_t>);
  static_assert(!implicitly_default_constructible<folly::from_range_t>);
  static_assert(implicitly_default_constructible<std::vector<int>>);

  const std::vector<int> values{1, 2, 3};
  EXPECT_EQ(3, FromRangeConstructible(folly::from_range, values).size);

#if defined(__cpp_lib_ranges_to_container) && \
    __cpp_lib_ranges_to_container >= 202202L
  static_assert(std::is_same_v<folly::from_range_t, std::from_range_t>);
  EXPECT_EQ(3, FromRangeConstructible(std::from_range, values).size);
#endif
}

TEST_F(RangeTraitsTest, is_contiguous_range_v) {
  enum some_enum {};

  EXPECT_FALSE((folly::is_contiguous_range_v<void>));
  EXPECT_FALSE((folly::is_contiguous_range_v<int>));
  EXPECT_FALSE((folly::is_contiguous_range_v<int&>));
  EXPECT_FALSE((folly::is_contiguous_range_v<int[]>));
  EXPECT_FALSE((folly::is_contiguous_range_v<int const[]>));
  EXPECT_FALSE((folly::is_contiguous_range_v<int*>));
  EXPECT_FALSE((folly::is_contiguous_range_v<int const*>));
  EXPECT_FALSE((folly::is_contiguous_range_v<int()>));
  EXPECT_FALSE((folly::is_contiguous_range_v<some_enum>));
  EXPECT_FALSE((folly::is_contiguous_range_v<std::list<int>>));

  EXPECT_TRUE((folly::is_contiguous_range_v<int[7]>));
  EXPECT_TRUE((folly::is_contiguous_range_v<int const[7]>));
  EXPECT_TRUE((folly::is_contiguous_range_v<std::array<int, 7>>));
  EXPECT_TRUE((folly::is_contiguous_range_v<std::string>));
  EXPECT_TRUE((folly::is_contiguous_range_v<std::string_view>));
  EXPECT_TRUE((folly::is_contiguous_range_v<std::vector<int>>));
  EXPECT_FALSE((folly::is_contiguous_range_v<std::vector<bool>>));
  EXPECT_TRUE((folly::is_contiguous_range_v<std::span<int>>));
  EXPECT_TRUE((folly::is_contiguous_range_v<std::span<int const>>));
  EXPECT_TRUE((folly::is_contiguous_range_v<std::span<int, 7>>));
  EXPECT_TRUE((folly::is_contiguous_range_v<std::span<int const, 7>>));
}
