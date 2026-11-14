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

#include <folly/Conv.h>
#include <folly/Portability.h>
#include <folly/container/F14Map.h>

#include <folly/coro/Baton.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/GtestHelpers.h>
#include <folly/coro/Invoke.h>
#include <folly/coro/Mutex.h>
#include <folly/coro/SharedMutex.h>
#include <folly/coro/Task.h>
#include <folly/coro/ValueOrError.h>
#include <folly/coro/detail/InlineTask.h>
#include <folly/executors/InlineExecutor.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/futures/Future.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/portability/GTest.h>
#include <folly/result/coro.h>

#include <stdexcept>
#include <type_traits>

#if FOLLY_HAS_COROUTINES

using namespace folly;

constexpr bool check_for_size_regressions() {
  namespace detail = folly::coro::detail;

  static_assert(sizeof(coro::Task<>) == sizeof(void*));
  static_assert(sizeof(coro::Task<int>) == sizeof(void*));

  // Prevent size regressions due to member or base ordering
  constexpr size_t promiseSize =
      // From TaskPromiseBase:
      sizeof(coro::ExtendedCoroutineHandle) + sizeof(folly::AsyncStackFrame) +
      sizeof(folly::Executor::KeepAlive<>) + sizeof(folly::CancellationToken) +
      sizeof(coro::coroutine_handle<detail::ScopeExitTaskPromiseBase>) +
      // hasCancelTokenOverride_ and bypassExceptionThrowing_ should pack into
      sizeof(void*) +
      // From TaskPromiseCrtpBase:
      sizeof(Try<int>) +
      // From ExtendedCoroutinePromiseCrtp:
      sizeof(coro::ExtendedCoroutineHandle::PromiseBase);
  static_assert(sizeof(detail::TaskPromise<void>) == promiseSize);
  static_assert(sizeof(detail::TaskPromise<int>) == promiseSize);

  return true;
}
static_assert(check_for_size_regressions());

static_assert( //
    std::is_same<
        folly::coro::semi_await_result_t<folly::coro::Task<void>>,
        void>::value);
static_assert( //
    std::is_same<
        folly::coro::semi_await_result_t<folly::coro::Task<int>>,
        int>::value);

static_assert(
    std::is_same<
        folly::coro::semi_await_result_t<folly::coro::detail::InlineTask<void>>,
        void>::value);
static_assert(
    std::is_same<
        folly::coro::semi_await_result_t<folly::coro::detail::InlineTask<int>>,
        int>::value);

static_assert(
    std::is_same<folly::coro::semi_await_result_t<folly::coro::Baton&>, void>::
        value);
static_assert(
    std::is_same<
        folly::coro::semi_await_result_t<
            decltype(std::declval<folly::coro::SharedMutex&>()
                         .co_scoped_lock_shared())>,
        folly::coro::SharedLock<folly::coro::SharedMutex>>::value);

namespace {

const RequestToken testToken1("corotest1");
const RequestToken testToken2("corotest2");

class TestRequestData : public RequestData {
 public:
  explicit TestRequestData(std::string key) noexcept : key_(std::move(key)) {}

  bool hasCallback() override { return false; }

  const std::string& key() const noexcept { return key_; }

 private:
  std::string key_;
};

} // namespace

static coro::Task<void> childRequest(coro::Mutex& m, coro::Baton& b) {
  ShallowCopyRequestContextScopeGuard requestScope;

  auto* parentContext = dynamic_cast<TestRequestData*>(
      RequestContext::get()->getContextData(testToken1));

  EXPECT_TRUE(parentContext != nullptr);

  auto childKey = parentContext->key() + ".child";

  RequestContext::get()->setContextData(
      testToken2, std::make_unique<TestRequestData>(childKey));

  auto* childContext = dynamic_cast<TestRequestData*>(
      RequestContext::get()->getContextData(testToken2));
  CHECK(childContext != nullptr);

  {
    auto lock = co_await m.co_scoped_lock();

    CHECK_EQ(
        parentContext,
        dynamic_cast<TestRequestData*>(
            RequestContext::get()->getContextData(testToken1)));
    CHECK_EQ(
        childContext,
        dynamic_cast<TestRequestData*>(
            RequestContext::get()->getContextData(testToken2)));

    co_await b;

    CHECK_EQ(
        parentContext,
        dynamic_cast<TestRequestData*>(
            RequestContext::get()->getContextData(testToken1)));
    CHECK_EQ(
        childContext,
        dynamic_cast<TestRequestData*>(
            RequestContext::get()->getContextData(testToken2)));
  }

  CHECK_EQ(
      parentContext,
      dynamic_cast<TestRequestData*>(
          RequestContext::get()->getContextData(testToken1)));
  CHECK_EQ(
      childContext,
      dynamic_cast<TestRequestData*>(
          RequestContext::get()->getContextData(testToken2)));
}

static coro::Task<void> parentRequest(int id) {
  ShallowCopyRequestContextScopeGuard requestScope;

  // Should have captured the value at the time the coroutine was co_awaited
  // rather than at the time the coroutine was called.
  auto* globalData = dynamic_cast<TestRequestData*>(
      RequestContext::get()->getContextData("global"));
  CHECK(globalData != nullptr);
  CHECK_EQ("other value", globalData->key());

  std::string key = folly::to<std::string>("request", id);

  RequestContext::get()->setContextData(
      testToken1, std::make_unique<TestRequestData>(key));

  auto* contextData = RequestContext::get()->getContextData(testToken1);
  CHECK(contextData != nullptr);

  coro::Mutex mutex;
  coro::Baton baton1;
  coro::Baton baton2;

  auto fut1 =
      co_withExecutor(
          co_await coro::co_current_executor, childRequest(mutex, baton1))
          .start();
  auto fut2 =
      co_withExecutor(
          co_await coro::co_current_executor, childRequest(mutex, baton1))
          .start();

  CHECK_EQ(contextData, RequestContext::get()->getContextData(testToken1));

  baton1.post();
  baton2.post();

  (void)co_await std::move(fut1);

  CHECK_EQ(contextData, RequestContext::get()->getContextData(testToken1));

  // Check that context from child operation doesn't leak into this coroutine.
  CHECK(RequestContext::get()->getContextData(testToken2) == nullptr);

  (void)co_await std::move(fut2);

  // Check that context from child operation doesn't leak into this coroutine.
  CHECK(RequestContext::get()->getContextData(testToken2) == nullptr);
}

class TaskTest : public testing::Test {};

TEST_F(TaskTest, F14Lookup) {
  const F14FastMap<int, int> map{{1, 42}};
  auto lookup = [&](int key) -> coro::Task<int> {
    auto it = map.find(key);
    co_return it == map.end() ? -1 : it->second;
  };

  EXPECT_EQ(42, coro::blockingWait(lookup(1)));
  EXPECT_EQ(-1, coro::blockingWait(lookup(2)));
}

TEST_F(TaskTest, RequestContextIsPreservedAcrossSuspendResume) {
  ManualExecutor executor;

  RequestContextScopeGuard requestScope;

  RequestContext::get()->setContextData(
      "global", std::make_unique<TestRequestData>("global value"));

  // Context should be captured at coroutine co_await time and not at
  // call time.
  auto task1 = co_withExecutor(&executor, parentRequest(1));
  auto task2 = co_withExecutor(&executor, parentRequest(2));

  {
    RequestContextScopeGuard nestedRequestScope;

    RequestContext::get()->setContextData(
        "global", std::make_unique<TestRequestData>("other value"));

    // Start execution of the tasks.
    auto fut1 = std::move(task1).start();
    auto fut2 = std::move(task2).start();

    // Check that the contexts set by starting the tasks don't bleed out
    // to the caller.
    CHECK(RequestContext::get()->getContextData(testToken1) == nullptr);
    CHECK(RequestContext::get()->getContextData(testToken2) == nullptr);
    CHECK_EQ(
        "other value",
        dynamic_cast<TestRequestData*>(
            RequestContext::get()->getContextData("global"))
            ->key());

    executor.drain();

    CHECK(fut1.isReady());
    CHECK(fut2.isReady());

    // Check that the contexts set by the coroutines executing on the executor
    // do not leak out to the caller.
    CHECK(RequestContext::get()->getContextData(testToken1) == nullptr);
    CHECK(RequestContext::get()->getContextData(testToken2) == nullptr);
    CHECK_EQ(
        "other value",
        dynamic_cast<TestRequestData*>(
            RequestContext::get()->getContextData("global"))
            ->key());
  }
}

TEST_F(TaskTest, ContextPreservedAcrossMutexLock) {
  folly::coro::Mutex mutex;

  auto handleRequest =
      [&](folly::coro::Baton& event) -> folly::coro::Task<void> {
    RequestContextScopeGuard requestScope;

    RequestData* contextDataPtr = nullptr;
    {
      auto contextData = std::make_unique<TestRequestData>("some value");
      contextDataPtr = contextData.get();
      RequestContext::get()->setContextData(
          "mutex_test", std::move(contextData));
    }

    auto lock = co_await mutex.co_scoped_lock();

    // Check that the request context was preserved across mutex lock
    // acquisition.
    CHECK_EQ(
        RequestContext::get()->getContextData("mutex_test"), contextDataPtr);

    co_await event;

    // Check that request context was preserved across baton wait.
    CHECK_EQ(
        RequestContext::get()->getContextData("mutex_test"), contextDataPtr);
  };

  folly::ManualExecutor manualExecutor;

  folly::coro::Baton event1;
  folly::coro::Baton event2;
  auto t1 = co_withExecutor(&manualExecutor, handleRequest(event1)).start();
  auto t2 = co_withExecutor(&manualExecutor, handleRequest(event2)).start();

  manualExecutor.drain();

  event1.post();

  manualExecutor.drain();

  event2.post();

  manualExecutor.drain();

  EXPECT_TRUE(t1.isReady());
  EXPECT_TRUE(t2.isReady());

  EXPECT_FALSE(t1.hasException());
  EXPECT_FALSE(t2.hasException());
}

TEST_F(TaskTest, RequestContextSideEffectsArePreserved) {
  auto f =
      [&](folly::coro::Baton& baton) -> folly::coro::detail::InlineTask<void> {
    RequestContext::create();

    RequestContext::get()->setContextData(
        testToken1, std::make_unique<TestRequestData>("test"));

    EXPECT_NE(RequestContext::get()->getContextData(testToken1), nullptr);

    // HACK: Need to use co_viaIfAsync() to ensure request context is preserved
    // across suspend-point.
    co_await folly::coro::co_viaIfAsync(
        &folly::InlineExecutor::instance(), baton);

    EXPECT_NE(RequestContext::get()->getContextData(testToken1), nullptr);

    co_return;
  };

  auto g = [&](folly::coro::Baton& baton) -> folly::coro::Task<void> {
    EXPECT_EQ(RequestContext::get()->getContextData(testToken1), nullptr);
    co_await f(baton);
    EXPECT_NE(RequestContext::get()->getContextData(testToken1), nullptr);
    EXPECT_EQ(
        dynamic_cast<TestRequestData*>(
            RequestContext::get()->getContextData(testToken1))
            ->key(),
        "test");
  };

  folly::ManualExecutor executor;
  folly::coro::Baton baton;

  auto t = co_withExecutor(&executor, g(baton)).start();

  executor.drain();

  baton.post();

  executor.drain();

  EXPECT_TRUE(t.isReady());
  EXPECT_FALSE(t.hasException());
}

TEST_F(TaskTest, FutureTailCall) {
  EXPECT_EQ(
      42,
      folly::coro::blockingWait(
          folly::coro::co_invoke([&]() -> folly::coro::Task<int> {
            co_return co_await folly::makeSemiFuture().deferValue([](auto) {
              return folly::makeSemiFuture(42);
            });
          })));
}

TEST_F(TaskTest, FutureRoundtrip) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    co_yield folly::coro::co_result(
        co_await folly::coro::co_awaitTry(
            []() -> folly::coro::Task<void> { co_return; }().semi()));
  }());

  EXPECT_THROW(
      folly::coro::blockingWait([]() -> folly::coro::Task<void> {
        co_yield folly::coro::co_result(
            co_await folly::coro::co_awaitTry(
                []() -> folly::coro::Task<void> {
                  co_yield folly::coro::co_error(std::runtime_error(""));
                }()
                            .semi()));
      }()),
      std::runtime_error);
}

CO_TEST_F(TaskTest, CoResultUnitCompletesVoidTask) {
  { // `Try<Unit>` value
    auto res = co_await co_awaitTry([]() -> folly::coro::Task<void> {
      co_yield folly::coro::co_result(folly::Try<folly::Unit>{folly::unit});
    }());
    EXPECT_TRUE(res.hasValue());
  }
  { // `Try<Unit>` error
    auto res = co_await co_awaitTry([]() -> folly::coro::Task<void> {
      co_yield folly::coro::co_result(
          folly::Try<folly::Unit>{
              folly::make_exception_wrapper<std::runtime_error>("try")});
    }());
    auto* ex = res.tryGetExceptionObject<std::runtime_error>();
    CO_ASSERT_NE(nullptr, ex);
    EXPECT_STREQ("try", ex->what());
  }
#if FOLLY_HAS_RESULT
  { // `result<Unit>` value
    auto res = co_await co_awaitTry([]() -> folly::coro::Task<void> {
      co_yield folly::coro::co_result(folly::result<folly::Unit>{folly::unit});
    }());
    EXPECT_TRUE(res.hasValue());
  }
  { // `result<Unit>` error
    auto res = co_await co_awaitTry([]() -> folly::coro::Task<void> {
      co_yield folly::coro::co_result(
          folly::result<folly::Unit>{
              folly::error_or_stopped{std::runtime_error{"result"}}});
    }());
    auto* ex = res.tryGetExceptionObject<std::runtime_error>();
    CO_ASSERT_NE(nullptr, ex);
    EXPECT_STREQ("result", ex->what());
  }
  { // `value_only_result<Unit>` value
    auto res = co_await co_awaitTry([]() -> folly::coro::Task<void> {
      co_yield folly::coro::co_result(
          folly::value_only_result<folly::Unit>{folly::unit});
    }());
    EXPECT_TRUE(res.hasValue());
  }
#endif
}

namespace {

// We just want to make sure this compiles without errors or warnings.
[[maybe_unused]] folly::coro::Task<void>
checkAwaitingFutureOfUnitDoesntWarnAboutDiscardedResult() {
  co_await folly::makeSemiFuture();

  using namespace std::literals::chrono_literals;
  co_await folly::futures::sleep(1ms);
}

} // namespace

TEST_F(TaskTest, TaskOfLvalueReference) {
  auto returnIntRef = [](int& value) -> folly::coro::Task<int&> {
    co_return value;
  };

  int value = 123;
  auto&& result = folly::coro::blockingWait(returnIntRef(value));
  static_assert(std::is_same_v<decltype(result), int&>);
  CHECK_EQ(&value, &result);
}

TEST_F(TaskTest, TaskOfLvalueReferenceAsTry) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    auto returnIntRef = [](int& value) -> folly::coro::Task<int&> {
      co_return value;
    };

    int value = 123;
    auto&& result = co_await co_awaitTry(returnIntRef(value));
    CHECK(result.hasValue());
    CHECK_EQ(&value, &result.value().get());

    int& valueRef = co_await returnIntRef(value);
    CHECK_EQ(&value, &valueRef);
  }());
}

TEST_F(TaskTest, TaskOfLvalueReferenceAsResult) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    auto returnIntRef = [](int& value) -> folly::coro::Task<int&> {
      co_return value;
    };

    int value = 123;
    auto&& res = co_await value_or_error_or_stopped(returnIntRef(value));
    CHECK(res.has_value());
    CHECK_EQ(&value, &res.value_or_throw());
    CHECK_EQ(&value, &(co_await folly::or_unwind(std::move(res))));

    int& valueRef = co_await returnIntRef(value);
    CHECK_EQ(&value, &valueRef);
  }());
}

TEST_F(TaskTest, CancellationPropagation) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    auto token = co_await folly::coro::co_current_cancellation_token;
    CHECK(!token.canBeCancelled());

    folly::CancellationSource cancelSource;

    co_await folly::coro::co_withCancellation(
        cancelSource.getToken(), [&]() -> folly::coro::Task<void> {
          auto token2 = co_await folly::coro::co_current_cancellation_token;
          CHECK(token2.canBeCancelled());
          CHECK(!token2.isCancellationRequested());

          // The cancellation token should implicitly propagate into the
          //
          co_await [&]() -> folly::coro::Task<void> {
            auto token3 = co_await folly::coro::co_current_cancellation_token;
            CHECK(token3 == token2);
            cancelSource.requestCancellation();
            CHECK(token3.isCancellationRequested());
          }();
          CHECK(token2.isCancellationRequested());
        }());
  }());
}

TEST_F(TaskTest, CancellationPropagatesThroughCoAwaitTry) {
  folly::CancellationSource source;
  folly::Try<int> result = folly::coro::blockingWait(
      folly::coro::co_withCancellation(
          source.getToken(),
          folly::coro::co_awaitTry([&]() -> folly::coro::Task<int> {
            auto cancelToken =
                co_await folly::coro::co_current_cancellation_token;
            EXPECT_TRUE(cancelToken == source.getToken());
            co_return 42;
          }())));
  EXPECT_EQ(42, result.value());
}

TEST_F(TaskTest, CancellationOverrideConsumesMovedToken) {
  folly::CancellationSource firstSource;
  folly::CancellationSource ignoredSource;
  auto firstToken = firstSource.getToken();
  auto ignoredToken = ignoredSource.getToken();
  auto readToken = []() -> folly::coro::Task<folly::CancellationToken> {
    co_return co_await folly::coro::co_current_cancellation_token;
  };

  auto task = folly::coro::co_withCancellation(firstToken, readToken());
  auto overridden = folly::coro::co_withCancellation(
      std::move(ignoredToken), std::move(task));
  EXPECT_FALSE(ignoredToken.canBeCancelled());
  EXPECT_EQ(firstToken, folly::coro::blockingWait(std::move(overridden)));
}

TEST_F(TaskTest, EmptyCancellationOverrideConsumesMovedTokenWithExecutor) {
  folly::ManualExecutor executor;
  folly::CancellationSource ignoredSource;
  auto ignoredToken = ignoredSource.getToken();
  auto readToken = []() -> folly::coro::Task<folly::CancellationToken> {
    co_return co_await folly::coro::co_current_cancellation_token;
  };

  auto task = co_withExecutor(
      &executor,
      folly::coro::co_withCancellation(
          folly::CancellationToken{}, readToken()));
  auto overridden = folly::coro::co_withCancellation(
      std::move(ignoredToken), std::move(task));
  EXPECT_FALSE(ignoredToken.canBeCancelled());
  EXPECT_FALSE(
      folly::coro::blockingWait(std::move(overridden), &executor)
          .canBeCancelled());
}

TEST_F(TaskTest, StartInlineUnsafe) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    auto executor = co_await folly::coro::co_current_executor;
    bool hasStarted = false;
    bool hasFinished = false;
    auto makeTask = [&]() -> folly::coro::Task<void> {
      hasStarted = true;
      co_await folly::coro::co_reschedule_on_current_executor;
      hasFinished = true;
    };
    auto sf = co_withExecutor(executor, makeTask()).startInlineUnsafe();

    // Check that the task started inline on the current thread.
    // It should not yet have completed, however, since the rest
    // of the coroutine needs this coroutine to suspend so the
    // executor can schedule the rest of it.
    CHECK(hasStarted);
    CHECK(!hasFinished);

    co_await std::move(sf);

    CHECK(hasFinished);
  }());
}

TEST_F(TaskTest, StartInlineUnsafePreservesRequestContext) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    RequestContextScopeGuard parentGuard;
    auto* parentCtx = RequestContext::try_get();
    auto executor = co_await folly::coro::co_current_executor;

    bool hasStarted = false;
    coro::Baton baton;

    auto makeTask = [&]() -> folly::coro::Task<void> {
      RequestContextScopeGuard childGuard;
      auto* childCtx = RequestContext::try_get();
      hasStarted = true;
      co_await baton;
      EXPECT_EQ(childCtx, RequestContext::try_get());
    };
    auto sf = co_withExecutor(executor, makeTask()).startInlineUnsafe();

    EXPECT_TRUE(hasStarted);
    EXPECT_EQ(parentCtx, RequestContext::try_get());

    baton.post();
    co_await std::move(sf);
  }());
}

TEST_F(TaskTest, YieldTry) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    auto innerTaskVoid = []() -> folly::coro::Task<void> {
      co_yield folly::coro::co_error(std::runtime_error(""));
    }();
    auto retVoid = co_await co_awaitTry([&]() -> folly::coro::Task<void> {
      co_yield folly::coro::co_result(
          co_await co_awaitTry(std::move(innerTaskVoid)));
    }());
    EXPECT_TRUE(retVoid.hasException());

    innerTaskVoid = []() -> folly::coro::Task<void> { co_return; }();
    retVoid = co_await co_awaitTry([&]() -> folly::coro::Task<void> {
      co_yield folly::coro::co_result(
          co_await co_awaitTry(std::move(innerTaskVoid)));
    }());
    EXPECT_FALSE(retVoid.hasException());

    auto innerTaskInt = []() -> folly::coro::Task<int> {
      co_yield folly::coro::co_error(std::runtime_error(""));
    }();
    auto retInt = co_await co_awaitTry([&]() -> folly::coro::Task<int> {
      co_yield folly::coro::co_result(
          co_await co_awaitTry(std::move(innerTaskInt)));
    }());
    EXPECT_TRUE(retInt.hasException());

    innerTaskInt = []() -> folly::coro::Task<int> { co_return 0; }();
    retInt = co_await co_awaitTry([&]() -> folly::coro::Task<int> {
      co_yield folly::coro::co_result(
          co_await co_awaitTry(std::move(innerTaskInt)));
    }());
    EXPECT_TRUE(retInt.hasValue());

    EXPECT_THROW(
        co_await [&]() -> folly::coro::Task<int> {
          co_yield folly::coro::co_result(folly::Try<int>());
        }(),
        UsingUninitializedTry);
  }());
}

TEST_F(TaskTest, MakeTask) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    auto ret = co_await folly::coro::makeTask(42);
    EXPECT_EQ(ret, 42);

    co_await folly::coro::makeTask();
    co_await folly::coro::makeTask(folly::unit);

    auto err = co_await co_awaitTry(
        folly::coro::makeErrorTask<int>(
            folly::make_exception_wrapper<std::runtime_error>("")));
    EXPECT_TRUE(err.hasException());

    err = co_await co_awaitTry(
        folly::coro::makeResultTask(
            folly::Try<int>(
                folly::make_exception_wrapper<std::runtime_error>(""))));
    EXPECT_TRUE(err.hasException());

    auto try1 = co_await co_awaitTry(
        folly::coro::makeResultTask(
            folly::Try<folly::Unit>(
                folly::make_exception_wrapper<std::runtime_error>(""))));
    EXPECT_TRUE(try1.hasException());
    try1 = co_await co_awaitTry(
        folly::coro::makeResultTask(folly::Try<folly::Unit>(folly::unit)));
    EXPECT_TRUE(try1.hasValue());

    // test move happens immediately (i.e. no dangling reference)
    struct {
      int i{0};
    } s;
    auto t = folly::coro::makeTask(std::move(s));
    s.i = 1;
    auto s2 = co_await std::move(t);
    EXPECT_EQ(s2.i, 0);
  }());
}

TEST_F(TaskTest, ScheduleOnRestoresExecutor) {
  folly::ScopedEventBaseThread ebt;
  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    co_await co_withExecutor(&ebt, [&]() -> folly::coro::Task<void> {
      EXPECT_TRUE(ebt.getEventBase()->inRunningEventBaseThread());
      co_return;
    }());
    EXPECT_FALSE(ebt.getEventBase()->inRunningEventBaseThread());
    try {
      co_await co_withExecutor(&ebt, [&]() -> folly::coro::Task<void> {
        EXPECT_TRUE(ebt.getEventBase()->inRunningEventBaseThread());
        throw std::runtime_error("");
        co_return;
      }());
    } catch (...) {
    }
    EXPECT_FALSE(ebt.getEventBase()->inRunningEventBaseThread());
  }());
}

TEST_F(TaskTest, CoAwaitTryWithScheduleOn) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    auto t = []() -> folly::coro::Task<int> { co_return 42; }();

    folly::Try<int> result = co_await folly::coro::co_awaitTry(
        co_withExecutor(folly::getGlobalCPUExecutor(), std::move(t)));
    EXPECT_EQ(42, result.value());
  }());
}

TEST_F(TaskTest, CoAwaitTryWithScheduleOnAndCancellation) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    folly::CancellationSource cancelSrc;

    auto makeTask = [&]() -> folly::coro::Task<int> {
      auto ct = co_await folly::coro::co_current_cancellation_token;
      EXPECT_FALSE(ct.isCancellationRequested());
      cancelSrc.requestCancellation();
      EXPECT_TRUE(ct.isCancellationRequested());
      co_return 42;
    };

    {
      folly::Try<int> result = co_await folly::coro::co_withCancellation(
          cancelSrc.getToken(),
          folly::coro::co_awaitTry(
              co_withExecutor(folly::getGlobalCPUExecutor(), makeTask())));
      EXPECT_EQ(42, result.value());
    }

    cancelSrc = {};

    {
      folly::Try<int> result = co_await folly::coro::co_awaitTry(
          folly::coro::co_withCancellation(
              cancelSrc.getToken(),
              co_withExecutor(folly::getGlobalCPUExecutor(), makeTask())));
      EXPECT_EQ(42, result.value());
    }
  }());
}

TEST_F(TaskTest, Moved) {
  if (folly::kIsDebug) {
    ASSERT_DEATH_IF_SUPPORTED(
        folly::coro::blockingWait([]() -> folly::coro::Task<void> {
          folly::coro::Task<int> task = folly::coro::makeTask(1);
          co_await std::move(task);
          co_await std::move(task);
        }()),
        "task\\.coro_");
  }
}

TEST_F(TaskTest, SafePoint) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    enum class step_type {
      init,
      before_continue_sp,
      after_continue_sp,
      before_cancel_sp,
      after_cancel_sp,
    };
    step_type step = step_type::init;

    folly::CancellationSource cancelSrc;
    auto makeTask = [&]() -> folly::coro::Task<void> {
      step = step_type::before_continue_sp;
      co_await folly::coro::co_safe_point;
      step = step_type::after_continue_sp;

      cancelSrc.requestCancellation();

      step = step_type::before_cancel_sp;
      co_await folly::coro::co_safe_point;
      step = step_type::after_cancel_sp;
    };

    auto result = co_await folly::coro::co_awaitTry( //
        folly::coro::co_withCancellation(cancelSrc.getToken(), makeTask()));
    EXPECT_THROW(result.value(), folly::OperationCancelled);
    EXPECT_EQ(step_type::before_cancel_sp, step);
  }());
}

TEST_F(TaskTest, CoAwaitNothrow) {
  auto res =
      folly::coro::blockingWait(co_awaitTry([]() -> folly::coro::Task<void> {
        auto t = []() -> folly::coro::Task<int> { co_return 42; }();

        int result = co_await folly::coro::co_nothrow(std::move(t));
        EXPECT_EQ(42, result);

        t = []() -> folly::coro::Task<int> {
          co_yield folly::coro::co_error(std::runtime_error(""));
        }();

        try {
          result = co_await folly::coro::co_nothrow(std::move(t));
        } catch (...) {
          ADD_FAILURE();
        }
        ADD_FAILURE();
      }()));
  EXPECT_TRUE(res.hasException<std::runtime_error>());
}

TEST_F(TaskTest, CoAwaitNothrowWithScheduleOn) {
  auto res =
      folly::coro::blockingWait(co_awaitTry([]() -> folly::coro::Task<void> {
        auto t = []() -> folly::coro::Task<int> { co_return 42; }();

        int result = co_await folly::coro::co_nothrow(
            co_withExecutor(folly::getGlobalCPUExecutor(), std::move(t)));
        EXPECT_EQ(42, result);

        t = []() -> folly::coro::Task<int> {
          co_yield folly::coro::co_error(std::runtime_error(""));
        }();

        try {
          result = co_await folly::coro::co_nothrow(
              co_withExecutor(folly::getGlobalCPUExecutor(), std::move(t)));
        } catch (...) {
          ADD_FAILURE();
        }
        ADD_FAILURE();
      }()));
  EXPECT_TRUE(res.hasException<std::runtime_error>());
}

TEST_F(TaskTest, CoAwaitThrowAfterNothrow) {
  auto res =
      folly::coro::blockingWait(co_awaitTry([]() -> folly::coro::Task<void> {
        auto t = []() -> folly::coro::Task<int> { co_return 42; }();

        int result = co_await folly::coro::co_nothrow(std::move(t));
        EXPECT_EQ(42, result);

        t = []() -> folly::coro::Task<int> {
          co_yield folly::coro::co_error(std::runtime_error(""));
        }();

        try {
          result = co_await std::move(t);
          ADD_FAILURE();
        } catch (...) {
          throw std::logic_error("translated");
        }
      }()));
  EXPECT_TRUE(res.hasException<std::logic_error>());
}

TEST_F(TaskTest, CoAwaitNothrowDestructorOrdering) {
  int i = 0;
  folly::coro::blockingWait(co_awaitTry([&]() -> folly::coro::Task<> {
    co_await folly::coro::co_nothrow([&]() -> folly::coro::Task<> {
      SCOPE_EXIT {
        i *= i;
      };
      co_await folly::coro::co_nothrow([&]() -> folly::coro::Task<> {
        SCOPE_EXIT {
          i *= 3;
        };
        co_await folly::coro::co_nothrow([&]() -> folly::coro::Task<> {
          SCOPE_EXIT {
            i += 1;
          };
          co_return;
        }());
      }());
    }());
  }()));
  EXPECT_EQ(i, 9);
}

struct ExpectedException : public std::runtime_error {
  ExpectedException() : std::runtime_error("expected") {}
};

TEST_F(TaskTest, CoYieldCoErrorSameExecutor) {
  folly::ScopedEventBaseThread ebThread;

  auto scopeAndThrow = [&]() -> folly::coro::Task<void> {
    auto eb = dynamic_cast<folly::EventBase*>(
        co_await folly::coro::co_current_executor);
    CHECK(eb);
    SCOPE_EXIT {
      CHECK(eb->inRunningEventBaseThread());
    };
    co_yield folly::coro::co_error(ExpectedException());
  };
  auto scopeAndThrowWrapper = [&]() -> folly::coro::Task<void> {
    co_await co_withExecutor(ebThread.getEventBaseKeepAlive(), scopeAndThrow());
  };

  EXPECT_THROW(
      folly::coro::blockingWait(scopeAndThrowWrapper()), ExpectedException);
}
#endif
