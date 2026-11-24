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

#include <folly/coro/AsyncGenerator.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/GtestHelpers.h>
#include <folly/coro/safe/NowTask.h>

// Throughout this file, we have blocks of static asserts that follow the same
// pattern:
//  - Do some trait tests for movable `Task`, making sure they come out as
//    expected.  This serves mainly to validate the trait actually checks what
//    we want it to test.
//  - Then, flip to `now_task` and confirm that the "move" action fails.

#if FOLLY_HAS_COROUTINES

namespace folly::coro {

static_assert(lenient_safe_alias_of_v<Task<int>> == safe_alias::unsafe);
static_assert(lenient_safe_alias_of_v<now_task<int>> == safe_alias::unsafe);

static_assert(std::is_void_v<await_result_t<now_task_with_executor<void>>>);
static_assert(std::is_same_v<int, await_result_t<now_task_with_executor<int>>>);

Task<int> demoTask(int x) {
  co_return 1300 + x;
}
now_task<int> demoNowTask(int x) {
  co_return 1300 + x;
}

template <typename T, typename Res = int>
inline constexpr bool test_semi_await_result_v =
    std::is_same_v<detected_t<semi_await_result_t, T>, Res>;

static_assert(test_semi_await_result_v<Task<int>>);
static_assert(!test_semi_await_result_v<Task<int>&>);
static_assert(test_semi_await_result_v<Task<int>&&>);
static_assert(test_semi_await_result_v<now_task<int>>);
static_assert(!test_semi_await_result_v<now_task<int>&>);
static_assert(!test_semi_await_result_v<now_task<int>&&>);

using DemoTryTask = decltype(co_awaitTry(demoTask(37)));
using DemoTryNowTask = decltype(co_awaitTry(demoNowTask(37)));
static_assert(test_semi_await_result_v<DemoTryTask, Try<int>>);
static_assert(!test_semi_await_result_v<DemoTryTask&, Try<int>>);
static_assert(test_semi_await_result_v<DemoTryTask&&, Try<int>>);
static_assert(test_semi_await_result_v<DemoTryNowTask, Try<int>>);
static_assert(!test_semi_await_result_v<DemoTryNowTask&, Try<int>>);
static_assert(!test_semi_await_result_v<DemoTryNowTask&&, Try<int>>);

// Note: This `test_` predicate, and similar ones below, may look somewhat
// redundant with `test_semi_await_result_v` above.  Both aim to test this:
//   co_await demoNowTask(37); // works
//   auto t = demoNowTask(37);
//   co_await std::move(t); // does not compile
// We check against test bugs by ensuring that BOTH forms work for `Task`.
//
// The rationale for the supposed redundancy is that here, we spell out the
// expected call-path-to-awaiter.  This is pretty robust, so long as I check
// both `Task` (good) and `now_task` (bad), whereas with the fancy
// metaprogramming of `semi_await_result_t`, there's more risk that
// `co_await std::move(t)` compiles for `now_task`, even when the type
// function fails to substitute.
template <typename T>
using await_transform_result_t =
    decltype(std::declval<detail::TaskPromise<void>>().await_transform(
        FOLLY_DECLVAL(T)));
template <typename T>
inline constexpr bool test_transform_moved_v = std::is_same_v<
    detected_t<await_transform_result_t, T>,
    typename Task<int>::PrivateAwaiterTypeForTests>;

CO_TEST(NowTaskTest, simple) {
  EXPECT_EQ(1337, co_await demoNowTask(37));

  static_assert(test_transform_moved_v<Task<int>>);
  // Won't compile: static_assert(!test_transform_moved_v<Task<int>&>);
  static_assert(test_transform_moved_v<Task<int>&&>);
  static_assert(test_transform_moved_v<now_task<int>>);
  static_assert(!test_transform_moved_v<now_task<int>&>);
  static_assert(!test_transform_moved_v<now_task<int>&&>);
#if 0 // The above asserts approximate this manual test
  auto t = demoNowTask(37);
  co_await std::move(t);
#endif
}

template <typename T>
using co_withExecutor_result_t = decltype(co_withExecutor(
    FOLLY_DECLVAL(Executor::KeepAlive<>), FOLLY_DECLVAL(T)));
// Check type of `co_withExecutor(...)` for task prvalue or ref inputs.
template <typename T, typename TWithExec>
inline constexpr bool test_move_into_co_withExecutor_v =
    std::is_same_v<detected_t<co_withExecutor_result_t, T>, TWithExec>;

// Check type of `co_await @`, where `@` is a task-with-executor prvalue or ref
template <typename T>
inline constexpr bool test_transform_moved_with_executor_v = std::is_same_v<
    detected_t<await_transform_result_t, T>,
    StackAwareViaIfAsyncAwaitable<TaskWithExecutor<int>>>;

// Check that `AsyncGenerator::await_transform` behaves like that of `Task`.
CO_TEST(NowTaskTest, awaitFromGenerator) {
  auto now30 = []() -> now_task<int> { co_return 30; };
  auto now7 = []() -> now_task<int> { co_return 7; };
  auto genFn = [&]() -> AsyncGenerator<int> {
    co_yield co_await now30();
    co_yield co_await co_nothrow(now7());
  };
  auto gen = genFn();
  EXPECT_EQ(30, *(co_await gen.next()));
  EXPECT_EQ(7, *(co_await gen.next()));
}

CO_TEST(NowTaskTest, withExecutor) {
  auto exec = co_await co_current_executor;
  EXPECT_EQ(1337, co_await co_withExecutor(exec, demoNowTask(37)));

  // Check passing a task/ref into `co_withExecutor()`.
  static_assert(
      test_move_into_co_withExecutor_v<Task<int>, TaskWithExecutor<int>>);
  static_assert(
      !test_move_into_co_withExecutor_v<Task<int>&, TaskWithExecutor<int>>);
  static_assert(
      test_move_into_co_withExecutor_v<Task<int>&&, TaskWithExecutor<int>>);
  static_assert(
      test_move_into_co_withExecutor_v<
          now_task<int>,
          now_task_with_executor<int>>);
  static_assert(
      !test_move_into_co_withExecutor_v<
          now_task<int>&,
          now_task_with_executor<int>>);
  static_assert(
      !test_move_into_co_withExecutor_v<
          now_task<int>&&,
          now_task_with_executor<int>>);
#if 0 // The above asserts approximate this manual test
  auto t = demoNowTask(37);
  co_await co_withExecutor(exec, std::move(t));
#endif

  // Check awaiting a task-with-executor or ref.
  static_assert(test_transform_moved_with_executor_v<TaskWithExecutor<int>>);
  // Won't compile:
  // static_assert(!test_transform_moved_with_executor_v<TaskWithExecutor<int>&>);
  static_assert(test_transform_moved_with_executor_v<TaskWithExecutor<int>&&>);
  static_assert(
      test_transform_moved_with_executor_v<now_task_with_executor<int>>);
  static_assert(
      !test_transform_moved_with_executor_v<now_task_with_executor<int>&>);
  static_assert(
      !test_transform_moved_with_executor_v<now_task_with_executor<int>&&>);
#if 0 // The above asserts approximate this manual test
  auto twe = co_withExecutor(exec, demoNowTask(37));
  co_await std::move(twe);
#endif
}

// `co_nothrow` isn't a function object, so we can't wrap it & pass prvalues
template <typename T>
using co_nothrow_result_t = decltype(co_nothrow(FOLLY_DECLVAL(T)));
template <typename T>
inline constexpr bool test_make_co_nothrow_v = std::is_same_v<
    detected_t<co_nothrow_result_t, T>,
    detail::NothrowAwaitable<std::remove_reference_t<T>>>;

CO_TEST(NowTaskTest, nothrow) {
  EXPECT_EQ(1337, co_await co_nothrow(demoNowTask(37)));

  static_assert(test_make_co_nothrow_v<Task<int>>);
  // False -- there's no substitution failure, but instantiating the awaitable
  // is an error.  This doesn't invalidate the test.
  // static_assert(!test_make_co_nothrow_v<Task<int>&>);
  static_assert(test_make_co_nothrow_v<Task<int>&&>);
  static_assert(test_make_co_nothrow_v<now_task<int>>);
  static_assert(!test_make_co_nothrow_v<now_task<int>&>);
  static_assert(!test_make_co_nothrow_v<now_task<int>&&>);
#if 0 // The above asserts approximate this manual test
  auto t = demoNowTask(37);
  co_nothrow(std::move(t));
#endif

  using DemoNothrowTask = decltype(co_nothrow(demoTask(37)));
  using DemoNothrowNowTask = decltype(co_nothrow(demoNowTask(37)));
  static_assert(test_transform_moved_v<DemoNothrowTask>);
  // Won't compile: static_assert(!test_transform_moved_v<DemoNothrowTask&>);
  static_assert(test_transform_moved_v<DemoNothrowTask&&>);
  static_assert(test_transform_moved_v<DemoNothrowNowTask>);
  static_assert(!test_transform_moved_v<DemoNothrowNowTask&>);
  static_assert(!test_transform_moved_v<DemoNothrowNowTask&&>);
#if 0 // The above asserts approximate this manual test
  auto t = co_nothrow(demoNowTask(37));
  co_await std::move(t);
#endif

  // Check that the "nothrow" functionality still works
  struct MyErr : std::exception {};
  auto errTask = []() -> now_task<> { co_yield co_error{MyErr{}}; };
  auto outerTask = [&]() -> now_task<> {
    try {
      co_await co_nothrow(errTask());
    } catch (...) {
      ADD_FAILURE() << "Not reached";
    }
  };
  EXPECT_THROW(co_await outerTask(), MyErr);
}

// `TryAwaitable` has a custom `operator co_await`, unlike `simple` and
// `nothrow` that just return an awaiter from `await_transform`.
template <typename T>
using co_await_and_transform_result_t = decltype(operator co_await(
    std::declval<detail::TaskPromise<void>>().await_transform(
        FOLLY_DECLVAL(T))));
template <typename T>
inline constexpr bool test_transform_and_await_moved_v = std::is_same_v<
    detected_t<co_await_and_transform_result_t, T>,
    detail::TryAwaiter<typename Task<int>::PrivateAwaiterTypeForTests>>;

// `co_awaitTry` isn't a function object, so we can't wrap it & pass prvalues
template <typename T>
using co_awaitTry_result_t = decltype(co_awaitTry(FOLLY_DECLVAL(T)));
template <typename T>
inline constexpr bool test_make_co_awaitTry_v = std::is_same_v<
    detected_t<co_awaitTry_result_t, T>,
    detail::TryAwaitable<std::remove_reference_t<T>>>;

CO_TEST(NowTaskTest, awaitTry) {
  EXPECT_EQ(1337, *(co_await co_awaitTry(demoNowTask(37))));

  static_assert(test_make_co_awaitTry_v<Task<int>>);
  // False -- there's no substitution failure, but instantiating the awaitable
  // is an error.  This doesn't invalidate the test.
  // static_assert(!test_make_co_awaitTry_v<Task<int>&>);
  static_assert(test_make_co_awaitTry_v<Task<int>&&>);
  static_assert(test_make_co_awaitTry_v<now_task<int>>);
  static_assert(!test_make_co_awaitTry_v<now_task<int>&>);
  static_assert(!test_make_co_awaitTry_v<now_task<int>&&>);
#if 0 // The above asserts approximate this manual test
  auto t = demoNowTask(37);
  co_awaitTry(std::move(t));
#endif

  static_assert(test_transform_and_await_moved_v<DemoTryTask>);
  // Won't compile:
  // static_assert(!test_transform_and_await_moved_v<DemoTryTask&>);
  static_assert(test_transform_and_await_moved_v<DemoTryTask&&>);
  static_assert(test_transform_and_await_moved_v<DemoTryNowTask>);
  static_assert(!test_transform_and_await_moved_v<DemoTryNowTask&>);
  static_assert(!test_transform_and_await_moved_v<DemoTryNowTask&&>);
#if 0 // The above asserts approximate this manual test
  auto t = co_awaitTry(demoNowTask(37));
  co_await std::move(t);
#endif
}

// `invoke_result_t` cannot pass prvalues -- it invokes a move ctor.
template <typename T>
using blockingWait_result_t = decltype(blockingWait(FOLLY_DECLVAL(T)));
template <typename T, typename Res>
inline constexpr bool test_blocking_wait_moved_v =
    std::is_same_v<detected_t<blockingWait_result_t, T>, Res>;

TEST(NowTaskTest, blockingWait) {
  EXPECT_EQ(1337, blockingWait(demoNowTask(37)));

  static_assert(test_blocking_wait_moved_v<Task<int>, int>);
  static_assert(!test_blocking_wait_moved_v<Task<int>&, int>);
  static_assert(test_blocking_wait_moved_v<Task<int>&&, int>);
  static_assert(test_blocking_wait_moved_v<now_task<int>, int>);
  static_assert(!test_blocking_wait_moved_v<now_task<int>&, int>);
  static_assert(!test_blocking_wait_moved_v<now_task<int>&&, int>);
#if 0 // The above asserts approximate this manual test
  auto t = demoNowTask(37);
  blockingWait(std::move(t));
#endif
}

TEST(NowTaskTest, blockingWaitTry) {
  EXPECT_EQ(1337, *blockingWait(co_awaitTry(demoNowTask(37))));

  static_assert(test_blocking_wait_moved_v<DemoTryTask, Try<int>>);
  static_assert(!test_blocking_wait_moved_v<DemoTryTask&, Try<int>>);
  static_assert(test_blocking_wait_moved_v<DemoTryTask&&, Try<int>>);
  static_assert(test_blocking_wait_moved_v<DemoTryNowTask, Try<int>>);
  static_assert(!test_blocking_wait_moved_v<DemoTryNowTask&, Try<int>>);
  static_assert(!test_blocking_wait_moved_v<DemoTryNowTask&&, Try<int>>);
#if 0 // The above asserts approximate this manual test
  auto t = co_awaitTry(demoNowTask(37));
  blockingWait(std::move(t));
#endif
}

// Both of these are antipatterns with `Task` because if you awaited either
// of these coros outside of the statement that created them, it would have
// dangling refs.
//
// Since `now_task` tries to ensure it can ONLY be awaited in the statement
// that created it, C++ lifetime extension should save our bacon.
CO_TEST(NowTaskTest, passByRef) {
  auto res = co_await [](int&& x) -> now_task<int> { co_return 1300 + x; }(37);
  EXPECT_EQ(1337, res);
}
CO_TEST(NowTaskTest, lambdaWithCaptures) {
  int a = 1300, b = 37;
  auto res = co_await [&a, b]() -> now_task<int> { co_return a + b; }();
  EXPECT_EQ(1337, res);
}

CO_TEST(NowTaskTest, to_now_task) {
  static_assert(
      std::is_same_v<now_task<int>, decltype(to_now_task(demoNowTask(5)))>);
  auto t = []() -> Task<int> { co_return 5; }();
  static_assert(
      std::is_same_v<now_task<int>, decltype(to_now_task(std::move(t)))>);
  EXPECT_EQ(5, co_await to_now_task(std::move(t)));
}

// Test `as_unsafe()` escape hatch for `now_task` and `now_task_with_executor`.
// These methods are marked deprecated, intended use is only for future->coro
// migrations.
FOLLY_PUSH_WARNING
FOLLY_GNU_DISABLE_WARNING("-Wdeprecated-declarations")

CO_TEST(NowTaskTest, asUnsafe) {
  static_assert(
      std::is_same_v<
          Task<int>,
          decltype(std::declval<now_task<int>>().as_unsafe())>);

  // Passing by value is safe here
  // @lint-ignore CLANGTIDY facebook-hte-Deprecated
  EXPECT_EQ(1337, co_await demoNowTask(37).as_unsafe());
}

CO_TEST(NowTaskTest, asUnsafeWithExecutor) {
  auto exec = co_await co_current_executor;
  static_assert(
      std::is_same_v<
          TaskWithExecutor<int>,
          decltype(std::declval<now_task_with_executor<int>>().as_unsafe())>);

  // Passing by value is safe here
  // @lint-ignore CLANGTIDY facebook-hte-Deprecated
  EXPECT_EQ(1337, co_await co_withExecutor(exec, demoNowTask(37)).as_unsafe());
}

// Recommended migration patterns when using `now_task` with `SemiFuture`.
// See `folly/coro/safe/docs/AsUnsafe.md` for full documentation.
now_task<int> processData(const int& x) {
  // Force suspension, trigger potential lifetime issues with caller's stack
  co_await folly::coro::co_reschedule_on_current_executor;
  co_return 1300 + x;
}

// Pattern 1 (Preferred): Use `co_invoke` to allocate argument copies.
folly::SemiFuture<int> processDataCoInvoke(const int& x) {
  // co_invoke makes decay-copies of arguments into a new coro frame,
  // guaranteeing lifetime for the duration of `processData`

  // @lint-ignore CLANGTIDY facebook-folly-coro-return-captures-local-var
  return folly::coro::co_invoke(
             [](auto&&... args) {
               // @lint-ignore CLANGTIDY facebook-hte-Deprecated
               return processData(std::forward<decltype(args)>(args)...)
                   .as_unsafe();
             },
             x)
      .semi();
}

// Pattern 2: Use `deferValue` to extend heap-allocated data lifetime.
folly::SemiFuture<int> processDataDefer(const int& x) {
  // `make_unique` makes a copy onto the heap, but this would normally be
  // destroyed at the return of `processDataDefer`.
  // In order to guarantee lifetime for the duration of `processData` we must
  // move the underlying data into the `deferValue` lambda
  auto dataPtr = std::make_unique<int>(x);

  // workaround for `facebook-hte-MoveEvaluationOrder`
  auto rawPtr = dataPtr.get();

  // @lint-ignore CLANGTIDY facebook-hte-Deprecated
  // @lint-ignore CLANGTIDY facebook-folly-coro-temporary-by-ref
  return processData(*rawPtr).as_unsafe().semi().deferValue(
      [dataPtr = std::move(dataPtr)](int result) { return result; });
}

TEST(NowTaskTest, asUnsafeSemiFutureWithCoInvokeGuard) {
  // Good hygiene is to `.get()` futures derived from `now_task` on the
  // same line that creates them. Here, we intentionally use multi-line to
  // demonstrate that the future became self-contained despite
  // the by-ref args in `processData`.

  auto future = processDataCoInvoke(37);
  auto result = std::move(future).get();
  EXPECT_EQ(result, 1337);
}

TEST(NowTaskTest, asUnsafeSemiFutureWithDeferGuard) {
  // Bad code hygiene, refer to `asUnsafeSemiFutureWithCoInvokeGuard`
  auto future = processDataDefer(37);
  auto result = std::move(future).get();
  EXPECT_EQ(result, 1337);
}

FOLLY_POP_WARNING

} // namespace folly::coro

#endif
