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

#include <folly/ConstexprMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <type_traits>

#include <folly/lang/Bits.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

namespace {
class ConstexprMathTest : public testing::Test {};
} // namespace

TEST_F(ConstexprMathTest, constexpr_iterated_squares_desc_scaling_array) {
  using lim = std::numeric_limits<float>;
  constexpr auto& isq = folly::constexpr_iterated_squares_desc_2_v<float>;

  auto get = [](auto const& arr, auto fun) {
    constexpr auto size = sizeof(arr) / sizeof(arr[0]);
    using res_t = decltype(fun(arr[0]));
    std::array<res_t, size> res{};
    for (size_t i = 0; i < size; ++i) {
      res[i] = fun(arr[i]);
    }
    return res;
  };

  EXPECT_EQ(7, isq.size);
  auto apowers = get(isq.scaling, [](auto _) { return _.power; });
  constexpr size_t epowers[] = {
      64,
      32,
      16,
      8,
      4,
      2,
      1, //
  };
  EXPECT_THAT(apowers, testing::ElementsAreArray(epowers));
  auto ascales = get(isq.scaling, [](auto _) { return _.scale; });
  constexpr float escales[] = {
      0x1p64,
      0x1p32,
      0x1p16,
      0x1p8,
      0x1p4,
      0x1p2,
      0x1p1, //
  };
  EXPECT_THAT(ascales, testing::ElementsAreArray(escales));
  EXPECT_GT(isq.scaling[0].scale, lim::max() / isq.scaling[0].scale);
}

TEST_F(ConstexprMathTest, constexpr_iterated_squares_desc_shrink) {
  using lim = std::numeric_limits<double>;
  constexpr auto& isq = folly::constexpr_iterated_squares_desc_2_v<double>;

  {
    constexpr auto n = 1.;
    constexpr auto a = isq.shrink(n, 1.);
    EXPECT_EQ(0, a.power);
    EXPECT_EQ(1., a.scale);
    EXPECT_LE(n / a.scale, 1.);
    EXPECT_GT(n / a.scale, .5);
  }
  {
    constexpr auto n = 2.;
    constexpr auto a = isq.shrink(n, 1.);
    EXPECT_EQ(1, a.power);
    EXPECT_EQ(2., a.scale);
    EXPECT_LE(n / a.scale, 1.);
    EXPECT_GT(n / a.scale, .5);
  }
  {
    constexpr auto n = 4.;
    constexpr auto a = isq.shrink(n, 1.);
    EXPECT_EQ(2, a.power);
    EXPECT_EQ(4., a.scale);
    EXPECT_LE(n / a.scale, 1.);
    EXPECT_GT(n / a.scale, .5);
  }
  {
    constexpr auto n = 7.;
    constexpr auto a = isq.shrink(n, 1.);
    EXPECT_EQ(3, a.power);
    EXPECT_EQ(8., a.scale);
    EXPECT_LE(n / a.scale, 1.);
    EXPECT_GT(n / a.scale, .5);
  }
  {
    constexpr auto n = 8.;
    constexpr auto a = isq.shrink(n, 1.);
    EXPECT_EQ(3, a.power);
    EXPECT_EQ(8., a.scale);
    EXPECT_LE(n / a.scale, 1.);
    EXPECT_GT(n / a.scale, .5);
  }
  {
    constexpr auto n = 9.;
    constexpr auto a = isq.shrink(n, 1.);
    EXPECT_EQ(4, a.power);
    EXPECT_EQ(16., a.scale);
    EXPECT_LE(n / a.scale, 1.);
    EXPECT_GT(n / a.scale, .5);
  }
  {
    constexpr auto n = 513.;
    constexpr auto a = isq.shrink(n, 1.);
    EXPECT_EQ(10, a.power);
    EXPECT_EQ(1024., a.scale);
    EXPECT_LE(n / a.scale, 1.);
    EXPECT_GT(n / a.scale, .5);
  }
  {
    constexpr auto n = 1023.;
    constexpr auto a = isq.shrink(n, 1.);
    EXPECT_EQ(10, a.power);
    EXPECT_EQ(1024., a.scale);
    EXPECT_LE(n / a.scale, 1.);
    EXPECT_GT(n / a.scale, .5);
  }
  {
    constexpr auto n = lim::max();
    constexpr auto a = isq.shrink(n, 2.);
    EXPECT_EQ(1023, a.power);
    EXPECT_EQ(folly::constexpr_pow(2., 1023), a.scale);
    EXPECT_LE(n / a.scale, 2.);
    EXPECT_GT(n / a.scale, .5);
  }
}

TEST_F(ConstexprMathTest, constexpr_iterated_squares_desc_growth) {
  using lim = std::numeric_limits<double>;
  constexpr auto& isq = folly::constexpr_iterated_squares_desc_2_v<double>;

  {
    constexpr auto n = 1.;
    constexpr auto a = isq.growth(n, 1.);
    EXPECT_EQ(0, a.power);
    EXPECT_EQ(1., a.scale);
    EXPECT_GE(n * a.scale, 1.);
    EXPECT_LT(n * a.scale, 2.);
  }
  {
    constexpr auto n = 1. / 2.;
    constexpr auto a = isq.growth(n, 1.);
    EXPECT_EQ(1, a.power);
    EXPECT_EQ(2., a.scale);
    EXPECT_GE(n * a.scale, 1.);
    EXPECT_LT(n * a.scale, 2.);
  }
  {
    constexpr auto n = 1. / 4.;
    constexpr auto a = isq.growth(n, 1.);
    EXPECT_EQ(2, a.power);
    EXPECT_EQ(4., a.scale);
    EXPECT_GE(n * a.scale, 1.);
    EXPECT_LT(n * a.scale, 2.);
  }
  {
    constexpr auto n = 1. / 7.;
    constexpr auto a = isq.growth(n, 1.);
    EXPECT_EQ(3, a.power);
    EXPECT_EQ(8., a.scale);
    EXPECT_GE(n * a.scale, 1.);
    EXPECT_LT(n * a.scale, 2.);
  }
  {
    constexpr auto n = 1. / 8.;
    constexpr auto a = isq.growth(n, 1.);
    EXPECT_EQ(3, a.power);
    EXPECT_EQ(8., a.scale);
    EXPECT_GE(n * a.scale, 1.);
    EXPECT_LT(n * a.scale, 2.);
  }
  {
    constexpr auto n = 1. / 9.;
    constexpr auto a = isq.growth(n, 1.);
    EXPECT_EQ(4, a.power);
    EXPECT_EQ(16., a.scale);
    EXPECT_GE(n * a.scale, 1.);
    EXPECT_LT(n * a.scale, 2.);
  }
  {
    constexpr auto n = 1. / 513.;
    constexpr auto a = isq.growth(n, 1.);
    EXPECT_EQ(10, a.power);
    EXPECT_EQ(1024., a.scale);
    EXPECT_GE(n * a.scale, 1.);
    EXPECT_LT(n * a.scale, 2.);
  }
  {
    constexpr auto n = 1. / 1023.;
    constexpr auto a = isq.growth(n, 1.);
    EXPECT_EQ(10, a.power);
    EXPECT_EQ(1024., a.scale);
    EXPECT_GE(n * a.scale, 1.);
    EXPECT_LT(n * a.scale, 2.);
  }
  {
    constexpr auto n = lim::min(); // NOLINT
    constexpr auto a = isq.growth(n, .5);
    EXPECT_EQ(1021, a.power);
    EXPECT_EQ(folly::constexpr_pow(2., 1021), a.scale);
    EXPECT_GE(n * a.scale, .5);
    EXPECT_LT(n * a.scale, 1.);
  }
}

TEST_F(ConstexprMathTest, constexpr_min) {
  constexpr auto x = uint16_t(3);
  constexpr auto y = uint16_t(7);
  constexpr auto z = uint16_t(4);
  constexpr auto a = folly::constexpr_min(x, y, z);
  EXPECT_EQ(3, a);
  EXPECT_TRUE((std::is_same<const uint16_t, decltype(a)>::value));
}

TEST_F(ConstexprMathTest, constexpr_max) {
  constexpr auto x = uint16_t(3);
  constexpr auto y = uint16_t(7);
  constexpr auto z = uint16_t(4);
  constexpr auto a = folly::constexpr_max(x, y, z);
  EXPECT_EQ(7, a);
  EXPECT_TRUE((std::is_same<const uint16_t, decltype(a)>::value));
}

TEST_F(ConstexprMathTest, constexpr_clamp) {
  constexpr auto lo = uint16_t(3);
  constexpr auto hi = uint16_t(7);
  constexpr auto x = folly::constexpr_clamp(uint16_t(2), lo, hi);
  constexpr auto y = folly::constexpr_clamp(uint16_t(5), lo, hi);
  constexpr auto z = folly::constexpr_clamp(uint16_t(8), lo, hi);
  EXPECT_EQ(3, x);
  EXPECT_EQ(5, y);
  EXPECT_EQ(7, z);
  EXPECT_TRUE((std::is_same<const uint16_t, decltype(y)>::value));
}

TEST_F(ConstexprMathTest, constexpr_abs_unsigned) {
  constexpr auto v = uint32_t(17);
  constexpr auto a = folly::constexpr_abs(v);
  EXPECT_EQ(17, a);
  EXPECT_TRUE((std::is_same<const uint32_t, decltype(a)>::value));
}

TEST_F(ConstexprMathTest, constexpr_abs_signed_positive) {
  constexpr auto v = int32_t(17);
  constexpr auto a = folly::constexpr_abs(v);
  EXPECT_EQ(17, a);
  EXPECT_TRUE((std::is_same<const uint32_t, decltype(a)>::value));
}

TEST_F(ConstexprMathTest, constexpr_abs_signed_negative) {
  constexpr auto v = int32_t(-17);
  constexpr auto a = folly::constexpr_abs(v);
  EXPECT_EQ(17, a);
  EXPECT_TRUE((std::is_same<const uint32_t, decltype(a)>::value));
}

TEST_F(ConstexprMathTest, constexpr_abs_narrow) {
  constexpr auto u = folly::constexpr_abs(uint8_t(17));
  constexpr auto s = folly::constexpr_abs(int8_t(-17));
  EXPECT_EQ(17, u);
  EXPECT_EQ(17, s);
  EXPECT_TRUE((std::is_same<const uint8_t, decltype(u)>::value));
  EXPECT_TRUE((std::is_same<const uint8_t, decltype(s)>::value));
}

TEST_F(ConstexprMathTest, constexpr_abs_float_positive) {
  constexpr auto v = 17.5f;
  constexpr auto a = folly::constexpr_abs(v);
  EXPECT_EQ(17.5, a);
  EXPECT_TRUE((std::is_same<const float, decltype(a)>::value));
}

TEST_F(ConstexprMathTest, constexpr_abs_float_negative) {
  constexpr auto v = -17.5f;
  constexpr auto a = folly::constexpr_abs(v);
  EXPECT_EQ(17.5, a);
  EXPECT_TRUE((std::is_same<const float, decltype(a)>::value));
}

TEST_F(ConstexprMathTest, constexpr_abs_double_positive) {
  constexpr auto v = 17.5;
  constexpr auto a = folly::constexpr_abs(v);
  EXPECT_EQ(17.5, a);
  EXPECT_TRUE((std::is_same<const double, decltype(a)>::value));
}

TEST_F(ConstexprMathTest, constexpr_abs_double_negative) {
  constexpr auto v = -17.5;
  constexpr auto a = folly::constexpr_abs(v);
  EXPECT_EQ(17.5, a);
  EXPECT_TRUE((std::is_same<const double, decltype(a)>::value));
}

TEST_F(ConstexprMathTest, constexpr_log2_1) {
  constexpr auto v = 1ull;
  constexpr auto a = folly::constexpr_log2(v);
  EXPECT_EQ(0ull, a);
  EXPECT_TRUE((std::is_same<decltype(v), decltype(a)>::value));
}

TEST_F(ConstexprMathTest, constexpr_log2_2) {
  constexpr auto v = 2ull;
  constexpr auto a = folly::constexpr_log2(v);
  EXPECT_EQ(1ull, a);
  EXPECT_TRUE((std::is_same<decltype(v), decltype(a)>::value));
}

TEST_F(ConstexprMathTest, constexpr_log2_64) {
  constexpr auto v = 64ull;
  constexpr auto a = folly::constexpr_log2(v);
  EXPECT_EQ(6ull, a);
  EXPECT_TRUE((std::is_same<decltype(v), decltype(a)>::value));
}

TEST_F(ConstexprMathTest, constexpr_log2_ceil_1) {
  constexpr auto v = 1ull;
  constexpr auto a = folly::constexpr_log2_ceil(v);
  EXPECT_EQ(0ull, a);
  EXPECT_TRUE((std::is_same<decltype(v), decltype(a)>::value));
}

TEST_F(ConstexprMathTest, constexpr_log2_ceil_2) {
  constexpr auto v = 2ull;
  constexpr auto a = folly::constexpr_log2_ceil(v);
  EXPECT_EQ(1ull, a);
  EXPECT_TRUE((std::is_same<decltype(v), decltype(a)>::value));
}

TEST_F(ConstexprMathTest, constexpr_log2_ceil_3) {
  constexpr auto v = 3ull;
  constexpr auto a = folly::constexpr_log2_ceil(v);
  EXPECT_EQ(2ull, a);
  EXPECT_TRUE((std::is_same<decltype(v), decltype(a)>::value));
}

TEST_F(ConstexprMathTest, constexpr_log2_ceil_63) {
  constexpr auto v = 63ull;
  constexpr auto a = folly::constexpr_log2_ceil(v);
  EXPECT_EQ(6ull, a);
  EXPECT_TRUE((std::is_same<decltype(v), decltype(a)>::value));
}

TEST_F(ConstexprMathTest, constexpr_log2_ceil_64) {
  constexpr auto v = 64ull;
  constexpr auto a = folly::constexpr_log2_ceil(v);
  EXPECT_EQ(6ull, a);
  EXPECT_TRUE((std::is_same<decltype(v), decltype(a)>::value));
}

TEST_F(ConstexprMathTest, constexpr_trunc_floating) {
  using lim = std::numeric_limits<double>;

  {
    constexpr auto n = lim::infinity();
    constexpr auto a = folly::constexpr_trunc(n);
    EXPECT_EQ(n, a);
  }
  {
    constexpr auto n = -lim::infinity();
    constexpr auto a = folly::constexpr_trunc(n);
    EXPECT_EQ(n, a);
  }
  {
    constexpr auto n = lim::quiet_NaN();
    constexpr auto a = folly::constexpr_trunc(n);
    EXPECT_TRUE(std::isnan(a));
  }
  {
    constexpr auto n = -lim::quiet_NaN();
    constexpr auto a = folly::constexpr_trunc(n);
    EXPECT_TRUE(std::isnan(a));
  }
  {
    constexpr auto n = lim::max();
    constexpr auto a = folly::constexpr_trunc(n);
    EXPECT_EQ(n, a);
  }
  {
    constexpr auto n = -lim::max();
    constexpr auto a = folly::constexpr_trunc(n);
    EXPECT_EQ(n, a);
  }
  {
    constexpr auto a = folly::constexpr_trunc(0.);
    EXPECT_EQ(0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_trunc(-0.);
    EXPECT_EQ(-0., a);
    EXPECT_TRUE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_trunc(.5);
    EXPECT_EQ(0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_trunc(-.5);
    EXPECT_EQ(0., a);
    EXPECT_TRUE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_trunc(3.5);
    EXPECT_EQ(3.0, a);
  }
  {
    constexpr auto a = folly::constexpr_trunc(-3.5);
    EXPECT_EQ(-3.0, a);
  }
  {
    constexpr auto d = lim::digits - 2;
    constexpr auto n = folly::constexpr_pow(2., d) - .5;
    constexpr auto a = folly::constexpr_trunc(n);
    EXPECT_EQ(.5, n - a);
  }
  {
    constexpr auto d = lim::digits - 2;
    constexpr auto n = folly::constexpr_pow(2., d) + .5;
    constexpr auto a = folly::constexpr_trunc(n);
    EXPECT_EQ(.5, n - a);
  }
  {
    constexpr auto d = lim::digits - 1;
    constexpr auto n = folly::constexpr_pow(2., d) - .5;
    constexpr auto a = folly::constexpr_trunc(n);
    EXPECT_EQ(.5, n - a);
  }
  {
    constexpr auto d = lim::digits - 1;
    constexpr auto n = folly::constexpr_pow(2., d) + .5;
    constexpr auto a = folly::constexpr_trunc(n);
    EXPECT_EQ(.0, n - a);
  }
  {
    constexpr auto d = lim::digits;
    constexpr auto n = folly::constexpr_pow(2., d) - .5;
    constexpr auto a = folly::constexpr_trunc(n);
    EXPECT_EQ(.0, n - a);
  }
  {
    constexpr auto d = lim::digits;
    constexpr auto n = folly::constexpr_pow(2., d) + .5;
    constexpr auto a = folly::constexpr_trunc(n);
    EXPECT_EQ(.0, n - a);
  }
  {
    constexpr auto d = lim::digits + 1;
    constexpr auto n = folly::constexpr_pow(2., d) - .5;
    constexpr auto a = folly::constexpr_trunc(n);
    EXPECT_EQ(.0, n - a);
  }
  {
    constexpr auto d = lim::digits + 1;
    constexpr auto n = folly::constexpr_pow(2., d) + .5;
    constexpr auto a = folly::constexpr_trunc(n);
    EXPECT_EQ(.0, n - a);
  }
}

TEST_F(ConstexprMathTest, constexpr_trunc_integral) {
  {
    constexpr auto n = 0u;
    constexpr auto a = folly::constexpr_trunc(n);
    EXPECT_EQ(0u, a);
  }
  {
    constexpr auto n = -1;
    constexpr auto a = folly::constexpr_trunc(n);
    EXPECT_EQ(-1, a);
  }
  {
    constexpr auto n = 100;
    constexpr auto a = folly::constexpr_trunc(n);
    EXPECT_EQ(100, a);
  }
}

TEST_F(ConstexprMathTest, constexpr_round_floating) {
  using lim = std::numeric_limits<double>;

  {
    constexpr auto n = lim::infinity();
    constexpr auto a = folly::constexpr_round(n);
    EXPECT_EQ(n, a);
  }
  {
    constexpr auto n = -lim::infinity();
    constexpr auto a = folly::constexpr_round(n);
    EXPECT_EQ(n, a);
  }
  {
    constexpr auto n = lim::quiet_NaN();
    constexpr auto a = folly::constexpr_round(n);
    EXPECT_TRUE(std::isnan(a));
  }
  {
    constexpr auto n = -lim::quiet_NaN();
    constexpr auto a = folly::constexpr_round(n);
    EXPECT_TRUE(std::isnan(a));
  }
  {
    constexpr auto n = lim::max();
    constexpr auto a = folly::constexpr_round(n);
    EXPECT_EQ(n, a);
  }
  {
    constexpr auto n = -lim::max();
    constexpr auto a = folly::constexpr_round(n);
    EXPECT_EQ(n, a);
  }
  {
    constexpr auto a = folly::constexpr_round(0.);
    EXPECT_EQ(0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_round(-0.);
    EXPECT_EQ(-0., a);
    EXPECT_TRUE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_round(0.3);
    EXPECT_EQ(0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_round(0.5);
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_round(0.7);
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_round(-0.3);
    EXPECT_EQ(-0., a);
    EXPECT_TRUE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_round(-0.5);
    EXPECT_EQ(-1., a);
  }
  {
    constexpr auto a = folly::constexpr_round(-0.7);
    EXPECT_EQ(-1., a);
  }
  {
    constexpr auto a = folly::constexpr_round(7.3);
    EXPECT_EQ(7., a);
  }
  {
    constexpr auto a = folly::constexpr_round(7.5);
    EXPECT_EQ(8., a);
  }
  {
    constexpr auto a = folly::constexpr_round(7.7);
    EXPECT_EQ(8., a);
  }
  {
    constexpr auto a = folly::constexpr_round(-7.3);
    EXPECT_EQ(-7., a);
  }
  {
    constexpr auto a = folly::constexpr_round(-7.5);
    EXPECT_EQ(-8., a);
  }
  {
    constexpr auto a = folly::constexpr_round(-7.7);
    EXPECT_EQ(-8., a);
  }
}

TEST_F(ConstexprMathTest, constexpr_floor) {
  // floating-point one-argument integer
  {
    constexpr auto a = folly::constexpr_floor(size_t(0));
    EXPECT_EQ(size_t(0), a);
  }
  {
    constexpr auto a = folly::constexpr_floor(~size_t(0));
    EXPECT_EQ(~size_t(0), a);
  }
  // floating-point one-argument floating-point
  {
    constexpr auto a = folly::constexpr_floor(0.);
    EXPECT_EQ(0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_floor(-0.);
    EXPECT_EQ(-0., a);
    EXPECT_TRUE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_floor(0.5);
    EXPECT_EQ(0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_floor(-0.5);
    EXPECT_EQ(-1., a);
  }
  {
    constexpr auto a = folly::constexpr_floor(1.);
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_floor(-1.);
    EXPECT_EQ(-1., a);
  }
  {
    constexpr auto n = std::numeric_limits<double>::max();
    constexpr auto a = folly::constexpr_floor(n);
    EXPECT_EQ(std::numeric_limits<double>::max(), a);
    EXPECT_EQ(std::floor(n), a);
  }
  {
    constexpr auto n = std::numeric_limits<double>::infinity();
    constexpr auto a = folly::constexpr_floor(n);
    EXPECT_EQ(std::numeric_limits<double>::infinity(), a);
    EXPECT_EQ(std::floor(n), a);
  }
  {
    constexpr auto n = std::numeric_limits<double>::quiet_NaN();
    constexpr auto a = folly::constexpr_floor(n);
    EXPECT_TRUE(std::isnan(a));
    EXPECT_TRUE(std::isnan(std::floor(n)));
  }
  for (size_t i = 0; i < std::numeric_limits<double>::max_exponent; ++i) {
    auto n = folly::constexpr_pow(2., i);
    auto a = folly::constexpr_floor(n);
    EXPECT_EQ(n, a);
    EXPECT_EQ(std::floor(n), a);
  }
  for (size_t i = 0; i < std::numeric_limits<double>::max_exponent; ++i) {
    auto n = folly::constexpr_pow(2., i) + 1.;
    auto a = folly::constexpr_floor(n);
    EXPECT_EQ(n, a);
    EXPECT_EQ(std::floor(n), a);
  }
  for (size_t i = 0; i < std::numeric_limits<double>::max_exponent; ++i) {
    auto n = folly::constexpr_pow(2., i);
    auto a = folly::constexpr_floor(n + .5);
    EXPECT_EQ(n, a);
    EXPECT_EQ(std::floor(n), a);
  }
  for (size_t i = 0; i < std::numeric_limits<double>::max_exponent; ++i) {
    auto n = folly::constexpr_pow(2., i);
    auto a = folly::constexpr_floor(n - .5);
    auto e = i < std::numeric_limits<double>::digits ? n - 1. : n;
    EXPECT_EQ(e, a);
    EXPECT_EQ(std::floor(n - .5), e);
    EXPECT_EQ(std::floor(n - .5), a);
  }
}

TEST_F(ConstexprMathTest, constexpr_ceil_integral) {
  {
    constexpr auto a = folly::constexpr_ceil(size_t(0));
    EXPECT_EQ(size_t(0), a);
  }
  {
    constexpr auto a = folly::constexpr_ceil(~size_t(0));
    EXPECT_EQ(~size_t(0), a);
  }
}

TEST_F(ConstexprMathTest, constexpr_ceil_floating) {
  using lim = std::numeric_limits<double>;

  {
    constexpr auto a = folly::constexpr_ceil(0.);
    EXPECT_EQ(0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_ceil(-0.);
    EXPECT_EQ(-0., a);
    EXPECT_TRUE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_ceil(0.5);
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_ceil(-0.5);
    EXPECT_EQ(-0., a);
    EXPECT_TRUE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_ceil(1.);
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_ceil(-1.);
    EXPECT_EQ(-1., a);
  }
  {
    constexpr auto n = lim::max();
    constexpr auto a = folly::constexpr_ceil(n);
    EXPECT_EQ(n, a);
    EXPECT_EQ(std::ceil(n), a);
  }
  {
    constexpr auto n = lim::infinity();
    constexpr auto a = folly::constexpr_ceil(n);
    EXPECT_EQ(n, a);
    EXPECT_EQ(std::ceil(n), a);
  }
  {
    constexpr auto n = lim::quiet_NaN();
    constexpr auto a = folly::constexpr_ceil(n);
    EXPECT_TRUE(std::isnan(a));
    EXPECT_TRUE(std::isnan(std::ceil(n)));
  }
  for (size_t i = 0; i < lim::max_exponent; ++i) {
    auto n = folly::constexpr_pow(2., i);
    auto a = folly::constexpr_ceil(n);
    EXPECT_EQ(n, a);
    EXPECT_EQ(std::ceil(n), a);
  }
  for (size_t i = 0; i < lim::max_exponent; ++i) {
    auto n = folly::constexpr_pow(2., i) + 1.;
    auto a = folly::constexpr_ceil(n);
    EXPECT_EQ(n, a);
    EXPECT_EQ(std::ceil(n), a);
  }
  for (size_t i = 0; i < lim::max_exponent; ++i) {
    auto n = folly::constexpr_pow(2., i);
    auto a = folly::constexpr_ceil(n - .5);
    EXPECT_EQ(n, a);
    EXPECT_EQ(std::ceil(n), a);
  }
  for (size_t i = 0; i < lim::max_exponent; ++i) {
    auto n = folly::constexpr_pow(2., i);
    auto a = folly::constexpr_ceil(n + .5);
    auto e = i < lim::digits - 1 ? n + 1. : n;
    EXPECT_EQ(e, a) << i;
    EXPECT_EQ(std::ceil(n + .5), e);
    EXPECT_EQ(std::ceil(n + .5), a);
  }
}

TEST_F(ConstexprMathTest, constexpr_ceil_integral_round) {
  {
    constexpr auto roundable = 20ull;
    constexpr auto round = 6ull;
    constexpr auto rounded = folly::constexpr_ceil(roundable, round);
    EXPECT_EQ(24ull, rounded);
  }
  {
    constexpr auto roundable = -20ll;
    constexpr auto round = 6ll;
    constexpr auto rounded = folly::constexpr_ceil(roundable, round);
    EXPECT_EQ(-18ll, rounded);
  }
  {
    constexpr auto roundable = -20ll;
    constexpr auto round = 0ll;
    constexpr auto rounded = folly::constexpr_ceil(roundable, round);
    EXPECT_EQ(-20ll, rounded);
  }
  {
    constexpr auto roundable = 0ll;
    constexpr auto round = std::numeric_limits<long long>::min();
    constexpr auto rounded = folly::constexpr_ceil(roundable, round);
    EXPECT_EQ(0ll, rounded);
  }
}

TEST_F(ConstexprMathTest, constexpr_mult_floating) {
  using lim = std::numeric_limits<double>;

  {
    constexpr auto a = folly::constexpr_mult(lim::quiet_NaN(), 1.);
    EXPECT_TRUE(std::isnan(a));
  }
  {
    constexpr auto a = folly::constexpr_mult(1., lim::quiet_NaN());
    EXPECT_TRUE(std::isnan(a));
  }
  {
    constexpr auto a = folly::constexpr_mult(0., lim::infinity());
    EXPECT_TRUE(std::isnan(a));
  }
  {
    constexpr auto a = folly::constexpr_mult(0., -lim::infinity());
    EXPECT_TRUE(std::isnan(a));
  }
  {
    constexpr auto a = folly::constexpr_mult(lim::infinity(), 0.);
    EXPECT_TRUE(std::isnan(a));
  }
  {
    constexpr auto a = folly::constexpr_mult(-lim::infinity(), 0.);
    EXPECT_TRUE(std::isnan(a));
  }
  {
    constexpr auto a = folly::constexpr_mult(1., lim::infinity());
    EXPECT_EQ(lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_mult(1., -lim::infinity());
    EXPECT_EQ(-lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_mult(-1., lim::infinity());
    EXPECT_EQ(-lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_mult(-1., -lim::infinity());
    EXPECT_EQ(lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_mult(lim::max(), lim::max());
    EXPECT_EQ(lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_mult(-lim::max(), lim::max());
    EXPECT_EQ(-lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_mult(lim::max(), -lim::max());
    EXPECT_EQ(-lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_mult(-lim::max(), -lim::max());
    EXPECT_EQ(lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_mult(lim::min(), 1.); // NOLINT
    EXPECT_EQ(lim::min(), a); // NOLINT
  }
  {
    constexpr auto a = folly::constexpr_mult(-lim::min(), 1.); // NOLINT
    EXPECT_EQ(-lim::min(), a); // NOLINT
  }
  {
    constexpr auto a = folly::constexpr_mult(lim::min(), -1.); // NOLINT
    EXPECT_EQ(-lim::min(), a); // NOLINT
  }
  {
    constexpr auto a = folly::constexpr_mult(-lim::min(), -1.); // NOLINT
    EXPECT_EQ(lim::min(), a); // NOLINT
  }
  {
    constexpr auto a = folly::constexpr_mult(1., lim::min()); // NOLINT
    EXPECT_EQ(lim::min(), a); // NOLINT
  }
  {
    constexpr auto a = folly::constexpr_mult(-1., lim::min()); // NOLINT
    EXPECT_EQ(-lim::min(), a); // NOLINT
  }
  {
    constexpr auto a = folly::constexpr_mult(1., -lim::min()); // NOLINT
    EXPECT_EQ(-lim::min(), a); // NOLINT
  }
  {
    constexpr auto a = folly::constexpr_mult(-1., -lim::min()); // NOLINT
    EXPECT_EQ(lim::min(), a); // NOLINT
  }
  {
    constexpr auto a = folly::constexpr_mult(lim::denorm_min(), .125);
    EXPECT_EQ(0., a);
  }
  {
    constexpr auto a = folly::constexpr_mult(.125, lim::denorm_min());
    EXPECT_EQ(0., a);
  }
  {
    constexpr auto n = lim::denorm_min();
    constexpr auto a = folly::constexpr_mult(n, n);
    EXPECT_EQ(0., a);
  }
}

TEST_F(ConstexprMathTest, constexpr_exp_integral) {
  using lim = std::numeric_limits<double>;

  {
    constexpr auto n = folly::constexpr_exp(0u); // unsigned
    EXPECT_DOUBLE_EQ(1., n);
  }
  {
    constexpr auto n = folly::constexpr_exp(0); // signed
    EXPECT_DOUBLE_EQ(1., n);
  }
  {
    constexpr auto n = folly::constexpr_exp(1u); // unsigned
    EXPECT_DOUBLE_EQ(std::numbers::e, n);
  }
  {
    constexpr auto n = folly::constexpr_exp(1); // signed
    EXPECT_DOUBLE_EQ(std::numbers::e, n);
  }
  {
    constexpr auto n = folly::constexpr_exp(13u); // unsigned
    EXPECT_DOUBLE_EQ(folly::constexpr_pow(std::numbers::e, 13u), n);
  }
  {
    constexpr auto n = folly::constexpr_exp(13); // signed positive
    EXPECT_DOUBLE_EQ(folly::constexpr_pow(std::numbers::e, 13u), n);
  }
  {
    constexpr auto n = folly::constexpr_exp(-13); // signed negative
    EXPECT_DOUBLE_EQ(1. / folly::constexpr_pow(std::numbers::e, 13u), n);
  }
  {
    constexpr auto n = folly::constexpr_exp(4097u); // unsigned
    EXPECT_EQ(lim::infinity(), n);
  }
  {
    constexpr auto n = folly::constexpr_exp(4097); // signed positive
    EXPECT_EQ(lim::infinity(), n);
  }
  {
    constexpr auto n = folly::constexpr_exp(-4097); // signed negative
    EXPECT_EQ(0., n);
  }
}

TEST_F(ConstexprMathTest, constexpr_exp_floating) {
  using lim = std::numeric_limits<double>;

  {
    constexpr auto n = lim::quiet_NaN();
    constexpr auto a = folly::constexpr_exp(n);
    EXPECT_NE(a, a);
    EXPECT_NE(std::exp(n), std::exp(n));
  }
  {
    constexpr auto n = -lim::infinity();
    constexpr auto a = folly::constexpr_exp(n);
    EXPECT_EQ(std::exp(n), a);
    EXPECT_EQ(+0., a);
  }
  {
    constexpr auto n = +lim::infinity();
    constexpr auto a = folly::constexpr_exp(n);
    EXPECT_EQ(std::exp(n), a);
    EXPECT_EQ(n, a);
  }
  {
    constexpr auto n = lim::lowest();
    constexpr auto a = folly::constexpr_exp(n);
    EXPECT_EQ(std::exp(n), a);
    EXPECT_EQ(+0., a);
  }
  {
    constexpr auto n = lim::max();
    constexpr auto a = folly::constexpr_exp(n);
    EXPECT_EQ(std::exp(n), a);
    EXPECT_EQ(std::numeric_limits<double>::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_exp(+0.);
    EXPECT_EQ(std::exp(+0.), a);
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_exp(-0.);
    EXPECT_EQ(std::exp(-0.), a);
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto n = +lim::min(); // NOLINT
    constexpr auto a = folly::constexpr_exp(n);
    EXPECT_EQ(std::exp(n), a);
    EXPECT_EQ(+1., a);
  }
  {
    constexpr auto n = -lim::min(); // NOLINT
    constexpr auto a = folly::constexpr_exp(n);
    EXPECT_EQ(std::exp(n), a);
    EXPECT_EQ(+1., a);
  }
  {
    constexpr auto a = folly::constexpr_exp(1.);
    EXPECT_DOUBLE_EQ(std::exp(1.), a);
    EXPECT_DOUBLE_EQ(std::numbers::e, a);
  }
  {
    constexpr auto a = folly::constexpr_exp(3.3);
    EXPECT_DOUBLE_EQ(std::exp(3.3), a);
  }
  {
    constexpr auto a = folly::constexpr_exp(471.L);
#if defined(__APPLE__) && defined(FOLLY_AARCH64)
    EXPECT_LT( // too inexact for expect-double-eq
        std::exp(471.L) / a - 1,
        16 * lim::epsilon());
#else
    EXPECT_DOUBLE_EQ(std::exp(471.L), a);
#endif
  }
  {
    constexpr auto a = folly::constexpr_exp(600.);
    EXPECT_NE(lim::infinity(), a);
    EXPECT_LT( // too inexact for expect-double-eq
        std::exp(600.) / a - 1,
        16 * lim::epsilon());
  }
  {
    constexpr auto a = folly::constexpr_exp(709.8);
    EXPECT_EQ(lim::infinity(), a);
    EXPECT_EQ(std::exp(709.8), a);
  }
  {
    constexpr auto a = folly::constexpr_exp(-4.1);
    EXPECT_DOUBLE_EQ(std::exp(-4.1), a);
  }
  {
    constexpr auto a = folly::constexpr_exp(-77.1);
    EXPECT_DOUBLE_EQ(std::exp(-77.1), a);
  }
}

TEST_F(ConstexprMathTest, constexpr_log) {
  using lim = std::numeric_limits<double>;

  {
    constexpr auto a = folly::constexpr_log(1.);
    EXPECT_DOUBLE_EQ(0., a);
  }
  {
    constexpr auto a = folly::constexpr_log(std::numbers::e);
    EXPECT_DOUBLE_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_log(0.);
    EXPECT_DOUBLE_EQ(-lim::infinity(), a);
  }
  {
    constexpr auto n = 15539.1;
    constexpr auto a = folly::constexpr_log(n);
    EXPECT_DOUBLE_EQ(std::log(n), a);
  }
  {
    constexpr auto n = 15539.1;
    constexpr auto a = folly::constexpr_log(n * n * n);
    EXPECT_DOUBLE_EQ(std::log(n * n * n), a);
  }
  {
    constexpr auto n = .00033112;
    constexpr auto a = folly::constexpr_log(n);
    EXPECT_DOUBLE_EQ(std::log(n), a);
  }
  {
    constexpr auto n = lim::epsilon();
    constexpr auto a = folly::constexpr_log(n);
    EXPECT_DOUBLE_EQ(std::log(n), a);
  }
  {
    constexpr auto n = lim::min(); // NOLINT
    constexpr auto a = folly::constexpr_log(n);
    EXPECT_DOUBLE_EQ(std::log(n), a);
  }
  {
    constexpr auto n = lim::min(); // NOLINT
    constexpr auto a = folly::constexpr_log(n);
    EXPECT_DOUBLE_EQ(std::log(n), a);
  }
  {
    constexpr auto n = lim::max();
    constexpr auto a = folly::constexpr_log(n);
    EXPECT_DOUBLE_EQ(std::log(n), a);
  }
  {
    constexpr auto n = lim::max();
    static_assert(n < lim::infinity());
    constexpr auto a = folly::constexpr_log(n);
    EXPECT_DOUBLE_EQ(std::log(n), a);
  }
}

TEST_F(ConstexprMathTest, constexpr_pow_integral_base_integral_exp) {
  {
    constexpr auto a = folly::constexpr_pow(uint64_t(0), 15);
    EXPECT_EQ(0, a);
  }
  {
    constexpr auto a = folly::constexpr_pow(uint64_t(15), 0);
    EXPECT_EQ(1, a);
  }
  {
    constexpr auto a = folly::constexpr_pow(uint64_t(2), 6);
    EXPECT_EQ(64, a);
  }
}

TEST_F(ConstexprMathTest, constexpr_pow_floating_base_integral_exp) {
  using lim = std::numeric_limits<double>;

  {
    constexpr auto a = folly::constexpr_pow(2., 0u);
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(2., 1u);
    EXPECT_EQ(2., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(2., 7u);
    EXPECT_EQ(128., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(2., 12345u);
    EXPECT_EQ(lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_pow(+0., 7u);
    EXPECT_EQ(+0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(-0., 7u);
    EXPECT_EQ(-0., a);
    EXPECT_TRUE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(-0., 8u);
    EXPECT_EQ(+0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(lim::infinity(), 7u);
    EXPECT_EQ(lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_pow(-lim::infinity(), 7u);
    EXPECT_EQ(-lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_pow(lim::infinity(), 8u);
    EXPECT_EQ(lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_pow(-lim::infinity(), 8u);
    EXPECT_EQ(lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_pow(lim::quiet_NaN(), 0u);
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(-lim::quiet_NaN(), 0u);
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(lim::quiet_NaN(), 8u);
    EXPECT_TRUE(std::isnan(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(-lim::quiet_NaN(), 8u);
    EXPECT_TRUE(std::isnan(a));
  }
}

TEST_F(ConstexprMathTest, constexpr_pow_floating_base_floating_exp) {
  using lim = std::numeric_limits<double>;

  {
    constexpr auto a = folly::constexpr_pow(1., 1.);
    EXPECT_DOUBLE_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(1., 7.);
    EXPECT_DOUBLE_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(0., 7.);
    EXPECT_DOUBLE_EQ(0., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(7., 0.);
    EXPECT_DOUBLE_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(7., 1.);
    EXPECT_DOUBLE_EQ(7., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(24.331, 6.922);
    EXPECT_DOUBLE_EQ(std::pow(24.331, 6.922), a);
  }
  {
    constexpr auto a = folly::constexpr_pow(24.331, -6.922);
    EXPECT_DOUBLE_EQ(std::pow(24.331, -6.922), a);
  }
  {
    constexpr auto a = folly::constexpr_pow(0., 0.);
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(1., 0.);
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(-1., 0.);
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(-1., 1.);
    EXPECT_EQ(-1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(-1., 2.);
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(lim::infinity(), 0.);
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(-lim::infinity(), 0.);
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(lim::quiet_NaN(), 0.);
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(lim::infinity(), -1.);
    EXPECT_EQ(0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(lim::infinity(), -9.);
    EXPECT_EQ(0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(lim::infinity(), -1.5);
    EXPECT_EQ(0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(lim::infinity(), -9.5);
    EXPECT_EQ(0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(lim::infinity(), -lim::infinity());
    EXPECT_EQ(0., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(lim::infinity(), 1.);
    EXPECT_EQ(lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_pow(lim::infinity(), 9.);
    EXPECT_EQ(lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_pow(lim::infinity(), 1.5);
    EXPECT_EQ(lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_pow(lim::infinity(), lim::infinity());
    EXPECT_EQ(lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_pow(lim::infinity(), lim::quiet_NaN());
    EXPECT_TRUE(std::isnan(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(-lim::infinity(), -.5);
    EXPECT_EQ(0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(-lim::infinity(), -3.);
    EXPECT_EQ(0., a);
    EXPECT_TRUE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(-lim::infinity(), -6.);
    EXPECT_EQ(0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(-lim::infinity(), -lim::infinity());
    EXPECT_EQ(0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(-lim::infinity(), .5);
    EXPECT_EQ(lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_pow(-lim::infinity(), 3.);
    EXPECT_EQ(-lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_pow(-lim::infinity(), 6.);
    EXPECT_EQ(lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_pow(-lim::infinity(), lim::infinity());
    EXPECT_EQ(lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_pow(-lim::infinity(), lim::quiet_NaN());
    EXPECT_TRUE(std::isnan(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(2., -lim::infinity());
    EXPECT_EQ(0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(2., lim::infinity());
    EXPECT_EQ(lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_pow(1., lim::quiet_NaN());
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(1., -lim::infinity());
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(1., lim::lowest());
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(1., lim::min()); // NOLINT
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(1., lim::max());
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(1., lim::infinity());
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(1., lim::quiet_NaN());
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(0., lim::infinity());
    EXPECT_EQ(0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(0., 1.);
    EXPECT_EQ(0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(-0., .5);
    EXPECT_EQ(0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(-0., 1.);
    EXPECT_EQ(0., a);
    EXPECT_TRUE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(-0., 2.);
    EXPECT_EQ(0., a);
    EXPECT_FALSE(std::signbit(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(0., -lim::infinity());
    EXPECT_EQ(lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_pow(-1., -lim::infinity());
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(-1., lim::infinity());
    EXPECT_EQ(1., a);
  }
  {
    constexpr auto a = folly::constexpr_pow(-1., lim::quiet_NaN());
    EXPECT_TRUE(std::isnan(a));
  }
  {
    constexpr auto a = folly::constexpr_pow(-2., lim::infinity());
    EXPECT_EQ(lim::infinity(), a);
  }
  {
    constexpr auto a = folly::constexpr_pow(-2., -lim::infinity());
    EXPECT_EQ(0., a);
    EXPECT_FALSE(std::signbit(a));
  }
}

TEST_F(ConstexprMathTest, constexpr_find_last_set_examples) {
  {
    constexpr auto a = folly::constexpr_find_last_set(int64_t(0));
    EXPECT_EQ(0, a);
  }
  {
    constexpr auto a = folly::constexpr_find_last_set(int64_t(2));
    EXPECT_EQ(2, a);
  }
  {
    constexpr auto a = folly::constexpr_find_last_set(int64_t(4096 + 64));
    EXPECT_EQ(13, a);
  }
}

TEST_F(ConstexprMathTest, constexpr_find_last_set_all_64_adjacents) {
  using type = uint64_t;
  constexpr auto const bits = std::numeric_limits<type>::digits;
  EXPECT_EQ(0, folly::constexpr_find_last_set(type(0)));
  for (size_t i = 0; i < bits; ++i) {
    type const v = type(1) << i;
    EXPECT_EQ(i + 1, folly::constexpr_find_last_set(v));
    EXPECT_EQ(i, folly::constexpr_find_last_set((v - 1)));
  }
}

TEST_F(ConstexprMathTest, constexpr_find_last_set_all_8_reference) {
  using type = char;
  for (size_t i = 0; i < 256u; ++i) {
    auto const expected = folly::findLastSet(type(i));
    EXPECT_EQ(expected, folly::constexpr_find_last_set(type(i)));
  }
}

TEST_F(ConstexprMathTest, constexpr_find_first_set_examples) {
  {
    constexpr auto a = folly::constexpr_find_first_set(int64_t(0));
    EXPECT_EQ(0, a);
  }
  {
    constexpr auto a = folly::constexpr_find_first_set(int64_t(2));
    EXPECT_EQ(2, a);
  }
  {
    constexpr auto a = folly::constexpr_find_first_set(int64_t(4096 + 64));
    EXPECT_EQ(7, a);
  }
}

TEST_F(ConstexprMathTest, constexpr_find_first_set_all_64_adjacent) {
  using type = uint64_t;
  constexpr auto const bits = std::numeric_limits<type>::digits;
  EXPECT_EQ(0, folly::constexpr_find_first_set(type(0)));
  for (size_t i = 0; i < bits; ++i) {
    type const v = (type(1) << (bits - 1)) | (type(1) << i);
    EXPECT_EQ(i + 1, folly::constexpr_find_first_set(v));
  }
}

TEST_F(ConstexprMathTest, constexpr_find_first_set_all_8_reference) {
  using type = char;
  for (size_t i = 0; i < 256u; ++i) {
    auto const expected = folly::findFirstSet(type(i));
    EXPECT_EQ(expected, folly::constexpr_find_first_set(type(i)));
  }
}

constexpr auto kInt64Max = std::numeric_limits<int64_t>::max();
constexpr auto kInt64Min = std::numeric_limits<int64_t>::min();
constexpr auto kUInt64Max = std::numeric_limits<uint64_t>::max();
constexpr auto kInt8Max = std::numeric_limits<int8_t>::max();
constexpr auto kInt8Min = std::numeric_limits<int8_t>::min();
constexpr auto kUInt8Max = std::numeric_limits<uint8_t>::max();

TEST_F(ConstexprMathTest, constexpr_add_overflow_clamped) {
  for (int a = kInt8Min; a <= kInt8Max; a++) {
    for (int b = kInt8Min; b <= kInt8Max; b++) {
      int c = std::clamp(a + b, int(kInt8Min), int(kInt8Max));
      int8_t a1 = a;
      int8_t b1 = b;
      int8_t c1 = folly::constexpr_add_overflow_clamped(a1, b1);
      ASSERT_LE(c1, kInt8Max);
      ASSERT_GE(c1, kInt8Min);
      ASSERT_EQ(c1, c);
    }
  }

  for (int a = 0; a <= kUInt8Max; a++) {
    for (int b = 0; b <= kUInt8Max; b++) {
      int c = std::clamp(a + b, 0, int(kUInt8Max));
      uint8_t a1 = a;
      uint8_t b1 = b;
      uint8_t c1 = folly::constexpr_add_overflow_clamped(a1, b1);
      ASSERT_LE(c1, kUInt8Max);
      ASSERT_GE(c1, 0);
      ASSERT_EQ(c1, c);
    }
  }

  constexpr auto v1 =
      folly::constexpr_add_overflow_clamped(int64_t(23), kInt64Max - 12);
  EXPECT_EQ(kInt64Max, v1);

  constexpr auto v2 =
      folly::constexpr_add_overflow_clamped(int64_t(23), int64_t(12));
  EXPECT_EQ(int64_t(35), v2);

  constexpr auto v3 =
      folly::constexpr_add_overflow_clamped(int64_t(-23), int64_t(12));
  EXPECT_EQ(int64_t(-11), v3);

  constexpr auto v4 =
      folly::constexpr_add_overflow_clamped(int64_t(-23), int64_t(-12));
  EXPECT_EQ(int64_t(-35), v4);

  constexpr auto v5 =
      folly::constexpr_add_overflow_clamped(uint64_t(23), kUInt64Max - 12);
  EXPECT_EQ(kUInt64Max, v5);
}

TEST_F(ConstexprMathTest, constexpr_sub_overflow_clamped) {
  for (int a = kInt8Min; a <= kInt8Max; a++) {
    for (int b = kInt8Min; b <= kInt8Max; b++) {
      int c = std::clamp(a - b, int(kInt8Min), int(kInt8Max));
      int8_t a1 = a;
      int8_t b1 = b;
      int8_t c1 = folly::constexpr_sub_overflow_clamped(a1, b1);
      ASSERT_LE(c1, kInt8Max);
      ASSERT_GE(c1, kInt8Min);
      ASSERT_EQ(c1, c);
    }
  }

  for (int a = 0; a <= kUInt8Max; a++) {
    for (int b = 0; b <= kUInt8Max; b++) {
      int c = std::clamp(a - b, 0, int(kUInt8Max));
      uint8_t a1 = a;
      uint8_t b1 = b;
      uint8_t c1 = folly::constexpr_sub_overflow_clamped(a1, b1);
      ASSERT_LE(c1, kUInt8Max);
      ASSERT_GE(c1, 0);
      ASSERT_EQ(c1, c);
    }
  }

  constexpr auto v1 =
      folly::constexpr_sub_overflow_clamped(int64_t(23), int64_t(12));
  EXPECT_EQ(int64_t(11), v1);

  constexpr auto v2 =
      folly::constexpr_sub_overflow_clamped(int64_t(-23), int64_t(-12));
  EXPECT_EQ(int64_t(-11), v2);

  constexpr auto v3 =
      folly::constexpr_sub_overflow_clamped(int64_t(23), int64_t(-12));
  EXPECT_EQ(int64_t(35), v3);

  constexpr auto v4 =
      folly::constexpr_sub_overflow_clamped(int64_t(23), kInt64Min);
  EXPECT_EQ(kInt64Max, v4);

  constexpr auto v5 =
      folly::constexpr_sub_overflow_clamped(int64_t(-23), kInt64Min);
  EXPECT_EQ(kInt64Max - 22, v5);

  constexpr auto v6 =
      folly::constexpr_sub_overflow_clamped(uint64_t(23), uint64_t(12));
  EXPECT_EQ(uint64_t(11), v6);

  constexpr auto v7 =
      folly::constexpr_sub_overflow_clamped(uint64_t(12), uint64_t(23));
  EXPECT_EQ(uint64_t(0), v7);
}

template <class F, class... Args>
void for_each_argument(F&& f, Args&&... args) {
  [](...) {}((f(std::forward<Args>(args)), 0)...);
}

// float -> integral clamp cast
template <typename Src, typename Dst>
static typename std::enable_if<std::is_floating_point<Src>::value, void>::type
run_constexpr_clamp_cast_test(Src, Dst) {
  constexpr auto kQuietNaN = std::numeric_limits<Src>::quiet_NaN();
  constexpr auto kSignalingNaN = std::numeric_limits<Src>::signaling_NaN();
  constexpr auto kPositiveInf = std::numeric_limits<Src>::infinity();
  constexpr auto kNegativeInf = -kPositiveInf;

  constexpr auto kDstMax = std::numeric_limits<Dst>::max();
  constexpr auto kDstMin = std::numeric_limits<Dst>::min();

  // Check f != f trick for identifying NaN.
  EXPECT_TRUE(kQuietNaN != kQuietNaN);
  EXPECT_TRUE(kSignalingNaN != kSignalingNaN);
  EXPECT_FALSE(kPositiveInf != kPositiveInf);
  EXPECT_FALSE(kNegativeInf != kNegativeInf);

  // NaN -> 0
  EXPECT_EQ(0, folly::constexpr_clamp_cast<Dst>(kQuietNaN));
  EXPECT_EQ(0, folly::constexpr_clamp_cast<Dst>(kSignalingNaN));

  // Inf -> Max/Min
  EXPECT_EQ(kDstMax, folly::constexpr_clamp_cast<Dst>(kPositiveInf));
  EXPECT_EQ(kDstMin, folly::constexpr_clamp_cast<Dst>(kNegativeInf));

  // barely out of range -> Max/Min
  EXPECT_EQ(kDstMax, folly::constexpr_clamp_cast<Dst>(Src(kDstMax) * 1.001));
  EXPECT_EQ(kDstMin, folly::constexpr_clamp_cast<Dst>(Src(kDstMin) * 1.001));

  // value in range -> same value
  EXPECT_EQ(Dst(0), folly::constexpr_clamp_cast<Dst>(Src(0)));
  EXPECT_EQ(Dst(123), folly::constexpr_clamp_cast<Dst>(Src(123)));
}

// integral -> integral clamp cast
template <typename Src, typename Dst>
static typename std::enable_if<std::is_integral<Src>::value, void>::type
run_constexpr_clamp_cast_test(Src, Dst) {
  constexpr auto kSrcMax = std::numeric_limits<Src>::max();
  constexpr auto kSrcMin = std::numeric_limits<Src>::min();

  constexpr auto kDstMax = std::numeric_limits<Dst>::max();
  constexpr auto kDstMin = std::numeric_limits<Dst>::min();

  // value in range -> same value
  EXPECT_EQ(Dst(0), folly::constexpr_clamp_cast<Dst>(Src(0)));
  EXPECT_EQ(Dst(123), folly::constexpr_clamp_cast<Dst>(Src(123)));

  // int -> uint
  if (std::is_signed<Src>::value && std::is_unsigned<Dst>::value) {
    EXPECT_EQ(Dst(0), folly::constexpr_clamp_cast<Dst>(Src(-123)));
  }

  if (sizeof(Src) > sizeof(Dst)) {
    // range clamping
    EXPECT_EQ(kDstMax, folly::constexpr_clamp_cast<Dst>(kSrcMax));

    // int -> int
    if (std::is_signed<Src>::value && std::is_signed<Dst>::value) {
      EXPECT_EQ(kDstMin, folly::constexpr_clamp_cast<Dst>(kSrcMin));
    }

  } else if (
      std::is_unsigned<Src>::value && std::is_signed<Dst>::value &&
      sizeof(Src) == sizeof(Dst)) {
    // uint -> int, same size
    EXPECT_EQ(kDstMax, folly::constexpr_clamp_cast<Dst>(kSrcMax));
  }
}

TEST_F(ConstexprMathTest, constexpr_clamp_cast) {
  for_each_argument(
      [](auto dst) {
        for_each_argument(
            [&](auto src) { run_constexpr_clamp_cast_test(src, dst); },
            // source types
            float(),
            double(),
            (long double)(0),
            int8_t(),
            uint8_t(),
            int16_t(),
            uint16_t(),
            int32_t(),
            uint32_t(),
            int64_t(),
            uint64_t());
      },
      // dst types
      int8_t(),
      uint8_t(),
      int16_t(),
      uint16_t(),
      int32_t(),
      uint32_t(),
      int64_t(),
      uint64_t());
}
