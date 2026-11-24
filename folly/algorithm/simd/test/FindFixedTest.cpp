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

#include <folly/Portability.h>

#include <folly/algorithm/simd/FindFixed.h>

#include <fmt/core.h>
#include <fmt/ranges.h>

#include <folly/portability/GTest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

template <typename T, std::size_t N>
void allTestsForN(std::span<T, N> buf) {
  auto errorMsg = [&] {
    // std::byte (and similar) is not directly formattable by fmt, so widen
    // each element to an integer for the diagnostic message.
    std::vector<std::uint64_t> values;
    values.reserve(buf.size());
    for (const auto& x : buf) {
      values.push_back(static_cast<std::uint64_t>(x));
    }
    return fmt::format("looking in: {}, sizeof(T): {}", values, sizeof(T));
  };

  T foundX = static_cast<T>(0);
  T notFoundX = static_cast<T>(1);

  std::fill_n(buf.begin(), N, foundX);
  std::span<const T, N> cbuf = buf;
  ASSERT_EQ(std::nullopt, folly::findFixed(cbuf, notFoundX)) << errorMsg();
  if (N == 0) {
    return;
  }

  ASSERT_EQ(0, folly::findFixed(cbuf, foundX)) << errorMsg();
  for (std::size_t found = 1; found < N; ++found) {
    buf[found - 1] = notFoundX;
    ASSERT_EQ(found, folly::findFixed(cbuf, foundX)) << errorMsg();
  }
}

template <typename T, std::size_t... idx>
void allTestsForImpl(std::span<T> buf, std::index_sequence<idx...>) {
  (allTestsForN(std::span<T, idx>(buf.data(), idx)), ...);
}

template <typename T>
void allTestsFor() {
  constexpr std::size_t kMaxSize = 64 / sizeof(T);
  std::vector<T> buf;
  buf.resize(2 * kMaxSize, static_cast<T>(0));

  // simd code can depend on alignment, so we better test it
  for (std::size_t offset = 0; offset != kMaxSize; ++offset) {
    ASSERT_NO_FATAL_FAILURE(allTestsForImpl(
        std::span(buf.data() + offset, kMaxSize),
        std::make_index_sequence<kMaxSize + 1>{}))
        << offset;
  }
}

} // namespace

TEST(FindFixed, Basic) {
  // Int is an important case, it should work
  ASSERT_NO_FATAL_FAILURE(allTestsFor<int>());

  ASSERT_NO_FATAL_FAILURE(allTestsFor<std::int8_t>());
  ASSERT_NO_FATAL_FAILURE(allTestsFor<std::int16_t>());
  ASSERT_NO_FATAL_FAILURE(allTestsFor<std::int32_t>());
  ASSERT_NO_FATAL_FAILURE(allTestsFor<std::int64_t>());

  ASSERT_NO_FATAL_FAILURE(allTestsFor<std::uint8_t>());
  ASSERT_NO_FATAL_FAILURE(allTestsFor<std::uint16_t>());
  ASSERT_NO_FATAL_FAILURE(allTestsFor<std::uint32_t>());
  ASSERT_NO_FATAL_FAILURE(allTestsFor<std::uint64_t>());

  // just a enum test
  ASSERT_NO_FATAL_FAILURE(allTestsFor<std::byte>());
}

TEST(FindFixed, Interfaces) {
  // constexpr
  {
    constexpr std::array<std::int64_t, 3> arr{1, 2, 3};
    static_assert(folly::findFixed(std::span(std::as_const(arr)), 1) == 0);
    static_assert(folly::findFixed(std::span(std::as_const(arr)), 3) == 2);
    static_assert(
        folly::findFixed(std::span(std::as_const(arr)), 4) == std::nullopt);
  }

  // array
  {
    std::array<int, 3> arr{1, 2, 3};
    ASSERT_EQ(std::nullopt, folly::findFixed(std::span(std::as_const(arr)), 0));
    ASSERT_EQ(1, folly::findFixed(std::span(std::as_const(arr)), 2));
  }

  // mutable span
  {
    std::array<int, 3> arr{1, 2, 3};
    std::span<int, 3> s(arr);
    ASSERT_EQ(std::nullopt, folly::findFixed(std::span<const int, 3>(s), 0));
    ASSERT_EQ(1, folly::findFixed(std::span<const int, 3>(s), 2));
  }
}

template <typename T>
concept findFixedWorksFor = requires(const T& x) {
  { folly::findFixed(x) };
};

TEST(FollyFindFixed, SfianeFriendlyUnsupportedTypes) {
  EXPECT_FALSE(findFixedWorksFor<std::span<const int>>)
      << "dynamic extend is not supported";
  EXPECT_FALSE(findFixedWorksFor<std::vector<int>>)
      << "vector is dynamic size by definition";
  EXPECT_FALSE((findFixedWorksFor<std::span<const std::vector<int>, 3>>))
      << "find fixed works only for some trivial types.";
}
