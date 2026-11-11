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

#include <folly/Overload.h>

#include <optional>
#include <type_traits>
#include <variant>

#include <folly/portability/GTest.h>

namespace folly {
namespace test {
namespace {

constexpr int functionUnsigned(const unsigned x) {
  return x + 3;
}
struct HasMemberFunctions {
  constexpr int withConst(const int x) const { return x + 4; }
  constexpr int withMutable(const int x) { return x + 5; }
};
struct HasRefQualifiedMemberFunctions {
  constexpr int withConstRef(const int x) const& { return x + 6; }
  constexpr int withMutableRef(const int x) & { return x + 7; }
  constexpr int withMutableRefRef(const int x) && { return x + 8; }
  constexpr int withConstRefRef(const int x) const&& { return x + 9; }
};
struct HasNullaryMemberFunction {
  constexpr int nullary() const { return -1; }
};
struct HasMemberData {
  int member = -2;
};
struct HasNullaryMemberFunctionAndMemberData {
  constexpr int nullary() const { return -3; }
  int member = -4;
};

constexpr auto overloadSet = overload(
    []() { return 1; },
    [](const int x) { return x + 1; },
    [](const long x) { return x + 2; },
    functionUnsigned,
    &HasMemberFunctions::withConst,
    &HasMemberFunctions::withMutable,
    &HasRefQualifiedMemberFunctions::withConstRef,
    &HasRefQualifiedMemberFunctions::withMutableRef,
    &HasRefQualifiedMemberFunctions::withMutableRefRef,
    &HasRefQualifiedMemberFunctions::withConstRefRef,
    &HasNullaryMemberFunction::nullary,
    &HasMemberData::member);

static_assert(overloadSet() == 1);
static_assert(overloadSet(1) == 2);
static_assert(overloadSet(1L) == 3);
static_assert(overloadSet(1U) == 4);

constexpr auto testConstMember() {
  HasMemberFunctions const c;
  return overloadSet(c, 1);
}
static_assert(testConstMember() == 5);
static_assert(
    overload(&HasMemberFunctions::withConst)(HasMemberFunctions(), 1) == 5);

constexpr auto testMutableMember() {
  HasMemberFunctions c;
  return overloadSet(c, 1);
}
static_assert(testMutableMember() == 6);
static_assert(overloadSet(HasMemberFunctions(), 1) == 6);

constexpr auto testConstRefMember() {
  const HasRefQualifiedMemberFunctions c;
  return overloadSet(c, 1);
}
static_assert(testConstRefMember() == 7);

constexpr auto testMutableRefMember() {
  HasRefQualifiedMemberFunctions c;
  return overloadSet(c, 1);
}
static_assert(testMutableRefMember() == 8);

static_assert(overloadSet(HasRefQualifiedMemberFunctions(), 1) == 9);

constexpr auto testConstRefRefMember() {
  const HasRefQualifiedMemberFunctions c;
  // NOLINTNEXTLINE(performance-move-const-arg)
  return overloadSet(std::move(c), 1);
}
static_assert(testConstRefRefMember() == 10);

static_assert(overloadSet(HasNullaryMemberFunction()) == -1);
static_assert(overloadSet(HasMemberData()) == -2);

void isNoexcept(int) noexcept {}
void isNotNoexcept(unsigned) {}

constexpr auto noexceptOverload = overload(isNoexcept, isNotNoexcept);
static_assert(noexcept(noexceptOverload(1)));
static_assert(!noexcept(noexceptOverload(1U)));

constexpr auto ambiguous = overload([] {}, [] { return 5; });
static_assert(!std::is_invocable_v<decltype(ambiguous)>);

constexpr auto emptyOverloadSet = overload();
static_assert(!std::is_invocable_v<decltype(emptyOverloadSet)>);

constexpr auto nullaryMemberAndMemberData = overload(
    &HasNullaryMemberFunctionAndMemberData::nullary,
    &HasNullaryMemberFunctionAndMemberData::member);
static_assert(!std::is_invocable_v<decltype(nullaryMemberAndMemberData)>);

constexpr auto onlyMemberData = overload(&HasMemberData::member);
static_assert(std::is_same_v<
              decltype(onlyMemberData(std::declval<HasMemberData&>())),
              int&>);
static_assert(std::is_same_v<
              decltype(onlyMemberData(std::declval<const HasMemberData&>())),
              const int&>);
static_assert(std::is_same_v<
              decltype(onlyMemberData(std::declval<HasMemberData&&>())),
              int&&>);
static_assert(std::is_same_v<
              decltype(onlyMemberData(std::declval<const HasMemberData&&>())),
              const int&&>);

struct One {
  std::string toString() const { return "One"; }
};
struct Two {
  std::string toString() const { return "Two"; }
};

TEST(Overload, StdVariant) {
  using V = std::variant<One, Two>;
  V one(One{});
  V two(Two{});

  EXPECT_TRUE(variant_match(
      one, [](const One&) { return true; }, [](const Two&) { return false; }));
  EXPECT_TRUE(variant_match(
      two, [](const One&) { return false; }, [](const Two&) { return true; }));
  EXPECT_TRUE(variant_match(
      std::move(one), [](One&&) { return true; }, [](Two&&) { return false; }));
  EXPECT_TRUE(variant_match(
      std::move(two), [](One&&) { return false; }, [](Two&&) { return true; }));

  auto toString = [](const auto& variant) {
    return variant_match(variant, [](const auto& value) {
      return value.toString();
    });
  };
  EXPECT_EQ(toString(one), "One");
  EXPECT_EQ(toString(two), "Two");
}

struct DerivedVariant : std::variant<One, Two> {
  using std::variant<One, Two>::variant;
};

TEST(Overload, DerivedStdVariant) {
  const DerivedVariant one(One{});
  EXPECT_TRUE(variant_match(
      one, [](const One&) { return true; }, [](const Two&) { return false; }));
  EXPECT_EQ(
      variant_match<std::string>(
          one,
          [](const One&) { return "One"; },
          [](const Two&) { return "Two"; }),
      "One");
  EXPECT_TRUE(variant_match(
      DerivedVariant(Two{}),
      [](One&&) { return false; },
      [](Two&&) { return true; }));
  EXPECT_EQ(
      variant_match<std::string>(
          DerivedVariant(Two{}),
          [](One&&) { return "One"; },
          [](Two&&) { return "Two"; }),
      "Two");
}

TEST(Overload, StdPattern) {
  using V = std::variant<One, Two>;
  V one(One{});
  V two(Two{});

  auto is_one_overload = overload(
      [](const One&) { return true; }, [](const Two&) { return false; });
  EXPECT_TRUE(std::visit(is_one_overload, one));
  EXPECT_TRUE(variant_match(one, is_one_overload));
  EXPECT_FALSE(variant_match(two, is_one_overload));

  auto is_two_overload = overload(
      [](const One&) { return false; }, [](const Two&) { return true; });
  EXPECT_TRUE(std::visit(is_two_overload, two));
  EXPECT_FALSE(variant_match(one, is_two_overload));
  EXPECT_TRUE(variant_match(two, is_two_overload));

  auto is_one_copy = overload(is_one_overload);
  auto is_one_const_copy =
      overload(static_cast<const decltype(is_one_overload)&>(is_one_overload));
  EXPECT_TRUE(variant_match(one, is_one_copy));
  EXPECT_TRUE(variant_match(one, is_one_const_copy));
  EXPECT_FALSE(variant_match(two, is_one_copy));
  EXPECT_FALSE(variant_match(two, is_one_const_copy));
}

TEST(Overload, ReturnType) {
  using V = std::variant<std::monostate, One>;
  V null(std::monostate{});
  V one(One{});

  using R = std::optional<int>;

  EXPECT_FALSE(variant_match(
      null,
      [](const std::monostate&) -> R { return std::nullopt; },
      [](const One&) -> R { return 1; }));
  EXPECT_EQ(
      1,
      variant_match(
          one,
          [](const std::monostate&) -> R { return std::nullopt; },
          [](const One&) -> R { return 1; }));

  EXPECT_FALSE(
      variant_match<R>(
          null,
          [](const std::monostate&) { return std::nullopt; },
          [](const One&) { return 1; }));
  EXPECT_EQ(
      1,
      variant_match<R>(
          one,
          [](const std::monostate&) { return std::nullopt; },
          [](const One&) { return 1; }));

  int null_result = -1;
  variant_match<void>(
      null,
      [&null_result](const std::monostate&) {
        null_result = 0;
        return std::nullopt;
      },
      [&null_result](const One&) {
        null_result = 1;
        return 1;
      });
  EXPECT_EQ(0, null_result);

  int one_result = -1;
  variant_match<void>(
      one,
      [&one_result](const std::monostate&) {
        one_result = 0;
        return std::nullopt;
      },
      [&one_result](const One&) {
        one_result = 1;
        return 1;
      });
  EXPECT_EQ(1, one_result);

  EXPECT_FALSE(
      variant_match<R>(
          std::move(null),
          [](std::monostate&&) { return std::nullopt; },
          [](One&&) { return 1; }));
}

} // namespace
} // namespace test
} // namespace folly
