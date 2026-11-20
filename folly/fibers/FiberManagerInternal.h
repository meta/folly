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

#include <functional>
#include <memory>
#include <queue>
#include <thread>
#include <type_traits>
#include <typeindex>
#include <unordered_set>
#include <vector>

#include <folly/AtomicIntrusiveLinkedList.h>
#include <folly/CPortability.h>
#include <folly/Executor.h>
#include <folly/IntrusiveList.h>
#include <folly/Likely.h>
#include <folly/Portability.h>
#include <folly/Try.h>
#include <folly/functional/Invoke.h>
#include <folly/io/async/HHWheelTimer.h>
#include <folly/io/async/Request.h>

#include <folly/executors/ExecutionObserver.h>
#include <folly/fibers/Fiber.h>
#include <folly/fibers/FiberImpl.h>
#include <folly/fibers/GuardPageAllocator.h>
#include <folly/fibers/LoopController.h>
#include <folly/fibers/traits.h>

namespace folly {

template <class T>
class Future;

namespace fibers {

class Baton;
class Fiber;

struct TaskOptions;

template <typename T>
class LocalType {};

class InlineFunctionRunner {
 public:
  virtual ~InlineFunctionRunner() {}

  /**
   * func must be executed inline and only once.
   */
  virtual void run(folly::Function<void()> func) = 0;
};

/**
 * @class FiberManager
 * @brief Single-threaded task execution engine.
 *
 * FiberManager allows semi-parallel task execution on the same thread. Each
 * task can notify FiberManager that it is blocked on something (via await())
 * call. This will pause execution of this task and it will be resumed only
 * when it is unblocked (via setData()).
 */
class FiberManager : public ::folly::Executor {
 public:
  struct Options {
    static constexpr size_t kDefaultStackSize{16 * 1024};

    /**
     * Maximum stack size for fibers which will be used for executing all the
     * tasks.
     */
    size_t stackSize{kDefaultStackSize};

    /**
     * Sanitizers need a lot of extra stack space. Benchmarks results at
     * https://www.usenix.org/system/files/conference/atc12/atc12-final39.pdf,
     * table 2, show that average stack increase with ASAN is 10% and extreme is
     * 306%. Setting to 400% as "works for majority of clients" setting. If
     * stack data layout of the service leads to even higher ASAN memory
     * overhead, those services can tune their fiber stack sizes further.
     *
     * Even in the absence of ASAN, debug builds still need extra stack space
     * due to reduced inlining.
     *
     */
    size_t stackSizeMultiplier{kIsSanitize ? 4 : (!kIsOptimize ? 2 : 1)};

    /**
     * Record exact amount of stack used.
     *
     * This is fairly expensive: we fill each newly allocated stack
     * with some known value and find the boundary of unused stack
     * with linear search every time we surrender the stack back to fibersPool.
     * 0 disables stack recording.
     */
    size_t recordStackEvery{0};

    /**
     * Keep at most this many free fibers in the pool.
     * This way the total number of fibers in the system is always bounded
     * by the number of active fibers + maxFibersPoolSize.
     */
    size_t maxFibersPoolSize{1000};

    /**
     * Protect a small number of fiber stacks with this many guard pages.
     */
    size_t guardPagesPerStack{1};

    /**
     * Free unnecessary fibers in the fibers pool every fibersPoolResizePeriodMs
     * milliseconds. If value is 0, periodic resizing of the fibers pool is
     * disabled.
     */
    uint32_t fibersPoolResizePeriodMs{0};

    constexpr Options() {}

    auto hash() const {
      return std::make_tuple(
          stackSize,
          stackSizeMultiplier,
          recordStackEvery,
          maxFibersPoolSize,
          guardPagesPerStack,
          fibersPoolResizePeriodMs);
    }
  };

  /**
   * A (const) Options instance with a dedicated unique identifier,
   * which is used as a key in FiberManagerMap.
   * This is relevant if you want to run different FiberManager,
   * with different Option, on the same EventBase.
   */
  struct FrozenOptions {
    explicit FrozenOptions(Options opts)
        : options(std::move(opts)), token(create(options)) {}

    const Options options;
    const ssize_t token;

   private:
    static ssize_t create(const Options&);
  };

  using ExceptionCallback =
      folly::Function<void(const std::exception_ptr&, StringPiece context)>;

  FiberManager(const FiberManager&) = delete;
  FiberManager& operator=(const FiberManager&) = delete;

  /**
   * Initializes, but doesn't start FiberManager loop
   *
   * @param loopController A LoopController object
   * @param options FiberManager options
   */
  explicit FiberManager(
      std::unique_ptr<LoopController> loopController,
      Options options = Options());

  /**
   * Initializes, but doesn't start FiberManager loop
   *
   * @param loopController A LoopController object
   * @param options FiberManager options
   * @tparam LocalT only local of this type may be stored on fibers.
   *                Locals of other types will be considered thread-locals.
   */
  template <typename LocalT>
  FiberManager(
      LocalType<LocalT>,
      std::unique_ptr<LoopController> loopController,
      Options options = Options());

  ~FiberManager() override;

  /**
   * Controller access.
   */
  LoopController& loopController();
  const LoopController& loopController() const;

  /**
   * Keeps running ready tasks until the list of ready tasks is empty.
   */
  void loopUntilNoReady();

  /**
   * This should only be called by a LoopController.
   */
  void loopUntilNoReadyImpl();

  /**
   * This should only be called by a LoopController.
   */
  void runEagerFiberImpl(Fiber*);

  /**
   * This should only be called by a LoopController.
   */
  bool shouldRunLoopRemote();

  /**
   * @return true if there are outstanding tasks.
   */
  bool hasTasks() const;
  bool isRemoteScheduled() const;

  /**
   * @return The number of currently active fibers (ready to run or blocked).
   * Does not include the number of remotely enqueued tasks that have not been
   * run yet.
   */
  size_t numActiveTasks() const noexcept {
    return fibersActive_.load(std::memory_order_relaxed);
  }

  /**
   * @return true if there are tasks ready to run.
   */
  bool hasReadyTasks() const;

  /**
   * Sets exception callback which will be called if any of the tasks throws an
   * exception.
   *
   * @param ec An ExceptionCallback object.
   */
  void setExceptionCallback(ExceptionCallback ec);

  /**
   * Add a new task to be executed. Must be called from FiberManager's thread.
   *
   * @param func Task functor; must have a signature of `void func()`.
   *             The object will be destroyed once task execution is complete.
   * @param taskOptions Task specific configs.
   */
  template <typename F>
  void addTask(F&& func, TaskOptions taskOptions = TaskOptions());

  /**
   * Add a new task to be executed and return a future that will be set on
   * return from func. Must be called from FiberManager's thread.
   *
   * @param func Task functor; must have a signature of `void func()`.
   *             The object will be destroyed once task execution is complete.
   */
  template <typename F>
  auto addTaskFuture(F&& func)
      -> folly::Future<folly::lift_unit_t<std::invoke_result_t<F>>>;

  /**
   * Add a new task to be executed. Must be called from FiberManager's thread.
   * The new task is run eagerly. addTaskEager will return only once the new
   * task reaches its first suspension point or is completed.
   *
   * @param func Task functor; must have a signature of `void func()`.
   *             The object will be destroyed once task execution is complete.
   */
  template <typename F>
  void addTaskEager(F&& func);

  /**
   * Add a new task to be executed and return a future that will be set on
   * return from func. Must be called from FiberManager's thread.
   * The new task is run eagerly. addTaskEager will return only once the new
   * task reaches its first suspension point or is completed.
   *
   * @param func Task functor; must have a signature of `void func()`.
   *             The object will be destroyed once task execution is complete.
   */
  template <typename F>
  auto addTaskEagerFuture(F&& func)
      -> folly::Future<folly::lift_unit_t<std::invoke_result_t<F>>>;

  /**
   * Add a new task to be executed. Safe to call from other threads.
   *
   * @param func Task function; must have a signature of `void func()`.
   *             The object will be destroyed once task execution is complete.
   */
  template <typename F>
  void addTaskRemote(F&& func);

  /**
   * Add a new task to be executed and return a future that will be set on
   * return from func. Safe to call from other threads.
   *
   * @param func Task function; must have a signature of `void func()`.
   *             The object will be destroyed once task execution is complete.
   */
  template <typename F>
  auto addTaskRemoteFuture(F&& func)
      -> folly::Future<folly::lift_unit_t<std::invoke_result_t<F>>>;

  // Executor interface calls addTaskRemote
  void add(folly::Func f) override { addTaskRemote(std::move(f)); }

  /**
   * Add a new task. When the task is complete, execute finally(Try<Result>&&)
   * on the main context.
   *
   * @param func Task functor; must have a signature of `T func()` for some T.
   * @param finally Finally functor; must have a signature of
   *                `void finally(Try<T>&&)` and will be passed
   *                the result of func() (including the exception if occurred).
   */
  template <typename F, typename G>
  void addTaskFinally(F&& func, G&& finally);

  /**
   * Add a new task. When the task is complete, execute finally(Try<Result>&&)
   * on the main context.
   * The new task is run eagerly. addTaskEager will return only once the new
   * task reaches its first suspension point or is completed.
   *
   * @param func Task functor; must have a signature of `T func()` for some T.
   * @param finally Finally functor; must have a signature of
   *                `void finally(Try<T>&&)` and will be passed
   *                the result of func() (including the exception if occurred).
   */
  template <typename F, typename G>
  void addTaskFinallyEager(F&& func, G&& finally);

  /**
   * If called from a fiber, immediately switches to the FiberManager's context
   * and runs func(), going back to the Fiber's context after completion.
   * Outside a fiber, just calls func() directly.
   *
   * @return value returned by func().
   */
  template <typename F>
  std::invoke_result_t<F> runInMainContext(F&& func);

  /**
   * Returns a refference to a fiber-local context for given Fiber. Should be
   * always called with the same T for each fiber. Fiber-local context is lazily
   * default-constructed on first request.
   * When new task is scheduled via addTask / addTaskRemote from a fiber its
   * fiber-local context is copied into the new fiber.
   */
  template <typename T>
  T& local();

  template <typename T>
  FOLLY_EXPORT static T& localThread();

  /**
   * @return How many fiber objects (and stacks) has this manager allocated.
   */
  size_t fibersAllocated() const;

  /**
   * @return How many of the allocated fiber objects are currently
   * in the free pool.
   */
  size_t fibersPoolSize() const;

  /**
   * @return true if running activeFiber_ is not nullptr.
   */
  bool hasActiveFiber() const;

  /**
   * @return How long has the currently running task on the fiber ran, in
   * terms of wallclock time. This excludes the time spent in preempted or
   * waiting stages. This only works if TaskOptions.logRunningTime is true
   * during addTask().
   */
  folly::Optional<std::chrono::nanoseconds> getCurrentTaskRunningTime() const;

  /**
   * @return The currently running fiber or null if no fiber is executing.
   */
  Fiber* currentFiber() const { return currentFiber_; }

  /**
   * @return What was the most observed fiber stack usage (in bytes).
   */
  size_t stackHighWatermark() const;

  /**
   * Yield execution of the currently running fiber. Must only be called from a
   * fiber executing on this FiberManager. The calling fiber will be scheduled
   * when all other fibers have had a chance to run and the event loop is
   * serviced.
   */
  void yield();

  /**
   * Setup fibers execution observation/instrumentation. Fiber locals are
   * available to observer.
   *
   * @param observer  Fiber's execution observer.
   */
  void addObserver(ExecutionObserver* observer);

  void removeObserver(ExecutionObserver* observer);

  /**
   * @return Current observer for this FiberManager. Returns nullptr
   * if no observer has been set.
   */
  ExecutionObserver::List& getObserverList();

  /**
   * Setup fibers preempt runner.
   */
  void setPreemptRunner(InlineFunctionRunner* preemptRunner);

  /**
   * Returns an estimate of the number of fibers which are waiting to run (does
   * not include fibers or tasks scheduled remotely).
   */
  size_t runQueueSize() const {
    return readyFibers_.size() + (yieldedFibers_ ? yieldedFibers_->size() : 0);
  }

  static FiberManager& getFiberManager();
  static FiberManager* getFiberManagerUnsafe();

  const Options& getOptions() const { return options_; }

 private:
  friend class Baton;
  friend class Fiber;
  template <typename F>
  struct AddTaskHelper;
  template <typename F, typename G>
  struct AddTaskFinallyHelper;

  struct RemoteTask {
    template <typename F>
    explicit RemoteTask(F&& f)
        : func(std::forward<F>(f)), rcontext(RequestContext::saveContext()) {}
    template <typename F>
    RemoteTask(F&& f, const Fiber::LocalData& localData_)
        : func(std::forward<F>(f)),
          localData(std::make_unique<Fiber::LocalData>(localData_)),
          rcontext(RequestContext::saveContext()) {}
    folly::Function<void()> func;
    std::unique_ptr<Fiber::LocalData> localData;
    std::shared_ptr<RequestContext> rcontext;
    AtomicIntrusiveLinkedListHook<RemoteTask> nextRemoteTask;
  };

  static void defaultExceptionCallback(
      const std::exception_ptr& eptr, StringPiece context);

  template <typename F>
  Fiber* createTask(F&& func, TaskOptions taskOptions);

  template <typename F, typename G>
  Fiber* createTaskFinally(F&& func, G&& finally);

  void runEagerFiber(Fiber* fiber);

  void activateFiber(Fiber* fiber);
  void deactivateFiber(Fiber* fiber);

  template <typename LoopFunc>
  void runFibersHelper(LoopFunc&& loopFunc);

  size_t recordStackPosition(size_t position);

  using FiberTailQueue = folly::IntrusiveList<Fiber, &Fiber::listHook_>;
  using GlobalFiberTailQueue =
      folly::IntrusiveList<Fiber, &Fiber::globalListHook_>;

  Fiber* activeFiber_{nullptr}; /**< active fiber, nullptr on main context */
  /**
   * Same as active fiber, but also set for functions run from fiber on main
   * context.
   */
  Fiber* currentFiber_{nullptr};

  FiberTailQueue readyFibers_; /**< queue of fibers ready to be executed */
  FiberTailQueue* yieldedFibers_{nullptr}; /**< queue of fibers which have
                                      yielded execution */
  FiberTailQueue fibersPool_; /**< pool of uninitialized Fiber objects */

  GlobalFiberTailQueue allFibers_; /**< list of all Fiber objects owned */

  // total number of fibers allocated
  std::atomic<size_t> fibersAllocated_{0};
  // total number of fibers in the free pool
  std::atomic<size_t> fibersPoolSize_{0};
  std::atomic<size_t> fibersActive_{
      0}; /**< number of running or blocked fibers */
  size_t fiberId_{0}; /**< id of last fiber used */

  /**
   * Maximum number of active fibers in the last period lasting
   * Options::fibersPoolResizePeriod milliseconds.
   */
  size_t maxFibersActiveLastPeriod_{0};

  std::unique_ptr<LoopController> loopController_;
  bool isLoopScheduled_{false}; /**< was the ready loop scheduled to run? */

  /**
   * When we are inside FiberManager loop this points to FiberManager. Otherwise
   * it's nullptr
   */
  static FiberManager*& getCurrentFiberManager();

  /**
   * Allocator used to allocate stack for Fibers in the pool.
   * Allocates stack on the stack of the main context.
   */
  GuardPageAllocator stackAllocator_;

  const Options options_; /**< FiberManager options */

  /**
   * Largest observed individual Fiber stack usage in bytes.
   */
  std::atomic<size_t> stackHighWatermark_{0};

  /**
   * Schedules a loop with loopController (unless already scheduled before).
   */
  void ensureLoopScheduled();

  /**
   * @return An initialized Fiber object from the pool
   */
  Fiber* getFiber();

  /**
   * Sets local data for given fiber if all conditions are met.
   */
  void initLocalData(Fiber& fiber);

  /**
   * Function passed to the await call.
   */
  folly::Function<void(Fiber&)> awaitFunc_;

  /**
   * Function passed to the runInMainContext call.
   */
  folly::Function<void()> immediateFunc_;

  /**
   * Preempt runner.
   */
  InlineFunctionRunner* preemptRunner_{nullptr};

  /**
   * Fiber's execution observer.
   */
  ExecutionObserver::List observerList_{};

  ExceptionCallback exceptionCallback_; /**< task exception callback */

  folly::AtomicIntrusiveLinkedList<Fiber, &Fiber::nextRemoteReady_>
      remoteReadyQueue_;

  folly::AtomicIntrusiveLinkedList<RemoteTask, &RemoteTask::nextRemoteTask>
      remoteTaskQueue_;

  ssize_t remoteCount_{0};

  /**
   * Number of uncaught exceptions when FiberManager loop was called.
   */
  ssize_t numUncaughtExceptions_{0};
  /**
   * Current exception when FiberManager loop was called.
   */
  std::exception_ptr currentException_;

  class FibersPoolResizer final : private HHWheelTimer::Callback {
   public:
    explicit FibersPoolResizer(FiberManager& fm) : fiberManager_(fm) {}
    void run();

   private:
    FiberManager& fiberManager_;
    void timeoutExpired() noexcept override { run(); }
    void callbackCanceled() noexcept override {}
  };

  FibersPoolResizer fibersPoolResizer_;
  bool fibersPoolResizerScheduled_{false};

  void doFibersPoolResizing();

  /**
   * Only local of this type will be available for fibers.
   */
  std::type_index localType_;

  void runReadyFiber(Fiber* fiber, RequestContextSaverScopeGuard& ctxGuard);
  void remoteReadyInsert(Fiber* fiber);

  // These methods notify ASAN when a fiber is entered/exited so that ASAN can
  // find the right stack extents when it needs to poison/unpoison the stack.
  void registerStartSwitchStackWithAsan(
      void** saveFakeStack, const void* stackBase, size_t stackSize);
  void registerFinishSwitchStackWithAsan(
      void* fakeStack, const void** saveStackBase, size_t* saveStackSize);
  void freeFakeStack(void* fakeStack);
  void unpoisonFiberStack(const Fiber* fiber);

  bool alternateSignalStackRegistered_{false};

  void maybeRegisterAlternateSignalStack();
};

/**
 * @return      true iff we are running in a fiber's context
 */
inline bool onFiber() {
  auto fm = FiberManager::getFiberManagerUnsafe();
  return fm ? fm->hasActiveFiber() : false;
}

/**
 * Add a new task to be executed.
 *
 * @param func Task functor; must have a signature of `void func()`.
 *             The object will be destroyed once task execution is complete.
 */
template <typename F>
inline void addTask(F&& func) {
  return FiberManager::getFiberManager().addTask(std::forward<F>(func));
}

template <typename F>
inline void addTaskEager(F&& func) {
  return FiberManager::getFiberManager().addTaskEager(std::forward<F>(func));
}

/**
 * Add a new task. When the task is complete, execute finally(Try<Result>&&)
 * on the main context.
 * Task functor is run and destroyed on the fiber context.
 * Finally functor is run and destroyed on the main context.
 *
 * @param func Task functor; must have a signature of `T func()` for some T.
 * @param finally Finally functor; must have a signature of
 *                `void finally(Try<T>&&)` and will be passed
 *                the result of func() (including the exception if occurred).
 */
template <typename F, typename G>
inline void addTaskFinally(F&& func, G&& finally) {
  return FiberManager::getFiberManager().addTaskFinally(
      std::forward<F>(func), std::forward<G>(finally));
}

template <typename F, typename G>
inline void addTaskFinallyEager(F&& func, G&& finally) {
  return FiberManager::getFiberManager().addTaskFinallyEager(
      std::forward<F>(func), std::forward<G>(finally));
}

/**
 * Blocks task execution until given promise is fulfilled.
 *
 * Calls function passing in a Promise<T>, which has to be fulfilled.
 *
 * @return data which was used to fulfill the promise.
 */
template <typename F>
typename FirstArgOf<F>::type::value_type inline await_async(F&& func);
#if !defined(_MSC_VER)
template <typename F>
FOLLY_ERASE typename FirstArgOf<F>::type::value_type await(F&& func) {
  return await_async(static_cast<F&&>(func));
}
#endif

/**
 * If called from a fiber, immediately switches to the FiberManager's context
 * and runs func(), going back to the Fiber's context after completion.
 * Outside a fiber, just calls func() directly.
 *
 * @return value returned by func().
 */
template <typename F>
std::invoke_result_t<F> inline runInMainContext(F&& func);

/**
 * Returns a refference to a fiber-local context for given Fiber. Should be
 * always called with the same T for each fiber. Fiber-local context is lazily
 * default-constructed on first request.
 * When new task is scheduled via addTask / addTaskRemote from a fiber its
 * fiber-local context is copied into the new fiber.
 */
template <typename T>
T& local() {
  auto fm = FiberManager::getFiberManagerUnsafe();
  if (fm) {
    return fm->local<T>();
  }
  return FiberManager::localThread<T>();
}

inline void yield() {
  auto fm = FiberManager::getFiberManagerUnsafe();
  if (fm) {
    fm->yield();
  } else {
    std::this_thread::yield();
  }
}
} // namespace fibers
} // namespace folly

#include <folly/fibers/FiberManagerInternal-inl.h>
