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

#include <folly/coro/GtestHelpers.h>
#include <folly/coro/Timeout.h>
#include <folly/coro/safe/SafeTask.h>
#include <folly/fibers/Semaphore.h>
#include <folly/init/Init.h>
#include <folly/portability/GTest.h>

#if FOLLY_HAS_COROUTINES

using namespace folly;
using namespace folly::coro;
using namespace std::literals::chrono_literals;

struct StatelessClass {
  value_task<void> validSafeTask() { co_return; }
};

struct StatefulClass {
  int i;
};

struct SafeStatefulClass {
  int i;
  template <safe_alias>
  using folly_private_safe_alias_t =
      // The least-safe marking that `member_task` will accept
      safe_alias_constant<safe_alias::after_cleanup_ref>;
};

struct UnsafeStatefulClass {
  int i;
  template <safe_alias>
  using folly_private_safe_alias_t =
      // The most-safe marking that `member_task` won't accept
      safe_alias_constant<safe_alias::shared_cleanup>;
};

TEST(SafeTask, isSafeTaskValid) {
  using folly::coro::detail::is_safe_task_valid;
  constexpr auto kVal = safe_alias::maybe_value;
  constexpr auto kPost = safe_alias::co_cleanup_safe_ref;
  constexpr auto kPre = safe_alias::after_cleanup_ref;
  constexpr auto kMember = safe_alias::unsafe_member_internal;
  constexpr auto kClosure = safe_alias::unsafe_closure_internal;

  // Without an implicit object parameter
  static_assert(is_safe_task_valid<kVal, int, int>);
  static_assert(!is_safe_task_valid<kVal, int, int*>);
  static_assert(!is_safe_task_valid<kVal, int*, int>);
  static_assert(is_safe_task_valid<kVal, void, int>);
  static_assert(!is_safe_task_valid<kVal, void, int*>);

  // With an implicit "class" object parameter
  static_assert(is_safe_task_valid<kVal, int, StatelessClass&, int>);
  static_assert(is_safe_task_valid<kVal, int, const StatelessClass&, int>);
  static_assert(!is_safe_task_valid<kVal, int, StatefulClass&, int>);
  static_assert(!is_safe_task_valid<kVal, int, const StatefulClass&, int>);

  // Unlike `closure_task`, `member_task` tolerates stateful classes iff their
  // safety is explicitly marked.
  // (1) Stateless
  static_assert(is_safe_task_valid<kClosure, int, StatelessClass&, int>);
  static_assert(is_safe_task_valid<kClosure, int, const StatelessClass&, int>);
  static_assert(is_safe_task_valid<kMember, int, StatelessClass&, int>);
  static_assert(is_safe_task_valid<kMember, int, const StatelessClass&, int>);
  // (2) Stateful, unmarked
  static_assert(!is_safe_task_valid<kClosure, int, StatefulClass&, int>);
  static_assert(!is_safe_task_valid<kClosure, int, const StatefulClass&, int>);
  static_assert(!is_safe_task_valid<kMember, int, StatefulClass&, int>);
  static_assert(!is_safe_task_valid<kMember, int, const StatefulClass&, int>);
  // (3) Stateful, marked safe enough
  static_assert(!is_safe_task_valid<kClosure, int, SafeStatefulClass&, int>);
  static_assert(
      !is_safe_task_valid<kClosure, int, const SafeStatefulClass&, int>);
  static_assert(is_safe_task_valid<kMember, int, SafeStatefulClass&, int>);
  static_assert(
      is_safe_task_valid<kMember, int, const SafeStatefulClass&, int>);
  // (4) Stateful, marked not safe enough
  static_assert(!is_safe_task_valid<kClosure, int, UnsafeStatefulClass&, int>);
  static_assert(
      !is_safe_task_valid<kClosure, int, const UnsafeStatefulClass&, int>);
  static_assert(!is_safe_task_valid<kMember, int, UnsafeStatefulClass&, int>);
  static_assert(
      !is_safe_task_valid<kMember, int, const UnsafeStatefulClass&, int>);

  // With an implicit "lambda" object parameter
  auto okFn = [](int x) -> value_task<int> { co_return x; };
  static_assert(is_safe_task_valid<kVal, int, decltype(okFn)&, int>);
  static_assert(is_safe_task_valid<kVal, int, const decltype(okFn)&, int>);
  // Can declare this with captures because it's not a coro
  auto badFn = [okFn](int x) -> value_task<int> { return okFn(x); };
  static_assert(!is_safe_task_valid<kVal, int, decltype(badFn)&, int>);
  static_assert(!is_safe_task_valid<kVal, int, const decltype(badFn)&, int>);

  // With a templated implicit object parameter
  auto okTmpl = [](auto x) -> value_task<int> { co_return x; };
  static_assert(is_safe_task_valid<kVal, int, decltype(okTmpl)&, int>);
  static_assert(is_safe_task_valid<kVal, int, const decltype(okTmpl)&, int>);
  // Can declare this with captures because it's not a coro
  auto badTmpl = [okTmpl](auto x) -> value_task<int> { return okTmpl(x); };
  static_assert(!is_safe_task_valid<kVal, int, decltype(badTmpl)&, int>);
  static_assert(!is_safe_task_valid<kVal, int, const decltype(badTmpl)&, int>);

  // safe_alias::after_cleanup_ref relaxes constraint on args, but not return
  // val
  static_assert(is_safe_task_valid<kPre, int, manual_safe_ref_t<kPre, int>>);
  static_assert(is_safe_task_valid<kPre, int, manual_safe_ref_t<kPost, int>>);
  static_assert(!is_safe_task_valid<kPre, int, int*>);
  static_assert(!is_safe_task_valid<kPre, int*, int>);
  static_assert(!is_safe_task_valid<kPre, manual_safe_ref_t<kPre, int>, int>);
  static_assert(!is_safe_task_valid<kPre, manual_safe_ref_t<kPost, int>, int>);
  static_assert(is_safe_task_valid<kPre, void, int>);
  static_assert(!is_safe_task_valid<kPre, void, int*>);

  // Ditto for safe_alias::co_cleanup_safe_ref
  static_assert(!is_safe_task_valid<kPost, int, manual_safe_ref_t<kPre, int>>);
  static_assert(is_safe_task_valid<kPost, int, manual_safe_ref_t<kPost, int>>);
  static_assert(!is_safe_task_valid<kPost, int, int*>);
  static_assert(!is_safe_task_valid<kPost, int*, int>);
  static_assert(!is_safe_task_valid<kPost, manual_safe_ref_t<kPre, int>, int>);
  static_assert(!is_safe_task_valid<kPost, manual_safe_ref_t<kPost, int>, int>);
  static_assert(is_safe_task_valid<kPost, void, int>);
  static_assert(!is_safe_task_valid<kPost, void, int*>);
}

TEST(SafeTask, safe_alias_of_v) {
  static_assert(
      strict_safe_alias_of_v<value_task<int>> == safe_alias::maybe_value);
  static_assert(
      lenient_safe_alias_of_v<value_task<int>> == safe_alias::maybe_value);
  static_assert(
      lenient_safe_alias_of_v<safe_task<safe_alias::after_cleanup_ref, int>> ==
      safe_alias::after_cleanup_ref);
}

CO_TEST(SafeTask, trivial) {
  EXPECT_EQ(
      1337, co_await [](int x) -> value_task<int> { co_return 1300 + x; }(37));
}

CO_TEST(CoCleanupSafeTask, trivial) {
  int x = 37;
  auto t = [](auto x) -> co_cleanup_safe_task<int> { co_return 1300 + x; };
  EXPECT_EQ(
      1337, co_await t(manual_safe_ref<safe_alias::co_cleanup_safe_ref>(x)));
  EXPECT_EQ(1337, co_await t(manual_safe_ref(x)));
}

CO_TEST(PreCleanupTask, trivial) {
  int x = 37;
  auto t = [](auto x) -> safe_task<safe_alias::after_cleanup_ref, int> {
    co_return 1300 + x;
  };
  EXPECT_EQ(
      1337, co_await t(manual_safe_ref<safe_alias::after_cleanup_ref>(x)));
  EXPECT_EQ(
      1337, co_await t(manual_safe_ref<safe_alias::co_cleanup_safe_ref>(x)));
  EXPECT_EQ(1337, co_await t(manual_safe_ref(x)));
}

namespace {
value_task<int> intFunc(auto x) {
  co_return *x;
}
} // namespace

CO_TEST(SafeTask, returnsNonVoid) {
  auto x = std::make_unique<int>(17);
  auto lambdaTmpl = [](auto x) -> value_task<int> { co_return x; };
  EXPECT_EQ(
      20,
      // Would fail to compile with a raw pointer (i.e. `.get()`)
      co_await intFunc(std::move(x)) + co_await lambdaTmpl(3));
}

namespace {
value_task<void> voidFunc(auto x) {
  EXPECT_EQ(17, *x);
  co_return;
}
} // namespace

CO_TEST(SafeTask, returnsVoid) {
  auto lambdaTmpl = [](auto x) -> value_task<void> {
    EXPECT_EQ(3, x);
    co_return;
  };
  co_await lambdaTmpl(3);
  auto x = std::make_unique<int>(17);
#if 1
  co_await voidFunc(std::move(x));
#else // Manual test: passing `int*` breaks the build with "Bad safe_task"
  co_await voidFunc(x.get());
#endif
}

CO_TEST(SafeTask, awaitsTask) {
  EXPECT_EQ(
      1337, co_await []() -> value_task<int> {
        co_return 1300 + co_await ([]() -> Task<int> { co_return 37; }());
      }());
}

CO_TEST(SafeTask, cancellation) {
  EXPECT_THROW(
      co_await timeout(
          []() -> value_task<void> {
            folly::fibers::Semaphore stuck{0}; // a cancellable baton
            co_await stuck.co_wait();
          }(),
          200ms),
      folly::FutureTimeout);
}

namespace {
struct MyError : std::exception {};
} // namespace

CO_TEST(SafeTask, throws) {
  EXPECT_THROW(
      co_await []() -> value_task<void> { co_yield co_error(MyError{}); }(),
      MyError);
}

CO_TEST(SafeTask, co_awaitTry) {
  auto res = co_await co_awaitTry([]() -> value_task<void> {
    co_yield co_error(MyError{});
  }());
  EXPECT_TRUE(res.hasException<MyError>());
}

namespace folly::coro::detail {

struct SafeTaskTest : testing::Test {
  template <safe_alias NewSafety>
  auto withNewSafety(auto t) {
    return std::move(t).template withNewSafety<NewSafety>();
  }
};

// DO NOT COPY THIS!  `withNewSafety` is a hacks meant EXCLUSIVELY for the
// `async_closure` implementation.
CO_TEST_F(SafeTaskTest, withNewSafety) {
  int x = 7;
  auto t = withNewSafety<safe_alias::maybe_value>(
      [](auto x) -> safe_task<safe_alias::shared_cleanup, int> {
        co_return 30 + x;
      }(manual_safe_ref<safe_alias::shared_cleanup>(x)));
  static_assert(std::is_same_v<decltype(t), value_task<int>>);
  EXPECT_EQ(37, co_await std::move(t));
}

CO_TEST_F(SafeTaskTest, ClosureTask) {
  int x = 37;
  auto t = [](auto x) -> closure_task<int> { co_return 1300 + x; };
  // These must be unwrapped to be awaited. The "new safety" is incidental.
  EXPECT_EQ(
      1337,
      co_await withNewSafety<safe_alias::maybe_value>(
          t(manual_safe_ref<safe_alias::shared_cleanup>(x))));
  EXPECT_EQ(
      1337,
      co_await withNewSafety<safe_alias::maybe_value>(t(manual_safe_ref(x))));
}

struct HasMemberTask {
  member_task<int> task(auto x) { co_return 1300 + x; }
};

static_assert(!std::is_move_constructible_v<member_task<int>>);
static_assert(!std::is_move_assignable_v<member_task<int>>);

CO_TEST_F(SafeTaskTest, MemberTask) {
  HasMemberTask mt;
  int x = 37;
  EXPECT_EQ(1337, co_await mt.task(x));
  EXPECT_EQ(
      1337, co_await mt.task(manual_safe_ref<safe_alias::shared_cleanup>(x)));
  EXPECT_EQ(1337, co_await mt.task(manual_safe_ref(x)));
}

static_assert(std::is_void_v<await_result_t<
                  safe_task_with_executor<safe_alias::maybe_value, void>>>);
static_assert(
    std::is_same_v<
        int,
        await_result_t<safe_task_with_executor<safe_alias::maybe_value, int>>>);

} // namespace folly::coro::detail

#endif

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  folly::Init init(&argc, &argv); // `timeout` uses a `Timekeeper` singleton
  return RUN_ALL_TESTS();
}
