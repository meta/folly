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

#include <folly/executors/FunctionScheduler.h>

#include <random>

#include <glog/logging.h>

#include <folly/Conv.h>
#include <folly/Random.h>
#include <folly/String.h>
#include <folly/system/ThreadName.h>

using std::chrono::microseconds;
using std::chrono::steady_clock;

namespace folly {

namespace {

struct ConsistentDelayFunctor {
  const microseconds constInterval;

  explicit ConsistentDelayFunctor(microseconds interval)
      : constInterval(interval) {
    if (interval < microseconds::zero()) {
      throw std::invalid_argument(
          "FunctionScheduler: "
          "time interval must be non-negative");
    }
  }

  steady_clock::time_point operator()(
      steady_clock::time_point curNextRunTime,
      steady_clock::time_point curTime) const {
    auto intervalsPassed = (curTime - curNextRunTime) / constInterval;
    return (intervalsPassed + 1) * constInterval + curNextRunTime;
  }
};

struct ConstIntervalFunctor {
  const microseconds constInterval;

  explicit ConstIntervalFunctor(microseconds interval)
      : constInterval(interval) {
    if (interval < microseconds::zero()) {
      throw std::invalid_argument(
          "FunctionScheduler: "
          "time interval must be non-negative");
    }
  }

  microseconds operator()() const { return constInterval; }
};

struct PoissonDistributionFunctor {
  std::default_random_engine generator;
  std::poisson_distribution<microseconds::rep> poissonRandom;

  explicit PoissonDistributionFunctor(microseconds meanPoissonUsec)
      : poissonRandom(meanPoissonUsec.count()) {
    if (meanPoissonUsec.count() < 0) {
      throw std::invalid_argument(
          "FunctionScheduler: "
          "Poisson mean interval must be non-negative");
    }
  }

  microseconds operator()() { return microseconds(poissonRandom(generator)); }
};

struct UniformDistributionFunctor {
  std::default_random_engine generator;
  std::uniform_int_distribution<microseconds::rep> dist;

  UniformDistributionFunctor(microseconds minInterval, microseconds maxInterval)
      : generator(Random::rand32()),
        dist(minInterval.count(), maxInterval.count()) {
    if (minInterval > maxInterval) {
      throw std::invalid_argument(
          "FunctionScheduler: "
          "min time interval must be less or equal than max interval");
    }
    if (minInterval < microseconds::zero()) {
      throw std::invalid_argument(
          "FunctionScheduler: "
          "time interval must be non-negative");
    }
  }

  microseconds operator()() { return microseconds(dist(generator)); }
};

} // namespace

FunctionScheduler::FunctionScheduler() = default;

FunctionScheduler::~FunctionScheduler() {
  // make sure to stop the thread (if running)
  shutdown();
  clearHeap();
}

void FunctionScheduler::addFunction(
    Function<void()>&& cb,
    microseconds interval,
    StringPiece nameID,
    microseconds startDelay) {
  addFunctionInternal(
      std::move(cb),
      ConstIntervalFunctor(interval),
      nameID.str(),
      to<std::string>(interval.count(), "us"),
      startDelay,
      false /*runOnce*/);
}

void FunctionScheduler::addFunction(
    Function<void()>&& cb,
    microseconds interval,
    const LatencyDistribution& latencyDistr,
    StringPiece nameID,
    microseconds startDelay) {
  if (latencyDistr.isPoisson) {
    addFunctionInternal(
        std::move(cb),
        PoissonDistributionFunctor(latencyDistr.poissonMean),
        nameID.str(),
        to<std::string>(latencyDistr.poissonMean.count(), "us (Poisson mean)"),
        startDelay,
        false /*runOnce*/);
  } else {
    addFunction(std::move(cb), interval, nameID, startDelay);
  }
}

void FunctionScheduler::addFunctionOnce(
    Function<void()>&& cb, StringPiece nameID, microseconds startDelay) {
  addFunctionInternal(
      std::move(cb),
      ConstIntervalFunctor(microseconds::zero()),
      nameID.str(),
      "once",
      startDelay,
      true /*runOnce*/);
}

void FunctionScheduler::addFunctionUniformDistribution(
    Function<void()>&& cb,
    microseconds minInterval,
    microseconds maxInterval,
    StringPiece nameID,
    microseconds startDelay) {
  addFunctionInternal(
      std::move(cb),
      UniformDistributionFunctor(minInterval, maxInterval),
      nameID.str(),
      to<std::string>(
          "[", minInterval.count(), " , ", maxInterval.count(), "] us"),
      startDelay,
      false /*runOnce*/);
}

void FunctionScheduler::addFunctionConsistentDelay(
    Function<void()>&& cb,
    microseconds interval,
    StringPiece nameID,
    microseconds startDelay) {
  addFunctionInternal(
      std::move(cb),
      ConsistentDelayFunctor(interval),
      nameID.str(),
      to<std::string>(interval.count(), "us"),
      startDelay,
      false /*runOnce*/);
}

void FunctionScheduler::addFunctionGenericDistribution(
    Function<void()>&& cb,
    IntervalDistributionFunc&& intervalFunc,
    const std::string& nameID,
    const std::string& intervalDescr,
    microseconds startDelay) {
  addFunctionInternal(
      std::move(cb),
      std::move(intervalFunc),
      nameID,
      intervalDescr,
      startDelay,
      false /*runOnce*/);
}

void FunctionScheduler::addFunctionGenericNextRunTimeFunctor(
    Function<void()>&& cb,
    NextRunTimeFunc&& fn,
    const std::string& nameID,
    const std::string& intervalDescr,
    microseconds startDelay) {
  addFunctionInternal(
      std::move(cb),
      std::move(fn),
      nameID,
      intervalDescr,
      startDelay,
      false /*runOnce*/);
}

template <typename RepeatFuncNextRunTimeFunc>
void FunctionScheduler::addFunctionToHeapChecked(
    Function<void()>&& cb,
    RepeatFuncNextRunTimeFunc&& fn,
    const std::string& nameID,
    const std::string& intervalDescr,
    microseconds startDelay,
    bool runOnce) {
  if (!cb) {
    throw std::invalid_argument(
        "FunctionScheduler: Scheduled function must be set");
  }
  if (!fn) {
    throw std::invalid_argument(
        "FunctionScheduler: "
        "interval distribution or next run time function must be set");
  }
  if (startDelay < microseconds::zero()) {
    throw std::invalid_argument(
        "FunctionScheduler: start delay must be non-negative");
  }

  std::unique_lock l(mutex_);
  auto it = functionsMap_.find(nameID);
  // check if the nameID is unique
  if (it != functionsMap_.end()) {
    throw std::invalid_argument(
        to<std::string>(
            "FunctionScheduler: a function named \"",
            nameID,
            "\" already exists"));
  }

  if (currentFunction_ && currentFunction_->name == nameID) {
    throw std::invalid_argument(
        to<std::string>(
            "FunctionScheduler: a function named \"",
            nameID,
            "\" already exists"));
  }

  addFunctionToHeap(
      l,
      std::make_unique<RepeatFunc>(
          std::move(cb),
          std::forward<RepeatFuncNextRunTimeFunc>(fn),
          nameID,
          intervalDescr,
          startDelay,
          runOnce));
}

void FunctionScheduler::addFunctionInternal(
    Function<void()>&& cb,
    NextRunTimeFunc&& fn,
    const std::string& nameID,
    const std::string& intervalDescr,
    microseconds startDelay,
    bool runOnce) {
  return addFunctionToHeapChecked(
      std::move(cb), std::move(fn), nameID, intervalDescr, startDelay, runOnce);
}

void FunctionScheduler::addFunctionInternal(
    Function<void()>&& cb,
    IntervalDistributionFunc&& fn,
    const std::string& nameID,
    const std::string& intervalDescr,
    microseconds startDelay,
    bool runOnce) {
  return addFunctionToHeapChecked(
      std::move(cb), std::move(fn), nameID, intervalDescr, startDelay, runOnce);
}

bool FunctionScheduler::cancelFunctionWithLock(
    std::unique_lock<std::mutex>& lock, StringPiece nameID) {
  CHECK_EQ(lock.owns_lock(), true);
  if (currentFunction_ && currentFunction_->name == nameID) {
    auto erased = functionsMap_.erase(currentFunction_->name);
    DCHECK_NE(erased, 0);
    // This function is currently being run. Clear currentFunction_
    // The running thread will see this and won't reschedule the function.
    currentFunction_ = nullptr;
    cancellingCurrentFunction_ = true;
    return true;
  }
  return false;
}

bool FunctionScheduler::cancelFunction(StringPiece nameID) {
  std::unique_lock l(mutex_);
  if (cancelFunctionWithLock(l, nameID)) {
    return true;
  }
  auto it = functionsMap_.find(nameID);
  if (it != functionsMap_.end()) {
    cancelFunction(l, it->second);
    return true;
  }

  return false;
}

bool FunctionScheduler::cancelFunctionAndWait(StringPiece nameID) {
  std::unique_lock l(mutex_);

  if (cancelFunctionWithLock(l, nameID)) {
    runningCondvar_.wait(l, [this]() { return !cancellingCurrentFunction_; });
    return true;
  }

  auto it = functionsMap_.find(nameID);
  if (it != functionsMap_.end()) {
    cancelFunction(l, it->second);
    return true;
  }
  return false;
}

void FunctionScheduler::cancelFunction(
    const std::unique_lock<std::mutex>& l, RepeatFunc* it) {
  // This function should only be called with mutex_ already locked.
  DCHECK(l.mutex() == &mutex_);
  DCHECK(l.owns_lock());
  functionsMap_.erase(it->name);
  functions_.erase(it);
  delete it;
}

bool FunctionScheduler::cancelAllFunctionsWithLock(
    std::unique_lock<std::mutex>& lock) {
  CHECK_EQ(lock.owns_lock(), true);
  clearHeap();
  functionsMap_.clear();
  if (currentFunction_) {
    cancellingCurrentFunction_ = true;
  }
  currentFunction_ = nullptr;
  return cancellingCurrentFunction_;
}

void FunctionScheduler::cancelAllFunctions() {
  std::unique_lock l(mutex_);
  cancelAllFunctionsWithLock(l);
}

void FunctionScheduler::cancelAllFunctionsAndWait() {
  std::unique_lock l(mutex_);
  if (cancelAllFunctionsWithLock(l)) {
    runningCondvar_.wait(l, [this]() { return !cancellingCurrentFunction_; });
  }
}

bool FunctionScheduler::resetFunctionTimer(StringPiece nameID) {
  return resetFunctionTimerImpl(nameID, std::nullopt);
}

bool FunctionScheduler::rescheduleFunction(
    StringPiece nameID, microseconds delay) {
  if (delay < microseconds::zero()) {
    throw std::invalid_argument("FunctionScheduler: negative delay");
  }
  return resetFunctionTimerImpl(nameID, delay);
}

bool FunctionScheduler::resetFunctionTimerImpl(
    StringPiece nameID, std::optional<microseconds> delay) {
  std::unique_lock l(mutex_);
  if (delay && !running_) {
    return false;
  }
  const auto resetNextRunTime = [delay](RepeatFunc& func) {
    const auto now = steady_clock::now();
    if (delay) {
      func.nextRunTime = now + *delay;
    } else {
      func.resetNextRunTime(now);
    }
  };
  if (currentFunction_ && currentFunction_->name == nameID) {
    if (cancellingCurrentFunction_ || currentFunction_->runOnce) {
      return false;
    }
    resetNextRunTime(*currentFunction_);
    return true;
  }

  auto it = functionsMap_.find(nameID);
  if (it != functionsMap_.end()) {
    if (running_) {
      resetNextRunTime(*it->second);
      functions_.update(it->second);
      runningCondvar_.notify_one();
    }
    return true;
  }
  return false;
}

bool FunctionScheduler::start() {
  std::unique_lock l(mutex_);
  if (running_) {
    return false;
  }

  VLOG(1) << "Starting FunctionScheduler with " << functionsMap_.size()
          << " functions.";
  auto now = steady_clock::now();
  // Reset the next run time. for all functions.
  // note: this is needed since one can shutdown() and start() again
  functions_.visit([&now](RepeatFunc* f) {
    f->resetNextRunTime(now);
    VLOG(1) << "   - func: " << (f->name.empty() ? "(anon)" : f->name.c_str())
            << ", period = " << f->intervalDescr
            << ", delay = " << f->startDelay.count() << "us";
  });

  thread_ = std::thread([&] { this->run(); });
  running_ = true;

  return true;
}

bool FunctionScheduler::shutdown() {
  {
    std::lock_guard g(mutex_);
    if (!running_) {
      return false;
    }

    running_ = false;
    runningCondvar_.notify_one();
  }
  thread_.join();
  return true;
}

// Use a dedicated method for sleeping in the main loop, so it can be identified
// easily in profiles and traces.
FOLLY_NOINLINE void FunctionScheduler::waitForWork(
    std::unique_lock<std::mutex>& lock) {
  runningCondvar_.wait(lock);
}
FOLLY_NOINLINE void FunctionScheduler::waitForWork(
    std::unique_lock<std::mutex>& lock,
    std::chrono::steady_clock::duration timeout) {
  runningCondvar_.wait_for(lock, timeout);
}

void FunctionScheduler::run() {
  std::unique_lock lock(mutex_);

  folly::setThreadName(threadName_);

  while (running_) {
    // If we have nothing to run, wait until a function is added or until we
    // are stopped.
    if (functions_.empty()) {
      waitForWork(lock);
      continue;
    }

    auto now = steady_clock::now();
    auto sleepTime = functions_.top()->getNextRunTime() - now;
    if (sleepTime <= steady_clock::duration::zero()) {
      // We need to run this function now
      runOneFunction(lock, now);
      runningCondvar_.notify_all();
    } else {
      waitForWork(lock, sleepTime);
    }
  }
}

void FunctionScheduler::runOneFunction(
    std::unique_lock<std::mutex>& lock, steady_clock::time_point now) {
  DCHECK(lock.mutex() == &mutex_);
  DCHECK(lock.owns_lock());

  // Pop the function from the heap: we need to release mutex_ while we invoke
  // this function, and we need to maintain the heap property on functions_
  // while mutex_ is unlocked.
  auto func = std::unique_ptr<RepeatFunc>(functions_.pop());
  currentFunction_ = func.get();
  // Update the function's next run time.
  if (steady_) {
    // This allows scheduler to catch up
    func->setNextRunTimeSteady();
  } else {
    // Note that we set nextRunTime based on the current time where we started
    // the function call, rather than the time when the function finishes.
    // This ensures that we call the function once every time interval, as
    // opposed to waiting time interval seconds between calls.  (These can be
    // different if the function takes a significant amount of time to run.)
    func->setNextRunTimeStrict(now);
  }

  // Release the lock while we invoke the user's function
  lock.unlock();

  // Invoke the function
  try {
    VLOG(5) << "Now running " << func->name;
    func->cb();
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Error running the scheduled function <" << func->name
               << ">: " << exceptionStr(ex);
  }

  // Re-acquire the lock
  lock.lock();

  if (!currentFunction_) {
    // The function was cancelled while we were running it.
    // We shouldn't reschedule it;
    cancellingCurrentFunction_ = false;
    return;
  }
  if (currentFunction_->runOnce) {
    functionsMap_.erase(currentFunction_->name);
  } else {
    functions_.push(func.release());
  }
  currentFunction_ = nullptr;
}

void FunctionScheduler::addFunctionToHeap(
    const std::unique_lock<std::mutex>& lock,
    std::unique_ptr<RepeatFunc> func) {
  // This function should only be called with mutex_ already locked.
  DCHECK(lock.mutex() == &mutex_);
  DCHECK(lock.owns_lock());

  func->resetNextRunTime(steady_clock::now());
  functionsMap_.emplace(func->name, func.get());
  functions_.push(func.release()); // heap takes ownership
  if (running_) {
    // Signal the running thread to wake up and see if it needs to change
    // its current scheduling decision.
    runningCondvar_.notify_one();
  }
}

void FunctionScheduler::setThreadName(StringPiece threadName) {
  std::unique_lock l(mutex_);
  threadName_ = threadName.str();
}

void FunctionScheduler::clearHeap() {
  while (auto top = functions_.pop()) {
    delete top;
  }
}

} // namespace folly
