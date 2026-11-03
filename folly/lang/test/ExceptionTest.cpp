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

#include <folly/lang/Exception.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>

#include <fmt/format.h>

#include <folly/Portability.h>
#include <folly/lang/Align.h>
#include <folly/lang/Keep.h>
#include <folly/lang/Pretty.h>
#include <folly/portability/GTest.h>

using namespace std::literals;

extern "C" FOLLY_KEEP void check_cond_std_terminate(bool c) {
  if (c) {
    std::terminate();
  }
  folly::detail::keep_sink();
}
extern "C" FOLLY_KEEP void check_cond_folly_terminate_with(bool c) {
  if (c) {
    folly::terminate_with<std::runtime_error>("bad error");
  }
  folly::detail::keep_sink();
}
extern "C" FOLLY_KEEP std::exception const* check_get_object_exception(
    std::exception_ptr const& ptr) {
  return folly::exception_ptr_get_object<std::exception>(ptr);
}

extern "C" FOLLY_KEEP void check_cond_catch_exception(bool c) {
  auto try_ = [=] { c ? folly::throw_exception(0) : void(); };
  auto catch_ = []() { folly::detail::keep_sink(); };
  folly::catch_exception(try_, catch_);
}
extern "C" FOLLY_KEEP void check_cond_catch_exception_nx(bool c) {
  auto try_ = [=] { c ? folly::throw_exception(0) : void(); };
  auto catch_ = []() noexcept { folly::detail::keep_sink_nx(); };
  folly::catch_exception(try_, catch_);
}
extern "C" FOLLY_KEEP void check_cond_catch_exception_ptr(bool c) {
  auto try_ = [=] { c ? folly::throw_exception(0) : void(); };
  auto catch_ = folly::detail::keep_sink<>;
  folly::catch_exception(try_, catch_);
}
extern "C" FOLLY_KEEP void check_cond_catch_exception_ptr_nx(bool c) {
  auto try_ = [=] { c ? folly::throw_exception(0) : void(); };
  auto catch_ = folly::detail::keep_sink_nx<>;
  folly::catch_exception(try_, catch_);
}

extern "C" FOLLY_KEEP void check_std_make_exception_ptr(
    std::exception_ptr* ptr) {
  ::new (ptr) std::exception_ptr( //
      std::make_exception_ptr(std::runtime_error("foo")));
}
extern "C" FOLLY_KEEP void check_folly_make_exception_ptr_with_in_place(
    std::exception_ptr* ptr) {
  ::new (ptr) std::exception_ptr(
      folly::make_exception_ptr_with(std::in_place, std::runtime_error("foo")));
}
extern "C" FOLLY_KEEP void check_folly_make_exception_ptr_with_in_place_type(
    std::exception_ptr* ptr) {
  constexpr auto tag = std::in_place_type<std::runtime_error>;
  ::new (ptr) std::exception_ptr( //
      folly::make_exception_ptr_with(tag, "foo"));
}
extern "C" FOLLY_KEEP void check_folly_make_exception_ptr_with_invocable(
    std::exception_ptr* ptr) {
  ::new (ptr) std::exception_ptr(folly::make_exception_ptr_with([] {
    return std::runtime_error("foo");
  }));
}

template <typename Ex>
static std::string message_for_terminate_with(std::string const& what) {
  auto const name = folly::pretty_name<Ex>();
  std::string const p0 = "terminate called after throwing an instance of";
  std::string const p1 = "terminating (due to|with) uncaught exception of type";
  // clang-format off
  return
      folly::kIsGlibcxx ? p0 + " '" + name + "'\\s+what\\(\\):\\s+" + what :
      folly::kIsLibcpp ? p1 + " " + name + ": " + what :
      "" /* empty regex matches anything */;
  // clang-format on
}

static std::string message_for_terminate() {
  // clang-format off
  return
      folly::kIsGlibcxx ? "terminate called without an active exception" :
      folly::kIsLibcpp ? "terminating" :
      "" /* empty regex matches anything */;
  // clang-format on
}

namespace {

template <int I>
struct Virt {
  virtual ~Virt() {}
  int value = I;
  operator int() const { return value; }
};

} // namespace

class MyException : public std::exception {
 private:
  char const* what_;

 public:
  explicit MyException(char const* const what) : MyException(what, 0) {}
  MyException(char const* const what, std::size_t const strip)
      : what_(what + strip) {}

  char const* what() const noexcept override { return what_; }
};

struct MyFmtFormatCStrException : std::exception {
  std::runtime_error inner;
  explicit MyFmtFormatCStrException(char const* const what) noexcept
      : inner{what} {}
  char const* what() const noexcept override { return inner.what(); }
};

struct MyFmtFormatStringException : std::exception {
  std::runtime_error inner;
  explicit MyFmtFormatStringException(std::string&& what) : inner{what} {}
  explicit MyFmtFormatStringException(char const*) : inner{""} {
    ADD_FAILURE();
  }
  char const* what() const noexcept override { return inner.what(); }
};

class ExceptionTest : public testing::Test {};

TEST_F(ExceptionTest, throw_exception_direct) {
  try {
    folly::throw_exception<MyException>("hello world");
    ADD_FAILURE();
  } catch (MyException const& ex) {
    EXPECT_STREQ("hello world", ex.what());
  }
}

TEST_F(ExceptionTest, throw_exception_variadic) {
  try {
    folly::throw_exception<MyException>("hello world", 6);
    ADD_FAILURE();
  } catch (MyException const& ex) {
    EXPECT_STREQ("world", ex.what());
  }
}

struct non_copyable {
  std::string s = "hello world";

  non_copyable() = default;
  non_copyable(non_copyable const&) = delete;
  non_copyable& operator=(non_copyable const&) = delete;
  non_copyable(non_copyable&&) = default;
  non_copyable& operator=(non_copyable&&) = default;
};

template <>
struct fmt::formatter<non_copyable> {
  constexpr auto parse(format_parse_context& ctx) -> decltype(ctx.begin()) {
    return ctx.begin();
  }
  auto format(const non_copyable& val, format_context& ctx) const
      -> decltype(ctx.out()) {
    return fmt::format_to(ctx.out(), "{}", val.s);
  }
};

TEST_F(ExceptionTest, throw_exception_fmt_format_string) {
  try {
    folly::throw_exception_fmt_format<std::runtime_error>(
        "{}, {}, {}", non_copyable{}, 42, "foo");
  } catch (std::runtime_error const& ex) {
    EXPECT_STREQ("hello world, 42, foo", ex.what());
  }
  try {
    non_copyable nc;
    int i = 42;
    auto& foo = "foo";
    folly::throw_exception_fmt_format<std::runtime_error>(
        "{}, {}, {}", nc, i, foo);
  } catch (std::runtime_error const& ex) {
    EXPECT_STREQ("hello world, 42, foo", ex.what());
  }

  try {
    const non_copyable nc;
    const int i = 42;
    const auto& foo = "foo";
    folly::throw_exception_fmt_format<std::runtime_error>(
        "{}, {}, {}", nc, i, foo);
  } catch (std::runtime_error const& ex) {
    EXPECT_STREQ("hello world, 42, foo", ex.what());
  }
  try {
    const non_copyable nc;
    const int i = 42;
    const auto& foo = "foo";
    // NOLINTBEGIN(performance-move-const-arg)
    folly::throw_exception_fmt_format<std::runtime_error>(
        "{}, {}, {}", std::move(nc), std::move(i), std::move(foo));
    // NOLINTEND(performance-move-const-arg)
  } catch (std::runtime_error const& ex) {
    EXPECT_STREQ("hello world, 42, foo", ex.what());
  }
}

TEST_F(ExceptionTest, throw_exception_fmt_format_c_str) {
  try {
    folly::throw_exception_fmt_format<MyFmtFormatCStrException>(
        "{} {}", "hello", "world");
    ADD_FAILURE();
  } catch (MyFmtFormatCStrException const& ex) {
    EXPECT_STREQ("hello world", ex.what());
  }
}

TEST_F(ExceptionTest, throw_exception_fmt_format_string_view) {
  try {
    folly::throw_exception_fmt_format<MyFmtFormatStringException>(
        "{} {}", "hello", "world");
    ADD_FAILURE();
  } catch (MyFmtFormatStringException const& ex) {
    EXPECT_STREQ("hello world", ex.what());
  }
}

TEST_F(ExceptionTest, terminate_with_direct) {
  EXPECT_DEATH(
      folly::terminate_with<MyException>("hello world"),
      message_for_terminate_with<MyException>("hello world"));
}

TEST_F(ExceptionTest, terminate_with_variadic) {
  EXPECT_DEATH(
      folly::terminate_with<MyException>("hello world", 6),
      message_for_terminate_with<MyException>("world"));
}

TEST_F(ExceptionTest, terminate_with_fmt_format_c_str) {
  EXPECT_DEATH(
      folly::terminate_with_fmt_format<MyFmtFormatCStrException>(
          "{} {}", "hello", "world"),
      message_for_terminate_with<MyFmtFormatCStrException>("hello world"));
}

TEST_F(ExceptionTest, terminate_with_fmt_format_string_view) {
  EXPECT_DEATH(
      folly::terminate_with_fmt_format<MyFmtFormatStringException>(
          "{} {}", "hello", "world"),
      message_for_terminate_with<MyFmtFormatStringException>("hello world"));
}

TEST_F(ExceptionTest, invoke_cold) {
  EXPECT_THROW(
      folly::invoke_cold([] { throw std::runtime_error("bad"); }),
      std::runtime_error);
  EXPECT_EQ(7, folly::invoke_cold([] { return 7; }));
}

TEST_F(ExceptionTest, invoke_noreturn_cold) {
  EXPECT_THROW(
      folly::invoke_noreturn_cold([] { throw std::runtime_error("bad"); }),
      std::runtime_error);
  EXPECT_DEATH(folly::invoke_noreturn_cold([] {}), message_for_terminate());
}

TEST_F(ExceptionTest, catch_exception) {
  auto identity = [](int i) { return i; };
  auto returner = [](int i) { return [=] { return i; }; };
  auto thrower = [](int i) { return [=]() -> int { throw i; }; };
  EXPECT_EQ(3, folly::catch_exception(returner(3), returner(4)));
  EXPECT_EQ(3, folly::catch_exception<int>(returner(3), identity));
  EXPECT_EQ(3, folly::catch_exception<int>(returner(3), +identity));
  EXPECT_EQ(3, folly::catch_exception<int>(returner(3), *+identity));
  EXPECT_EQ(4, folly::catch_exception(thrower(3), returner(4)));
  EXPECT_EQ(3, folly::catch_exception<int>(thrower(3), identity));
  EXPECT_EQ(3, folly::catch_exception<int>(thrower(3), +identity));
  EXPECT_EQ(3, folly::catch_exception<int>(thrower(3), *+identity));
}

TEST_F(ExceptionTest, rethrow_current_exception) {
  EXPECT_THROW(
      folly::invoke_noreturn_cold([] {
        try {
          throw std::runtime_error("bad");
        } catch (...) {
          folly::rethrow_current_exception();
        }
      }),
      std::runtime_error);
}

TEST_F(ExceptionTest, uncaught_exception) {
  struct dtor {
    unsigned expected;
    explicit dtor(unsigned e) noexcept : expected{e} {}
    ~dtor() {
      EXPECT_EQ(expected, std::uncaught_exceptions());
      EXPECT_EQ(expected, folly::uncaught_exceptions());
    }
  };
  try {
    dtor obj{0};
  } catch (...) {
  }
  try {
    dtor obj{1};
    throw std::exception();
  } catch (...) {
  }
}

TEST_F(ExceptionTest, current_exception) {
  EXPECT_EQ(nullptr, std::current_exception());
  EXPECT_EQ(nullptr, folly::current_exception());
  try {
    throw std::exception();
  } catch (...) {
    // primary exception?
#if defined(_CPPLIB_VER)
    // As per
    // https://learn.microsoft.com/en-us/cpp/standard-library/exception-functions?view=msvc-170
    // current_exception() returns a new value each time, so its not directly
    // comparable on MSVC
    EXPECT_NE(nullptr, std::current_exception());
    EXPECT_NE(nullptr, folly::current_exception());
#else
    EXPECT_EQ(std::current_exception(), folly::current_exception());
#endif
  }
  try {
    throw std::exception();
  } catch (...) {
    try {
      throw;
    } catch (...) {
      // dependent exception?
#if defined(_CPPLIB_VER)
      EXPECT_NE(nullptr, std::current_exception());
      EXPECT_NE(nullptr, folly::current_exception());
#else
      EXPECT_EQ(std::current_exception(), folly::current_exception());
#endif
    }
  }
}

TEST_F(ExceptionTest, exception_ptr_empty) {
  auto ptr = std::exception_ptr();
  EXPECT_EQ(nullptr, folly::exception_ptr_get_type(ptr));
  EXPECT_EQ(nullptr, folly::exception_ptr_get_object(ptr, nullptr));
  EXPECT_EQ(nullptr, folly::exception_ptr_get_object(ptr, &typeid(long)));
  EXPECT_EQ(nullptr, folly::exception_ptr_get_object(ptr, &typeid(int)));
  EXPECT_EQ(nullptr, folly::exception_ptr_get_object<int>(ptr));
  EXPECT_EQ(nullptr, folly::exception_ptr_get_object(ptr));
}

TEST_F(ExceptionTest, exception_ptr_int) {
  auto ptr = std::make_exception_ptr(17);
  EXPECT_EQ(&typeid(int), folly::exception_ptr_get_type(ptr));
  EXPECT_EQ(17, *(int*)(folly::exception_ptr_get_object(ptr, nullptr)));
  EXPECT_EQ(nullptr, folly::exception_ptr_get_object(ptr, &typeid(long)));
  EXPECT_EQ(17, *(int*)(folly::exception_ptr_get_object(ptr, &typeid(int))));
  EXPECT_EQ(17, *folly::exception_ptr_get_object<int>(ptr));
  EXPECT_EQ(17, *(int*)(folly::exception_ptr_get_object(ptr)));
}

TEST_F(ExceptionTest, exception_ptr_vmi) {
  using A0 = Virt<0>;
  using A1 = Virt<1>;
  using A2 = Virt<2>;
  struct C;
  struct B0 : virtual A1, virtual A2 {
    using folly_get_exception_hint_types = folly::tag_t<B0, C>;
  };
  struct B1 : virtual A2, virtual A0 {};
  struct B2 : virtual A0, virtual A1 {};
  struct C : B0, B1, B2 {
    int value = 44;
    operator int() const { return value; }
  };

  auto ptr = std::make_exception_ptr(C());
  EXPECT_EQ(&typeid(C), folly::exception_ptr_get_type(ptr));
  EXPECT_EQ(44, *(C*)(folly::exception_ptr_get_object(ptr, nullptr)));
  EXPECT_EQ(nullptr, folly::exception_ptr_get_object(ptr, &typeid(long)));
  EXPECT_EQ(44, *(C*)(folly::exception_ptr_get_object(ptr, &typeid(C))));
  EXPECT_EQ(1, *(A1*)(folly::exception_ptr_get_object(ptr, &typeid(A1))));
  EXPECT_EQ(1, folly::exception_ptr_get_object<A1>(ptr)->value);
  EXPECT_EQ(44, *(C*)(folly::exception_ptr_get_object(ptr)));

  EXPECT_EQ(
      nullptr,
      folly::exception_ptr_try_get_object_exact_fast<A0>(
          nullptr, folly::tag<B1, B2>));
  EXPECT_EQ(
      nullptr,
      folly::exception_ptr_try_get_object_exact_fast<A0>(
          std::make_exception_ptr(17), folly::tag<B1, B2>));
  EXPECT_EQ(
      nullptr,
      folly::exception_ptr_try_get_object_exact_fast<A0>(
          ptr, folly::tag<B1, B2>));
  EXPECT_EQ(
      folly::exception_ptr_get_object<C>(ptr),
      folly::exception_ptr_try_get_object_exact_fast<A0>(
          ptr, folly::tag<B1, C, B2>));

  EXPECT_EQ(
      nullptr,
      folly::exception_ptr_get_object_hint<A0>(nullptr, folly::tag<B1, B2>));
  EXPECT_EQ(
      nullptr,
      folly::exception_ptr_get_object_hint<A0>(
          std::make_exception_ptr(17), folly::tag<B1, B2>));
  EXPECT_EQ(
      folly::exception_ptr_get_object<C>(ptr),
      folly::exception_ptr_get_object_hint<A0>(ptr, folly::tag<B1, B2>));
  EXPECT_EQ(
      folly::exception_ptr_get_object<C>(ptr),
      folly::exception_ptr_get_object_hint<A0>(ptr, folly::tag<B1, C, B2>));
  EXPECT_EQ(
      folly::exception_ptr_get_object<B0>(ptr),
      // Uses `C::folly_get_exception_hint_types`, put `void` in list to confirm
      folly::exception_ptr_get_object_hint<B0>(ptr));
}

TEST_F(ExceptionTest, make_exception_ptr_with_invocable_fail) {
  auto ptr = folly::make_exception_ptr_with( //
      []() -> std::string { throw 17; });
  EXPECT_EQ(&typeid(int), folly::exception_ptr_get_type(ptr));
  EXPECT_EQ(17, *folly::exception_ptr_get_object<int>(ptr));
}

TEST_F(ExceptionTest, make_exception_ptr_with_invocable) {
  auto ptr = folly::make_exception_ptr_with( //
      [] { return std::string("hello world"); });
  EXPECT_EQ(&typeid(std::string), folly::exception_ptr_get_type(ptr));
  EXPECT_EQ("hello world", *folly::exception_ptr_get_object<std::string>(ptr));
}

TEST_F(ExceptionTest, make_exception_ptr_with_in_place_type) {
  auto ptr = folly::make_exception_ptr_with(
      std::in_place_type<std::string>, "hello world");
  EXPECT_EQ(&typeid(std::string), folly::exception_ptr_get_type(ptr));
  EXPECT_EQ("hello world", *folly::exception_ptr_get_object<std::string>(ptr));
}

TEST_F(ExceptionTest, make_exception_ptr_with_in_place) {
  auto ptr = folly::make_exception_ptr_with(std::in_place, 17);
  EXPECT_EQ(&typeid(int), folly::exception_ptr_get_type(ptr));
  EXPECT_EQ(17, *folly::exception_ptr_get_object<int>(ptr));
}

TEST_F(ExceptionTest, get_exception_from_std_exception_ptr) {
  using folly::get_exception;
  using folly::get_mutable_exception;

  static_assert(
      std::is_invocable_v<
          folly::get_exception_fn<std::exception>,
          const std::exception_ptr&>);
  static_assert(
      std::is_invocable_v<
          folly::get_mutable_exception_fn<std::exception>,
          std::exception_ptr&>);
  static_assert(
      !std::is_invocable_v<
          folly::get_mutable_exception_fn<std::exception>,
          const std::exception_ptr&>);

  // Unsafe to extract a pointer out of rvalues
  static_assert(
      !std::is_invocable_v<
          folly::get_exception_fn<std::exception>,
          std::exception_ptr&&>);
  static_assert(
      !std::is_invocable_v<
          folly::get_exception_fn<std::exception>,
          const std::exception_ptr&&>);
  static_assert(
      !std::is_invocable_v<
          folly::get_mutable_exception_fn<std::exception>,
          std::exception_ptr&&>);
  static_assert(
      !std::is_invocable_v<
          folly::get_mutable_exception_fn<std::exception>,
          const std::exception_ptr&&>);

#if 0 // manual test for "clang:lifetimebound"
  const std::exception* ex = []() {
    auto ep = folly::make_exception_ptr_with([]() {
      return std::runtime_error{"foo"};
    });
    return get_exception<>(ep);
  }();
  EXPECT_EQ("foo", ex->what());
#endif

  auto eptr = folly::make_exception_ptr_with([]() {
    return std::runtime_error{"foo"};
  });

  EXPECT_EQ(nullptr, get_exception<std::system_error>(eptr));

  EXPECT_STREQ("foo", get_exception<std::exception>(eptr)->what());
  EXPECT_STREQ(
      "foo", get_exception<std::exception>(std::as_const(eptr))->what());
  EXPECT_STREQ("foo", get_mutable_exception<std::exception>(eptr)->what());

  EXPECT_STREQ("foo", get_exception<const std::exception>(eptr)->what());
  EXPECT_STREQ(
      "foo", get_exception<const std::exception>(std::as_const(eptr))->what());
  // While this is a very silly kind of usage, it does work.
  EXPECT_STREQ(
      "foo", get_mutable_exception<const std::exception>(eptr)->what());

  EXPECT_STREQ("foo", get_exception<>(eptr)->what());
  EXPECT_STREQ("foo", get_exception<>(std::as_const(eptr))->what());
  EXPECT_STREQ("foo", get_mutable_exception<>(eptr)->what());

  EXPECT_STREQ("foo", get_exception<std::runtime_error>(eptr)->what());
  EXPECT_STREQ(
      "foo", get_exception<std::runtime_error>(std::as_const(eptr))->what());
  EXPECT_STREQ("foo", get_mutable_exception<std::runtime_error>(eptr)->what());

  auto* expected_p = folly::exception_ptr_get_object<std::runtime_error>(eptr);
  EXPECT_EQ(expected_p, get_exception<std::runtime_error>(eptr));
  EXPECT_EQ(expected_p, get_exception<std::runtime_error>(std::as_const(eptr)));

  static_assert(
      std::is_same_v<
          const std::runtime_error*,
          decltype(get_exception<std::runtime_error>(eptr))>);
  static_assert(
      std::is_same_v<
          const std::runtime_error*,
          decltype(get_exception<const std::runtime_error>(eptr))>);
  static_assert(
      std::is_same_v<
          const std::runtime_error*,
          decltype(get_exception<std::runtime_error>(std::as_const(eptr)))>);
  static_assert(
      std::is_same_v<
          std::runtime_error*,
          decltype(get_mutable_exception<std::runtime_error>(eptr))>);
}

namespace {
// Example of using `get_exception` with a custom pointer wrapper
template <typename T>
struct AccessCountingPtr {
  T* ptr_;
  std::atomic<int>* accessCount_;

  AccessCountingPtr(T* p, std::atomic<int>* count)
      : ptr_(p), accessCount_(count) {}
  explicit operator bool() const { return ptr_; }
  T& operator*() const {
    ++*accessCount_;
    return *ptr_;
  }
  T* operator->() const {
    ++*accessCount_;
    return ptr_;
  }
};

// Example of an exception container with a custom pointer wrapper
struct CustomExceptionPtr : std::exception_ptr {
  mutable std::atomic<int> accessCount_ = 0;

  explicit CustomExceptionPtr(std::exception_ptr ep)
      : std::exception_ptr(std::move(ep)) {}

  template <typename Ex>
  AccessCountingPtr<const Ex> get_exception(
      folly::get_exception_tag_t) const noexcept {
    return {
        folly::exception_ptr_get_object_hint<const Ex>(*this), &accessCount_};
  }

  template <typename Ex>
  AccessCountingPtr<Ex> get_mutable_exception(
      folly::get_exception_tag_t) noexcept {
    return {folly::exception_ptr_get_object_hint<Ex>(*this), &accessCount_};
  }
};
} // namespace

TEST_F(ExceptionTest, get_exception_with_custom_pointer_traits) {
  auto customPtr = CustomExceptionPtr{folly::make_exception_ptr_with(
      std::in_place_type<std::runtime_error>, "custom pointer test")};

  EXPECT_EQ(0, customPtr.accessCount_);

  if (auto ex = folly::get_exception<std::runtime_error>(customPtr)) {
    EXPECT_EQ(0, customPtr.accessCount_); // No access yet
    EXPECT_STREQ("custom pointer test", ex->what());
    EXPECT_EQ(1, customPtr.accessCount_); // Access counted
  }

  if (auto ex = folly::get_mutable_exception<std::runtime_error>(customPtr)) {
    EXPECT_STREQ("custom pointer test", (*ex).what());
    EXPECT_EQ(2, customPtr.accessCount_); // Second access
  }
}

template <typename String>
void test_exception_shared_string_construct(const char* c, String s0) {
  EXPECT_STREQ(c, s0.what());
  {
    static_assert(std::is_copy_constructible_v<String>);
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    auto s1 = s0;
    EXPECT_STREQ(c, s1.what());
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    auto s2 = s1;
    EXPECT_STREQ(c, s2.what());
  }
  {
    static_assert(std::is_move_constructible_v<String>);
    auto s1 = std::move(s0);
    EXPECT_STREQ(c, s1.what());
    // NOLINTNEXTLINE(bugprone-use-after-move)
    EXPECT_STREQ("", s0.what());
  }
}

template <typename String>
void test_exception_shared_string_assign(const char* c, String s0) {
  {
    static_assert(std::is_copy_assignable_v<String>);
    String s1{"original", 8};
    s1 = s0;
    EXPECT_STREQ(c, s1.what());
    s1 = std::as_const(std::as_const(s1));
    EXPECT_STREQ(c, s1.what());
  }
  {
    static_assert(std::is_move_assignable_v<String>);

    String s1{"original", 8};
    s1 = std::move(s0);
    EXPECT_STREQ(c, s1.what());
    // NOLINTNEXTLINE(bugprone-use-after-move)
    EXPECT_STREQ("", s0.what());

    s1 = std::move(std::move(s1)); // suppress self-move warning
    EXPECT_STREQ(c, s1.what());
  }
}

TEST_F(ExceptionTest, exception_shared_string_construct_cstr) {
  test_exception_shared_string_construct(
      "msg", folly::exception_shared_string("msg", strlen("msg")));
}

TEST_F(ExceptionTest, exception_shared_string_assign_cstr) {
  test_exception_shared_string_assign(
      "msg", folly::exception_shared_string("msg", strlen("msg")));
}

TEST_F(ExceptionTest, exception_shared_string_construct_string_view) {
  test_exception_shared_string_construct(
      "msg", folly::exception_shared_string(std::string_view("msg")));
}

TEST_F(ExceptionTest, exception_shared_string_assign_string_view) {
  test_exception_shared_string_assign(
      "msg", folly::exception_shared_string(std::string_view("msg")));
}

TEST_F(ExceptionTest, exception_shared_string_construct_string) {
  test_exception_shared_string_construct(
      "msg", folly::exception_shared_string(std::string("msg")));
}

TEST_F(ExceptionTest, exception_shared_string_assign_string) {
  test_exception_shared_string_assign(
      "msg", folly::exception_shared_string(std::string("msg")));
}

TEST_F(ExceptionTest, exception_shared_string_literal_consteval) {
  constexpr const char* c = "hello, world!";
  {
    folly::exception_shared_string s0{c};
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    auto s1 = s0;
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    auto s2 = s1;
    EXPECT_STREQ(c, s2.what());
  }
  // Same, but `constexpr`.  Future: need C++20 `std::is_constant_evaluated` to
  // make the copy ctor `constexpr` as well.
  {
    constexpr folly::exception_shared_string s0{c};
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    auto s1 = s0;
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    auto s2 = s1;
    EXPECT_STREQ(c, s2.what());
  }
}

// example of how to do the in-place formatting efficiently
struct format_param_fn {
  template <typename A>
  using arg_t = folly::conditional_t<folly::is_register_pass_v<A>, A, A const&>;

  template <typename... A>
  auto operator()(
      fmt::format_string<arg_t<A>...> const& fmt, A const&... arg) const {
    return std::pair{
        fmt::formatted_size(fmt, static_cast<arg_t<A>>(arg)...),
        [&](auto buf, auto len) {
          auto res =
              fmt::format_to_n(buf, len, fmt, static_cast<arg_t<A>>(arg)...);
          FOLLY_SAFE_DCHECK(len == res.size);
        }};
  }
};
inline constexpr format_param_fn format_param{};

TEST_F(ExceptionTest, exception_shared_string_format) {
  auto s = std::invoke(
      [](auto p) { return folly::exception_shared_string(p.first, p.second); },
      format_param("a number {} and a string {}", 217, "flobber"s));
  EXPECT_STREQ("a number 217 and a string flobber", s.what());
}

TEST_F(ExceptionTest, exception_ptr_use_count_empty) {
  auto ptr = std::exception_ptr();
  EXPECT_EQ(0, folly::exception_ptr_use_count(ptr));
  EXPECT_FALSE(folly::exception_ptr_unique(ptr));
}

TEST_F(ExceptionTest, exception_ptr_use_count_single) {
  auto ptr = std::make_exception_ptr(42);
  EXPECT_EQ(1, folly::exception_ptr_use_count(ptr));
  EXPECT_TRUE(folly::exception_ptr_unique(ptr));
}

TEST_F(ExceptionTest, exception_ptr_use_count_copied) {
  auto ptr1 = std::make_exception_ptr(42);
  auto ptr2 = ptr1;
  EXPECT_EQ(2, folly::exception_ptr_use_count(ptr1));
  EXPECT_EQ(2, folly::exception_ptr_use_count(ptr2));
  EXPECT_FALSE(folly::exception_ptr_unique(ptr1));
  EXPECT_FALSE(folly::exception_ptr_unique(ptr2));
}

TEST_F(ExceptionTest, exception_ptr_use_count_after_reset) {
  auto ptr1 = std::make_exception_ptr(42);
  auto ptr2 = ptr1;
  EXPECT_EQ(2, folly::exception_ptr_use_count(ptr1));
  ptr2 = std::exception_ptr();
  EXPECT_EQ(1, folly::exception_ptr_use_count(ptr1));
  EXPECT_TRUE(folly::exception_ptr_unique(ptr1));
  EXPECT_EQ(0, folly::exception_ptr_use_count(ptr2));
}
