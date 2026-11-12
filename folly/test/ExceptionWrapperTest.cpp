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

#include <folly/ExceptionWrapper.h>

#include <memory>
#include <stdexcept>

#include <folly/Conv.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

using namespace folly;
using namespace ::testing;

class AbstractIntException : public std::exception {
 public:
  virtual int getInt() const = 0;
};

class IntException : public AbstractIntException {
 public:
  explicit IntException(int i) : i_(i), what_(to<std::string>("int == ", i_)) {}

  int getInt() const override { return i_; }
  const char* what() const noexcept override { return what_.c_str(); }

 private:
  int i_;
  std::string what_;
};

const static std::string kExceptionClassName =
    demangle(typeid(std::exception)).toStdString();
const static std::string kRuntimeErrorClassName =
    demangle(typeid(std::runtime_error)).toStdString();
const static std::string kIntExceptionClassName =
    demangle(typeid(IntException)).toStdString();
const static std::string kIntClassName = demangle(typeid(int)).toStdString();

TEST(ExceptionWrapper, nothrow) {
  EXPECT_TRUE(std::is_nothrow_default_constructible<exception_wrapper>::value);
  EXPECT_TRUE(std::is_nothrow_move_constructible<exception_wrapper>::value);
  EXPECT_TRUE(std::is_nothrow_move_assignable<exception_wrapper>::value);
  EXPECT_TRUE(std::is_nothrow_copy_constructible<exception_wrapper>::value);
  EXPECT_TRUE(std::is_nothrow_copy_assignable<exception_wrapper>::value);
}

namespace {
struct NonStdException {};
struct NonCopyableException : std::exception {
  NonCopyableException() = default;
  NonCopyableException(const NonCopyableException&) = delete;
};
class WrapperConstructibleException : public std::runtime_error {
 public:
  explicit WrapperConstructibleException(const std::string& message = "")
      : std::runtime_error(message) {}
  explicit WrapperConstructibleException(const exception_wrapper& ew)
      : std::runtime_error(ew.what().toStdString()) {}
};
class DerivedWrapperConstructibleException
    : public WrapperConstructibleException {
 public:
  explicit DerivedWrapperConstructibleException(const std::string& message = "")
      : WrapperConstructibleException(message) {}
  explicit DerivedWrapperConstructibleException(const exception_wrapper& ew)
      : WrapperConstructibleException(ew) {}
};
} // namespace

TEST(ExceptionWrapper, constructorConstraints) {
  static_assert(std::is_convertible_v<std::runtime_error, exception_wrapper>);
  static_assert(
      std::is_convertible_v<const std::runtime_error&, exception_wrapper>);
  static_assert(!std::is_constructible_v<exception_wrapper, NonStdException>);
  static_assert(
      std::is_constructible_v<
          exception_wrapper,
          std::in_place_t,
          NonStdException>);
  static_assert(
      std::is_constructible_v<
          exception_wrapper,
          std::in_place_type_t<int>,
          int>);
  static_assert(
      !std::is_constructible_v<exception_wrapper, AbstractIntException&>);
  static_assert(
      !std::is_constructible_v<exception_wrapper, NonCopyableException>);
  static_assert(
      !std::is_constructible_v<
          exception_wrapper,
          std::in_place_t,
          NonCopyableException>);
  static_assert(
      !std::is_constructible_v<
          exception_wrapper,
          std::in_place_type_t<NonCopyableException>>);
  static_assert(
      !std::is_constructible_v<
          exception_wrapper,
          std::in_place_t,
          exception_wrapper>);
}

TEST(ExceptionWrapper, exceptionConstructibleFromWrapper) {
  auto ep = std::make_exception_ptr(DerivedWrapperConstructibleException());
  exception_wrapper ew{ep};
  EXPECT_TRUE(ew.is_compatible_with<DerivedWrapperConstructibleException>());
}

// Tests that when we call throw_exception, the proper type is thrown (derived)
TEST(ExceptionWrapper, throwTest) {
  std::runtime_error e("payload");
  auto ew = make_exception_wrapper<std::runtime_error>(e);

  std::vector<exception_wrapper> container;
  container.push_back(ew);

  EXPECT_THAT(
      [&]() { container[0].throw_exception(); },
      ThrowsMessage<std::runtime_error>(StrEq("payload")));
}

// Tests that when we call throw_with_nested, we can unnest it later.
TEST(ExceptionWrapper, throwWithNested) {
  auto ew = make_exception_wrapper<std::runtime_error>("inner");
  try {
    ew.throw_with_nested(std::runtime_error("outer"));
    // throw_with_nested is marked [[noreturn]] so an exception is guaranteed.
  } catch (std::runtime_error& outer) {
    EXPECT_STREQ(outer.what(), "outer");

    EXPECT_THAT(
        [&]() { std::rethrow_if_nested(outer); },
        ThrowsMessage<std::runtime_error>(StrEq("inner")));
  }
}

TEST(ExceptionWrapper, members) {
  auto ew = exception_wrapper();
  EXPECT_FALSE(bool(ew));
  EXPECT_EQ(ew.what(), "");
  EXPECT_EQ(ew.class_name(), "");
  EXPECT_EQ(nullptr, std::as_const(ew).to_exception_ptr());
  EXPECT_EQ(nullptr, ew.to_exception_ptr());
  ew = make_exception_wrapper<std::runtime_error>("payload");
  EXPECT_TRUE(bool(ew));
  EXPECT_EQ(ew.what(), kRuntimeErrorClassName + ": payload");
  EXPECT_EQ(ew.class_name(), kRuntimeErrorClassName);
}

TEST(ExceptionWrapper, equality) {
  auto ew = make_exception_wrapper<std::runtime_error>("wat");
  EXPECT_EQ(ew, ew);

  auto ewSame = ew;
  EXPECT_EQ(ew, ewSame);

  auto ewDiff = make_exception_wrapper<std::runtime_error>("wat");
  EXPECT_NE(ewDiff, ew);
  EXPECT_NE(ewDiff, ewSame);
}

TEST(ExceptionWrapper, tryAndCatchTest) {
  auto ew4 = try_and_catch([] { throw 17; });
  EXPECT_TRUE(bool(ew4));
  EXPECT_TRUE(ew4.is_compatible_with<int>());
}

TEST(ExceptionWrapper, withExceptionTest) {
  int expected = 23;

  auto ew = exception_wrapper{std::make_exception_ptr(IntException(expected))};
  EXPECT_TRUE(bool(ew));
  EXPECT_EQ(ew.what(), kIntExceptionClassName + ": int == 23");
  EXPECT_EQ(ew.class_name(), kIntExceptionClassName);
  EXPECT_TRUE(ew.with_exception([&](const IntException& ie) {
    EXPECT_EQ(ie.getInt(), expected);
  }));

  // Test with const this.  If this compiles and does not crash due to
  // infinite loop when it runs, it succeeds.
  const exception_wrapper& cew = ew;
  EXPECT_TRUE(cew.with_exception([&](const IntException& /* ie */) {
    SUCCEED();
  }));

  // Test with empty ew.
  exception_wrapper empty_ew;
  EXPECT_FALSE(empty_ew.with_exception([&](const std::exception& /* ie */) {
    FAIL();
  }));

  // Testing with const exception_wrapper; sanity check first:
  EXPECT_FALSE(cew.with_exception([&](const std::runtime_error&) {}));
  EXPECT_FALSE(cew.with_exception([&](const int&) {}));
  // This won't even compile.  You can't use a function which takes a
  // non-const reference with a const exception_wrapper.
  /*
  EXPECT_FALSE(cew.with_exception([&](std::runtime_error&) {}));
  EXPECT_FALSE(cew.with_exception([&](int&) {}));
  */

  // a move-only callback type:
  cew.with_exception([v = std::make_unique<int>(7)](const std::exception&) {});
}

TEST(ExceptionWrapper, getOrMakeExceptionPtrTest) {
  int expected = 23;

  auto ew = exception_wrapper{std::make_exception_ptr(IntException(expected))};
  std::exception_ptr eptr = ew.to_exception_ptr();
  EXPECT_THROW(std::rethrow_exception(eptr), IntException);

  // Test with const this.
  const exception_wrapper& cew = ew;
  eptr = cew.to_exception_ptr();
  EXPECT_THROW(std::rethrow_exception(eptr), IntException);

  // Test with empty ew.
  exception_wrapper empty_ew;
  eptr = empty_ew.to_exception_ptr();
  EXPECT_FALSE(eptr);
}

TEST(ExceptionWrapper, fromExceptionPtrEmpty) {
  auto ep = std::exception_ptr();
  auto ew = exception_wrapper{ep};
  EXPECT_FALSE(bool(ew));
}

TEST(ExceptionWrapper, fromExceptionPtrExn) {
  auto ep = std::make_exception_ptr(std::runtime_error("foo"));
  auto ew = exception_wrapper{ep};
  EXPECT_TRUE(bool(ew));
  EXPECT_EQ(ep, std::as_const(ew).to_exception_ptr());
  EXPECT_EQ(ep, ew.to_exception_ptr());
  EXPECT_TRUE(ew.is_compatible_with<std::runtime_error>());
}

TEST(ExceptionWrapper, fromExceptionPtrAny) {
  auto ep = std::make_exception_ptr<int>(12);
  auto ew = exception_wrapper{ep};
  EXPECT_TRUE(bool(ew));
  EXPECT_EQ(ep, std::as_const(ew).to_exception_ptr());
  EXPECT_EQ(ep, ew.to_exception_ptr());
  EXPECT_TRUE(ew.is_compatible_with<int>());
}

TEST(ExceptionWrapper, withExceptionPtrEmpty) {
  auto ew = exception_wrapper(std::exception_ptr());
  EXPECT_EQ(nullptr, ew.type());
  EXPECT_FALSE(bool(ew));
  EXPECT_EQ(nullptr, ew.get_exception());
  EXPECT_EQ(nullptr, ew.get_exception<std::exception>());
  EXPECT_EQ(nullptr, ew.get_exception<int>());
  EXPECT_FALSE(ew.has_exception_ptr());
  EXPECT_EQ(nullptr, std::as_const(ew).to_exception_ptr());
  EXPECT_FALSE(ew.has_exception_ptr());
  EXPECT_EQ(nullptr, ew.to_exception_ptr());
  EXPECT_FALSE(ew.has_exception_ptr());
  EXPECT_EQ("", ew.class_name());
  EXPECT_EQ("", ew.what());
  EXPECT_FALSE(ew.is_compatible_with<std::exception>());
  EXPECT_FALSE(ew.is_compatible_with<int>());
  EXPECT_DEATH(ew.throw_exception(), "empty folly::exception_wrapper");
}

TEST(ExceptionWrapper, withSharedPtrTest) {
  auto ew = exception_wrapper(std::runtime_error("foo"));
  EXPECT_TRUE(bool(ew));
  EXPECT_EQ(&typeid(std::runtime_error), ew.type());
  EXPECT_NE(nullptr, ew.get_exception());
  EXPECT_NE(nullptr, ew.get_exception<std::exception>());
  EXPECT_STREQ("foo", ew.get_exception<std::exception>()->what());
  EXPECT_EQ(nullptr, ew.get_exception<int>());
  EXPECT_TRUE(ew.has_exception_ptr());
  EXPECT_NE(nullptr, std::as_const(ew).to_exception_ptr());
  EXPECT_TRUE(ew.has_exception_ptr());
  EXPECT_NE(nullptr, ew.to_exception_ptr());
  EXPECT_TRUE(ew.has_exception_ptr());
  EXPECT_EQ(kRuntimeErrorClassName, ew.class_name());
  EXPECT_EQ(kRuntimeErrorClassName + ": foo", ew.what());
  EXPECT_TRUE(ew.is_compatible_with<std::exception>());
  EXPECT_TRUE(ew.is_compatible_with<std::runtime_error>());
  EXPECT_FALSE(ew.is_compatible_with<int>());
  EXPECT_THROW(ew.throw_exception(), std::runtime_error);

  exception_wrapper(std::move(ew));
  EXPECT_FALSE(bool(ew));
  EXPECT_EQ(nullptr, ew.type());
  EXPECT_EQ(nullptr, ew.get_exception());
  EXPECT_EQ(nullptr, ew.get_exception<std::exception>());
  EXPECT_EQ(nullptr, ew.get_exception<int>());
  EXPECT_EQ(nullptr, std::as_const(ew).to_exception_ptr());
  EXPECT_EQ(nullptr, ew.to_exception_ptr());
  EXPECT_EQ("", ew.class_name());
  EXPECT_EQ("", ew.what());
  EXPECT_FALSE(ew.is_compatible_with<std::exception>());
  EXPECT_FALSE(ew.is_compatible_with<std::runtime_error>());
  EXPECT_FALSE(ew.is_compatible_with<int>());
}

TEST(ExceptionWrapper, withExceptionPtrExnTest) {
  auto ep = std::make_exception_ptr(std::runtime_error("foo"));
  auto ew = exception_wrapper(ep);
  EXPECT_TRUE(bool(ew));
  EXPECT_EQ(&typeid(std::runtime_error), ew.type());
  EXPECT_NE(nullptr, ew.get_exception());
  EXPECT_NE(nullptr, ew.get_exception<std::exception>());
  EXPECT_STREQ("foo", ew.get_exception<std::exception>()->what());
  EXPECT_EQ(nullptr, ew.get_exception<int>());
  EXPECT_TRUE(ew.has_exception_ptr());
  EXPECT_EQ(ep, ew.to_exception_ptr());
  EXPECT_TRUE(ew.has_exception_ptr());
  EXPECT_EQ(kRuntimeErrorClassName, ew.class_name());
  EXPECT_EQ(kRuntimeErrorClassName + ": foo", ew.what());
  EXPECT_TRUE(ew.is_compatible_with<std::exception>());
  EXPECT_TRUE(ew.is_compatible_with<std::runtime_error>());
  EXPECT_FALSE(ew.is_compatible_with<int>());
  EXPECT_THROW(ew.throw_exception(), std::runtime_error);

  exception_wrapper(std::move(ew));
  EXPECT_FALSE(bool(ew));
  EXPECT_EQ(nullptr, ew.type());
  EXPECT_EQ(nullptr, ew.get_exception());
  EXPECT_EQ(nullptr, ew.get_exception<std::exception>());
  EXPECT_EQ(nullptr, ew.get_exception<int>());
  EXPECT_EQ(nullptr, std::as_const(ew).to_exception_ptr());
  EXPECT_EQ(nullptr, ew.to_exception_ptr());
  EXPECT_EQ("", ew.class_name());
  EXPECT_EQ("", ew.what());
  EXPECT_FALSE(ew.is_compatible_with<std::exception>());
  EXPECT_FALSE(ew.is_compatible_with<std::runtime_error>());
  EXPECT_FALSE(ew.is_compatible_with<int>());
}

TEST(ExceptionWrapper, withExceptionPtrAnyTest) {
  auto ep = std::make_exception_ptr<int>(12);
  auto ew = exception_wrapper(ep);
  EXPECT_TRUE(bool(ew));
  EXPECT_EQ(nullptr, ew.get_exception());
  EXPECT_EQ(nullptr, ew.get_exception<std::exception>());
  EXPECT_NE(nullptr, ew.get_exception<int>());
  EXPECT_EQ(12, *ew.get_exception<int>());
  EXPECT_TRUE(ew.has_exception_ptr());
  EXPECT_EQ(ep, std::as_const(ew).to_exception_ptr());
  EXPECT_EQ(ep, ew.to_exception_ptr());
  EXPECT_TRUE(ew.has_exception_ptr());
  EXPECT_EQ(demangle(typeid(int)), ew.class_name());
  EXPECT_EQ(demangle(typeid(int)), ew.what());
  EXPECT_FALSE(ew.is_compatible_with<std::exception>());
  EXPECT_FALSE(ew.is_compatible_with<std::runtime_error>());
  EXPECT_TRUE(ew.is_compatible_with<int>());
  EXPECT_THROW(ew.throw_exception(), int);

  exception_wrapper(std::move(ew));
  EXPECT_FALSE(bool(ew));
  EXPECT_EQ(nullptr, ew.get_exception());
  EXPECT_EQ(nullptr, ew.get_exception<std::exception>());
  EXPECT_EQ(nullptr, ew.get_exception<int>());
  EXPECT_EQ(nullptr, std::as_const(ew).to_exception_ptr());
  EXPECT_EQ(nullptr, ew.to_exception_ptr());
  EXPECT_FALSE(ew.has_exception_ptr());
  EXPECT_EQ("", ew.class_name());
  EXPECT_EQ("", ew.what());
  EXPECT_FALSE(ew.is_compatible_with<std::exception>());
  EXPECT_FALSE(ew.is_compatible_with<std::runtime_error>());
  EXPECT_FALSE(ew.is_compatible_with<int>());
}

TEST(ExceptionWrapper, withNonStdExceptionTest) {
  auto ew = exception_wrapper(std::in_place, 42);
  EXPECT_TRUE(bool(ew));
  EXPECT_EQ(nullptr, ew.get_exception());
  EXPECT_EQ(nullptr, ew.get_exception<std::exception>());
  EXPECT_NE(nullptr, ew.get_exception<int>());
  EXPECT_EQ(42, *ew.get_exception<int>());
  EXPECT_TRUE(ew.has_exception_ptr());
  EXPECT_EQ(demangle(typeid(int)), ew.class_name());
  EXPECT_EQ(demangle(typeid(int)), ew.what());
  EXPECT_NE(nullptr, std::as_const(ew).to_exception_ptr());
  EXPECT_NE(nullptr, ew.to_exception_ptr());
  EXPECT_TRUE(ew.has_exception_ptr());
  EXPECT_EQ(demangle(typeid(int)), ew.class_name());
  EXPECT_EQ(demangle(typeid(int)), ew.what());
  EXPECT_FALSE(ew.is_compatible_with<std::exception>());
  EXPECT_FALSE(ew.is_compatible_with<std::runtime_error>());
  EXPECT_TRUE(ew.is_compatible_with<int>());
  EXPECT_THROW(ew.throw_exception(), int);

  exception_wrapper(std::move(ew));
  EXPECT_FALSE(bool(ew));
  EXPECT_EQ(nullptr, ew.get_exception());
  EXPECT_EQ(nullptr, ew.get_exception<std::exception>());
  EXPECT_EQ(nullptr, ew.get_exception<int>());
  EXPECT_EQ(nullptr, std::as_const(ew).to_exception_ptr());
  EXPECT_EQ(nullptr, ew.to_exception_ptr());
  EXPECT_FALSE(ew.has_exception_ptr());
  EXPECT_EQ("", ew.class_name());
  EXPECT_EQ("", ew.what());
  EXPECT_FALSE(ew.is_compatible_with<std::exception>());
  EXPECT_FALSE(ew.is_compatible_with<std::runtime_error>());
  EXPECT_FALSE(ew.is_compatible_with<int>());
}

TEST(ExceptionWrapper, withExceptionPtrAnyNilTest) {
  auto ep = std::make_exception_ptr<int>(12);
  auto ew = exception_wrapper(ep);
  EXPECT_TRUE(bool(ew));
  EXPECT_EQ(nullptr, ew.get_exception());
  EXPECT_EQ(nullptr, ew.get_exception<std::exception>());
  EXPECT_NE(nullptr, ew.get_exception<int>());
  EXPECT_EQ(12, *ew.get_exception<int>());
  EXPECT_EQ(ep, std::as_const(ew).to_exception_ptr());
  EXPECT_EQ(ep, ew.to_exception_ptr());
  EXPECT_EQ("int", ew.class_name());
  EXPECT_EQ("int", ew.what());
  EXPECT_FALSE(ew.is_compatible_with<std::exception>());
  EXPECT_FALSE(ew.is_compatible_with<std::runtime_error>());
  EXPECT_TRUE(ew.is_compatible_with<int>());
  EXPECT_THROW(ew.throw_exception(), int);

  exception_wrapper(std::move(ew));
  EXPECT_FALSE(bool(ew));
  EXPECT_EQ(nullptr, ew.get_exception());
  EXPECT_EQ(nullptr, ew.get_exception<std::exception>());
  EXPECT_EQ(nullptr, ew.get_exception<int>());
  EXPECT_EQ(nullptr, std::as_const(ew).to_exception_ptr());
  EXPECT_EQ(nullptr, ew.to_exception_ptr());
  EXPECT_EQ("", ew.class_name());
  EXPECT_EQ("", ew.what());
  EXPECT_FALSE(ew.is_compatible_with<std::exception>());
  EXPECT_FALSE(ew.is_compatible_with<std::runtime_error>());
  EXPECT_FALSE(ew.is_compatible_with<int>());
}

TEST(ExceptionWrapper, get_exception_from_exception_wrapper) {
  static_assert(
      std::is_invocable_v<
          get_exception_fn<std::exception>,
          const exception_wrapper&>);
  static_assert(
      std::is_invocable_v<
          get_mutable_exception_fn<std::exception>,
          exception_wrapper&>);
  static_assert(
      !std::is_invocable_v<
          get_mutable_exception_fn<std::exception>,
          const exception_wrapper&>);

  // Unsafe to extract a pointer out of rvalues
  static_assert(
      !std::is_invocable_v<
          get_exception_fn<std::exception>,
          const exception_wrapper&&>);
  static_assert(
      !std::is_invocable_v<
          get_mutable_exception_fn<std::exception>,
          exception_wrapper&&>);

  auto ew = make_exception_wrapper<std::runtime_error>("foo");

  EXPECT_EQ(nullptr, get_exception<std::system_error>(ew));
  EXPECT_EQ(nullptr, get_mutable_exception<std::system_error>(ew));

  EXPECT_STREQ("foo", get_exception<std::exception>(ew)->what());
  EXPECT_STREQ("foo", get_exception<const std::exception>(ew)->what());
  EXPECT_STREQ("foo", get_exception<>(ew)->what());
  EXPECT_STREQ("foo", get_exception<>(std::as_const(ew))->what());
  EXPECT_STREQ("foo", get_mutable_exception<>(ew)->what());

  EXPECT_STREQ("foo", get_exception<std::runtime_error>(ew)->what());
  EXPECT_STREQ("foo", get_mutable_exception<std::runtime_error>(ew)->what());

  auto expected_p = ew.get_exception<std::runtime_error>();
  EXPECT_EQ(expected_p, get_exception<std::runtime_error>(ew));
  EXPECT_EQ(expected_p, get_mutable_exception<std::runtime_error>(ew));

  static_assert(
      std::is_same_v<
          const std::runtime_error*,
          decltype(get_exception<std::runtime_error>(ew))>);
  static_assert(
      std::is_same_v<
          const std::runtime_error*,
          decltype(get_exception<const std::runtime_error>(ew))>);
  static_assert(
      std::is_same_v<
          const std::runtime_error*,
          decltype(get_exception<std::runtime_error>(std::as_const(ew)))>);
  static_assert(
      std::is_same_v<
          std::runtime_error*,
          decltype(get_mutable_exception<std::runtime_error>(ew))>);
}

TEST(ExceptionWrapper, withExceptionDeductionExnConst) {
  auto ew = make_exception_wrapper<std::runtime_error>("hi");
  EXPECT_TRUE(ew.with_exception([](const std::runtime_error&) {}));
  EXPECT_TRUE(ew.with_exception([](const std::exception&) {}));
  EXPECT_FALSE(ew.with_exception([](const std::logic_error&) {}));
}

TEST(ExceptionWrapper, withExceptionDeductionWrapConstExnConst) {
  const auto cew = make_exception_wrapper<std::runtime_error>("hi");
  EXPECT_TRUE(cew.with_exception([](const std::runtime_error&) {}));
  EXPECT_TRUE(cew.with_exception([](const std::exception&) {}));
  EXPECT_FALSE(cew.with_exception([](const std::logic_error&) {}));
}

TEST(ExceptionWrapper, withExceptionDeductionReturning) {
  auto ew = make_exception_wrapper<std::runtime_error>("hi");
  EXPECT_TRUE(ew.with_exception([](const std::runtime_error&) { return 3; }));
  EXPECT_TRUE(ew.with_exception([](const std::exception&) { return "hello"; }));
  EXPECT_FALSE(ew.with_exception([](const std::logic_error&) {
    return nullptr;
  }));
}

namespace {
template <typename T>
T& r_to_l(T v) {
  return std::ref(v);
}
} // namespace

TEST(ExceptionWrapper, withExceptionDeductionFunctorLvalue) {
  auto ew = make_exception_wrapper<std::runtime_error>("hi");
  EXPECT_TRUE(ew.with_exception(r_to_l([](const std::runtime_error&) {})));
  EXPECT_TRUE(ew.with_exception(r_to_l([](const std::exception&) {})));
  EXPECT_FALSE(ew.with_exception(r_to_l([](const std::logic_error&) {})));
}

TEST(ExceptionWrapper, nonStdExceptionTest) {
  int expected = 17;

  auto ew = exception_wrapper{std::make_exception_ptr(expected)};
  EXPECT_TRUE(bool(ew));
  EXPECT_FALSE(ew.is_compatible_with<std::exception>());
  EXPECT_TRUE(ew.is_compatible_with<int>());
  EXPECT_EQ(ew.what(), kIntClassName);
  EXPECT_EQ(ew.class_name(), kIntClassName);
  // non-std::exception types are supported, but the only way to
  // access their value is to explicity rethrow and catch it.
  try {
    ew.throw_exception();
    // throw_exception is marked [[noreturn]] so an exception is guaranteed.
  } catch /* nolint */ (int& i) {
    EXPECT_EQ(i, expected);
  }
}

TEST(ExceptionWrapper, formatNoException) {
  exception_wrapper ew;
  auto expected = "";
  EXPECT_EQ(expected, ew.what());
  EXPECT_EQ(expected, exceptionStr(ew));
  EXPECT_EQ(expected, fmt::format("{}", ew));
}

TEST(ExceptionWrapper, formatStdException) {
  auto ew = make_exception_wrapper<std::runtime_error>("argh");
  auto expected = kRuntimeErrorClassName + ": argh";
  EXPECT_EQ(expected, ew.what());
  EXPECT_EQ(expected, exceptionStr(ew));
  EXPECT_EQ(expected, fmt::format("{}", ew));
}

TEST(ExceptionWrapper, formatNonStdException) {
  int inner = 17;
  auto ew = exception_wrapper{std::make_exception_ptr(inner)};
  EXPECT_EQ(kIntClassName, ew.what());
  EXPECT_EQ(kIntClassName, exceptionStr(ew));
  EXPECT_EQ(kIntClassName, fmt::format("{}", ew));
}

TEST(ExceptionWrapper, throwExceptionNoexception) {
  exception_wrapper ew;
  ASSERT_DEATH(ew.throw_exception(), "empty folly::exception_wrapper");
}

namespace {
class TestException : public std::exception {};
void testEW(const exception_wrapper& ew) {
  EXPECT_THROW(ew.throw_exception(), TestException);
}
} // namespace

TEST(ExceptionWrapper, implicitConstruction) {
  // Try with both lvalue and rvalue references
  TestException e;
  testEW(e);
  testEW(TestException());
}

namespace {
struct BaseNonStdException {
  virtual ~BaseNonStdException() {}
};
struct DerivedNonStdException : BaseNonStdException {};
} // namespace

TEST(ExceptionWrapper, baseDerivedNonStdExceptionTest) {
  exception_wrapper ew{std::make_exception_ptr(DerivedNonStdException{})};
  EXPECT_EQ(ew.type(), &typeid(DerivedNonStdException));
  EXPECT_TRUE(ew.with_exception([](const DerivedNonStdException&) {}));
}

namespace {
struct ThrownException {};
struct InSituException : std::exception {
  InSituException() = default;
  InSituException(const InSituException&) noexcept {}
};
struct OnHeapException : std::exception {
  OnHeapException() = default;
  OnHeapException(const OnHeapException&) {}
};
} // namespace

TEST(ExceptionWrapper, makeWrapperNoArgs) {
  EXPECT_THAT(
      folly::make_exception_wrapper<ThrownException>().class_name(),
      testing::Eq(demangle(typeid(ThrownException))));
  EXPECT_THAT(
      folly::make_exception_wrapper<InSituException>().class_name(),
      testing::Eq(demangle(typeid(InSituException))));
  EXPECT_THAT(
      folly::make_exception_wrapper<OnHeapException>().class_name(),
      testing::Eq(demangle(typeid(OnHeapException))));
}

namespace {
// Cannot be stored within an exception_wrapper
struct BigRuntimeError : std::runtime_error {
  using std::runtime_error::runtime_error;
  char data_[sizeof(exception_wrapper) + 1]{};
};

struct BigNonStdError {
  char data_[sizeof(exception_wrapper) + 1]{};
};
} // namespace

TEST(ExceptionWrapper, handleStdException) {
  auto ep = std::make_exception_ptr(std::runtime_error{"hello world"});
  exception_wrapper const ew_eptr(ep);
  exception_wrapper const ew_small(std::runtime_error{"hello world"});
  exception_wrapper const ew_big(BigRuntimeError{"hello world"});

  bool handled = false;
  auto expect_runtime_error_yes_catch_all = [&](const exception_wrapper& ew) {
    ew.handle(
        [](const std::logic_error&) { ADD_FAILURE(); },
        [&](const std::runtime_error&) noexcept { handled = true; },
        [](const std::exception&) { ADD_FAILURE(); },
        [](...) { ADD_FAILURE(); });
  };

  expect_runtime_error_yes_catch_all(ew_eptr);
  EXPECT_TRUE(handled);
  handled = false;
  expect_runtime_error_yes_catch_all(ew_small);
  EXPECT_TRUE(handled);
  handled = false;
  expect_runtime_error_yes_catch_all(ew_big);
  EXPECT_TRUE(handled);
  handled = false;

  auto expect_runtime_error_no_catch_all = [&](const exception_wrapper& ew) {
    ew.handle(
        [](const std::logic_error&) noexcept { ADD_FAILURE(); },
        [&](const std::runtime_error&) { handled = true; },
        [](const std::exception&) { ADD_FAILURE(); });
  };

  expect_runtime_error_no_catch_all(ew_eptr);
  EXPECT_TRUE(handled);
  handled = false;
  expect_runtime_error_no_catch_all(ew_small);
  EXPECT_TRUE(handled);
  handled = false;
  expect_runtime_error_no_catch_all(ew_big);
  EXPECT_TRUE(handled);
  handled = false;

  auto expect_runtime_error_catch_non_std = [&](const exception_wrapper& ew) {
    ew.handle(
        [](const std::logic_error&) { ADD_FAILURE(); },
        [&](const std::runtime_error&) { handled = true; },
        [](const std::exception&) { ADD_FAILURE(); },
        [](const int&) { ADD_FAILURE(); });
  };

  expect_runtime_error_catch_non_std(ew_eptr);
  EXPECT_TRUE(handled);
  handled = false;
  expect_runtime_error_catch_non_std(ew_small);
  EXPECT_TRUE(handled);
  handled = false;
  expect_runtime_error_catch_non_std(ew_big);
  EXPECT_TRUE(handled);
  handled = false;

  // Test that an exception thrown from one handler is not caught by an
  // outer handler:
  auto expect_runtime_error_rethrow = [&](const exception_wrapper& ew) {
    ew.handle(
        [](const std::logic_error&) { ADD_FAILURE(); },
        [&](const std::runtime_error& e) {
          handled = true;
          throw e;
        },
        [](const std::exception&) { ADD_FAILURE(); });
  };

  EXPECT_THROW(expect_runtime_error_rethrow(ew_eptr), std::runtime_error);
  EXPECT_TRUE(handled);
  handled = false;
  EXPECT_THROW(expect_runtime_error_rethrow(ew_small), std::runtime_error);
  EXPECT_TRUE(handled);
  handled = false;
  EXPECT_THROW(expect_runtime_error_rethrow(ew_big), std::runtime_error);
  EXPECT_TRUE(handled);
}

TEST(ExceptionWrapper, handleStdExceptionUnhandled) {
  auto ep = std::make_exception_ptr(std::exception{});
  exception_wrapper const ew_eptr(ep);
  exception_wrapper const ew_small(std::exception{});

  bool handled = false;
  auto expect_runtime_error_yes_catch_all = [&](const exception_wrapper& ew) {
    ew.handle(
        [](const std::logic_error&) { ADD_FAILURE(); },
        [](const std::runtime_error&) { ADD_FAILURE(); },
        [&](...) { handled = true; });
  };

  expect_runtime_error_yes_catch_all(ew_eptr);
  EXPECT_TRUE(handled);
  handled = false;
  expect_runtime_error_yes_catch_all(ew_small);
  EXPECT_TRUE(handled);
}

TEST(ExceptionWrapper, handleStdExceptionPropagated) {
  auto ep = std::make_exception_ptr(std::runtime_error{"hello world"});
  exception_wrapper const ew_eptr(ep);
  exception_wrapper const ew_small(std::runtime_error{"hello world"});
  exception_wrapper const ew_big(BigRuntimeError{"hello world"});

  EXPECT_THROW(ew_eptr.handle(), std::runtime_error);
  EXPECT_THROW(ew_small.handle(), std::runtime_error);
  EXPECT_THROW(ew_big.handle(), std::runtime_error);
}

TEST(ExceptionWrapper, handleNonStdExceptionSmall) {
  auto ep = std::make_exception_ptr(42);
  exception_wrapper const ew_eptr1(ep);
  exception_wrapper const ew_eptr2(ep);
  exception_wrapper const ew_small(std::in_place, 42);
  bool handled = false;

  auto expect_int_yes_catch_all = [&](const exception_wrapper& ew) {
    ew.handle(
        [](const std::exception&) { ADD_FAILURE(); },
        [&](...) { handled = true; });
  };

  expect_int_yes_catch_all(ew_eptr1);
  EXPECT_TRUE(handled);
  handled = false;
  expect_int_yes_catch_all(ew_eptr2);
  EXPECT_TRUE(handled);
  handled = false;
  expect_int_yes_catch_all(ew_small);
  EXPECT_TRUE(handled);
  handled = false;

  auto expect_int_no_catch_all = [&](const exception_wrapper& ew) {
    ew.handle(
        [](const std::exception&) { ADD_FAILURE(); },
        [&](const int&) { handled = true; });
  };

  expect_int_no_catch_all(ew_eptr1);
  EXPECT_TRUE(handled);
  handled = false;
  expect_int_no_catch_all(ew_eptr2);
  EXPECT_TRUE(handled);
  handled = false;
  expect_int_no_catch_all(ew_small);
  EXPECT_TRUE(handled);
  handled = false;

  auto expect_int_no_catch_all_2 = [&](const exception_wrapper& ew) {
    ew.handle(
        [&](const int&) { handled = true; },
        [](const std::exception&) { ADD_FAILURE(); });
  };

  expect_int_no_catch_all_2(ew_eptr1);
  EXPECT_TRUE(handled);
  handled = false;
  expect_int_no_catch_all_2(ew_eptr2);
  EXPECT_TRUE(handled);
  handled = false;
  expect_int_no_catch_all_2(ew_small);
  EXPECT_TRUE(handled);
}

TEST(ExceptionWrapper, handleNonStdExceptionBig) {
  auto ep = std::make_exception_ptr(BigNonStdError{});
  exception_wrapper const ew_eptr1(ep);
  exception_wrapper const ew_eptr2(ep);
  exception_wrapper const ew_big(std::in_place, BigNonStdError{});
  bool handled = false;

  auto expect_int_yes_catch_all = [&](const exception_wrapper& ew) {
    ew.handle(
        [](const std::exception&) { ADD_FAILURE(); },
        [&](...) { handled = true; });
  };

  expect_int_yes_catch_all(ew_eptr1);
  EXPECT_TRUE(handled);
  handled = false;
  expect_int_yes_catch_all(ew_eptr2);
  EXPECT_TRUE(handled);
  handled = false;
  expect_int_yes_catch_all(ew_big);
  EXPECT_TRUE(handled);
  handled = false;

  auto expect_int_no_catch_all = [&](const exception_wrapper& ew) {
    ew.handle(
        [](const std::exception&) { ADD_FAILURE(); },
        [&](const BigNonStdError&) { handled = true; });
  };

  expect_int_no_catch_all(ew_eptr1);
  EXPECT_TRUE(handled);
  handled = false;
  expect_int_no_catch_all(ew_eptr2);
  EXPECT_TRUE(handled);
  handled = false;
  expect_int_no_catch_all(ew_big);
  EXPECT_TRUE(handled);
  handled = false;

  auto expect_int_no_catch_all_2 = [&](const exception_wrapper& ew) {
    ew.handle(
        [&](const BigNonStdError&) { handled = true; },
        [](const std::exception&) { ADD_FAILURE(); });
  };

  expect_int_no_catch_all_2(ew_eptr1);
  EXPECT_TRUE(handled);
  handled = false;
  expect_int_no_catch_all_2(ew_eptr2);
  EXPECT_TRUE(handled);
  handled = false;
  expect_int_no_catch_all_2(ew_big);
  EXPECT_TRUE(handled);
  handled = false;

  EXPECT_THROW(
      expect_int_no_catch_all_2(exception_wrapper{std::in_place, 42}), int);
}

TEST(ExceptionWrapper, handleNonStdExceptionRethrowBaseDerived) {
  exception_wrapper ew{std::make_exception_ptr(DerivedNonStdException{})};
  bool handled = false;
  EXPECT_THROW(
      ew.handle(
          [&](const DerivedNonStdException& e) {
            handled = true;
            throw e;
          },
          [](const BaseNonStdException&) { ADD_FAILURE(); }),
      DerivedNonStdException);
  EXPECT_TRUE(handled);
  handled = false;
  EXPECT_THROW(
      ew.handle(
          [&](const DerivedNonStdException& e) {
            handled = true;
            throw e;
          },
          [](...) { ADD_FAILURE(); }),
      DerivedNonStdException);
  EXPECT_TRUE(handled);
}

TEST(ExceptionWrapper, selfSwapTest) {
  exception_wrapper ew(std::runtime_error("hello world"));
  folly::swap(ew, ew);
  EXPECT_EQ(kRuntimeErrorClassName + ": hello world", ew.what());
  auto& ew2 = ew;
  ew = std::move(ew2); // should not crash
}

TEST(ExceptionWrapper, terminateWithTest) {
  auto ew = make_exception_wrapper<int>(42);
  EXPECT_DEATH(try { ew.terminate_with(); } catch (...){}, "int");
}
