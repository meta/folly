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

#include <folly/json/DynamicConverter.h>

#include <algorithm>
#include <map>
#include <optional>
#include <unordered_set>
#include <vector>

#include <folly/Expected.h>
#include <folly/FBVector.h>
#include <folly/Optional.h>
#include <folly/portability/GTest.h>

FOLLY_GNU_DISABLE_WARNING("-Wdeprecated-declarations")

namespace folly {

namespace { // static tests

using namespace dynamicconverter_detail;
struct Foo {};

static_assert(!is_container<int>::value);
static_assert(!is_container<std::pair<int, int>>::value);
static_assert(!is_container<Foo>::value);
static_assert(!class_is_container<Foo>::value);

static_assert(is_container<std::vector<int>>::value);
static_assert(is_container<std::set<int>>::value);
static_assert(is_container<std::map<int, int>>::value);
static_assert(class_is_container<std::vector<Foo>>::value);

static_assert(!is_map<int>::value);
static_assert(!is_map<std::set<int>>::value);

static_assert(is_map<std::map<int, int>>::value);

static_assert(!is_range<int>::value);

static_assert(is_range<std::set<int>>::value);
static_assert(is_range<std::vector<int>>::value);

static_assert(is_like_pointer<Expected<int, std::nullptr_t>>::value);
static_assert(is_like_pointer<std::optional<int>>::value);
static_assert(is_like_pointer<folly::Optional<int>>::value);
static_assert(is_like_pointer<int*>::value);
static_assert(is_like_pointer<std::unique_ptr<int>>::value);

static_assert(is_optional<Expected<int, std::nullptr_t>>::value);
static_assert(is_optional<std::optional<int>>::value);
static_assert(is_optional<folly::Optional<int>>::value);
static_assert(!is_optional<int*>::value);
static_assert(!is_optional<std::unique_ptr<int>>::value);

} // namespace

TEST(DynamicConverter, dynamics) {
  dynamic d1 = 12;
  auto d2 = convertTo<dynamic>(d1);
  EXPECT_EQ(12, d2.getInt());

  dynamic d3 = dynamic::array(1, 2, 3);
  auto v = convertTo<std::vector<dynamic>>(d3);
  EXPECT_EQ(1, v.at(0).getInt());
  EXPECT_EQ(2, v.at(1).getInt());
  EXPECT_EQ(3, v.at(2).getInt());

  dynamic d4 = dynamic::object("1", 1)("2", 2.3);
  auto m = convertTo<std::map<std::string, dynamic>>(d4);
  EXPECT_EQ(1, m.at("1").getInt());
  EXPECT_EQ(2.3, m.at("2").getDouble());
}

TEST(DynamicConverter, arithmeticTypes) {
  dynamic d1 = 12;
  auto i1 = convertTo<int>(d1);
  EXPECT_EQ(i1, 12);

  dynamic d2 = 123456789012345;
  auto i2 = convertTo<int64_t>(d2);
  EXPECT_EQ(i2, 123456789012345);

  dynamic d4 = 3.141;
  auto i4 = convertTo<float>(d4);
  EXPECT_EQ((int)(i4 * 100), 314);

  dynamic d5 = true;
  auto i5 = convertTo<bool>(d5);
  EXPECT_EQ(i5, true);

  dynamic d6 = 15;
  const auto i6 = convertTo<const int>(d6);
  EXPECT_EQ(i6, 15);

  dynamic d7 = "87";
  auto i7 = convertTo<int>(d7);
  EXPECT_EQ(i7, 87);

  dynamic d8 = "false";
  auto i8 = convertTo<bool>(d8);
  EXPECT_EQ(i8, false);
}

TEST(DynamicConverter, enums) {
  enum enum1 { foo = 1, bar = 2 };

  dynamic d1 = 1;
  auto i1 = convertTo<enum1>(d1);
  EXPECT_EQ(i1, foo);

  dynamic d2 = 2;
  auto i2 = convertTo<enum1>(d2);
  EXPECT_EQ(i2, bar);

  enum class enum2 { FOO = 1, BAR = 2 };

  dynamic d3 = 1;
  auto i3 = convertTo<enum2>(d3);
  EXPECT_EQ(i3, enum2::FOO);

  dynamic d4 = 2;
  auto i4 = convertTo<enum2>(d4);
  EXPECT_EQ(i4, enum2::BAR);
}

TEST(DynamicConverter, simpleBuiltins) {
  dynamic d1 = "Haskell";
  auto i1 = convertTo<folly::fbstring>(d1);
  EXPECT_EQ(i1, "Haskell");

  dynamic d2 = 13;
  auto i2 = convertTo<std::string>(d2);
  EXPECT_EQ(i2, "13");

  dynamic d3 = dynamic::array(12, "Scala");
  auto i3 = convertTo<std::pair<int, std::string>>(d3);
  EXPECT_EQ(i3.first, 12);
  EXPECT_EQ(i3.second, "Scala");

  dynamic d4 = dynamic::object("C", "C++");
  auto i4 = convertTo<std::pair<std::string, folly::fbstring>>(d4);
  EXPECT_EQ(i4.first, "C");
  EXPECT_EQ(i4.second, "C++");
}

TEST(DynamicConverter, optional) {
  using Optionals = std::pair<std::optional<int>, Optional<int>>;
  const dynamic null;
  EXPECT_TRUE(Optionals(4, 5) == convertTo<Optionals>(dynamic::array(4, 5)));
  EXPECT_TRUE(Optionals() == convertTo<Optionals>(dynamic::array(null, null)));
  EXPECT_TRUE(toDynamic(Optionals(4, 5)) == dynamic::array(4, 5));
  EXPECT_TRUE(toDynamic(Optionals()) == dynamic::array(null, null));
}

TEST(DynamicConverter, pointer) {
  const int x = 7;
  using Pointers = std::pair<const int*, int*>;
  const dynamic null;
  EXPECT_TRUE(toDynamic(Pointers(&x, nullptr)) == dynamic::array(7, null));
  // Cannot convert to pointers.
}

TEST(DynamicConverter, simpleFbvector) {
  dynamic d1 = dynamic::array(1, 2, 3);
  auto i1 = convertTo<folly::fbvector<int>>(d1);
  decltype(i1) i1b = {1, 2, 3};
  EXPECT_EQ(i1, i1b);
}

TEST(DynamicConverter, simpleContainer) {
  dynamic d1 = dynamic::array(1, 2, 3);
  auto i1 = convertTo<std::vector<int>>(d1);
  decltype(i1) i1b = {1, 2, 3};
  EXPECT_EQ(i1, i1b);

  dynamic d2 = dynamic::array(1, 3, 5, 2, 4);
  auto i2 = convertTo<std::set<int>>(d2);
  decltype(i2) i2b = {1, 2, 3, 5, 4};
  EXPECT_EQ(i2, i2b);
}

TEST(DynamicConverter, simpleMap) {
  dynamic d1 = dynamic::object(1, "one")(2, "two");
  auto i1 = convertTo<std::map<int, std::string>>(d1);
  decltype(i1) i1b = {{1, "one"}, {2, "two"}};
  EXPECT_EQ(i1, i1b);

  dynamic d2 =
      dynamic::array(dynamic::array(3, "three"), dynamic::array(4, "four"));
  auto i2 = convertTo<std::unordered_map<int, std::string>>(d2);
  decltype(i2) i2b = {{3, "three"}, {4, "four"}};
  EXPECT_EQ(i2, i2b);
}

TEST(DynamicConverter, mapKeyedByString) {
  dynamic d1 = dynamic::object("1", "one")("2", "two");
  auto i1 = convertTo<std::map<std::string, std::string>>(d1);
  decltype(i1) i1b = {{"1", "one"}, {"2", "two"}};
  EXPECT_EQ(i1, i1b);

  dynamic d2 =
      dynamic::array(dynamic::array("3", "three"), dynamic::array("4", "four"));
  auto i2 = convertTo<std::unordered_map<std::string, std::string>>(d2);
  decltype(i2) i2b = {{"3", "three"}, {"4", "four"}};
  EXPECT_EQ(i2, i2b);
}

TEST(DynamicConverter, mapToVectorOfPairs) {
  dynamic d1 = dynamic::object("1", "one")("2", "two");
  auto i1 = convertTo<std::vector<std::pair<std::string, std::string>>>(d1);
  std::sort(i1.begin(), i1.end());
  decltype(i1) i1b = {{"1", "one"}, {"2", "two"}};
  EXPECT_EQ(i1, i1b);
}

TEST(DynamicConverter, nestedContainers) {
  dynamic d1 =
      dynamic::array(dynamic::array(1), dynamic::array(), dynamic::array(2, 3));
  auto i1 = convertTo<folly::fbvector<std::vector<uint8_t>>>(d1);
  decltype(i1) i1b = {{1}, {}, {2, 3}};
  EXPECT_EQ(i1, i1b);

  dynamic h2a = dynamic::array("3", ".", "1", "4");
  dynamic h2b = dynamic::array("2", ".", "7", "2");
  dynamic d2 = dynamic::object(3.14, h2a)(2.72, h2b);
  auto i2 = convertTo<std::map<double, std::vector<folly::fbstring>>>(d2);
  decltype(i2) i2b = {
      {3.14, {"3", ".", "1", "4"}},
      {2.72, {"2", ".", "7", "2"}},
  };
  EXPECT_EQ(i2, i2b);
}

struct A {
  int i;
  bool operator==(const A& o) const { return i == o.i; }
};

template <>
struct DynamicConverter<A> {
  static A convert(const dynamic& d) { return {convertTo<int>(d["i"])}; }
};

TEST(DynamicConverter, customClass) {
  dynamic d1 = dynamic::object("i", 17);
  auto i1 = convertTo<A>(d1);
  EXPECT_EQ(i1.i, 17);

  dynamic d2 =
      dynamic::array(dynamic::object("i", 18), dynamic::object("i", 19));
  auto i2 = convertTo<std::vector<A>>(d2);
  decltype(i2) i2b = {{18}, {19}};
  EXPECT_EQ(i2, i2b);
}

TEST(DynamicConverter, crazy) {
  // we are going to create a vector<unordered_map<bool, T>>
  // we will construct some of the maps from dynamic objects,
  //   some from a vector of KV pairs.
  // T will be vector<set<string>>

  std::set<std::string> s1 = {"a", "e", "i", "o", "u"};
  std::set<std::string> s2 = {"2", "3", "5", "7"};
  std::set<std::string> s3 = {"Hello", "World"};

  std::vector<std::set<std::string>> v1 = {};
  std::vector<std::set<std::string>> v2 = {s1, s2};
  std::vector<std::set<std::string>> v3 = {s3};

  std::unordered_map<bool, std::vector<std::set<std::string>>> m1 = {
      {true, v1}, {false, v2}};
  std::unordered_map<bool, std::vector<std::set<std::string>>> m2 = {
      {true, v3}};

  std::vector<std::unordered_map<bool, std::vector<std::set<std::string>>>> f1 =
      {m1, m2};

  dynamic ds1 = dynamic::array("a", "e", "i", "o", "u");
  dynamic ds2 = dynamic::array("2", "3", "5", "7");
  dynamic ds3 = dynamic::array("Hello", "World");

  dynamic dv1 = dynamic::array;
  dynamic dv2 = dynamic::array(ds1, ds2);
  dynamic dv3(dynamic::array(ds3));

  dynamic dm1 = dynamic::object(true, dv1)(false, dv2);
  dynamic dm2 = dynamic::array(dynamic::array(true, dv3));

  dynamic df1 = dynamic::array(dm1, dm2);

  auto i = convertTo<std::vector<
      std::unordered_map<bool, std::vector<std::set<std::string>>>>>(
      df1); // yes, that is 5 close-chevrons

  EXPECT_EQ(f1, i);
}

TEST(DynamicConverter, consts) {
  dynamic d1 = 7.5;
  auto i1 = convertTo<const double>(d1);
  EXPECT_EQ(7.5, i1);

  dynamic d2 = "Hello";
  auto i2 = convertTo<const std::string>(d2);
  decltype(i2) i2b = "Hello";
  EXPECT_EQ(i2b, i2);

  dynamic d3 = true;
  auto i3 = convertTo<const bool>(d3);
  EXPECT_TRUE(i3);

  dynamic d4 = "true";
  auto i4 = convertTo<const bool>(d4);
  EXPECT_TRUE(i4);

  dynamic d5 = dynamic::array(1, 2);
  auto i5 = convertTo<const std::pair<const int, const int>>(d5);
  decltype(i5) i5b = {1, 2};
  EXPECT_EQ(i5b, i5);
}

struct Token {
  int kind_;
  fbstring lexeme_;

  explicit Token(int kind, const fbstring& lexeme)
      : kind_(kind), lexeme_(lexeme) {}
};

template <>
struct DynamicConverter<Token> {
  static Token convert(const dynamic& d) {
    int k = convertTo<int>(d["KIND"]);
    fbstring lex = convertTo<fbstring>(d["LEXEME"]);
    return Token(k, lex);
  }
};

TEST(DynamicConverter, example) {
  dynamic d1 = dynamic::object("KIND", 2)("LEXEME", "a token");
  auto i1 = convertTo<Token>(d1);
  EXPECT_EQ(i1.kind_, 2);
  EXPECT_EQ(i1.lexeme_, "a token");
}

TEST(DynamicConverter, construct) {
  using std::map;
  using std::pair;
  using std::string;
  using std::vector;
  {
    vector<int> c{1, 2, 3};
    dynamic d = dynamic::array(1, 2, 3);
    EXPECT_EQ(d, toDynamic(c));
  }

  {
    vector<float> c{1.0f, 2.0f, 4.0f};
    dynamic d = dynamic::array(1.0, 2.0, 4.0);
    EXPECT_EQ(d, toDynamic(c));
  }

  {
    map<int, int> c{{2, 4}, {3, 9}};
    dynamic d = dynamic::object(2, 4)(3, 9);
    EXPECT_EQ(d, toDynamic(c));
  }

  {
    map<string, string> c{{"a", "b"}};
    dynamic d = dynamic::object("a", "b");
    EXPECT_EQ(d, toDynamic(c));
  }

  {
    map<string, pair<string, int>> c{{"a", {"b", 3}}};
    dynamic d = dynamic::object("a", dynamic::array("b", 3));
    EXPECT_EQ(d, toDynamic(c));
  }

  {
    map<string, pair<string, int>> c{{"a", {"b", 3}}};
    dynamic d = dynamic::object("a", dynamic::array("b", 3));
    EXPECT_EQ(d, toDynamic(c));
  }

  {
    vector<int> vi{2, 3, 4, 5};
    auto c = std::make_pair(
        range(vi.begin(), vi.begin() + 3),
        range(vi.begin() + 1, vi.begin() + 4));
    dynamic d =
        dynamic::array(dynamic::array(2, 3, 4), dynamic::array(3, 4, 5));
    EXPECT_EQ(d, toDynamic(c));
  }

  {
    vector<Optional<int>> c{3, none, 4};
    dynamic d = dynamic::array(3, dynamic(), 4);
    EXPECT_EQ(d, toDynamic(c));
  }

  {
    vector<bool> vb{true, false};
    dynamic d = dynamic::array(true, false);
    EXPECT_EQ(d, toDynamic(vb));
  }

  {
    enum enum1 { foo = 1, bar = 2 };

    dynamic d1 = 1;
    EXPECT_EQ(d1, toDynamic(enum1::foo));

    dynamic d2 = 2;
    EXPECT_EQ(d2, toDynamic(enum1::bar));

    enum class enum2 : char { FOO = 'a', BAR = 'b' };

    dynamic d3 = 'a';
    EXPECT_EQ(d3, toDynamic(enum2::FOO));

    dynamic d4 = 'b';
    EXPECT_EQ(d4, toDynamic(enum2::BAR));
  }
}

TEST(DynamicConverter, errors) {
  const auto int32Over =
      static_cast<int64_t>(std::numeric_limits<int32_t>().max()) + 1;
  const auto floatOver =
      static_cast<double>(std::numeric_limits<float>().max()) * 2;

  dynamic d1 = int32Over;
  EXPECT_THROW(convertTo<int32_t>(d1), std::range_error);

  dynamic d2 = floatOver;
  EXPECT_THROW(convertTo<float>(d2), std::range_error);
}

TEST(DynamicConverter, partialDynamics) {
  std::vector<dynamic> c{
      dynamic::array(2, 3, 4),
      dynamic::array(3, 4, 5),
  };
  dynamic d = dynamic::array(dynamic::array(2, 3, 4), dynamic::array(3, 4, 5));
  EXPECT_EQ(d, toDynamic(c));

  std::unordered_map<std::string, dynamic> m{{"one", 1}, {"two", 2}};
  dynamic md = dynamic::object("one", 1)("two", 2);
  EXPECT_EQ(md, toDynamic(m));
}

TEST(DynamicConverter, asanExceptionCaseUmap) {
  EXPECT_THROW(
      (convertTo<std::unordered_map<int, int>>(dynamic::array(1))), TypeError);
}

TEST(DynamicConverter, asanExceptionCaseUset) {
  EXPECT_THROW(
      (convertTo<std::unordered_set<int>>(
          dynamic::array(1, dynamic::array(), 3))),
      TypeError);
}

static int constructB = 0;
static int destroyB = 0;
static int ticker = 0;
struct B {
  struct BException : std::exception {};

  /* implicit */ B(int x) : x_(x) {
    if (ticker-- == 0) {
      throw BException();
    }
    constructB++;
  }
  B(const B& o) : x_(o.x_) { constructB++; }
  ~B() { destroyB++; }
  int x_;
};

template <>
struct DynamicConverter<B> {
  static B convert(const dynamic& d) { return B(convertTo<int>(d)); }
};

TEST(DynamicConverter, doubleDestroy) {
  dynamic d = dynamic::array(1, 3, 5, 7, 9, 11, 13, 15, 17);
  ticker = 3;

  EXPECT_THROW(convertTo<std::vector<B>>(d), B::BException);
  EXPECT_EQ(constructB, destroyB);
}

TEST(DynamicConverter, simpleVectorBool) {
  std::vector<bool> bools{true, false};
  auto d = toDynamic(bools);
  auto actual = convertTo<decltype(bools)>(d);
  EXPECT_EQ(bools, actual);
}

} // namespace folly
