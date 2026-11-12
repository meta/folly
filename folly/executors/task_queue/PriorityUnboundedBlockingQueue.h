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
#include <stdexcept>

#include <folly/ConstexprMath.h>
#include <folly/Executor.h>
#include <folly/concurrency/PriorityUnboundedQueueSet.h>
#include <folly/executors/task_queue/BlockingQueue.h>
#include <folly/lang/Exception.h>
#include <folly/synchronization/LifoSem.h>

namespace folly {

template <class T, class Semaphore = folly::LifoSem>
class PriorityUnboundedBlockingQueue : public BlockingQueue<T> {
 public:
  // Note: To use folly::Executor::*_PRI, for numPriorities == 2
  //       MID_PRI and HI_PRI are treated at the same priority level.
  explicit PriorityUnboundedBlockingQueue(
      uint8_t numPriorities,
      const typename Semaphore::Options& semaphoreOptions = {})
      : sem_(semaphoreOptions), queue_(numPriorities) {
    if (numPriorities == 0) {
      throw_exception<std::invalid_argument>(
          "Number of priorities should be positive");
    }
  }

  uint8_t getNumPriorities() override { return queue_.priorities(); }

  // Add at medium priority by default
  BlockingQueueAddResult add(T&& item) override {
    return addWithPriority(std::move(item), folly::Executor::MID_PRI);
  }

  BlockingQueueAddResult addWithPriority(T&& item, int8_t priority) override {
    queue_.at_priority(translatePriority(priority)).enqueue(std::move(item));
    return sem_.post();
  }

  T take() override {
    sem_.wait();
    return dequeue();
  }

  folly::Optional<T> try_take() {
    if (!sem_.try_wait()) {
      return none;
    }
    return dequeue();
  }

  folly::Optional<T> try_take_for(std::chrono::milliseconds time) override {
    if (!sem_.try_wait_for(time)) {
      return none;
    }
    return dequeue();
  }

  size_t size() override { return sem_.valueGuess(); }

 private:
  size_t translatePriority(int8_t const priority) {
    size_t const priorities = queue_.priorities();
    assert(priorities <= 255);
    int8_t const hi = (priorities + 1) / 2 - 1;
    int8_t const lo = hi - (priorities - 1);
    return hi - std::clamp(priority, lo, hi);
  }

  T dequeue() {
    // must follow a successful sem wait
    if (auto obj = queue_.try_dequeue()) {
      return std::move(*obj);
    }
    terminate_with<std::logic_error>("bug in task queue");
  }

  Semaphore sem_;
  PriorityUMPMCQueueSet<T, /* MayBlock = */ true> queue_;
};

} // namespace folly
