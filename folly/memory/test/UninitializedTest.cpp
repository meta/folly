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

#include <folly/memory/Uninitialized.h>

#include <array>
#include <concepts>
#include <deque>
#include <forward_list>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <folly/portability/GTest.h>

namespace folly::test {
namespace {

template <typename T, size_t N>
struct Storage {
  T* data = std::allocator<T>{}.allocate(N);
  size_t size = 0;

  Storage() = default;
  Storage(const Storage&) = delete;
  Storage& operator=(const Storage&) = delete;

  ~Storage() {
    std::destroy_n(data, size);
    std::allocator<T>{}.deallocate(data, N);
  }
};

struct CopyOverload {
  int value;

  explicit CopyOverload(int v) : value(v) {}
  CopyOverload(const CopyOverload&) = default;
  CopyOverload& operator=(const CopyOverload&) = default;

  template <typename T>
    requires std::same_as<T, CopyOverload>
  explicit CopyOverload(T& other) : value(other.value + 100) {}
};

static_assert(std::is_trivially_copyable_v<CopyOverload>);
static_assert(
    std::is_trivially_constructible_v<CopyOverload, const CopyOverload&>);
static_assert(!std::is_trivially_constructible_v<CopyOverload, CopyOverload&>);

struct ThrowingCopy {
  int value;
  int* live;

  ThrowingCopy(int v, int& count) : value(v), live(&count) { ++*live; }

  ThrowingCopy(const ThrowingCopy& other)
      : value(other.value), live(other.live) {
    if (value == 2) {
      throw std::runtime_error("copy");
    }
    ++*live;
  }

  ThrowingCopy& operator=(const ThrowingCopy&) = delete;

  ~ThrowingCopy() { --*live; }
};

} // namespace

TEST(Uninitialized, copyTrivialPointers) {
  constexpr int source[] = {3, 1, 4, 1, 5};
  Storage<int, 6> destination;
  std::construct_at(destination.data + 5, 99);

  EXPECT_EQ(
      destination.data + 5,
      folly::uninitialized_copy(
          std::begin(source), std::end(source), destination.data));
  destination.size = 6;

  const std::vector<int> expected = {3, 1, 4, 1, 5, 99};
  EXPECT_EQ(expected, std::vector<int>(destination.data, destination.data + 6));
}

TEST(Uninitialized, copyTrivialPairs) {
  using Value = std::pair<const int, double>;
  const Value source[] = {{1, 0.5}, {2, 1.5}, {3, 2.5}};
  Storage<Value, 3> destination;

  EXPECT_EQ(
      destination.data + 3,
      folly::uninitialized_copy(
          std::begin(source), std::end(source), destination.data));
  destination.size = 3;

  EXPECT_EQ(
      std::vector<Value>(std::begin(source), std::end(source)),
      std::vector<Value>(destination.data, destination.data + 3));
}

TEST(Uninitialized, copyEmptyPointers) {
  const int* first = nullptr;
  int* result = nullptr;
  EXPECT_EQ(result, folly::uninitialized_copy(first, first, result));

  constexpr int source[] = {1, 2};
  Storage<int, 1> destination;
  EXPECT_EQ(
      destination.data,
      folly::uninitialized_copy(
          std::end(source), std::end(source), destination.data));
}

TEST(Uninitialized, copyNonTrivialPointers) {
  std::string source[] = {std::string(64, 'a'), std::string(64, 'b')};
  const std::vector<std::string> expected(std::begin(source), std::end(source));
  Storage<std::string, 2> destination;

  EXPECT_EQ(
      destination.data + 2,
      folly::uninitialized_copy(
          std::begin(source), std::end(source), destination.data));
  destination.size = 2;
  source[0] = "changed";

  EXPECT_EQ(
      expected,
      std::vector<std::string>(destination.data, destination.data + 2));
}

TEST(Uninitialized, copyGenericInput) {
  const std::forward_list<int> source = {3, 1, 4};
  Storage<int, 3> destination;

  EXPECT_EQ(
      destination.data + 3,
      folly::uninitialized_copy(
          source.begin(), source.end(), destination.data));
  destination.size = 3;

  const std::vector<int> expected = {3, 1, 4};
  EXPECT_EQ(expected, std::vector<int>(destination.data, destination.data + 3));
}

TEST(Uninitialized, copyGenericOutput) {
  constexpr int source[] = {3, 1, 4};
  Storage<int, 3> destination;
  const auto result = std::make_reverse_iterator(destination.data + 3);

  EXPECT_EQ(
      std::make_reverse_iterator(destination.data),
      folly::uninitialized_copy(std::begin(source), std::end(source), result));
  destination.size = 3;

  const std::vector<int> expected = {4, 1, 3};
  EXPECT_EQ(expected, std::vector<int>(destination.data, destination.data + 3));
}

TEST(Uninitialized, copyConvertingPointers) {
  constexpr short source[] = {3, 1, 4};
  Storage<long, 3> destination;

  EXPECT_EQ(
      destination.data + 3,
      folly::uninitialized_copy(
          std::begin(source), std::end(source), destination.data));
  destination.size = 3;

  const std::vector<long> expected = {3, 1, 4};
  EXPECT_EQ(
      expected, std::vector<long>(destination.data, destination.data + 3));
}

TEST(Uninitialized, copyPreservesConstructorOverload) {
  CopyOverload source[] = {CopyOverload(3), CopyOverload(7)};
  Storage<CopyOverload, 2> mutableCopy;
  EXPECT_EQ(
      mutableCopy.data + 2,
      folly::uninitialized_copy(
          std::begin(source), std::end(source), mutableCopy.data));
  mutableCopy.size = 2;
  EXPECT_EQ(103, mutableCopy.data[0].value);
  EXPECT_EQ(107, mutableCopy.data[1].value);

  const auto& constSource = source;
  Storage<CopyOverload, 2> constCopy;
  EXPECT_EQ(
      constCopy.data + 2,
      folly::uninitialized_copy(
          std::begin(constSource), std::end(constSource), constCopy.data));
  constCopy.size = 2;
  EXPECT_EQ(3, constCopy.data[0].value);
  EXPECT_EQ(7, constCopy.data[1].value);
}

TEST(Uninitialized, copyVolatileInput) {
  // NOLINTNEXTLINE
  volatile int source[] = {3, 1, 4};
  Storage<int, 3> destination;

  EXPECT_EQ(
      destination.data + 3,
      folly::uninitialized_copy(
          std::begin(source), std::end(source), destination.data));
  destination.size = 3;

  const std::vector<int> expected = {3, 1, 4};
  EXPECT_EQ(expected, std::vector<int>(destination.data, destination.data + 3));
}

TEST(Uninitialized, copyMoveIterators) {
  std::unique_ptr<int> source[] = {
      std::make_unique<int>(3),
      std::make_unique<int>(7),
  };
  Storage<std::unique_ptr<int>, 2> destination;

  EXPECT_EQ(
      destination.data + 2,
      folly::uninitialized_copy(
          std::make_move_iterator(std::begin(source)),
          std::make_move_iterator(std::end(source)),
          destination.data));
  destination.size = 2;

  EXPECT_EQ(nullptr, source[0]);
  EXPECT_EQ(nullptr, source[1]);
  EXPECT_EQ(3, *destination.data[0]);
  EXPECT_EQ(7, *destination.data[1]);
}

TEST(Uninitialized, copyRollsBackOnException) {
  int live = 0;
  const ThrowingCopy source[] = {
      {1, live},
      {2, live},
      {3, live},
  };
  Storage<ThrowingCopy, 3> destination;
  EXPECT_EQ(3, live);

  EXPECT_THROW(
      folly::uninitialized_copy(
          std::begin(source), std::end(source), destination.data),
      std::runtime_error);
  EXPECT_EQ(3, live);
}

TEST(Uninitialized, copyContiguousIterators) {
  const std::vector<int> source = {3, 1, 4, 1, 5};
  using Input = std::counted_iterator<decltype(source.begin())>;
  using Output = std::counted_iterator<int*>;
  static_assert(std::contiguous_iterator<Input>);
  static_assert(std::contiguous_iterator<Output>);
  Storage<int, 5> destination;
  const Input first(source.begin(), 5);
  const Output result(destination.data, 5);

  const auto end = folly::uninitialized_copy(first, first + 5, result);
  destination.size = 5;

  EXPECT_EQ(result + 5, end);
  EXPECT_EQ(0, end.count());
  EXPECT_EQ(destination.data + 5, end.base());
  EXPECT_EQ(source, std::vector<int>(destination.data, destination.data + 5));
}

TEST(Uninitialized, copyEmptyContiguousIterators) {
  using Input = std::counted_iterator<const int*>;
  using Output = std::counted_iterator<int*>;
  static_assert(std::contiguous_iterator<Input>);
  static_assert(std::contiguous_iterator<Output>);
  const Input first;
  const Output result;
  EXPECT_EQ(result, folly::uninitialized_copy(first, first, result));

  constexpr int source[] = {1};
  const Input last(std::end(source), 0);
  EXPECT_EQ(result, folly::uninitialized_copy(last, last, result));
}

TEST(Uninitialized, copyNonContiguousRandomAccessIterators) {
  std::vector<int> expected;
  expected.reserve(2048);
  for (int i = 0; i < 2048; ++i) {
    expected.push_back(i * 7 + 3);
  }
  const std::deque<int> source(expected.begin(), expected.end());
  static_assert(std::random_access_iterator<decltype(source.begin())>);
  static_assert(!std::contiguous_iterator<decltype(source.begin())>);
  Storage<int, 2048> destination;

  EXPECT_EQ(
      destination.data + 2048,
      folly::uninitialized_copy(
          source.begin(), source.end(), destination.data));
  destination.size = 2048;

  EXPECT_EQ(
      expected,
      std::vector<int>(destination.data, destination.data + destination.size));
}

TEST(Uninitialized, copyContiguousPreservesConstructorOverload) {
  CopyOverload source[] = {CopyOverload(3), CopyOverload(7)};
  Storage<CopyOverload, 2> mutableCopy;
  const std::counted_iterator<CopyOverload*> first(source, 2);
  const std::counted_iterator<CopyOverload*> result(mutableCopy.data, 2);
  EXPECT_EQ(result + 2, folly::uninitialized_copy(first, first + 2, result));
  mutableCopy.size = 2;
  EXPECT_EQ(103, mutableCopy.data[0].value);
  EXPECT_EQ(107, mutableCopy.data[1].value);

  Storage<CopyOverload, 2> constCopy;
  const std::counted_iterator<const CopyOverload*> constFirst(source, 2);
  const std::counted_iterator<CopyOverload*> constResult(constCopy.data, 2);
  EXPECT_EQ(
      constResult + 2,
      folly::uninitialized_copy(constFirst, constFirst + 2, constResult));
  constCopy.size = 2;
  EXPECT_EQ(3, constCopy.data[0].value);
  EXPECT_EQ(7, constCopy.data[1].value);
}

} // namespace folly::test
