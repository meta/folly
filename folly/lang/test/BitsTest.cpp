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

#include <folly/lang/Bits.h>

#include <algorithm>
#include <bit>
#include <random>
#include <vector>

#include <fmt/core.h>

#include <folly/Random.h>
#include <folly/portability/GTest.h>

namespace folly {

// Test constexpr-ness.
#if !defined(__clang__) && !defined(_MSC_VER)
static_assert(findFirstSet(2u) == 2, "findFirstSet");
static_assert(findLastSet(2u) == 2, "findLastSet");
static_assert(nextPowTwo(2u) == 2, "nextPowTwo");
#endif

#ifndef __clang__
static_assert(isPowTwo(2u), "isPowTwo");
#endif

namespace {

template <class INT>
void testFFS() {
  EXPECT_EQ(0, findFirstSet(static_cast<INT>(0)));
  size_t bits =
      std::numeric_limits<typename std::make_unsigned<INT>::type>::digits;
  for (size_t i = 0; i < bits; i++) {
    INT v = (static_cast<INT>(1) << (bits - 1)) | (static_cast<INT>(1) << i);
    EXPECT_EQ(i + 1, findFirstSet(v));
  }
}

template <class INT>
void testFLS() {
  using UINT_T = typename std::make_unsigned<INT>::type;
  EXPECT_EQ(0, findLastSet(static_cast<INT>(0)));
  size_t bits = std::numeric_limits<UINT_T>::digits;
  for (size_t i = 0; i < bits; i++) {
    INT v1 = static_cast<UINT_T>(1) << i;
    EXPECT_EQ(i + 1, findLastSet(v1));

    INT v2 = (static_cast<UINT_T>(1) << i) - 1;
    EXPECT_EQ(i, findLastSet(v2));
  }
}

template <class UINT>
void testEFS() {
  EXPECT_EQ(0, extractFirstSet(static_cast<UINT>(0)));
  size_t bits = std::numeric_limits<UINT>::digits;
  for (size_t i = 0; i < bits; i++) {
    UINT lsb = static_cast<UINT>(1) << i;
    UINT v = (static_cast<UINT>(1) << (bits - 1)) | lsb;
    EXPECT_EQ(lsb, extractFirstSet(v));
  }
}

template <typename T>
struct BitsAllUintsTest : ::testing::Test {};

using UintsToTest =
    ::testing::Types<std::uint8_t, std::uint16_t, std::uint32_t, std::uint64_t>;

TYPED_TEST_SUITE(BitsAllUintsTest, UintsToTest);

} // namespace

TEST(Bits, FindFirstSet) {
  testFFS<char>();
  testFFS<signed char>();
  testFFS<unsigned char>();
  testFFS<short>();
  testFFS<unsigned short>();
  testFFS<int>();
  testFFS<unsigned int>();
  testFFS<long>();
  testFFS<unsigned long>();
  testFFS<long long>();
  testFFS<unsigned long long>();
}

TEST(Bits, FindLastSet) {
  testFLS<char>();
  testFLS<signed char>();
  testFLS<unsigned char>();
  testFLS<short>();
  testFLS<unsigned short>();
  testFLS<int>();
  testFLS<unsigned int>();
  testFLS<long>();
  testFLS<unsigned long>();
  testFLS<long long>();
  testFLS<unsigned long long>();
}

TEST(Bits, ExtractFirstSet) {
  testEFS<unsigned char>();
  testEFS<unsigned short>();
  testEFS<unsigned int>();
  testEFS<unsigned long>();
  testEFS<unsigned long long>();
}

TEST(Bits, nextPowTwoClz) {
  EXPECT_EQ(1, nextPowTwo(0u));
  EXPECT_EQ(1, nextPowTwo(1u));
  EXPECT_EQ(2, nextPowTwo(2u));
  EXPECT_EQ(4, nextPowTwo(3u));
  EXPECT_EQ(4, nextPowTwo(4u));
  EXPECT_EQ(8, nextPowTwo(5u));
  EXPECT_EQ(8, nextPowTwo(6u));
  EXPECT_EQ(8, nextPowTwo(7u));
  EXPECT_EQ(8, nextPowTwo(8u));
  EXPECT_EQ(16, nextPowTwo(9u));
  EXPECT_EQ(16, nextPowTwo(13u));
  EXPECT_EQ(16, nextPowTwo(16u));
  EXPECT_EQ(512, nextPowTwo(510u));
  EXPECT_EQ(512, nextPowTwo(511u));
  EXPECT_EQ(512, nextPowTwo(512u));
  EXPECT_EQ(1024, nextPowTwo(513u));
  EXPECT_EQ(1024, nextPowTwo(777u));
  EXPECT_EQ(1ul << 31, nextPowTwo((1ul << 31) - 1));
  EXPECT_EQ(1ull << 32, nextPowTwo((1ull << 32) - 1));
  EXPECT_EQ(1ull << 63, nextPowTwo((1ull << 62) + 1));
}

TEST(Bits, strictNextPowTwoClz) {
  EXPECT_EQ(1, strictNextPowTwo(0u));
  EXPECT_EQ(2, strictNextPowTwo(1u));
  EXPECT_EQ(4, strictNextPowTwo(2u));
  EXPECT_EQ(4, strictNextPowTwo(3u));
  EXPECT_EQ(8, strictNextPowTwo(4u));
  EXPECT_EQ(8, strictNextPowTwo(5u));
  EXPECT_EQ(8, strictNextPowTwo(6u));
  EXPECT_EQ(8, strictNextPowTwo(7u));
  EXPECT_EQ(16, strictNextPowTwo(8u));
  EXPECT_EQ(16, strictNextPowTwo(9u));
  EXPECT_EQ(16, strictNextPowTwo(13u));
  EXPECT_EQ(32, strictNextPowTwo(16u));
  EXPECT_EQ(512, strictNextPowTwo(510u));
  EXPECT_EQ(512, strictNextPowTwo(511u));
  EXPECT_EQ(1024, strictNextPowTwo(512u));
  EXPECT_EQ(1024, strictNextPowTwo(513u));
  EXPECT_EQ(1024, strictNextPowTwo(777u));
  EXPECT_EQ(1ul << 31, strictNextPowTwo((1ul << 31) - 1));
  EXPECT_EQ(1ull << 32, strictNextPowTwo((1ull << 32) - 1));
  EXPECT_EQ(1ull << 63, strictNextPowTwo((1ull << 62) + 1));
}

TEST(Bits, strictPrevPowTwoClz) {
  EXPECT_EQ(0, strictPrevPowTwo(0u));
  EXPECT_EQ(0, strictPrevPowTwo(1u));
  EXPECT_EQ(1, strictPrevPowTwo(2u));
  EXPECT_EQ(2, strictPrevPowTwo(3u));
  EXPECT_EQ(2, strictPrevPowTwo(4u));
  EXPECT_EQ(4, strictPrevPowTwo(5u));
  EXPECT_EQ(4, strictPrevPowTwo(6u));
  EXPECT_EQ(4, strictPrevPowTwo(7u));
  EXPECT_EQ(4, strictPrevPowTwo(8u));
  EXPECT_EQ(8, strictPrevPowTwo(9u));
  EXPECT_EQ(8, strictPrevPowTwo(13u));
  EXPECT_EQ(8, strictPrevPowTwo(16u));
  EXPECT_EQ(256, strictPrevPowTwo(510u));
  EXPECT_EQ(256, strictPrevPowTwo(511u));
  EXPECT_EQ(256, strictPrevPowTwo(512u));
  EXPECT_EQ(512, strictPrevPowTwo(513u));
  EXPECT_EQ(512, strictPrevPowTwo(777u));
  EXPECT_EQ(1ul << 30, strictPrevPowTwo((1ul << 31) - 1));
  EXPECT_EQ(1ull << 31, strictPrevPowTwo((1ull << 32) - 1));
  EXPECT_EQ(1ull << 62, strictPrevPowTwo((1ull << 62) + 1));
}

TEST(Bits, isPowTwo) {
  EXPECT_FALSE(isPowTwo(0u));
  EXPECT_TRUE(isPowTwo(1ul));
  EXPECT_TRUE(isPowTwo(2ull));
  EXPECT_FALSE(isPowTwo(3ul));
  EXPECT_TRUE(isPowTwo(4ul));
  EXPECT_FALSE(isPowTwo(5ul));
  EXPECT_TRUE(isPowTwo(8ul));
  EXPECT_FALSE(isPowTwo(15u));
  EXPECT_TRUE(isPowTwo(16u));
  EXPECT_FALSE(isPowTwo(17u));
  EXPECT_FALSE(isPowTwo(511ul));
  EXPECT_TRUE(isPowTwo(512ul));
  EXPECT_FALSE(isPowTwo(513ul));
  EXPECT_FALSE(isPowTwo((1ul << 31) - 1));
  EXPECT_TRUE(isPowTwo(1ul << 31));
  EXPECT_FALSE(isPowTwo((1ul << 31) + 1));
  EXPECT_FALSE(isPowTwo((1ull << 63) - 1));
  EXPECT_TRUE(isPowTwo(1ull << 63));
  EXPECT_FALSE(isPowTwo((1ull << 63) + 1));
}

TEST(Bits, popcount) {
  EXPECT_EQ(0, popcount(0U));
  EXPECT_EQ(1, popcount(1U));
  EXPECT_EQ(32, popcount(uint32_t(-1)));
  EXPECT_EQ(64, popcount(uint64_t(-1)));
}

TEST(Bits, EndianSwapUint) {
  EXPECT_EQ(uint8_t(0xda), Endian::swap(uint8_t(0xda)));
  EXPECT_EQ(uint16_t(0x4175), Endian::swap(uint16_t(0x7541)));
  EXPECT_EQ(uint32_t(0x42efb918), Endian::swap(uint32_t(0x18b9ef42)));
  EXPECT_EQ(
      uint64_t(0xa244f5e862c71d8a), Endian::swap(uint64_t(0x8a1dc762e8f544a2)));
}

TEST(Bits, EndianSwapReal) {
  std::mt19937_64 rng;
  auto f = std::uniform_real_distribution<float>()(rng);
  EXPECT_NE(f, Endian::swap(f));
  EXPECT_EQ(f, Endian::swap(Endian::swap(f)));
  auto d = std::uniform_real_distribution<double>()(rng);
  EXPECT_NE(d, Endian::swap(d));
  EXPECT_EQ(d, Endian::swap(Endian::swap(d)));
}

uint64_t reverse_simple(uint64_t v) {
  uint64_t r = 0;

  for (int i = 0; i < 64; i++) {
    r <<= 1;
    r |= v & 1;
    v >>= 1;
  }
  return r;
}

TEST(Bits, BitReverse) {
  EXPECT_EQ(folly::bitReverse<uint8_t>(0), 0);
  EXPECT_EQ(folly::bitReverse<uint8_t>(1), 128);
  for (int i = 0; i < 100; i++) {
    uint64_t v = folly::Random::rand64();
    EXPECT_EQ(folly::bitReverse(v), reverse_simple(v));
    uint32_t b = folly::Random::rand32();
    EXPECT_EQ(folly::bitReverse(b), reverse_simple(b) >> 32);
  }
}

TEST(Bits, BitReverseFallback) {
  EXPECT_EQ(folly::detail::bitReverseFallback<uint8_t>(0), 0);
  EXPECT_EQ(folly::detail::bitReverseFallback<uint8_t>(1), 128);
  for (int i = 0; i < 100; i++) {
    uint64_t v = folly::Random::rand64();
    EXPECT_EQ(folly::detail::bitReverseFallback(v), reverse_simple(v));
    uint32_t b = folly::Random::rand32();
    EXPECT_EQ(folly::detail::bitReverseFallback(b), reverse_simple(b) >> 32);
  }
}

static_assert(std::is_trivial_v<Unaligned<uint64_t>>);
static_assert(std::is_trivially_copy_assignable_v<Unaligned<uint64_t>>);
static_assert(std::is_nothrow_constructible_v<Unaligned<uint64_t>, uint64_t>);
static_assert(std::is_nothrow_assignable_v<Unaligned<uint64_t>, uint64_t>);
static_assert(sizeof(Unaligned<uint64_t>) == sizeof(uint64_t));
static_assert(alignof(Unaligned<uint64_t>) == 1);

TEST(Bits, PartialLoadUnaligned) {
  std::vector<char> buf(128);
  std::generate(buf.begin(), buf.end(), [] {
    return folly::Random::rand32(255);
  });
  for (size_t l = 0; l < 8; ++l) {
    for (size_t pos = 0; pos <= buf.size() - l; ++pos) {
      auto p = buf.data() + pos;
      auto x = folly::partialLoadUnaligned<uint64_t>(p, l);

      uint64_t expected = 0;
      memcpy(&expected, p, l);

      EXPECT_EQ(x, expected);

      if (l < 4) {
        auto x32 = folly::partialLoadUnaligned<uint32_t>(p, l);
        EXPECT_EQ(x32, static_cast<uint32_t>(expected));
      }
    }
  }
}

TEST(Bits, BitCastBasic) {
  auto one = std::make_unique<int>();
  auto two = std::bit_cast<std::uintptr_t>(one.get());
  EXPECT_EQ(std::bit_cast<int*>(two), one.get());

  struct FancyInt {
    FancyInt() {
      ADD_FAILURE() << "Default constructor should not be called by bit_cast";
    }

    int value;
  };

  int x = 5;
  auto bi = std::bit_cast<FancyInt>(x);
  EXPECT_EQ(x, bi.value);
}

TEST(Bits, BitCastCompatibilityTest) {
  static_assert(sizeof(double) == sizeof(std::uint64_t));
  auto one = folly::Random::rand64();
  auto dbl = std::bit_cast<double>(one);
  auto two = std::bit_cast<std::uint64_t>(dbl);
  EXPECT_EQ(one, two);
}

TEST(Bits, LoadUnalignedUB) {
  //  test case from: https://github.com/facebook/folly/issues/2150
  auto func = [](uint8_t a, uint8_t b, uint64_t c, uint64_t d) -> uint32_t {
    char buffer[1 + 1 + 8 + 8];
    folly::storeUnaligned<uint8_t>(buffer + 0, a);
    folly::storeUnaligned<uint8_t>(buffer + 1, b);
    folly::storeUnaligned<uint64_t>(buffer + 2, c);
    folly::storeUnaligned<uint64_t>(buffer + 2 + 8, d);

    uint16_t ret = 0;
    for (std::size_t i = 0; i < sizeof(buffer); i += 2) {
      ret += folly::loadUnaligned<uint16_t>(buffer + i);
    }
    return ret;
  };

  uint8_t a = 0, b = 0;
  uint64_t c = 0, d = 0;
  uint16_t x = func(a, b, c, d);
  EXPECT_EQ(0, x);
}

TEST(Bits, ConstexprLoadUnaligned) {
  auto data = "hello world";
  EXPECT_EQ(
      folly::loadUnaligned<uint8_t>(data + 1),
      folly::constexprLoadUnaligned<uint8_t>(data + 1));
  EXPECT_EQ(
      folly::loadUnaligned<uint16_t>(data + 1),
      folly::constexprLoadUnaligned<uint16_t>(data + 1));
  EXPECT_EQ(
      folly::loadUnaligned<uint32_t>(data + 1),
      folly::constexprLoadUnaligned<uint32_t>(data + 1));
  EXPECT_EQ(
      folly::loadUnaligned<uint64_t>(data + 1),
      folly::constexprLoadUnaligned<uint64_t>(data + 1));
}

TEST(Bits, ConstexprPartialLoadUnaligned) {
  auto data = "hello world";
  EXPECT_EQ(
      folly::partialLoadUnaligned<uint8_t>(data + 1, 0),
      folly::constexprPartialLoadUnaligned<uint8_t>(data + 1, 0));
  EXPECT_EQ(
      folly::partialLoadUnaligned<uint16_t>(data + 1, 1),
      folly::constexprPartialLoadUnaligned<uint16_t>(data + 1, 1));
  EXPECT_EQ(
      folly::partialLoadUnaligned<uint32_t>(data + 1, 3),
      folly::constexprPartialLoadUnaligned<uint32_t>(data + 1, 3));
  EXPECT_EQ(
      folly::partialLoadUnaligned<uint64_t>(data + 1, 5),
      folly::constexprPartialLoadUnaligned<uint64_t>(data + 1, 5));
}

TYPED_TEST(BitsAllUintsTest, NLeastSignificantBits) {
  using T = TypeParam;

  static constexpr std::size_t kBitSize = sizeof(T) * 8;

  static_assert(n_least_significant_bits<T>(0) == 0b0);
  static_assert(n_least_significant_bits<T>(1) == 0b1);
  static_assert(n_least_significant_bits<T>(2) == 0b11);
  static_assert(n_least_significant_bits<T>(3) == 0b111);
  static_assert(n_least_significant_bits<T>(4) == 0b1111);

  constexpr auto cactual = [] {
    std::array<T, kBitSize> ret{};
    for (std::size_t i = 0; i < kBitSize; ++i) {
      ret[i] = n_least_significant_bits<T>(i);
    }
    return ret;
  }();
  auto ractual = [] { // runtime can use a different implementation
    std::array<T, kBitSize> ret{};
    for (std::size_t i = 0; i < kBitSize; ++i) {
      compiler_must_not_predict(i); // runtime
      ret[i] = n_least_significant_bits<T>(i);
    }
    return ret;
  }();
  for (std::size_t i = 0; i < kBitSize; ++i) {
    std::uint64_t expected = (std::uint64_t{1} << i) - 1;
#ifdef __cpp_lib_bitops
    EXPECT_EQ(static_cast<int>(i), std::countr_one(expected));
#endif
    EXPECT_EQ(expected, cactual[i]);
    EXPECT_EQ(expected, ractual[i]);
  }
}

TYPED_TEST(BitsAllUintsTest, NMostSignificantBits) {
  using T = TypeParam;

  static constexpr std::size_t kBitSize = sizeof(T) * 8;

  static_assert(n_most_significant_bits<T>(kBitSize) == static_cast<T>(~0b0));
  static_assert(
      n_most_significant_bits<T>(kBitSize - 1) == static_cast<T>(~0b1));
  static_assert(
      n_most_significant_bits<T>(kBitSize - 2) == static_cast<T>(~0b11));
  static_assert(
      n_most_significant_bits<T>(kBitSize - 3) == static_cast<T>(~0b111));
  static_assert(
      n_most_significant_bits<T>(kBitSize - 4) == static_cast<T>(~0b1111));

  constexpr auto cactual = [] {
    std::array<T, kBitSize> ret{};
    for (std::size_t i = 0; i < kBitSize; ++i) {
      ret[i] = n_most_significant_bits<T>(i);
    }
    return ret;
  }();
  auto ractual = [] { // runtime can use a different implementation
    std::array<T, kBitSize> ret{};
    for (std::size_t i = 0; i < kBitSize; ++i) {
      compiler_must_not_predict(i); // runtime
      ret[i] = n_most_significant_bits<T>(i);
    }
    return ret;
  }();
  for (std::size_t i = 0; i < kBitSize; ++i) {
    T expected = ~n_least_significant_bits<T>(kBitSize - i);
#ifdef __cpp_lib_bitops
    EXPECT_EQ(static_cast<int>(i), std::countl_one(expected));
#endif
    EXPECT_EQ(expected, cactual[i]);
    EXPECT_EQ(expected, ractual[i]);
  }
}

TYPED_TEST(BitsAllUintsTest, ClearNLeastSignificantBits) {
  using T = TypeParam;

  static constexpr std::size_t kBitSize = sizeof(T) * 8;

  static_assert(clear_n_least_significant_bits(T{0b11U}, 1U) == 0b10U);
  static_assert(clear_n_least_significant_bits(T{0b101U}, 1U) == 0b100U);

  constexpr auto cactual = [] {
    std::array<T, kBitSize> ret{};
    for (std::size_t i = 0; i < kBitSize; ++i) {
      ret[i] = clear_n_least_significant_bits(static_cast<T>(-1), i);
    }
    return ret;
  }();
  auto ractual = [] { // runtime can use a different implementation
    std::array<T, kBitSize> ret{};
    for (std::size_t i = 0; i < kBitSize; ++i) {
      compiler_must_not_predict(i); // runtime
      ret[i] = clear_n_least_significant_bits(static_cast<T>(-1), i);
    }
    return ret;
  }();
  for (std::size_t i = 0; i < kBitSize; ++i) {
    T expected = n_most_significant_bits<T>(kBitSize - i);
#ifdef __cpp_lib_bitops
    EXPECT_EQ(static_cast<int>(i), std::countr_zero(expected));
#endif
    EXPECT_EQ(expected, cactual[i]);
    EXPECT_EQ(expected, ractual[i]);
  }
}

TYPED_TEST(BitsAllUintsTest, SetNLeastSignificantBits) {
  using T = TypeParam;

  static constexpr std::size_t kBitSize = sizeof(T) * 8;

  static_assert(set_n_least_significant_bits(T{0b10U}, 1U) == 0b11U);
  static_assert(set_n_least_significant_bits(T{0b100U}, 1U) == 0b101U);
  static_assert(set_n_least_significant_bits(T{0b100U}, 2U) == 0b111U);

  constexpr auto cactual = [] {
    std::array<T, kBitSize> ret{};
    for (std::size_t i = 0; i < kBitSize; ++i) {
      ret[i] = set_n_least_significant_bits(T{}, i);
    }
    return ret;
  }();
  auto ractual = [] { // runtime can use a different implementation
    std::array<T, kBitSize> ret{};
    for (std::size_t i = 0; i < kBitSize; ++i) {
      compiler_must_not_predict(i); // runtime
      ret[i] = set_n_least_significant_bits(T{}, i);
    }
    return ret;
  }();
  for (std::size_t i = 0; i < kBitSize; ++i) {
    T expected = n_least_significant_bits<T>(i);
#ifdef __cpp_lib_bitops
    EXPECT_EQ(static_cast<int>(i), std::countr_one(expected));
#endif
    EXPECT_EQ(expected, cactual[i]);
    EXPECT_EQ(expected, ractual[i]);
  }
}

TYPED_TEST(BitsAllUintsTest, ClearNMostSignificantBits) {
  using T = TypeParam;

  static constexpr std::size_t kBitSize = sizeof(T) * 8;

  static_assert(clear_n_most_significant_bits(T{0b101U}, kBitSize - 1) == 0b1U);
  static_assert(
      clear_n_most_significant_bits(T{0b1100U}, kBitSize - 3) == 0b100U);

  constexpr auto cactual = [] {
    std::array<T, kBitSize> ret{};
    for (std::size_t i = 0; i < kBitSize; ++i) {
      ret[i] = clear_n_most_significant_bits(static_cast<T>(-1), i);
    }
    return ret;
  }();
  auto ractual = [] { // runtime can use a different implementation
    std::array<T, kBitSize> ret{};
    for (std::size_t i = 0; i < kBitSize; ++i) {
      compiler_must_not_predict(i); // runtime
      ret[i] = clear_n_most_significant_bits(static_cast<T>(-1), i);
    }
    return ret;
  }();
  for (std::size_t i = 0; i < kBitSize; ++i) {
    T expected = n_least_significant_bits<T>(kBitSize - i);
#ifdef __cpp_lib_bitops
    EXPECT_EQ(static_cast<int>(i), std::countl_zero(expected));
#endif
    EXPECT_EQ(expected, cactual[i]);
    EXPECT_EQ(expected, ractual[i]);
  }
}

TYPED_TEST(BitsAllUintsTest, SetNMostSignificantBits) {
  using T = TypeParam;

  static constexpr std::size_t kBitSize = sizeof(T) * 8;

  static_assert(
      set_n_most_significant_bits(T{0b1}, kBitSize - 2) ==
      static_cast<T>(~0b10));
  static_assert(
      set_n_most_significant_bits(T{0b1100U}, kBitSize - 3) ==
      static_cast<T>(~0b11));

  constexpr auto cactual = [] {
    std::array<T, kBitSize> ret{};
    for (std::size_t i = 0; i < kBitSize; ++i) {
      ret[i] = set_n_most_significant_bits(T{}, i);
    }
    return ret;
  }();
  auto ractual = [] { // runtime can use a different implementation
    std::array<T, kBitSize> ret{};
    for (std::size_t i = 0; i < kBitSize; ++i) {
      compiler_must_not_predict(i); // runtime
      ret[i] = set_n_most_significant_bits(T{}, i);
    }
    return ret;
  }();
  for (std::size_t i = 0; i < kBitSize; ++i) {
    T expected = n_most_significant_bits<T>(i);
#ifdef __cpp_lib_bitops
    EXPECT_EQ(static_cast<int>(i), std::countl_one(expected));
#endif
    EXPECT_EQ(expected, cactual[i]);
    EXPECT_EQ(expected, ractual[i]);
  }
}

TYPED_TEST(BitsAllUintsTest, GetBitAt) {
  using T = TypeParam;

  static_assert(sizeof(std::size_t) == 8, "");
  static_assert(sizeof(unsigned long) == 8, "");

  constexpr std::size_t kBitSize = sizeof(T) * 8;

  T kOnes = folly::set_n_least_significant_bits(T{}, kBitSize);
  T in[] = {kOnes, 0, kOnes};

  for (std::size_t i = 0; i != kBitSize; ++i) {
    in[1] = T{0};
    in[2] = kOnes;
    T bit = T(T{1} << i);
    in[1] = in[1] | bit;
    in[2] = in[2] ^ bit;
    ASSERT_TRUE(folly::get_bit_at(in, kBitSize + i)) << "i=" << i;
    ASSERT_FALSE(folly::get_bit_at(in, kBitSize * 2 + i)) << "i=" << i;
  }
}

TYPED_TEST(BitsAllUintsTest, GetBitAtLE) {
  using T = TypeParam;

  if constexpr (!folly::kIsLittleEndian) {
    return;
  }

  for (std::size_t i = 0; i != 64; ++i) {
    // stop linter complaining when T == uint64_t
    // @lint-ignore CLANGTIDY bugprone-sizeof-expression
    auto words =
        std::bit_cast<std::array<T, sizeof(std::uint64_t) / sizeof(T)>>(
            ~std::uint64_t(0));
    const auto* ptr = words.data();
    EXPECT_TRUE(folly::get_bit_at(ptr, i));
  }

  for (std::size_t i = 0; i != 64; ++i) {
    // stop linter complaining when T == uint64_t
    // @lint-ignore CLANGTIDY bugprone-sizeof-expression
    auto words =
        std::bit_cast<std::array<T, sizeof(std::uint64_t) / sizeof(T)>>(
            std::uint64_t(0));
    const auto* ptr = words.data();
    EXPECT_FALSE(folly::get_bit_at(ptr, i));
  }

  {
    const std::uint8_t in[] = {0b101, 0b1110, 0, 0, 0, 0, 0, 0};
    std::array<T, sizeof(in) / sizeof(T)> words{};
    std::memcpy(words.data(), in, sizeof(in));
    const auto* ptr = words.data();

    // in[0]
    EXPECT_EQ(1, folly::get_bit_at(ptr, 0));
    EXPECT_EQ(0, folly::get_bit_at(ptr, 1));
    EXPECT_EQ(1, folly::get_bit_at(ptr, 2));
    EXPECT_EQ(0, folly::get_bit_at(ptr, 3));

    // in[1]
    EXPECT_EQ(0, folly::get_bit_at(ptr, 8));
    EXPECT_EQ(1, folly::get_bit_at(ptr, 9));
    EXPECT_EQ(1, folly::get_bit_at(ptr, 10));
    EXPECT_EQ(1, folly::get_bit_at(ptr, 11));
    EXPECT_EQ(0, folly::get_bit_at(ptr, 12));
  }
}

} // namespace folly
