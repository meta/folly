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

#include <folly/ConstexprMath.h>
#include <folly/Executor.h>
#include <folly/GLog.h>
#include <folly/concurrency/CacheLocality.h>
#include <folly/concurrency/PriorityUnboundedQueueSet.h>
#include <folly/executors/task_queue/BlockingQueue.h>
#include <folly/executors/task_queue/PriorityUnboundedBlockingQueue.h>
#include <folly/lang/Exception.h>
#include <folly/synchronization/StripedThrottledLifoSem.h>

namespace folly {

/**
 * A BlockingQueue sharded by LLC cache. Compared to the default
 * PriorityUnboundedBlockingQueue, this can reduce contention on systems with a
 * large number of LLC caches, at the cost of unfairness and work conservation;
 * see the StripedThrottledLifoSem documentation for a detailed explanation.
 *
 * This queue should preferably be used on thread pools with numThreads equal to
 * the number of available cores, in order to guarantee good balancing across
 * the LLCs. For the same reason, dynamic resizing should be disabled.
 *
 * NOTE: StripeGetter is customizable only for testing purposes.
 */
template <typename T, class StripeGetter = LLCAccessSpreader>
class StripedPriorityUnboundedBlockingQueue : public BlockingQueue<T> {
  struct PrivateTag {};

 public:
  static std::unique_ptr<BlockingQueue<T>> create(
      uint8_t numPriorities,
      const typename ThrottledLifoSem::Options& semaphoreOptions = {}) {
    const auto numStripes = StripeGetter::get().numStripes();
    if (numStripes == 1) {
      // Don't need the overhead, just use the non-striped version.
      return std::make_unique<
          PriorityUnboundedBlockingQueue<T, folly::ThrottledLifoSem>>(
          numPriorities, semaphoreOptions);
    }
    return std::make_unique<
        StripedPriorityUnboundedBlockingQueue<T, StripeGetter>>(
        PrivateTag{}, numStripes, numPriorities, semaphoreOptions);
  }

  // Private, use create().
  StripedPriorityUnboundedBlockingQueue(
      PrivateTag,
      size_t numStripes,
      uint8_t numPriorities,
      const typename ThrottledLifoSem::Options& semaphoreOptions = {})
      : numPriorities_(numPriorities),
        sem_(numStripes, std::tuple{numPriorities_}, semaphoreOptions) {
    StripedThrottledLifoSemBalancer::subscribe(sem_);
  }

  ~StripedPriorityUnboundedBlockingQueue() override {
    StripedThrottledLifoSemBalancer::unsubscribe(sem_);
  }

  uint8_t getNumPriorities() override { return numPriorities_; }

  BlockingQueueAddResult add(T&& item) override {
    return addWithPriority(std::move(item), folly::Executor::MID_PRI);
  }

  BlockingQueueAddResult addWithPriority(T&& item, int8_t priority) override {
    auto stripeIdx = getStripeIdx();
    sem_.payload(stripeIdx)
        .at_priority(translatePriority(priority))
        .enqueue(std::move(item));
    return sem_.post(stripeIdx);
  }

  T take() override {
    const auto stripeIdx = getStripeIdx();
    auto foundStripeIdx = sem_.wait(stripeIdx);
    return dequeue(foundStripeIdx);
  }

  folly::Optional<T> try_take_for(std::chrono::milliseconds time) override {
    const auto stripeIdx = getStripeIdx();
    if (auto foundStripeIdx = sem_.try_wait_for(stripeIdx, time)) {
      return dequeue(*foundStripeIdx);
    }
    return none;
  }

  size_t size() override { return sem_.valueGuess(); }

 private:
  size_t translatePriority(int8_t const priority) {
    int8_t const hi = (numPriorities_ + 1) / 2 - 1;
    int8_t const lo = hi - (numPriorities_ - 1);
    return hi - std::clamp(priority, lo, hi);
  }

  T dequeue(size_t stripeIdx) {
    // must follow a successful sem wait
    if (auto obj = sem_.payload(stripeIdx).try_dequeue()) {
      return std::move(*obj);
    }
    terminate_with<std::logic_error>("bug in task queue");
  }

  static size_t getStripeIdx() { return StripeGetter::get().current(); }

  using Queue = PriorityUMPMCQueueSet<T, /* MayBlock = */ false>;

  const size_t numPriorities_;
  StripedThrottledLifoSem<Queue> sem_;
};

} // namespace folly
