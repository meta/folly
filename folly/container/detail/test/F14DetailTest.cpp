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

#include <folly/container/detail/F14Table.h>

#include <folly/Traits.h>
#include <folly/portability/GTest.h>

template <typename A, typename B>
using detect_op_eq = decltype(FOLLY_DECLVAL(A) == FOLLY_DECLVAL(B));

struct F14HashedKeyTest : testing::Test {};

TEST_F(F14HashedKeyTest, string) {
  using namespace std::literals;
  folly::F14HashedKey<std::string> key{"foo"};

  EXPECT_EQ("foo", key);
  EXPECT_EQ(key, "foo");
  EXPECT_NE("bar", key);
  EXPECT_NE(key, "bar");

  EXPECT_EQ("foo"s, key);
  EXPECT_EQ(key, "foo"s);
  EXPECT_NE("bar"s, key);
  EXPECT_NE(key, "bar"s);

  EXPECT_EQ("foo"sv, key);
  EXPECT_EQ(key, "foo"sv);
  EXPECT_NE("bar"sv, key);
  EXPECT_NE(key, "bar"sv);
}

TEST_F(F14HashedKeyTest, transparent) {
  struct Key {
    int num{};
    Key(double, int num_) : num{num_} {}
  };
  static_assert(!std::is_constructible_v<Key, int>);
  static_assert(!std::is_constructible_v<int, Key>);
  static_assert(!folly::is_detected_v<detect_op_eq, Key, Key>);
  static_assert(!folly::is_detected_v<detect_op_eq, Key, int>);
  static_assert(!folly::is_detected_v<detect_op_eq, int, Key>);
  struct KeyHash {
    using is_transparent = void;
    size_t operator()(Key key) const { return key.num; }
    [[maybe_unused]] size_t operator()(int key) const;
  };
  struct KeyEqual {
    using is_transparent = void;
    bool operator()(Key a, Key b) const { return a.num == b.num; }
    bool operator()(Key a, int b) const { return a.num == b; }
    bool operator()(int a, Key b) const { return a == b.num; }
  };
  using HKey = folly::F14HashedKey<Key, KeyHash, KeyEqual>;

  HKey key{Key{0., 7}};

  EXPECT_TRUE(key == Key(0., 7));
  EXPECT_FALSE(key == Key(0., 8));
  EXPECT_TRUE(Key(0., 7) == key);
  EXPECT_FALSE(Key(0., 8) == key);

  EXPECT_TRUE(key == 7);
  EXPECT_FALSE(key == 8);
  EXPECT_TRUE(7 == key);
  EXPECT_FALSE(8 == key);

  EXPECT_TRUE(key == HKey(Key(0., 7)));
  EXPECT_FALSE(key == HKey(Key(0., 8)));
  EXPECT_TRUE(HKey(Key(0., 7)) == key);
  EXPECT_FALSE(HKey(Key(0., 8)) == key);
}

template <typename SizeAndChunkShiftT>
void testSizeAndChunkShift() {
  SizeAndChunkShiftT sz;
  EXPECT_EQ(sz.size(), 0);
  EXPECT_EQ(sz.chunkShift(), 0);
  EXPECT_EQ(sz.scale(), 0);

  sz.setSize(12345678);
  EXPECT_EQ(sz.size(), 12345678);
  EXPECT_EQ(sz.chunkShift(), 0);
  EXPECT_EQ(sz.scale(), 0);

  sz.setChunkCountAndScale(1, 40960);
  EXPECT_EQ(sz.size(), 12345678);
  EXPECT_EQ(sz.chunkCount(), 1);
  EXPECT_EQ(sz.chunkShift(), 0);
  EXPECT_EQ(sz.scale(), 40960);

  sz.incrementSize();
  EXPECT_EQ(sz.size(), 12345679);
  EXPECT_EQ(sz.chunkShift(), 0);
  EXPECT_EQ(sz.scale(), 40960);

  sz.decrementSize();
  EXPECT_EQ(sz.size(), 12345678);
  EXPECT_EQ(sz.chunkShift(), 0);
  EXPECT_EQ(sz.scale(), 40960);

  SizeAndChunkShiftT other;
  other.setSize(42);
  other.setChunkCountAndScale(8, 123);
  sz.swap(other);
  EXPECT_EQ(sz.size(), 42);
  EXPECT_EQ(sz.chunkCount(), 8);
  EXPECT_EQ(sz.scale(), 123);
  EXPECT_EQ(other.size(), 12345678);
  EXPECT_EQ(other.chunkCount(), 1);
  EXPECT_EQ(other.scale(), 40960);
  sz.swap(other);

  for (int shift = 0; shift <= SizeAndChunkShiftT::kMaxSupportedChunkShift;
       ++shift) {
    const auto count = (std::size_t{1} << shift);
    const std::size_t scale = (shift * 997) & 0xffff;
    sz.setChunkCountAndScale(count, scale);
    EXPECT_EQ(sz.size(), 12345678);
    EXPECT_EQ(sz.chunkCount(), count);
    EXPECT_EQ(sz.chunkShift(), shift);
    EXPECT_EQ(sz.scale(), scale);

    sz.setSize(SizeAndChunkShiftT::kMaxSize);
    EXPECT_EQ(sz.size(), SizeAndChunkShiftT::kMaxSize);
    EXPECT_EQ(sz.chunkShift(), shift);
    EXPECT_EQ(sz.scale(), scale);

    sz.setSize(12345678);
    EXPECT_EQ(sz.size(), 12345678);
    EXPECT_EQ(sz.chunkShift(), shift);
    EXPECT_EQ(sz.scale(), scale);
  }
}

TEST(F14SizeAndChunkShift, packed) {
  static_assert(
      sizeof(folly::f14::detail::PackedSizeAndChunkShift) == sizeof(uint64_t));
  if constexpr (sizeof(uint64_t) == sizeof(std::size_t)) {
    testSizeAndChunkShift<folly::f14::detail::PackedSizeAndChunkShift>();
  }
}

TEST(F14SizeAndChunkShift, unpacked) {
  static_assert(
      sizeof(folly::f14::detail::UnpackedSizeAndChunkShift) ==
      2 * sizeof(std::size_t));
  testSizeAndChunkShift<folly::f14::detail::UnpackedSizeAndChunkShift>();
}
