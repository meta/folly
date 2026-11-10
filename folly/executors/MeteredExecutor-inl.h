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

#include <limits>
#include <stdexcept>

#include <folly/Exception.h>
#include <folly/io/async/AtomicNotificationQueue.h>

namespace folly {
namespace detail {

template <template <typename> class Atom>
MeteredExecutorImpl<Atom>::MeteredExecutorImpl(
    KeepAlive keepAlive, Options options)
    : MeteredExecutorImpl(
          std::move(keepAlive),
          validateOptions(std::move(options)),
          OptionsValidated{}) {}

template <template <typename> class Atom>
MeteredExecutorImpl<Atom>::MeteredExecutorImpl(
    KeepAlive keepAlive, Options options, OptionsValidated)
    : options_(std::move(options)), kaInner_(std::move(keepAlive)) {}

template <template <typename> class Atom>
auto MeteredExecutorImpl<Atom>::validateOptions(Options options) -> Options {
  if (options.maxInQueue < 1) {
    throw_exception<std::invalid_argument>(
        "MeteredExecutor: maxInQueue must be >= 1");
  }
  if (options.maxInQueue >= (uint32_t(1) << 31)) {
    throw_exception<std::invalid_argument>(
        "MeteredExecutor: maxInQueue must be < 2^31");
  }
  return options;
}

template <template <typename> class Atom>
MeteredExecutorImpl<Atom>::MeteredExecutorImpl(
    std::unique_ptr<Executor> executor, Options options)
    : MeteredExecutorImpl(getKeepAliveToken(*executor), std::move(options)) {
  ownedExecutor_ = std::move(executor);
}

template <template <typename> class Atom>
std::unique_ptr<QueueObserver> MeteredExecutorImpl<Atom>::setupQueueObserver() {
  if (options_.enableQueueObserver) {
    std::string name = "unk";
    if (options_.name != "") {
      name = options_.name;
    }
    if (auto factory = folly::QueueObserverFactory::make(
            "mex." + name, options_.numPriorities)) {
      return factory->create(options_.priority);
    }
  }
  return nullptr;
}

template <template <typename> class Atom>
template <class F>
void MeteredExecutorImpl<Atom>::modifyState(F f) {
  uint64_t oldState = state_.load(std::memory_order_relaxed);
  uint64_t newState;
  do {
    newState = f(oldState);
    // Verify invariants: no more in-queue than allowed.
    DCHECK_LE(newState >> kInQueueShift, options_.maxInQueue);
    // No more in queue than pending tasks.
    DCHECK_LE(newState >> kInQueueShift, newState & kSizeMask);
  } while (!state_.compare_exchange_strong(
      oldState,
      newState,
      std::memory_order_relaxed,
      std::memory_order_relaxed));
}

template <template <typename> class Atom>
void MeteredExecutorImpl<Atom>::add(Func func) {
  auto task = Task(std::move(func), RequestContext::saveContext());
  if (queueObserver_) {
    auto payload = queueObserver_->onEnqueued(task.requestContext());
    task.setQueueObserverPayload(payload);
  }

  queue_.enqueue(std::move(task));

  bool shouldScheduleWorker;
  modifyState([&](uint64_t state) {
    state += kSizeInc;
    CHECK_NE(state & kSizeMask, 0)
        << "Too many pending tasks in MeteredExecutor";
    if (!(state & kPausedBit) &&
        ((state >> kInQueueShift) < options_.maxInQueue)) {
      state += kInQueueInc;
      shouldScheduleWorker = true;
    } else {
      shouldScheduleWorker = false;
    }
    return state;
  });

  if (shouldScheduleWorker) {
    scheduleWorker();
  }
}

template <template <typename> class Atom>
bool MeteredExecutorImpl<Atom>::pause() {
  auto oldState = state_.fetch_or(kPausedBit, std::memory_order_relaxed);
  return !(oldState & kPausedBit);
}

template <template <typename> class Atom>
bool MeteredExecutorImpl<Atom>::resume() {
  bool wasPaused = false;
  size_t workersToSchedule = 0;
  modifyState([&](uint64_t state) {
    if (state & kPausedBit) {
      wasPaused = true;
    } else {
      wasPaused = false;
      return state;
    }
    // Workers may have aborted without consuming tasks, reschedule them.
    auto curSize = state & kSizeMask;
    auto curInQueue = state >> kInQueueShift;
    DCHECK_LE(curInQueue, options_.maxInQueue);
    DCHECK_LE(curInQueue, curSize);
    workersToSchedule =
        std::min(static_cast<uint64_t>(options_.maxInQueue), curSize) -
        curInQueue;
    state &= ~kPausedBit;
    state += workersToSchedule * kInQueueInc;
    return state;
  });

  if (!wasPaused) {
    return false;
  }

  for (size_t i = 0; i < workersToSchedule; ++i) {
    scheduleWorker();
  }
  return true;
}

template <template <typename> class Atom>
void MeteredExecutorImpl<Atom>::Task::run() && {
  folly::RequestContextScopeGuard rctxGuard{std::move(rctx_)};
  invokeCatchingExns("MeteredExecutor", std::exchange(func_, {}));
}

template <template <typename> class Atom>
void MeteredExecutorImpl<Atom>::worker() {
  bool shouldAbort = false;
  bool shouldRescheduleWorker = false;
  modifyState([&](uint64_t state) {
    if (state & kPausedBit) {
      shouldAbort = true;
      shouldRescheduleWorker = false;
    } else {
      shouldAbort = false;
      // More work to do than workers in queue, re-schedule the worker without
      // changing the in-queue count.
      shouldRescheduleWorker = (state & kSizeMask) > (state >> kInQueueShift);
      DCHECK_GT(state & kSizeMask, 0);
      state -= kSizeInc;
    }
    if (!shouldRescheduleWorker) {
      state -= kInQueueInc;
    }
    return state;
  });

  if (shouldAbort) {
    return;
  }
  if (shouldRescheduleWorker) {
    scheduleWorker();
  }

  Task task;
  CHECK(queue_.try_dequeue(task));
  std::move(task).run();
}

template <template <typename> class Atom>
void MeteredExecutorImpl<Atom>::scheduleWorker() {
  folly::RequestContextScopeGuard rctxGuard{nullptr};
  kaInner_->add([self = getKeepAliveToken(this)] { self->worker(); });
}

template <template <typename> class Atom>
MeteredExecutorImpl<Atom>::~MeteredExecutorImpl() {
  joinKeepAlive();
}

} // namespace detail
} // namespace folly
