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

#include <folly/lang/cstring_view.h>

#include <compare>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include <folly/FBString.h>
#include <folly/Range.h>
#include <folly/portability/GTest.h>

using folly::cstring_view;
using folly::fbstring;
using folly::StringPiece;
using folly::tag;
using folly::tag_t;
using std::string;
using std::string_view;

class CStringViewTest : public testing::Test {};

TEST_F(CStringViewTest, DefaultConstructor) {
  cstring_view sv;
  EXPECT_TRUE(sv.empty());
  EXPECT_EQ(sv.size(), 0);
  EXPECT_EQ(sv.data(), nullptr);
}

TEST_F(CStringViewTest, ConstructFromNullPointer) {
  cstring_view sv(nullptr, 0);
  EXPECT_TRUE(sv.empty());
  EXPECT_EQ(sv.size(), 0);
  EXPECT_EQ(sv.data(), nullptr);
  EXPECT_EQ(sv.c_str(), nullptr);
}

TEST_F(CStringViewTest, ConstructFromCString) {
  const char* cstr = "hello";
  cstring_view sv(cstr);
  EXPECT_FALSE(sv.empty());
  EXPECT_EQ(sv.size(), std::strlen(cstr));
  EXPECT_EQ(sv.data(), cstr);
  EXPECT_STREQ(sv.c_str(), "hello");
  EXPECT_EQ(sv.c_str(), cstr);
}

TEST_F(CStringViewTest, ConstructFromCStringWithSize) {
  const char* cstr = "hello world";
  cstring_view sv(cstr, std::strlen(cstr));
  EXPECT_EQ(sv.size(), std::strlen(cstr));
  EXPECT_EQ(sv.data(), cstr);
  EXPECT_STREQ(sv.c_str(), "hello world");
}

TEST_F(CStringViewTest, ConstructFromStdString) {
  std::string str = "hello";
  cstring_view sv(str);
  EXPECT_EQ(sv.size(), str.size());
  EXPECT_EQ(sv.data(), str.data());
  EXPECT_STREQ(sv.c_str(), "hello");
}

TEST_F(CStringViewTest, CopyConstructor) {
  cstring_view sv1("hello");
  cstring_view sv2(sv1);
  EXPECT_EQ(sv1.size(), sv2.size());
  EXPECT_EQ(sv1.data(), sv2.data());
  EXPECT_STREQ(sv2.c_str(), "hello");
}

TEST_F(CStringViewTest, CopyAssignment) {
  cstring_view sv1("hello");
  cstring_view sv2;
  sv2 = sv1;
  EXPECT_EQ(sv1.size(), sv2.size());
  EXPECT_EQ(sv1.data(), sv2.data());
  EXPECT_STREQ(sv2.c_str(), "hello");
}

TEST_F(CStringViewTest, AssignFromCString) {
  cstring_view sv("initial");
  const char* cstr = "assigned";
  sv = cstr;
  EXPECT_EQ(sv.size(), std::strlen(cstr));
  EXPECT_EQ(sv.data(), cstr);
  EXPECT_STREQ(sv.c_str(), "assigned");
}

TEST_F(CStringViewTest, AssignFromStdString) {
  cstring_view sv("initial");
  std::string str = "assigned";
  sv = str;
  EXPECT_EQ(sv.size(), str.size());
  EXPECT_EQ(sv.data(), str.data());
  EXPECT_STREQ(sv.c_str(), "assigned");
}

TEST_F(CStringViewTest, CStrMethod) {
  const char* cstr = "test";
  cstring_view sv(cstr);
  EXPECT_EQ(sv.c_str(), cstr);
  EXPECT_STREQ(sv.c_str(), "test");
  EXPECT_EQ(sv.c_str()[sv.size()], '\0');
}

TEST_F(CStringViewTest, ImplicitConversionToStringView) {
  cstring_view svz("hello");
  std::string_view sv = svz;
  EXPECT_EQ(sv.size(), svz.size());
  EXPECT_STREQ(sv.data(), "hello");
}

TEST_F(CStringViewTest, ImplicitConversionInFunctionCall) {
  auto take_string_view = [](std::string_view sv) { return sv.size(); };

  cstring_view svz("hello world");
  size_t result = take_string_view(svz);
  EXPECT_EQ(result, svz.size());
}

TEST_F(CStringViewTest, ImplicitConversionWithComparison) {
  cstring_view svz("hello");
  std::string_view converted = svz;
  EXPECT_EQ(converted, std::string_view("hello"));
  EXPECT_NE(converted, std::string_view("world"));
}

TEST_F(CStringViewTest, ImplicitConversionPreservesData) {
  const char* cstr = "test string";
  cstring_view svz(cstr);
  std::string_view sv = svz;

  EXPECT_STREQ(sv.data(), cstr);
  EXPECT_EQ(sv.size(), std::strlen(cstr));
}

TEST_F(CStringViewTest, ImplicitConversionOfEmptyString) {
  cstring_view svz("");
  std::string_view sv = svz;

  EXPECT_TRUE(sv.empty());
  EXPECT_EQ(sv.size(), 0u);
}

TEST_F(CStringViewTest, ImplicitConversionAfterRemovePrefix) {
  cstring_view svz("hello world");
  svz.remove_prefix(6);

  std::string_view sv = svz;
  EXPECT_STREQ(sv.data(), "world");
  EXPECT_EQ(sv.size(), 5u);
}

TEST_F(CStringViewTest, ImplicitConversionOfSubstr) {
  cstring_view svz("hello world");
  auto sub = svz.substr(6);

  std::string_view sv = sub;
  EXPECT_STREQ(sv.data(), "world");
  EXPECT_EQ(sv.size(), 5u);
}

TEST_F(CStringViewTest, ImplicitConversionInReturn) {
  auto get_string_view = []() -> std::string_view {
    cstring_view svz("returned");
    return svz;
  };

  std::string_view sv = get_string_view();
  EXPECT_STREQ(sv.data(), "returned");
}

TEST_F(CStringViewTest, ImplicitConversionWithStdStringComparison) {
  cstring_view svz("hello");
  std::string str("hello");

  std::string_view sv = svz;
  EXPECT_EQ(sv, std::string_view(str));
}

TEST_F(CStringViewTest, ImplicitConversionMultipleTimes) {
  cstring_view svz("test");

  std::string_view sv1 = svz;
  std::string_view sv2 = svz;

  EXPECT_STREQ(sv1.data(), sv2.data());
  EXPECT_STREQ(sv1.data(), svz.c_str());
  EXPECT_STREQ(sv2.data(), svz.c_str());
}

TEST_F(CStringViewTest, SubstrOneParameter) {
  cstring_view sv("hello world");
  auto sub = sv.substr(6);
  EXPECT_EQ(sub.size(), 5);
  EXPECT_STREQ(sub.c_str(), "world");
}

TEST_F(CStringViewTest, SubstrZeroParameter) {
  cstring_view sv("hello");
  auto sub = sv.substr();
  EXPECT_EQ(sub.size(), 5);
  EXPECT_STREQ(sub.c_str(), "hello");
}

TEST_F(CStringViewTest, SubstrFromMiddle) {
  cstring_view sv("hello world");
  auto sub = sv.substr(3);
  EXPECT_EQ(sub.size(), 8);
  EXPECT_STREQ(sub.c_str(), "lo world");
}

TEST_F(CStringViewTest, RemovePrefix) {
  cstring_view sv("hello world");
  sv.remove_prefix(6);
  EXPECT_EQ(sv.size(), 5);
  EXPECT_STREQ(sv.c_str(), "world");
}

TEST_F(CStringViewTest, RemovePrefixMultiple) {
  cstring_view sv("hello world");
  sv.remove_prefix(3);
  EXPECT_STREQ(sv.c_str(), "lo world");
  sv.remove_prefix(3);
  EXPECT_STREQ(sv.c_str(), "world");
}

TEST_F(CStringViewTest, BasicMethods) {
  cstring_view sv("hello");
  EXPECT_EQ(sv.size(), 5);
  EXPECT_EQ(sv.length(), 5);
  EXPECT_FALSE(sv.empty());
  EXPECT_EQ(sv.front(), 'h');
  EXPECT_EQ(sv.back(), 'o');
  EXPECT_EQ(sv[0], 'h');
  EXPECT_EQ(sv.at(1), 'e');
}

TEST_F(CStringViewTest, Iterators) {
  cstring_view sv("hello");
  std::string result;
  for (char c : sv) {
    result += c;
  }
  EXPECT_EQ(result, "hello");
}

TEST_F(CStringViewTest, ReverseIterators) {
  cstring_view sv("hello");
  std::string result;
  for (auto it = sv.rbegin(); it != sv.rend(); ++it) {
    result += *it;
  }
  EXPECT_EQ(result, "olleh");
}

TEST_F(CStringViewTest, Find) {
  cstring_view sv("hello world");
  EXPECT_EQ(sv.find("world"), 6);
  EXPECT_EQ(sv.find("xyz"), std::string_view::npos);
  EXPECT_EQ(sv.find('o'), 4);
}

TEST_F(CStringViewTest, Compare) {
  cstring_view sv1("abc");
  cstring_view sv2("abc");
  cstring_view sv3("def");
  EXPECT_EQ(sv1.compare(sv2.c_str()), 0);
  EXPECT_LT(sv1.compare(sv3.c_str()), 0);
  EXPECT_GT(sv3.compare(sv1.c_str()), 0);
}

TEST_F(CStringViewTest, UserDefinedLiteral) {
  using namespace folly::literals::string_literals;
  auto sv = "hello"_csv;
  EXPECT_EQ(sv.size(), 5);
  EXPECT_STREQ(sv.c_str(), "hello");
}

TEST_F(CStringViewTest, EmptyString) {
  cstring_view sv("");
  EXPECT_TRUE(sv.empty());
  EXPECT_EQ(sv.size(), 0);
  EXPECT_STREQ(sv.c_str(), "");
}

TEST_F(CStringViewTest, ConstCorrectness) {
  const cstring_view sv("hello");
  EXPECT_EQ(sv.size(), 5);
  EXPECT_STREQ(sv.c_str(), "hello");
  auto sub = sv.substr(2);
  EXPECT_STREQ(sub.c_str(), "llo");
}

TEST_F(CStringViewTest, SwapBasic) {
  const char* cstr1 = "hello";
  const char* cstr2 = "world";
  cstring_view sv1(cstr1);
  cstring_view sv2(cstr2);

  EXPECT_STREQ(sv1.c_str(), "hello");
  EXPECT_STREQ(sv2.c_str(), "world");

  sv1.swap(sv2);

  EXPECT_STREQ(sv1.c_str(), "world");
  EXPECT_STREQ(sv2.c_str(), "hello");
  EXPECT_EQ(sv1.data(), cstr2);
  EXPECT_EQ(sv2.data(), cstr1);
}

TEST_F(CStringViewTest, SwapDifferentSizes) {
  cstring_view sv1("short");
  cstring_view sv2("much longer string");

  EXPECT_EQ(sv1.size(), 5);
  EXPECT_EQ(sv2.size(), 18);

  sv1.swap(sv2);

  EXPECT_EQ(sv1.size(), 18);
  EXPECT_EQ(sv2.size(), 5);
  EXPECT_STREQ(sv1.c_str(), "much longer string");
  EXPECT_STREQ(sv2.c_str(), "short");
}

TEST_F(CStringViewTest, SwapWithEmpty) {
  cstring_view sv1("hello");
  cstring_view sv2("");

  EXPECT_FALSE(sv1.empty());
  EXPECT_TRUE(sv2.empty());

  sv1.swap(sv2);

  EXPECT_TRUE(sv1.empty());
  EXPECT_FALSE(sv2.empty());
  EXPECT_STREQ(sv1.c_str(), "");
  EXPECT_STREQ(sv2.c_str(), "hello");
}

TEST_F(CStringViewTest, SwapWithDefault) {
  cstring_view sv1("hello");
  cstring_view sv2;

  EXPECT_FALSE(sv1.empty());
  EXPECT_TRUE(sv2.empty());
  EXPECT_EQ(sv2.data(), nullptr);

  sv1.swap(sv2);

  EXPECT_TRUE(sv1.empty());
  EXPECT_FALSE(sv2.empty());
  EXPECT_STREQ(sv2.c_str(), "hello");
}

TEST_F(CStringViewTest, SwapSelf) {
  cstring_view sv("hello");
  sv.swap(sv);
  EXPECT_STREQ(sv.c_str(), "hello");
  EXPECT_EQ(sv.size(), 5);
}

TEST_F(CStringViewTest, SwapAfterRemovePrefix) {
  cstring_view sv1("hello world");
  cstring_view sv2("foo bar");

  sv1.remove_prefix(6);
  sv2.remove_prefix(4);

  EXPECT_STREQ(sv1.c_str(), "world");
  EXPECT_STREQ(sv2.c_str(), "bar");

  sv1.swap(sv2);

  EXPECT_STREQ(sv1.c_str(), "bar");
  EXPECT_STREQ(sv2.c_str(), "world");
}

TEST_F(CStringViewTest, SwapAfterSubstr) {
  cstring_view sv1("hello world");
  cstring_view sv2("foo bar");

  auto sub1 = sv1.substr(6);
  auto sub2 = sv2.substr(4);

  EXPECT_STREQ(sub1.c_str(), "world");
  EXPECT_STREQ(sub2.c_str(), "bar");

  sub1.swap(sub2);

  EXPECT_STREQ(sub1.c_str(), "bar");
  EXPECT_STREQ(sub2.c_str(), "world");
}

TEST_F(CStringViewTest, SwapPreservesNullTermination) {
  const char* cstr1 = "hello";
  const char* cstr2 = "world";
  cstring_view sv1(cstr1);
  cstring_view sv2(cstr2);

  sv1.swap(sv2);

  // Verify null-termination is preserved
  EXPECT_EQ(sv1.c_str()[sv1.size()], '\0');
  EXPECT_EQ(sv2.c_str()[sv2.size()], '\0');
}

TEST_F(CStringViewTest, StartsWithStringView) {
  cstring_view sv("hello world");
  EXPECT_TRUE(sv.starts_with(std::string_view("hello")));
  EXPECT_TRUE(sv.starts_with(std::string_view("hello world")));
  EXPECT_FALSE(sv.starts_with(std::string_view("world")));
  EXPECT_TRUE(sv.starts_with(std::string_view("")));
}

TEST_F(CStringViewTest, StartsWithChar) {
  cstring_view sv("hello");
  EXPECT_TRUE(sv.starts_with('h'));
  EXPECT_FALSE(sv.starts_with('e'));
  EXPECT_FALSE(sv.starts_with('x'));
}

TEST_F(CStringViewTest, StartsWithCString) {
  cstring_view sv("hello world");
  EXPECT_TRUE(sv.starts_with("hello"));
  EXPECT_TRUE(sv.starts_with("hello world"));
  EXPECT_FALSE(sv.starts_with("world"));
  EXPECT_TRUE(sv.starts_with(""));
}

TEST_F(CStringViewTest, StartsWithEmptyString) {
  cstring_view sv("");
  EXPECT_TRUE(sv.starts_with(""));
  EXPECT_FALSE(sv.starts_with("a"));
}

TEST_F(CStringViewTest, StartsWithAfterRemovePrefix) {
  cstring_view sv("hello world");
  sv.remove_prefix(6);
  EXPECT_TRUE(sv.starts_with("world"));
  EXPECT_FALSE(sv.starts_with("hello"));
}

TEST_F(CStringViewTest, EndsWithStringView) {
  cstring_view sv("hello world");
  EXPECT_TRUE(sv.ends_with(std::string_view("world")));
  EXPECT_TRUE(sv.ends_with(std::string_view("hello world")));
  EXPECT_FALSE(sv.ends_with(std::string_view("hello")));
  EXPECT_TRUE(sv.ends_with(std::string_view("")));
}

TEST_F(CStringViewTest, EndsWithChar) {
  cstring_view sv("hello");
  EXPECT_TRUE(sv.ends_with('o'));
  EXPECT_FALSE(sv.ends_with('l'));
  EXPECT_FALSE(sv.ends_with('x'));
}

TEST_F(CStringViewTest, EndsWithCString) {
  cstring_view sv("hello world");
  EXPECT_TRUE(sv.ends_with("world"));
  EXPECT_TRUE(sv.ends_with("hello world"));
  EXPECT_FALSE(sv.ends_with("hello"));
  EXPECT_TRUE(sv.ends_with(""));
}

TEST_F(CStringViewTest, EndsWithEmptyString) {
  cstring_view sv("");
  EXPECT_TRUE(sv.ends_with(""));
  EXPECT_FALSE(sv.ends_with("a"));
}

TEST_F(CStringViewTest, EndsWithAfterSubstr) {
  cstring_view sv("hello world");
  auto sub = sv.substr(6);
  EXPECT_TRUE(sub.ends_with("world"));
  EXPECT_TRUE(sub.ends_with("d"));
  EXPECT_FALSE(sub.ends_with("hello"));
}

#if defined(__cpp_lib_string_contains) && __cpp_lib_string_contains >= 202011L

TEST_F(CStringViewTest, ContainsStringView) {
  cstring_view sv("hello world");
  EXPECT_TRUE(sv.contains(std::string_view("hello")));
  EXPECT_TRUE(sv.contains(std::string_view("world")));
  EXPECT_TRUE(sv.contains(std::string_view("lo wo")));
  EXPECT_FALSE(sv.contains(std::string_view("xyz")));
  EXPECT_TRUE(sv.contains(std::string_view("")));
}

TEST_F(CStringViewTest, ContainsChar) {
  cstring_view sv("hello");
  EXPECT_TRUE(sv.contains('h'));
  EXPECT_TRUE(sv.contains('e'));
  EXPECT_TRUE(sv.contains('l'));
  EXPECT_TRUE(sv.contains('o'));
  EXPECT_FALSE(sv.contains('x'));
}

TEST_F(CStringViewTest, ContainsCString) {
  cstring_view sv("hello world");
  EXPECT_TRUE(sv.contains("hello"));
  EXPECT_TRUE(sv.contains("world"));
  EXPECT_TRUE(sv.contains("lo wo"));
  EXPECT_FALSE(sv.contains("xyz"));
  EXPECT_TRUE(sv.contains(""));
}

TEST_F(CStringViewTest, ContainsEmptyString) {
  cstring_view sv("");
  EXPECT_TRUE(sv.contains(""));
  EXPECT_FALSE(sv.contains("a"));
}

TEST_F(CStringViewTest, ContainsAfterRemovePrefix) {
  cstring_view sv("hello world");
  sv.remove_prefix(6);
  EXPECT_TRUE(sv.contains("world"));
  EXPECT_TRUE(sv.contains("or"));
  EXPECT_FALSE(sv.contains("hello"));
}

#endif

// Comparison operator tests
template <typename TypeParam>
struct CStringViewCmpTest : testing::TestWithParam<TypeParam> {
  using str_t = TypeParam;
  static constexpr char const* nullstr = nullptr;
  static constexpr inline std::array data{
      nullstr,
      "",
      "aloha",
      "hell",
      "hello",
      "hello!",
      "world",
  };

  static auto make_data() {
    std::vector<std::tuple<cstring_view, cstring_view, str_t, str_t>> ret;
    for (char const* const lhs : data) {
      for (char const* const rhs : data) {
        auto lhs_str = !lhs ? str_t{} : str_t{lhs};
        auto rhs_str = !rhs ? str_t{} : str_t{rhs};
        ret.emplace_back(lhs, rhs, std::move(lhs_str), std::move(rhs_str));
      }
    }
    return ret;
  }
};
TYPED_TEST_SUITE_P(CStringViewCmpTest);

#define FOLLY_DETAIL_DEFN_OP(op)                                 \
  [](auto const& lhs, auto const& rhs) {                         \
    if constexpr (                                               \
        std::is_pointer_v<std::remove_cvref_t<decltype(lhs)>> && \
        std::is_pointer_v<std::remove_cvref_t<decltype(rhs)>>) { \
      return strcmp(lhs ? lhs : "", rhs ? rhs : "") op 0;        \
    }                                                            \
    return lhs op rhs;                                           \
  }

TYPED_TEST_P(CStringViewCmpTest, EQ) {
  constexpr auto cmp = FOLLY_DETAIL_DEFN_OP(==);
  for (auto const& [lhs_csv, rhs_csv, lhs_str, rhs_str] : this->make_data()) {
    auto const expected = cmp(lhs_str, rhs_str);
    EXPECT_EQ(expected, cmp(lhs_csv, rhs_str));
    EXPECT_EQ(expected, cmp(lhs_str, rhs_csv));
  }
}

TYPED_TEST_P(CStringViewCmpTest, NE) {
  constexpr auto cmp = FOLLY_DETAIL_DEFN_OP(!=);
  for (auto const& [lhs_csv, rhs_csv, lhs_str, rhs_str] : this->make_data()) {
    auto const expected = cmp(lhs_str, rhs_str);
    EXPECT_EQ(expected, cmp(lhs_csv, rhs_str));
    EXPECT_EQ(expected, cmp(lhs_str, rhs_csv));
  }
}

TYPED_TEST_P(CStringViewCmpTest, LT) {
  constexpr auto cmp = FOLLY_DETAIL_DEFN_OP(<);
  for (auto const& [lhs_csv, rhs_csv, lhs_str, rhs_str] : this->make_data()) {
    auto const expected = cmp(lhs_str, rhs_str);
    EXPECT_EQ(expected, cmp(lhs_csv, rhs_str));
    EXPECT_EQ(expected, cmp(lhs_str, rhs_csv));
  }
}

TYPED_TEST_P(CStringViewCmpTest, LE) {
  constexpr auto cmp = FOLLY_DETAIL_DEFN_OP(<=);
  for (auto const& [lhs_csv, rhs_csv, lhs_str, rhs_str] : this->make_data()) {
    auto const expected = cmp(lhs_str, rhs_str);
    EXPECT_EQ(expected, cmp(lhs_csv, rhs_str));
    EXPECT_EQ(expected, cmp(lhs_str, rhs_csv));
  }
}

TYPED_TEST_P(CStringViewCmpTest, GT) {
  constexpr auto cmp = FOLLY_DETAIL_DEFN_OP(>);
  for (auto const& [lhs_csv, rhs_csv, lhs_str, rhs_str] : this->make_data()) {
    auto const expected = cmp(lhs_str, rhs_str);
    EXPECT_EQ(expected, cmp(lhs_csv, rhs_str));
    EXPECT_EQ(expected, cmp(lhs_str, rhs_csv));
  }
}

TYPED_TEST_P(CStringViewCmpTest, GE) {
  constexpr auto cmp = FOLLY_DETAIL_DEFN_OP(>=);
  for (auto const& [lhs_csv, rhs_csv, lhs_str, rhs_str] : this->make_data()) {
    auto const expected = cmp(lhs_str, rhs_str);
    EXPECT_EQ(expected, cmp(lhs_csv, rhs_str));
    EXPECT_EQ(expected, cmp(lhs_str, rhs_csv));
  }
}

REGISTER_TYPED_TEST_SUITE_P(CStringViewCmpTest, EQ, NE, LT, LE, GT, GE);

INSTANTIATE_TYPED_TEST_SUITE_P( //
    folly_cstring_view,
    CStringViewCmpTest,
    cstring_view);
INSTANTIATE_TYPED_TEST_SUITE_P( //
    std_string_view,
    CStringViewCmpTest,
    string_view);
INSTANTIATE_TYPED_TEST_SUITE_P( //
    folly_string_piece,
    CStringViewCmpTest,
    StringPiece);
INSTANTIATE_TYPED_TEST_SUITE_P( //
    std_string,
    CStringViewCmpTest,
    string);
INSTANTIATE_TYPED_TEST_SUITE_P( //
    folly_fbstring,
    CStringViewCmpTest,
    fbstring);
INSTANTIATE_TYPED_TEST_SUITE_P( //
    c_string,
    CStringViewCmpTest,
    char const*);

TEST_F(CStringViewTest, ThreeWayComparison) {
  const cstring_view a("abc");
  const cstring_view b("abd");
  const cstring_view prefix("ab");
  const cstring_view empty;
  static_assert(std::same_as<decltype(a <=> b), std::strong_ordering>);
  static_assert(noexcept(a <=> b));
  EXPECT_EQ(a <=> b, std::strong_ordering::less);
  EXPECT_EQ(b <=> a, std::strong_ordering::greater);
  EXPECT_EQ(a <=> a, std::strong_ordering::equal);
  EXPECT_TRUE((prefix <=> a) < 0);
  EXPECT_TRUE((empty <=> prefix) < 0);
  EXPECT_EQ(
      (folly::wcstring_view(L"abc") <=> folly::wcstring_view(L"abd")),
      std::strong_ordering::less);
}

// std::hash tests
TEST_F(CStringViewTest, StdHashBasic) {
  cstring_view svz("hello");
  std::string_view sv("hello");

  std::hash<cstring_view> hasher_svz;
  std::hash<std::string_view> hasher_sv;

  EXPECT_EQ(hasher_svz(svz), hasher_sv(sv));
}

TEST_F(CStringViewTest, StdHashDifferentStrings) {
  cstring_view svz1("hello");
  cstring_view svz2("world");

  std::hash<cstring_view> hasher;

  EXPECT_NE(hasher(svz1), hasher(svz2));
}

TEST_F(CStringViewTest, StdHashEmptyString) {
  cstring_view svz("");
  std::string_view sv("");

  std::hash<cstring_view> hasher_svz;
  std::hash<std::string_view> hasher_sv;

  EXPECT_EQ(hasher_svz(svz), hasher_sv(sv));
}

TEST_F(CStringViewTest, StdHashAfterRemovePrefix) {
  cstring_view svz("hello world");
  svz.remove_prefix(6);

  std::string_view sv("world");

  std::hash<cstring_view> hasher_svz;
  std::hash<std::string_view> hasher_sv;

  EXPECT_EQ(hasher_svz(svz), hasher_sv(sv));
}

TEST_F(CStringViewTest, StdHashConsistency) {
  cstring_view svz("test");
  std::hash<cstring_view> hasher;

  size_t hash1 = hasher(svz);
  size_t hash2 = hasher(svz);

  EXPECT_EQ(hash1, hash2);
}

TEST_F(CStringViewTest, UnorderedSetUsage) {
  std::unordered_set<cstring_view> set;

  set.insert(cstring_view("hello"));
  set.insert(cstring_view("world"));
  set.insert(cstring_view("hello")); // duplicate

  EXPECT_EQ(set.size(), 2u);
  EXPECT_TRUE(set.find(cstring_view("hello")) != set.end());
  EXPECT_TRUE(set.find(cstring_view("world")) != set.end());
  EXPECT_TRUE(set.find(cstring_view("foo")) == set.end());
}

TEST_F(CStringViewTest, UnorderedMapUsage) {
  std::unordered_map<cstring_view, int> map;

  map[cstring_view("one")] = 1;
  map[cstring_view("two")] = 2;
  map[cstring_view("three")] = 3;

  EXPECT_EQ(map.size(), 3u);
  EXPECT_EQ(map[cstring_view("one")], 1);
  EXPECT_EQ(map[cstring_view("two")], 2);
  EXPECT_EQ(map[cstring_view("three")], 3);
}

TEST_F(CStringViewTest, Stream) {
  std::ostringstream out;
  out << cstring_view("hello");
  EXPECT_EQ("hello", out.str());
}

// fmt::formatter tests
TEST_F(CStringViewTest, FmtFormatterBasic) {
  cstring_view svz("hello");
  std::string_view sv("hello");

  std::string result_svz = fmt::format("{}", svz);
  std::string result_sv = fmt::format("{}", sv);

  EXPECT_EQ(result_svz, result_sv);
  EXPECT_EQ(result_svz, "hello");
}

TEST_F(CStringViewTest, FmtFormatterEmptyString) {
  cstring_view svz("");
  std::string result = fmt::format("{}", svz);

  EXPECT_EQ(result, "");
}

TEST_F(CStringViewTest, FmtFormatterWithFormatSpec) {
  cstring_view svz("hello");
  std::string_view sv("hello");

  std::string result_svz = fmt::format("{:>10}", svz);
  std::string result_sv = fmt::format("{:>10}", sv);

  EXPECT_EQ(result_svz, result_sv);
  EXPECT_EQ(result_svz, "     hello");
}

TEST_F(CStringViewTest, FmtFormatterMultipleValues) {
  cstring_view svz1("hello");
  cstring_view svz2("world");

  std::string result = fmt::format("{} {}", svz1, svz2);

  EXPECT_EQ(result, "hello world");
}

TEST_F(CStringViewTest, FmtFormatterAfterRemovePrefix) {
  cstring_view svz("hello world");
  svz.remove_prefix(6);

  std::string result = fmt::format("{}", svz);

  EXPECT_EQ(result, "world");
}

TEST_F(CStringViewTest, FmtFormatterPadding) {
  cstring_view svz("test");
  std::string_view sv("test");

  std::string result_svz = fmt::format("{:<10}", svz);
  std::string result_sv = fmt::format("{:<10}", sv);

  EXPECT_EQ(result_svz, result_sv);
  EXPECT_EQ(result_svz, "test      ");
}

TEST_F(CStringViewTest, FmtFormatterCenter) {
  cstring_view svz("hi");
  std::string_view sv("hi");

  std::string result_svz = fmt::format("{:^6}", svz);
  std::string result_sv = fmt::format("{:^6}", sv);

  EXPECT_EQ(result_svz, result_sv);
  EXPECT_EQ(result_svz, "  hi  ");
}
