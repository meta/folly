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

#include <folly/Chrono.h>
#include <folly/ExceptionString.h>
#include <folly/Function.h>
#include <folly/executors/DrivableExecutor.h>
#include <folly/executors/SequencedExecutor.h>
#include <folly/io/async/NotificationQueue.h>
#include <folly/portability/GFlags.h>
#include <folly/python/Weak.h>

FOLLY_GFLAGS_DECLARE_uint32(folly_asyncio_executor_drive_time_slice_ms);

namespace folly {
namespace python {

class AsyncioExecutor : public DrivableExecutor {
 public:
  ~AsyncioExecutor() override { DCHECK_EQ(keepAliveCounter_, 0); }

 protected:
  /**
   * This function must be called before the parent python loop is closed. It
   * takes care of draining any pending callbacks and destroying the executor
   * instance.
   */
  void drop() noexcept {
    keepAliveRelease();

    /**
     * If python is finalizing, calling scheduled functions MAY segfault.
     * Any code that could have been called is now inconsequential.
     */
    if ((isLinked() && !isFinalizing()) || !isLinked()) {
      while (keepAliveCounter_ > 0) {
        drive();
        // Note: We're busy waiting for new callbacks (not ideal at all)
      }
    }

    /**
     * If there are pending keep-alives, it is not safe to delete *this*.
     * Leak it instead: This is done to ensure that the pending keep-alive
     * pointers are still valid i.e. we cannot destroy this object if the
     * ref-count > 0
     */
    if (keepAliveCounter_ == 0) {
      delete this;
    }
  }

  bool keepAliveAcquire() noexcept override {
    auto keepAliveCounter =
        keepAliveCounter_.fetch_add(1, std::memory_order_relaxed);
    // We should never increment from 0
    DCHECK(keepAliveCounter > 0);
    return true;
  }

  void keepAliveRelease() noexcept override {
    auto keepAliveCounter = --keepAliveCounter_;
    DCHECK(keepAliveCounter >= 0);
  }

 private:
  std::atomic<size_t> keepAliveCounter_{1};
};

// Helper to ensure that `drop` is called reliably
template <typename Derived>
class DroppableAsyncioExecutor : public AsyncioExecutor {
 public:
  struct Deleter {
    void operator()(Derived* ptr) { ptr->drop(); }
  };

  using PtrType = std::unique_ptr<Derived, Deleter>;

  template <typename... Args>
  static PtrType create(Args&&... args) noexcept {
    return PtrType(new Derived(std::forward<Args>(args)...), Deleter());
  }
};

class NotificationQueueAsyncioExecutor
    : public DroppableAsyncioExecutor<NotificationQueueAsyncioExecutor>,
      public SequencedExecutor {
 public:
  using Func = folly::Func;
  using Clock = folly::chrono::coarse_steady_clock;

  NotificationQueueAsyncioExecutor() noexcept
      : driveTimeSlice_(getDefaultTimeSlice()) {}
  explicit NotificationQueueAsyncioExecutor(
      std::chrono::milliseconds driveTimeSlice) noexcept
      : driveTimeSlice_(driveTimeSlice) {}

  struct Stats {
    size_t driveCount{0};
  };

  void add(Func func) override { queue_.putMessage(std::move(func)); }

  int fileno() const { return consumer_.getFd(); }

  /**
   * Drive does work while messages remain in the queue
   * for time slice duration.
   *
   * This ensures a healthy compromise between spending too many resources
   * going back and forth between Python EventLoop & AsyncioExecutor,
   * while still ensuring that we are properly interleaving Native & Python
   * work.
   *
   * This is inspired from the same logic in EventBase
   * (see setLoopCallbacksTimeslice)
   */
  void drive() noexcept override {
    ++stats_.driveCount;

    // Reset current context on scope exit
    RequestContextSaverScopeGuard ctxGuard;
    auto deadline = Clock::now() + driveTimeSlice_;
    // Do-while so we process at least 1 task to make forward progress
    // (even if time_slice is set to 0).
    do {
      // Defined in scope, to ensure it's destroyed immediatly,
      // while in the right request context
      Func func;
      // Note: tryConsume calls RequestContext::setContext().
      // Doing this under a RequestContextSaverScopeGuard instead of a
      // per-callback RequestContextScopeGuard to avoid switching context back
      // and forth when consecutive callbacks have the same context.
      if (!queue_.tryConsume(func)) {
        break;
      }
      invokeCatchingExns("NotificationQueueExecutor: task", std::move(func));
    } while (Clock::now() < deadline);
  }

  const Stats& stats() const { return stats_; }

 private:
  folly::NotificationQueue<Func> queue_;
  folly::NotificationQueue<Func>::SimpleConsumer consumer_{queue_};

  static std::chrono::milliseconds getDefaultTimeSlice();
  std::chrono::milliseconds driveTimeSlice_{0};

  Stats stats_;
}; // NotificationQueueAsyncioExecutor

} // namespace python
} // namespace folly
