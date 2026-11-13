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

#include <folly/Try.h>

#include <glog/logging.h>

#include <folly/Traits.h>
#include <folly/lang/Exception.h>
#include <folly/portability/GTest.h>

using namespace folly;

namespace {

class A {
 public:
  explicit A(int x) : x_(x) {}

  int x() const { return x_; }

 private:
  int x_;
};

template <bool Nothrow>
class HasCtors {
 public:
  explicit HasCtors(int) noexcept(Nothrow) {}
  HasCtors(HasCtors&&) noexcept(Nothrow) {}
  HasCtors& operator=(HasCtors&&) noexcept(Nothrow) {}
  HasCtors(HasCtors const&) noexcept(Nothrow) {}
  HasCtors& operator=(HasCtors const&) noexcept(Nothrow) {}
};

class MoveConstructOnly {
 public:
  MoveConstructOnly() = default;
  MoveConstructOnly(const MoveConstructOnly&) = delete;
  MoveConstructOnly(MoveConstructOnly&&) = default;
};

class MutableContainer {
 public:
  mutable MoveConstructOnly val;
};
} // namespace

TEST(Try, basic) {
  A a(5);
  Try<A> t_a(std::move(a));

  Try<Unit> t_void;

  EXPECT_EQ(5, t_a.value().x());
}

TEST(Try, inPlace) {
  Try<A> t_a(std::in_place, 5);

  EXPECT_EQ(5, t_a.value().x());
}

TEST(Try, inPlaceNested) {
  Try<Try<A>> t_t_a(std::in_place, std::in_place, 5);

  EXPECT_EQ(5, t_t_a.value().value().x());
}

TEST(Try, assignmentWithThrowingCopyConstructor) {
  struct MyException : std::exception {};
  struct ThrowingCopyConstructor {
    int& counter_;
    explicit ThrowingCopyConstructor(int& counter) : counter_(counter) {
      ++counter_;
    }

    [[noreturn]] ThrowingCopyConstructor(
        const ThrowingCopyConstructor& other) noexcept(false)
        : counter_(other.counter_) {
      throw MyException{};
    }

    ThrowingCopyConstructor& operator=(const ThrowingCopyConstructor&) = delete;

    ~ThrowingCopyConstructor() { --counter_; }
  };

  int counter = 0;

  {
    Try<ThrowingCopyConstructor> t1{std::in_place, counter};
    Try<ThrowingCopyConstructor> t2{std::in_place, counter};
    EXPECT_EQ(2, counter);
    EXPECT_THROW(t2 = t1, MyException);
    EXPECT_EQ(1, counter);
    EXPECT_FALSE(t2.hasValue());
    EXPECT_TRUE(t1.hasValue());
  }
  EXPECT_EQ(0, counter);
  {
    Try<ThrowingCopyConstructor> t1{std::in_place, counter};
    Try<ThrowingCopyConstructor> t2;
    EXPECT_EQ(1, counter);
    EXPECT_THROW(t2 = t1, MyException);
    EXPECT_EQ(1, counter);
    EXPECT_FALSE(t2.hasValue());
    EXPECT_TRUE(t1.hasValue());
  }
  EXPECT_EQ(0, counter);
}

TEST(Try, assignmentWithThrowingMoveConstructor) {
  struct MyException : std::exception {};
  struct ThrowingMoveConstructor {
    int& counter_;
    explicit ThrowingMoveConstructor(int& counter) : counter_(counter) {
      ++counter_;
    }

    [[noreturn]] ThrowingMoveConstructor(
        ThrowingMoveConstructor&& other) noexcept(false)
        : counter_(other.counter_) {
      throw MyException{};
    }

    ThrowingMoveConstructor& operator=(ThrowingMoveConstructor&&) = delete;

    ~ThrowingMoveConstructor() { --counter_; }
  };

  int counter = 0;

  {
    Try<ThrowingMoveConstructor> t1{std::in_place, counter};
    Try<ThrowingMoveConstructor> t2{std::in_place, counter};
    EXPECT_EQ(2, counter);
    EXPECT_THROW(t2 = std::move(t1), MyException);
    EXPECT_EQ(1, counter);
    EXPECT_FALSE(t2.hasValue());
    EXPECT_TRUE(t1.hasValue());
  }
  EXPECT_EQ(0, counter);
  {
    Try<ThrowingMoveConstructor> t1{std::in_place, counter};
    Try<ThrowingMoveConstructor> t2;
    EXPECT_EQ(1, counter);
    EXPECT_THROW(t2 = std::move(t1), MyException);
    EXPECT_EQ(1, counter);
    EXPECT_FALSE(t2.hasValue());
    EXPECT_TRUE(t1.hasValue());
  }
  EXPECT_EQ(0, counter);
}

TEST(Try, assignmentUsesNothrowMoveAssignmentForValues) {
  struct TrackedMove {
    int value;
    int& constructions;
    int& assignments;

    TrackedMove(int value_, int& constructions_, int& assignments_)
        : value(value_),
          constructions(constructions_),
          assignments(assignments_) {}

    TrackedMove(TrackedMove&& other) noexcept
        : value(other.value),
          constructions(other.constructions),
          assignments(other.assignments) {
      ++constructions;
    }

    TrackedMove& operator=(TrackedMove&& other) noexcept {
      value = other.value;
      ++assignments;
      return *this;
    }
  };

  int constructions = 0;
  int assignments = 0;
  Try<TrackedMove> source{std::in_place, 2, constructions, assignments};
  Try<TrackedMove> destination{std::in_place, 1, constructions, assignments};

  destination = std::move(source);

  EXPECT_EQ(2, destination.value().value);
  EXPECT_EQ(0, constructions);
  EXPECT_EQ(1, assignments);
}

TEST(Try, emplace) {
  Try<A> t;
  A& t_a = t.emplace(10);
  EXPECT_TRUE(t.hasValue());
  EXPECT_EQ(t_a.x(), 10);
}

TEST(Try, emplaceWithThrowingConstructor) {
  struct MyException : std::exception {};
  struct ThrowingConstructor {
    explicit ThrowingConstructor(bool shouldThrow) {
      if (shouldThrow) {
        throw MyException{};
      }
    }
  };

  {
    // Try constructing from empty state to new value and constructor throws.
    Try<ThrowingConstructor> t;
    EXPECT_FALSE(t.hasValue());
    EXPECT_FALSE(t.hasException());
    EXPECT_THROW(t.emplace(true), MyException);

    EXPECT_FALSE(t.hasValue());
    EXPECT_FALSE(t.hasException());

    EXPECT_THROW(t.value_or(true), MyException);
  }

  {
    // Initialise to value, then re-emplace with throwing constructor.
    // This should reset the object back to empty.
    Try<ThrowingConstructor> t{std::in_place, false};
    EXPECT_TRUE(t.hasValue());
    EXPECT_THROW(t.emplace(true), MyException);
    EXPECT_FALSE(t.hasValue());
    EXPECT_FALSE(t.hasException());

    EXPECT_THROW(t.value_or(true), MyException);
  }
}

TEST(Try, tryEmplace) {
  Try<A> t;
  A* a = tryEmplace(t, 10);
  EXPECT_EQ(&t.value(), a);
  EXPECT_TRUE(t.hasValue());
  EXPECT_EQ(10, t.value().x());
}

TEST(Try, tryEmplaceWithThrowingConstructor) {
  struct MyException : std::exception {};
  struct NonInheritingException {};
  struct ThrowingConstructor {
    [[noreturn]] ThrowingConstructor() noexcept(false) {
      throw NonInheritingException{};
    }

    explicit ThrowingConstructor(bool shouldThrow) {
      if (shouldThrow) {
        throw MyException{};
      }
    }
  };

  {
    Try<ThrowingConstructor> t;
    EXPECT_EQ(nullptr, tryEmplace(t, true));
    EXPECT_TRUE(t.hasException());
    EXPECT_NE(t.tryGetExceptionObject<MyException>(), nullptr);
  }

  {
    Try<ThrowingConstructor> t;
    EXPECT_EQ(nullptr, tryEmplace(t));
    EXPECT_TRUE(t.hasException());
    EXPECT_NE(t.tryGetExceptionObject<NonInheritingException>(), nullptr);
  }

  {
    Try<ThrowingConstructor> t;
    EXPECT_NE(nullptr, tryEmplace(t, false));
    EXPECT_TRUE(t.hasValue());
    EXPECT_EQ(nullptr, tryEmplace(t, true));
    EXPECT_TRUE(t.hasException());
    EXPECT_NE(t.tryGetExceptionObject<MyException>(), nullptr);
  }
}

TEST(Try, emplaceVoidTry) {
  struct MyException : std::exception {};
  Try<void> t;
  t.emplace();
  EXPECT_TRUE(t.hasValue());
  t.emplaceException(std::in_place_type<MyException>);
  EXPECT_FALSE(t.hasValue());
  EXPECT_TRUE(t.hasException());
  EXPECT_TRUE(t.hasException<MyException>());
  t.emplace();
  EXPECT_TRUE(t.hasValue());
  EXPECT_FALSE(t.hasException());
}

TEST(Try, tryEmplaceVoidTry) {
  struct MyException : std::exception {};
  Try<void> t;
  tryEmplace(t);
  EXPECT_TRUE(t.hasValue());
  t.emplaceException(std::in_place_type<MyException>);
  EXPECT_FALSE(t.hasValue());
  EXPECT_TRUE(t.hasException());
  EXPECT_TRUE(t.hasException<MyException>());
  t.emplace();
  EXPECT_TRUE(t.hasValue());
  EXPECT_FALSE(t.hasException());
}

TEST(Try, tryEmplaceWith) {
  Try<std::string> t;
  tryEmplaceWith(t, [] { return "hello"; });
  EXPECT_EQ("hello", t.value());
}

TEST(Try, tryEmplaceWithFunctionThrows) {
  struct MyException : std::exception {};
  Try<int> t;
  tryEmplaceWith(t, []() -> int { throw MyException{}; });
  EXPECT_TRUE(t.hasException());
  EXPECT_TRUE(t.hasException<MyException>());
}

TEST(Try, tryEmplaceWithConstructorThrows) {
  struct MyException : std::exception {};
  struct ThrowingConstructor {
    int value_;
    explicit ThrowingConstructor(bool shouldThrow) noexcept(false) : value_(0) {
      if (shouldThrow) {
        throw MyException{};
      }
    }
  };

  Try<ThrowingConstructor> t;
  tryEmplaceWith(t, [] { return false; });
  EXPECT_TRUE(t.hasValue());
  tryEmplaceWith(t, [] { return true; });
  EXPECT_TRUE(t.hasException());
  EXPECT_TRUE(t.hasException<MyException>());
}

TEST(Try, tryEmplaceWithVoidTry) {
  Try<void> t;
  bool hasRun = false;
  tryEmplaceWith(t, [&] { hasRun = true; });
  EXPECT_TRUE(t.hasValue());
  EXPECT_TRUE(hasRun);

  struct MyException : std::exception {};
  tryEmplaceWith(t, [&] { throw MyException{}; });
  EXPECT_TRUE(t.hasException());
  EXPECT_TRUE(t.hasException<MyException>());
}

TEST(Try, nothrow) {
  using F = HasCtors<false>;
  using T = HasCtors<true>;

  // default ctor
  EXPECT_TRUE(std::is_nothrow_default_constructible<Try<F>>::value);
  EXPECT_TRUE(std::is_nothrow_default_constructible<Try<T>>::value);
  EXPECT_TRUE(std::is_nothrow_default_constructible<Try<void>>::value);

  // inner ctor - no void
  EXPECT_FALSE((std::is_nothrow_constructible<Try<F>, F&&>::value));
  EXPECT_TRUE((std::is_nothrow_constructible<Try<T>, T&&>::value));
  EXPECT_FALSE((std::is_nothrow_constructible<Try<F>, F const&>::value));
  EXPECT_TRUE((std::is_nothrow_constructible<Try<T>, T const&>::value));

  // emplacing ctor - no void
  EXPECT_FALSE(
      (std::is_nothrow_constructible<Try<F>, std::in_place_t, int>::value));
  EXPECT_TRUE(
      (std::is_nothrow_constructible<Try<T>, std::in_place_t, int>::value));

  // copy/move ctor/assign
  EXPECT_TRUE(std::is_nothrow_constructible<Try<void>>::value);
  EXPECT_FALSE(std::is_nothrow_move_constructible<Try<F>>::value);
  EXPECT_TRUE(std::is_nothrow_move_constructible<Try<T>>::value);
  EXPECT_TRUE(std::is_nothrow_move_constructible<Try<void>>::value);
  EXPECT_FALSE(std::is_nothrow_move_assignable<Try<F>>::value);
  EXPECT_TRUE(std::is_nothrow_move_assignable<Try<T>>::value);
  EXPECT_TRUE(std::is_nothrow_move_assignable<Try<void>>::value);
  EXPECT_FALSE(std::is_nothrow_copy_constructible<Try<F>>::value);
  EXPECT_TRUE(std::is_nothrow_copy_constructible<Try<T>>::value);
  EXPECT_TRUE(std::is_nothrow_copy_constructible<Try<void>>::value);
  EXPECT_FALSE(std::is_nothrow_copy_assignable<Try<F>>::value);
  EXPECT_TRUE(std::is_nothrow_copy_assignable<Try<T>>::value);
  EXPECT_TRUE(std::is_nothrow_copy_assignable<Try<void>>::value);

  // conversion ctor - void to unit
  EXPECT_TRUE((std::is_nothrow_constructible<Try<Unit>, Try<void>&&>::value));
  EXPECT_TRUE(
      (std::is_nothrow_constructible<Try<Unit>, Try<void> const&>::value));

  // conversion ctor - unit to void
  EXPECT_TRUE((std::is_nothrow_constructible<Try<void>, Try<Unit>&&>::value));
  EXPECT_TRUE(
      (std::is_nothrow_constructible<Try<void>, Try<Unit> const&>::value));
}

TEST(Try, MoveConversionFromVoidToUnit) {
  Try<void> source{make_exception_wrapper<std::runtime_error>("error")};

  Try<Unit> result{std::move(source)};

  EXPECT_TRUE(result.hasException<std::runtime_error>());
  EXPECT_FALSE(source.exception());
}

TEST(Try, MoveConversionFromUnitToVoid) {
  Try<Unit> source{make_exception_wrapper<std::runtime_error>("error")};

  Try<void> result{std::move(source)};

  EXPECT_TRUE(result.hasException<std::runtime_error>());
  EXPECT_FALSE(source.exception());
}

TEST(Try, MoveDereference) {
  auto ptr = std::make_unique<int>(1);
  auto t = Try<std::unique_ptr<int>>{std::move(ptr)};
  auto result = *std::move(t);
  EXPECT_EQ(*result, 1);
  EXPECT_TRUE(t.hasValue());
}

TEST(Try, MoveConstRvalue) {
  // tests to see if Try returns a const Rvalue, this is required in the case
  // where for example MutableContainer has a mutable memebr that is move only
  // and you want to fetch the value from the Try and move it into a member
  {
    const Try<MutableContainer> t{std::in_place};
    // NOLINTNEXTLINE(performance-move-const-arg)
    auto val = MoveConstructOnly(std::move(t).value().val);
    static_cast<void>(val);
  }
  {
    const Try<MutableContainer> t{std::in_place};
    // NOLINTNEXTLINE(performance-move-const-arg)
    auto val = (*(std::move(t))).val;
    static_cast<void>(val);
  }
}

TEST(Try, ValueOverloads) {
  using ML = int&;
  using MR = int&&;
  using CL = const int&;
  using CR = const int&&;

  {
    auto obj = Try<int>{};
    using ActualML = decltype(obj.value());
    using ActualMR = decltype(std::move(obj).value());
    using ActualCL = decltype(std::as_const(obj).value());
    // NOLINTNEXTLINE(performance-move-const-arg)
    using ActualCR = decltype(std::move(std::as_const(obj)).value());
    EXPECT_TRUE((std::is_same<ML, ActualML>::value));
    EXPECT_TRUE((std::is_same<MR, ActualMR>::value));
    EXPECT_TRUE((std::is_same<CL, ActualCL>::value));
    EXPECT_TRUE((std::is_same<CR, ActualCR>::value));
  }

  {
    auto obj = Try<int>{3};
    EXPECT_EQ(obj.value(), 3);
    EXPECT_EQ(std::move(obj).value(), 3);
    EXPECT_EQ(std::as_const(obj).value(), 3);
    // NOLINTNEXTLINE(performance-move-const-arg)
    EXPECT_EQ(std::move(std::as_const(obj)).value(), 3);
  }

  {
    auto obj = Try<int>{make_exception_wrapper<std::range_error>("oops")};
    EXPECT_THROW(std::ignore = obj.value(), std::range_error);
    EXPECT_THROW(std::ignore = std::move(obj).value(), std::range_error);
    EXPECT_THROW(std::ignore = std::as_const(obj).value(), std::range_error);
    // NOLINTNEXTLINE(performance-move-const-arg)
    EXPECT_THROW(
        std::ignore = std::move(std::as_const(obj)).value(), std::range_error);
  }
}

TEST(Try, ValueOr) {
  struct CopyableValue {
    int x;
  };
  {
    Try<CopyableValue> o{CopyableValue{42}};
    CopyableValue defaultValue{17};
    EXPECT_EQ(o.value_or(defaultValue).x, 42);
    EXPECT_EQ(o.value_or(defaultValue).x, (*o).x);
  }

  {
    Try<CopyableValue> empty;
    EXPECT_FALSE(empty.hasValue());
    CopyableValue defaultValue{17};
    EXPECT_EQ(empty.value_or(defaultValue).x, defaultValue.x);
  }

  {
    Try<std::unique_ptr<int>> o{std::make_unique<int>(42)};
    std::unique_ptr<int> defaultValue = std::make_unique<int>(17);
    std::unique_ptr<int> v = std::move(o).value_or(std::move(defaultValue));
    ASSERT_TRUE(v);
    EXPECT_EQ(*v, 42);
    ASSERT_TRUE(defaultValue);
    EXPECT_EQ(*defaultValue, 17);
    EXPECT_TRUE(o.hasValue());
    ASSERT_FALSE(*o);
  }

  {
    Try<std::unique_ptr<int>> empty;
    std::unique_ptr<int> defaultValue = std::make_unique<int>(17);
    std::unique_ptr<int> v = std::move(empty).value_or(std::move(defaultValue));
    ASSERT_TRUE(v);
    EXPECT_EQ(*v, 17);
    EXPECT_FALSE(defaultValue);
    EXPECT_FALSE(empty.hasValue());
  }
}

// Make sure we can copy Trys for copyable types
TEST(Try, copy) {
  Try<int> t;
  auto t2 = t;
}

// But don't choke on move-only types
TEST(Try, moveOnly) {
  Try<std::unique_ptr<int>> t;
  std::vector<Try<std::unique_ptr<int>>> v;
  v.reserve(10);
}

TEST(Try, makeTryWith) {
  auto func = []() { return std::make_unique<int>(1); };

  auto result = makeTryWith(func);
  EXPECT_TRUE(result.hasValue());
  EXPECT_EQ(*result.value(), 1);
}

TEST(Try, makeTryWithThrow) {
  auto func = []() -> std::unique_ptr<int> {
    throw std::runtime_error("Runtime");
  };

  auto result = makeTryWith(func);
  EXPECT_TRUE(result.hasException<std::runtime_error>());
}

TEST(Try, makeTryWithVoid) {
  auto func = []() { return; };

  auto result = makeTryWith(func);
  EXPECT_TRUE(result.hasValue());
}

TEST(Try, makeTryWithVoidThrow) {
  auto func = []() { throw std::runtime_error("Runtime"); };

  auto result = makeTryWith(func);
  EXPECT_TRUE(result.hasException<std::runtime_error>());
}

TEST(Try, exception) {
  using ML = exception_wrapper&;
  using MR = exception_wrapper&&;
  using CL = exception_wrapper const&;
  using CR = exception_wrapper const&&;

  {
    auto obj = Try<int>();
    using ActualML = decltype(obj.exception());
    using ActualMR = decltype(std::move(obj).exception());
    using ActualCL = decltype(std::as_const(obj).exception());
    // NOLINTNEXTLINE(performance-move-const-arg)
    using ActualCR = decltype(std::move(std::as_const(obj)).exception());
    EXPECT_TRUE((std::is_same<ML, ActualML>::value));
    EXPECT_TRUE((std::is_same<MR, ActualMR>::value));
    EXPECT_TRUE((std::is_same<CL, ActualCL>::value));
    EXPECT_TRUE((std::is_same<CR, ActualCR>::value));
  }

  {
    auto obj = Try<int>(3);
    EXPECT_THROW(obj.exception(), TryException);
    EXPECT_THROW(std::move(obj).exception(), TryException);
    EXPECT_THROW(std::as_const(obj).exception(), TryException);
    // NOLINTNEXTLINE(performance-move-const-arg)
    EXPECT_THROW(std::move(std::as_const(obj)).exception(), TryException);
  }

  {
    auto obj = Try<int>(make_exception_wrapper<int>(-3));
    EXPECT_EQ(-3, *obj.exception().get_exception<int>());
    EXPECT_EQ(-3, *std::move(obj).exception().get_exception<int>());
    EXPECT_EQ(-3, *std::as_const(obj).exception().get_exception<int>());
    // NOLINTNEXTLINE(performance-move-const-arg)
    EXPECT_EQ(
        -3, *std::move(std::as_const(obj)).exception().get_exception<int>());
  }

  {
    auto obj = Try<void>();
    using ActualML = decltype(obj.exception());
    using ActualMR = decltype(std::move(obj).exception());
    using ActualCL = decltype(std::as_const(obj).exception());
    // NOLINTNEXTLINE(performance-move-const-arg)
    using ActualCR = decltype(std::move(std::as_const(obj)).exception());
    EXPECT_TRUE((std::is_same<ML, ActualML>::value));
    EXPECT_TRUE((std::is_same<MR, ActualMR>::value));
    EXPECT_TRUE((std::is_same<CL, ActualCL>::value));
    EXPECT_TRUE((std::is_same<CR, ActualCR>::value));
  }

  {
    auto obj = Try<void>();
    EXPECT_THROW(obj.exception(), TryException);
    EXPECT_THROW(std::move(obj).exception(), TryException);
    EXPECT_THROW(std::as_const(obj).exception(), TryException);
    // NOLINTNEXTLINE(performance-move-const-arg)
    EXPECT_THROW(std::move(std::as_const(obj)).exception(), TryException);
  }

  {
    auto obj = Try<void>(make_exception_wrapper<int>(-3));
    EXPECT_EQ(-3, *obj.exception().get_exception<int>());
    EXPECT_EQ(-3, *std::move(obj).exception().get_exception<int>());
    EXPECT_EQ(-3, *std::as_const(obj).exception().get_exception<int>());
    // NOLINTNEXTLINE(performance-move-const-arg)
    EXPECT_EQ(
        -3, *std::move(std::as_const(obj)).exception().get_exception<int>());
  }
}

TEST(Try, tryGetExceptionObject) {
  auto epexn = std::make_exception_ptr(std::range_error("oops"));
  auto epnum = std::make_exception_ptr(17);

  auto exn = CHECK_NOTNULL(exception_ptr_get_object<std::range_error>(epexn));
  auto num = CHECK_NOTNULL(exception_ptr_get_object<int>(epnum));

  {
    auto t = Try<bool>(true);
    EXPECT_EQ(nullptr, t.tryGetExceptionObject());
    EXPECT_EQ(nullptr, t.tryGetExceptionObject<std::runtime_error>());
    EXPECT_EQ(nullptr, t.tryGetExceptionObject<int>());
  }

  {
    auto t = Try<bool>(exception_wrapper(epexn));
    EXPECT_EQ(exn, t.tryGetExceptionObject());
    EXPECT_EQ(exn, t.tryGetExceptionObject<std::runtime_error>());
    EXPECT_EQ(nullptr, t.tryGetExceptionObject<int>());
  }

  {
    auto t = Try<bool>(exception_wrapper(epnum));
    EXPECT_EQ(nullptr, t.tryGetExceptionObject());
    EXPECT_EQ(nullptr, t.tryGetExceptionObject<std::runtime_error>());
    EXPECT_EQ(num, t.tryGetExceptionObject<int>());
  }

  {
    auto t = Try<void>();
    EXPECT_EQ(nullptr, t.tryGetExceptionObject());
    EXPECT_EQ(nullptr, t.tryGetExceptionObject<std::runtime_error>());
    EXPECT_EQ(nullptr, t.tryGetExceptionObject<int>());
  }

  {
    auto t = Try<void>(exception_wrapper(epexn));
    EXPECT_EQ(exn, t.tryGetExceptionObject());
    EXPECT_EQ(exn, t.tryGetExceptionObject<std::runtime_error>());
    EXPECT_EQ(nullptr, t.tryGetExceptionObject<int>());
  }

  {
    auto t = Try<void>(exception_wrapper(epnum));
    EXPECT_EQ(nullptr, t.tryGetExceptionObject());
    EXPECT_EQ(nullptr, t.tryGetExceptionObject<std::runtime_error>());
    EXPECT_EQ(num, t.tryGetExceptionObject<int>());
  }

  {
    auto const t = Try<bool>(true);
    EXPECT_EQ(nullptr, t.tryGetExceptionObject());
    EXPECT_EQ(nullptr, t.tryGetExceptionObject<std::runtime_error>());
    EXPECT_EQ(nullptr, t.tryGetExceptionObject<int>());
  }

  {
    auto const t = Try<bool>(exception_wrapper(epexn));
    EXPECT_EQ(exn, t.tryGetExceptionObject());
    EXPECT_EQ(exn, t.tryGetExceptionObject<std::runtime_error>());
    EXPECT_EQ(nullptr, t.tryGetExceptionObject<int>());
  }

  {
    auto const t = Try<bool>(exception_wrapper(epnum));
    EXPECT_EQ(nullptr, t.tryGetExceptionObject());
    EXPECT_EQ(nullptr, t.tryGetExceptionObject<std::runtime_error>());
    EXPECT_EQ(num, t.tryGetExceptionObject<int>());
  }

  {
    auto const t = Try<void>();
    EXPECT_EQ(nullptr, t.tryGetExceptionObject());
    EXPECT_EQ(nullptr, t.tryGetExceptionObject<std::runtime_error>());
    EXPECT_EQ(nullptr, t.tryGetExceptionObject<int>());
  }

  {
    auto const t = Try<void>(exception_wrapper(epexn));
    EXPECT_EQ(exn, t.tryGetExceptionObject());
    EXPECT_EQ(exn, t.tryGetExceptionObject<std::runtime_error>());
    EXPECT_EQ(nullptr, t.tryGetExceptionObject<int>());
  }

  {
    auto const t = Try<void>(exception_wrapper(epnum));
    EXPECT_EQ(nullptr, t.tryGetExceptionObject());
    EXPECT_EQ(nullptr, t.tryGetExceptionObject<std::runtime_error>());
    EXPECT_EQ(num, t.tryGetExceptionObject<int>());
  }
}

TEST(Try, withException) {
  auto ew = make_exception_wrapper<std::range_error>("oops");

  {
    auto t = Try<bool>(true);
    EXPECT_FALSE(t.withException<std::runtime_error>([](auto&) {}));
    EXPECT_FALSE(t.withException<std::logic_error>([](auto&) {}));
    EXPECT_FALSE(t.withException([](std::runtime_error&) {}));
    EXPECT_FALSE(t.withException([](std::logic_error&) {}));
  }

  {
    auto t = Try<bool>(ew);
    EXPECT_TRUE(t.withException<std::runtime_error>([](auto&) {}));
    EXPECT_FALSE(t.withException<std::logic_error>([](auto&) {}));
    EXPECT_TRUE(t.withException([](std::runtime_error&) {}));
    EXPECT_FALSE(t.withException([](std::logic_error&) {}));
  }

  {
    auto t = Try<void>();
    EXPECT_FALSE(t.withException<std::runtime_error>([](auto&) {}));
    EXPECT_FALSE(t.withException<std::logic_error>([](auto&) {}));
    EXPECT_FALSE(t.withException([](std::runtime_error&) {}));
    EXPECT_FALSE(t.withException([](std::logic_error&) {}));
  }

  {
    auto t = Try<void>(ew);
    EXPECT_TRUE(t.withException<std::runtime_error>([](auto&) {}));
    EXPECT_FALSE(t.withException<std::logic_error>([](auto&) {}));
    EXPECT_TRUE(t.withException([](std::runtime_error&) {}));
    EXPECT_FALSE(t.withException([](std::logic_error&) {}));
  }

  {
    auto const t = Try<bool>(true);
    EXPECT_FALSE(t.withException<std::runtime_error>([](auto&) {}));
    EXPECT_FALSE(t.withException<std::logic_error>([](auto&) {}));
    EXPECT_FALSE(t.withException([](std::runtime_error const&) {}));
    EXPECT_FALSE(t.withException([](std::logic_error const&) {}));
  }

  {
    auto const t = Try<bool>(ew);
    EXPECT_TRUE(t.withException<std::runtime_error>([](auto&) {}));
    EXPECT_FALSE(t.withException<std::logic_error>([](auto&) {}));
    EXPECT_TRUE(t.withException([](std::runtime_error const&) {}));
    EXPECT_FALSE(t.withException([](std::logic_error const&) {}));
  }

  {
    auto const t = Try<void>();
    EXPECT_FALSE(t.withException<std::runtime_error>([](auto&) {}));
    EXPECT_FALSE(t.withException<std::logic_error>([](auto&) {}));
    EXPECT_FALSE(t.withException([](std::runtime_error const&) {}));
    EXPECT_FALSE(t.withException([](std::logic_error const&) {}));
  }

  {
    auto const t = Try<void>(ew);
    EXPECT_TRUE(t.withException<std::runtime_error>([](auto&) {}));
    EXPECT_FALSE(t.withException<std::logic_error>([](auto&) {}));
    EXPECT_TRUE(t.withException([](std::runtime_error const&) {}));
    EXPECT_FALSE(t.withException([](std::logic_error const&) {}));
  }
}

TEST(Try, TestUnwrapTuple) {
  auto original = std::make_tuple(Try<int>{1}, Try<int>{2});
  EXPECT_EQ(std::make_tuple(1, 2), unwrapTryTuple(original));
  EXPECT_EQ(std::make_tuple(1, 2), unwrapTryTuple(folly::copy(original)));
  EXPECT_EQ(std::make_tuple(1, 2), unwrapTryTuple(std::as_const(original)));
}

TEST(Try, TestUnwrapPair) {
  auto original = std::make_pair(Try<int>{1}, Try<int>{2});
  EXPECT_EQ(std::make_pair(1, 2), unwrapTryTuple(original));
  EXPECT_EQ(std::make_pair(1, 2), unwrapTryTuple(folly::copy(original)));
  EXPECT_EQ(std::make_pair(1, 2), unwrapTryTuple(std::as_const(original)));
}

TEST(Try, TestUnwrapForward) {
  using UPtr_t = std::unique_ptr<int>;
  auto original = std::make_tuple(Try<UPtr_t>{std::make_unique<int>(1)});
  auto unwrapped = unwrapTryTuple(std::move(original));
  EXPECT_EQ(*std::get<0>(unwrapped), 1);
}

TEST(Try, CopyConstructible) {
  EXPECT_TRUE(std::is_copy_constructible<Try<int>>::value);
  EXPECT_FALSE(std::is_copy_constructible<Try<MoveConstructOnly>>::value);
}

TEST(Try, CTAD) {
  folly::Try t1(folly::unit);
  folly::Try t2(42);
}

TEST(Try, voidMove) {
  EXPECT_TRUE(std::is_move_constructible<Try<void>>::value);
  EXPECT_TRUE(std::is_nothrow_move_constructible<Try<void>>::value);
  EXPECT_TRUE(std::is_move_assignable<Try<void>>::value);
  EXPECT_TRUE(std::is_nothrow_move_assignable<Try<void>>::value);
  // Still copyable.
  EXPECT_TRUE(std::is_copy_constructible<Try<void>>::value);
  EXPECT_TRUE(std::is_copy_assignable<Try<void>>::value);

  // Move-construct from a value.
  Try<void> v;
  Try<void> v2(std::move(v));
  EXPECT_TRUE(v2.hasValue());
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_TRUE(v.hasValue());

  // Move-construct from an exception: the exception transfers.
  Try<void> e(make_exception_wrapper<int>(-3));
  Try<void> e2(std::move(e));
  EXPECT_TRUE(e2.hasException());
  EXPECT_EQ(-3, *e2.exception().get_exception<int>());
  // Like Try<T>, the source keeps its state (with a moved-from wrapper).
  // The null type proves the wrapper moved rather than copied.
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_TRUE(e.hasException());
  EXPECT_EQ(nullptr, e.exception().type());

  // Move-assign, value to exception.
  Try<void> a;
  a = Try<void>(make_exception_wrapper<int>(-3));
  EXPECT_TRUE(a.hasException());
  EXPECT_EQ(-3, *a.exception().get_exception<int>());

  // Move-assign, exception to exception.
  Try<void> b(make_exception_wrapper<int>(-4));
  Try<void> bsrc(make_exception_wrapper<int>(-3));
  b = std::move(bsrc);
  EXPECT_TRUE(b.hasException());
  EXPECT_EQ(-3, *b.exception().get_exception<int>());
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(nullptr, bsrc.exception().type());

  // Move-assign, exception to value.
  Try<void> c(make_exception_wrapper<int>(-3));
  c = Try<void>();
  EXPECT_TRUE(c.hasValue());

  // Self-move-assign is a no-op.
  Try<void> s(make_exception_wrapper<int>(-3));
  s = std::move(std::move(s)); // suppress self-move warning
  EXPECT_TRUE(s.hasException());
  EXPECT_EQ(-3, *s.exception().get_exception<int>());
}
