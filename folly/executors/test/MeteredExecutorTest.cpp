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

#include <list>
#include <stdexcept>

#include <folly/Benchmark.h>
#include <folly/Synchronized.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Task.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/MeteredExecutor.h>
#include <folly/init/Init.h>
#include <folly/portability/GTest.h>
#include <folly/synchronization/Baton.h>
#include <folly/synchronization/Latch.h>
#include <folly/synchronization/LifoSem.h>
#include <folly/system/HardwareConcurrency.h>
#include <folly/test/DeterministicSchedule.h>

using namespace folly;

TEST(MeteredExecutorOptionsTest, ZeroMaxInQueueThrows) {
  MeteredExecutor::Options options;
  options.maxInQueue = 0;
  EXPECT_THROW(
      MeteredExecutor(std::make_unique<CPUThreadPoolExecutor>(1), options),
      std::invalid_argument);
}

TEST(MeteredExecutorOptionsTest, TooLargeMaxInQueueThrows) {
  MeteredExecutor::Options options;
  options.maxInQueue = uint32_t(1) << 31;
  EXPECT_THROW(
      MeteredExecutor(std::make_unique<CPUThreadPoolExecutor>(1), options),
      std::invalid_argument);
}

class MeteredExecutorTest : public testing::Test {
 protected:
  template <template <typename> class Atom = std::atomic>
  void createAdapter(
      int numLevels,
      std::unique_ptr<Executor> exc =
          std::make_unique<CPUThreadPoolExecutor>(1)) {
    executors_.resize(numLevels + 1);
    executors_[0] = exc.get();
    for (int i = 0; i < numLevels; i++) {
      auto mlsa =
          std::make_unique<detail::MeteredExecutorImpl<Atom>>(std::move(exc));
      exc = std::move(mlsa);
      executors_[i + 1] = exc.get();
    }
    owning_ = std::move(exc);
  }

  void add(Func f, uint8_t level = 0) { executors_[level]->add(std::move(f)); }

  void join() {
    folly::Baton baton;
    executors_.back()->add([&] { baton.post(); });
    baton.wait();
  }

  Executor::KeepAlive<> getKeepAlive(int level) { return executors_[level]; }

 protected:
  std::shared_ptr<void> owning_;
  std::vector<Executor*> executors_;
};

TEST_F(MeteredExecutorTest, SingleLevel) {
  createAdapter(1);

  folly::Baton baton;
  add([&] { baton.wait(); });

  int32_t v = 0;
  add([&] { EXPECT_EQ(0, v++); });
  // first lopri task executes in the scheduling order with hipri
  // tasks, but all subsequent lopri tasks yield.
  add([&] { EXPECT_EQ(1, v++); }, 1);
  add([&] { EXPECT_EQ(4, v++); }, 1);
  add([&] { EXPECT_EQ(5, v++); }, 1);
  add([&] { EXPECT_EQ(6, v++); }, 1);
  add([&] { EXPECT_EQ(2, v++); });
  add([&] { EXPECT_EQ(3, v++); });
  baton.post();

  join();
  EXPECT_EQ(v, 7);
}

TEST_F(MeteredExecutorTest, TwoLevels) {
  int32_t v = 0;

  createAdapter(2);

  folly::Baton baton;
  add([&] { baton.wait(); });
  add([&] { EXPECT_EQ(0, v++); });
  add([&] { EXPECT_EQ(1, v++); }, 1);
  add([&] { EXPECT_EQ(4, v++); }, 1);
  add([&] { EXPECT_EQ(5, v++); }, 1);
  add([&] { EXPECT_EQ(6, v++); }, 1);
  add([&] { EXPECT_EQ(7, v++); }, 1);
  add([&] { EXPECT_EQ(8, v++); }, 2);
  add([&] { EXPECT_EQ(13, v++); }, 2);
  add([&] { EXPECT_EQ(14, v++); }, 2);
  add([&] { EXPECT_EQ(15, v++); }, 2);
  add([&] { EXPECT_EQ(16, v++); }, 2);
  add([&] { EXPECT_EQ(9, v++); }, 1);
  add([&] { EXPECT_EQ(10, v++); }, 1);
  add([&] { EXPECT_EQ(11, v++); }, 1);
  add([&] { EXPECT_EQ(12, v++); }, 1);
  add([&] { EXPECT_EQ(2, v++); });
  add([&] { EXPECT_EQ(3, v++); });
  baton.post();

  join();
  EXPECT_EQ(v, 17);
}

TEST_F(MeteredExecutorTest, PreemptTest) {
  int32_t v = 0;

  createAdapter(1);

  folly::Baton baton1, baton2, baton3;
  add([&] { baton1.wait(); });

  add([&] { EXPECT_EQ(0, v++); });
  // first lopri task executes in the scheduling order with hipri
  // tasks, but all subsequent lopri tasks yield.
  add([&] { EXPECT_EQ(1, v++); }, 1);
  add([&] { EXPECT_EQ(4, v++); }, 1);
  add([&] { EXPECT_EQ(5, v++); }, 1);
  add([&] { EXPECT_EQ(2, v++); });
  add([&] { EXPECT_EQ(3, v++); });
  add(
      [&] {
        EXPECT_EQ(6, v++);
        baton2.post();
        baton3.wait();
      },
      1);

  // These P1 tasks should be run AFTER the 3 P0 tasks are added below. We
  // should expect the P0 tasks to preempt these.
  add([&] { EXPECT_EQ(7, v++); }, 1);
  add([&] { EXPECT_EQ(11, v++); }, 1);
  add([&] { EXPECT_EQ(12, v++); }, 1);
  add([&] { EXPECT_EQ(13, v++); }, 1);

  // Kick off the tasks, but we'll halt part-way through the P1 tasks to add
  // some P0 tasks into the mix.
  baton1.post();
  baton2.wait();

  // Throw in the high pris.
  add([&] { EXPECT_EQ(8, v++); });
  add([&] { EXPECT_EQ(9, v++); });
  add([&] { EXPECT_EQ(10, v++); });

  baton3.post();

  join();
  EXPECT_EQ(v, 14);
}

TEST_F(MeteredExecutorTest, TwoLevelsWithKeepAlives) {
  auto hipri_exec = std::make_unique<CPUThreadPoolExecutor>(1);
  auto hipri_ka = getKeepAliveToken(hipri_exec.get());
  auto mipri_exec = std::make_unique<MeteredExecutor>(hipri_ka);
  auto mipri_ka = getKeepAliveToken(mipri_exec.get());
  auto lopri_exec = std::make_unique<MeteredExecutor>(mipri_ka);
  executors_ = {hipri_exec.get(), mipri_exec.get(), lopri_exec.get()};

  int32_t v = 0;
  folly::Baton baton;
  add([&] { baton.wait(); });
  add([&] { EXPECT_EQ(0, v++); });
  add([&] { EXPECT_EQ(1, v++); }, 1);
  add([&] { EXPECT_EQ(4, v++); }, 1);
  add([&] { EXPECT_EQ(5, v++); }, 1);
  add([&] { EXPECT_EQ(6, v++); }, 1);
  add([&] { EXPECT_EQ(7, v++); }, 1);
  add([&] { EXPECT_EQ(8, v++); }, 2);
  add([&] { EXPECT_EQ(13, v++); }, 2);
  add([&] { EXPECT_EQ(14, v++); }, 2);
  add([&] { EXPECT_EQ(15, v++); }, 2);
  add([&] { EXPECT_EQ(16, v++); }, 2);
  add([&] { EXPECT_EQ(9, v++); }, 1);
  add([&] { EXPECT_EQ(10, v++); }, 1);
  add([&] { EXPECT_EQ(11, v++); }, 1);
  add([&] { EXPECT_EQ(12, v++); }, 1);
  add([&] { EXPECT_EQ(2, v++); });
  add([&] { EXPECT_EQ(3, v++); });
  baton.post();

  lopri_exec.reset();
  EXPECT_EQ(v, 17);
}

TEST_F(MeteredExecutorTest, RequestContext) {
  createAdapter(3);

  folly::Baton baton;
  add([&] { baton.wait(); });

  auto addAndCheckRCTX = [this](int8_t pri = 0) {
    RequestContextScopeGuard g;
    auto f = [rctx = RequestContext::saveContext()]() mutable {
      EXPECT_EQ(rctx.get(), RequestContext::get());

      // Verify we do not have dangling reference to finished requests.
      static std::shared_ptr<folly::RequestContext> lastFinished;
      EXPECT_TRUE(!lastFinished || lastFinished.unique());
      lastFinished = std::move(rctx);
    };
    add(std::move(f), pri);
  };

  addAndCheckRCTX();
  addAndCheckRCTX(3);
  addAndCheckRCTX(3);
  addAndCheckRCTX(3);
  addAndCheckRCTX(1);
  addAndCheckRCTX(1);
  addAndCheckRCTX(1);
  addAndCheckRCTX(2);
  addAndCheckRCTX(2);
  addAndCheckRCTX(2);
  baton.post();
  join();
}

TEST_F(MeteredExecutorTest, ResetJoins) {
  createAdapter(2);

  folly::Baton baton;
  add([&] { baton.wait(); });

  int v = 0;
  add([&] { ++v; });
  add([&] { ++v; }, 2);
  add([&] { ++v; }, 2);
  add([&] { ++v; }, 1);
  add([&] { ++v; }, 1);

  baton.post();
  owning_.reset();
  EXPECT_EQ(v, 5);
}

TEST_F(MeteredExecutorTest, ConcurrentShutdown) {
  // ensure no data races on shutdown when executor has 2 threads
  createAdapter(2, std::make_unique<CPUThreadPoolExecutor>(2));
}

TEST_F(MeteredExecutorTest, CostOfMeteredExecutors) {
  // This test is to demonstrate how many tasks are scheduled
  // on the primarty executor when it is wrapped by MeteredExecutors
  class MyExecutor : public folly::Executor {
   public:
    int count{0};
    bool driveWhenAdded{false};
    std::list<folly::Func> queue;
    void add(folly::Func f) override {
      ++count;
      queue.push_back(std::move(f));
      if (driveWhenAdded) {
        drive();
      }
    }
    void drive() {
      while (!queue.empty()) {
        auto ff = std::move(queue.front());
        queue.pop_front();
        ff();
      }
    }
  };

  auto exc = std::make_unique<MyExecutor>();
  auto drive = [exc = exc.get()] { exc->drive(); };
  auto getCount = [exc = exc.get()] { return std::exchange(exc->count, 0); };
  auto driveOnAdd = [exc = exc.get()] { exc->driveWhenAdded = true; };
  createAdapter(3, std::move(exc));

  // Whether or not the queues are empty, we schedule exactly one task on the
  // primary executor for each schedule task, regardless of the chain depth.
  add([&] {}, 0);
  drive();
  EXPECT_EQ(1, getCount());

  add([&] {}, 1);
  drive();
  EXPECT_EQ(1, getCount());

  add([&] {}, 2);
  drive();
  EXPECT_EQ(1, getCount());

  add([&] {}, 3);
  drive();
  EXPECT_EQ(1, getCount());

  add([&] {}, 3);
  drive();
  EXPECT_EQ(1, getCount());

  add([&] {}, 3);
  add([&] {}, 3);
  add([&] {}, 3);
  drive();
  EXPECT_EQ(3, getCount());

  // To allow shutting down properly
  driveOnAdd();
}

TEST_F(MeteredExecutorTest, ExceptionHandling) {
  createAdapter(2);

  folly::Baton baton;
  add([&] {
    baton.wait();
    throw std::runtime_error("dummy");
  });

  bool thrown = false;
  add(
      [&] {
        thrown = true;
        throw std::runtime_error("dummy");
      },
      1);
  baton.post();
  join();
  EXPECT_EQ(true, thrown);
}

namespace {

coro::Task<bool> co_isOnCPUExc() {
  auto executor = co_await coro::co_current_executor;
  auto cpuexec = dynamic_cast<CPUThreadPoolExecutor*>(executor);
  co_return cpuexec != nullptr;
}

template <typename T>
coro::Task<bool> co_run(Executor::KeepAlive<> ka, coro::Task<T> f) {
  auto cpuexec = dynamic_cast<CPUThreadPoolExecutor*>(ka.get());
  EXPECT_TRUE(cpuexec != nullptr);
  co_return co_await co_withExecutor(cpuexec, std::move(f));
}

} // namespace

TEST_F(MeteredExecutorTest, UnderlyingExecutor) {
  createAdapter(1);
  EXPECT_FALSE(
      coro::blockingWait(co_withExecutor(getKeepAlive(1), co_isOnCPUExc())));
  EXPECT_TRUE(
      coro::blockingWait(co_withExecutor(
          getKeepAlive(0), co_run(getKeepAlive(0), co_isOnCPUExc()))));
}

TEST_F(MeteredExecutorTest, PauseResume) {
  createAdapter(1, std::make_unique<ManualExecutor>());
  ManualExecutor* manual = dynamic_cast<ManualExecutor*>(getKeepAlive(0).get());
  MeteredExecutor* metered =
      dynamic_cast<MeteredExecutor*>(getKeepAlive(1).get());

  size_t executed = 0;
  add([&] { executed++; }, 1);
  add([&] { executed++; }, 1);
  manual->run();
  ASSERT_EQ(1, executed);
  ASSERT_EQ(1, metered->pendingTasks());

  ASSERT_TRUE(metered->pause());
  ASSERT_FALSE(metered->pause()); // pausing a paused executor
  add([&] { executed++; }, 1);
  manual->drain();
  ASSERT_EQ(1, executed);
  ASSERT_EQ(2, metered->pendingTasks());

  ASSERT_TRUE(metered->resume());
  ASSERT_FALSE(metered->resume()); // resuming a non-paused executor
  manual->drain();
  ASSERT_EQ(3, executed);
  ASSERT_EQ(0, metered->pendingTasks());
}

TEST_F(MeteredExecutorTest, PauseResumeStress) {
  using DSched = folly::test::DeterministicSchedule;
  DSched sched(DSched::uniform(0));

  class DeterministicExecutor : public folly::Executor {
   public:
    explicit DeterministicExecutor(int threads) {
      for (int i = 0; i < threads; i++) {
        threads_.push_back(DSched::thread([this]() { loop(); }));
      }
    }
    void join() {
      sem_.shutdown();
      DSched::joinAll(threads_);
      threads_.clear();
    }
    void add(folly::Func f) override {
      tasks_.lock()->push(std::move(f));
      sem_.post();
    }

   private:
    void loop() {
      while (true) {
        try {
          sem_.wait();
          auto fn = tasks_.withLock([](auto&& tasks) {
            if (tasks.empty()) {
              return folly::Func{};
            }
            auto ret = std::move(tasks.front());
            tasks.pop();
            return ret;
          });
          if (fn) {
            fn();
          }
        } catch (const ShutdownSemError&) {
          break;
        } catch (const std::exception& e) {
          LOG(ERROR) << "Failed: " << e.what();
        }
      }
    }

    LifoSemImpl<test::DeterministicAtomic> sem_;
    folly::Synchronized<std::queue<folly::Func>, test::DeterministicMutex>
        tasks_;
    std::vector<std::thread> threads_;
  };

  createAdapter<test::DeterministicAtomic>(
      1, std::make_unique<DeterministicExecutor>(5));

  auto exec = getKeepAlive(1);
  auto metered =
      dynamic_cast<detail::MeteredExecutorImpl<test::DeterministicAtomic>*>(
          exec.get());
  auto dexec = dynamic_cast<DeterministicExecutor*>(getKeepAlive(0).get());

  const auto numElems = 150;
  std::atomic<uint64_t> executed{0};
  std::atomic<uint64_t> active{0};
  for (int pass = 0; pass < 10; pass++) {
    executed = 0;
    active = 0;
    const auto numProducers = 2;
    std::vector<std::thread> producers;
    for (int p = 0; p < numProducers; p++) {
      producers.push_back(DSched::thread([&]() {
        for (int i = 0; i < numElems; i++) {
          exec->add([&]() {
            executed++;
            bool paused = false;
            if (active.fetch_add(1) == 3) {
              metered->pause();
              paused = true;
            }
            if (paused) {
              active.fetch_sub(1);
              metered->resume();
            }
          });
        }
      }));
    }
    DSched::joinAll(producers);
    folly::Baton<true, test::DeterministicAtomic> b;
    exec->add([&]() { b.post(); });
    b.wait();
    EXPECT_EQ(numElems * numProducers, executed);
  }

  dexec->join();
}

TEST_F(MeteredExecutorTest, RealThreadConcurrentStress) {
  // Exercises modifyState()'s CAS with genuine OS-thread concurrency (as
  // opposed to PauseResumeStress, which uses DeterministicSchedule's mocked
  // atomics to explore interleavings deterministically but does not exercise
  // real hardware memory ordering). This is meant to catch any regression
  // from relaxing the CAS from seq_cst to acq_rel: producers hammer add()
  // concurrently with pause()/resume() from independent threads while a pool
  // of real worker threads drains the wrapped executor.
  createAdapter(1, std::make_unique<CPUThreadPoolExecutor>(4));
  MeteredExecutor* metered =
      dynamic_cast<MeteredExecutor*>(getKeepAlive(1).get());

  constexpr int kNumProducers = 8;
  constexpr int kTasksPerProducer = 2000;
  std::atomic<uint64_t> executed{0};

  std::vector<std::thread> producers;
  for (int p = 0; p < kNumProducers; ++p) {
    producers.emplace_back([&] {
      for (int i = 0; i < kTasksPerProducer; ++i) {
        add([&] { executed.fetch_add(1, std::memory_order_relaxed); }, 1);
      }
    });
  }

  std::atomic<bool> stopPauser{false};
  std::thread pauser([&] {
    while (!stopPauser.load(std::memory_order_relaxed)) {
      metered->pause();
      metered->resume();
    }
  });

  for (auto& t : producers) {
    t.join();
  }
  stopPauser.store(true, std::memory_order_relaxed);
  pauser.join();

  // Ensure the executor isn't left paused, then drain everything.
  metered->resume();
  folly::Baton<> baton;
  add([&] { baton.post(); }, 1);
  baton.wait();

  EXPECT_EQ(
      static_cast<uint64_t>(kNumProducers) * kTasksPerProducer,
      executed.load());
  EXPECT_EQ(0, metered->pendingTasks());
}

namespace {

// Simulate saturation regime (queue almost always non-empty) on a
// high-concurrency executor to measure to what extent MeteredExecutor
// artificially limits concurrency in a situation where it is the only producer
// in the executor (so it should ideally not impose any overhead).
void benchmarkSaturation(
    size_t meteredExecutorChainDepth, uint32_t maxInQueue, size_t iters) {
  const size_t numThreads = folly::available_concurrency();

  BenchmarkSuspender suspender;

  CPUThreadPoolExecutor producer(
      std::make_pair(numThreads, numThreads),
      CPUThreadPoolExecutor::makeThrottledLifoSemQueue());
  CPUThreadPoolExecutor consumerParent(
      std::make_pair(numThreads, numThreads),
      CPUThreadPoolExecutor::makeThrottledLifoSemQueue());

  Executor* consumer = &consumerParent;
  std::unique_ptr<MeteredExecutor> lastMeteredExecutor;
  for (size_t i = 0; i < meteredExecutorChainDepth; ++i) {
    MeteredExecutor::Options options;
    options.maxInQueue = maxInQueue;
    if (lastMeteredExecutor == nullptr) {
      lastMeteredExecutor = std::make_unique<MeteredExecutor>(
          &consumerParent, std::move(options));
    } else {
      lastMeteredExecutor = std::make_unique<MeteredExecutor>(
          std::move(lastMeteredExecutor), std::move(options));
    }
    consumer = lastMeteredExecutor.get();
  }

  suspender.dismiss();

  Latch done{static_cast<ptrdiff_t>(iters * numThreads)};

  for (size_t t = 0; t < numThreads; ++t) {
    producer.add([&] {
      for (size_t i = 0; i < iters; ++i) {
        consumer->add([&] { done.count_down(); });
      }
    });
  }

  done.wait();
  suspender.rehire(); // Do not measure destruction.
}

} // namespace

BENCHMARK(Saturation_no_MeteredExecutor, iters) {
  benchmarkSaturation(0, /* unused */ 0, iters);
}

BENCHMARK_DRAW_LINE();

BENCHMARK(Saturation_MeteredExecutor_chain_1_maxInQueue_1, iters) {
  benchmarkSaturation(1, 1, iters);
}
BENCHMARK(Saturation_MeteredExecutor_chain_1_maxInQueue_2, iters) {
  benchmarkSaturation(1, 2, iters);
}
BENCHMARK(Saturation_MeteredExecutor_chain_1_maxInQueue_4, iters) {
  benchmarkSaturation(1, 4, iters);
}

BENCHMARK_DRAW_LINE();

BENCHMARK(Saturation_MeteredExecutor_chain_2_maxInQueue_1, iters) {
  benchmarkSaturation(2, 1, iters);
}
BENCHMARK(Saturation_MeteredExecutor_chain_2_maxInQueue_2, iters) {
  benchmarkSaturation(2, 2, iters);
}
BENCHMARK(Saturation_MeteredExecutor_chain_2_maxInQueue_4, iters) {
  benchmarkSaturation(2, 4, iters);
}

BENCHMARK_DRAW_LINE();

BENCHMARK(Saturation_MeteredExecutor_chain_3_maxInQueue_1, iters) {
  benchmarkSaturation(3, 1, iters);
}
BENCHMARK(Saturation_MeteredExecutor_chain_3_maxInQueue_2, iters) {
  benchmarkSaturation(3, 2, iters);
}
BENCHMARK(Saturation_MeteredExecutor_chain_3_maxInQueue_4, iters) {
  benchmarkSaturation(3, 4, iters);
}

int main(int argc, char** argv) {
  // Enable glog logging to stderr by default.
  FLAGS_logtostderr = true;
  ::testing::InitGoogleTest(&argc, argv);
  folly::Init init(&argc, &argv);
  folly::runBenchmarksOnFlag();
  return RUN_ALL_TESTS();
}
