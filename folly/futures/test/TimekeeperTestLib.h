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

#pragma once

#include <thread>
#include <vector>

#include <fmt/format.h>
#include <glog/logging.h>
#include <folly/DefaultKeepAliveExecutor.h>
#include <folly/Random.h>
#include <folly/Singleton.h>
#include <folly/executors/GlobalExecutor.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/executors/SerialExecutor.h>
#include <folly/executors/VirtualExecutor.h>
#include <folly/futures/Future.h>
#include <folly/portability/GTest.h>

namespace folly {

using namespace std::chrono_literals;
using std::chrono::steady_clock;

template <class Tk>
class TimekeeperTest : public testing::Test {
 protected:
  void SetUp() override {
    // Replace the default timekeeper with the class under test, and verify that
    // the replacement was successful.
    Singleton<Timekeeper, detail::TimekeeperSingletonTag>::make_mock([] {
      return new Tk;
    });
    ASSERT_TRUE(
        dynamic_cast<Tk*>(detail::getTimekeeperSingleton().get()) != nullptr);
  }

  void TearDown() override {
    // Invalidate any mocks that were installed.
    folly::SingletonVault::singleton()->destroyInstances();
    folly::SingletonVault::singleton()->reenableInstances();
  }
};

TYPED_TEST_SUITE_P(TimekeeperTest);

TYPED_TEST_P(TimekeeperTest, After) {
  auto t1 = steady_clock::now();
  auto f = detail::getTimekeeperSingleton()->after(10ms);
  EXPECT_FALSE(f.isReady());
  std::move(f).get();
  auto t2 = steady_clock::now();

  EXPECT_GE(t2 - t1, 10ms);
}

TYPED_TEST_P(TimekeeperTest, AfterUnsafe) {
  auto t1 = steady_clock::now();
  auto f = detail::getTimekeeperSingleton()->afterUnsafe(10ms);
  EXPECT_FALSE(f.isReady());
  std::move(f).get();
  auto t2 = steady_clock::now();

  EXPECT_GE(t2 - t1, 10ms);
}

TYPED_TEST_P(TimekeeperTest, FutureGet) {
  Promise<int> p;
  auto t = std::thread([&] { p.setValue(42); });
  EXPECT_EQ(42, p.getFuture().get());
  t.join();
}

TYPED_TEST_P(TimekeeperTest, FutureGetBeforeTimeout) {
  Promise<int> p;
  auto t = std::thread([&] { p.setValue(42); });
  // Technically this is a race and if the test server is REALLY overloaded
  // and it takes more than a second to do that thread it could be flaky. But
  // I want a low timeout (in human terms) so if this regresses and someone
  // runs it by hand they're not sitting there forever wondering why it's
  // blocked, and get a useful error message instead. If it does get flaky,
  // empirically increase the timeout to the point where it's very improbable.
  EXPECT_EQ(42, p.getFuture().get(std::chrono::seconds(2)));
  t.join();
}

TYPED_TEST_P(TimekeeperTest, FutureGetTimeout) {
  Promise<int> p;
  EXPECT_THROW(p.getFuture().get(1ms), folly::FutureTimeout);
}

TYPED_TEST_P(TimekeeperTest, FutureSleep) {
  auto t1 = steady_clock::now();
  futures::sleep(1ms).get();
  EXPECT_GE(steady_clock::now() - t1, 1ms);
}

TYPED_TEST_P(TimekeeperTest, FutureDelayed) {
  auto t1 = steady_clock::now();
  auto dur =
      makeFuture()
          .delayed(1ms)
          .thenValue([=](auto&&) { return steady_clock::now() - t1; })
          .get();

  EXPECT_GE(dur, 1ms);
}

TYPED_TEST_P(TimekeeperTest, SemiFutureDelayed) {
  auto t1 = steady_clock::now();
  auto dur =
      makeSemiFuture()
          .delayed(1ms)
          .toUnsafeFuture()
          .thenValue([=](auto&&) { return steady_clock::now() - t1; })
          .get();

  EXPECT_GE(dur, 1ms);
}

TYPED_TEST_P(TimekeeperTest, FutureDelayedStickyExecutor) {
  // Check that delayed without an executor binds the inline executor.
  {
    auto t1 = steady_clock::now();
    std::thread::id timekeeper_thread_id =
        folly::detail::getTimekeeperSingleton()
            // Ensure that the continuation is run almost certainly in the
            // timekeeper's thread.
            ->after(100ms)
            .toUnsafeFuture()
            .thenValue([](auto&&) { return std::this_thread::get_id(); })
            .get();
    std::thread::id task_thread_id{};
    auto dur =
        makeFuture()
            .delayed(1ms)
            .thenValue([=, &task_thread_id](auto&&) {
              task_thread_id = std::this_thread::get_id();
              return steady_clock::now() - t1;
            })
            .get();

    EXPECT_GE(dur, 1ms);
    EXPECT_EQ(timekeeper_thread_id, task_thread_id);
  }

  // Check that delayed applied to an executor returns a future that binds
  // to the same executor as was input.
  {
    auto t1 = steady_clock::now();
    std::thread::id driver_thread_id{};
    std::thread::id first_task_thread_id{};
    std::thread::id second_task_thread_id{};
    folly::ManualExecutor me;
    std::atomic<bool> stop_signal{false};
    std::thread me_driver{[&me, &driver_thread_id, &stop_signal] {
      driver_thread_id = std::this_thread::get_id();
      while (!stop_signal) {
        me.run();
      }
    }};
    auto dur =
        makeSemiFuture()
            .via(&me)
            .thenValue([&first_task_thread_id](auto&&) {
              first_task_thread_id = std::this_thread::get_id();
            })
            .delayed(1ms)
            .thenValue([=, &second_task_thread_id](auto&&) {
              second_task_thread_id = std::this_thread::get_id();
              return steady_clock::now() - t1;
            })
            .get();
    stop_signal = true;
    me_driver.join();
    EXPECT_GE(dur, 1ms);
    EXPECT_EQ(driver_thread_id, first_task_thread_id);
    EXPECT_EQ(driver_thread_id, second_task_thread_id);
  }
}

TYPED_TEST_P(TimekeeperTest, FutureWithinThrows) {
  Promise<int> p;
  auto f = p.getFuture().within(1ms).thenError(
      tag_t<FutureTimeout>{}, [](auto&&) { return -1; });

  EXPECT_EQ(-1, std::move(f).get());
}

TYPED_TEST_P(TimekeeperTest, SemiFutureWithinThrows) {
  Promise<int> p;
  auto f = p.getSemiFuture().within(1ms).toUnsafeFuture().thenError(
      tag_t<FutureTimeout>{}, [](auto&&) { return -1; });

  EXPECT_EQ(-1, std::move(f).get());
}

TYPED_TEST_P(TimekeeperTest, FutureWithinAlreadyComplete) {
  auto f = makeFuture(42).within(1ms).thenError(
      tag_t<FutureTimeout>{}, [&](auto&&) { return -1; });

  EXPECT_EQ(42, std::move(f).get());
}

TYPED_TEST_P(TimekeeperTest, SemiFutureWithinAlreadyComplete) {
  auto f = makeSemiFuture(42).within(1ms).toUnsafeFuture().thenError(
      tag_t<FutureTimeout>{}, [&](auto&&) { return -1; });

  EXPECT_EQ(42, std::move(f).get());
}

TYPED_TEST_P(TimekeeperTest, FutureWithinFinishesInTime) {
  Promise<int> p;
  auto f =
      p.getFuture()
          .within(std::chrono::minutes(1))
          .thenError(tag_t<FutureTimeout>{}, [&](auto&&) { return -1; });
  p.setValue(42);

  EXPECT_EQ(42, std::move(f).get());
}

TYPED_TEST_P(TimekeeperTest, SemiFutureWithinFinishesInTime) {
  Promise<int> p;
  auto f =
      p.getSemiFuture()
          .within(std::chrono::minutes(1))
          .toUnsafeFuture()
          .thenError(tag_t<FutureTimeout>{}, [&](auto&&) { return -1; });
  p.setValue(42);

  EXPECT_EQ(42, std::move(f).get());
}

TYPED_TEST_P(TimekeeperTest, FutureWithinVoidSpecialization) {
  makeFuture().within(1ms);
}

TYPED_TEST_P(TimekeeperTest, SemiFutureWithinVoidSpecialization) {
  makeSemiFuture().within(1ms);
}

TYPED_TEST_P(TimekeeperTest, FutureWithinException) {
  Promise<Unit> p;
  auto f = p.getFuture().within(10ms, std::runtime_error("expected"));
  EXPECT_THROW(std::move(f).get(), std::runtime_error);
}

TYPED_TEST_P(TimekeeperTest, SemiFutureWithinException) {
  Promise<Unit> p;
  auto f = p.getSemiFuture().within(10ms, std::runtime_error("expected"));
  EXPECT_THROW(std::move(f).get(), std::runtime_error);
}

TYPED_TEST_P(TimekeeperTest, OnTimeout) {
  bool flag = false;
  makeFuture(42)
      .delayed(10 * 1ms)
      .onTimeout(
          0ms,
          [&] {
            flag = true;
            return -1;
          })
      .get();
  EXPECT_TRUE(flag);
}

TYPED_TEST_P(TimekeeperTest, OnTimeoutComplete) {
  bool flag = false;
  makeFuture(42)
      .onTimeout(
          0ms,
          [&] {
            flag = true;
            return -1;
          })
      .get();
  EXPECT_FALSE(flag);
}

TYPED_TEST_P(TimekeeperTest, OnTimeoutReturnsFuture) {
  bool flag = false;
  makeFuture(42)
      .delayed(10 * 1ms)
      .onTimeout(
          0ms,
          [&] {
            flag = true;
            return makeFuture(-1);
          })
      .get();
  EXPECT_TRUE(flag);
}

TYPED_TEST_P(TimekeeperTest, OnTimeoutVoid) {
  makeFuture().delayed(1ms).onTimeout(0ms, [&] {});
  makeFuture().delayed(1ms).onTimeout(0ms, [&] {
    return makeFuture<Unit>(std::runtime_error("expected"));
  });
  // just testing compilation here
}

TYPED_TEST_P(TimekeeperTest, InterruptDoesntCrash) {
  auto f = futures::sleep(10s);
  f.cancel();
}

TYPED_TEST_P(TimekeeperTest, ChainedInterruptTest) {
  bool test = false;
  auto f = futures::sleep(100ms).deferValue([&](auto&&) { test = true; });
  f.cancel();
  f.wait();
  EXPECT_FALSE(test);
}

TYPED_TEST_P(TimekeeperTest, FutureWithinChainedInterruptTest) {
  bool test = false;
  Promise<Unit> p;
  p.setInterruptHandler([&test, &p](const exception_wrapper& ex) {
    ex.handle([&test](const FutureCancellation& /* cancellation */) {
      test = true;
    });
    p.setException(ex);
  });
  auto f = p.getFuture().within(100ms);
  EXPECT_FALSE(test) << "Sanity check";
  f.cancel();
  f.wait();
  EXPECT_TRUE(test);
}

TYPED_TEST_P(TimekeeperTest, SemiFutureWithinChainedInterruptTest) {
  bool test = false;
  Promise<Unit> p;
  p.setInterruptHandler([&test, &p](const exception_wrapper& ex) {
    ex.handle([&test](const FutureCancellation& /* cancellation */) {
      test = true;
    });
    p.setException(ex);
  });
  auto f = p.getSemiFuture().within(100ms);
  EXPECT_FALSE(test) << "Sanity check";
  f.cancel();
  f.wait();
  EXPECT_TRUE(test);
}

TYPED_TEST_P(TimekeeperTest, Executor) {
  class ExecutorTester : public DefaultKeepAliveExecutor {
   public:
    virtual void add(Func f) override {
      count++;
      f();
    }
    void join() { joinKeepAlive(); }
    std::atomic<int> count{0};
  };

  {
    Promise<Unit> p;
    ExecutorTester tester;
    auto f =
        p.getFuture()
            .via(&tester)
            .within(std::chrono::seconds(10))
            .thenValue([&](auto&&) {});
    p.setValue();
    std::move(f).get();
    tester.join();
    EXPECT_EQ(3, tester.count);
  }

  {
    Promise<Unit> p;
    ExecutorTester tester;
    auto f =
        p.getFuture()
            .via(&tester)
            .within(std::chrono::milliseconds(10))
            .thenValue([&](auto&&) {});
    EXPECT_THROW(std::move(f).get(), FutureTimeout);
    p.setValue();
    tester.join();
    EXPECT_EQ(3, tester.count);
  }
}

// TODO(5921764)
/*
TYPED_TEST_P(TimekeeperTest, OnTimeoutPropagates) {
  bool flag = false;
  EXPECT_THROW(
    makeFuture(42).delayed(1ms)
      .onTimeout(0ms, [&]{ flag = true; })
      .get(),
    FutureTimeout);
  EXPECT_TRUE(flag);
}
*/

TYPED_TEST_P(TimekeeperTest, AtBeforeNow) {
  auto f = detail::getTimekeeperSingleton()->at(steady_clock::now() - 10s);
  EXPECT_TRUE(f.isReady());
  EXPECT_FALSE(f.hasException());
}

TYPED_TEST_P(TimekeeperTest, HowToCastDuration) {
  // I'm not sure whether this rounds up or down but it's irrelevant for the
  // purpose of this example.
  auto f = detail::getTimekeeperSingleton()->after(
      std::chrono::duration_cast<Duration>(std::chrono::nanoseconds(1)));
}

TYPED_TEST_P(TimekeeperTest, Destruction) {
  folly::Optional<TypeParam> tk{std::in_place};
  auto f = tk->after(std::chrono::seconds(10));
  EXPECT_FALSE(f.isReady());
  tk.reset();
  EXPECT_TRUE(f.isReady());
  EXPECT_TRUE(f.hasException());
}

TYPED_TEST_P(TimekeeperTest, ConcurrentDestructionAndCancellation) {
  folly::Optional<TypeParam> tk{std::in_place};
  auto f = tk->after(std::chrono::seconds(10));
  EXPECT_FALSE(f.isReady());
  std::thread t{[&] { f.cancel(); }};
  tk.reset();
  t.join();
  EXPECT_TRUE(f.isReady());
  EXPECT_TRUE(f.hasException());
}

namespace {

template <class Tk>
void stressTest(
    std::chrono::microseconds duration, std::chrono::microseconds period) {
  using usec = std::chrono::microseconds;

  folly::Optional<Tk> tk{std::in_place};
  std::vector<std::thread> workers;

  // Run continuations on a serial executor so we don't need synchronization to
  // modify shared state.
  folly::Optional<VirtualExecutor> continuationsThread{
      std::in_place, SerialExecutor::create(folly::getGlobalCPUExecutor())};
  size_t numCompletions = 0;
  usec sumDelay{0};
  usec maxDelay{0};

  // Wait for any lazy initialization in the timekeeper and executor.
  tk->after(1ms).via(&*continuationsThread).then([](auto&&) {}).get();

  static const auto jitter = [](usec avg) {
    // Center around average.
    return usec(folly::Random::rand64(2 * avg.count()));
  };

  static const auto jitterSleep = [](steady_clock::time_point& now, usec avg) {
    now += jitter(avg);
    if (now - steady_clock::now() < 10us) {
      // Busy-sleep if yielding the CPU would take too long.
      while (now > steady_clock::now()) {
      }
    } else {
      /* sleep override */ std::this_thread::sleep_until(now);
    }
  };

  for (size_t i = 0; i < 8; ++i) {
    workers.emplace_back([&] {
      std::vector<Future<Unit>> futures;
      for (auto start = steady_clock::now(), now = start;
           now < start + duration;
           jitterSleep(now, period)) {
        // Use the test duration as rough range for the timeouts.
        auto dur = jitter(duration);
        auto expected = steady_clock::now() + dur;
        futures.push_back(
            tk->after(dur)
                .toUnsafeFuture()
                .thenValue([](auto&&) { return steady_clock::now(); })
                .via(&*continuationsThread)
                .thenValue([&, expected](auto fired) {
                  auto delay =
                      std::chrono::duration_cast<usec>(fired - expected);
                  // TODO(ott): HHWheelTimer-based timekeepers round down the
                  // timeout, so they may fire early, for now ignore this.
                  if (delay < 0us && delay > -1ms) {
                    delay = 0us;
                  }
                  ASSERT_GE(delay.count(), 0);
                  ++numCompletions;
                  sumDelay += delay;
                  maxDelay = std::max(maxDelay, delay);
                }));
      }

      for (auto& f : futures) {
        // While at it, check that canceling the future after it has been
        // fulfilled has no effect. To do so, we wait non-destructively.
        while (!f.isReady()) {
          /* sleep override */ std::this_thread::sleep_for(1ms);
        }
        f.cancel();
        EXPECT_NO_THROW(std::move(f).get());
      }
    });
  }

  // Add a worker that cancels all its futures.
  size_t numAttemptedCancellations = 0;
  size_t numCancellations = 0;
  workers.emplace_back([&] {
    std::vector<SemiFuture<Unit>> futures;
    for (auto start = steady_clock::now(), now = start; now < start + duration;
         jitterSleep(now, 1ms)) {
      // Pick a wide range of durations to exercise various positions in the
      // sequence of timeouts.
      auto dur = 5ms + jitter(5s);
      futures.push_back(tk->after(dur));
      // Cancel the future scheduled in the previous iteration.
      if (futures.size() > 1) {
        futures[futures.size() - 2].cancel();
      }
    }

    futures.back().cancel();
    numAttemptedCancellations = futures.size();

    for (auto& f : futures) {
      if (std::move(f).getTry().hasException<FutureCancellation>()) {
        ++numCancellations;
      }
    }
  });

  // Add a few timeouts that will not survive the timekeeper.
  std::vector<SemiFuture<Unit>> shutdownFutures;
  for (size_t i = 0; i < 10; ++i) {
    shutdownFutures.push_back(tk->after(10min));
  }

  for (auto& worker : workers) {
    worker.join();
  }

  continuationsThread.reset(); // Wait for all continuations.
  ASSERT_GT(numCompletions, 0);

  // In principle the delay is unbounded (depending on the state of the system),
  // so we cannot have any upper bound that is both meaningful and reliable, but
  // we can log it to manually inspect the behavior.
  LOG(INFO) << fmt::format(
      "Successful completions: {}, avg delay: {} us, max delay: {} us ",
      numCompletions,
      sumDelay.count() / numCompletions,
      maxDelay.count());

  // Similarly, a cancellation may be processed only after the future has fired,
  // but in normal conditions this should never happen.
  LOG(INFO) << fmt::format(
      "Attempted cancellations: {}, successful: {}",
      numAttemptedCancellations,
      numCancellations);

  tk.reset();
  for (auto& f : shutdownFutures) {
    EXPECT_TRUE(std::move(f).getTry().hasException<FutureNoTimekeeper>());
  }
}

} // namespace

TYPED_TEST_P(TimekeeperTest, Stress) {
  stressTest<TypeParam>(/* duration */ 1s, /* period */ 10ms);
}

TYPED_TEST_P(TimekeeperTest, StressHighContention) {
  // Test that nothing breaks when scheduling a large number of timeouts
  // concurrently. In this case the timekeeper thread will be overloaded, so the
  // measured delays are going to be large.
  stressTest<TypeParam>(/* duration */ 50ms, /* period */ 5us);
}

REGISTER_TYPED_TEST_SUITE_P(
    TimekeeperTest,
    After,
    AfterUnsafe,
    FutureGet,
    FutureGetBeforeTimeout,
    FutureGetTimeout,
    FutureSleep,
    FutureDelayed,
    SemiFutureDelayed,
    FutureDelayedStickyExecutor,
    FutureWithinThrows,
    SemiFutureWithinThrows,
    FutureWithinAlreadyComplete,
    SemiFutureWithinAlreadyComplete,
    FutureWithinFinishesInTime,
    SemiFutureWithinFinishesInTime,
    FutureWithinVoidSpecialization,
    SemiFutureWithinVoidSpecialization,
    FutureWithinException,
    SemiFutureWithinException,
    OnTimeout,
    OnTimeoutComplete,
    OnTimeoutReturnsFuture,
    OnTimeoutVoid,
    InterruptDoesntCrash,
    ChainedInterruptTest,
    FutureWithinChainedInterruptTest,
    SemiFutureWithinChainedInterruptTest,
    Executor,
    AtBeforeNow,
    HowToCastDuration,
    Destruction,
    ConcurrentDestructionAndCancellation,
    Stress,
    StressHighContention);

} // namespace folly
