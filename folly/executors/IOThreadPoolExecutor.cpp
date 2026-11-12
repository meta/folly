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

#include <folly/executors/IOThreadPoolExecutor.h>

#include <glog/logging.h>

#include <folly/detail/MemoryIdler.h>
#include <folly/portability/GFlags.h>
#include <folly/system/ThreadId.h>

FOLLY_GFLAGS_DEFINE_bool(
    dynamic_iothreadpoolexecutor,
    true,
    "IOThreadPoolExecutor will dynamically create threads");

FOLLY_GFLAGS_DEFINE_int32(
    folly_iothreadpoolexecutor_max_read_at_once,
    -1,
    "IOThreadPoolExecutor will use this value as default for maxReadAtOnce in its event bases, valid values are [0, inf)");

namespace folly {

namespace {

using folly::detail::MemoryIdler;

/* Class that will free jemalloc caches and madvise the stack away
 * if the event loop is unused for some period of time
 */
class MemoryIdlerTimeout : public AsyncTimeout, public EventBase::LoopCallback {
 public:
  explicit MemoryIdlerTimeout(EventBase* b) : AsyncTimeout(b), base_(b) {}

  void timeoutExpired() noexcept override {
    idled_ = true;
    timerRunning_ = false;
  }

  void runLoopCallback() noexcept override {
    if (idled_) {
      if (num_ == 0) {
        MemoryIdler::flushLocalMallocCaches();
        MemoryIdler::unmapUnusedStack(MemoryIdler::kDefaultStackToRetain);
      }

      idled_ = false;
      num_ = 0;
    } else {
      if (!timerRunning_) {
        timerRunning_ = true;
        std::chrono::steady_clock::duration idleTimeout =
            MemoryIdler::defaultIdleTimeout.load(std::memory_order_acquire);

        idleTimeout = MemoryIdler::getVariationTimeout(idleTimeout);

        scheduleTimeout(
            static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    idleTimeout)
                    .count()));
      } else {
        num_++;
      }
    }

    // reschedule this callback for the next event loop.
    base_->runBeforeLoop(this);
  }

 private:
  EventBase* base_;
  bool idled_{false};
  bool timerRunning_{false};
  size_t num_{0};
};

} // namespace

// IOThreadPoolExecutorBase
EventBase* IOThreadPoolExecutor::getEventBase(
    ThreadPoolExecutor::ThreadHandle* h) {
  auto thread = dynamic_cast<IOThread*>(h);

  if (thread) {
    return thread->eventBase;
  }

  return nullptr;
}

// IOThreadPoolExecutor
IOThreadPoolExecutor::IOThreadPoolExecutor(
    size_t numThreads,
    std::shared_ptr<ThreadFactory> threadFactory,
    EventBaseManager* ebm,
    Options options)
    : IOThreadPoolExecutor(
          numThreads,
          FLAGS_dynamic_iothreadpoolexecutor ? 0 : numThreads,
          std::move(threadFactory),
          ebm,
          std::move(options)) {}

IOThreadPoolExecutor::IOThreadPoolExecutor(
    size_t maxThreads,
    size_t minThreads,
    std::shared_ptr<ThreadFactory> threadFactory,
    EventBaseManager* ebm,
    Options options)
    : IOThreadPoolExecutorBase(
          maxThreads, minThreads, std::move(threadFactory)),
      isWaitForAll_(options.waitForAll),
      nextThread_(0),
      eventBaseManager_(ebm),
      maxReadAtOnce_(options.maxReadAtOnce) {
  setNumThreads(maxThreads);
  registerThreadPoolExecutor(this);
  if (options.enableThreadIdCollection) {
    threadIdCollector_ = std::make_unique<ThreadIdWorkerProvider>();
  }
}

IOThreadPoolExecutor::~IOThreadPoolExecutor() {
  deregisterThreadPoolExecutor(this);
  stop();
  destroyTaskObservers();
}

void IOThreadPoolExecutor::add(Func func) {
  add(std::move(func), std::chrono::milliseconds(0));
}

void IOThreadPoolExecutor::add(
    Func func, std::chrono::milliseconds expiration, Func expireCallback) {
  ensureActiveThreads();
  std::shared_lock r{threadListLock_};
  if (threadList_.get().empty()) {
    throw std::runtime_error("No threads available");
  }
  auto ioThread = pickThread();

  auto task = Task(
      std::move(func),
      folly::RequestContext::saveContext(),
      expiration,
      std::move(expireCallback));
  registerTaskEnqueue(task);
  auto wrappedFunc = [this, ioThread, task = std::move(task)]() mutable {
    runTask(ioThread, std::move(task));
    ioThread->pendingTasks--;
  };

  ioThread->pendingTasks++;
  ioThread->eventBase->runInEventBaseThread(std::move(wrappedFunc));
}

std::shared_ptr<IOThreadPoolExecutor::IOThread>
IOThreadPoolExecutor::pickThread() {
  auto& me = *thisThread_;
  auto& ths = threadList_.get();
  // When new task is added to IOThreadPoolExecutor, a thread is chosen for it
  // to be executed on, thisThread_ is by default chosen, however, if the new
  // task is added by the clean up operations on thread destruction, thisThread_
  // is not an available thread anymore, thus, always check whether or not
  // thisThread_ is an available thread before choosing it.
  if (me && me->inThreadList) {
    return me;
  }
  auto n = ths.size();
  if (n == 0) {
    // XXX I think the only way this can happen is if somebody calls
    // getEventBase (1) from one of the executor's threads while the executor
    // is stopping or getting downsized to zero or (2) from outside the executor
    // when it has no threads. In the first case, it's not obvious what the
    // correct behavior should be-- do we really want to return ourselves even
    // though we're about to exit? (The comment above seems to imply no.) In
    // the second case, `!me` so we'll crash anyway.
    return me;
  }
  auto thread = ths[nextThread_++ % n];
  return std::static_pointer_cast<IOThread>(thread);
}

EventBase* IOThreadPoolExecutor::getEventBase() {
  ensureActiveThreads();
  std::shared_lock r{threadListLock_};
  if (threadList_.get().empty()) {
    throw std::runtime_error("No threads available");
  }
  return pickThread()->eventBase;
}

Executor::KeepAlive<EventBase> IOThreadPoolExecutor::getEventBaseKeepAlive() {
  ensureActiveThreads();
  std::shared_lock r{threadListLock_};
  if (threadList_.get().empty()) {
    throw std::runtime_error("No threads available");
  }
  return getKeepAliveToken(pickThread()->eventBase);
}

std::vector<Executor::KeepAlive<EventBase>>
IOThreadPoolExecutor::getAllEventBases() {
  ensureMaxActiveThreads();
  std::vector<Executor::KeepAlive<EventBase>> evbs;
  std::shared_lock r{threadListLock_};
  const auto& threads = threadList_.get();
  evbs.reserve(threads.size());
  for (const auto& thr : threads) {
    evbs.emplace_back(static_cast<IOThread&>(*thr).eventBase);
  }
  return evbs;
}

EventBaseManager* IOThreadPoolExecutor::getEventBaseManager() {
  return eventBaseManager_;
}

std::shared_ptr<ThreadPoolExecutor::Thread> IOThreadPoolExecutor::makeThread() {
  return std::make_shared<IOThread>();
}

void IOThreadPoolExecutor::threadRun(ThreadPtr thread) {
  const auto& ioThread = *thisThread_ =
      std::static_pointer_cast<IOThread>(thread);
  try {
    ioThread->eventBase = eventBaseManager_->getEventBase();
  } catch (...) {
    thread->startupException = std::current_exception();
  }
  if (thread->startupException) {
    thread->initBaton.post();
    return;
  }
  this->threadPoolHook_.registerThread();
  if (maxReadAtOnce_) {
    ioThread->eventBase->setMaxReadAtOnce(*maxReadAtOnce_);
  }

  auto tid = folly::getOSThreadID();
  if (threadIdCollector_) {
    threadIdCollector_->addTid(tid);
  }
  SCOPE_EXIT {
    if (threadIdCollector_) {
      threadIdCollector_->removeTid(tid);
    }
  };

  auto idler = std::make_unique<MemoryIdlerTimeout>(ioThread->eventBase);
  ioThread->eventBase->runBeforeLoop(idler.get());

  // Post initBaton from inside the event loop. This guarantees loopTid_
  // is set before afterConstructThreads returns, preventing the TOCTOU
  // race where isInEventBaseThread() sees kNotRunningTid but loopTid_
  // changes before dcheckIsInEventBaseThread() runs.
  ioThread->eventBase->runInEventBaseThread([thread] {
    thread->initBaton.post();
  });

  // Poll readyBaton without blocking the event loop. Observer
  // registration (between initBaton and readyBaton) may call
  // runInEventBaseThreadAndWait, so blocking would deadlock. In normal
  // operation readyBaton is posted within microseconds of initBaton.
  // In the rollback case, cancelledBeforeReady terminates the loop.
  struct ReadyCheck : EventBase::LoopCallback {
    ThreadPtr thread_;
    IOThread* ioThread_{};
    void runLoopCallback() noexcept override {
      if (thread_->readyBaton.try_wait()) {
        if (thread_->cancelledBeforeReady) {
          ioThread_->shouldRun = false;
          ioThread_->eventBase->terminateLoopSoon();
        }
      } else {
        ioThread_->eventBase->runBeforeLoop(this);
      }
    }
  };

  {
    ReadyCheck readyCheck;
    readyCheck.thread_ = std::move(thread);
    readyCheck.ioThread_ = ioThread.get();
    ioThread->eventBase->runBeforeLoop(&readyCheck);

    ExecutorBlockingGuard guard{
        ExecutorBlockingGuard::TrackTag{}, this, getName()};
    while (ioThread->shouldRun) {
      ioThread->eventBase->loopForever();
    }
    if (isJoin_) {
      while (ioThread->pendingTasks > 0) {
        ioThread->eventBase->loopOnce();
      }
    }
    readyCheck.cancelLoopCallback();
    idler.reset();
    if (isWaitForAll_) {
      // some tasks, like thrift asynchronous calls, create additional
      // event base hookups, let's wait till all of them complete.
      ioThread->eventBase->loop();
    }
  }

  std::lock_guard guard(ioThread->eventBaseShutdownMutex_);
  ioThread->eventBase = nullptr;
  eventBaseManager_->clearEventBase();
}

// threadListLock_ is writelocked
void IOThreadPoolExecutor::stopThreads(size_t n) {
  std::vector<ThreadPtr> stoppedThreads;
  stoppedThreads.reserve(n);
  for (size_t i = 0; i < n; i++) {
    const auto ioThread =
        std::static_pointer_cast<IOThread>(threadList_.get()[i]);
    for (auto& o : observers_) {
      o->threadStopped(ioThread.get());
      handleObserverUnregisterThread(ioThread.get(), *o);
    }
    ioThread->shouldRun = false;
    stoppedThreads.push_back(ioThread);
    std::lock_guard guard(ioThread->eventBaseShutdownMutex_);
    if (ioThread->eventBase) {
      ioThread->eventBase->terminateLoopSoon();
    }
  }
  for (const auto& thread : stoppedThreads) {
    stoppedThreadProcessedTasks_ += thread->processedTasks;
    thread->processedTasks = 0;
    stoppedThreads_.add(folly::copy(thread));
    threadList_.remove(thread);
  }
}

// threadListLock_ is readlocked
size_t IOThreadPoolExecutor::getPendingTaskCountImpl() const {
  size_t count = 0;
  for (const auto& thread : threadList_.get()) {
    auto ioThread = std::static_pointer_cast<IOThread>(thread);
    size_t pendingTasks = ioThread->pendingTasks;
    if (pendingTasks > 0 && !ioThread->idle.load(std::memory_order_relaxed)) {
      pendingTasks--;
    }
    count += pendingTasks;
  }
  return count;
}

void IOThreadPoolExecutor::handleObserverRegisterThread(
    ThreadHandle* h, Observer& observer) noexcept {
  auto thread = CHECK_NOTNULL(dynamic_cast<IOThread*>(h));
  if (auto ioObserver = dynamic_cast<IOObserver*>(&observer)) {
    ioObserver->registerEventBase(*thread->eventBase);
  }
}

void IOThreadPoolExecutor::handleObserverUnregisterThread(
    ThreadHandle* h, Observer& observer) noexcept {
  auto thread = CHECK_NOTNULL(dynamic_cast<IOThread*>(h));
  if (auto ioObserver = dynamic_cast<IOObserver*>(&observer)) {
    ioObserver->unregisterEventBase(*thread->eventBase);
  }
}

} // namespace folly
