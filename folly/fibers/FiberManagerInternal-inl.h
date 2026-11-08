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

#include <cassert>
#include <type_traits>

#include <folly/CPortability.h>
#include <folly/Memory.h>
#include <folly/Optional.h>
#include <folly/Portability.h>
#include <folly/ScopeGuard.h>
#include <folly/Try.h>
#include <folly/fibers/Baton.h>
#include <folly/fibers/Fiber.h>
#include <folly/fibers/LoopController.h>
#include <folly/fibers/Promise.h>
#include <folly/tracing/AsyncStack.h>

namespace folly {
namespace fibers {

namespace detail {

inline FiberManager::Options preprocessOptions(FiberManager::Options opts) {
  /**
   * Adjust the stack size according to the multiplier config.
   * Typically used with sanitizers, which need a lot of extra stack space.
   */
  opts.stackSize *= std::exchange(opts.stackSizeMultiplier, 1);
  return opts;
}

template <class F>
FOLLY_NOINLINE std::invoke_result_t<F> runNoInline(F&& func) {
  return func();
}

template <class Result, class F>
FOLLY_NOINLINE void tryEmplaceWithNoInline(
    folly::Try<Result>& result, F&& func) {
  folly::tryEmplaceWith(result, std::forward<F>(func));
}

} // namespace detail

inline void FiberManager::ensureLoopScheduled() {
  if (isLoopScheduled_) {
    return;
  }

  isLoopScheduled_ = true;
  loopController_->schedule();
}

inline void FiberManager::activateFiber(Fiber* fiber) {
  DCHECK_EQ(activeFiber_, (Fiber*)nullptr);

#ifdef FOLLY_SANITIZE_ADDRESS
  DCHECK(!fiber->asanMainStackBase_);
  DCHECK(!fiber->asanMainStackSize_);
  auto stack = fiber->getStack();
  void* asanFakeStack;
  registerStartSwitchStackWithAsan(&asanFakeStack, stack.first, stack.second);
  SCOPE_EXIT {
    registerFinishSwitchStackWithAsan(asanFakeStack, nullptr, nullptr);
    fiber->asanMainStackBase_ = nullptr;
    fiber->asanMainStackSize_ = 0;
  };
#endif

  activeFiber_ = fiber;

#ifdef FOLLY_SANITIZE_THREAD
  auto tsanCtx = __tsan_get_current_fiber();
  __tsan_switch_to_fiber(fiber->tsanCtx_, 0);
#endif

  fiber->fiberImpl_.activate();

#ifdef FOLLY_SANITIZE_THREAD
  __tsan_switch_to_fiber(tsanCtx, 0);
#endif
}

inline void FiberManager::deactivateFiber(Fiber* fiber) {
  DCHECK_EQ(activeFiber_, fiber);

#ifdef FOLLY_SANITIZE_ADDRESS
  // The hooks that report the main stack are null unless
  // kIsLibrarySanitizeAddress, which HWASAN does not set.
  if (kIsLibrarySanitizeAddress) {
    DCHECK(fiber->asanMainStackBase_);
    DCHECK(fiber->asanMainStackSize_);
  }

  registerStartSwitchStackWithAsan(
      &fiber->asanFakeStack_,
      fiber->asanMainStackBase_,
      fiber->asanMainStackSize_);
  SCOPE_EXIT {
    registerFinishSwitchStackWithAsan(
        fiber->asanFakeStack_,
        &fiber->asanMainStackBase_,
        &fiber->asanMainStackSize_);
    fiber->asanFakeStack_ = nullptr;
  };
#endif

  activeFiber_ = nullptr;
  fiber->fiberImpl_.deactivate();
}

inline void FiberManager::runReadyFiber(
    Fiber* fiber, RequestContextSaverScopeGuard& ctxGuard) {
  SCOPE_EXIT {
    assert(currentFiber_ == nullptr);
    assert(activeFiber_ == nullptr);
  };

  assert(
      fiber->state_ == Fiber::NOT_STARTED ||
      fiber->state_ == Fiber::READY_TO_RUN);
  currentFiber_ = fiber;
  ctxGuard.setContext(std::move(fiber->rcontext_));

  (void)folly::exchangeCurrentAsyncStackRoot(
      std::exchange(fiber->asyncRoot_, nullptr));

  folly::Optional<ExecutionObserverScopeGuard> observersGuard{
      std::in_place,
      &observerList_,
      fiber,
      folly::ExecutionObserver::CallbackType::Fiber};
  SCOPE_EXIT {
    // Ensure that the guard is explicitly destroyed for all terminal states, so
    // that it is done at the right time.
    assert(!observersGuard);
  };

  while (fiber->state_ == Fiber::NOT_STARTED ||
         fiber->state_ == Fiber::READY_TO_RUN) {
    activateFiber(fiber);
    if (fiber->state_ == Fiber::AWAITING_IMMEDIATE) {
      try {
        immediateFunc_();
      } catch (...) {
        exceptionCallback_(current_exception(), "running immediateFunc_");
      }
      immediateFunc_ = nullptr;
      fiber->state_ = Fiber::READY_TO_RUN;
    }
  }

  if (fiber->state_ == Fiber::AWAITING) {
    awaitFunc_(*fiber);
    awaitFunc_ = nullptr;
    observersGuard.reset();
    currentFiber_ = nullptr;
    fiber->rcontext_ = RequestContext::saveContext();
    fiber->asyncRoot_ = folly::exchangeCurrentAsyncStackRoot(nullptr);
  } else if (fiber->state_ == Fiber::INVALID) {
    assert(fibersActive_.load(std::memory_order_relaxed) > 0);
    fibersActive_.fetch_sub(1, std::memory_order_relaxed);
    // Making sure that task functor is deleted once task is complete.
    // NOTE: we must do it on main context, as the fiber is not
    // running at this point.
    fiber->func_ = nullptr;
    fiber->resultFunc_ = nullptr;
    fiber->taskOptions_ = TaskOptions();
    if (fiber->finallyFunc_) {
      try {
        fiber->finallyFunc_();
      } catch (...) {
        exceptionCallback_(current_exception(), "running finallyFunc_");
      }
      fiber->finallyFunc_ = nullptr;
    }
    observersGuard.reset();

    // Make sure LocalData is not accessible from its destructor
    currentFiber_ = nullptr;
    fiber->rcontext_ = RequestContext::saveContext();
    // Async stack roots should have been popped by the time the
    // func_() call has returned.
    fiber->asyncRoot_ = folly::exchangeCurrentAsyncStackRoot(nullptr);
    CHECK(fiber->asyncRoot_ == nullptr);

    fiber->localData_.reset();
    fiber->rcontext_.reset();

    if (fibersPoolSize_ < options_.maxFibersPoolSize ||
        options_.fibersPoolResizePeriodMs > 0) {
      fiber->fiberStackHighWatermark_ = 0;
      fibersPool_.push_front(*fiber);
      ++fibersPoolSize_;
    } else {
      delete fiber;
      assert(fibersAllocated_ > 0);
      --fibersAllocated_;
    }
  } else if (fiber->state_ == Fiber::YIELDED) {
    observersGuard.reset();
    currentFiber_ = nullptr;
    fiber->rcontext_ = RequestContext::saveContext();
    fiber->asyncRoot_ = folly::exchangeCurrentAsyncStackRoot(nullptr);
    fiber->state_ = Fiber::READY_TO_RUN;
    yieldedFibers_->push_back(*fiber);
  }
}

inline void FiberManager::loopUntilNoReady() {
  return loopController_->runLoop();
}

template <typename LoopFunc>
void FiberManager::runFibersHelper(LoopFunc&& loopFunc) {
  if (!alternateSignalStackRegistered_) [[unlikely]] {
    maybeRegisterAlternateSignalStack();
  }

  // Support nested FiberManagers
  auto originalFiberManager = std::exchange(getCurrentFiberManager(), this);

  numUncaughtExceptions_ = uncaught_exceptions();
  currentException_ = current_exception();

  auto* curAsyncRoot = folly::exchangeCurrentAsyncStackRoot(nullptr);

  FiberTailQueue yieldedFibers;
  auto prevYieldedFibers = std::exchange(yieldedFibers_, &yieldedFibers);

  SCOPE_EXIT {
    // Restore the previous AsyncStackRoot and make sure that none of
    // the fibers left any AsyncStackRoot pointers lying around.
    auto* oldAsyncRoot = folly::exchangeCurrentAsyncStackRoot(curAsyncRoot);
    CHECK(oldAsyncRoot == nullptr);

    yieldedFibers_ = prevYieldedFibers;
    readyFibers_.splice(readyFibers_.end(), yieldedFibers);
    if (!readyFibers_.empty()) {
      ensureLoopScheduled();
    }
    std::swap(getCurrentFiberManager(), originalFiberManager);
    CHECK_EQ(this, originalFiberManager);
  };

  // Save current context, and reset it after executing all fibers. This can
  // avoid a lot of context swapping, if the Fibers share the same context
  RequestContextSaverScopeGuard ctxGuard;
  loopFunc(ctxGuard);
}

inline size_t FiberManager::recordStackPosition(size_t position) {
  auto newPosition = std::max(stackHighWatermark(), position);
  stackHighWatermark_.store(newPosition, std::memory_order_relaxed);
  return newPosition;
}

inline void FiberManager::loopUntilNoReadyImpl() {
  runFibersHelper([&](RequestContextSaverScopeGuard& ctxGuard) {
    SCOPE_EXIT {
      isLoopScheduled_ = false;
    };

    bool hadRemote = true;
    while (hadRemote) {
      while (!readyFibers_.empty()) {
        auto& fiber = readyFibers_.front();
        readyFibers_.pop_front();
        runReadyFiber(&fiber, ctxGuard);
      }

      auto hadRemoteFiber = remoteReadyQueue_.sweepOnce(
          [this, &ctxGuard](Fiber* fiber) { runReadyFiber(fiber, ctxGuard); });

      if (hadRemoteFiber) {
        ++remoteCount_;
      }

      auto hadRemoteTask =
          remoteTaskQueue_.sweepOnce([this, &ctxGuard](RemoteTask* taskPtr) {
            std::unique_ptr<RemoteTask> task(taskPtr);
            auto fiber = getFiber();
            if (task->localData) {
              fiber->localData_ = *task->localData;
            }
            fiber->rcontext_ = std::move(task->rcontext);

            fiber->setFunction(std::move(task->func), TaskOptions());
            runReadyFiber(fiber, ctxGuard);
          });

      if (hadRemoteTask) {
        ++remoteCount_;
      }

      hadRemote = hadRemoteTask || hadRemoteFiber;
    }
  });
}

inline void FiberManager::runEagerFiber(Fiber* fiber) {
  loopController_->runEagerFiber(fiber);
}

inline void FiberManager::runEagerFiberImpl(Fiber* fiber) {
  folly::fibers::runInMainContext([&] {
    auto prevCurrentFiber = std::exchange(currentFiber_, fiber);
    SCOPE_EXIT {
      currentFiber_ = prevCurrentFiber;
    };
    runFibersHelper([&](RequestContextSaverScopeGuard& ctxGuard) {
      runReadyFiber(fiber, ctxGuard);
    });
  });
}

inline bool FiberManager::shouldRunLoopRemote() {
  --remoteCount_;
  return !remoteReadyQueue_.empty() || !remoteTaskQueue_.empty();
}

inline bool FiberManager::hasReadyTasks() const {
  return !readyFibers_.empty() || !remoteReadyQueue_.empty() ||
      !remoteTaskQueue_.empty();
}

// We need this to be in a struct, not inlined in addTask, because clang crashes
// otherwise.
template <typename F>
struct FiberManager::AddTaskHelper {
  class Func;

  static constexpr bool allocateInBuffer =
      sizeof(Func) <= Fiber::kUserBufferSize;

  class Func {
   public:
    Func(F&& func, FiberManager& fm) : func_(std::forward<F>(func)), fm_(fm) {}

    void operator()() {
      try {
        func_();
      } catch (...) {
        fm_.exceptionCallback_(current_exception(), "running Func functor");
      }
      if (allocateInBuffer) {
        this->~Func();
      } else {
        delete this;
      }
    }

   private:
    F func_;
    FiberManager& fm_;
  };
};

template <typename F>
Fiber* FiberManager::createTask(F&& func, TaskOptions taskOptions) {
  typedef AddTaskHelper<F> Helper;

  auto fiber = getFiber();
  initLocalData(*fiber);

  if (Helper::allocateInBuffer) {
    auto funcLoc = static_cast<typename Helper::Func*>(fiber->getUserBuffer());
    new (funcLoc) typename Helper::Func(std::forward<F>(func), *this);

    fiber->setFunction(std::ref(*funcLoc), std::move(taskOptions));
  } else {
    auto funcLoc = new typename Helper::Func(std::forward<F>(func), *this);

    fiber->setFunction(std::ref(*funcLoc), std::move(taskOptions));
  }

  return fiber;
}

template <typename F>
void FiberManager::addTask(F&& func, TaskOptions taskOptions) {
  readyFibers_.push_back(
      *createTask(std::forward<F>(func), std::move(taskOptions)));
  ensureLoopScheduled();
}

template <typename F>
void FiberManager::addTaskEager(F&& func) {
  runEagerFiber(createTask(std::forward<F>(func), TaskOptions()));
}

template <typename F>
void FiberManager::addTaskRemote(F&& func) {
  auto task = [&]() {
    auto currentFm = getFiberManagerUnsafe();
    if (currentFm && currentFm->currentFiber_ &&
        currentFm->localType_ == localType_) {
      return std::make_unique<RemoteTask>(
          std::forward<F>(func), currentFm->currentFiber_->localData_);
    }
    return std::make_unique<RemoteTask>(std::forward<F>(func));
  }();
  if (remoteTaskQueue_.insertHead(task.release())) {
    loopController_->scheduleThreadSafe();
  }
}

template <typename X>
struct IsRvalueRefTry {
  static const bool value = false;
};
template <typename T>
struct IsRvalueRefTry<folly::Try<T>&&> {
  static const bool value = true;
};

// We need this to be in a struct, not inlined in addTaskFinally, because clang
// crashes otherwise.
template <typename F, typename G>
struct FiberManager::AddTaskFinallyHelper {
  class Func;

  typedef std::invoke_result_t<F> Result;

  class Finally {
   public:
    Finally(G finally, FiberManager& fm)
        : finally_(std::move(finally)), fm_(fm) {}

    void operator()() {
      try {
        finally_(std::move(result_));
      } catch (...) {
        fm_.exceptionCallback_(current_exception(), "running Finally functor");
      }

      if (allocateInBuffer) {
        this->~Finally();
      } else {
        delete this;
      }
    }

   private:
    friend class Func;

    G finally_;
    folly::Try<Result> result_;
    FiberManager& fm_;
  };

  class Func {
   public:
    Func(F func, Finally& finally)
        : func_(std::move(func)), result_(finally.result_) {}

    void operator()() {
      folly::tryEmplaceWith(result_, std::move(func_));

      if (allocateInBuffer) {
        this->~Func();
      } else {
        delete this;
      }
    }

   private:
    F func_;
    folly::Try<Result>& result_;
  };

  static constexpr bool allocateInBuffer =
      sizeof(Func) + sizeof(Finally) <= Fiber::kUserBufferSize;
};

template <typename F, typename G>
Fiber* FiberManager::createTaskFinally(F&& func, G&& finally) {
  typedef std::invoke_result_t<F> Result;

  static_assert(
      IsRvalueRefTry<typename FirstArgOf<G>::type>::value,
      "finally(arg): arg must be Try<T>&&");
  static_assert(
      std::is_convertible<
          Result,
          typename std::remove_reference<
              typename FirstArgOf<G>::type>::type::element_type>::value,
      "finally(Try<T>&&): T must be convertible from func()'s return type");

  auto fiber = getFiber();
  initLocalData(*fiber);

  typedef AddTaskFinallyHelper<
      typename std::decay<F>::type,
      typename std::decay<G>::type>
      Helper;

  if (Helper::allocateInBuffer) {
    auto funcLoc = static_cast<typename Helper::Func*>(fiber->getUserBuffer());
    auto finallyLoc =
        static_cast<typename Helper::Finally*>(static_cast<void*>(funcLoc + 1));

    new (finallyLoc) typename Helper::Finally(std::forward<G>(finally), *this);
    new (funcLoc) typename Helper::Func(std::forward<F>(func), *finallyLoc);

    fiber->setFunctionFinally(std::ref(*funcLoc), std::ref(*finallyLoc));
  } else {
    auto finallyLoc =
        new typename Helper::Finally(std::forward<G>(finally), *this);
    auto funcLoc =
        new typename Helper::Func(std::forward<F>(func), *finallyLoc);

    fiber->setFunctionFinally(std::ref(*funcLoc), std::ref(*finallyLoc));
  }

  return fiber;
}

template <typename F, typename G>
void FiberManager::addTaskFinally(F&& func, G&& finally) {
  readyFibers_.push_back(
      *createTaskFinally(std::forward<F>(func), std::forward<G>(finally)));
  ensureLoopScheduled();
}

template <typename F, typename G>
void FiberManager::addTaskFinallyEager(F&& func, G&& finally) {
  runEagerFiber(
      createTaskFinally(std::forward<F>(func), std::forward<G>(finally)));
}

template <typename F>
std::invoke_result_t<F> FiberManager::runInMainContext(F&& func) {
  if (activeFiber_ == nullptr) [[unlikely]] {
    return detail::runNoInline(std::forward<F>(func));
  }

  typedef std::invoke_result_t<F> Result;

  folly::Try<Result> result;
  auto f = [&func, &result]() mutable {
    detail::tryEmplaceWithNoInline(result, std::forward<F>(func));
  };

  immediateFunc_ = std::ref(f);
  activeFiber_->preempt(Fiber::AWAITING_IMMEDIATE);

  return std::move(result).value();
}

inline FiberManager& FiberManager::getFiberManager() {
  assert(getCurrentFiberManager() != nullptr);
  return *getCurrentFiberManager();
}

inline FiberManager* FiberManager::getFiberManagerUnsafe() {
  return getCurrentFiberManager();
}

inline bool FiberManager::hasActiveFiber() const {
  return activeFiber_ != nullptr;
}

inline folly::Optional<std::chrono::nanoseconds>
FiberManager::getCurrentTaskRunningTime() const {
  return currentFiber_ ? currentFiber_->getRunningTime() : folly::none;
}

inline void FiberManager::yield() {
  assert(getCurrentFiberManager() == this);
  assert(activeFiber_ != nullptr);
  assert(activeFiber_->state_ == Fiber::RUNNING);
  activeFiber_->preempt(Fiber::YIELDED);
}

template <typename T>
T& FiberManager::local() {
  if (std::type_index(typeid(T)) == localType_ && currentFiber_) {
    return currentFiber_->localData_.get<T>();
  }
  return localThread<T>();
}

template <typename T>
T& FiberManager::localThread() {
  static thread_local T t;
  return t;
}

inline void FiberManager::initLocalData(Fiber& fiber) {
  auto fm = getFiberManagerUnsafe();
  if (fm && fm->currentFiber_ && fm->localType_ == localType_) {
    fiber.localData_ = fm->currentFiber_->localData_;
  }
  fiber.rcontext_ = RequestContext::saveContext();
}

template <typename LocalT>
FiberManager::FiberManager(
    LocalType<LocalT>,
    std::unique_ptr<LoopController> loopController__,
    Options options)
    : loopController_(std::move(loopController__)),
      stackAllocator_(options.guardPagesPerStack),
      options_(detail::preprocessOptions(std::move(options))),
      exceptionCallback_(defaultExceptionCallback),
      fibersPoolResizer_(*this),
      localType_(typeid(LocalT)) {
  loopController_->setFiberManager(this);
}

template <typename F>
typename FirstArgOf<F>::type::value_type inline await_async(F&& func) {
  typedef typename FirstArgOf<F>::type::value_type Result;
  typedef typename FirstArgOf<F>::type::baton_type BatonT;

  return Promise<Result, BatonT>::await_async(std::forward<F>(func));
}

template <typename F>
std::invoke_result_t<F> inline runInMainContext(F&& func) {
  auto fm = FiberManager::getFiberManagerUnsafe();
  if (fm == nullptr) [[unlikely]] {
    return detail::runNoInline(std::forward<F>(func));
  }
  return fm->runInMainContext(std::forward<F>(func));
}

} // namespace fibers
} // namespace folly
