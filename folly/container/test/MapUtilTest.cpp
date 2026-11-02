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

#include <folly/MapUtil.h>

#include <cstddef>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

#include <folly/Traits.h>
#include <folly/portability/GTest.h>
#include <folly/sorted_vector_types.h>

using namespace folly;

TEST(MapUtil, getDefault) {
  std::map<int, int> m;
  m[1] = 2;
  EXPECT_EQ(2, get_default(m, 1, 42));
  EXPECT_EQ(42, get_default(m, 2, 42));
  EXPECT_EQ(0, get_default(m, 3));
}

TEST(MapUtil, getDefaultFunction) {
  std::map<int, int> m;
  m[1] = 2;
  EXPECT_EQ(2, get_default(m, 1, [] { return 42; }));
  EXPECT_EQ(42, get_default(m, 2, [] { return 42; }));
  EXPECT_EQ(0, get_default(m, 3));
}

TEST(MapUtil, getDefaultPointer) {
  std::map<int, int> m;
  m[1] = 2;
  const auto* pm = &m;
  const std::map<int, int>* nullMap = nullptr;
  // No-default overload.
  EXPECT_EQ(2, get_default(pm, 1));
  EXPECT_EQ(0, get_default(pm, 3));
  // Explicit-default overload.
  EXPECT_EQ(2, get_default(pm, 1, 42));
  EXPECT_EQ(42, get_default(pm, 2, 42));
  // Lazy-default overload.
  EXPECT_EQ(2, get_default(pm, 1, [] { return 42; }));
  EXPECT_EQ(42, get_default(pm, 2, [] { return 42; }));
  // Null map returns the default in all forms.
  EXPECT_EQ(0, get_default(nullMap, 1));
  EXPECT_EQ(42, get_default(nullMap, 1, 42));
  EXPECT_EQ(42, get_default(nullMap, 1, [] { return 42; }));
}

TEST(MapUtil, getDefaultPathPointer) {
  using std::map;
  map<int, map<int, int>> m;
  m[4][2] = 42;
  const auto* pm = &m;
  const map<int, map<int, int>>* nullMap = nullptr;
  EXPECT_EQ(42, get_default(pm, 4, 2, 0));
  EXPECT_EQ(0, get_default(pm, 1, 3, 0));
  EXPECT_EQ(0, get_default(nullMap, 4, 2, 0));
}

TEST(MapUtil, getOrThrow) {
  std::map<int, int> m;
  m[1] = 2;
  EXPECT_EQ(2, get_or_throw(m, 1));
  EXPECT_THROW(get_or_throw(m, 2), std::out_of_range);
  EXPECT_EQ(&m[1], &get_or_throw(m, 1));
  get_or_throw(m, 1) = 3;
  EXPECT_EQ(3, get_or_throw(m, 1));
  const auto& cm = m;
  EXPECT_EQ(&m[1], &get_or_throw(cm, 1));
  EXPECT_EQ(3, get_or_throw(cm, 1));
  EXPECT_THROW(get_or_throw(cm, 2), std::out_of_range);
}

TEST(MapUtil, getOrThrowSpecified) {
  std::map<int, int> m;
  m[1] = 2;
  EXPECT_EQ(2, get_or_throw<std::runtime_error>(m, 1));
  EXPECT_THROW(get_or_throw<std::runtime_error>(m, 2), std::runtime_error);
}

namespace {

enum class ScopedKey { A = 1, B = 7 };
enum UnscopedKey { kUnscopedA = 1, kUnscopedB = 42 };
enum class FormattableKey { A = 1, B = 2 };

template <typename Map, typename Key>
std::string getOrThrowMessage(Map& map, const Key& key) {
  try {
    get_or_throw(map, key, "missing: ");
  } catch (const std::out_of_range& e) {
    return e.what();
  }
  return {};
}

} // namespace

template <>
struct fmt::formatter<FormattableKey> : fmt::formatter<std::string_view> {
  auto format(FormattableKey key, fmt::format_context& ctx) const {
    return fmt::formatter<std::string_view>::format(
        key == FormattableKey::A ? "A" : "B", ctx);
  }
};

TEST(MapUtil, getOrThrowEnumKey) {
  std::map<ScopedKey, int> scoped{{ScopedKey::A, 1}};
  EXPECT_EQ("missing: 7", getOrThrowMessage(scoped, ScopedKey::B));
  EXPECT_EQ(
      "missing: 7", getOrThrowMessage(std::as_const(scoped), ScopedKey::B));

  std::map<UnscopedKey, int> unscoped{{kUnscopedA, 1}};
  EXPECT_EQ("missing: 42", getOrThrowMessage(unscoped, kUnscopedB));
  EXPECT_EQ(
      "missing: 42", getOrThrowMessage(std::as_const(unscoped), kUnscopedB));

  std::map<FormattableKey, int> formattable{{FormattableKey::A, 1}};
  EXPECT_EQ("missing: B", getOrThrowMessage(formattable, FormattableKey::B));
}

TEST(MapUtil, getOptional) {
  std::map<int, int> m;
  m[1] = 2;
  EXPECT_TRUE(get_optional(m, 1).has_value());
  EXPECT_EQ(2, get_optional(m, 1).value());
  EXPECT_FALSE(get_optional(m, 2).has_value());
}

TEST(MapUtil, getOptionalView) {
  std::map<int, std::string> m;
  m[1] = "2";
  EXPECT_TRUE(get_optional<std::optional<std::string_view>>(m, 1).has_value());
  EXPECT_EQ("2", get_optional<std::optional<std::string_view>>(m, 1).value());
  EXPECT_FALSE(get_optional<std::optional<std::string_view>>(m, 2).has_value());
}

TEST(MapUtil, getOptionalPathSimple) {
  using std::map;
  map<int, map<int, map<int, map<int, int>>>> m{{1, {{2, {{3, {{4, 5}}}}}}}};
  EXPECT_EQ(folly::Optional<int>(5), get_optional(m, 1, 2, 3, 4));
  EXPECT_TRUE(get_optional(m, 1, 2, 3, 4));
  EXPECT_FALSE(get_optional(m, 1, 2, 3, 0));
  EXPECT_TRUE(get_optional(m, 1, 2, 3));
  EXPECT_FALSE(get_optional(m, 1, 2, 0));
  EXPECT_TRUE(get_optional(m, 1, 2));
  EXPECT_FALSE(get_optional(m, 1, 0));
  EXPECT_TRUE(get_optional(m, 1));
  EXPECT_FALSE(get_optional(m, 0));
}

TEST(MapUtil, getOptionalPathMixed) {
  using std::map;
  using std::string;
  using std::unordered_map;
  unordered_map<string, map<int, map<string, int>>> m{{"a", {{1, {{"b", 2}}}}}};
  EXPECT_EQ(folly::Optional<int>(2), get_optional(m, "a", 1, "b"));
  EXPECT_TRUE(get_optional(m, "a", 1, "b"));
  EXPECT_FALSE(get_optional(m, "b", 1, "b"));
  EXPECT_FALSE(get_optional(m, "a", 2, "b"));
  EXPECT_FALSE(get_optional(m, "a", 1, "c"));
  EXPECT_TRUE(get_optional(m, "a", 1));
  EXPECT_TRUE(get_optional(m, "a"));
}

TEST(MapUtil, getOptionalStd) {
  std::map<int, int> m;
  m[1] = 2;
  EXPECT_TRUE(get_optional<std::optional>(m, 1).has_value());
  EXPECT_EQ(2, get_optional<std::optional>(m, 1).value());
  EXPECT_FALSE(get_optional<std::optional>(m, 2).has_value());

  std::map<int, std::map<int, int>> m2{{1, {{2, 3}}}};
  EXPECT_TRUE(get_optional<std::optional>(m2, 1, 2).has_value());
  EXPECT_EQ(3, get_optional<std::optional>(m2, 1, 2).value());
  EXPECT_FALSE(get_optional<std::optional>(m2, 1, 3).has_value());
}

TEST(MapUtil, getRefDefault) {
  std::map<int, int> m;
  m[1] = 2;
  const int i = 42;
  EXPECT_EQ(2, get_ref_default(m, 1, i));
  EXPECT_EQ(42, get_ref_default(m, 2, i));
  EXPECT_EQ(std::addressof(i), std::addressof(get_ref_default(m, 2, i)));
}

TEST(MapUtil, getRefDefaultFunction) {
  std::map<int, int> m;
  m[1] = 2;
  const int i = 42;
  EXPECT_EQ(2, get_ref_default(m, 1, [&i]() -> const int& { return i; }));
  EXPECT_EQ(42, get_ref_default(m, 2, [&i]() -> const int& { return i; }));
  EXPECT_EQ(
      std::addressof(i),
      std::addressof(get_ref_default(m, 2, [&i]() -> const int& {
        return i;
      })));
  // statically disallowed:
  // get_ref_default(m, 2, [] { return 7; });
}

TEST(MapUtil, getPtr) {
  std::map<int, int> m;
  m[1] = 2;
  EXPECT_EQ(2, *get_ptr(m, 1));
  EXPECT_TRUE(get_ptr(m, 2) == nullptr);
  *get_ptr(m, 1) = 4;
  EXPECT_EQ(4, m.at(1));
  EXPECT_EQ(4, *get_ptr(&m, 1));

  std::map<int, int>* nullMap = nullptr;
  EXPECT_EQ(nullptr, get_ptr(nullMap, 2));
}

namespace {
template <typename Map, typename = void>
struct GetPtrCompiles : std::false_type {};

template <typename Map>
struct GetPtrCompiles<
    Map,
    std::void_t<decltype(get_ptr(
        std::declval<Map>(),
        std::declval<typename std::remove_cvref_t<Map>::key_type>()))>>
    : std::true_type {};
} // namespace

TEST(MapUtil, getPtrTemporary) {
  // get_ptr returns a pointer into the map, so calling it on a temporary
  // (rvalue) map is disallowed to prevent a dangling pointer. lvalue maps
  // (const or mutable) remain valid.
  EXPECT_TRUE((GetPtrCompiles<std::map<int, std::string>&>::value));
  EXPECT_TRUE((GetPtrCompiles<const std::map<int, std::string>&>::value));
  EXPECT_FALSE((GetPtrCompiles<std::map<int, std::string>&&>::value));
}

TEST(MapUtil, getPtr2) {
  folly::sorted_vector_map<int, int> m;
  m[1] = 7;
  m[4] = 9;
  auto r0 = get_ptr2(m, 1, 4);
  EXPECT_EQ(7, *r0.first);
  EXPECT_EQ(9, *r0.second);
  auto r1 = get_ptr2(m, 1, 5);
  EXPECT_EQ(7, *r1.first);
  EXPECT_EQ(nullptr, r1.second);
  auto r2 = get_ptr2(m, 2, 4);
  EXPECT_EQ(nullptr, r2.first);
  EXPECT_EQ(9, *r2.second);
  auto r3 = get_ptr2(m, 2, 5);
  EXPECT_EQ(nullptr, r3.first);
  EXPECT_EQ(nullptr, r3.second);
}

TEST(MapUtil, getPtrReferenceValueType) {
  std::map<int, const std::string&> testMap;
  EXPECT_EQ(nullptr, folly::get_ptr(testMap, 3));

  std::string someString = "some";
  testMap.emplace(3, someString);
  EXPECT_EQ(&someString, folly::get_ptr(testMap, 3));
}

TEST(MapUtil, getPtrPathSimple) {
  using std::map;
  map<int, map<int, map<int, map<int, int>>>> m{{1, {{2, {{3, {{4, 5}}}}}}}};
  EXPECT_EQ(5, *get_ptr(m, 1, 2, 3, 4));
  EXPECT_TRUE(get_ptr(m, 1, 2, 3, 4));
  EXPECT_FALSE(get_ptr(m, 1, 2, 3, 0));
  EXPECT_TRUE(get_ptr(m, 1, 2, 3));
  EXPECT_FALSE(get_ptr(m, 1, 2, 0));
  EXPECT_TRUE(get_ptr(m, 1, 2));
  EXPECT_FALSE(get_ptr(m, 1, 0));
  EXPECT_TRUE(get_ptr(m, 1));
  EXPECT_FALSE(get_ptr(m, 0));
  const auto& cm = m;
  ++*get_ptr(m, 1, 2, 3, 4);
  EXPECT_EQ(6, *get_ptr(cm, 1, 2, 3, 4));
  EXPECT_TRUE(get_ptr(cm, 1, 2, 3, 4));
  EXPECT_FALSE(get_ptr(cm, 1, 2, 3, 0));

  EXPECT_EQ(6, *get_ptr(&cm, 1, 2, 3, 4));

  map<int, map<int, map<int, map<int, int>>>>* nullMap = nullptr;
  EXPECT_EQ(nullptr, get_ptr(nullMap, 1, 2, 3, 4));
}

TEST(MapUtil, getPtrPathMixed) {
  using std::map;
  using std::string;
  using std::unordered_map;
  unordered_map<string, map<int, map<string, int>>> m{{"a", {{1, {{"b", 7}}}}}};
  EXPECT_EQ(7, *get_ptr(m, "a", 1, "b"));
  EXPECT_TRUE(get_ptr(m, "a", 1, "b"));
  EXPECT_FALSE(get_ptr(m, "b", 1, "b"));
  EXPECT_FALSE(get_ptr(m, "a", 2, "b"));
  EXPECT_FALSE(get_ptr(m, "a", 1, "c"));
  EXPECT_TRUE(get_ptr(m, "a", 1));
  EXPECT_TRUE(get_ptr(m, "a"));
  const auto& cm = m;
  ++*get_ptr(m, "a", 1, "b");
  EXPECT_EQ(8, *get_ptr(cm, "a", 1, "b"));
  EXPECT_TRUE(get_ptr(cm, "a", 1, "b"));
  EXPECT_FALSE(get_ptr(cm, "b", 1, "b"));
}

namespace {
template <typename T>
struct element_type {
  using type = typename std::decay<T>::type;
};

template <typename T>
struct element_type<T()> {
  using type = T;
};

template <typename T>
using element_type_t = typename element_type<T>::type;

template <typename T, typename = void>
struct Compiles : std::false_type {};

template <typename T>
struct Compiles<
    T,
    std::void_t<decltype(get_ref_default(
        std::declval<const std::map<int, element_type_t<T>>&>(),
        std::declval<int>(),
        std::declval<T>()))>> : std::true_type {};
} // namespace

TEST(MapUtil, getDefaultTemporary) {
  EXPECT_TRUE(Compiles<const int&>::value);
  EXPECT_TRUE(Compiles<int&>::value);
  EXPECT_FALSE(Compiles<const int&&>::value);
  EXPECT_FALSE(Compiles<int&&>::value);

  EXPECT_TRUE(Compiles<const int&()>::value);
  EXPECT_TRUE(Compiles<int&()>::value);
  EXPECT_FALSE(Compiles<int()>::value);
}

TEST(MapUtil, getDefaultPath) {
  using std::map;
  map<int, map<int, int>> m;
  m[4][2] = 42;
  EXPECT_EQ(42, get_default(m, 4, 2, 42));
  EXPECT_EQ(42, get_default(m, 1, 3, 42));
}

TEST(MapUtil, getDefaultPathMixed) {
  using std::map;
  using std::string;
  using std::unordered_map;
  map<int, unordered_map<string, StringPiece>> m;
  int key1 = 42;
  const string key2 = "hello";
  constexpr StringPiece value = "world";
  constexpr StringPiece dflt = "default";
  m[key1][key2] = value;
  EXPECT_EQ(value, get_default(m, 42, key2, dflt));
  EXPECT_EQ(value, get_default(m, key1, "hello", dflt));
  EXPECT_EQ(dflt, get_default(m, 0, key2, dflt));
  EXPECT_EQ(dflt, get_default(m, key1, "bad", "default"));
}

TEST(MapUtil, getRefDefaultPath) {
  using std::map;
  map<int, map<int, int>> m;
  m[4][2] = 42;
  const int dflt = 13;
  EXPECT_EQ(42, get_ref_default(m, 4, 2, dflt));
  EXPECT_EQ(dflt, get_ref_default(m, 1, 3, dflt));
}

TEST(MapUtil, getRefDefaultPathMixed) {
  using std::map;
  using std::string;
  using std::unordered_map;
  map<int, unordered_map<string, StringPiece>> m;
  int key1 = 42;
  const string key2 = "hello";
  constexpr StringPiece value = "world";
  constexpr StringPiece dflt = "default";
  m[key1][key2] = value;
  EXPECT_EQ(value, get_ref_default(m, 42, key2, dflt));
  EXPECT_EQ(value, get_ref_default(m, key1, "hello", dflt));
  EXPECT_EQ(dflt, get_ref_default(m, 0, key2, dflt));
  EXPECT_EQ(dflt, get_ref_default(m, key1, "bad", dflt));
}

namespace {
template <typename T, typename = void>
struct GetRefDefaultPathCompiles : std::false_type {};

template <typename T>
struct GetRefDefaultPathCompiles<
    T,
    std::void_t<decltype(get_ref_default(
        std::declval<const std::map<int, std::map<int, element_type_t<T>>>&>(),
        std::declval<int>(),
        std::declval<int>(),
        std::declval<T>()))>> : std::true_type {};
} // namespace

TEST(MapUtil, getRefDefaultPathTemporary) {
  EXPECT_TRUE(GetRefDefaultPathCompiles<const int&>::value);
  EXPECT_TRUE(GetRefDefaultPathCompiles<int&>::value);
  EXPECT_FALSE(GetRefDefaultPathCompiles<const int&&>::value);
  EXPECT_FALSE(GetRefDefaultPathCompiles<int&&>::value);
}

namespace {

class TestConstruction {
 public:
  [[maybe_unused]] TestConstruction() { EXPECT_TRUE(false); }
  [[maybe_unused]] TestConstruction(TestConstruction&&) { EXPECT_TRUE(false); }
  TestConstruction(const TestConstruction&) { EXPECT_TRUE(false); }

  explicit TestConstruction(std::string&& string)
      : string_{std::move(string)} {}
  explicit TestConstruction(int&& integer) : integer_{integer} {}

  TestConstruction& operator=(const TestConstruction&) = delete;
  TestConstruction& operator=(TestConstruction&&) = delete;

  int integer_{};
  std::string string_{};
};

} // namespace

TEST(MapUtil, testGetDefaultDeferredConstruction) {
  auto map = std::unordered_map<int, TestConstruction>{};
  map.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(1),
      std::forward_as_tuple(1));

  EXPECT_EQ(map.at(1).integer_, 1);

  {
    auto val = get_default(map, 0, 1);
    EXPECT_EQ(val.integer_, 1);
    EXPECT_EQ(val.string_, "");
  }

  {
    auto val = get_default(map, 0, "something");
    EXPECT_EQ(val.integer_, 0);
    EXPECT_EQ(val.string_, "something");
  }
}
