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

#include <folly/lang/CheckedMath.h>

#include <cstdint>
#include <limits>
#include <random>

#include <folly/Traits.h>
#include <folly/lang/Keep.h>
#include <folly/portability/GTest.h>

namespace {

template <typename T, typename T2>
FOLLY_ERASE T check_folly_checked_add_(T const a, T2 const b) {
  T r{};
  if (FOLLY_UNLIKELY(!folly::checked_add(&r, a, b))) {
    auto const ac = a;
    auto const bc = b;
    folly::detail::keep_sink_nx(ac, bc);
  }
  return r;
}

} // namespace

extern "C" FOLLY_KEEP int //
check_folly_checked_add_int(int a, int b) {
  return check_folly_checked_add_(a, b);
}

extern "C" FOLLY_KEEP unsigned int //
check_folly_checked_add_uint(unsigned int a, unsigned int b) {
  return check_folly_checked_add_(a, b);
}

extern "C" FOLLY_KEEP int64_t
check_folly_checked_add_int64(int64_t a, int64_t b) {
  return check_folly_checked_add_(a, b);
}

extern "C" FOLLY_KEEP uint64_t
check_folly_checked_add_uint64(uint64_t a, uint64_t b) {
  return check_folly_checked_add_(a, b);
}

extern "C" FOLLY_KEEP double* //
check_folly_checked_add_double_ptr_size(double* a, size_t b) {
  return check_folly_checked_add_(a, b);
}

TEST(CheckedMath, checkedAddNoOverflow) {
  unsigned int a;

  EXPECT_TRUE(folly::checked_add(&a, 5u, 4u));
  EXPECT_EQ(a, 9);
}

TEST(CheckedMath, checkedAddOverflow) {
  unsigned int a;

  EXPECT_FALSE(
      folly::checked_add(&a, std::numeric_limits<unsigned int>::max(), 4u));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, checkedAddUint64TOverflow) {
  uint64_t a;

  EXPECT_FALSE(
      folly::checked_add<uint64_t>(
          &a, std::numeric_limits<uint64_t>::max() - 7, 9));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, checkedAdd3NoOverflow) {
  uint64_t a;

  EXPECT_TRUE(folly::checked_add<uint64_t>(&a, 5, 7, 9));
  EXPECT_EQ(a, 21);
}

TEST(CheckedMath, checkedAdd3Overflow) {
  uint64_t a;

  EXPECT_FALSE(
      folly::checked_add<uint64_t>(
          &a, 5, std::numeric_limits<uint64_t>::max() - 7, 9));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, checkedAdd3Overflow2) {
  uint64_t a;

  EXPECT_FALSE(
      folly::checked_add<uint64_t>(
          &a, 5, 7, std::numeric_limits<uint64_t>::max() - 7));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, checkedAdd4NoOverflow) {
  uint64_t a;

  EXPECT_TRUE(folly::checked_add<uint64_t>(&a, 5, 7, 9, 11));
  EXPECT_EQ(a, 32);
}

TEST(CheckedMath, checkedAdd4Overflow) {
  uint64_t a;

  EXPECT_FALSE(
      folly::checked_add<uint64_t>(
          &a, 5, std::numeric_limits<uint64_t>::max() - 7, 9, 11));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, checkedAdd4Overflow2) {
  uint64_t a;

  EXPECT_FALSE(
      folly::checked_add<uint64_t>(
          &a, 5, 7, std::numeric_limits<uint64_t>::max() - 7, 11));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, checkedAdd4Overflow3) {
  uint64_t a;

  EXPECT_FALSE(
      folly::checked_add<uint64_t>(
          &a, 5, 7, 9, std::numeric_limits<uint64_t>::max() - 7));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, genericCheckedAddSignedNoOverflow) {
  int a;

  EXPECT_TRUE(folly::detail::generic_checked_add(&a, 5, 4));
  EXPECT_EQ(a, 9);
}

TEST(CheckedMath, genericCheckedAddSignedOverflow) {
  int a;

  EXPECT_FALSE(
      folly::detail::generic_checked_add(
          &a, std::numeric_limits<int>::max(), 4));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, genericCheckedAddSignedUnderflow) {
  int a;

  EXPECT_FALSE(
      folly::detail::generic_checked_add(
          &a, std::numeric_limits<int>::min(), -3));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, genericCheckedAddNoOverflow) {
  unsigned int a;

  EXPECT_TRUE(folly::detail::generic_checked_add(&a, 5u, 4u));
  EXPECT_EQ(a, 9);
}

TEST(CheckedMath, genericCheckedAddOverflow) {
  unsigned int a;

  EXPECT_FALSE(
      folly::detail::generic_checked_add(
          &a, std::numeric_limits<unsigned int>::max(), 4u));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, genericCheckedAddLimit) {
  unsigned int a;

  EXPECT_TRUE(
      folly::detail::generic_checked_add(
          &a, std::numeric_limits<unsigned int>::max() - 7u, 7u));
  EXPECT_EQ(a, std::numeric_limits<unsigned int>::max());
}

TEST(CheckedMath, genericCheckedAddUint64TOverflow) {
  uint64_t a;

  EXPECT_FALSE(
      folly::detail::generic_checked_add<uint64_t>(
          &a, std::numeric_limits<uint64_t>::max() - 7, 9));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, checkedDivSafeDivisor) {
  unsigned int a;

  EXPECT_TRUE(folly::checked_div(&a, 8u, 4u));
  EXPECT_EQ(a, 2);
}

TEST(CheckedMath, checkedDivZeroDivisor) {
  unsigned int a;

  EXPECT_FALSE(folly::checked_div(&a, 8u, 0u));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, checkedModSafeDivisor) {
  unsigned int a;

  EXPECT_TRUE(folly::checked_mod(&a, 5u, 4u));
  EXPECT_EQ(a, 1);
}

TEST(CheckedMath, checkedModZeroDivisor) {
  unsigned int a;

  EXPECT_FALSE(folly::checked_mod(&a, 5u, 0u));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, checkedDivSignedOverflow) {
  int64_t result64 = 1;
  EXPECT_FALSE(
      folly::checked_div(
          &result64, std::numeric_limits<int64_t>::min(), int64_t{-1}));
  EXPECT_EQ(result64, 0);

  int32_t result32 = 1;
  EXPECT_FALSE(
      folly::checked_div(
          &result32, std::numeric_limits<int32_t>::min(), int32_t{-1}));
  EXPECT_EQ(result32, 0);
}

TEST(CheckedMath, checkedModSignedOverflow) {
  int64_t result64 = 1;
  EXPECT_FALSE(
      folly::checked_mod(
          &result64, std::numeric_limits<int64_t>::min(), int64_t{-1}));
  EXPECT_EQ(result64, 0);

  int32_t result32 = 1;
  EXPECT_FALSE(
      folly::checked_mod(
          &result32, std::numeric_limits<int32_t>::min(), int32_t{-1}));
  EXPECT_EQ(result32, 0);
}

TEST(CheckedMath, checkedDivNarrowSignedOverflow) {
  int8_t result8 = 1;
  EXPECT_FALSE(
      folly::checked_div(
          &result8, std::numeric_limits<int8_t>::min(), int8_t{-1}));
  EXPECT_EQ(result8, 0);

  int16_t result16 = 1;
  EXPECT_FALSE(
      folly::checked_div(
          &result16, std::numeric_limits<int16_t>::min(), int16_t{-1}));
  EXPECT_EQ(result16, 0);
}

#if FOLLY_HAVE_INT128_T
TEST(CheckedMath, checkedDivModInt128SignedOverflow) {
  constexpr auto min = folly::int128_t(folly::uint128_t{1} << 127);
  folly::int128_t result = 1;
  EXPECT_FALSE(folly::checked_div(&result, min, folly::int128_t{-1}));
  EXPECT_EQ(result, 0);

  result = 1;
  EXPECT_FALSE(folly::checked_mod(&result, min, folly::int128_t{-1}));
  EXPECT_EQ(result, 0);
}

TEST(CheckedMath, checkedDivModInt128ZeroDividend) {
  folly::int128_t result = 1;
  EXPECT_TRUE(
      folly::checked_div(&result, folly::int128_t{0}, folly::int128_t{-1}));
  EXPECT_EQ(result, 0);

  result = 1;
  EXPECT_TRUE(
      folly::checked_mod(&result, folly::int128_t{0}, folly::int128_t{-1}));
  EXPECT_EQ(result, 0);
}
#endif

TEST(CheckedMath, checkedDivSignedSafeDivisors) {
  int64_t result = 1;
  auto min = std::numeric_limits<int64_t>::min();
  EXPECT_TRUE(folly::checked_div(&result, min, int64_t{1}));
  EXPECT_EQ(result, min);
  EXPECT_TRUE(folly::checked_div(&result, min, int64_t{-2}));
  EXPECT_EQ(result, -(min / 2));
  EXPECT_TRUE(folly::checked_div(&result, int64_t{8}, int64_t{-1}));
  EXPECT_EQ(result, -8);
}

TEST(CheckedMath, checkedModSignedSafeDivisors) {
  int8_t smallResult = 1;
  EXPECT_TRUE(
      folly::checked_mod(
          &smallResult, std::numeric_limits<int8_t>::min(), int8_t{-1}));
  EXPECT_EQ(smallResult, 0);

  int64_t result = 1;
  auto min = std::numeric_limits<int64_t>::min();
  EXPECT_TRUE(folly::checked_mod(&result, min, int64_t{1}));
  EXPECT_EQ(result, 0);
  EXPECT_TRUE(folly::checked_mod(&result, min, int64_t{-2}));
  EXPECT_EQ(result, 0);
  EXPECT_TRUE(folly::checked_mod(&result, int64_t{8}, int64_t{-1}));
  EXPECT_EQ(result, 0);
}

TEST(CheckedMath, checkedMulNoOverflow) {
  unsigned int a;

  EXPECT_TRUE(folly::checked_mul(&a, 5u, 4u));
  EXPECT_EQ(a, 20);
}

TEST(CheckedMath, checkedMulOverflow) {
  unsigned int a;

  EXPECT_FALSE(
      folly::checked_mul(&a, std::numeric_limits<unsigned int>::max(), 4u));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, checkedMulUint64TOverflow) {
  uint64_t a;

  EXPECT_FALSE(
      folly::checked_mul<uint64_t>(
          &a, std::numeric_limits<uint64_t>::max() - 7, 9));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, genericCheckedMulNoOverflow) {
  unsigned int a;

  EXPECT_TRUE(folly::detail::generic_checked_mul(&a, 5u, 4u));
  EXPECT_EQ(a, 20);
}

TEST(CheckedMath, genericCheckedMulOverflow) {
  unsigned int a;

  EXPECT_FALSE(
      folly::detail::generic_checked_mul(
          &a, std::numeric_limits<unsigned int>::max(), 4u));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, genericCheckedMulUint64TOverflow) {
  uint64_t a;

  EXPECT_FALSE(
      folly::detail::generic_checked_mul<uint64_t>(
          &a, std::numeric_limits<uint64_t>::max() - 7, 9));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, genericCheckedMulUint64TOverflow1) {
  uint64_t a;

  // lhs_high != 0 && rhs_high != 0
  EXPECT_FALSE(
      folly::detail::generic_checked_mul<uint64_t>(
          &a, 0x1'0000'0000, 0x1'0000'0000));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, genericCheckedMulUint64TOverflow2) {
  uint64_t a;

  // lhs_low * rhs_high overflows
  EXPECT_FALSE(
      folly::detail::generic_checked_mul<uint64_t>(
          &a, 0xFFFF'FFFF, 0xF'0000'0000));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, genericCheckedMulUint64TOverflow3) {
  uint64_t a;

  // lhs_high * rhs_low overflows
  EXPECT_FALSE(
      folly::detail::generic_checked_mul<uint64_t>(
          &a, 0xF'0000'0000, 0xFFFF'FFFF));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, genericCheckedMulUint64TOverflow4) {
  uint64_t a;

  // mid_bits1 + mid_bits2 overflows
  EXPECT_FALSE(
      folly::detail::generic_checked_mul<uint64_t>(
          &a, 0x2'7FFF'FFFF, 0x2'7FFF'FFFF));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, genericCheckedMulUint64TOverflow5) {
  uint64_t a;

  // (lhs_low * rhs_low) + (mid_bits << 32) overflows
  EXPECT_FALSE(
      folly::detail::generic_checked_mul<uint64_t>(
          &a, 0x2'0000'0203, 0x7FFF'FFFF));
  EXPECT_EQ(a, {});
}

// __builtin_mul_overflow has the most straight-forward interface for doing
// this check, and the generic code will be the same regardless of the compiler.
#if FOLLY_HAS_BUILTIN(__builtin_mul_overflow)
TEST(CheckedMath, genericCheckedMulVsBuiltin) {
  std::mt19937_64 mt{std::random_device()()};

  constexpr size_t IterationCount = 1'000'000;
  size_t overflowCount = 0;
  for (size_t i = 0; i < IterationCount; i++) {
    // There's probably a distribution that produces values on the
    // edge of likely overflow, but I don't know the name, so use
    // the full range for now and just drop bits from a and b to
    // get us down to only ~90% overflow, which is what we care the
    // most about testing.
    // 70 bits of random data across a & b nets 90% overflow
    // 69 bits of data nets 86% overflow
    // 68 bits of data nets 76% overflow
    uint64_t a = mt() >> 16;
    uint64_t b = mt() >> 43;
    uint64_t genRes = 0;
    uint64_t builtinRes = 0;
    bool genOverflow =
        folly::detail::generic_checked_mul<uint64_t>(&genRes, a, b);
    bool builtinOverflow = !__builtin_mul_overflow(a, b, &builtinRes);
    EXPECT_EQ(genOverflow, builtinOverflow);

    if (genOverflow && builtinOverflow) {
      // __builtin doesn't guarantee the value of builtinRes when an overflow
      // happens, but generic_checked_* guarantees it's zero.
      EXPECT_EQ(genRes, builtinRes);
    } else {
      overflowCount++;
    }
  }

  EXPECT_NE(overflowCount, IterationCount);
  EXPECT_NE(overflowCount, 0);
}
#endif

TEST(CheckedMath, checkedMuladdNoOverflow) {
  unsigned int a;

  EXPECT_TRUE(folly::checked_muladd(&a, 5u, 4u, 1u));
  EXPECT_EQ(a, 21);
}

TEST(CheckedMath, checkedMuladdOverflow) {
  unsigned int a;

  EXPECT_FALSE(
      folly::checked_muladd(
          &a, 5u, std::numeric_limits<unsigned int>::max(), 1u));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, checkedMuladdOverflow2) {
  unsigned int a;

  EXPECT_FALSE(
      folly::checked_muladd(
          &a, 5u, 4u, std::numeric_limits<unsigned int>::max()));
  EXPECT_EQ(a, {});
}

TEST(CheckedMath, checkedPtrAddNoOverflow) {
  unsigned int buf[4];
  unsigned int* a;

  EXPECT_TRUE(folly::checked_add(&a, &buf[0], 1u));
  EXPECT_EQ(a, &buf[1]);
}

TEST(CheckedMath, checkedPtrAddOverflow) {
  unsigned int buf[4];
  unsigned int* a{nullptr};

  EXPECT_FALSE(
      folly::checked_add(
          &a, &buf[0], std::numeric_limits<uint64_t>::max() - 7));
  EXPECT_EQ(a, nullptr);
}
