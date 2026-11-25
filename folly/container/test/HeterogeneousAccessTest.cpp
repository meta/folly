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

#include <folly/container/HeterogeneousAccess.h>

#include <set>
#include <string_view>
#include <vector>

#include <folly/FBString.h>
#include <folly/Portability.h>
#include <folly/Range.h>
#include <folly/Traits.h>
#include <folly/portability/GTest.h>
#include <folly/small_vector.h>

using namespace folly;

namespace {

template <typename T>
void checkTransparent() {
  static_assert(is_transparent_v<HeterogeneousAccessEqualTo<T>>);
  static_assert(is_transparent_v<HeterogeneousAccessHash<T>>);
}

template <typename T>
void checkNotTransparent() {
  static_assert(!is_transparent_v<HeterogeneousAccessEqualTo<T>>);
  static_assert(!is_transparent_v<HeterogeneousAccessHash<T>>);
}

struct StringVector {
  std::vector<std::string> data_;

  [[maybe_unused]] /* implicit */ operator Range<std::string const*>() const {
    return {&data_[0], data_.size()};
  }
};

} // namespace

namespace std {
template <>
struct hash<StringVector> {
  [[maybe_unused]] std::size_t operator()(StringVector const& value) const {
    return folly::hash::hash_range(value.data_.begin(), value.data_.end());
  }
};
} // namespace std

TEST(HeterogeneousAccess, transparentIsSelected) {
  checkTransparent<std::string>();
  checkTransparent<std::wstring>();
  checkTransparent<std::u16string>();
  checkTransparent<std::u32string>();

  checkTransparent<std::string_view>();
  checkTransparent<std::wstring_view>();
  checkTransparent<std::u16string_view>();
  checkTransparent<std::u32string_view>();

  checkTransparent<fbstring>();

  checkTransparent<StringPiece>();
  checkTransparent<MutableStringPiece>();

  checkTransparent<Range<char const*>>();
  checkTransparent<Range<wchar_t const*>>();
  checkTransparent<Range<char16_t const*>>();
  checkTransparent<Range<char32_t const*>>();
  checkTransparent<Range<int const*>>();

  checkTransparent<Range<char*>>();
  checkTransparent<Range<wchar_t*>>();
  checkTransparent<Range<char16_t*>>();
  checkTransparent<Range<char32_t*>>();
  checkTransparent<Range<int*>>();

  checkTransparent<std::vector<char>>();
  checkTransparent<std::vector<wchar_t>>();
  checkTransparent<std::vector<char16_t>>();
  checkTransparent<std::vector<char32_t>>();
  checkTransparent<std::vector<int>>();

  checkTransparent<std::array<char const, 2>>();
  checkTransparent<std::array<wchar_t const, 2>>();
  checkTransparent<std::array<char16_t const, 2>>();
  checkTransparent<std::array<char32_t const, 2>>();
  checkTransparent<std::array<int const, 2>>();

  checkTransparent<std::array<char, 2>>();
  checkTransparent<std::array<wchar_t, 2>>();
  checkTransparent<std::array<char16_t, 2>>();
  checkTransparent<std::array<char32_t, 2>>();
  checkTransparent<std::array<int, 2>>();
}

TEST(HeterogeneousAccess, transparentIsNotSelected) {
  checkNotTransparent<char>();
  checkNotTransparent<int>();
  checkNotTransparent<float>();
  checkNotTransparent<std::pair<StringPiece, StringPiece>>();
  checkNotTransparent<StringVector>(); // no folly::hasher for Range
}

template <typename L, typename R, typename S>
void runTestMatches2(S src) {
  S smaller{src};
  smaller.resize(smaller.size() - 1);

  using RangeType = Range<typename S::value_type*>;

  L lhs1{RangeType{&src[0], src.size()}};
  L lhs2{RangeType{&smaller[0], smaller.size()}};
  R rhs1{RangeType{&src[0], src.size()}};
  R rhs2{RangeType{&smaller[0], smaller.size()}};

  HeterogeneousAccessEqualTo<L> equalTo;
  HeterogeneousAccessHash<L> hash;

  EXPECT_TRUE(equalTo(lhs1, rhs1));
  EXPECT_FALSE(equalTo(lhs1, rhs2));
  EXPECT_FALSE(equalTo(lhs2, rhs1));
  EXPECT_TRUE(equalTo(lhs2, rhs2));

  EXPECT_EQ(hash(lhs1), hash(rhs1));
  EXPECT_NE(hash(lhs1), hash(rhs2)); // technically only low probability
  EXPECT_NE(hash(lhs2), hash(rhs1)); // technically only low probability
  EXPECT_EQ(hash(lhs2), hash(rhs2));

  auto v0 = smaller[0];
  std::array<decltype(v0), 1> a{{v0}};
  EXPECT_FALSE(equalTo(a, lhs1));
  EXPECT_FALSE(equalTo(a, rhs1));

  smaller.resize(1);
  EXPECT_FALSE(equalTo(a, lhs1));
  EXPECT_FALSE(equalTo(a, lhs2));
  EXPECT_TRUE(equalTo(a, smaller));

  EXPECT_EQ(hash(a), hash(smaller));
}

template <typename S>
void runTestMatches(S const& src) {
  using SP = Range<typename S::value_type const*>;
  using MSP = Range<typename S::value_type*>;
  using SV = std::basic_string_view<typename S::value_type>;
  using V = std::vector<typename S::value_type>;

  runTestMatches2<S, S>(src);
  runTestMatches2<S, SP>(src);
  runTestMatches2<S, MSP>(src);
  runTestMatches2<S, SV>(src);
  runTestMatches2<S, V>(src);
  runTestMatches2<SP, S>(src);
  runTestMatches2<SP, SP>(src);
  runTestMatches2<SP, MSP>(src);
  runTestMatches2<SP, SV>(src);
  runTestMatches2<SP, V>(src);
  runTestMatches2<MSP, S>(src);
  runTestMatches2<MSP, SP>(src);
  runTestMatches2<MSP, MSP>(src);
  runTestMatches2<MSP, SV>(src);
  runTestMatches2<MSP, V>(src);
  runTestMatches2<SV, S>(src);
  runTestMatches2<SV, SP>(src);
  runTestMatches2<SV, MSP>(src);
  runTestMatches2<SV, SV>(src);
  runTestMatches2<SV, V>(src);
  runTestMatches2<V, S>(src);
  runTestMatches2<V, SP>(src);
  runTestMatches2<V, MSP>(src);
  runTestMatches2<V, SV>(src);
  runTestMatches2<V, V>(src);
}

Range<int const*> foo(small_vector<int, 2> const& sv) {
  return sv;
}

TEST(HeterogeneousAccess, transparentMatches) {
  runTestMatches<std::string>("abcd");
#if !defined(__cpp_lib_char8_t) || __cpp_lib_char8_t < 201907
  runTestMatches<std::string>(u8"abcd");
#else
  runTestMatches<std::u8string>(u8"abcd");
#endif
  runTestMatches<std::wstring>(L"abcd");
  runTestMatches<std::u16string>(u"abcd");
  runTestMatches<std::u32string>(U"abcd");
  runTestMatches<fbstring>("abcd");
  runTestMatches<std::vector<int>>({1, 2, 3, 4});

  static_assert(
      std::is_convertible<small_vector<int, 2>, Range<int const*>>::value);
  runTestMatches<small_vector<int, 2>>({1, 2, 3, 4});
}

TEST(HeterogeneousAccess, stringEqualityAtEveryLengthAndPosition) {
  HeterogeneousAccessEqualTo<std::string> equal;
  for (std::size_t n = 0; n <= 80; ++n) {
    std::string const a(n, 'x');
    EXPECT_TRUE(equal(a, std::string(a)));
    EXPECT_TRUE(equal(a, std::string_view(a)));
    EXPECT_TRUE(equal(StringPiece(a), a));
    EXPECT_FALSE(equal(a, std::string(n + 1, 'x')));
    for (std::size_t i = 0; i < n; ++i) {
      std::string b = a;
      b[i] = 'y';
      EXPECT_FALSE(equal(a, b)) << "length " << n << ", position " << i;
      EXPECT_FALSE(equal(std::string_view(b), a))
          << "length " << n << ", position " << i;
    }
  }
}
