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

#include <folly/executors/ThreadPoolExecutor.h>

#include <ctime>
#include <span>
#include <type_traits>
#include <vector>

#include <folly/ExceptionString.h>
#include <folly/GLog.h>
#include <folly/concurrency/ProcessLocalUniqueId.h>
#include <folly/executors/GlobalThreadPoolList.h>
#include <folly/portability/PThread.h>
#include <folly/synchronization/AsymmetricThreadFence.h>
#include <folly/tracing/StaticTracepoint.h>

namespace folly {

using SyncVecThreadPoolExecutors =
    folly::Synchronized<std::vector<ThreadPoolExecutor*>>;

SyncVecThreadPoolExecutors& getSyncVecThreadPoolExecutors() {
  static Indestructible<SyncVecThreadPoolExecutors> storage;
  return *storage;
}

void ThreadPoolExecutor::registerThreadPoolExecutor(ThreadPoolExecutor* tpe) {
  getSyncVecThreadPoolExecutors().wlock()->push_back(tpe);
}

void ThreadPoolExecutor::deregisterThreadPoolExecutor(ThreadPoolExecutor* tpe) {
  getSyncVecThreadPoolExecutors().withWLock([tpe](auto& tpes) {
    std::erase(tpes, tpe);
  });
}

FOLLY_GFLAGS_DEFINE_int64(
    threadtimeout_ms,
    60000,
    "Idle time before ThreadPoolExecutor threads are joined");

ThreadPoolExecutor::ThreadPoolExecutor(
    size_t /* maxThreads */,
    size_t minThreads,
    std::shared_ptr<ThreadFactory> threadFactory)
    : threadFactory_(std::move(threadFactory)),
      threadPoolHook_("folly::ThreadPoolExecutor"),
      minThreads_(minThreads) {
  threadTimeout_ = std::chrono::milliseconds(FLAGS_threadtimeout_ms);
}

ThreadPoolExecutor::~ThreadPoolExecutor() {
  joinKeepAliveOnce();
  CHECK_EQ(0, threadList_.get().size());
  // Derived class destructors must call destroyTaskObservers() after stop().
  // This ensures observer cleanup (e.g., fb303 callback unregistration) runs
  // while the derived vtable is still valid, preventing pure virtual calls.
  CHECK_EQ(taskObservers_.load(), nullptr)
      << "ThreadPoolExecutor subclasses should call destroyTaskObservers() in their destructor";
}

void ThreadPoolExecutor::destroyTaskObservers() {
  auto* taskObserver =
      taskObservers_.exchange(nullptr, std::memory_order_acquire);
  while (taskObserver != nullptr) {
    delete std::exchange(taskObserver, taskObserver->next_);
  }
}

ThreadPoolExecutor::Task::Task(
    Func&& func,
    std::shared_ptr<folly::RequestContext> context,
    std::chrono::milliseconds expiration,
    Func&& expireCallback,
    int8_t pri)
    : func_(std::move(func)),
      // Assume that the task in enqueued on creation
      enqueueTime_(std::chrono::steady_clock::now()),
      context_(std::move(context)),
      priority_(pri),
      taskId_(processLocalUniqueId()) {
  if (expiration > std::chrono::milliseconds::zero()) {
    expiration_ = std::make_unique<Expiration>();
    expiration_->expiration = expiration;
    expiration_->expireCallback = std::move(expireCallback);
  }
}

/* static */ void ThreadPoolExecutor::fillTaskInfo(
    const Task& task, TaskInfo& info) {
  info.priority = task.priority();
  if (task.context_) {
    info.requestId = task.context_->getRootId();
  }
  info.enqueueTime = task.enqueueTime_;
  info.taskId = task.taskId_;
}

void ThreadPoolExecutor::registerTaskEnqueue(const Task& task) {
  TaskInfo info;
  fillTaskInfo(task, info);
  forEachTaskObserver([&](auto& observer) { observer.taskEnqueued(info); });
  FOLLY_SDT(
      folly,
      thread_pool_executor_task_enqueued,
      threadFactory_->getNamePrefix().c_str(),
      info.requestId,
      info.enqueueTime.time_since_epoch().count(),
      info.taskId);
}

void ThreadPoolExecutor::runTask(const ThreadPtr& thread, Task&& task) {
  thread->idle.store(false, std::memory_order_relaxed);

  auto startTime = std::chrono::steady_clock::now();
  ProcessedTaskInfo taskInfo;
  fillTaskInfo(task, taskInfo);
  taskInfo.waitTime = startTime - task.enqueueTime_;

  {
    folly::RequestContextScopeGuard rctx(std::move(task.context_));
    // taskDequeued() and taskProcessed() run inside this scope so observers see
    // the task's RequestContext -- the same context task.func_ runs under --
    // letting them associate executor accounting (e.g. queue wait) with the
    // request. (taskEnqueued() already runs under the request context: it fires
    // synchronously in add() on the submitting thread.)
    forEachTaskObserver([&](auto& observer) {
      observer.taskDequeued(taskInfo);
    });
    FOLLY_SDT(
        folly,
        thread_pool_executor_task_dequeued,
        threadFactory_->getNamePrefix().c_str(),
        taskInfo.requestId,
        taskInfo.enqueueTime.time_since_epoch().count(),
        taskInfo.waitTime.count(),
        taskInfo.taskId);

    if (task.expiration_ != nullptr &&
        taskInfo.waitTime >= task.expiration_->expiration) {
      task.func_ = nullptr;
      taskInfo.expired = true;
      if (task.expiration_->expireCallback != nullptr) {
        invokeCatchingExns(
            "ThreadPoolExecutor: expireCallback",
            std::exchange(task.expiration_->expireCallback, {}));
      }
    } else {
      invokeCatchingExns(
          "ThreadPoolExecutor: func", std::exchange(task.func_, {}));
      if (task.expiration_ != nullptr) {
        task.expiration_->expireCallback = nullptr;
      }
    }
    if (!taskInfo.expired) {
      taskInfo.runTime = std::chrono::steady_clock::now() - startTime;
    }

    // Times in this USDT use granularity of
    // std::chrono::steady_clock::duration, which is platform dependent. On
    // Facebook servers, the granularity is nanoseconds. We explicitly do not
    // perform any unit conversions to avoid unnecessary costs and leave it to
    // consumers of this data to know what effective clock resolution is.
    FOLLY_SDT(
        folly,
        thread_pool_executor_task_stats,
        threadFactory_->getNamePrefix().c_str(),
        taskInfo.requestId,
        taskInfo.enqueueTime.time_since_epoch().count(),
        taskInfo.waitTime.count(),
        taskInfo.runTime.count(),
        taskInfo.taskId);
    forEachTaskObserver([&](auto& observer) {
      observer.taskProcessed(taskInfo);
    });
  }

  thread->processedTasks = thread->processedTasks + 1;

  thread->idle.store(true, std::memory_order_relaxed);
  thread->lastActiveTime.store(
      std::chrono::steady_clock::now(), std::memory_order_relaxed);
}

void ThreadPoolExecutor::add(Func, std::chrono::milliseconds, Func) {
  throw std::runtime_error(
      "add() with expiration is not implemented for this Executor");
}

size_t ThreadPoolExecutor::numThreads() const {
  return maxThreads_.load(std::memory_order_relaxed);
}

size_t ThreadPoolExecutor::numActiveThreads() const {
  return activeThreads_.load(std::memory_order_relaxed);
}

// Set the maximum number of running threads.
void ThreadPoolExecutor::setNumThreads(size_t numThreads) {
  /* Since ThreadPoolExecutor may be dynamically adjusting the number of
     threads, we adjust the relevant variables instead of changing
     the number of threads directly.  Roughly:

     If numThreads < minthreads reset minThreads to numThreads.

     If numThreads < active threads, reduce number of running threads.

     If the number of pending tasks is > 0, then increase the currently
     active number of threads such that we can run all the tasks, or reach
     numThreads.

     Note that if there are observers, we actually have to create all
     the threads, because some observer implementations need to 'observe'
     all thread creation (see tests for an example of this)
  */

  size_t numThreadsToJoin = 0;
  {
    std::unique_lock w{threadListLock_};
    prepareSetNumThreads(numThreads);
    auto pending = getPendingTaskCountImpl();
    auto active = activeThreads_.load(std::memory_order_relaxed);

    // Save original values for rollback in case addThreads throws.
    auto origMaxThreads = maxThreads_.load(std::memory_order_relaxed);
    auto origMinThreads = minThreads_.load(std::memory_order_relaxed);
    auto origThreadsCanTimeout =
        threadsCanTimeout_.load(std::memory_order_relaxed);

    maxThreads_.store(numThreads, std::memory_order_relaxed);
    auto minthreads = origMinThreads;
    if (numThreads < minthreads) {
      minthreads = numThreads;
      minThreads_.store(numThreads, std::memory_order_relaxed);
    }
    // threadsCanTimeout_ can only transition from false to true if maxThreads_
    // is increased (in which case minThreads_ will stay the same). So even if
    // the current threads cannot timeout until the next wakeup, all the new
    // threads can timeout, thus we can get down to minThreads_ active threads.
    threadsCanTimeout_.store(minthreads != numThreads);
    if (active > numThreads) {
      numThreadsToJoin = active - numThreads;
      assert(numThreadsToJoin <= active - minthreads);
      ThreadPoolExecutor::removeThreads(numThreadsToJoin, false);
      activeThreads_.store(numThreads, std::memory_order_relaxed);
    } else if (pending > 0 || !observers_.empty() || active < minthreads) {
      size_t numToAdd = std::min(pending, numThreads - active);
      if (!observers_.empty()) {
        numToAdd = numThreads - active;
      }
      if (active + numToAdd < minthreads) {
        numToAdd = minthreads - active;
      }
      auto rollback = makeGuard([&] {
        maxThreads_.store(origMaxThreads, std::memory_order_relaxed);
        minThreads_.store(origMinThreads, std::memory_order_relaxed);
        threadsCanTimeout_.store(
            origThreadsCanTimeout, std::memory_order_relaxed);
      });
      ThreadPoolExecutor::addThreads(numToAdd);
      rollback.dismiss();
      activeThreads_.store(active + numToAdd, std::memory_order_relaxed);
    }
  }

  /* We may have removed some threads, attempt to join them */
  joinStoppedThreads(numThreadsToJoin);
}

// threadListLock_ is writelocked
void ThreadPoolExecutor::addThreads(size_t n) {
  std::vector<ThreadPtr> newThreads;
  for (size_t i = 0; i < n; i++) {
    newThreads.push_back(makeThread());
  }
  size_t created = 0;
  bool initialized = false;
  auto rollback = makeGuard([&] {
    for (size_t i = 0; i < created; i++) {
      if (!initialized) {
        newThreads[i]->initBaton.wait(
            folly::Baton<>::wait_options().logging_enabled(false));
      }
      newThreads[i]->cancelledBeforeReady = true;
      newThreads[i]->readyBaton.post();
    }
    for (size_t i = 0; i < created; i++) {
      newThreads[i]->handle.join();
    }
  });
  for (auto& thread : newThreads) {
    thread->handle = threadFactory_->newThread([this, thread] {
      threadRun(thread);
    });
    ++created;
  }
  auto startupException = afterConstructThreads(newThreads);
  initialized = true;
  if (startupException) {
    std::rethrow_exception(startupException);
  }
  rollback.dismiss();
}

// threadListLock_ is writelocked
bool ThreadPoolExecutor::tryAddOneThread() noexcept {
  auto thread = makeThread();
  try {
    thread->handle = threadFactory_->newThread([this, thread] {
      threadRun(thread);
    });
  } catch (...) {
    FB_LOG_EVERY_MS(ERROR, 1000)
        << "ThreadPoolExecutor: thread creation failed: "
        << folly::exceptionStr(std::current_exception());
    return false;
  }
  auto rollback = makeGuard([&] {
    thread->cancelledBeforeReady = true;
    thread->readyBaton.post();
    thread->handle.join();
  });
  if (auto ex = afterConstructThreads({&thread, 1})) {
    FB_LOG_EVERY_MS(ERROR, 1000)
        << "ThreadPoolExecutor: thread startup failed: "
        << folly::exceptionStr(ex);
    return false;
  }
  rollback.dismiss();
  return true;
}

// threadListLock_ is writelocked
std::exception_ptr ThreadPoolExecutor::afterConstructThreads(
    std::span<const ThreadPtr> newThreads) noexcept {
  for (auto& thread : newThreads) {
    thread->initBaton.wait(
        folly::Baton<>::wait_options().logging_enabled(false));
  }
  for (auto& thread : newThreads) {
    if (thread->startupException) {
      return thread->startupException;
    }
  }
  for (auto& thread : newThreads) {
    threadList_.add(thread);
  }
  for (auto& o : observers_) {
    for (auto& thread : newThreads) {
      o->threadStarted(thread.get());
      handleObserverRegisterThread(thread.get(), *o);
    }
  }
  for (auto& thread : newThreads) {
    thread->readyBaton.post();
  }
  return nullptr;
}

// threadListLock_ is writelocked
void ThreadPoolExecutor::removeThreads(size_t n, bool isJoin) {
  // Block early thread stopping.
  isJoin_.store(isJoin, std::memory_order_release);
  stopThreads(n);
}

void ThreadPoolExecutor::joinStoppedThreads(size_t n) noexcept {
  for (size_t i = 0; i < n; i++) {
    auto thread = stoppedThreads_.take();
    thread->handle.join();
  }
}

void ThreadPoolExecutor::stopAndJoinAllThreads(bool isJoin) {
  size_t n = 0;
  {
    std::unique_lock w{threadListLock_};
    maxThreads_.store(0, std::memory_order_release);
    activeThreads_.store(0, std::memory_order_release);
    n = threadList_.get().size();
    removeThreads(n, isJoin);
    n += threadsToJoin_.load(std::memory_order_relaxed);
    threadsToJoin_.store(0, std::memory_order_relaxed);
  }
  joinStoppedThreads(n);
  CHECK_EQ(0, threadList_.get().size());
  CHECK_EQ(0, stoppedThreads_.size());
}

void ThreadPoolExecutor::stop() {
  joinKeepAliveOnce();
  stopAndJoinAllThreads(/* isJoin */ false);
}

void ThreadPoolExecutor::join() {
  joinKeepAliveOnce();
  stopAndJoinAllThreads(/* isJoin */ true);
}

void ThreadPoolExecutor::withAll(FunctionRef<void(ThreadPoolExecutor&)> f) {
  getSyncVecThreadPoolExecutors().withRLock([f](auto& tpes) {
    for (auto tpe : tpes) {
      f(*tpe);
    }
  });
}

template <typename Stats>
Stats ThreadPoolExecutor::getPoolStatsImpl() const {
  constexpr bool kLight = std::is_same_v<Stats, PoolStatsLight>;
  const auto now = kLight
      ? std::chrono::steady_clock::time_point{}
      : std::chrono::steady_clock::now();
  std::shared_lock r{threadListLock_};
  Stats stats;
  const auto& threads = threadList_.get();
  size_t idleAlive = 0;
  [[maybe_unused]] uint64_t processedTasks =
      kLight ? 0 : stoppedThreadProcessedTasks_;
  // Maximizing (now - lastActiveTime) over the idle workers is the same as
  // minimizing lastActiveTime over them, which keeps the subtraction out of
  // the loop. Seeding with now also supplies the identity for busy workers and
  // clamps the result at zero, matching a worker that went idle after now was
  // sampled.
  [[maybe_unused]] auto minLastActive = now;
  for (const auto& thread : threads) {
    const bool idle = thread->idle.load(std::memory_order_relaxed);
    idleAlive += idle ? 1 : 0;
    if constexpr (!kLight) {
      // lastActiveTime is read unconditionally so that the whole body is
      // branch-free: whether a worker is idle is close to random at any
      // instant, so a branch here mispredicts on roughly a fifth of
      // iterations. The read is always safe - threadListLock_ keeps the thread
      // alive - and lands on a cacheline the idle load already pulled in.
      const auto lastActive =
          thread->lastActiveTime.load(std::memory_order_relaxed);
      minLastActive = std::min(minLastActive, idle ? lastActive : now);
      processedTasks += thread->processedTasks;
    }
  }
  stats.pendingTaskCount = getPendingTaskCountImpl();
  if constexpr (!kLight) {
    const size_t activeTasks = threads.size() - idleAlive;
    stats.maxIdleTime = now - minLastActive;
    stats.totalTaskCount = stats.pendingTaskCount + activeTasks;
    stats.processedTaskCount = processedTasks;
  }

  stats.threadCount = maxThreads_.load(std::memory_order_relaxed);
  stats.activeThreadCount =
      activeThreads_.load(std::memory_order_relaxed) - idleAlive;
  stats.idleThreadCount = stats.threadCount - stats.activeThreadCount;
  return stats;
}

ThreadPoolExecutor::PoolStats ThreadPoolExecutor::getPoolStats() const {
  return getPoolStatsImpl<PoolStats>();
}

ThreadPoolExecutor::PoolStatsLight ThreadPoolExecutor::getPoolStatsLight()
    const {
  return getPoolStatsImpl<PoolStatsLight>();
}

size_t ThreadPoolExecutor::getPendingTaskCount() const {
  std::shared_lock r{threadListLock_};
  return getPendingTaskCountImpl();
}

const std::string& ThreadPoolExecutor::getName() const {
  return threadFactory_->getNamePrefix();
}

std::atomic<uint64_t> ThreadPoolExecutor::Thread::nextId(0);

std::chrono::nanoseconds ThreadPoolExecutor::Thread::usedCpuTime() const {
  using std::chrono::nanoseconds;
  using std::chrono::seconds;
  timespec tp{};
#ifdef __linux__
  clockid_t clockid;
  auto th = const_cast<std::thread&>(handle).native_handle();
  if (!pthread_getcpuclockid(th, &clockid)) {
    clock_gettime(clockid, &tp);
  }
#endif
  return nanoseconds(tp.tv_nsec) + seconds(tp.tv_sec);
}

void ThreadPoolExecutor::subscribeToTaskStats(TaskStatsCallback cb) {
  class TaskStatsCallbackObserver : public TaskObserver {
   public:
    explicit TaskStatsCallbackObserver(TaskStatsCallback&& cob)
        : cob_(std::move(cob)) {}

    void taskProcessed(const ProcessedTaskInfo& info) noexcept override {
      invokeCatchingExns("ThreadPoolExecutor: task stats callback", [&] {
        cob_(info);
      });
    }

   private:
    TaskStatsCallback cob_;
  };

  addTaskObserver(std::make_unique<TaskStatsCallbackObserver>(std::move(cb)));
}

void ThreadPoolExecutor::addTaskObserver(
    std::unique_ptr<TaskObserver> taskObserver) {
  auto taskObserverPtr = taskObserver.release();
  auto* head = taskObservers_.load(std::memory_order_relaxed);
  do {
    taskObserverPtr->next_ = head;
  } while (!taskObservers_.compare_exchange_weak(
      head,
      taskObserverPtr,
      std::memory_order_acq_rel,
      std::memory_order_relaxed));
}

BlockingQueueAddResult ThreadPoolExecutor::StoppedThreadQueue::add(
    ThreadPoolExecutor::ThreadPtr&& item) {
  std::lock_guard guard(mutex_);
  queue_.push(std::move(item));
  return sem_.post();
}

ThreadPoolExecutor::ThreadPtr ThreadPoolExecutor::StoppedThreadQueue::take() {
  while (true) {
    {
      std::lock_guard guard(mutex_);
      if (!queue_.empty()) {
        auto item = std::move(queue_.front());
        queue_.pop();
        return item;
      }
    }
    sem_.wait();
  }
}

folly::Optional<ThreadPoolExecutor::ThreadPtr>
ThreadPoolExecutor::StoppedThreadQueue::try_take_for(
    std::chrono::milliseconds time) {
  while (true) {
    {
      std::lock_guard guard(mutex_);
      if (!queue_.empty()) {
        auto item = std::move(queue_.front());
        queue_.pop();
        return item;
      }
    }
    if (!sem_.try_wait_for(time)) {
      return folly::none;
    }
  }
}

size_t ThreadPoolExecutor::StoppedThreadQueue::size() {
  std::lock_guard guard(mutex_);
  return queue_.size();
}

void ThreadPoolExecutor::addObserver(std::shared_ptr<Observer> o) {
  {
    std::unique_lock r{threadListLock_};
    observers_.push_back(o);
    for (auto& thread : threadList_.get()) {
      o->threadPreviouslyStarted(thread.get());
      handleObserverRegisterThread(thread.get(), *o);
    }
  }
  ensureMaxActiveThreads();
}

void ThreadPoolExecutor::removeObserver(std::shared_ptr<Observer> o) {
  std::unique_lock r{threadListLock_};
  for (auto& thread : threadList_.get()) {
    o->threadNotYetStopped(thread.get());
    handleObserverUnregisterThread(thread.get(), *o);
  }

  for (auto it = observers_.begin(); it != observers_.end(); it++) {
    if (*it == o) {
      observers_.erase(it);
      return;
    }
  }
  DCHECK(false);
}

// Idle threads may have destroyed themselves, attempt to join
// them here
void ThreadPoolExecutor::ensureJoined() noexcept {
  auto tojoin = threadsToJoin_.load(std::memory_order_relaxed);
  if (tojoin) {
    {
      std::unique_lock w{threadListLock_};
      tojoin = threadsToJoin_.load(std::memory_order_relaxed);
      threadsToJoin_.store(0, std::memory_order_relaxed);
    }
    joinStoppedThreads(tojoin);
  }
}

// threadListLock_ must be write locked.
bool ThreadPoolExecutor::tryTimeoutThread() {
  // Try to stop based on idle thread timeout (try_take_for),
  // if there are at least minThreads running.
  if (!minActive()) {
    return false;
  }

  // Remove thread from active count
  activeThreads_.store(
      activeThreads_.load(std::memory_order_relaxed) - 1,
      std::memory_order_relaxed);

  // There is a memory ordering constraint w.r.t the queue
  // implementation's add() and getPendingTaskCountImpl() - while many
  // queues have seq_cst ordering, some do not, so add an explicit
  // barrier.  tryTimeoutThread is the slow path and only happens once
  // every thread timeout; use asymmetric barrier to keep add() fast.
  asymmetric_thread_fence_heavy(std::memory_order_seq_cst);

  // If this is based on idle thread timeout, then
  // adjust vars appropriately (otherwise stop() or join()
  // does this).
  if (getPendingTaskCountImpl() > 0) {
    // There are still pending tasks, we can't stop yet.
    // re-up active threads and return.
    activeThreads_.store(
        activeThreads_.load(std::memory_order_relaxed) + 1,
        std::memory_order_relaxed);
    return false;
  }

  threadsToJoin_.store(
      threadsToJoin_.load(std::memory_order_relaxed) + 1,
      std::memory_order_relaxed);

  return true;
}

// If we can't ensure that we were able to hand off a task to a thread,
// attempt to start a thread that handled the task, if we aren't already
// running the maximum number of threads.
void ThreadPoolExecutor::ensureActiveThreads() noexcept {
  ensureJoined();

  // Matches barrier in tryTimeoutThread().  Ensure task added
  // is seen before loading activeThreads_ below.
  asymmetric_thread_fence_light(std::memory_order_seq_cst);

  // Fast path assuming we are already at max threads.
  auto active = activeThreads_.load(std::memory_order_relaxed);
  auto total = maxThreads_.load(std::memory_order_relaxed);

  if (active >= total) {
    return;
  }

  std::unique_lock w{threadListLock_};
  // Double check behind lock.
  active = activeThreads_.load(std::memory_order_relaxed);
  total = maxThreads_.load(std::memory_order_relaxed);
  if (active >= total) {
    return;
  }
  if (tryAddOneThread()) {
    activeThreads_.store(active + 1, std::memory_order_relaxed);
  }
}

void ThreadPoolExecutor::ensureMaxActiveThreads() {
  ensureJoined();
  asymmetric_thread_fence_light(std::memory_order_seq_cst);
  auto active = activeThreads_.load(std::memory_order_relaxed);
  auto total = maxThreads_.load(std::memory_order_relaxed);
  if (active >= total) {
    return;
  }
  std::unique_lock w{threadListLock_};
  active = activeThreads_.load(std::memory_order_relaxed);
  total = maxThreads_.load(std::memory_order_relaxed);
  if (active >= total) {
    return;
  }
  // A startup failure rolls back every unpublished thread before propagating,
  // leaving activeThreads_ unchanged so a later call can retry.
  ThreadPoolExecutor::addThreads(total - active);
  activeThreads_.store(total, std::memory_order_relaxed);
}

// If an idle thread times out, only join it if there are at least
// minThreads threads.
bool ThreadPoolExecutor::minActive() {
  return activeThreads_.load(std::memory_order_relaxed) >
      minThreads_.load(std::memory_order_relaxed);
}

} // namespace folly
