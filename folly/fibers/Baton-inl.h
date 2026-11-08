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

#include <folly/detail/AsyncTrace.h>
#include <folly/fibers/Fiber.h>
#include <folly/fibers/FiberManagerInternal.h>

namespace folly {
namespace fibers {

class Baton::FiberWaiter : public Baton::Waiter {
 public:
  void setFiber(Fiber& fiber) {
    DCHECK(!fiber_);
    fiber_ = &fiber;
  }

  void post() override { fiber_->resume(); }

 private:
  Fiber* fiber_{nullptr};
};

inline Baton::Baton() noexcept : Baton(NO_WAITER) {
  assert(Baton(NO_WAITER).futex_.futex == static_cast<uint32_t>(NO_WAITER));
  assert(Baton(POSTED).futex_.futex == static_cast<uint32_t>(POSTED));
  assert(Baton(TIMEOUT).futex_.futex == static_cast<uint32_t>(TIMEOUT));
  assert(
      Baton(THREAD_WAITING).futex_.futex ==
      static_cast<uint32_t>(THREAD_WAITING));

  assert(futex_.futex.is_lock_free());
  assert(waiter_.is_lock_free());
}

template <typename F>
void Baton::wait(F&& mainContextFunc) {
  auto fm = FiberManager::getFiberManagerUnsafe();
  if (!fm || !fm->activeFiber_) {
    mainContextFunc();
    return waitThread();
  }

  return waitFiber(*fm, std::forward<F>(mainContextFunc));
}

template <typename F>
void Baton::waitFiber(FiberManager& fm, F&& mainContextFunc) {
  FiberWaiter waiter;
  auto f = [this, &mainContextFunc, &waiter](Fiber& fiber) mutable {
    waiter.setFiber(fiber);
    setWaiter(waiter);

    mainContextFunc();
  };

  fm.awaitFunc_ = std::ref(f);
  fm.activeFiber_->preempt(Fiber::AWAITING);
}

template <typename Clock, typename Duration>
bool Baton::timedWaitThread(
    const std::chrono::time_point<Clock, Duration>& deadline) {
  auto waiter = waiter_.load();

  folly::async_tracing::logBlockingOperation(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - Clock::now()));

  if (waiter == NO_WAITER &&
      waiter_.compare_exchange_strong(waiter, THREAD_WAITING)) [[likely]] {
    do {
      auto* futex = &futex_.futex;
      const auto wait_rv = folly::detail::futexWaitUntil(
          futex, uint32_t(THREAD_WAITING), deadline);
      if (wait_rv == folly::detail::FutexResult::TIMEDOUT) {
        return false;
      }
      waiter = waiter_.load(std::memory_order_acquire);
    } while (waiter == THREAD_WAITING);
  }

  if (waiter == POSTED) [[likely]] {
    return true;
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

template <typename Rep, typename Period, typename F>
bool Baton::try_wait_for(
    const std::chrono::duration<Rep, Period>& timeout, F&& mainContextFunc) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  return try_wait_until(deadline, static_cast<F&&>(mainContextFunc));
}

template <typename Clock, typename Duration, typename F>
bool Baton::try_wait_until(
    const std::chrono::time_point<Clock, Duration>& deadline,
    F&& mainContextFunc) {
  auto fm = FiberManager::getFiberManagerUnsafe();

  if (!fm || !fm->activeFiber_) {
    mainContextFunc();
    return timedWaitThread(deadline);
  }

  assert(Clock::is_steady); // fiber timer assumes deadlines are steady

  auto timeoutFunc = [this]() mutable { this->postHelper(TIMEOUT); };
  TimeoutHandler handler;
  handler.timeoutFunc_ = std::ref(timeoutFunc);

  // TODO: have timer support arbitrary clocks
  const auto now = Clock::now();
  const auto timeoutMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      FOLLY_LIKELY(now <= deadline) ? deadline - now : Duration{});
  fm->loopController_->timer()->scheduleTimeout(&handler, timeoutMs);
  waitFiber(*fm, static_cast<F&&>(mainContextFunc));

  return waiter_ == POSTED;
}
} // namespace fibers
} // namespace folly
