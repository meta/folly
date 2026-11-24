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

#include <atomic>
#include <optional>

#include <folly/DefaultKeepAliveExecutor.h>
#include <folly/concurrency/UnboundedQueue.h>
#include <folly/executors/QueueObserver.h>

namespace folly {

namespace detail {
// Attaches a queue to an already existing executor and exposes Executor
// interface to add tasks to that queue.
// Consumption from this queue is "metered". Specifically, only a limited number
// of tasks scheduled onto the resulting executor will be ever simultaneously
// present in the wrapped executor's queue. This mechanism can be used e.g. to
// attach lower priority queue to an already existing executor -
// meaning tasks scheduled on the wrapper will, in practice, yield to task
// scheduled directly on the wrapped executor.
//
// Notice that multi level priorities can be easily achieved via chaining,
// for example:
// auto hipri_exec = std::make_unique<CPUThreadPoolExecutor>(numThreads);
// auto hipri_ka = getKeepAliveToken(hipri_exec.get());
// auto mipri_exec = std::make_unique<MeteredExecutor>(hipri_ka);
// auto mipri_ka = getKeepAliveToken(mipri_exec.get());
// auto lopri_exec = std::make_unique<MeteredExecutor>(mipri_ka);
// auto lopri_ka = getKeepAliveToken(lopri_exec.get());
template <template <typename> class Atom = std::atomic>
class MeteredExecutorImpl : public DefaultKeepAliveExecutor {
 public:
  struct Options {
    Options() {}
    std::string name;
    bool enableQueueObserver{false};
    size_t numPriorities{1};
    int8_t priority{0};
    // Maximum number of tasks allowed in the wrapped executor's queue at any
    // given time. This must be >= 1 and < 2^31.
    uint32_t maxInQueue = 1;
  };

  using KeepAlive = Executor::KeepAlive<>;
  // owning constructor
  explicit MeteredExecutorImpl(
      std::unique_ptr<Executor> exe, Options options = Options());
  // non-owning constructor
  explicit MeteredExecutorImpl(
      KeepAlive keepAlive, Options options = Options());
  ~MeteredExecutorImpl() override;

  void add(Func func) override;

  size_t pendingTasks() const { return state_ & kSizeMask; }

  // Pause executing tasks. Subsequent resume() necessary to
  // start executing tasks again. Can be invoked from any thread and only
  // prevents scheduling of future tasks for executions i.e. doesn't impact
  // tasks already scheduled. The method returns `false` if we invoke `pause`
  // on an already paused executor.
  bool pause();

  // Resume executing tasks. no-op if not paused. Can be invoked from any
  // thread. Returns `false` if `resume` is invoked on an executor that's
  // not paused.
  bool resume();

 private:
  // Routes construction through validateOptions() so that a bad Options
  // value throws before the DefaultKeepAliveExecutor base class -- whose
  // destructor asserts its keepAlive_ was already released via
  // joinKeepAlive(), only called from ~MeteredExecutorImpl() -- is ever
  // constructed. Throwing from this class's own constructor body would be
  // too late: the base class subobject would already be alive, and its
  // destructor would run during unwinding without joinKeepAlive() ever
  // having been called. Evaluating validateOptions() as an argument to a
  // delegating constructor call happens before the delegate (and hence its
  // base class) is constructed at all, so a throw here leaves nothing to
  // unwind.
  struct OptionsValidated {};
  MeteredExecutorImpl(KeepAlive keepAlive, Options options, OptionsValidated);
  static Options validateOptions(Options options);

  class Task {
   public:
    Task() = default;

    explicit Task(Func&& func, std::shared_ptr<RequestContext>&& rctx)
        : func_(std::move(func)), rctx_(std::move(rctx)) {}

    void setQueueObserverPayload(intptr_t newValue) {
      queueObserverPayload_ = newValue;
    }

    intptr_t getQueueObserverPayload() const { return queueObserverPayload_; }

    void run() &&;

    RequestContext* requestContext() const { return rctx_.get(); }

   private:
    Func func_;
    std::shared_ptr<RequestContext> rctx_;
    intptr_t queueObserverPayload_;
  };

  template <std::memory_order SuccessOrder, class F>
  void modifyState(F f);

  std::unique_ptr<folly::QueueObserver> setupQueueObserver();
  void worker();
  void scheduleWorker();

  const Options options_;
  std::unique_ptr<Executor> ownedExecutor_;
  const KeepAlive kaInner_;
  const std::unique_ptr<folly::QueueObserver> queueObserver_{
      setupQueueObserver()};

  // State is encoded in a 64-bit word so we can modify it atomically (using
  // modifyState(), which verifies the invariants.
  // Layout:
  // [<workers in queue> 31 bits][<paused> 1 bit>][<pending tasks> 32 bits]
  static constexpr uint64_t kSizeInc = 1;
  static constexpr uint64_t kSizeMask = (uint64_t{1} << 32) - 1;
  static constexpr uint64_t kPausedBit = uint64_t{1} << 32;
  static constexpr uint64_t kInQueueShift = 33;
  static constexpr uint64_t kInQueueInc = uint64_t{1} << kInQueueShift;
  alignas(folly::cacheline_align_v) std::atomic<uint64_t> state_ = 0;
  // Use a smaller segment size than default to limit memory usage in case there
  // are a large number of MeteredExecutors instances in the process.
  // The queue doesn't need blocking because we only dequeue when non-empty.
  UMPMCQueue<Task, /* MayBlock */ false, /* LgSegmentSize */ 5> queue_;
};

} // namespace detail

using MeteredExecutor = detail::MeteredExecutorImpl<>;

} // namespace folly

#include <folly/executors/MeteredExecutor-inl.h>
