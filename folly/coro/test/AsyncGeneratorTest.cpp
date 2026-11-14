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

#include <folly/Portability.h>

#include <folly/ScopeGuard.h>
#include <folly/Traits.h>
#include <folly/coro/AsyncGenerator.h>
#include <folly/coro/Baton.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/coro/GtestHelpers.h>
#include <folly/coro/Invoke.h>
#include <folly/coro/Sleep.h>
#include <folly/coro/Task.h>
#include <folly/coro/ValueOrError.h>
#include <folly/coro/WithCancellation.h>
#include <folly/coro/safe/NowTask.h>
#include <folly/futures/Future.h>

#include <folly/portability/GTest.h>
#include <folly/portability/PThread.h>

#include <chrono>
#include <map>
#include <string>
#include <tuple>

#if FOLLY_HAS_COROUTINES

constexpr bool check_for_size_regressions() {
  using namespace folly::coro;
  namespace detail = folly::coro::detail;

  static_assert(sizeof(AsyncGenerator<int&>) == sizeof(void*));

  // Prevent size regressions due to member or base ordering
  constexpr size_t promiseSize =
      // From AsyncGeneratorPromise:
      sizeof(ExtendedCoroutineHandle) + sizeof(folly::AsyncStackFrame) +
      sizeof(folly::Executor::KeepAlive<>) + sizeof(folly::CancellationToken) +
      // The value/error union
      sizeof(folly::exception_wrapper) +
      // state_, hasCancelTokenOverride_ and bypassExceptionThrowing_ together:
      sizeof(void*) +
      // From ExtendedCoroutinePromiseCrtp:
      sizeof(ExtendedCoroutineHandle::PromiseBase);
  static_assert(
      sizeof(detail::AsyncGeneratorPromise<int&, int>) == promiseSize);

  return true;
}
static_assert(check_for_size_regressions());

class AsyncGeneratorTest : public testing::Test {};

TEST_F(AsyncGeneratorTest, DefaultConstructedGeneratorIsEmpty) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    folly::coro::AsyncGenerator<int> g;
    auto result = co_await g.next();
    CHECK(!result);
  }());
}

TEST_F(AsyncGeneratorTest, EmptyGeneratorConsumesMovedCancellationToken) {
  folly::CancellationSource source;
  auto token = source.getToken();
  folly::coro::AsyncGenerator<int> generator;
  auto next =
      folly::coro::co_withCancellation(std::move(token), generator.next());

  EXPECT_FALSE(token.canBeCancelled());
  EXPECT_FALSE(folly::coro::blockingWait(next));
}

TEST_F(AsyncGeneratorTest, GeneratorDestroyedBeforeCallingBegin) {
  bool started = false;
  auto makeGenerator = [&]() -> folly::coro::AsyncGenerator<int> {
    started = true;
    co_return;
  };

  {
    auto gen = makeGenerator();
    (void)gen;
  }

  CHECK(!started);
}

TEST_F(AsyncGeneratorTest, PartiallyConsumingSequenceDestroysObjectsInScope) {
  bool started = false;
  bool destroyed = false;
  auto makeGenerator = [&]() -> folly::coro::AsyncGenerator<int> {
    SCOPE_EXIT {
      destroyed = true;
    };
    started = true;
    co_yield 1;
    co_yield 2;
    co_return;
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    {
      auto gen = makeGenerator();
      CHECK(!started);
      CHECK(!destroyed);
      auto result = co_await gen.next();
      CHECK(started);
      CHECK(!destroyed);
      CHECK(result);
      CHECK_EQ(1, *result);
    }
    CHECK(destroyed);
  }());
}

TEST_F(AsyncGeneratorTest, FullyConsumeSequence) {
  auto makeGenerator = []() -> folly::coro::AsyncGenerator<int> {
    for (int i = 0; i < 4; ++i) {
      co_yield i;
    }
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto gen = makeGenerator();
    auto result = co_await gen.next();
    CHECK(result);
    CHECK_EQ(0, *result);
    result = co_await gen.next();
    CHECK(result);
    CHECK_EQ(1, *result);
    result = co_await gen.next();
    CHECK(result);
    CHECK_EQ(2, *result);
    result = co_await gen.next();
    CHECK(result);
    CHECK_EQ(3, *result);
    result = co_await gen.next();
    CHECK(!result);
  }());
}

namespace {
struct SomeError : std::exception {};
} // namespace

TEST_F(AsyncGeneratorTest, ThrowExceptionBeforeFirstYield) {
  auto makeGenerator = []() -> folly::coro::AsyncGenerator<int> {
    if (true) {
      throw SomeError{};
    }
    co_return;
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto gen = makeGenerator();
    bool caughtException = false;
    try {
      (void)co_await gen.next();
      CHECK(false);
    } catch (const SomeError&) {
      caughtException = true;
    }
    CHECK(caughtException);
  }());
}

TEST_F(AsyncGeneratorTest, ThrowExceptionAfterFirstYield) {
  auto makeGenerator = []() -> folly::coro::AsyncGenerator<int> {
    co_yield 42;
    throw SomeError{};
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto gen = makeGenerator();
    auto result = co_await gen.next();
    CHECK(result);
    CHECK_EQ(42, *result);
    bool caughtException = false;
    try {
      (void)co_await gen.next();
      CHECK(false);
    } catch (const SomeError&) {
      caughtException = true;
    }
    CHECK(caughtException);
  }());
}

TEST_F(
    AsyncGeneratorTest, ConsumingManySynchronousElementsDoesNotOverflowStack) {
  auto makeGenerator = []() -> folly::coro::AsyncGenerator<std::uint64_t> {
    for (std::uint64_t i = 0; i < 1'000'000; ++i) {
      co_yield i;
    }
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto gen = makeGenerator();
    std::uint64_t sum = 0;
    while (auto result = co_await gen.next()) {
      sum += *result;
    }
    CHECK_EQ(499999500000u, sum);
  }());
}

TEST_F(AsyncGeneratorTest, ProduceResultsAsynchronously) {
  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    folly::Executor* executor = co_await folly::coro::co_current_executor;
    auto makeGenerator = [&]() -> folly::coro::AsyncGenerator<int> {
      using namespace std::literals::chrono_literals;
      CHECK_EQ(executor, co_await folly::coro::co_current_executor);
      co_await folly::coro::sleep(1ms);
      CHECK_EQ(executor, co_await folly::coro::co_current_executor);
      co_yield 1;
      CHECK_EQ(executor, co_await folly::coro::co_current_executor);
      co_await folly::coro::sleep(1ms);
      CHECK_EQ(executor, co_await folly::coro::co_current_executor);
      co_yield 2;
      CHECK_EQ(executor, co_await folly::coro::co_current_executor);
      co_await folly::coro::sleep(1ms);
      CHECK_EQ(executor, co_await folly::coro::co_current_executor);
    };

    auto gen = makeGenerator();
    auto result = co_await gen.next();
    CHECK_EQ(1, *result);
    result = co_await gen.next();
    CHECK_EQ(2, *result);
    result = co_await gen.next();
    CHECK(!result);
  }());
}

struct ConvertibleToIntReference {
  int value;
  operator int&() { return value; }
};

TEST_F(AsyncGeneratorTest, GeneratorOfLValueReference) {
  auto makeGenerator = []() -> folly::coro::AsyncGenerator<int&> {
    int value = 10;
    co_yield value;
    // Consumer gets a mutable reference to the value and can modify it.
    CHECK_EQ(20, value);

    // NOTE: Not allowed to yield an rvalue from an AsyncGenerator<T&>?
    // co_yield 30;  // Compile-error

    co_yield ConvertibleToIntReference{30};
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto gen = makeGenerator();
    auto result = co_await gen.next();
    CHECK_EQ(10, result.value());
    *result = 20;
    result = co_await gen.next();
    CHECK_EQ(30, result.value());
    result = co_await gen.next();
    CHECK(!result.has_value());
  }());
}

struct ConvertibleToInt {
  operator int() const { return 99; }
};

TEST_F(AsyncGeneratorTest, GeneratorOfConstLValueReference) {
  auto makeGenerator = []() -> folly::coro::AsyncGenerator<const int&> {
    int value = 10;
    co_yield value;
    // Consumer gets a const reference to the value.

    // Allowed to yield an rvalue from an AsyncGenerator<const T&>.
    co_yield 30;

    co_yield ConvertibleToInt{};
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto gen = makeGenerator();
    auto result = co_await gen.next();
    CHECK_EQ(10, *result);
    result = co_await gen.next();
    CHECK_EQ(30, *result);
    result = co_await gen.next();
    CHECK_EQ(99, *result);
    result = co_await gen.next();
    CHECK(!result);
  }());
}

TEST_F(AsyncGeneratorTest, GeneratorOfRValueReference) {
  auto makeGenerator =
      []() -> folly::coro::AsyncGenerator<std::unique_ptr<int>&&> {
    co_yield std::make_unique<int>(10);

    auto ptr = std::make_unique<int>(20);
    co_yield std::move(ptr);
    CHECK(ptr == nullptr);
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto gen = makeGenerator();

    auto result = co_await gen.next();
    CHECK_EQ(10, **result);
    // Don't move it to a local var.

    result = co_await gen.next();
    CHECK_EQ(20, **result);
    auto ptr = *result; // Move it to a local var.

    result = co_await gen.next();
    CHECK(!result);
  }());
}

struct MoveOnly {
  explicit MoveOnly(int value) : value_(value) {}
  MoveOnly(MoveOnly&& other) noexcept
      : value_(std::exchange(other.value_, -1)) {}
  ~MoveOnly() {}
  MoveOnly& operator=(MoveOnly&&) = delete;
  int value() const { return value_; }

 private:
  int value_;
};

TEST_F(AsyncGeneratorTest, GeneratorOfMoveOnlyType) {
  auto makeGenerator = []() -> folly::coro::AsyncGenerator<MoveOnly> {
    MoveOnly rvalue(1);
    co_yield std::move(rvalue);
    CHECK_EQ(-1, rvalue.value());

    co_yield MoveOnly(2);
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto gen = makeGenerator();
    auto result = co_await gen.next();

    // NOTE: It's an error to dereference using '*it' as this returns a copy
    // of the iterator's reference type, which in this case is 'MoveOnly'.
    CHECK_EQ(1, result->value());

    result = co_await gen.next();
    CHECK_EQ(2, result->value());

    result = co_await gen.next();
    CHECK(!result);
  }());
}

TEST_F(AsyncGeneratorTest, GeneratorOfConstValue) {
  auto makeGenerator = []() -> folly::coro::AsyncGenerator<const int> {
    // OK to yield prvalue
    co_yield 42;

    // OK to yield lvalue
    int x = 123;
    co_yield x;

    co_yield ConvertibleToInt{};
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto gen = makeGenerator();
    auto result = co_await gen.next();
    CHECK_EQ(42, *result);
    static_assert(std::is_same_v<decltype(*result), const int&>);
    result = co_await gen.next();
    CHECK_EQ(123, *result);
    result = co_await gen.next();
    CHECK_EQ(99, *result);
    result = co_await gen.next();
    CHECK(!result);
  }());
}

TEST_F(AsyncGeneratorTest, ExplicitValueType) {
  std::map<std::string, std::string> items;
  items["foo"] = "hello";
  items["bar"] = "goodbye";

  auto makeGenerator = [&]()
      -> folly::coro::AsyncGenerator<
          std::tuple<const std::string&, std::string&>,
          std::tuple<std::string, std::string>> {
    for (auto& [k, v] : items) {
      co_yield {k, v};
    }
  };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto gen = makeGenerator();
    auto result = co_await gen.next();
    {
      auto [kRef, vRef] = *result;
      CHECK_EQ("bar", kRef);
      CHECK_EQ("goodbye", vRef);
      decltype(gen)::value_type copy = *result;
      vRef = "au revoir";
      CHECK_EQ("goodbye", std::get<1>(copy));
      CHECK_EQ("au revoir", std::get<1>(*result));
    }
  }());

  CHECK_EQ("au revoir", items["bar"]);
}

TEST_F(AsyncGeneratorTest, InvokeLambda) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    auto ptr = std::make_unique<int>(123);
    auto gen = folly::coro::co_invoke(
        [p = std::move(ptr), str = std::string("test")]() mutable
            -> folly::coro::AsyncGenerator<std::unique_ptr<int>&&> {
          SCOPE_EXIT {
            CHECK_EQ(str, "test");
          };
          co_yield std::move(p);
        });

    auto result = co_await gen.next();
    CHECK(result);
    ptr = *result;
    CHECK(ptr);
    CHECK(*ptr == 123);
  }());
}

TEST_F(AsyncGeneratorTest, InvokeLambdaRequiresCleanup) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    auto ptr = std::make_unique<int>(123);
    auto gen = folly::coro::co_invoke(
        [p = std::move(ptr), str = std::string("test")]() mutable
            -> folly::coro::CleanableAsyncGenerator<std::unique_ptr<int>&&> {
          SCOPE_EXIT {
            CHECK_EQ(str, "test");
          };
          co_yield std::move(p);
        });

    auto result = co_await gen.next();
    CHECK(result);
    ptr = *result;
    CHECK(ptr);
    CHECK(*ptr == 123);
    co_await std::move(gen).cleanup();
  }());
}

template <typename Ref, typename Value = std::remove_cvref_t<Ref>>
folly::coro::AsyncGenerator<Ref, Value> neverStream() {
  folly::coro::Baton baton;
  folly::CancellationCallback cb{
      co_await folly::coro::co_current_cancellation_token,
      [&] { baton.post(); }};
  co_await baton;
}

TEST_F(AsyncGeneratorTest, CancellationTokenPropagatesFromConsumer) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    folly::CancellationSource cancelSource;
    bool suspended = false;
    bool done = false;
    co_await folly::coro::collectAll(
        folly::coro::co_withCancellation(
            cancelSource.getToken(),
            [&]() -> folly::coro::Task<void> {
              auto stream = neverStream<int>();
              suspended = true;
              auto result = co_await stream.next();
              CHECK(!result.has_value());
              done = true;
            }()),
        [&]() -> folly::coro::Task<void> {
          co_await folly::coro::co_reschedule_on_current_executor;
          co_await folly::coro::co_reschedule_on_current_executor;
          co_await folly::coro::co_reschedule_on_current_executor;
          CHECK(suspended);
          CHECK(!done);
          cancelSource.requestCancellation();
        }());
    CHECK(done);
  }());
}

TEST_F(AsyncGeneratorTest, BlockingWaitOnFinalNextDoesNotDeadlock) {
  auto gen = []() -> folly::coro::AsyncGenerator<int> { co_yield 42; };

  auto g = gen();
  auto val1 = folly::coro::blockingWait(g.next());
  CHECK_EQ(42, val1.value());
  auto val2 = folly::coro::blockingWait(g.next());
  CHECK(!val2.has_value());
}

TEST_F(AsyncGeneratorTest, BlockingWaitOnThrowingFinalNextDoesNotDeadlock) {
  auto gen = []() -> folly::coro::AsyncGenerator<int> {
    co_yield 42;
    throw SomeError{};
  };

  auto g = gen();
  auto val1 = folly::coro::blockingWait(g.next());
  CHECK_EQ(42, val1.value());
  try {
    folly::coro::blockingWait(g.next());
    CHECK(false);
  } catch (const SomeError&) {
  }
}

folly::coro::AsyncGenerator<size_t> sizeRange(size_t from, size_t to) {
  for (size_t i = from; i < to; ++i) {
    co_yield i;
  }
}

TEST_F(AsyncGeneratorTest, SymmetricTransfer) {
  // 512KiB is larger than we need, technically.  It comes from MacOS's
  // default stack size for secondary threads.  The superstition behind
  // using a realistic size is that even if someone stubs out `setstacksize`
  // to be a no-op for a platform, we still have a chance of catching bugs.
  constexpr size_t kStackSize = 1 << 19;
  auto testFn = [](void* numItersPtr) -> void* {
    size_t numIters = *reinterpret_cast<size_t*>(numItersPtr);
    size_t sum = 0;
    folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
      auto g = sizeRange(1, numIters + 1);
      int* prevAddress = nullptr;
      while (auto result = co_await g.next()) {
        [[maybe_unused]] int stackVariable = 0;
        if (!prevAddress) {
          prevAddress = &stackVariable;
        } else {
          // If symmetric transfer is broken, this should fire before we
          // overflow the stack. `EXPECT_EQ` hangs on Mac / Rosetta.
          CHECK_EQ(prevAddress, &stackVariable);
        }
        sum += *result;
      }
      CHECK_EQ((numIters + 1) * numIters / 2, sum);
    }());
    static_assert(sizeof(size_t) <= sizeof(void*));
    return reinterpret_cast<void*>(sum);
  };
  pthread_attr_t attr;
  ASSERT_EQ(0, pthread_attr_init(&attr));
  ASSERT_EQ(0, pthread_attr_setstacksize(&attr, kStackSize));
  pthread_t thread;
  // The goal is to overflow the stack with even 1 byte per item
  size_t numIters = kStackSize + 1;
  ASSERT_EQ(0, pthread_create(&thread, &attr, testFn, &numIters));
  void* sumAsPtr;
  ASSERT_EQ(0, pthread_join(thread, &sumAsPtr));
  // Redundant with above, but ensures the thread actually ran!
  EXPECT_EQ((numIters + 1) * numIters / 2, reinterpret_cast<size_t>(sumAsPtr));
}

TEST(AsyncGenerator, YieldCoError) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    auto gen = []() -> folly::coro::AsyncGenerator<std::string> {
      co_yield "foo";
      co_yield "bar";
      co_yield folly::coro::co_error(SomeError{});
      // not enforced in opt mode yet so this code is reached in the
      // EXPECT_DEBUG_DEATH line
      if (folly::kIsDebug) {
        ADD_FAILURE();
      }
    }();

    auto item1 = co_await gen.next();
    CHECK(item1.value() == "foo");
    auto item2 = co_await gen.next();
    CHECK(item2.value() == "bar");

    try {
      (void)co_await gen.next();
      ADD_FAILURE();
    } catch (const SomeError&) {
    }

    EXPECT_DEATH(
        co_await gen.next(), "Using generator after receiving exception.");
  }());
}

TEST(AsyncGenerator, YieldCoResult) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    auto gen = []() -> folly::coro::AsyncGenerator<std::string> {
      co_yield folly::coro::co_result(folly::Try<std::string>("foo"));
      co_yield folly::coro::co_result(folly::Try<std::string>("bar"));
      co_yield folly::coro::co_result(folly::Try<std::string>(SomeError{}));
      ADD_FAILURE();
    }();

    auto item1 = co_await gen.next();
    CHECK(item1.value() == "foo");
    auto item2 = co_await gen.next();
    CHECK(item2.value() == "bar");

    EXPECT_THROW(co_await gen.next(), SomeError);

    gen = []() -> folly::coro::AsyncGenerator<std::string> {
      co_yield folly::coro::co_result(folly::Try<std::string>("foo"));
      co_yield folly::coro::co_result(folly::Try<std::string>("bar"));
      co_yield folly::coro::co_result(folly::Try<std::string>());
      ADD_FAILURE();
    }();

    item1 = co_await gen.next();
    CHECK(item1.value() == "foo");
    item2 = co_await gen.next();
    CHECK(item2.value() == "bar");
    EXPECT_FALSE(co_await gen.next());
  }());
}

TEST(AsyncGenerator, CoResultProxyExample) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    auto makeProxy = [](folly::coro::AsyncGenerator<int> gen)
        -> folly::coro::AsyncGenerator<int> {
      while (true) {
        auto t = co_await folly::coro::co_awaitTry(gen.next());
        co_yield folly::coro::co_result(std::move(t));
      }
    };

    auto gen = []() -> folly::coro::AsyncGenerator<int> {
      for (int i = 0; i < 5; ++i) {
        co_yield i;
      }
    }();

    auto proxy = makeProxy(std::move(gen));

    for (int i = 0; i < 5; ++i) {
      auto val = co_await proxy.next();
      EXPECT_EQ(*val, i);
    }
    EXPECT_FALSE(co_await proxy.next());

    gen = []() -> folly::coro::AsyncGenerator<int> {
      for (int i = 0; i < 5; ++i) {
        co_yield i;
      }
      throw SomeError();
    }();

    proxy = makeProxy(std::move(gen));

    for (int i = 0; i < 5; ++i) {
      auto val = co_await proxy.next();
      EXPECT_EQ(*val, i);
    }
    EXPECT_THROW(co_await proxy.next(), SomeError);
  }());
}

TEST(AsyncGenerator, CoAwaitTry) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    auto gen = []() -> folly::coro::AsyncGenerator<std::string> {
      co_yield "foo";
      co_yield "bar";
      co_yield folly::coro::co_error(SomeError{});
      CHECK(false);
    }();

    auto item1 = co_await folly::coro::co_awaitTry(gen.next());
    CHECK(item1.hasValue());
    CHECK(*item1.value() == "foo");
    auto item2 = co_await folly::coro::co_awaitTry(gen.next());
    CHECK(item2.hasValue());
    CHECK(*item2.value() == "bar");
    auto item3 = co_await folly::coro::co_awaitTry(gen.next());
    CHECK(item3.hasException());
    CHECK(item3.exception().is_compatible_with<SomeError>());
  }());
}

TEST(AsyncGenerator, CoAwaitValueOrError) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    auto gen = []() -> folly::coro::AsyncGenerator<std::string> {
      co_yield "foo";
      co_yield "bar";
      co_yield folly::coro::co_error(SomeError{});
      CHECK(false);
    }();

    auto item1 = co_await folly::coro::value_or_error_or_stopped(gen.next());
    CHECK(item1.has_value());
    CHECK(*item1.value_or_throw() == "foo");
    auto item2 = co_await folly::coro::value_or_error_or_stopped(gen.next());
    CHECK(item2.has_value());
    CHECK(*item2.value_or_throw() == "bar");
    auto item3 = co_await folly::coro::value_or_error_or_stopped(gen.next());
    CHECK(!item3.has_value() && !item3.has_stopped());
    CHECK(folly::get_exception<SomeError>(item3));
  }());
}

#if FOLLY_HAS_RESULT
// Test that value_or_error() auto-propagates OperationCancelled (stops the
// consuming coroutine) when an AsyncGenerator yields co_stopped_may_throw.
CO_TEST(AsyncGenerator, ValueOrErrorPropagatesStopped) {
  using namespace folly::coro;

  auto stoppedGen = []() -> AsyncGenerator<int> {
    co_yield 1;
    co_yield co_stopped_may_throw;
    ADD_FAILURE() << "Generator should not continue after co_stopped_may_throw";
  };

  int firstItem = 0;
  auto consumer = [&]() -> now_task<> {
    auto gen = stoppedGen();
    auto r1 = co_await value_or_error(gen.next());
    firstItem = *r1.value_or_throw();
    // This should propagate OperationCancelled, stopping this task
    (void)co_await value_or_error(gen.next());
    ADD_FAILURE();
  };

  auto res = co_await value_or_error_or_stopped(consumer());
  EXPECT_EQ(1, firstItem);
  EXPECT_TRUE(res.has_stopped());
}
#endif

TEST(AsyncGenerator, SafePoint) {
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
    auto gen =
        folly::coro::co_invoke([&]() -> folly::coro::AsyncGenerator<int> {
          step = step_type::before_continue_sp;
          co_await folly::coro::co_safe_point;
          step = step_type::after_continue_sp;

          cancelSrc.requestCancellation();

          step = step_type::before_cancel_sp;
          co_await folly::coro::co_safe_point;
          step = step_type::after_cancel_sp;
        });

    auto result = co_await folly::coro::co_awaitTry(
        folly::coro::co_withCancellation(cancelSrc.getToken(), gen.next()));
    EXPECT_THROW(result.value(), folly::OperationCancelled);
    EXPECT_EQ(step_type::before_cancel_sp, step);
  }());
}

TEST_F(AsyncGeneratorTest, NextAfterCancel) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    folly::CancellationSource cancelSrc;
    auto gen =
        folly::coro::co_invoke([&]() -> folly::coro::AsyncGenerator<int> {
          co_yield 1;
          co_yield 2;
          co_await folly::coro::co_safe_point;
          co_yield 3;
        });

    auto result = co_await folly::coro::co_awaitTry(
        folly::coro::co_withCancellation(cancelSrc.getToken(), gen.next()));
    EXPECT_TRUE(result.hasValue());
    EXPECT_EQ(1, *result.value());

    cancelSrc.requestCancellation();
    result = co_await folly::coro::co_awaitTry(
        folly::coro::co_withCancellation(cancelSrc.getToken(), gen.next()));
    EXPECT_TRUE(result.hasValue());
    EXPECT_EQ(2, *result.value());

    result = co_await folly::coro::co_awaitTry(
        folly::coro::co_withCancellation(cancelSrc.getToken(), gen.next()));
    EXPECT_TRUE(result.hasException());
    EXPECT_THROW(result.value(), folly::OperationCancelled);
  }());
}

TEST(AsyncGenerator, CoAwaitNothrow) {
  auto res =
      folly::coro::blockingWait(co_awaitTry([]() -> folly::coro::Task<void> {
        auto gen = []() -> folly::coro::AsyncGenerator<int> {
          auto inner = []() -> folly::coro::AsyncGenerator<int> {
            co_yield 42;
            throw std::runtime_error("");
          }();
          co_yield *co_await co_nothrow(inner.next());
          try {
            co_yield *co_await co_nothrow(inner.next());
          } catch (...) {
            ADD_FAILURE();
          }
          ADD_FAILURE();
        }();

        co_await co_nothrow(gen.next());
        try {
          co_await co_nothrow(gen.next());
        } catch (...) {
          ADD_FAILURE();
        }
        ADD_FAILURE();
      }()));
  EXPECT_TRUE(res.hasException<std::runtime_error>());
}

TEST(AsyncGenerator, CoAwaitNothrowMultiTask) {
  auto res =
      folly::coro::blockingWait(co_awaitTry([]() -> folly::coro::Task<void> {
        auto gen = co_await
            []() -> folly::coro::Task<folly::coro::AsyncGenerator<int>> {
          auto gen = []() -> folly::coro::AsyncGenerator<int> {
            co_yield 42;
            throw std::runtime_error("");
          }();

          co_await co_nothrow(gen.next());
          co_return gen;
        }();

        try {
          co_await co_nothrow(gen.next());
        } catch (...) {
          ADD_FAILURE();
        }
        ADD_FAILURE();
      }()));
  EXPECT_TRUE(res.hasException<std::runtime_error>());
}

#endif
