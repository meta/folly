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

#include <folly/test/DeterministicSchedule.h>

#include <algorithm>
#include <cassert>
#include <list>
#include <mutex>
#include <random>
#include <unordered_map>
#include <utility>

#include <folly/Random.h>
#include <folly/SingletonThreadLocal.h>

namespace folly {
namespace test {

using Sem = DeterministicSchedule::Sem;

AuxChk DeterministicSchedule::aux_chk;

// access is protected by futexLock
static std::unordered_map<
    const detail::Futex<DeterministicAtomic>*,
    std::list<std::pair<uint32_t, bool*>>>
    futexQueues;

static std::mutex futexLock;

void ThreadTimestamps::sync(const ThreadTimestamps& src) {
  if (src.timestamps_.size() > timestamps_.size()) {
    timestamps_.resize(src.timestamps_.size());
  }
  for (size_t i = 0; i < src.timestamps_.size(); i++) {
    timestamps_[i].sync(src.timestamps_[i]);
  }
}

DSchedTimestamp ThreadTimestamps::advance(DSchedThreadId tid) {
  assert(timestamps_.size() > tid.val);
  return timestamps_[tid.val].advance();
}

void ThreadTimestamps::setIfNotPresent(DSchedThreadId tid, DSchedTimestamp ts) {
  assert(ts.initialized());
  if (tid.val >= timestamps_.size()) {
    timestamps_.resize(tid.val + 1);
  }
  if (!timestamps_[tid.val].initialized()) {
    timestamps_[tid.val].sync(ts);
  }
}

void ThreadTimestamps::clear() {
  timestamps_.clear();
}

bool ThreadTimestamps::atLeastAsRecentAs(
    DSchedThreadId tid, DSchedTimestamp ts) const {
  // It is not meaningful learn whether any instance is at least
  // as recent as timestamp 0.
  assert(ts.initialized());
  if (tid.val >= timestamps_.size()) {
    return false;
  }
  return timestamps_[tid.val].atLeastAsRecentAs(ts);
}

bool ThreadTimestamps::atLeastAsRecentAsAny(const ThreadTimestamps& src) const {
  size_t min = timestamps_.size() < src.timestamps_.size()
      ? timestamps_.size()
      : src.timestamps_.size();
  for (size_t i = 0; i < min; i++) {
    if (src.timestamps_[i].initialized() &&
        timestamps_[i].atLeastAsRecentAs(src.timestamps_[i])) {
      return true;
    }
  }
  return false;
}

void ThreadSyncVar::acquire() {
  ThreadInfo& threadInfo = DeterministicSchedule::getCurrentThreadInfo();
  DSchedThreadId tid = DeterministicSchedule::getThreadId();
  threadInfo.acqRelOrder_.advance(tid);
  threadInfo.acqRelOrder_.sync(order_);
}

void ThreadSyncVar::release() {
  ThreadInfo& threadInfo = DeterministicSchedule::getCurrentThreadInfo();
  DSchedThreadId tid = DeterministicSchedule::getThreadId();
  threadInfo.acqRelOrder_.advance(tid);
  order_.sync(threadInfo.acqRelOrder_);
}

void ThreadSyncVar::acq_rel() {
  ThreadInfo& threadInfo = DeterministicSchedule::getCurrentThreadInfo();
  DSchedThreadId tid = DeterministicSchedule::getThreadId();
  threadInfo.acqRelOrder_.advance(tid);
  threadInfo.acqRelOrder_.sync(order_);
  order_.sync(threadInfo.acqRelOrder_);
}

namespace {

struct PerThreadState {
  // delete the constructors and assignment operators for sanity
  //
  // but... we can't delete the move constructor and assignment operators
  // because those are required before C++17 in the implementation of
  // SingletonThreadLocal
  PerThreadState(const PerThreadState&) = delete;
  PerThreadState& operator=(const PerThreadState&) = delete;
  PerThreadState(PerThreadState&&) = default;
  PerThreadState& operator=(PerThreadState&&) = default;
  PerThreadState() = default;

  Sem* sem{nullptr};
  DeterministicSchedule* sched{nullptr};
  bool exiting{false};
  DSchedThreadId threadId{};
  AuxAct aux_act{};
};
using TLState = SingletonThreadLocal<PerThreadState>;

} // namespace

DeterministicSchedule::DeterministicSchedule(
    std::function<size_t(size_t)> scheduler)
    : DeterministicSchedule(std::move(scheduler), FutexObserver{}) {}

DeterministicSchedule::DeterministicSchedule(
    std::function<size_t(size_t)> scheduler, FutexObserver futexObserver)
    : scheduler_(std::move(scheduler)),
      futexObserver_(std::move(futexObserver)),
      nextThreadId_(0),
      step_(0) {
  auto& tls = TLState::get();
  assert(tls.sem == nullptr);
  assert(tls.sched == nullptr);
  assert(tls.aux_act == nullptr);

  tls.exiting = false;
  tls.sem = new Sem(true);
  sems_.push_back(tls.sem);

  tls.threadId = nextThreadId_++;
  threadInfoMap_.emplace_back(tls.threadId);
  tls.sched = this;
}

DeterministicSchedule::~DeterministicSchedule() {
  auto& tls = TLState::get();
  static_cast<void>(tls);
  assert(tls.sched == this);
  assert(sems_.size() == 1);
  assert(sems_[0] == tls.sem);
  delete tls.sem;
  tls = PerThreadState();
}

std::function<size_t(size_t)> DeterministicSchedule::uniform(uint64_t seed) {
  auto rand = std::make_shared<std::ranlux48>(seed);
  return [rand](size_t numActive) {
    auto dist = std::uniform_int_distribution<size_t>(0, numActive - 1);
    return dist(*rand);
  };
}

struct UniformSubset {
  UniformSubset(uint64_t seed, size_t subsetSize, size_t stepsBetweenSelect)
      : uniform_(DeterministicSchedule::uniform(seed)),
        subsetSize_(subsetSize),
        stepsBetweenSelect_(stepsBetweenSelect),
        stepsLeft_(0) {}

  size_t operator()(size_t numActive) {
    adjustPermSize(numActive);
    if (stepsLeft_-- == 0) {
      stepsLeft_ = stepsBetweenSelect_ - 1;
      shufflePrefix();
    }
    return perm_[uniform_(std::min(numActive, subsetSize_))];
  }

 private:
  std::function<size_t(size_t)> uniform_;
  const size_t subsetSize_;
  const size_t stepsBetweenSelect_;

  size_t stepsLeft_;
  // only the first subsetSize_ is properly randomized
  std::vector<size_t> perm_;

  void adjustPermSize(size_t numActive) {
    if (perm_.size() > numActive) {
      perm_.erase(
          std::remove_if(
              perm_.begin(),
              perm_.end(),
              [=](size_t x) { return x >= numActive; }),
          perm_.end());
    } else {
      while (perm_.size() < numActive) {
        perm_.push_back(perm_.size());
      }
    }
    assert(perm_.size() == numActive);
  }

  void shufflePrefix() {
    for (size_t i = 0; i < std::min(perm_.size() - 1, subsetSize_); ++i) {
      size_t j = uniform_(perm_.size() - i) + i;
      std::swap(perm_[i], perm_[j]);
    }
  }
};

bool DeterministicSchedule::isCurrentThreadExiting() {
  auto& tls = TLState::get();
  return tls.exiting;
}

bool DeterministicSchedule::isActive() {
  auto& tls = TLState::get();
  return tls.sched != nullptr;
}

void DeterministicSchedule::notifyFutexEvent(
    const DeterministicFutexEvent& event) {
  auto& tls = TLState::get();
  if (tls.sched && tls.sched->futexObserver_) {
    tls.sched->futexObserver_(event);
  }
}

DSchedThreadId DeterministicSchedule::getThreadId() {
  auto& tls = TLState::get();
  assert(tls.sched != nullptr);
  return tls.threadId;
}

DeterministicSchedule* DeterministicSchedule::getCurrentSchedule() {
  auto& tls = TLState::get();
  return tls.sched;
}

std::function<size_t(size_t)> DeterministicSchedule::uniformSubset(
    uint64_t seed, size_t n, size_t m) {
  auto gen = std::make_shared<UniformSubset>(seed, n, m);
  return [=](size_t numActive) { return (*gen)(numActive); };
}

void DeterministicSchedule::beforeSharedAccess() {
  auto& tls = TLState::get();
  if (tls.sem) {
    tls.sem->wait();
  }
}

void DeterministicSchedule::afterSharedAccess() {
  auto& tls = TLState::get();
  auto sched = tls.sched;
  if (!sched) {
    return;
  }
  sched->sems_[sched->scheduler_(sched->sems_.size())]->post();
}

void DeterministicSchedule::afterSharedAccess(bool success) {
  auto& tls = TLState::get();
  auto sched = tls.sched;
  if (!sched) {
    return;
  }
  sched->callAux(success);
  sched->sems_[sched->scheduler_(sched->sems_.size())]->post();
}

size_t DeterministicSchedule::getRandNumber(size_t n) {
  auto& tls = TLState::get();
  if (tls.sched) {
    return tls.sched->scheduler_(n);
  }
  return Random::rand32() % n;
}

int DeterministicSchedule::getcpu(
    unsigned* cpu, unsigned* node, void* /* unused */) {
  auto& tls = TLState::get();
  if (cpu) {
    *cpu = tls.threadId.val;
  }
  if (node) {
    *node = tls.threadId.val;
  }
  return 0;
}

void DeterministicSchedule::setAuxAct(AuxAct& aux) {
  auto& tls = TLState::get();
  tls.aux_act = aux;
}

void DeterministicSchedule::setAuxChk(AuxChk& aux) {
  aux_chk = aux;
}

void DeterministicSchedule::clearAuxChk() {
  aux_chk = nullptr;
}

void DeterministicSchedule::reschedule(Sem* sem) {
  auto& tls = TLState::get();
  auto sched = tls.sched;
  if (sched) {
    sched->sems_.push_back(sem);
  }
}

Sem* DeterministicSchedule::descheduleCurrentThread() {
  auto& tls = TLState::get();
  auto sched = tls.sched;
  if (sched) {
    sched->sems_.erase(
        std::find(sched->sems_.begin(), sched->sems_.end(), tls.sem));
  }
  return tls.sem;
}

Sem* DeterministicSchedule::beforeThreadCreate() {
  Sem* s = new Sem(false);
  beforeSharedAccess();
  sems_.push_back(s);
  afterSharedAccess();
  return s;
}

void DeterministicSchedule::afterThreadCreate(Sem* sem) {
  auto& tls = TLState::get();
  assert(tls.sem == nullptr);
  assert(tls.sched == nullptr);
  tls.exiting = false;
  tls.sem = sem;
  tls.sched = this;
  bool started = false;
  while (!started) {
    beforeSharedAccess();
    if (active_.contains(std::this_thread::get_id())) {
      started = true;
      tls.threadId = nextThreadId_++;
      assert(tls.threadId.val == threadInfoMap_.size());
      threadInfoMap_.emplace_back(tls.threadId);
    }
    afterSharedAccess();
  }
  atomic_thread_fence(std::memory_order_seq_cst);
}

void DeterministicSchedule::beforeThreadExit() {
  auto& tls = TLState::get();
  assert(tls.sched == this);

  atomic_thread_fence(std::memory_order_seq_cst);
  beforeSharedAccess();
  auto parent = joins_.find(std::this_thread::get_id());
  if (parent != joins_.end()) {
    reschedule(parent->second);
    joins_.erase(parent);
  }
  descheduleCurrentThread();
  active_.erase(std::this_thread::get_id());

  FOLLY_TEST_DSCHED_VLOG("exiting");
  exitingSems_[std::this_thread::get_id()] = tls.sem;
  afterSharedAccess();
  // Wait for the parent thread to allow us to run thread-local destructors.
  tls.sem->wait();
  delete tls.sem;
  tls = PerThreadState();
}

void DeterministicSchedule::waitForBeforeThreadExit(std::thread& child) {
  auto& tls = TLState::get();
  assert(tls.sched == this);
  beforeSharedAccess();
  assert(!tls.sched->joins_.contains(child.get_id()));
  if (tls.sched->active_.contains(child.get_id())) {
    Sem* sem = descheduleCurrentThread();
    tls.sched->joins_.insert({child.get_id(), sem});
    afterSharedAccess();
    // Wait to be scheduled by exiting child thread
    beforeSharedAccess();
    assert(!tls.sched->active_.contains(child.get_id()));
  }
  afterSharedAccess();
}

void DeterministicSchedule::joinAll(std::vector<std::thread>& children) {
  auto& tls = TLState::get();
  auto sched = tls.sched;
  if (sched) {
    // Wait until all children are about to exit
    for (auto& child : children) {
      sched->waitForBeforeThreadExit(child);
    }
  }
  atomic_thread_fence(std::memory_order_seq_cst);
  /* Let each child thread proceed one at a time to protect
   * shared access during thread local destructors.*/
  for (auto& child : children) {
    if (sched) {
      beforeSharedAccess();
      sched->exitingSems_[child.get_id()]->post();
    }
    child.join();
    if (sched) {
      afterSharedAccess();
    }
  }
}

void DeterministicSchedule::join(std::thread& child) {
  auto& tls = TLState::get();
  auto sched = tls.sched;
  if (sched) {
    sched->waitForBeforeThreadExit(child);
  }
  atomic_thread_fence(std::memory_order_seq_cst);
  FOLLY_TEST_DSCHED_VLOG("joined " << std::hex << child.get_id());
  if (sched) {
    beforeSharedAccess();
    sched->exitingSems_[child.get_id()]->post();
  }
  child.join();
  if (sched) {
    afterSharedAccess();
  }
}

void DeterministicSchedule::callAux(bool success) {
  auto& tls = TLState::get();
  ++step_;
  if (tls.aux_act) {
    tls.aux_act(success);
    tls.aux_act = nullptr;
  }
  if (aux_chk) {
    aux_chk(step_);
  }
}

static std::unordered_map<Sem*, std::unique_ptr<ThreadSyncVar>> semSyncVar;

void DeterministicSchedule::post(Sem* sem) {
  beforeSharedAccess();
  if (!semSyncVar.contains(sem)) {
    semSyncVar[sem] = std::make_unique<ThreadSyncVar>();
  }
  semSyncVar[sem]->release();
  sem->post();
  FOLLY_TEST_DSCHED_VLOG("sem->post() [sem=" << sem << "]");
  afterSharedAccess();
}

bool DeterministicSchedule::tryWait(Sem* sem) {
  beforeSharedAccess();
  if (!semSyncVar.contains(sem)) {
    semSyncVar[sem] = std::make_unique<ThreadSyncVar>();
  }

  bool acquired = sem->try_wait();
  const char* acquired_s = acquired ? "true" : "false";
  FOLLY_TEST_DSCHED_VLOG(
      "sem->try_wait() [sem=" << sem << "] -> " << acquired_s);
  if (acquired) {
    semSyncVar[sem]->acq_rel();
  } else {
    semSyncVar[sem]->acquire();
  }

  afterSharedAccess();
  return acquired;
}

void DeterministicSchedule::wait(Sem* sem) {
  while (!tryWait(sem)) {
    // we're not busy waiting because this is a deterministic schedule
  }
}

ThreadInfo& DeterministicSchedule::getCurrentThreadInfo() {
  auto& tls = TLState::get();
  auto sched = tls.sched;
  assert(sched);
  assert(tls.threadId.val < sched->threadInfoMap_.size());
  return sched->threadInfoMap_[tls.threadId.val];
}

void DeterministicSchedule::atomic_thread_fence(std::memory_order mo) {
  auto& tls = TLState::get();
  if (!tls.sched) {
    std::atomic_thread_fence(mo);
    return;
  }
  beforeSharedAccess();
  ThreadInfo& threadInfo = getCurrentThreadInfo();
  switch (mo) {
    case std::memory_order_relaxed:
      assert(false);
      break;
    case std::memory_order_consume:
    case std::memory_order_acquire:
      threadInfo.acqRelOrder_.sync(threadInfo.acqFenceOrder_);
      break;
    case std::memory_order_release:
      threadInfo.relFenceOrder_.sync(threadInfo.acqRelOrder_);
      break;
    case std::memory_order_acq_rel:
      threadInfo.acqRelOrder_.sync(threadInfo.acqFenceOrder_);
      threadInfo.relFenceOrder_.sync(threadInfo.acqRelOrder_);
      break;
    case std::memory_order_seq_cst:
      threadInfo.acqRelOrder_.sync(threadInfo.acqFenceOrder_);
      threadInfo.acqRelOrder_.sync(tls.sched->seqCstFenceOrder_);
      tls.sched->seqCstFenceOrder_ = threadInfo.acqRelOrder_;
      threadInfo.relFenceOrder_.sync(threadInfo.acqRelOrder_);
      break;
  }
  FOLLY_TEST_DSCHED_VLOG("fence: " << folly::detail::memory_order_to_str(mo));
  afterSharedAccess();
}

detail::FutexResult futexWaitImpl(
    const detail::Futex<DeterministicAtomic>* futex,
    uint32_t expected,
    std::chrono::system_clock::time_point const* absSystemTimeout,
    std::chrono::steady_clock::time_point const* absSteadyTimeout,
    uint32_t waitMask) {
  return deterministicFutexWaitImpl<DeterministicAtomic>(
      futex,
      futexLock,
      futexQueues,
      expected,
      absSystemTimeout,
      absSteadyTimeout,
      waitMask);
}

int futexWakeImpl(
    const detail::Futex<DeterministicAtomic>* futex,
    int count,
    uint32_t wakeMask) {
  return deterministicFutexWakeImpl<DeterministicAtomic>(
      futex, futexLock, futexQueues, count, wakeMask);
}

} // namespace test
} // namespace folly

namespace folly {

template <>
CacheLocality const& CacheLocality::system<test::DeterministicAtomic>() {
  static CacheLocality cache(CacheLocality::uniform(16));
  return cache;
}

template <>
Getcpu::Func AccessSpreader<test::DeterministicAtomic>::pickGetcpuFunc() {
  return &test::DeterministicSchedule::getcpu;
}
} // namespace folly
