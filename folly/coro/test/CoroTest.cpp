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

#include <folly/CancellationToken.h>
#include <folly/Chrono.h>
#include <folly/coro/Baton.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Coroutine.h>
#include <folly/coro/CurrentExecutor.h>
#include <folly/coro/DetachOnCancel.h>
#include <folly/coro/GtestHelpers.h>
#include <folly/coro/Invoke.h>
#include <folly/coro/Sleep.h>
#include <folly/coro/Task.h>
#include <folly/coro/TimedWait.h>
#include <folly/coro/WithCancellation.h>
#include <folly/executors/InlineExecutor.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/fibers/Semaphore.h>
#include <folly/futures/Future.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/lang/Assume.h>
#include <folly/portability/GTest.h>

#if FOLLY_HAS_COROUTINES

using namespace folly;

class CoroTest : public testing::Test {};

TEST_F(CoroTest, Basic) {
  ManualExecutor executor;
  auto task42 = []() -> coro::Task<int> { co_return 42; };
  auto future = co_withExecutor(&executor, task42()).start();

  EXPECT_FALSE(future.isReady());

  executor.drain();

  EXPECT_TRUE(future.isReady());
  EXPECT_EQ(42, std::move(future).get());
}

TEST_F(CoroTest, BasicSemiFuture) {
  ManualExecutor executor;
  auto task42 = []() -> coro::Task<int> { co_return 42; };
  auto future = task42().semi().via(&executor);

  EXPECT_FALSE(future.isReady());

  executor.drain();

  EXPECT_TRUE(future.isReady());
  EXPECT_EQ(42, std::move(future).get());
}

TEST_F(CoroTest, BasicFuture) {
  ManualExecutor executor;

  auto task42 = []() -> coro::Task<int> { co_return 42; };
  auto future = co_withExecutor(&executor, task42()).start();

  EXPECT_FALSE(future.isReady());

  EXPECT_EQ(42, std::move(future).via(&executor).getVia(&executor));
}

coro::Task<void> taskVoid() {
  auto task42 = []() -> coro::Task<int> { co_return 42; };
  (void)co_await task42();
  co_return;
}

TEST_F(CoroTest, Basic2) {
  ManualExecutor executor;
  auto future = co_withExecutor(&executor, taskVoid()).start();

  EXPECT_FALSE(future.isReady());

  executor.drain();

  EXPECT_TRUE(future.isReady());
}

TEST_F(CoroTest, TaskOfMoveOnly) {
  auto f = []() -> coro::Task<std::unique_ptr<int>> {
    co_return std::make_unique<int>(123);
  };

  auto p = coro::blockingWait(f());
  EXPECT_TRUE(p);
  EXPECT_EQ(123, *p);
}

coro::Task<void> taskSleep() {
  (void)co_await coro::sleep(std::chrono::seconds{1});
  co_return;
}

TEST_F(CoroTest, Sleep) {
  ScopedEventBaseThread evbThread;

  auto startTime = std::chrono::steady_clock::now();
  auto task = co_withExecutor(evbThread.getEventBaseKeepAlive(), taskSleep());

  coro::blockingWait(std::move(task));

  // The total time should be roughly 1 second. Some builds, especially
  // optimized ones, may result in slightly less than 1 second, so we perform
  // rounding here.
  auto totalTime = std::chrono::steady_clock::now() - startTime;
  EXPECT_GE(
      std::chrono::round<std::chrono::seconds>(totalTime),
      std::chrono::seconds{1});
}

TEST_F(CoroTest, ExecutorKeepAlive) {
  auto future = [] {
    ScopedEventBaseThread evbThread;

    return co_withExecutor(evbThread.getEventBaseKeepAlive(), taskSleep())
        .start();
  }();
  EXPECT_TRUE(future.isReady());
}

TEST_F(CoroTest, ExecutorKeepAliveDummy) {
  struct CountingExecutor : public ManualExecutor {
    bool keepAliveAcquire() noexcept override {
      ++keepAliveCounter;
      return true;
    }
    void keepAliveRelease() noexcept override { --keepAliveCounter; }

    size_t keepAliveCounter{0};
  };

  struct ExecutorRec {
    static coro::Task<void> go(int depth) {
      if (depth == 0) {
        co_return;
      }

      auto executor =
          dynamic_cast<CountingExecutor*>(co_await coro::co_current_executor);
      DCHECK(executor);

      // Note, extra keepalives are being kept by the Futures.
      EXPECT_EQ(3, executor->keepAliveCounter);

      co_await go(depth - 1);
    }
  };

  CountingExecutor executor;
  co_withExecutor(&executor, ExecutorRec::go(42))
      .start()
      .via(&executor)
      .getVia(&executor);
}

TEST_F(CoroTest, FutureThrow) {
  auto taskException = []() -> coro::Task<int> {
    throw std::runtime_error("Test exception");
    co_return 42;
  };

  ManualExecutor executor;
  auto future = co_withExecutor(&executor, taskException()).start();

  EXPECT_FALSE(future.isReady());

  executor.drain();

  EXPECT_TRUE(future.isReady());
  EXPECT_THROW(std::move(future).get(), std::runtime_error);
}

coro::Task<int> taskRecursion(int depth) {
  if (depth > 0) {
    EXPECT_EQ(depth - 1, co_await taskRecursion(depth - 1));
  } else {
    (void)co_await coro::sleep(std::chrono::seconds{1});
  }

  co_return depth;
}

TEST_F(CoroTest, LargeStack) {
  ScopedEventBaseThread evbThread;
  auto task =
      co_withExecutor(evbThread.getEventBaseKeepAlive(), taskRecursion(50000));

  EXPECT_EQ(50000, coro::blockingWait(std::move(task)));
}

TEST_F(CoroTest, NestedThreads) {
  auto taskThreadNested = [](std::thread::id threadId) -> coro::Task<void> {
    EXPECT_EQ(threadId, std::this_thread::get_id());
    (void)co_await coro::sleep(std::chrono::seconds{1});
    EXPECT_EQ(threadId, std::this_thread::get_id());
    co_return;
  };

  auto taskThread = [&]() -> coro::Task<int> {
    auto threadId = std::this_thread::get_id();

    // BUG: Under @mode/clang-opt builds this object is placed on the coroutine
    // frame and the code for the constructor assumes that it is allocated on
    // a 16-byte boundary. However, when placed in the coroutine frame it can
    // end up at a location that is not 16-byte aligned. This causes a SIGSEGV
    // when performing a store to members that uses SSE instructions.
    folly::ScopedEventBaseThread evbThread;

    co_await co_withExecutor(
        evbThread.getEventBaseKeepAlive(),
        taskThreadNested(evbThread.getThreadId()));

    EXPECT_EQ(threadId, std::this_thread::get_id());

    co_return 42;
  };

  ScopedEventBaseThread evbThread;
  auto task = co_withExecutor(evbThread.getEventBaseKeepAlive(), taskThread());
  EXPECT_EQ(42, coro::blockingWait(std::move(task)));
}

TEST_F(CoroTest, CurrentExecutor) {
  auto taskGetCurrentExecutor = [](Executor* executor) -> coro::Task<int> {
    auto current = co_await coro::co_current_executor;
    EXPECT_EQ(executor, current);
    auto task42 = []() -> coro::Task<int> { co_return 42; };
    co_return co_await co_withExecutor(current, task42());
  };

  ScopedEventBaseThread evbThread;
  auto task = co_withExecutor(
      evbThread.getEventBaseKeepAlive(),
      taskGetCurrentExecutor(evbThread.getEventBase()));
  EXPECT_EQ(42, coro::blockingWait(std::move(task)));
}

TEST_F(CoroTest, TimedWaitFuture) {
  auto taskTimedWaitFuture = []() -> coro::Task<void> {
    auto ex = co_await coro::co_current_executor;
    auto fastFuture =
        futures::sleep(std::chrono::milliseconds{50})
            .via(ex)
            .thenValue([](Unit) { return 42; });
    auto fastResult = co_await coro::timed_wait(
        std::move(fastFuture), std::chrono::milliseconds{100});
    EXPECT_TRUE(fastResult);
    EXPECT_EQ(42, *fastResult);

    struct ExpectedException : public std::runtime_error {
      ExpectedException() : std::runtime_error("ExpectedException") {}
    };

    auto throwingFuture =
        futures::sleep(std::chrono::milliseconds{50})
            .via(ex)
            .thenValue([](Unit) { throw ExpectedException(); });
    EXPECT_THROW(
        (void)co_await coro::timed_wait(
            std::move(throwingFuture), std::chrono::milliseconds{100}),
        ExpectedException);

    auto [lifetimePromise, lifetimeFuture] =
        folly::makePromiseContract<folly::Unit>(ex);
    auto slowFuture =
        futures::sleep(std::chrono::milliseconds{200})
            .via(ex)
            .thenValue(
                [lifetimePromise_ = std::move(lifetimePromise)](Unit) mutable {
                  lifetimePromise_.setValue();
                  return 42;
                });
    auto slowResult = co_await coro::timed_wait(
        std::move(slowFuture), std::chrono::milliseconds{100});
    EXPECT_FALSE(slowResult);

    // Ensure that task completes for safe executor lifetimes
    (void)co_await std::move(lifetimeFuture);

    co_return;
  };

  coro::blockingWait(taskTimedWaitFuture());
}

TEST_F(CoroTest, TimedWaitTask) {
  auto taskTimedWaitTask = []() -> coro::Task<void> {
    auto fastTask = []() -> coro::Task<int> {
      co_await coro::sleep(std::chrono::milliseconds{50});
      co_return 42;
    }();
    auto fastResult = co_await coro::timed_wait(
        std::move(fastTask), std::chrono::milliseconds{100});
    EXPECT_TRUE(fastResult);
    EXPECT_EQ(42, *fastResult);

    struct ExpectedException : public std::runtime_error {
      ExpectedException() : std::runtime_error("ExpectedException") {}
    };

    auto throwingTask = []() -> coro::Task<void> {
      co_await coro::sleep(std::chrono::milliseconds{50});
      throw ExpectedException();
    }();
    EXPECT_THROW(
        (void)co_await coro::timed_wait(
            std::move(throwingTask), std::chrono::milliseconds{100}),
        ExpectedException);

    auto slowTask = []() -> coro::Task<int> {
      co_await futures::sleep(std::chrono::milliseconds{200});
      co_return 42;
    }();
    auto slowResult = co_await coro::timed_wait(
        std::move(slowTask), std::chrono::milliseconds{100});
    EXPECT_FALSE(slowResult);

    co_return;
  };

  coro::blockingWait(taskTimedWaitTask());
}

TEST_F(CoroTest, TimedWaitKeepAlive) {
  auto start = std::chrono::steady_clock::now();
  coro::blockingWait([]() -> coro::Task<void> {
    co_await coro::timed_wait(
        coro::sleep(std::chrono::milliseconds{100}), std::chrono::seconds{60});
    co_return;
  }());
  auto duration = std::chrono::steady_clock::now() - start;
  EXPECT_LE(duration, std::chrono::seconds{30});
}

TEST_F(CoroTest, TimedWaitNonCopyable) {
  auto task = []() -> coro::Task<std::unique_ptr<int>> {
    co_return std::make_unique<int>(42);
  }();
  EXPECT_EQ(
      42,
      **coro::blockingWait(
          [&]() -> coro::Task<folly::Optional<std::unique_ptr<int>>> {
            co_return co_await coro::timed_wait(
                std::move(task), std::chrono::seconds{60});
          }()));
}

namespace {

template <int value>
struct AwaitableInt {
  bool await_ready() const noexcept { return true; }

  bool await_suspend(coro::coroutine_handle<>) { assume_unreachable(); }

  int await_resume() { return value; }
};

struct AwaitableWithOperator {};

AwaitableInt<42> operator co_await(const AwaitableWithOperator&) {
  return {};
}

struct AwaitableWithMemberOperator {
  AwaitableInt<42> operator co_await() { return {}; }
};

[[maybe_unused]] AwaitableInt<24> operator co_await(
    const AwaitableWithMemberOperator&) {
  return {};
}

} // namespace

TEST_F(CoroTest, AwaitableWithOperator) {
  auto taskAwaitableWithOperator = []() -> coro::Task<int> {
    co_return co_await AwaitableWithOperator();
  };

  EXPECT_EQ(42, coro::blockingWait(taskAwaitableWithOperator()));
}

TEST_F(CoroTest, AwaitableWithMemberOperator) {
  auto taskAwaitableWithMemberOperator = []() -> coro::Task<int> {
    co_return co_await AwaitableWithMemberOperator();
  };

  EXPECT_EQ(42, coro::blockingWait(taskAwaitableWithMemberOperator()));
}

TEST_F(CoroTest, Baton) {
  auto taskBaton = [](fibers::Baton& baton) -> coro::Task<int> {
    co_await baton;
    co_return 42;
  };

  ManualExecutor executor;
  fibers::Baton baton;
  auto future = co_withExecutor(&executor, taskBaton(baton)).start();

  EXPECT_FALSE(future.isReady());

  executor.drain();

  EXPECT_FALSE(future.isReady());

  baton.post();
  executor.drain();

  EXPECT_TRUE(future.isReady());
  EXPECT_EQ(42, std::move(future).get());
}

TEST_F(CoroTest, FulfilledFuture) {
  auto taskFuture = [](auto value) -> coro::Task<decltype(value)> {
    co_return co_await folly::makeFuture(std::move(value));
  };

  EXPECT_EQ(42, coro::blockingWait(taskFuture(42)));
}

TEST_F(CoroTest, MoveOnlyReturn) {
  auto taskFuture = [](auto value) -> coro::Task<decltype(value)> {
    co_return co_await folly::makeFuture(std::move(value));
  };

  EXPECT_EQ(42, *coro::blockingWait(taskFuture(std::make_unique<int>(42))));
}

TEST_F(CoroTest, co_invoke) {
  ManualExecutor executor;
  Promise<folly::Unit> p;
  auto coroFuture =
      co_withExecutor(
          &executor,
          coro::co_invoke(
              [f = p.getSemiFuture()]() mutable -> coro::Task<void> {
                (void)co_await std::move(f);
                co_return;
              }))
          .start();
  executor.drain();
  EXPECT_FALSE(coroFuture.isReady());

  p.setValue(folly::unit);

  executor.drain();
  EXPECT_TRUE(coroFuture.isReady());
}

TEST_F(CoroTest, Semaphore) {
  static constexpr size_t kTasks = 10;
  static constexpr size_t kIterations = 10000;
  static constexpr size_t kNumTokens = 10;
  static constexpr size_t kNumThreads = 16;

  fibers::Semaphore sem(kNumTokens);

  struct Worker {
    explicit Worker(fibers::Semaphore& s) : sem(s), t([&] { run(); }) {}

    void run() {
      folly::EventBase evb;

      {
        std::shared_ptr<folly::EventBase> completionCounter(
            &evb, [](folly::EventBase* evb_) { evb_->terminateLoopSoon(); });

        for (size_t i = 0; i < kTasks; ++i) {
          co_withExecutor(
              &evb,
              coro::co_invoke([&, completionCounter]() -> coro::Task<void> {
                for (size_t j = 0; j < kIterations; ++j) {
                  co_await sem.co_wait();
                  ++counter;
                  sem.signal();
                  --counter;

                  EXPECT_LT(counter, kNumTokens);
                  EXPECT_GE(counter, 0);
                }
              }))
              .start();
        }
      }
      evb.loopForever();
    }

    fibers::Semaphore& sem;
    int counter{0};
    std::thread t;
  };

  std::vector<Worker> workers;
  workers.reserve(kNumThreads);
  for (size_t i = 0; i < kNumThreads; ++i) {
    workers.emplace_back(sem);
  }

  for (auto& worker : workers) {
    worker.t.join();
  }

  for (auto& worker : workers) {
    EXPECT_EQ(0, worker.counter);
  }
}

TEST_F(CoroTest, SemaphoreWaitWhenCancellationAlreadyRequested) {
  folly::coro::blockingWait([&]() -> folly::coro::Task<> {
    folly::CancellationSource cancelSource;
    cancelSource.requestCancellation();

    // Run some logic while in an already-cancelled state.
    co_await folly::coro::co_withCancellation(
        cancelSource.getToken(), []() -> folly::coro::Task<> {
          folly::fibers::Semaphore sem{1};

          // If in a signalled state then should complete normally
          co_await sem.co_wait();

          // And the semaphore should no longer be in the signalled state.

          // But if not signalled then should complete with cancellation
          // immediately.
          EXPECT_THROW(co_await sem.co_wait(), folly::OperationCancelled);
        }());
  }());
}

TEST_F(CoroTest, CancelOutstandingSemaphoreWait) {
  struct ExpectedError : std::exception {};

  folly::coro::blockingWait([&]() -> folly::coro::Task<> {
    folly::fibers::Semaphore sem{0};
    try {
      co_await folly::coro::collectAll(
          [&]() -> folly::coro::Task<> {
            EXPECT_THROW(co_await sem.co_wait(), OperationCancelled);
          }(),
          []() -> folly::coro::Task<> {
            co_await folly::coro::co_reschedule_on_current_executor;
            // Completing the second task with an error will cause
            // collectAll() to request cancellation of the other task
            // which should request cancellation of sem.co_wait().
            co_yield folly::coro::co_error(ExpectedError{});
          }());
    } catch (const ExpectedError&) {
    }
  }());
}

TEST_F(CoroTest, CancelOneSemaphoreWaitDoesNotAffectOthers) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    folly::fibers::Semaphore sem{0};

    folly::CancellationSource cancelSource;

    co_await folly::coro::collectAll(
        [&]() -> folly::coro::Task<> {
          EXPECT_THROW(
              (co_await folly::coro::co_withCancellation(
                  cancelSource.getToken(), sem.co_wait())),
              OperationCancelled);
        }(),
        [&]() -> folly::coro::Task<> { co_await sem.co_wait(); }(),
        [&]() -> folly::coro::Task<> {
          co_await folly::coro::co_reschedule_on_current_executor;
          cancelSource.requestCancellation();
          sem.signal();
        }());
  }());
}

TEST_F(CoroTest, FutureTry) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    auto task42 = []() -> coro::Task<int> { co_return 42; };
    auto taskException = []() -> coro::Task<int> {
      throw std::runtime_error("Test exception");
      co_return 42;
    };

    {
      auto result = co_await folly::coro::co_awaitTry(task42().semi());
      EXPECT_TRUE(result.hasValue());
      EXPECT_EQ(42, result.value());
    }

    {
      auto result = co_await folly::coro::co_awaitTry(taskException().semi());
      EXPECT_TRUE(result.hasException());
    }

    {
      auto result = co_await folly::coro::co_awaitTry(
          task42().semi().via(co_await folly::coro::co_current_executor));
      EXPECT_TRUE(result.hasValue());
      EXPECT_EQ(42, result.value());
    }

    {
      auto result = co_await folly::coro::co_awaitTry(
          taskException().semi().via(
              co_await folly::coro::co_current_executor));
      EXPECT_TRUE(result.hasException());
    }
  }());
}

TEST_F(CoroTest, CancellableSleep) {
  using namespace std::chrono;
  using namespace std::chrono_literals;

  CancellationSource cancelSrc;

  auto start = steady_clock::now();
  EXPECT_THROW(
      coro::blockingWait([&]() -> coro::Task<void> {
        co_await coro::collectAll(
            [&]() -> coro::Task<void> {
              co_await coro::co_withCancellation(
                  cancelSrc.getToken(), coro::sleep(10s));
            }(),
            [&]() -> coro::Task<void> {
              co_await coro::co_reschedule_on_current_executor;
              co_await coro::co_reschedule_on_current_executor;
              co_await coro::co_reschedule_on_current_executor;
              cancelSrc.requestCancellation();
            }());
      }()),
      OperationCancelled);
  auto end = steady_clock::now();
  CHECK((end - start) < 1s);
}

TEST_F(CoroTest, DefaultConstructible) {
  coro::blockingWait([]() -> coro::Task<void> {
    struct S {
      int x = 42;
    };

    auto taskS = []() -> coro::Task<S> { co_return {}; };

    auto s = co_await taskS();
    EXPECT_EQ(42, s.x);
  }());
}

FOLLY_PUSH_WARNING
FOLLY_GNU_DISABLE_WARNING("-Wdeprecated-declarations")
TEST(Coro, CoReturnTry) {
  EXPECT_EQ(42, folly::coro::blockingWait([]() -> folly::coro::Task<int> {
              co_return folly::Try<int>(42);
            }()));

  struct ExpectedException : public std::runtime_error {
    ExpectedException() : std::runtime_error("ExpectedException") {}
  };
  EXPECT_THROW(
      folly::coro::blockingWait([]() -> folly::coro::Task<int> {
        co_return folly::Try<int>(ExpectedException());
      }()),
      ExpectedException);

  EXPECT_EQ(42, folly::coro::blockingWait([]() -> folly::coro::Task<int> {
              folly::Try<int> t(42);
              co_return t;
            }()));

  EXPECT_EQ(42, folly::coro::blockingWait([]() -> folly::coro::Task<int> {
              const folly::Try<int> tConst(42);
              co_return tConst;
            }()));

  EXPECT_EQ(
      folly::unit,
      folly::coro::blockingWait([]() -> folly::coro::Task<folly::Unit> {
        co_return folly::Try<folly::Unit>(folly::unit);
      }()));

  EXPECT_EQ(
      folly::unit,
      folly::coro::blockingWait([]() -> folly::coro::Task<folly::Unit> {
        co_return folly::Try<void>();
      }()));
}
FOLLY_POP_WARNING

TEST(Coro, CoThrow) {
  struct ExpectedException : public std::runtime_error {
    ExpectedException() : std::runtime_error("ExpectedException") {}
  };
  EXPECT_THROW(
      folly::coro::blockingWait([]() -> folly::coro::Task<int> {
        co_yield folly::coro::co_error(ExpectedException());
        ADD_FAILURE() << "unreachable";
        // Intentional lack of co_return statement to check
        // that compiler treats code after co_yield co_error()
        // as unreachable.
      }()),
      ExpectedException);

  EXPECT_THROW(
      folly::coro::blockingWait([]() -> folly::coro::Task<void> {
        co_yield folly::coro::co_error(ExpectedException());
        ADD_FAILURE() << "unreachable";
      }()),
      ExpectedException);
}

TEST_F(CoroTest, DetachOnCancel) {
  folly::coro::blockingWait([&]() -> folly::coro::Task<> {
    folly::CancellationSource cancelSource;
    cancelSource.requestCancellation();

    // Run some logic while in an already-cancelled state.
    co_await folly::coro::co_withCancellation(
        cancelSource.getToken(), []() -> folly::coro::Task<> {
          EXPECT_THROW(
              co_await folly::coro::detachOnCancel(
                  folly::futures::sleep(std::chrono::seconds{1})
                      .deferValue([](auto) { return 42; })),
              folly::OperationCancelled);
        }());
  }());

  folly::coro::blockingWait([&]() -> folly::coro::Task<> {
    folly::CancellationSource cancelSource;

    co_await folly::coro::co_withCancellation(
        cancelSource.getToken(), []() -> folly::coro::Task<> {
          EXPECT_EQ(
              42,
              co_await folly::coro::detachOnCancel(
                  folly::futures::sleep(std::chrono::milliseconds{10})
                      .deferValue([](auto) { return 42; })));
        }());
  }());

  folly::coro::blockingWait([&]() -> folly::coro::Task<> {
    folly::CancellationSource cancelSource;

    co_await folly::coro::co_withCancellation(
        cancelSource.getToken(), []() -> folly::coro::Task<> {
          co_await folly::coro::detachOnCancel([]() -> folly::coro::Task<void> {
            co_await folly::futures::sleep(std::chrono::milliseconds{10});
          }());
        }());
  }());
}

struct CountingManualExecutor : public folly::ManualExecutor {
  void add(Func func) override {
    ++addCount_;
    ManualExecutor::add(std::move(func));
  }

  ssize_t getAddCount() const { return addCount_.load(); }

 private:
  std::atomic<ssize_t> addCount_{0};
};

ssize_t runAndCountExecutorAdd(folly::coro::Task<void> task) {
  CountingManualExecutor executor;
  auto future = co_withExecutor(&executor, std::move(task)).start();

  executor.drain();

  EXPECT_TRUE(future.isReady());
  return executor.getAddCount();
}

TEST_F(CoroTest, SemiNoReschedule) {
  auto task42 = []() -> coro::Task<int> { co_return 42; };
  auto semiFuture42 = []() {
    return makeSemiFuture().deferValue([](auto) { return 42; });
  };
  EXPECT_EQ(
      2, // One extra for keepAlive release logic of ManualExecutor
      runAndCountExecutorAdd(folly::coro::co_invoke([&]() -> coro::Task<void> {
        EXPECT_EQ(42, co_await task42());
      })));
  EXPECT_EQ(
      2, // One extra for keepAlive release logic of ManualExecutor
      runAndCountExecutorAdd(folly::coro::co_invoke([&]() -> coro::Task<void> {
        EXPECT_EQ(42, co_await task42().semi());
      })));
  EXPECT_EQ(
      2, // One extra for keepAlive release logic of ManualExecutor
      runAndCountExecutorAdd(folly::coro::co_invoke([&]() -> coro::Task<void> {
        EXPECT_EQ(42, co_await semiFuture42());
      })));
}

CO_TEST(FutureAwaiter, InvalidFutureTerminates) {
  auto fut = makeFuture(42);
  [[maybe_unused]] auto fut2 = std::move(fut);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_DEATH(co_await std::move(fut), "Future invalid");
}

CO_TEST(FutureAwaiter, AlreadyContinuedReturnsError) {
  Promise<int> promise;
  auto fut = promise.getFuture();
  [[maybe_unused]] auto fut2 = fut.via(&InlineExecutor::instance());
  if constexpr (kIsDebug) {
    EXPECT_DEATH(co_await std::move(fut), "state_ != State::OnlyCallback");
  } else {
    auto result = co_await coro::co_awaitTry(std::move(fut));
    EXPECT_TRUE(result.hasException());
    EXPECT_TRUE(
        result.exception().is_compatible_with<FutureAlreadyContinued>());
  }
}
#endif
