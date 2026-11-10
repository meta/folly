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

#include <folly/executors/GlobalExecutor.h>

#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/IOExecutor.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/executors/VirtualExecutor.h>
#include <folly/portability/GTest.h>
#include <folly/synchronization/Baton.h>
#include <folly/synchronization/SaturatingSemaphore.h>
#include <folly/system/HardwareConcurrency.h>

using namespace folly;

TEST(GlobalExecutorTest, GlobalImmutableCPUExecutor) {
  folly::Baton<> b;

  auto count = 0;
  auto f = [&]() {
    count++;
    b.post();
  };

  // Don't explode, we should create the default global CPUExecutor lazily here.
  getGlobalCPUExecutor()->add(f);
  b.wait();
  EXPECT_EQ(1, count);
}

TEST(GlobalExecutorTest, GlobalImmutableIOExecutor) {
  folly::Baton<> b;

  auto count = 0;
  auto f = [&]() {
    count++;
    b.post();
  };

  // Don't explode, we should create the default global CPUExecutor lazily here.
  getGlobalIOExecutor()->add(f);
  b.wait();
  EXPECT_EQ(1, count);
}

TEST(GlobalExecutorTest, GlobalCPUExecutor) {
  class DummyExecutor : public folly::Executor {
   public:
    void add(Func fn) override {
      fn();
      count++;
    }
    int count{0};
  };

  // The default CPU executor is a thread pool
  auto f = [&]() {};

  // Don't explode, we should create the default global CPUExecutor lazily here.
  FOLLY_PUSH_WARNING
  FOLLY_GNU_DISABLE_WARNING("-Wdeprecated-declarations")
  getCPUExecutor()->add(f);

  {
    auto dummy = std::make_shared<DummyExecutor>();
    setCPUExecutor(dummy);
    getCPUExecutor()->add(f);
    // Make sure we were properly installed.
    EXPECT_EQ(1, dummy->count);
  }

  // Don't explode, we should restore the default global CPUExecutor because our
  // weak reference to dummy has expired
  getCPUExecutor()->add(f);
  FOLLY_POP_WARNING
}

TEST(GlobalExecutorTest, GlobalIOExecutor) {
  class DummyExecutor : public IOExecutor {
   public:
    void add(Func) override { count++; }
    folly::EventBase* getEventBase() override { return nullptr; }
    Executor::KeepAlive<EventBase> getEventBaseKeepAlive() override {
      eventBaseKeepAliveCount++;
      return {};
    }
    int count{0};
    int eventBaseKeepAliveCount{0};
  };

  auto f = []() {};

  FOLLY_PUSH_WARNING
  FOLLY_GNU_DISABLE_WARNING("-Wdeprecated-declarations")
  // Don't explode, we should create the default global IOExecutor lazily here.
  getIOExecutor()->add(f);

  {
    auto dummy = std::make_shared<DummyExecutor>();
    setIOExecutor(dummy);
    getIOExecutor()->add(f);
    EXPECT_FALSE(folly::getEventBaseKeepAlive());
    EXPECT_EQ(1, dummy->eventBaseKeepAliveCount);
    // Make sure we were properly installed.
    EXPECT_EQ(1, dummy->count);
  }

  // Don't explode, we should restore the default global IOExecutor because our
  // weak reference to dummy has expired
  getIOExecutor()->add(f);
  auto eventBase = folly::getEventBaseKeepAlive();
  ASSERT_TRUE(eventBase);
  folly::Baton<> baton;
  eventBase->add([&] { baton.post(); });
  baton.wait();
  FOLLY_POP_WARNING
}

TEST(GlobalExecutorTest, IOThreadCountFlagUnset) {
  folly::gflags::FlagSaver flagsaver;

  auto io_threadpool = dynamic_cast<folly::IOThreadPoolExecutor*>(
      folly::getGlobalIOExecutor().get());

  EXPECT_EQ(io_threadpool->numThreads(), folly::available_concurrency());
}

TEST(GlobalExecutorTest, CPUThreadCountFlagUnset) {
  folly::gflags::FlagSaver flagsaver;

  EXPECT_EQ(
      getGlobalCPUExecutorCounters().numThreads,
      folly::available_concurrency());
}

TEST(GlobalExecutorTest, GetGlobalCPUExecutorCounters) {
  const size_t numThreads = getGlobalCPUExecutorCounters().numThreads;
  const size_t numTasks = 1000 + numThreads;
  // Makes all tasks block until we're done.
  folly::SaturatingSemaphore<> block;
  // Ensures that the semaphore is alive until all tasks have run.
  folly::VirtualExecutor ex(getGlobalCPUExecutor());
  for (size_t i = 0; i < numTasks; ++i) {
    ex.add([&] { block.wait(); });
  }
  while (true) {
    auto counters = getGlobalCPUExecutorCounters();
    // We don't know how many tasks have been picked up, but we know they have
    // to be at most numThreads because they're blocked.
    EXPECT_GE(counters.numPendingTasks, numTasks - numThreads);
    EXPECT_LE(counters.numPendingTasks, numTasks);
    // Eventually, the executor should start all the available threads. If not,
    // the test will deadlock (and thus time out).
    if (counters.numActiveThreads == counters.numThreads) {
      break;
    }
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::microseconds(1));
  }

  // Unblock everything so we can shut down.
  block.post();
}
