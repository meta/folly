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

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cassert>
#include <numeric>
#include <random>
#include <vector>

#include <glog/logging.h>

#include <folly/Random.h>
#include <folly/executors/FunctionScheduler.h>
#include <folly/portability/GTest.h>
#include <folly/portability/Unistd.h>
#include <folly/synchronization/Baton.h>

using namespace folly;
using std::atomic;
using std::chrono::duration_cast;
using std::chrono::microseconds;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

namespace {

/*
 * Helper functions for controlling how long this test takes.
 *
 * Using larger intervals here will make the tests less flaky when run on
 * heavily loaded systems.  However, this will also make the tests take longer
 * to run.
 */
static const auto timeFactor = std::chrono::milliseconds(400);
std::chrono::milliseconds testInterval(int n) {
  return n * timeFactor;
}
int getTicksWithinRange(int n, int min, int max) {
  assert(min <= max);
  n = std::max(min, n);
  n = std::min(max, n);
  return n;
}
void delay(float n) {
  microseconds usec(
      static_cast<microseconds::rep>(
          duration_cast<microseconds>(timeFactor).count() * n));
  usleep(usec.count());
}

} // namespace

TEST(FunctionScheduler, StartAndShutdown) {
  FunctionScheduler fs;
  EXPECT_TRUE(fs.start());
  EXPECT_FALSE(fs.start());
  EXPECT_TRUE(fs.shutdown());
  EXPECT_FALSE(fs.shutdown());
  // start again
  EXPECT_TRUE(fs.start());
  EXPECT_FALSE(fs.start());
  EXPECT_TRUE(fs.shutdown());
  EXPECT_FALSE(fs.shutdown());
}

TEST(FunctionScheduler, SimpleAdd) {
  atomic<int> total{0};
  FunctionScheduler fs;
  fs.addFunction([&] { total += 2; }, testInterval(2), "add2");
  fs.start();
  delay(1);
  EXPECT_EQ(2, total);
  fs.shutdown();
  delay(2);
  EXPECT_EQ(2, total);
}

TEST(FunctionScheduler, AddCancel) {
  atomic<int> total{0};
  FunctionScheduler fs;
  fs.addFunction([&] { total += 2; }, testInterval(2), "add2");
  fs.start();
  delay(1);
  EXPECT_EQ(2, total);
  delay(2);
  EXPECT_EQ(4, total);
  EXPECT_TRUE(fs.cancelFunction("add2"));
  EXPECT_FALSE(fs.cancelFunction("NO SUCH FUNC"));
  delay(2);
  EXPECT_EQ(4, total);
  fs.addFunction([&] { total += 1; }, testInterval(2), "add2");
  delay(1);
  EXPECT_EQ(5, total);
  delay(2);
  EXPECT_EQ(6, total);
  fs.shutdown();
}

TEST(FunctionScheduler, AddCancel2) {
  atomic<int> total{0};
  FunctionScheduler fs;

  // Test adds and cancels while the scheduler is stopped
  EXPECT_FALSE(fs.cancelFunction("add2"));
  fs.addFunction([&] { total += 1; }, testInterval(2), "add2");
  EXPECT_TRUE(fs.cancelFunction("add2"));
  EXPECT_FALSE(fs.cancelFunction("add2"));
  fs.addFunction([&] { total += 2; }, testInterval(2), "add2");
  fs.addFunction([&] { total += 3; }, testInterval(3), "add3");

  EXPECT_EQ(0, total);
  fs.start();
  delay(1);
  EXPECT_EQ(5, total);

  // Cancel add2 while the scheduler is running
  EXPECT_TRUE(fs.cancelFunction("add2"));
  EXPECT_FALSE(fs.cancelFunction("add2"));
  EXPECT_FALSE(fs.cancelFunction("bogus"));

  delay(3);
  EXPECT_EQ(8, total);
  EXPECT_TRUE(fs.cancelFunction("add3"));

  // Test a function that cancels itself
  atomic<int> selfCancelCount{0};
  fs.addFunction(
      [&] {
        ++selfCancelCount;
        if (selfCancelCount > 2) {
          fs.cancelFunction("selfCancel");
        }
      },
      testInterval(1),
      "selfCancel",
      testInterval(1));
  delay(4);
  EXPECT_EQ(3, selfCancelCount);
  EXPECT_FALSE(fs.cancelFunction("selfCancel"));

  // Test a function that schedules another function
  atomic<int> adderCount{0};
  atomic<int> fn2Count = 0;
  auto fn2 = [&] { ++fn2Count; };
  auto fnAdder = [&] {
    ++adderCount;
    if (adderCount == 2) {
      fs.addFunction(fn2, testInterval(3), "fn2", testInterval(2));
    }
  };
  fs.addFunction(fnAdder, testInterval(4), "adder");
  // t0: adder fires
  delay(1); // t1
  EXPECT_EQ(1, adderCount);
  EXPECT_EQ(0, fn2Count);
  // t4: adder fires, schedules fn2
  delay(4); // t5
  EXPECT_EQ(2, adderCount);
  EXPECT_EQ(0, fn2Count);
  // t6: fn2 fires
  delay(2); // t7
  EXPECT_EQ(2, adderCount);
  EXPECT_EQ(1, fn2Count);
  // t8: adder fires
  // t9: fn2 fires
  delay(3); // t10
  EXPECT_EQ(3, adderCount);
  EXPECT_EQ(2, fn2Count);
  EXPECT_TRUE(fs.cancelFunction("fn2"));
  EXPECT_TRUE(fs.cancelFunction("adder"));
  delay(5); // t10
  EXPECT_EQ(3, adderCount);
  EXPECT_EQ(2, fn2Count);

  EXPECT_EQ(8, total);
  EXPECT_EQ(3, selfCancelCount);
}

TEST(FunctionScheduler, AddCancelInitialDelayStress) {
  FunctionScheduler fs;
  folly::Baton<> startTest;
  fs.addFunctionOnce([&] { startTest.wait(); }, "entryBarrier");
  fs.start();

  auto seed = folly::randomNumberSeed();
  LOG(INFO) << "seed: " << seed;
  std::mt19937 rng(seed);
  auto numFunctions = folly::Random::rand32(1, 1000, rng);
  std::vector<int64_t> indices(numFunctions);
  std::iota(indices.begin(), indices.end(), 1);

  std::vector<bool> expectedToRun(numFunctions + 1);
  std::vector<int64_t> indicesRan;
  indicesRan.reserve(numFunctions);
  auto addFunction = [&](int64_t index, bool removeBeforeAdding) {
    std::string name = fmt::format("f{}", index);
    if (removeBeforeAdding) {
      EXPECT_TRUE(fs.cancelFunction(name));
    }
    fs.addFunctionOnce(
        [&, index = index] { indicesRan.push_back(index); },
        name,
        std::chrono::milliseconds(index * 50));
    expectedToRun[index] = true;
  };

  std::shuffle(indices.begin(), indices.end(), rng);
  for (int idx : indices) {
    addFunction(idx, false);
  }
  std::shuffle(indices.begin(), indices.end(), rng);
  for (int idx : indices) {
    if (folly::Random::oneIn(2, rng)) {
      addFunction(idx, true);
    }
    if (folly::Random::oneIn(7, rng)) {
      EXPECT_TRUE(fs.cancelFunction(fmt::format("f{}", idx)));
      expectedToRun[idx] = false;
    }
  }
  folly::Baton<> finishTest;
  fs.addFunctionOnce(
      [&] { finishTest.post(); },
      "exitBarrier",
      std::chrono::milliseconds(numFunctions * 60));
  startTest.post();
  finishTest.wait();
  fs.shutdown();

  std::sort(indicesRan.begin(), indicesRan.end());
  std::vector<int64_t> expectedIndices;
  for (size_t index = 1; index < expectedToRun.size(); ++index) {
    if (expectedToRun[index]) {
      expectedIndices.push_back(static_cast<int64_t>(index));
    }
  }
  EXPECT_EQ(expectedIndices, indicesRan);
}

TEST(FunctionScheduler, AddMultiple) {
  atomic<int> total{0};
  FunctionScheduler fs;
  fs.addFunction([&] { total += 2; }, testInterval(2), "add2");
  fs.addFunction([&] { total += 3; }, testInterval(3), "add3");
  EXPECT_THROW(
      fs.addFunction([&] { total += 2; }, testInterval(2), "add2"),
      std::invalid_argument); // function name already exists

  fs.start();
  delay(1);
  EXPECT_EQ(5, total);
  delay(4);
  EXPECT_EQ(12, total);
  EXPECT_TRUE(fs.cancelFunction("add2"));
  delay(2);
  EXPECT_EQ(15, total);
  fs.shutdown();
  delay(3);
  EXPECT_EQ(15, total);
  fs.shutdown();
}

TEST(FunctionScheduler, AddAfterStart) {
  atomic<int> total{0};
  FunctionScheduler fs;
  fs.addFunction([&] { total += 2; }, testInterval(2), "add2");
  fs.addFunction([&] { total += 3; }, testInterval(2), "add3");
  fs.start();
  delay(3);
  EXPECT_EQ(10, total);
  fs.addFunction([&] { total += 2; }, testInterval(3), "add22");
  delay(2);
  EXPECT_EQ(17, total);
}

TEST(FunctionScheduler, ShutdownStart) {
  atomic<int> total{0};
  FunctionScheduler fs;
  fs.addFunction([&] { total += 2; }, testInterval(2), "add2");
  fs.start();
  delay(1);
  fs.shutdown();
  fs.start();
  delay(1);
  EXPECT_EQ(4, total);
  EXPECT_FALSE(fs.cancelFunction("add3")); // non existing
  delay(2);
  EXPECT_EQ(6, total);
}

TEST(FunctionScheduler, ResetFunc) {
  atomic<int> total{0};
  FunctionScheduler fs;
  fs.addFunction([&] { total += 2; }, testInterval(3), "add2");
  fs.addFunction([&] { total += 3; }, testInterval(3), "add3");
  fs.start();
  delay(1);
  EXPECT_EQ(5, total);
  EXPECT_FALSE(fs.resetFunctionTimer("NON_EXISTING"));
  EXPECT_TRUE(fs.resetFunctionTimer("add2"));
  delay(1);
  // t2: after the reset, add2 should have been invoked immediately
  EXPECT_EQ(7, total);
  delay(1.5);
  // t3.5: add3 should have been invoked. add2 should not
  EXPECT_EQ(10, total);
  delay(1);
  // t4.5: add2 should have been invoked once more (it was reset at t1)
  EXPECT_EQ(12, total);
}

TEST(FunctionScheduler, ResetFunc2) {
  atomic<int> total{0};
  FunctionScheduler fs;
  fs.addFunctionOnce([&] { total += 2; }, "add2", testInterval(1));
  fs.addFunctionOnce([&] { total += 3; }, "add3", testInterval(1));
  fs.start();
  delay(2);
  fs.addFunctionOnce([&] { total += 3; }, "add4", testInterval(2));
  EXPECT_TRUE(fs.resetFunctionTimer("add4"));
  fs.addFunctionOnce([&] { total += 3; }, "add6", testInterval(2));
  delay(1);
  EXPECT_TRUE(fs.resetFunctionTimer("add4"));
  delay(3);
  EXPECT_FALSE(fs.resetFunctionTimer("add3"));
  fs.addFunctionOnce([&] { total += 3; }, "add4", testInterval(1));
}

TEST(FunctionScheduler, ResetFuncWhileRunning) {
  struct State {
    std::barrier<> barrier_a{2};
    std::barrier<> barrier_b{2};
    std::barrier<> barrier_c{2};
    std::barrier<> barrier_d{2};
    atomic<bool> set = false;
    atomic<size_t> count = 0;
  };

  State state; // held by ref
  auto mv = std::make_shared<size_t>(); // gets moved

  FunctionScheduler fs;
  fs.addFunction(
      [&, mv /* ref + shared_ptr fit in in-situ storage */] {
        if (!state.set) { // first invocation
          state.barrier_a.arrive_and_wait();
          // ensure that resetFunctionTimer is called in this critical section
          state.barrier_b.arrive_and_wait();
          ++state.count;
          EXPECT_TRUE(bool(mv)) << "bug repro: mv was moved-out";
          state.barrier_c.arrive_and_wait();
          // main thread checks count here
          state.barrier_d.arrive_and_wait();
        } else { // subsequent invocations
          ++state.count;
        }
      },
      testInterval(3),
      "nada");
  fs.start();

  state.barrier_a.arrive_and_wait();
  state.set = true;
  fs.resetFunctionTimer("nada");
  EXPECT_EQ(0, state.count) << "sanity check";
  state.barrier_b.arrive_and_wait();
  // fn thread increments count and checks mv here
  state.barrier_c.arrive_and_wait();
  EXPECT_EQ(1, state.count) << "sanity check";
  state.barrier_d.arrive_and_wait();
  delay(1);
  EXPECT_EQ(2, state.count) << "sanity check";
}

TEST(FunctionScheduler, AddInvalid) {
  atomic<int> total{0};
  FunctionScheduler fs;
  // interval may not be negative
  EXPECT_THROW(
      fs.addFunction([&] { total += 2; }, testInterval(-1), "add2"),
      std::invalid_argument);

  EXPECT_FALSE(fs.cancelFunction("addNoFunc"));
}

TEST(FunctionScheduler, NoFunctions) {
  FunctionScheduler fs;
  EXPECT_TRUE(fs.start());
  fs.shutdown();
  FunctionScheduler fs2;
  fs2.shutdown();
}

TEST(FunctionScheduler, AddWhileRunning) {
  atomic<int> total{0};
  FunctionScheduler fs;
  fs.start();
  delay(1);
  fs.addFunction([&] { total += 2; }, testInterval(2), "add2");
  // The function should be invoked nearly immediately when we add it
  // and the FunctionScheduler is already running
  delay(0.5);
  auto t = total.load();
  EXPECT_EQ(2, t);
  delay(2);
  t = total.load();
  EXPECT_EQ(4, t);
}

TEST(FunctionScheduler, NoShutdown) {
  atomic<int> total{0};
  {
    FunctionScheduler fs;
    fs.addFunction([&] { total += 2; }, testInterval(1), "add2");
    fs.start();
    delay(0.5);
    EXPECT_EQ(2, total);
  }
  // Destroyed the FunctionScheduler without calling shutdown.
  // Everything should have been cleaned up, and the function will no longer
  // get called.
  delay(2);
  EXPECT_EQ(2, total);
}

TEST(FunctionScheduler, StartDelay) {
  atomic<int> total{0};
  FunctionScheduler fs;
  fs.addFunction([&] { total += 2; }, testInterval(2), "add2", testInterval(2));
  fs.addFunction([&] { total += 3; }, testInterval(3), "add3", testInterval(2));
  EXPECT_THROW(
      fs.addFunction(
          [&] { total += 2; }, testInterval(3), "addX", testInterval(-1)),
      std::invalid_argument);
  fs.start();
  delay(1); // t1
  EXPECT_EQ(0, total);
  // t2 : add2 total=2
  // t2 : add3 total=5
  delay(2); // t3
  EXPECT_EQ(5, total);
  // t4 : add2: total=7
  // t5 : add3: total=10
  // t6 : add2: total=12
  delay(4); // t7
  EXPECT_EQ(12, total);
  fs.cancelFunction("add2");
  // t8 : add3: total=15
  delay(2); // t9
  EXPECT_EQ(15, total);
  fs.shutdown();
  delay(3);
  EXPECT_EQ(15, total);
  fs.shutdown();
}

TEST(FunctionScheduler, NoSteadyCatchup) {
  std::atomic<int> ticks(0);
  FunctionScheduler fs;
  // fs.setSteady(false); is the default
  fs.addFunction(
      [&ticks] {
        if (++ticks == 2) {
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
      },
      milliseconds(5));
  fs.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // no steady catch up means we'd tick once for 200ms, then remaining
  // 300ms / 5 = 60 times
  EXPECT_LE(ticks.load(), 61);
}

TEST(FunctionScheduler, SteadyCatchup) {
  std::atomic<int> ticks(0);
  FunctionScheduler fs;
  fs.setSteady(true);
  fs.addFunction(
      [&ticks] {
        if (++ticks == 2) {
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
      },
      milliseconds(5));
  fs.start();

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // tick every 5ms. Despite tick == 2 is slow, later ticks should be fast
  // enough to catch back up to schedule
  EXPECT_NEAR(100, ticks.load(), 10);
}

TEST(FunctionScheduler, UniformDistribution) {
  atomic<int> total{0};
  const int kTicks = 2;
  std::chrono::milliseconds minInterval =
      testInterval(kTicks) - (timeFactor / 5);
  std::chrono::milliseconds maxInterval =
      testInterval(kTicks) + (timeFactor / 5);
  FunctionScheduler fs;
  fs.addFunctionUniformDistribution(
      [&] { total += 2; },
      minInterval,
      maxInterval,
      "UniformDistribution",
      std::chrono::milliseconds(0));
  fs.start();
  delay(1);
  EXPECT_EQ(2, total);
  delay(kTicks);
  EXPECT_EQ(4, total);
  delay(kTicks);
  EXPECT_EQ(6, total);
  fs.shutdown();
  delay(2);
  EXPECT_EQ(6, total);
}

TEST(FunctionScheduler, ConsistentDelay) {
  std::atomic<int> ticks(0);
  FunctionScheduler fs;

  std::atomic<long long> epoch(0);
  epoch = duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
              .count();

  // We should have runs at t = 0, 600, 800, 1200, or 4 total.
  // If at const interval, it would be t = 0, 600, 1000, or 3 total.
  fs.addFunctionConsistentDelay(
      [&ticks, &epoch] {
        auto now =
            duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
                .count();
        int t = ++ticks;
        if (t != 2) {
          // Sensitive to delays above 100ms.
          EXPECT_NEAR((now - epoch) - (t - 1) * 400, 0, 100);
        }
        if (t == 1) {
          /* sleep override */
          std::this_thread::sleep_for(std::chrono::milliseconds(600));
        }
      },
      milliseconds(400),
      "ConsistentDelay");

  fs.start();

  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(1300));
  EXPECT_EQ(ticks.load(), 4);
}

TEST(FunctionScheduler, ExponentialBackoff) {
  atomic<int> total{0};
  atomic<int> expectedInterval{0};
  atomic<int> nextInterval{2};
  FunctionScheduler fs;
  fs.addFunctionGenericDistribution(
      [&] { total += 2; },
      [&expectedInterval, &nextInterval]() mutable {
        auto interval = nextInterval.load();
        expectedInterval = interval;
        nextInterval = interval * interval;
        return testInterval(interval);
      },
      "ExponentialBackoff",
      "2^n * 100ms",
      std::chrono::milliseconds(0));
  fs.start();
  delay(1);
  EXPECT_EQ(2, total);
  delay(expectedInterval);
  EXPECT_EQ(4, total);
  delay(expectedInterval);
  EXPECT_EQ(6, total);
  fs.shutdown();
  delay(2);
  EXPECT_EQ(6, total);
}

TEST(FunctionScheduler, GammaIntervalDistribution) {
  atomic<int> total{0};
  atomic<int> expectedInterval{0};
  FunctionScheduler fs;
  std::default_random_engine generator(folly::Random::rand32());
  // The alpha and beta arguments are selected, somewhat randomly, to be 2.0.
  // These values do not matter much in this test, as we are not testing the
  // std::gamma_distribution itself...
  std::gamma_distribution<double> gamma(2.0, 2.0);
  fs.addFunctionGenericDistribution(
      [&] { total += 2; },
      [&expectedInterval, generator, gamma]() mutable {
        expectedInterval =
            getTicksWithinRange(static_cast<int>(gamma(generator)), 2, 10);
        return testInterval(expectedInterval);
      },
      "GammaDistribution",
      "gamma(2.0,2.0)*100ms",
      std::chrono::milliseconds(0));
  fs.start();
  delay(1);
  EXPECT_EQ(2, total);
  delay(expectedInterval);
  EXPECT_EQ(4, total);
  delay(expectedInterval);
  EXPECT_EQ(6, total);
  fs.shutdown();
  delay(2);
  EXPECT_EQ(6, total);
}

TEST(FunctionScheduler, PoissonDistribution) {
  auto interval = std::chrono::hours(24 * 365 * 10);
  atomic<int> total{0};
  FunctionScheduler fs;
  fs.addFunction(
      [&] { total += 2; },
      interval,
      folly::FunctionScheduler::LatencyDistribution(true, interval),
      "PoissonDistribution",
      std::chrono::milliseconds(0));
  fs.start();
  delay(1);
  EXPECT_EQ(2, total);
}

TEST(FunctionScheduler, AddWithRunOnce) {
  atomic<int> total{0};
  FunctionScheduler fs;
  fs.addFunctionOnce([&] { total += 2; }, "add2");
  fs.start();
  delay(1);
  EXPECT_EQ(2, total);
  delay(2);
  EXPECT_EQ(2, total);

  fs.addFunctionOnce([&] { total += 2; }, "add2");
  delay(1);
  EXPECT_EQ(4, total);
  delay(2);
  EXPECT_EQ(4, total);

  fs.shutdown();
}

TEST(FunctionScheduler, cancelFunctionAndWait) {
  atomic<int> total{0};
  FunctionScheduler fs;
  fs.addFunction(
      [&] {
        delay(5);
        total += 2;
      },
      testInterval(100),
      "add2");

  fs.start();
  delay(1);
  EXPECT_EQ(0, total); // add2 is still sleeping

  EXPECT_TRUE(fs.cancelFunctionAndWait("add2"));
  EXPECT_EQ(2, total); // add2 should have completed

  EXPECT_FALSE(fs.cancelFunction("add2")); // add2 has been canceled
  fs.shutdown();
}

TEST(FunctionScheduler, cancelAllFunctionsAndWait) {
  atomic<int> total{0};
  FunctionScheduler fs;

  fs.addFunction(
      [&] {
        delay(5);
        total += 2;
      },
      testInterval(100),
      "add2");

  fs.start();
  delay(1);
  EXPECT_EQ(0, total); // add2 is still sleeping

  fs.cancelAllFunctionsAndWait();
  EXPECT_EQ(2, total);

  EXPECT_FALSE(fs.cancelFunction("add2")); // add2 has been canceled
  fs.shutdown();
}

TEST(FunctionScheduler, CancelAndWaitOnRunningFunc) {
  folly::Baton<> baton;
  std::thread th([&baton]() {
    FunctionScheduler fs;
    fs.addFunction([] { delay(10); }, testInterval(2), "func");
    fs.start();
    delay(1);
    EXPECT_TRUE(fs.cancelFunctionAndWait("func"));
    baton.post();
  });

  ASSERT_TRUE(baton.try_wait_for(testInterval(15)));
  th.join();
}

TEST(FunctionScheduler, CancelAllAndWaitWithRunningFunc) {
  folly::Baton<> baton;
  std::thread th([&baton]() {
    FunctionScheduler fs;
    fs.addFunction([] { delay(10); }, testInterval(2), "func");
    fs.start();
    delay(1);
    fs.cancelAllFunctionsAndWait();
    baton.post();
  });

  ASSERT_TRUE(baton.try_wait_for(testInterval(15)));
  th.join();
}

TEST(FunctionScheduler, CancelAllAndWaitWithOneRunningAndOneWaiting) {
  folly::Baton<> baton;
  std::thread th([&baton]() {
    std::atomic<int> nExecuted(0);
    FunctionScheduler fs;
    fs.addFunction(
        [&nExecuted] {
          nExecuted++;
          delay(10);
        },
        testInterval(2),
        "func0");
    fs.addFunction(
        [&nExecuted] {
          nExecuted++;
          delay(10);
        },
        testInterval(2),
        "func1",
        testInterval(5));
    fs.start();
    delay(1);
    fs.cancelAllFunctionsAndWait();
    EXPECT_EQ(nExecuted, 1);
    baton.post();
  });

  ASSERT_TRUE(baton.try_wait_for(testInterval(15)));
  th.join();
}

TEST(FunctionScheduler, ConcurrentCancelFunctionAndWait) {
  FunctionScheduler fs;
  fs.addFunction([] { delay(10); }, testInterval(2), "func");

  fs.start();
  delay(1);
  std::thread th1([&fs] { EXPECT_TRUE(fs.cancelFunctionAndWait("func")); });
  delay(1);
  std::thread th2([&fs] { EXPECT_FALSE(fs.cancelFunctionAndWait("func")); });
  th1.join();
  th2.join();
}
