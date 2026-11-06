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

#include <atomic>
#include <barrier>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>

#include <folly/CPortability.h>
#include <folly/DefaultKeepAliveExecutor.h>
#include <folly/Exception.h>
#include <folly/container/F14Map.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/FutureExecutor.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/executors/ThreadPoolExecutor.h>
#include <folly/executors/VirtualExecutor.h>
#include <folly/executors/task_queue/LifoSemMPMCQueue.h>
#include <folly/executors/task_queue/UnboundedBlockingQueue.h>
#include <folly/executors/thread_factory/InitThreadFactory.h>
#include <folly/executors/thread_factory/PriorityThreadFactory.h>
#include <folly/lang/Keep.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <folly/portability/PThread.h>
#include <folly/portability/SysResource.h>
#include <folly/synchronization/Latch.h>
#include <folly/synchronization/detail/Spin.h>
#include <folly/system/ThreadId.h>

using namespace folly;
using namespace std::chrono;

// Like ASSERT_NEAR, for chrono duration types
#define ASSERT_NEAR_NS(a, b, c)  \
  do {                           \
    ASSERT_NEAR(                 \
        nanoseconds(a).count(),  \
        nanoseconds(b).count(),  \
        nanoseconds(c).count()); \
  } while (0)

static Func burnMs(uint64_t ms) {
  return [ms]() { std::this_thread::sleep_for(milliseconds(ms)); };
}

#ifdef __linux__
static std::chrono::nanoseconds thread_clock_now() {
  timespec tp;
  clockid_t clockid;
  CHECK(!pthread_getcpuclockid(pthread_self(), &clockid));
  CHECK(!clock_gettime(clockid, &tp));
  return std::chrono::nanoseconds(tp.tv_nsec) + std::chrono::seconds(tp.tv_sec);
}

// Loop and burn cpu cycles
static void burnThreadCpu(milliseconds ms) {
  auto expires = thread_clock_now() + ms;
  while (thread_clock_now() < expires) {
  }
}

// Loop without using much cpu time
static void idleLoopFor(milliseconds ms) {
  using clock = high_resolution_clock;
  auto expires = clock::now() + ms;
  while (clock::now() < expires) {
    /* sleep override */ std::this_thread::sleep_for(100ms);
  }
}
#endif

static WorkerProvider* kWorkerProviderGlobal = nullptr;

namespace folly {

#if FOLLY_HAVE_WEAK_SYMBOLS
FOLLY_KEEP std::unique_ptr<QueueObserverFactory> make_queue_observer_factory(
    const std::string&, size_t, WorkerProvider* workerProvider) {
  kWorkerProviderGlobal = workerProvider;
  return {};
}
#endif

} // namespace folly

template <typename T>
class ThreadPoolExecutorTypedTest : public ::testing::Test {};

using ValueTypes =
    ::testing::Types<CPUThreadPoolExecutor, IOThreadPoolExecutor>;

TYPED_TEST_SUITE(ThreadPoolExecutorTypedTest, ValueTypes);

template <class TPE>
static void basic() {
  // Create and destroy
  TPE tpe(10);
}

TYPED_TEST(ThreadPoolExecutorTypedTest, Basic) {
  basic<TypeParam>();
}

template <class TPE>
static void resize() {
  TPE tpe(100);
  EXPECT_EQ(100, tpe.numThreads());
  tpe.setNumThreads(50);
  EXPECT_EQ(50, tpe.numThreads());
  tpe.setNumThreads(150);
  EXPECT_EQ(150, tpe.numThreads());
}

TYPED_TEST(ThreadPoolExecutorTypedTest, Resize) {
  resize<TypeParam>();
}

template <class TPE>
static void stop() {
  TPE tpe(1);
  std::atomic<int> completed(0);
  auto f = [&]() {
    burnMs(10)();
    completed++;
  };
  for (int i = 0; i < 1000; i++) {
    tpe.add(f);
  }
  tpe.stop();
  EXPECT_GT(1000, completed);
}

// IOThreadPoolExecutor's stop() behaves like join(). Outstanding tasks belong
// to the event base, will be executed upon its destruction, and cannot be
// taken back.
template <>
void stop<IOThreadPoolExecutor>() {
  IOThreadPoolExecutor tpe(1);
  std::atomic<int> completed(0);
  auto f = [&]() {
    burnMs(10)();
    completed++;
  };
  for (int i = 0; i < 10; i++) {
    tpe.add(f);
  }
  tpe.stop();
  EXPECT_EQ(10, completed);
}

TYPED_TEST(ThreadPoolExecutorTypedTest, Stop) {
  stop<TypeParam>();
}

template <class TPE>
static void join() {
  TPE tpe(10);
  std::atomic<int> completed(0);
  auto f = [&]() {
    burnMs(1)();
    completed++;
  };
  for (int i = 0; i < 1000; i++) {
    tpe.add(f);
  }
  tpe.join();
  EXPECT_EQ(1000, completed);
}

TYPED_TEST(ThreadPoolExecutorTypedTest, Join) {
  join<TypeParam>();
}

template <class TPE>
static void destroy() {
  TPE tpe(1);
  std::atomic<int> completed(0);
  auto f = [&]() {
    burnMs(10)();
    completed++;
  };
  for (int i = 0; i < 1000; i++) {
    tpe.add(f);
  }
  tpe.stop();
  EXPECT_GT(1000, completed);
}

// IOThreadPoolExecutor's destuctor joins all tasks. Outstanding tasks belong
// to the event base, will be executed upon its destruction, and cannot be
// taken back.
template <>
void destroy<IOThreadPoolExecutor>() {
  Optional<IOThreadPoolExecutor> tpe(std::in_place, 1);
  std::atomic<int> completed(0);
  auto f = [&]() {
    burnMs(10)();
    completed++;
  };
  for (int i = 0; i < 10; i++) {
    tpe->add(f);
  }
  tpe.reset();
  EXPECT_EQ(10, completed);
}

TYPED_TEST(ThreadPoolExecutorTypedTest, Destroy) {
  destroy<TypeParam>();
}

template <class TPE>
static void resizeUnderLoad() {
  TPE tpe(10);
  std::atomic<int> completed(0);
  auto f = [&]() {
    burnMs(1)();
    completed++;
  };
  for (int i = 0; i < 1000; i++) {
    tpe.add(f);
  }
  tpe.setNumThreads(5);
  tpe.setNumThreads(15);
  tpe.join();
  EXPECT_EQ(1000, completed);
}

TYPED_TEST(ThreadPoolExecutorTypedTest, ResizeUnderLoad) {
  resizeUnderLoad<TypeParam>();
}

template <class TPE>
static void poolStats() {
  folly::Baton<> startBaton, endBaton;
  TPE tpe(1);
  auto stats = tpe.getPoolStats();
  EXPECT_GE(1, stats.threadCount);
  EXPECT_GE(1, stats.idleThreadCount);
  EXPECT_EQ(0, stats.activeThreadCount);
  EXPECT_EQ(0, stats.pendingTaskCount);
  EXPECT_EQ(0, tpe.getPendingTaskCount());
  EXPECT_EQ(0, stats.totalTaskCount);
  tpe.add([&]() {
    startBaton.post();
    endBaton.wait();
  });
  tpe.add([&]() {});
  startBaton.wait();
  stats = tpe.getPoolStats();
  EXPECT_EQ(1, stats.threadCount);
  EXPECT_EQ(0, stats.idleThreadCount);
  EXPECT_EQ(1, stats.activeThreadCount);
  EXPECT_EQ(1, stats.pendingTaskCount);
  EXPECT_EQ(1, tpe.getPendingTaskCount());
  EXPECT_EQ(2, stats.totalTaskCount);
  endBaton.post();
}

TEST(ThreadPoolExecutorTest, CPUPoolStats) {
  poolStats<CPUThreadPoolExecutor>();
}

TEST(ThreadPoolExecutorTest, IOPoolStats) {
  poolStats<IOThreadPoolExecutor>();
}

template <class TPE>
static void poolStatsMaxIdleTime() {
  folly::Baton<> warmupBaton, startBaton, endBaton;
  TPE tpe(1);

  // Workers may be created lazily, and lastActiveTime is only written when a
  // worker goes idle, so run one task to get a worker into the thread list.
  tpe.add([&]() { warmupBaton.post(); });
  warmupBaton.wait();

  auto idledLongEnough = [&] {
    return tpe.getPoolStats().maxIdleTime >= milliseconds(50);
  };
  EXPECT_EQ(
      folly::detail::spin_result::success,
      folly::detail::spin_yield_until(
          std::chrono::steady_clock::now() + std::chrono::seconds(5),
          idledLongEnough));

  tpe.add([&]() {
    startBaton.post();
    endBaton.wait();
  });
  startBaton.wait();
  // A worker writes lastActiveTime only when it goes idle, so the worker now
  // running the task still carries the >=50ms-old timestamp observed above.
  // Busy workers must not contribute to maxIdleTime regardless.
  EXPECT_EQ(std::chrono::nanoseconds::zero(), tpe.getPoolStats().maxIdleTime);
  endBaton.post();
}

TEST(ThreadPoolExecutorTest, CPUPoolStatsMaxIdleTime) {
  poolStatsMaxIdleTime<CPUThreadPoolExecutor>();
}

TEST(ThreadPoolExecutorTest, IOPoolStatsMaxIdleTime) {
  poolStatsMaxIdleTime<IOThreadPoolExecutor>();
}

template <class TPE>
static void poolStatsLight() {
  folly::Baton<> warmupBaton, startBaton, endBaton;
  TPE tpe(1);

  tpe.add([&]() { warmupBaton.post(); });
  warmupBaton.wait();

  // PoolStatsLight does not declare the fields the light variant omits, so
  // their absence is a compile-time guarantee and needs no assertion here.
  // What remains to check is that the fields it does declare agree with the
  // full variant, in both a quiescent and a saturated pool.
  auto fullPopulated = [&] {
    const auto s = tpe.getPoolStats();
    return s.processedTaskCount > 0 && s.maxIdleTime >= milliseconds(50);
  };
  EXPECT_EQ(
      folly::detail::spin_result::success,
      folly::detail::spin_yield_until(
          std::chrono::steady_clock::now() + std::chrono::seconds(5),
          fullPopulated));

  const auto idleFull = tpe.getPoolStats();
  const auto idleLight = tpe.getPoolStatsLight();
  EXPECT_EQ(idleFull.threadCount, idleLight.threadCount);
  EXPECT_EQ(idleFull.activeThreadCount, idleLight.activeThreadCount);
  EXPECT_EQ(idleFull.idleThreadCount, idleLight.idleThreadCount);
  EXPECT_EQ(idleFull.pendingTaskCount, idleLight.pendingTaskCount);

  // Saturate the pool and queue one more, so pendingTaskCount is nonzero and
  // the agreement below is not vacuous.
  tpe.add([&]() {
    startBaton.post();
    endBaton.wait();
  });
  startBaton.wait();
  tpe.add([&]() {});

  const auto busyFull = tpe.getPoolStats();
  const auto busyLight = tpe.getPoolStatsLight();
  EXPECT_GT(busyFull.pendingTaskCount, 0);
  EXPECT_EQ(busyFull.pendingTaskCount, busyLight.pendingTaskCount);
  EXPECT_EQ(busyFull.threadCount, busyLight.threadCount);
  EXPECT_EQ(busyFull.activeThreadCount, busyLight.activeThreadCount);
  EXPECT_EQ(busyFull.idleThreadCount, busyLight.idleThreadCount);
  endBaton.post();
}

TEST(ThreadPoolExecutorTest, CPUPoolStatsLight) {
  poolStatsLight<CPUThreadPoolExecutor>();
}

TEST(ThreadPoolExecutorTest, IOPoolStatsLight) {
  poolStatsLight<IOThreadPoolExecutor>();
}

template <class TPE>
static void taskStats() {
  TPE tpe(1);
  std::atomic<int> c(0);
  auto now = std::chrono::steady_clock::now();
  tpe.subscribeToTaskStats([&](const ThreadPoolExecutor::TaskStats& stats) {
    int i = c++;
    EXPECT_LT(now, stats.enqueueTime);
    EXPECT_LT(milliseconds(0), stats.runTime);
    if (i == 1) {
      EXPECT_LT(milliseconds(0), stats.waitTime);
      EXPECT_NE(0, stats.requestId);
    } else {
      EXPECT_EQ(0, stats.requestId);
    }
  });
  tpe.add(burnMs(10));
  RequestContextScopeGuard rctx;
  tpe.add(burnMs(10));
  tpe.join();
  EXPECT_EQ(2, c);
}

TYPED_TEST(ThreadPoolExecutorTypedTest, TaskStats) {
  taskStats<TypeParam>();
}

TYPED_TEST(ThreadPoolExecutorTypedTest, TaskObserver) {
  struct TestTaskObserver : ThreadPoolExecutor::TaskObserver {
    struct TaskState {
      std::chrono::steady_clock::time_point enqueueTime;
      std::optional<std::chrono::nanoseconds> waitTime;
      bool ran = false;
    };

    void taskEnqueued(
        const ThreadPoolExecutor::TaskInfo& info) noexcept override {
      auto ts = taskStates.wlock();
      auto [it, inserted] = ts->try_emplace(info.taskId);
      ASSERT_TRUE(inserted);
      it->second.enqueueTime = info.enqueueTime;
    }

    void taskDequeued(
        const ThreadPoolExecutor::DequeuedTaskInfo& info) noexcept override {
      auto ts = taskStates.wlock();
      auto it = ts->find(info.taskId);
      ASSERT_TRUE(it != ts->end());
      EXPECT_EQ(it->second.enqueueTime, info.enqueueTime);
      ASSERT_FALSE(std::exchange(it->second.waitTime, info.waitTime));
    }

    void taskProcessed(
        const ThreadPoolExecutor::ProcessedTaskInfo& info) noexcept override {
      auto ts = taskStates.wlock();
      auto it = ts->find(info.taskId);
      ASSERT_TRUE(it != ts->end());
      EXPECT_EQ(it->second.enqueueTime, info.enqueueTime);
      ASSERT_TRUE(it->second.waitTime);
      EXPECT_EQ(*it->second.waitTime, info.waitTime);
      EXPECT_GT(info.runTime.count(), 0);
      it->second.ran = true;
    }

    Synchronized<F14FastMap<uint64_t, TaskState>> taskStates;
  };

  TypeParam ex{4};
  auto observer = std::make_unique<TestTaskObserver>();
  auto* observerPtr = observer.get();
  ex.addTaskObserver(std::move(observer));

  static constexpr size_t kNumTasks = 10;
  for (size_t i = 0; i < kNumTasks; ++i) {
    ex.add(burnMs(10));
  }

  ex.join();

  auto ts = observerPtr->taskStates.exchange({});
  EXPECT_EQ(ts.size(), kNumTasks);
  for (auto& [_, taskState] : ts) {
    EXPECT_TRUE(taskState.ran);
  }
}

// taskDequeued() and taskProcessed() run inside runTask()'s
// RequestContextScopeGuard, so observers see the task's RequestContext -- the
// same one task.func_ runs under. Verify both callbacks observe the
// RequestContext that was active when the task was submitted, for CPU and IO.
TEST(ThreadPoolExecutorTest, ObserverCallbacksRunUnderRequestContext) {
  struct ContextObserver : ThreadPoolExecutor::TaskObserver {
    void taskDequeued(
        const ThreadPoolExecutor::DequeuedTaskInfo&) noexcept override {
      dequeuedCtx = RequestContext::get();
    }
    void taskProcessed(
        const ThreadPoolExecutor::ProcessedTaskInfo&) noexcept override {
      processedCtx = RequestContext::get();
    }
    std::atomic<RequestContext*> dequeuedCtx{nullptr};
    std::atomic<RequestContext*> processedCtx{nullptr};
  };

  auto check = [&](ThreadPoolExecutor& ex, const char* /*label*/) {
    auto observer = std::make_unique<ContextObserver>();
    auto* observerPtr = observer.get();
    ex.addTaskObserver(std::move(observer));

    RequestContextScopeGuard rctx;
    RequestContext* submitCtx = RequestContext::get();
    ex.add([] {});
    ex.join();

    EXPECT_EQ(observerPtr->dequeuedCtx.load(), submitCtx);
    EXPECT_EQ(observerPtr->processedCtx.load(), submitCtx);
  };

  CPUThreadPoolExecutor cpu{1};
  check(cpu, "CPUThreadPoolExecutor");

  IOThreadPoolExecutor io{1};
  check(io, "IOThreadPoolExecutor");
}

TEST(ThreadPoolExecutorTest, GetUsedCpuTime) {
#ifdef __linux__
  CPUThreadPoolExecutor e(4);
  ASSERT_EQ(e.numActiveThreads(), 0);
  ASSERT_EQ(e.getUsedCpuTime(), nanoseconds(0));
  // get busy
  Latch latch(4);
  auto busy_loop = [&] {
    burnThreadCpu(1s);
    latch.count_down();
  };
  auto idle_loop = [&] {
    idleLoopFor(1s);
    latch.count_down();
  };
  e.add(busy_loop); // +1s cpu time
  e.add(busy_loop); // +1s cpu time
  e.add(idle_loop); // +0s cpu time
  e.add(idle_loop); // +0s cpu time
  latch.wait();
  // pool should have used 2s cpu time (in 1s wall clock time)
  auto elapsed0 = e.getUsedCpuTime();
  ASSERT_NEAR_NS(elapsed0, 2s, 100ms);
  // stop all threads
  e.setNumThreads(0);
  ASSERT_EQ(e.numActiveThreads(), 0);
  // total pool CPU time should not have changed
  auto elapsed1 = e.getUsedCpuTime();
  ASSERT_NEAR_NS(elapsed0, elapsed1, 100ms);
  // add a thread, do nothing, cpu time should stay the same
  e.setNumThreads(1);
  Baton<> baton;
  e.add([&] { baton.post(); });
  baton.wait();
  ASSERT_EQ(e.numActiveThreads(), 1);
  auto elapsed2 = e.getUsedCpuTime();
  ASSERT_NEAR_NS(elapsed1, elapsed2, 100ms);
  // now burn some more cycles
  baton.reset();
  e.add([&] {
    burnThreadCpu(500ms);
    baton.post();
  });
  baton.wait();
  auto elapsed3 = e.getUsedCpuTime();
  ASSERT_NEAR_NS(elapsed3, elapsed2 + 500ms, 100ms);
#else
  CPUThreadPoolExecutor e(1);
  // Just make sure 0 is returned
  ASSERT_EQ(e.getUsedCpuTime(), nanoseconds(0));
  Baton<> baton;
  e.add([&] {
    auto expires = steady_clock::now() + 500ms;
    while (steady_clock::now() < expires) {
    }
    baton.post();
  });
  baton.wait();
  ASSERT_EQ(e.getUsedCpuTime(), nanoseconds(0));
#endif
}

template <class TPE>
static void expiration() {
  TPE tpe(1);
  std::atomic<int> statCbCount(0);
  tpe.subscribeToTaskStats([&](const ThreadPoolExecutor::TaskStats& stats) {
    int i = statCbCount++;
    if (i == 0) {
      EXPECT_FALSE(stats.expired);
    } else if (i == 1) {
      EXPECT_TRUE(stats.expired);
    } else {
      FAIL();
    }
  });
  std::atomic<int> expireCbCount(0);
  auto expireCb = [&]() { expireCbCount++; };
  tpe.add(burnMs(10), seconds(60), expireCb);
  tpe.add(burnMs(10), milliseconds(10), expireCb);
  tpe.join();
  EXPECT_EQ(2, statCbCount);
  EXPECT_EQ(1, expireCbCount);
}

TEST(ThreadPoolExecutorTest, CPUExpiration) {
  expiration<CPUThreadPoolExecutor>();
}

TEST(ThreadPoolExecutorTest, IOExpiration) {
  expiration<IOThreadPoolExecutor>();
}

template <typename TPE>
static void futureExecutor() {
  FutureExecutor<TPE> fe(2);
  std::atomic<int> c{0};
  fe.addFuture([]() { return makeFuture<int>(42); }).then([&](Try<int>&& t) {
    c++;
    EXPECT_EQ(42, t.value());
  });
  fe.addFuture([]() { return 100; }).then([&](Try<int>&& t) {
    c++;
    EXPECT_EQ(100, t.value());
  });
  fe.addFuture([]() { return makeFuture(); }).then([&](Try<Unit>&& t) {
    c++;
    EXPECT_NO_THROW(t.value());
  });
  fe.addFuture([]() { return; }).then([&](Try<Unit>&& t) {
    c++;
    EXPECT_NO_THROW(t.value());
  });
  fe.addFuture([]() {
      throw std::runtime_error("oops");
    }).then([&](Try<Unit>&& t) {
    c++;
    EXPECT_THROW(t.value(), std::runtime_error);
  });
  // Test doing actual async work
  folly::Baton<> baton;
  fe.addFuture([&]() {
      auto p = std::make_shared<Promise<int>>();
      std::thread t([p]() {
        burnMs(10)();
        p->setValue(42);
      });
      t.detach();
      return p->getFuture();
    }).then([&](Try<int>&& t) {
    EXPECT_EQ(42, t.value());
    c++;
    baton.post();
  });
  baton.wait();
  fe.join();
  EXPECT_EQ(6, c);
}

TYPED_TEST(ThreadPoolExecutorTypedTest, FuturePool) {
  futureExecutor<TypeParam>();
}

TEST(ThreadPoolExecutorTest, PriorityPreemptionTest) {
  bool tookLopri = false;
  auto completed = 0;
  auto hipri = [&] {
    EXPECT_FALSE(tookLopri);
    completed++;
  };
  auto lopri = [&] {
    tookLopri = true;
    completed++;
  };
  CPUThreadPoolExecutor pool(0, 2);
  {
    VirtualExecutor ve(pool);
    for (int i = 0; i < 50; i++) {
      ve.addWithPriority(lopri, Executor::LO_PRI);
    }
    for (int i = 0; i < 50; i++) {
      ve.addWithPriority(hipri, Executor::HI_PRI);
    }
    pool.setNumThreads(1);
  }
  EXPECT_EQ(100, completed);
}

class TestObserver : public ThreadPoolExecutor::Observer {
 public:
  void threadStarted(ThreadPoolExecutor::ThreadHandle*) noexcept override {
    threads_++;
  }
  void threadStopped(ThreadPoolExecutor::ThreadHandle*) noexcept override {
    threads_--;
  }
  void threadPreviouslyStarted(
      ThreadPoolExecutor::ThreadHandle*) noexcept override {
    threads_++;
  }
  void threadNotYetStopped(
      ThreadPoolExecutor::ThreadHandle*) noexcept override {
    threads_--;
  }
  void checkCalls() { ASSERT_EQ(threads_, 0); }

 private:
  std::atomic<int> threads_{0};
};

template <typename TPE>
static void testObserver() {
  auto observer = std::make_shared<TestObserver>();

  {
    TPE exe(10);
    exe.addObserver(observer);
    exe.setNumThreads(3);
    exe.setNumThreads(0);
    exe.setNumThreads(7);
    exe.removeObserver(observer);
    exe.setNumThreads(10);
  }

  observer->checkCalls();
}

TYPED_TEST(ThreadPoolExecutorTypedTest, Observer) {
  testObserver<TypeParam>();
}

TEST(ThreadPoolExecutorTest, AddWithPriority) {
  std::atomic_int c{0};
  auto f = [&] { c++; };

  // IO exe doesn't support priorities
  IOThreadPoolExecutor ioExe(10);
  EXPECT_THROW(ioExe.addWithPriority(f, 0), std::runtime_error);

  CPUThreadPoolExecutor cpuExe(10, 3);
  cpuExe.addWithPriority(f, -1);
  cpuExe.addWithPriority(f, 0);
  cpuExe.addWithPriority(f, 1);
  cpuExe.addWithPriority(f, -2); // will add at the lowest priority
  cpuExe.addWithPriority(f, 2); // will add at the highest priority
  cpuExe.addWithPriority(f, Executor::LO_PRI);
  cpuExe.addWithPriority(f, Executor::HI_PRI);
  cpuExe.join();

  EXPECT_EQ(7, c);
}

TEST(ThreadPoolExecutorTest, BlockingQueue) {
  std::atomic_int c{0};
  auto f = [&] {
    burnMs(1)();
    c++;
  };
  const int kQueueCapacity = 1;
  const int kThreads = 1;

  auto queue = std::make_unique<LifoSemMPMCQueue<
      CPUThreadPoolExecutor::CPUTask,
      QueueBehaviorIfFull::BLOCK>>(kQueueCapacity);

  CPUThreadPoolExecutor cpuExe(
      kThreads,
      std::move(queue),
      std::make_shared<NamedThreadFactory>("CPUThreadPool"));

  // Add `f` five times. It sleeps for 1ms every time. Calling
  // `cppExec.add()` is *almost* guaranteed to block because there's
  // only 1 cpu worker thread.
  for (int i = 0; i < 5; i++) {
    EXPECT_NO_THROW(cpuExe.add(f));
  }
  cpuExe.join();

  EXPECT_EQ(5, c);
}

TEST(ThreadPoolExecutorTest, NoThreadPriorityInheritance) {
  constexpr size_t kNumThreads = 16;
  const auto initialPriority = getpriority(PRIO_PROCESS, 0);

  // If minThreads == maxThreads, no threads should be created on add(), and
  // thus they should not inherit the parent thread's priority. Instead, all
  // threads should inherit the priority at the time of construction.
  CPUThreadPoolExecutor exe{std::pair(kNumThreads, kNumThreads)};

  // Nice the current thread.
  setpriority(PRIO_PROCESS, 0, 19);

  Latch ready{kNumThreads + 1};
  for (size_t i = 0; i < kNumThreads; ++i) {
    exe.add([&] {
      EXPECT_EQ(getpriority(PRIO_PROCESS, 0), initialPriority);
      ready.arrive_and_wait();
    });
  }
  ready.arrive_and_wait();
  exe.join();
}

namespace {

template <class T>
class NoTimeoutBlockingQueue : public UnboundedBlockingQueue<T> {
 public:
  folly::Optional<T> try_take_for(std::chrono::milliseconds time) override {
    ADD_FAILURE() << "try_take_for() should not be called";
    return UnboundedBlockingQueue<T>::try_take_for(time);
  }
};

} // namespace

TEST(ThreadPoolExecutorTest, NoTimeoutInNonDynamicPool) {
  constexpr size_t kNumThreads = 16;
  // If minThreads == maxThreads, only non-timeout take() should be used on the
  // queue.
  CPUThreadPoolExecutor exe{
      std::pair(kNumThreads, kNumThreads),
      std::make_unique<
          NoTimeoutBlockingQueue<CPUThreadPoolExecutor::CPUTask>>()};
  exe.add([] {});
  exe.join();
}

TEST(PriorityThreadFactoryTest, ThreadPriority) {
  errno = 0;
  auto currentPriority = getpriority(PRIO_PROCESS, 0);
  if (errno != 0) {
    throwSystemError("failed to get current priority");
  }

  // Non-root users can only increase the priority value.  Make sure we are
  // trying to go to a higher priority than we are currently running as, up to
  // the maximum allowed of 20.
  int desiredPriority = std::min(20, currentPriority + 1);

  PriorityThreadFactory factory(
      std::make_shared<NamedThreadFactory>("stuff"), desiredPriority);
  int actualPriority = -21;
  factory.newThread([&]() { actualPriority = getpriority(PRIO_PROCESS, 0); })
      .join();
  EXPECT_EQ(desiredPriority, actualPriority);
}

TEST(InitThreadFactoryTest, InitializerCalled) {
  int initializerCalledCount = 0;
  InitThreadFactory factory(
      std::make_shared<NamedThreadFactory>("test"),
      [&initializerCalledCount] { initializerCalledCount++; });
  factory
      .newThread([&initializerCalledCount]() {
        EXPECT_EQ(initializerCalledCount, 1);
      })
      .join();
  EXPECT_EQ(initializerCalledCount, 1);
}

TEST(InitThreadFactoryTest, InitializerAndFinalizerCalled) {
  bool initializerCalled = false;
  bool taskBodyCalled = false;
  bool finalizerCalled = false;

  InitThreadFactory factory(
      std::make_shared<NamedThreadFactory>("test"),
      [&] {
        // thread initializer
        EXPECT_FALSE(initializerCalled);
        EXPECT_FALSE(taskBodyCalled);
        EXPECT_FALSE(finalizerCalled);
        initializerCalled = true;
      },
      [&] {
        // thread finalizer
        EXPECT_TRUE(initializerCalled);
        EXPECT_TRUE(taskBodyCalled);
        EXPECT_FALSE(finalizerCalled);
        finalizerCalled = true;
      });

  factory
      .newThread([&]() {
        EXPECT_TRUE(initializerCalled);
        EXPECT_FALSE(taskBodyCalled);
        EXPECT_FALSE(finalizerCalled);
        taskBodyCalled = true;
      })
      .join();

  EXPECT_TRUE(initializerCalled);
  EXPECT_TRUE(taskBodyCalled);
  EXPECT_TRUE(finalizerCalled);
}

class TestData : public folly::RequestData {
 public:
  explicit TestData(int data) : data_(data) {}
  ~TestData() override {}

  bool hasCallback() override { return false; }

  int data_;
};

TEST(ThreadPoolExecutorTest, RequestContext) {
  RequestContextScopeGuard rctx; // create new request context for this scope
  EXPECT_EQ(nullptr, RequestContext::get()->getContextData("test"));
  RequestContext::get()->setContextData("test", std::make_unique<TestData>(42));
  auto data = RequestContext::get()->getContextData("test");
  EXPECT_EQ(42, dynamic_cast<TestData*>(data)->data_);

  static constexpr auto verifyRequestContext = +[] {
    auto data2 = RequestContext::get()->getContextData("test");
    EXPECT_TRUE(data2 != nullptr);
    if (data2 != nullptr) {
      EXPECT_EQ(42, dynamic_cast<TestData*>(data2)->data_);
    }
  };

  {
    CPUThreadPoolExecutor executor(1);
    executor.add([] { verifyRequestContext(); });
    executor.add([x = makeGuard(verifyRequestContext)] {});
  }
}

std::atomic<int> g_sequence{};

struct SlowMover {
  explicit SlowMover(bool slow_ = false) : slow(slow_) {}
  SlowMover(SlowMover&& other) noexcept { *this = std::move(other); }
  SlowMover& operator=(SlowMover&& other) noexcept {
    ++g_sequence;
    slow = other.slow;
    if (slow) {
      /* sleep override */ std::this_thread::sleep_for(milliseconds(50));
    }
    ++g_sequence;
    return *this;
  }

  bool slow;
};

template <typename Q>
void bugD3527722_test() {
  // Test that the queue does not get stuck if writes are completed in
  // order opposite to how they are initiated.
  Q q(1024);
  std::atomic<int> turn{};

  std::thread consumer1([&] {
    ++turn;
    q.take();
  });
  std::thread consumer2([&] {
    ++turn;
    q.take();
  });

  std::thread producer1([&] {
    ++turn;
    while (turn < 4) {
      ;
    }
    ++turn;
    q.add(SlowMover(true));
  });
  std::thread producer2([&] {
    ++turn;
    while (turn < 5) {
      ;
    }
    q.add(SlowMover(false));
  });

  producer1.join();
  producer2.join();
  consumer1.join();
  consumer2.join();
}

TEST(ThreadPoolExecutorTest, LifoSemMPMCQueueBugD3527722) {
  bugD3527722_test<LifoSemMPMCQueue<SlowMover>>();
}

template <typename T>
struct UBQ : public UnboundedBlockingQueue<T> {
  explicit UBQ(int) {}
};

TEST(ThreadPoolExecutorTest, UnboundedBlockingQueueBugD3527722) {
  bugD3527722_test<UBQ<SlowMover>>();
}

template <typename Q>
void nothrow_not_full_test() {
  /* LifoSemMPMCQueue should not throw when not full when active
     consumers are delayed. */
  Q q(2);
  g_sequence = 0;

  std::thread consumer1([&] {
    while (g_sequence < 4) {
      ;
    }
    q.take(); // ++g_sequence to 5 then slow
  });
  std::thread consumer2([&] {
    while (g_sequence < 5) {
      ;
    }
    q.take(); // ++g_sequence to 6 and 7 - fast
  });

  std::thread producer([&] {
    q.add(SlowMover(true)); // ++g_sequence to 1 and 2
    q.add(SlowMover(false)); // ++g_sequence to 3 and 4
    while (g_sequence < 7) { // g_sequence == 7 implies queue is not full
      ;
    }
    EXPECT_NO_THROW(q.add(SlowMover(false)));
  });

  producer.join();
  consumer1.join();
  consumer2.join();
}

TEST(ThreadPoolExecutorTest, LifoSemMPMCQueueNoThrowNotFull) {
  nothrow_not_full_test<LifoSemMPMCQueue<SlowMover>>();
}

template <typename TPE>
static void removeThreadTest() {
  // test that adding a .then() after we have removed some threads
  // doesn't cause deadlock and they are executed on different threads
  folly::Optional<folly::Future<int>> f;
  std::thread::id id1, id2;
  TPE fe(2);
  f = folly::makeFuture()
          .via(&fe)
          .thenValue([&id1](auto&&) {
            burnMs(100)();
            id1 = std::this_thread::get_id();
          })
          .thenValue([&id2](auto&&) {
            return 77;
            id2 = std::this_thread::get_id();
          });
  fe.setNumThreads(1);

  // future::then should be fulfilled because there is other thread available
  EXPECT_EQ(77, std::move(*f).get());
  // two thread should be different because then part should be rescheduled to
  // the other thread
  EXPECT_NE(id1, id2);
}

TYPED_TEST(ThreadPoolExecutorTypedTest, RemoveThread) {
  removeThreadTest<TypeParam>();
}

template <typename TPE>
static void resizeThreadWhileExecutingTest() {
  TPE tpe(10);
  EXPECT_EQ(10, tpe.numThreads());

  std::atomic<int> completed(0);
  auto f = [&]() {
    burnMs(10)();
    completed++;
  };
  for (int i = 0; i < 1000; i++) {
    tpe.add(f);
  }
  tpe.setNumThreads(8);
  EXPECT_EQ(8, tpe.numThreads());
  tpe.setNumThreads(5);
  EXPECT_EQ(5, tpe.numThreads());
  tpe.setNumThreads(15);
  EXPECT_EQ(15, tpe.numThreads());
  tpe.join();
  EXPECT_EQ(1000, completed);
}

TYPED_TEST(ThreadPoolExecutorTypedTest, ResizeThreadWhileExecuting) {
  resizeThreadWhileExecutingTest<TypeParam>();
}

template <typename TPE>
void keepAliveTest() {
  auto executor = std::make_unique<TPE>(4);

  auto f =
      futures::sleep(std::chrono::milliseconds{100})
          .via(executor.get())
          .thenValue([keepAlive = getKeepAliveToken(executor.get())](auto&&) {
            return 42;
          })
          .semi();

  executor.reset();

  EXPECT_TRUE(f.isReady());
  EXPECT_EQ(42, std::move(f).get());
}

TYPED_TEST(ThreadPoolExecutorTypedTest, KeepAlive) {
  keepAliveTest<TypeParam>();
}

int getNumThreadPoolExecutors() {
  int count = 0;
  ThreadPoolExecutor::withAll([&count](ThreadPoolExecutor&) { count++; });
  return count;
}

template <typename TPE>
static void registersToExecutorListTest() {
  EXPECT_EQ(0, getNumThreadPoolExecutors());
  {
    TPE tpe(10);
    EXPECT_EQ(1, getNumThreadPoolExecutors());
    {
      TPE tpe2(5);
      EXPECT_EQ(2, getNumThreadPoolExecutors());
    }
    EXPECT_EQ(1, getNumThreadPoolExecutors());
  }
  EXPECT_EQ(0, getNumThreadPoolExecutors());
}

TYPED_TEST(ThreadPoolExecutorTypedTest, RegistersToExecutorList) {
  registersToExecutorListTest<TypeParam>();
}

template <typename TPE>
static void testUsesNameFromNamedThreadFactory() {
  // Verify that the name is propagated even if the NamedThreadFactory is
  // wrapped.
  auto tf = std::make_shared<InitThreadFactory>(
      std::make_shared<NamedThreadFactory>("my_executor"), [] {});
  TPE tpe(10, tf);
  EXPECT_EQ("my_executor", tpe.getName());
}

TYPED_TEST(ThreadPoolExecutorTypedTest, UsesNameFromNamedThreadFactory) {
  testUsesNameFromNamedThreadFactory<TypeParam>();
}

TEST(ThreadPoolExecutorTest, DynamicThreadsTest) {
  std::barrier<> barrier{3};
  auto twice_waiting_task = [&] {
    barrier.arrive_and_wait(), barrier.arrive_and_wait();
  };
  CPUThreadPoolExecutor e(2);
  e.setThreadDeathTimeout(std::chrono::milliseconds(100));
  e.add(twice_waiting_task);
  e.add(twice_waiting_task);
  barrier.arrive_and_wait(); // ensure both tasks are mid-flight
  EXPECT_EQ(2, e.getPoolStats().activeThreadCount) << "sanity check";

  auto pred = [&] { return e.getPoolStats().activeThreadCount == 0; };
  EXPECT_FALSE(pred()) << "sanity check";
  barrier.arrive_and_wait(); // let both mid-flight tasks complete
  EXPECT_EQ(
      folly::detail::spin_result::success,
      folly::detail::spin_yield_until(
          std::chrono::steady_clock::now() + std::chrono::seconds(1), pred));
}

TEST(ThreadPoolExecutorTest, GetThreadIdCollector) {
  CPUThreadPoolExecutor e(1);
  auto* collector = e.getThreadIdCollector();
  ASSERT_TRUE(collector != nullptr);

  EXPECT_THAT(collector->collectThreadIds().threadIds, testing::IsEmpty());

  pid_t tid;
  Baton<> ready;
  Baton<> unblock;
  e.add([&] {
    tid = getOSThreadID();
    ready.post();
    unblock.wait(); // Wait until we acquire the keepalive.
  });

  ready.wait();
  auto ids = collector->collectThreadIds();
  EXPECT_THAT(ids.threadIds, testing::ElementsAre(tid));
  unblock.post();

  Baton<> joined;
  std::thread t([&] {
    e.join();
    joined.post();
  });
  // Until we release ids, the executor join cannot complete
  EXPECT_FALSE(joined.try_wait_for(100ms));
  // But things should eventually complete when released.
  ids = {};
  t.join();
  EXPECT_THAT(collector->collectThreadIds().threadIds, testing::IsEmpty());
}

TEST(ThreadPoolExecutorTest, DynamicThreadAddRemoveRace) {
  CPUThreadPoolExecutor e(1);
  e.setThreadDeathTimeout(std::chrono::milliseconds(0));
  std::atomic<uint64_t> count{0};
  for (int i = 0; i < 10000; i++) {
    Baton<> b;
    e.add([&]() {
      count.fetch_add(1, std::memory_order_relaxed);
      b.post();
    });
    b.wait();
  }
  e.join();
  EXPECT_EQ(count, 10000);
}

TEST(ThreadPoolExecutorTest, AddPerf) {
  CPUThreadPoolExecutor e(
      kIsSanitizeThread ? 25 : 1000,
      std::make_shared<NamedThreadFactory>("CPUThreadPool"));
  e.setThreadDeathTimeout(std::chrono::milliseconds(1));
  for (int i = 0; i < 10000; i++) {
    e.add([&, ka = getKeepAliveToken(e)]() {
      // holding a keep-alive here permits the following add()
      // to occur safely, concurrently with the stop() below
      e.add([]() { /* sleep override */ usleep(1000); });
    });
  }
  e.stop();
}

class ExecutorWorkerProviderTest : public ::testing::Test {
 protected:
  void SetUp() override { kWorkerProviderGlobal = nullptr; }
  void TearDown() override { kWorkerProviderGlobal = nullptr; }
};

TEST_F(ExecutorWorkerProviderTest, ThreadCollectorBasicTest) {
  // Start 4 threads and have all of them work on a task.
  // Then invoke the ThreadIdCollector::collectThreadIds()
  // method to capture the set of active thread ids.
  std::barrier<> barrier{5};
  Synchronized<std::vector<pid_t>> expectedTids;
  auto task = [&]() {
    expectedTids.wlock()->push_back(folly::getOSThreadID());
    barrier.arrive_and_wait();
  };
  CPUThreadPoolExecutor e(4);
  for (int i = 0; i < 4; ++i) {
    e.add(task);
  }
  barrier.arrive_and_wait();
  {
    const auto threadIdsWithKA = kWorkerProviderGlobal->collectThreadIds();
    const auto& ids = threadIdsWithKA.threadIds;
    auto locked = expectedTids.rlock();
    EXPECT_EQ(ids.size(), locked->size());
    EXPECT_TRUE(std::is_permutation(ids.begin(), ids.end(), locked->begin()));
  }
  e.join();
}

TEST_F(ExecutorWorkerProviderTest, ThreadCollectorMultipleInvocationTest) {
  // Run some tasks via the executor and invoke
  // WorkerProvider::collectThreadIds() at least twice to make sure that there
  // is no deadlock in repeated invocations.
  CPUThreadPoolExecutor e(1);
  // Ensure thread is fully initialized by running a task and waiting for it.
  folly::Baton<> initialized;
  e.add([&]() { initialized.post(); });
  initialized.wait();
  {
    auto idsWithKA1 = kWorkerProviderGlobal->collectThreadIds();
    auto idsWithKA2 = kWorkerProviderGlobal->collectThreadIds();
    auto& ids1 = idsWithKA1.threadIds;
    auto& ids2 = idsWithKA2.threadIds;
    EXPECT_EQ(ids1.size(), 1);
    EXPECT_EQ(ids1.size(), ids2.size());
    EXPECT_EQ(ids1, ids2);
  }
  // Add some more threads and schedule tasks while the collector
  // is capturing thread Ids.
  std::array<folly::Baton<>, 4> bats;
  {
    auto idsWithKA1 = kWorkerProviderGlobal->collectThreadIds();
    e.setNumThreads(4);
    // Ensure all new threads are fully initialized by running tasks.
    std::array<folly::Baton<>, 4> threadInit;
    for (size_t i = 0; i < 4; ++i) {
      e.add([i, &bats, &threadInit]() {
        threadInit[i].post();
        bats[i].wait();
      });
    }
    for (auto& bat : threadInit) {
      bat.wait();
    }
    for (auto& bat : bats) {
      bat.post();
    }
    auto idsWithKA2 = kWorkerProviderGlobal->collectThreadIds();
    auto& ids1 = idsWithKA1.threadIds;
    auto& ids2 = idsWithKA2.threadIds;
    EXPECT_EQ(ids1.size(), 1);
    EXPECT_EQ(ids2.size(), 4);
  }
  e.join();
}

TEST_F(ExecutorWorkerProviderTest, ThreadCollectorBlocksThreadExitTest) {
  // We need to ensure that the collector's keep alive effectively
  // blocks the executor's threads from exiting. This is done by verifying
  // that a call to reduce the worker count via setNumThreads() does not
  // actually reduce the workers (kills threads) while  the keep alive is
  // in scope.
  constexpr size_t kNumThreads = 4;
  std::array<folly::Baton<>, kNumThreads> bats;
  CPUThreadPoolExecutor e(kNumThreads);
  for (size_t i = 0; i < kNumThreads; ++i) {
    e.add([i, &bats]() { bats[i].wait(); });
  }
  Baton<> baton;
  Baton<> threadCountBaton;
  auto bgCollector = std::thread([&]() {
    {
      auto idsWithKA = kWorkerProviderGlobal->collectThreadIds();
      baton.post();
      // Since this thread is holding the KeepAlive, it should block
      // the main thread's `setNumThreads()` call which is trying to
      // reduce the thread count of the executor. We verify that by
      // checking that the baton isn't posted after a 100ms wait.
      auto posted =
          threadCountBaton.try_wait_for(std::chrono::milliseconds(100));
      EXPECT_FALSE(posted);
      auto& ids = idsWithKA.threadIds;
      // The thread count should still be 4 since the collector's
      // keep alive is active. To further verify that the threads are
      EXPECT_EQ(ids.size(), kNumThreads);
    }
  });
  baton.wait();
  for (auto& bat : bats) {
    bat.post();
  }
  e.setNumThreads(2);
  threadCountBaton.post();
  bgCollector.join();
  // The thread count should now be reduced to 2.
  EXPECT_EQ(e.numThreads(), 2);
  e.join();
}

template <typename TPE>
static void WeakRefTest() {
  // test that adding a .then() after we have
  // started shutting down does not deadlock
  folly::Optional<folly::Future<folly::Unit>> f;
  int counter{0};
  {
    TPE fe(1);
    f = folly::makeFuture()
            .via(&fe)
            .thenValue([](auto&&) { burnMs(100)(); })
            .thenValue([&](auto&&) { ++counter; })
            .via(getWeakRef(fe))
            .thenValue([](auto&&) { burnMs(100)(); })
            .thenValue([&](auto&&) { ++counter; });
  }
  EXPECT_THROW(std::move(*f).get(), folly::BrokenPromise);
  EXPECT_EQ(1, counter);
}

template <typename TPE>
static void virtualExecutorTest() {
  using namespace std::literals;

  folly::Optional<folly::SemiFuture<folly::Unit>> f;
  int counter{0};
  {
    TPE fe(1);
    {
      VirtualExecutor ve(fe);
      f = futures::sleep(100ms)
              .via(&ve)
              .thenValue([&](auto&&) {
                ++counter;
                return futures::sleep(100ms);
              })
              .via(&fe)
              .thenValue([&](auto&&) { ++counter; })
              .semi();
    }
    EXPECT_EQ(1, counter);

    bool functionDestroyed{false};
    bool functionCalled{false};
    {
      VirtualExecutor ve(fe);
      auto guard = makeGuard([&functionDestroyed] {
        std::this_thread::sleep_for(100ms);
        functionDestroyed = true;
      });
      ve.add([&functionCalled, guard = std::move(guard)] {
        functionCalled = true;
      });
    }
    EXPECT_TRUE(functionCalled);
    EXPECT_TRUE(functionDestroyed);
  }
  EXPECT_TRUE(f->isReady());
  EXPECT_NO_THROW(std::move(*f).get());
  EXPECT_EQ(2, counter);
}

class SingleThreadedCPUThreadPoolExecutor
    : public CPUThreadPoolExecutor,
      public SequencedExecutor {
 public:
  explicit SingleThreadedCPUThreadPoolExecutor(size_t)
      : CPUThreadPoolExecutor(1) {}
  ~SingleThreadedCPUThreadPoolExecutor() override { stop(); }
};

TEST(ThreadPoolExecutorTest, CPUThreadPoolExecutorZeroNumPrioritiesThrows) {
  EXPECT_THROW(
      CPUThreadPoolExecutor(1 /* numThreads */, 0 /* numPriorities */),
      std::invalid_argument);
}

TEST(ThreadPoolExecutorTest, CPUThreadPoolExecutorNegativeNumPrioritiesThrows) {
  EXPECT_THROW(
      CPUThreadPoolExecutor(1 /* numThreads */, -1 /* numPriorities */),
      std::invalid_argument);
}

TEST(
    ThreadPoolExecutorTest,
    CPUThreadPoolExecutorZeroNumPrioritiesWithMaxQueueSizeThrows) {
  EXPECT_THROW(
      CPUThreadPoolExecutor(
          1 /* numThreads */, 0 /* numPriorities */, 1 /* maxQueueSize */),
      std::invalid_argument);
}

TEST(
    ThreadPoolExecutorTest,
    CPUThreadPoolExecutorNegativeNumPrioritiesWithMaxQueueSizeThrows) {
  EXPECT_THROW(
      CPUThreadPoolExecutor(
          1 /* numThreads */, -1 /* numPriorities */, 1 /* maxQueueSize */),
      std::invalid_argument);
}

TYPED_TEST(ThreadPoolExecutorTypedTest, WeakRef) {
  WeakRefTest<TypeParam>();
}

TEST(ThreadPoolExecutorTest, WeakRefTestSingleThreadedCPU) {
  WeakRefTest<SingleThreadedCPUThreadPoolExecutor>();
}

TEST(ThreadPoolExecutorTest, WeakRefTestSequential) {
  SingleThreadedCPUThreadPoolExecutor ex(1);
  auto weakRef = getWeakRef(ex);
  EXPECT_TRUE((std::is_same_v<
               decltype(weakRef),
               Executor::KeepAlive<SequencedExecutor>>));
}

TYPED_TEST(ThreadPoolExecutorTypedTest, VirtualExecutor) {
  virtualExecutorTest<TypeParam>();
}

// Test use of guard inside executors
template <class TPE>
static void currentThreadTest(std::string_view executorName) {
  folly::Optional<ExecutorBlockingContext> ctx{};
  TPE tpe(1, std::make_shared<NamedThreadFactory>(executorName));
  tpe.add([&ctx]() { ctx = getExecutorBlockingContext(); });
  tpe.join();
  EXPECT_EQ(ctx->tag, executorName);
}

// Test the nesting of the permit guard
template <class TPE>
static void currentThreadTestDisabled(std::string_view executorName) {
  folly::Optional<ExecutorBlockingContext> ctxPermit{};
  folly::Optional<ExecutorBlockingContext> ctxForbid{};
  TPE tpe(1, std::make_shared<NamedThreadFactory>(executorName));
  tpe.add([&]() {
    {
      // Nest the guard that permits blocking
      ExecutorBlockingGuard guard{ExecutorBlockingGuard::PermitTag{}};
      ctxPermit = getExecutorBlockingContext();
    }
    ctxForbid = getExecutorBlockingContext();
  });
  tpe.join();
  EXPECT_TRUE(!ctxPermit.has_value());
  EXPECT_EQ(ctxForbid->tag, executorName);
}

TYPED_TEST(ThreadPoolExecutorTypedTest, CurrentThreadExecutor) {
  currentThreadTest<TypeParam>("ExecutorName");
  currentThreadTestDisabled<TypeParam>("ExecutorName");
}

TYPED_TEST(ThreadPoolExecutorTypedTest, ProcessedTasksCountStartsAtZero) {
  TypeParam e(4);
  EXPECT_EQ(0, e.getPoolStats().processedTaskCount);
  e.join();
  EXPECT_EQ(0, e.getPoolStats().processedTaskCount);
}

TYPED_TEST(
    ThreadPoolExecutorTypedTest, ProcessedTasksIncrementsAfterCompletion) {
  TypeParam e(1);
  EXPECT_EQ(0, e.getPoolStats().processedTaskCount);

  folly::Baton<> task1;

  e.add([&]() { // 1
    EXPECT_EQ(0, e.getPoolStats().processedTaskCount);
    task1.post();
  });

  task1.wait();

  folly::Baton<> task2;

  e.add([&]() { // 2
    EXPECT_EQ(1, e.getPoolStats().processedTaskCount);
    task2.post();
  });

  task2.wait();

  folly::Baton<> task3;

  e.add([&]() { // 3
    EXPECT_EQ(2, e.getPoolStats().processedTaskCount);
    task3.post();
  });

  task3.wait();

  e.join();

  EXPECT_EQ(3, e.getPoolStats().processedTaskCount);
}

// ThreadFactory wrapper that throws std::runtime_error after N successful
// newThread() calls, simulating pthread_create failure under thread pressure.
class FailingThreadFactory : public ThreadFactory {
 public:
  explicit FailingThreadFactory(
      std::shared_ptr<ThreadFactory> delegate, size_t failAfter)
      : delegate_(std::move(delegate)), failAfter_(failAfter) {}

  std::thread newThread(Func&& func) override {
    if (count_.fetch_add(1) >= failAfter_) {
      throw std::runtime_error("newThread: simulated failure");
    }
    return delegate_->newThread(std::move(func));
  }

  const std::string& getNamePrefix() const override {
    return delegate_->getNamePrefix();
  }

  void reset(size_t failAfter) {
    count_.store(0, std::memory_order_relaxed);
    failAfter_ = failAfter;
  }

 private:
  std::shared_ptr<ThreadFactory> delegate_;
  std::atomic<size_t> count_{0};
  size_t failAfter_;
};

TEST(ThreadPoolExecutorTest, AddImplToleratesThreadCreationFailure) {
  auto delegate = std::make_shared<NamedThreadFactory>("test");
  auto factory = std::make_shared<FailingThreadFactory>(delegate, 1);

  // Dynamic executor: max=4, min=0
  CPUThreadPoolExecutor exe(std::pair(4, 0), std::move(factory));

  // Warm up: create one thread successfully, then make factory fail.
  folly::Baton<> warmup;
  exe.add([&] { warmup.post(); });
  warmup.wait();
  EXPECT_GE(exe.numActiveThreads(), 1);

  // Now the factory will fail for all subsequent newThread() calls.
  // The existing thread should process enqueued tasks without crashing.
  constexpr int kTasks = 10;
  std::atomic<int> completed{0};
  folly::Baton<> done;
  for (int i = 0; i < kTasks; ++i) {
    EXPECT_NO_THROW(exe.add([&] {
      if (completed.fetch_add(1) + 1 == kTasks) {
        done.post();
      }
    }));
  }
  done.wait();
  EXPECT_EQ(kTasks, completed.load());

  exe.join();
}

TEST(ThreadPoolExecutorTest, AddImplNoThreadsAndCreationFails) {
  auto delegate = std::make_shared<NamedThreadFactory>("test");
  // Fail from the very first newThread() call.
  auto factory = std::make_shared<FailingThreadFactory>(delegate, 0);

  // Dynamic executor: max=1, min=0. No threads will be created at startup.
  CPUThreadPoolExecutor exe(std::pair(1, 0), std::move(factory));

  // add() should NOT throw even though thread creation fails.
  EXPECT_NO_THROW(exe.add([] {}));
  EXPECT_EQ(0, exe.numActiveThreads());

  exe.stop();
}

TEST(ThreadPoolExecutorTest, SetNumThreadsNoOpOnFailure_CPU) {
  auto delegate = std::make_shared<NamedThreadFactory>("test");
  auto factory = std::make_shared<FailingThreadFactory>(delegate, 100);

  // Static executor: min == max == 4.
  CPUThreadPoolExecutor exe(std::pair(4, 4), factory);
  EXPECT_EQ(4, exe.numThreads());
  EXPECT_EQ(4, exe.numActiveThreads());

  // An observer forces setNumThreads to create all threads up to the new max.
  auto observer = std::make_shared<TestObserver>();
  exe.addObserver(observer);

  // Make the factory fail for all subsequent calls, then try to grow.
  factory->reset(0);
  EXPECT_THROW(exe.setNumThreads(8), std::runtime_error);

  // Everything should be as if setNumThreads was never called.
  EXPECT_EQ(4, exe.numThreads());
  EXPECT_EQ(4, exe.numActiveThreads());
  EXPECT_EQ(4, exe.getPoolStats().threadCount);

  // The executor should still be functional.
  folly::Baton<> done;
  exe.add([&] { done.post(); });
  done.wait();

  exe.removeObserver(observer);
  exe.join();
}

TEST(ThreadPoolExecutorTest, SetNumThreadsNoOpOnFailure_IO) {
  auto delegate = std::make_shared<NamedThreadFactory>("test");
  auto factory = std::make_shared<FailingThreadFactory>(delegate, 100);

  IOThreadPoolExecutor exe(4, 4, factory);
  EXPECT_EQ(4, exe.numThreads());
  EXPECT_EQ(4, exe.numActiveThreads());

  auto observer = std::make_shared<TestObserver>();
  exe.addObserver(observer);

  factory->reset(0);
  EXPECT_THROW(exe.setNumThreads(8), std::runtime_error);

  EXPECT_EQ(4, exe.numThreads());
  EXPECT_EQ(4, exe.numActiveThreads());
  EXPECT_EQ(4, exe.getPoolStats().threadCount);

  folly::Baton<> done;
  exe.add([&] { done.post(); });
  done.wait();

  exe.removeObserver(observer);
  exe.join();
}

TEST(ThreadPoolExecutorTest, SetNumThreadsNoOpOnPartialFailure_CPU) {
  auto delegate = std::make_shared<NamedThreadFactory>("test");
  // Allow 6 total threads (4 initial + 2 of the 4 new ones).
  auto factory = std::make_shared<FailingThreadFactory>(delegate, 6);

  CPUThreadPoolExecutor exe(std::pair(4, 4), factory);
  EXPECT_EQ(4, exe.numThreads());

  auto observer = std::make_shared<TestObserver>();
  exe.addObserver(observer);

  // Attempt to grow to 8 but only 2 more can be created before failure.
  // The strong guarantee means all or nothing: the 2 that were created
  // should be rolled back.
  EXPECT_THROW(exe.setNumThreads(8), std::runtime_error);

  EXPECT_EQ(4, exe.numThreads());
  EXPECT_EQ(4, exe.numActiveThreads());

  folly::Baton<> done;
  exe.add([&] { done.post(); });
  done.wait();

  exe.removeObserver(observer);
  exe.join();
}
