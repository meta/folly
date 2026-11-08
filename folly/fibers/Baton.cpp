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

#include <folly/fibers/Baton.h>

#include <chrono>

#include <folly/detail/MemoryIdler.h>
#include <folly/fibers/FiberManagerInternal.h>
#include <folly/portability/Asm.h>

namespace folly {
namespace fibers {

using folly::detail::futexWake;

void Baton::setWaiter(Waiter& waiter) {
  auto curr_waiter = waiter_.load();
  do {
    if (curr_waiter == NO_WAITER) [[likely]] {
      continue;
    } else if (curr_waiter == POSTED || curr_waiter == TIMEOUT) {
      waiter.post();
      break;
    } else {
      throw std::logic_error("Some waiter is already waiting on this Baton.");
    }
  } while (!waiter_.compare_exchange_weak(
      curr_waiter, reinterpret_cast<intptr_t>(&waiter)));
}

void Baton::wait() {
  wait([]() {});
}

void Baton::wait(TimeoutHandler& timeoutHandler) {
  auto timeoutFunc = [this] {
    if (!try_wait()) {
      postHelper(TIMEOUT);
    }
  };
  timeoutHandler.timeoutFunc_ = std::ref(timeoutFunc);
  timeoutHandler.fiberManager_ = FiberManager::getFiberManagerUnsafe();
  wait();
  timeoutHandler.cancelTimeout();
}

void Baton::waitThread() {
  auto waiter = waiter_.load();

  auto waitStart = std::chrono::steady_clock::now();

  if (waiter == NO_WAITER &&
      waiter_.compare_exchange_strong(waiter, THREAD_WAITING)) [[likely]] {
    do {
      folly::detail::MemoryIdler::futexWait(
          futex_.futex, uint32_t(THREAD_WAITING));
      waiter = waiter_.load(std::memory_order_acquire);
    } while (waiter == THREAD_WAITING);
  }

  folly::async_tracing::logBlockingOperation(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - waitStart));

  if (waiter == POSTED) [[likely]] {
    return;
  }

  // Handle errors
  if (waiter == TIMEOUT) {
    throw std::logic_error("Thread baton can't have timeout status");
  }
  if (waiter == THREAD_WAITING) {
    throw std::logic_error("Other thread is already waiting on this baton");
  }
  throw std::logic_error("Other waiter is already waiting on this baton");
}

void Baton::post() {
  postHelper(POSTED);
}

void Baton::postHelper(intptr_t new_value) {
  auto waiter = waiter_.load();

  do {
    if (waiter == THREAD_WAITING) {
      assert(new_value == POSTED);

      return postThread();
    }

    if (waiter == POSTED) {
      return;
    }
  } while (!waiter_.compare_exchange_weak(waiter, new_value));

  if (waiter != NO_WAITER && waiter != TIMEOUT) {
    reinterpret_cast<Waiter*>(waiter)->post();
  }
}

bool Baton::try_wait() {
  return ready();
}

void Baton::postThread() {
  auto expected = THREAD_WAITING;

  auto* futex = &futex_.futex;
  if (!waiter_.compare_exchange_strong(expected, POSTED)) {
    return;
  }
  futexWake(futex, 1);
}

void Baton::reset() {
  waiter_.store(NO_WAITER, std::memory_order_relaxed);
}

void Baton::TimeoutHandler::scheduleTimeout(std::chrono::milliseconds timeout) {
  assert(fiberManager_ != nullptr);
  assert(timeoutFunc_ != nullptr);

  if (timeout.count() > 0) {
    fiberManager_->loopController_->timer()->scheduleTimeout(this, timeout);
  }
}

} // namespace fibers
} // namespace folly
