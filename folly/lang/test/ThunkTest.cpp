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

#include <folly/lang/Thunk.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include <folly/Traits.h>
#include <folly/portability/GTest.h>

namespace folly::detail {

class ThunkTest : public testing::Test {};

TEST_F(ThunkTest, make_ruin) {
  auto make0 = thunk::make<std::string>;
  auto make1 = thunk::make<std::string, std::string_view>;
  auto makeC = thunk::make_copy<std::string>;
  auto makeM = thunk::make_move<std::string>;
  auto ruin = thunk::ruin<std::string>;

  {
    auto str = make0();
    EXPECT_EQ("", *(std::string*)str);
    ruin(str);
  }
  {
    auto str = make1("blargh");
    EXPECT_EQ("blargh", *(std::string*)str);
    ruin(str);
  }
  {
    std::string const src{"blargh"};
    auto str = makeC((void const*)&src);
    EXPECT_EQ("blargh", *(std::string*)str);
    ruin(str);
    EXPECT_EQ("blargh", src);
  }
  {
    std::string src{"blargh blargh blargh blargh"};
    auto str = makeM((void*)&src);
    EXPECT_EQ("blargh blargh blargh blargh", *(std::string*)str);
    ruin(str);
    EXPECT_EQ("", src);
  }
}

TEST_F(ThunkTest, ctor_dtor) {
  auto ctor0 = thunk::ctor<std::string>;
  auto ctor1 = thunk::ctor<std::string, std::string_view>;
  auto ctorC = thunk::ctor_copy<std::string>;
  auto ctorM = thunk::ctor_move<std::string>;
  auto dtor = thunk::dtor<std::string>;
  aligned_storage_for_t<std::string> buf;

  {
    auto str = ctor0(&buf);
    EXPECT_EQ("", *(std::string*)str);
    dtor(str);
  }
  {
    auto str = ctor1(&buf, "blargh");
    EXPECT_EQ("blargh", *(std::string*)str);
    dtor(str);
  }
  {
    std::string const src{"blargh"};
    auto str = ctorC(&buf, (void const*)&src);
    EXPECT_EQ("blargh", *(std::string*)str);
    dtor(str);
    EXPECT_EQ("blargh", src);
  }
  {
    std::string src{"blargh blargh blargh blargh"};
    auto str = ctorM(&buf, (void*)&src);
    EXPECT_EQ("blargh blargh blargh blargh", *(std::string*)str);
    dtor(str);
    EXPECT_EQ("", src);
  }
}

namespace {
struct CtorExceptionSpec {
  int value = 0;

  CtorExceptionSpec() noexcept = default;
  explicit CtorExceptionSpec(int v) noexcept : value(v) {}
  explicit CtorExceptionSpec(std::string_view input) {
    if (input == "failure") {
      throw std::runtime_error("ctor");
    }
  }
};
} // namespace

TEST_F(ThunkTest, ctor_exception_spec) {
  static_assert(noexcept(thunk::ctor<CtorExceptionSpec>(nullptr)));
  static_assert(noexcept(thunk::ctor<CtorExceptionSpec, int>(nullptr, 7)));
  static_assert(!noexcept(
      thunk::ctor<CtorExceptionSpec, std::string_view>(nullptr, "failure")));
  static_assert(
      std::is_same_v<
          decltype(&thunk::ctor<CtorExceptionSpec>),
          void* (*)(void*) noexcept>);

  aligned_storage_for_t<CtorExceptionSpec> buf;
  auto* defaultValue =
      static_cast<CtorExceptionSpec*>(thunk::ctor<CtorExceptionSpec>(&buf));
  EXPECT_EQ(&buf, static_cast<void*>(defaultValue));
  EXPECT_EQ(0, defaultValue->value);
  thunk::dtor<CtorExceptionSpec>(defaultValue);

  auto ctor = &thunk::ctor<CtorExceptionSpec, int>;
  auto* value = static_cast<CtorExceptionSpec*>(ctor(&buf, 7));
  EXPECT_EQ(&buf, static_cast<void*>(value));
  EXPECT_EQ(7, value->value);
  thunk::dtor<CtorExceptionSpec>(value);

  auto* successValue = static_cast<CtorExceptionSpec*>(
      thunk::ctor<CtorExceptionSpec, std::string_view>(&buf, "success"));
  EXPECT_EQ(&buf, static_cast<void*>(successValue));
  EXPECT_EQ(0, successValue->value);
  thunk::dtor<CtorExceptionSpec>(successValue);

  EXPECT_THROW(
      (thunk::ctor<CtorExceptionSpec, std::string_view>(&buf, "failure")),
      std::runtime_error);
}

TEST_F(ThunkTest, operator_new_delete) {
  auto ptr = thunk::operator_new<64, 32>();
  EXPECT_NE(nullptr, ptr);
  thunk::operator_delete<64, 32>(ptr);
  ptr = thunk::operator_new_nx<64, 32>();
  EXPECT_NE(nullptr, ptr);
  thunk::operator_delete<64, 32>(ptr);
}

} // namespace folly::detail
