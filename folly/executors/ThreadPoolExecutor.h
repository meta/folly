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

#include <algorithm>
#include <exception>
#include <mutex>
#include <queue>
#include <span>

#include <glog/logging.h>

#include <folly/DefaultKeepAliveExecutor.h>
#include <folly/Memory.h>
#include <folly/SharedMutex.h>
#include <folly/executors/GlobalThreadPoolList.h>
#include <folly/executors/task_queue/LifoSemMPMCQueue.h>
#include <folly/executors/thread_factory/NamedThreadFactory.h>
#include <folly/io/async/Request.h>
#include <folly/portability/GFlags.h>
#include <folly/synchronization/AtomicStruct.h>
#include <folly/synchronization/Baton.h>
#include <folly/synchronization/RelaxedAtomic.h>

namespace folly {

/* Base class for implementing threadpool based executors.
 *
 * Dynamic thread behavior:
 *
 * ThreadPoolExecutors may vary their actual running number of threads
 * between minThreads_ and maxThreads_, tracked by activeThreads_.
 * The actual implementation of joining an idle thread is left to the
 * ThreadPoolExecutors' subclass (typically by LifoSem try_take_for
 * timing out).  Idle threads should be removed from threadList_, and
 * threadsToJoin incremented, and activeThreads_ decremented.
 *
 * On task add(), if an executor can guarantee there is an active
 * thread that will handle the task, then nothing needs to be done.
 * If not, then ensureActiveThreads() should be called to possibly
 * start another pool thread, up to maxThreads_.
 *
 * ensureJoined() is called on add(), such that we can join idle
 * threads that were destroyed (which can't be joined from
 * themselves).
 *
 * Thread pool stats accounting:
 *
 * Derived classes must register instances to keep stats on all thread
 * pools by calling registerThreadPoolExecutor(this) on constructions
 * and deregisterThreadPoolExecutor(this) on destruction.
 *
 * Registration must be done wherever getPendingTaskCountImpl is implemented
 * and getPendingTaskCountImpl should be marked 'final' to avoid data races.
 */
class ThreadPoolExecutor : public DefaultKeepAliveExecutor {
 public:
  explicit ThreadPoolExecutor(
      size_t maxThreads,
      size_t minThreads,
      std::shared_ptr<ThreadFactory> threadFactory);

  ~ThreadPoolExecutor() override;

  void add(Func func) override = 0;
  /**
   * If func doesn't get started within expiration time after its enqueued,
   * expireCallback will be run
   *
   * @param func  Main function to be executed
   * @param expiration Maximum time to wait for func to start execution
   * @param expireCallback If expiration limit is reached, execute this callback
   */
  virtual void add(
      Func func, std::chrono::milliseconds expiration, Func expireCallback);

  void setThreadFactory(std::shared_ptr<ThreadFactory> threadFactory) {
    CHECK(numThreads() == 0);
    threadFactory_ = std::move(threadFactory);
  }

  std::shared_ptr<ThreadFactory> getThreadFactory() const {
    return threadFactory_;
  }

  size_t numThreads() const;
  void setNumThreads(size_t numThreads);

  // Return actual number of active threads -- this could be different from
  // numThreads() due to ThreadPoolExecutor's dynamic behavior.
  size_t numActiveThreads() const;

  /*
   * stop() is best effort - there is no guarantee that unexecuted tasks won't
   * be executed before it returns. Specifically, IOThreadPoolExecutor's stop()
   * behaves like join().
   */
  virtual void stop();
  virtual void join();

  /**
   * Execute f against all ThreadPoolExecutors, primarily for retrieving and
   * exporting stats.
   */
  static void withAll(FunctionRef<void(ThreadPoolExecutor&)> f);

  /**
   * The subset of PoolStats describing thread occupancy, which is what
   * getPoolStatsLight() reports. Split out so that its callers cannot name the
   * fields it does not compute.
   */
  struct PoolStatsLight {
    PoolStatsLight()
        : threadCount(0),
          idleThreadCount(0),
          activeThreadCount(0),
          pendingTaskCount(0) {}
    size_t threadCount, idleThreadCount, activeThreadCount;
    uint64_t pendingTaskCount;
  };

  struct PoolStats : PoolStatsLight {
    PoolStats() : totalTaskCount(0), processedTaskCount(0), maxIdleTime(0) {}
    uint64_t totalTaskCount, processedTaskCount;
    std::chrono::nanoseconds maxIdleTime;
  };

  PoolStats getPoolStats() const;

  /**
   * Cheaper getPoolStats() for callers that only need thread occupancy.
   *
   * Skipping maxIdleTime is what makes this cheap: it is the only field needing
   * steady_clock::now(), whose vDSO read costs more than the rest of the call
   * put together.
   */
  PoolStatsLight getPoolStatsLight() const;

  size_t getPendingTaskCount() const;
  const std::string& getName() const;

  /**
   * Return the cumulative CPU time used by all threads in the pool, including
   * those that are no longer alive. Requires system support for per-thread CPU
   * clocks. If not available, the function returns 0. This operation can be
   * expensive.
   */
  std::chrono::nanoseconds getUsedCpuTime() const {
    std::shared_lock r{threadListLock_};
    return threadList_.getUsedCpuTime();
  }

  /**
   * Base class for threads created with ThreadPoolExecutor.
   * Some subclasses have methods that operate on these
   * handles.
   */
  class ThreadHandle {
   public:
    virtual ~ThreadHandle() = default;
  };

  /**
   * Observer interface for thread start/stop.
   * Provides hooks so actions can be taken when
   * threads are created
   */
  class Observer {
   public:
    virtual ~Observer() = default;

    virtual void threadStarted(ThreadHandle*) noexcept {}
    virtual void threadStopped(ThreadHandle*) noexcept {}
    virtual void threadPreviouslyStarted(ThreadHandle* h) noexcept {
      threadStarted(h);
    }
    virtual void threadNotYetStopped(ThreadHandle* h) noexcept {
      threadStopped(h);
    }
  };

  virtual void addObserver(std::shared_ptr<Observer>);
  virtual void removeObserver(std::shared_ptr<Observer>);

  struct TaskInfo {
    int8_t priority;
    uint64_t requestId = 0;
    std::chrono::steady_clock::time_point enqueueTime;
    uint64_t taskId;
  };

  struct DequeuedTaskInfo : TaskInfo {
    std::chrono::nanoseconds waitTime{0}; // Dequeue time - enqueueTime.
  };

  struct ProcessedTaskInfo : DequeuedTaskInfo {
    bool expired = false;
    std::chrono::nanoseconds runTime{0};
  };

  class TaskObserver {
   public:
    virtual ~TaskObserver() = default;

    virtual void taskEnqueued(const TaskInfo& /* info */) noexcept {}
    virtual void taskDequeued(const DequeuedTaskInfo& /* info */) noexcept {}
    virtual void taskProcessed(const ProcessedTaskInfo& /* info */) noexcept {}

   private:
    friend class ThreadPoolExecutor;

    TaskObserver* next_ = nullptr;
  };

  // For performance reasons, TaskObservers can be added but not removed. All
  // added observers will be destroyed on executor destruction.
  void addTaskObserver(std::unique_ptr<TaskObserver> taskObserver);

  // TODO(ott): Migrate call sites to the TaskObserver interface.
  using TaskStats = ProcessedTaskInfo;
  using TaskStatsCallback = std::function<void(const TaskStats&)>;
  [[deprecated("Use addTaskObserver()")]] void subscribeToTaskStats(
      TaskStatsCallback cb);

  void setThreadDeathTimeout(std::chrono::milliseconds timeout) {
    threadTimeout_ = timeout;
  }

 protected:
  // Prerequisite: threadListLock_ writelocked
  void addThreads(size_t n);
  // Prerequisite: threadListLock_ writelocked
  bool tryAddOneThread() noexcept;
  // Prerequisite: threadListLock_ writelocked
  void removeThreads(size_t n, bool isJoin);

  struct //
      alignas(folly::cacheline_align_v) //
      alignas(folly::AtomicStruct<std::chrono::steady_clock::time_point>) //
      Thread : public ThreadHandle {
    explicit Thread()
        : id(nextId++),
          handle(),
          idle(true),
          lastActiveTime(std::chrono::steady_clock::now()) {}

    ~Thread() override = default;

    std::chrono::nanoseconds usedCpuTime() const;

    static std::atomic<uint64_t> nextId;
    uint64_t id;

    // Number of tasks processed by this worker.  Reset to zero when
    // the thread stops.
    folly::relaxed_atomic<uint64_t> processedTasks;

    std::thread handle;
    std::atomic<bool> idle;
    folly::AtomicStruct<std::chrono::steady_clock::time_point> lastActiveTime;
    folly::Baton<> initBaton;
    folly::Baton<> readyBaton;
    std::exception_ptr startupException;
    bool cancelledBeforeReady{false};
    // Membership in the owning executor's ThreadList. Written by
    // ThreadList::add/remove under the threadListLock_ write side and read
    // under the read side.
    bool inThreadList{false};
  };

  using ThreadPtr = std::shared_ptr<Thread>;

  // Prerequisite: threadListLock_ writelocked. Returns an initialization
  // exception before publishing any of the new threads.
  std::exception_ptr afterConstructThreads(
      std::span<const ThreadPtr> newThreads) noexcept;

  struct Task {
    struct Expiration {
      std::chrono::milliseconds expiration;
      Func expireCallback;
    };

    Task(
        Func&& func,
        std::shared_ptr<folly::RequestContext> context,
        std::chrono::milliseconds expiration,
        Func&& expireCallback,
        int8_t pri = 0);

    int8_t priority() const { return priority_; }

    Func func_;
    std::chrono::steady_clock::time_point enqueueTime_;
    std::shared_ptr<folly::RequestContext> context_;
    std::unique_ptr<Expiration> expiration_;

   private:
    friend class ThreadPoolExecutor;

    int8_t priority_;
    uint64_t taskId_;
  };

  static void fillTaskInfo(const Task& task, TaskInfo& info);
  void registerTaskEnqueue(const Task& task);
  template <class F>
  void forEachTaskObserver(F&& f) const {
    auto* taskObserver = taskObservers_.load(std::memory_order_acquire);
    while (taskObserver != nullptr) {
      f(*taskObserver);
      taskObserver = taskObserver->next_;
    }
  }

  void runTask(const ThreadPtr& thread, Task&& task);

  // Called by setNumThreads() while holding threadListLock_, before any
  // thread-count state is modified. Subclasses may reject the new value (by
  // throwing) and/or pre-adjust related state atomically with the update. Must
  // not block or re-enter the executor.
  virtual void prepareSetNumThreads(size_t /* numThreads */) {}

  // The function that will be bound to pool threads. It must call
  // thread->initBaton.post() once alive, then thread->readyBaton.wait()
  // followed by a check of thread->cancelledBeforeReady before entering the
  // work loop. If initialization fails, it may instead store the exception in
  // thread->startupException, post thread->initBaton, and return.
  virtual void threadRun(ThreadPtr thread) = 0;

  // Stop n threads and put their ThreadPtrs in the stoppedThreads_ queue
  // and remove them from threadList_, either synchronize or asynchronize
  // Prerequisite: threadListLock_ writelocked
  virtual void stopThreads(size_t n) = 0;

  // Join n stopped threads and remove them from waitingForJoinThreads_ queue.
  // Should not hold a lock because joining thread operation may invoke some
  // cleanup operations on the thread, and those cleanup operations may
  // require a lock on ThreadPoolExecutor.
  void joinStoppedThreads(size_t n) noexcept;

  // To implement shutdown.
  void stopAndJoinAllThreads(bool isJoin);

  // Create a suitable Thread struct
  virtual ThreadPtr makeThread() { return std::make_shared<Thread>(); }

  static void registerThreadPoolExecutor(ThreadPoolExecutor* tpe);
  static void deregisterThreadPoolExecutor(ThreadPoolExecutor* tpe);

  // Destroy all task observers. Must be called from derived class destructors
  // after stop(), while the derived class vtable is still valid. This ensures
  // observer cleanup (e.g., fb303 callback unregistration) happens before the
  // vtable reverts to the base class.
  void destroyTaskObservers();

  // Prerequisite: threadListLock_ readlocked or writelocked
  virtual size_t getPendingTaskCountImpl() const = 0;

  // Called with threadListLock_ readlocked or writelocked.
  virtual void handleObserverRegisterThread(ThreadHandle*, Observer&) noexcept {
  }
  virtual void handleObserverUnregisterThread(
      ThreadHandle*, Observer&) noexcept {}

  class ThreadList {
   public:
    void add(const ThreadPtr& state) {
      auto it = std::lower_bound(vec_.begin(), vec_.end(), state, Compare{});
      vec_.insert(it, state);
      state->inThreadList = true;
    }

    void remove(const ThreadPtr& state) {
      auto itPair =
          std::equal_range(vec_.begin(), vec_.end(), state, Compare{});
      CHECK(itPair.first != vec_.end());
      CHECK(std::next(itPair.first) == itPair.second);
      vec_.erase(itPair.first);
      state->inThreadList = false;
      pastCpuUsed_ += state->usedCpuTime();
    }

    const std::vector<ThreadPtr>& get() const { return vec_; }

    std::chrono::nanoseconds getUsedCpuTime() const {
      auto acc{pastCpuUsed_};
      for (const auto& thread : vec_) {
        acc += thread->usedCpuTime();
      }
      return acc;
    }

   private:
    struct Compare {
      bool operator()(const ThreadPtr& ts1, const ThreadPtr& ts2) const {
        return ts1->id < ts2->id;
      }
    };
    std::vector<ThreadPtr> vec_;
    // cpu time used by threads that are no longer alive
    std::chrono::nanoseconds pastCpuUsed_{0};
  };

  class StoppedThreadQueue : public BlockingQueue<ThreadPtr> {
   public:
    BlockingQueueAddResult add(ThreadPtr&& item) override;
    ThreadPtr take() override;
    size_t size() override;
    folly::Optional<ThreadPtr> try_take_for(
        std::chrono::milliseconds /*timeout */) override;

   private:
    folly::LifoSem sem_;
    std::mutex mutex_;
    std::queue<ThreadPtr> queue_;
  };

  std::shared_ptr<ThreadFactory> threadFactory_;

  ThreadList threadList_;
  mutable SharedMutex threadListLock_;
  StoppedThreadQueue stoppedThreads_;
  std::atomic<bool> isJoin_{false}; // whether the current downsizing is a join

  std::vector<std::shared_ptr<Observer>> observers_;
  folly::ThreadPoolListHook threadPoolHook_;

  // Dynamic thread sizing functions and variables
  void ensureMaxActiveThreads();
  void ensureActiveThreads() noexcept;
  void ensureJoined() noexcept;
  bool minActive();
  bool tryTimeoutThread();

  // These are only modified while holding threadListLock_, but
  // are read without holding the lock.
  std::atomic<size_t> maxThreads_{0};
  std::atomic<size_t> minThreads_{0};
  std::atomic<size_t> activeThreads_{0};
  std::atomic<bool> threadsCanTimeout_{true};

  std::atomic<size_t> threadsToJoin_{0};
  std::atomic<std::chrono::milliseconds> threadTimeout_;

  // Number of tasks processed by stopped or joined threads.  Updated
  // when a thread stops, which preceeds joining.  Requires holding
  // the threadListLock_.
  uint64_t stoppedThreadProcessedTasks_{0};

  bool joinKeepAliveOnce() {
    if (!std::exchange(keepAliveJoined_, true)) {
      joinKeepAlive();
      return true;
    }
    return false;
  }

  bool keepAliveJoined_{false};

 private:
  // Stats is the return type, and selects the field subset: it is a template
  // parameter rather than an argument so that each variant compiles to a loop
  // carrying only the work it needs.
  template <typename Stats>
  Stats getPoolStatsImpl() const;

  std::atomic<TaskObserver*> taskObservers_{nullptr};
};

} // namespace folly
