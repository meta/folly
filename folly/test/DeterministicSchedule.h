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

#include <assert.h>
#include <errno.h>

#include <atomic>
#include <functional>
#include <list>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glog/logging.h>

#include <folly/ScopeGuard.h>
#include <folly/concurrency/CacheLocality.h>
#include <folly/detail/Futex.h>
#include <folly/lang/CustomizationPoint.h>
#include <folly/synchronization/AsymmetricThreadFence.h>
#include <folly/synchronization/AtomicNotification.h>
#include <folly/synchronization/AtomicUtil.h>
#include <folly/synchronization/detail/AtomicUtils.h>
#include <folly/synchronization/test/Semaphore.h>

namespace folly {
namespace test {

// This is ugly, but better perf for DeterministicAtomic translates
// directly to more states explored and tested
#define FOLLY_TEST_DSCHED_VLOG(...)                             \
  do {                                                          \
    if (false) {                                                \
      VLOG(2) << std::hex << std::this_thread::get_id() << ": " \
              << __VA_ARGS__;                                   \
    }                                                           \
  } while (false)

/* signatures of user-defined auxiliary functions */
using AuxAct = std::function<void(bool)>;
using AuxChk = std::function<void(uint64_t)>;

struct DeterministicFutexEvent {
  enum class Kind : uint8_t {
    WaitQueued,
    Wake,
  };

  Kind kind;
  const void* futex;
  uint32_t mask;
  int count;
  int result;
};

// Called synchronously after the futex queue lock is released, while the
// caller still owns the deterministic scheduler turn.  Observers must not
// block or invoke schedule-aware operations such as DeterministicAtomic or
// deterministic futex functions; doing so deadlocks waiting for that turn to
// finish.  Use std::atomic for state shared with scheduler callbacks.
using FutexObserver = std::function<void(const DeterministicFutexEvent& event)>;

struct DSchedThreadId {
  unsigned val;
  explicit constexpr DSchedThreadId() : val(0) {}
  explicit constexpr DSchedThreadId(unsigned v) : val(v) {}
  unsigned operator=(unsigned v) { return val = v; }
};

class DSchedTimestamp {
 public:
  constexpr explicit DSchedTimestamp() : val_(0) {}
  DSchedTimestamp advance() { return DSchedTimestamp(++val_); }
  bool atLeastAsRecentAs(const DSchedTimestamp& other) const {
    return val_ >= other.val_;
  }
  void sync(const DSchedTimestamp& other) { val_ = std::max(val_, other.val_); }
  bool initialized() const { return val_ > 0; }
  static constexpr DSchedTimestamp initial() { return DSchedTimestamp(1); }

 protected:
  constexpr explicit DSchedTimestamp(size_t v) : val_(v) {}

 private:
  size_t val_;
};

class ThreadTimestamps {
 public:
  void sync(const ThreadTimestamps& src);
  DSchedTimestamp advance(DSchedThreadId tid);

  void setIfNotPresent(DSchedThreadId tid, DSchedTimestamp ts);
  void clear();
  bool atLeastAsRecentAs(DSchedThreadId tid, DSchedTimestamp ts) const;
  bool atLeastAsRecentAsAny(const ThreadTimestamps& src) const;

 private:
  std::vector<DSchedTimestamp> timestamps_;
};

struct ThreadInfo {
  ThreadInfo() = delete;
  explicit ThreadInfo(DSchedThreadId tid) {
    acqRelOrder_.setIfNotPresent(tid, DSchedTimestamp::initial());
  }
  ThreadTimestamps acqRelOrder_;
  ThreadTimestamps acqFenceOrder_;
  ThreadTimestamps relFenceOrder_;
};

class ThreadSyncVar {
 public:
  ThreadSyncVar() = default;

  void acquire();
  void release();
  void acq_rel();

 private:
  ThreadTimestamps order_;
};

/**
 * DeterministicSchedule coordinates the inter-thread communication of a
 * set of threads under test, so that despite concurrency the execution is
 * the same every time.  It works by stashing a reference to the schedule
 * in a thread-local variable, then blocking all but one thread at a time.
 *
 * In order for DeterministicSchedule to work, it needs to intercept
 * all inter-thread communication.  To do this you should use
 * DeterministicAtomic<T> instead of std::atomic<T>, create threads
 * using DeterministicSchedule::thread() instead of the std::thread
 * constructor, DeterministicSchedule::join(thr) instead of thr.join(),
 * and access semaphores via the helper functions in DeterministicSchedule.
 * Locks are not yet supported, although they would be easy to add with
 * the same strategy as the mapping of Sem::wait.
 *
 * The actual schedule is defined by a function from n -> [0,n). At
 * each step, the function will be given the number of active threads
 * (n), and it returns the index of the thread that should be run next.
 * Invocations of the scheduler function will be serialized, but will
 * occur from multiple threads.  A good starting schedule is uniform(0).
 */
class DeterministicSchedule {
 public:
  using Sem = Semaphore;

  /**
   * Arranges for the current thread (and all threads created by
   * DeterministicSchedule::thread on a thread participating in this
   * schedule) to participate in a deterministic schedule.
   */
  explicit DeterministicSchedule(std::function<size_t(size_t)> scheduler);

  DeterministicSchedule(
      std::function<size_t(size_t)> scheduler, FutexObserver futexObserver);

  DeterministicSchedule(const DeterministicSchedule&) = delete;
  DeterministicSchedule& operator=(const DeterministicSchedule&) = delete;

  /** Completes the schedule. */
  ~DeterministicSchedule();

  /**
   * Returns a scheduling function that randomly chooses one of the
   * runnable threads at each step, with no history.  This implements
   * a schedule that is equivalent to one in which the steps between
   * inter-thread communication are random variables following a poisson
   * distribution.
   */
  static std::function<size_t(size_t)> uniform(uint64_t seed);

  /**
   * Returns a scheduling function that chooses a subset of the active
   * threads and randomly chooses a member of the subset as the next
   * runnable thread.  The subset is chosen with size n, and the choice
   * is made every m steps.
   */
  static std::function<size_t(size_t)> uniformSubset(
      uint64_t seed, size_t n = 2, size_t m = 64);

  /** Obtains permission for the current thread to perform inter-thread
   *  communication. */
  static void beforeSharedAccess();

  /** Releases permission for the current thread to perform inter-thread
   *  communication. */
  static void afterSharedAccess();

  /** Calls a user-defined auxiliary function if any, and releases
   *  permission for the current thread to perform inter-thread
   *  communication. The bool parameter indicates the success of the
   *  shared access (if conditional, true otherwise). */
  static void afterSharedAccess(bool success);

  /** Launches a thread that will participate in the same deterministic
   *  schedule as the current thread. */
  template <typename Func, typename... Args>
  static inline std::thread thread(Func&& func, Args&&... args) {
    // TODO: maybe future versions of gcc will allow forwarding to thread
    atomic_thread_fence(std::memory_order_seq_cst);
    auto sched = getCurrentSchedule();
    auto sem = sched ? sched->beforeThreadCreate() : nullptr;
    auto child = std::thread(
        [=](Args... a) {
          if (sched) {
            sched->afterThreadCreate(sem);
            beforeSharedAccess();
            FOLLY_TEST_DSCHED_VLOG("running");
            afterSharedAccess();
          }
          SCOPE_EXIT {
            if (sched) {
              sched->beforeThreadExit();
            }
          };
          func(a...);
        },
        args...);
    if (sched) {
      beforeSharedAccess();
      sched->active_.insert(child.get_id());
      FOLLY_TEST_DSCHED_VLOG("forked " << std::hex << child.get_id());
      afterSharedAccess();
    }
    return child;
  }

  /** Calls child.join() as part of a deterministic schedule. */
  static void join(std::thread& child);

  /** Waits for each thread in children to reach the end of their
   * thread function without allowing them to fully terminate. Then,
   * allow one child at a time to fully terminate and join each one.
   * This functionality is important to protect shared access that can
   * take place after beforeThreadExit() has already been invoked,
   * for example when executing thread local destructors.
   */
  static void joinAll(std::vector<std::thread>& children);

  /** Calls sem->post() as part of a deterministic schedule. */
  static void post(Sem* sem);

  /** Calls sem->try_wait() as part of a deterministic schedule, returning
   *  true on success and false on transient failure. */
  static bool tryWait(Sem* sem);

  /** Calls sem->wait() as part of a deterministic schedule. */
  static void wait(Sem* sem);

  /** Used scheduler_ to get a random number b/w [0, n). If tls_sched is
   *  not set-up it falls back to std::rand() */
  static size_t getRandNumber(size_t n);

  /** Deterministic implemencation of getcpu */
  static int getcpu(unsigned* cpu, unsigned* node, void* unused);

  /** Sets up a thread-specific function for call immediately after
   *  the next shared access by the thread for managing auxiliary
   *  data. The function takes a bool parameter that indicates the
   *  success of the shared access (if it is conditional, true
   *  otherwise). The function is cleared after one use. */
  static void setAuxAct(AuxAct& aux);

  /** Sets up a function to be called after every subsequent shared
   *  access (until clearAuxChk() is called) for checking global
   *  invariants and logging. The function takes a uint64_t parameter
   *  that indicates the number of shared accesses so far. */
  static void setAuxChk(AuxChk& aux);

  /** Clears the function set by setAuxChk */
  static void clearAuxChk();

  /** Remove the current thread's semaphore from sems_ */
  static Sem* descheduleCurrentThread();

  /** Returns true if the current thread has already completed
   * the thread function, for example if the thread is executing
   * thread local destructors. */
  static bool isCurrentThreadExiting();

  /** Add sem back into sems_ */
  static void reschedule(Sem* sem);

  static bool isActive();

  static DSchedThreadId getThreadId();

  static ThreadInfo& getCurrentThreadInfo();

  static void atomic_thread_fence(std::memory_order mo);

  /** Reports a deterministic futex queue transition to the observer. */
  static void notifyFutexEvent(const DeterministicFutexEvent& event);

 private:
  static DeterministicSchedule* getCurrentSchedule();

  static AuxChk aux_chk;

  std::function<size_t(size_t)> scheduler_;
  FutexObserver futexObserver_;
  std::vector<Sem*> sems_;
  std::unordered_set<std::thread::id> active_;
  std::unordered_map<std::thread::id, Sem*> joins_;
  std::unordered_map<std::thread::id, Sem*> exitingSems_;

  std::vector<ThreadInfo> threadInfoMap_;
  ThreadTimestamps seqCstFenceOrder_;

  unsigned nextThreadId_;
  /* step_ keeps count of shared accesses that correspond to user
   * synchronization steps (atomic accesses for now).
   * The reason for keeping track of this here and not just with
   * auxiliary data is to provide users with warning signs (e.g.,
   * skipped steps) if they inadvertently forget to set up aux
   * functions for some shared accesses. */
  uint64_t step_;

  Sem* beforeThreadCreate();
  void afterThreadCreate(Sem*);
  void beforeThreadExit();
  void waitForBeforeThreadExit(std::thread& child);
  /** Calls user-defined auxiliary function (if any) */
  void callAux(bool);
};

/**
 * DeterministicAtomic<T> is a drop-in replacement std::atomic<T> that
 * cooperates with DeterministicSchedule.
 */
template <
    typename T,
    typename Schedule = DeterministicSchedule,
    template <typename> class Atom = std::atomic>
struct DeterministicAtomicImpl {
  DeterministicAtomicImpl() = default;
  ~DeterministicAtomicImpl() = default;
  DeterministicAtomicImpl(DeterministicAtomicImpl<T> const&) = delete;
  DeterministicAtomicImpl<T>& operator=(DeterministicAtomicImpl<T> const&) =
      delete;

  constexpr /* implicit */ DeterministicAtomicImpl(T v) noexcept : data_(v) {}

  bool is_lock_free() const noexcept { return data_.is_lock_free(); }

  bool compare_exchange_strong(
      T& v0, T v1, std::memory_order mo = std::memory_order_seq_cst) noexcept {
    return compare_exchange_strong(
        v0, v1, mo, ::folly::detail::default_failure_memory_order(mo));
  }
  bool compare_exchange_strong(
      T& v0,
      T v1,
      std::memory_order success,
      std::memory_order failure) noexcept {
    Schedule::beforeSharedAccess();
    auto orig = v0;
    bool rv = data_.compare_exchange_strong(v0, v1, success, failure);
    FOLLY_TEST_DSCHED_VLOG(
        this << ".compare_exchange_strong(" << std::hex << orig << ", "
             << std::hex << v1 << ") -> " << rv << "," << std::hex << v0);
    Schedule::afterSharedAccess(rv);
    return rv;
  }

  bool compare_exchange_weak(
      T& v0, T v1, std::memory_order mo = std::memory_order_seq_cst) noexcept {
    return compare_exchange_weak(
        v0, v1, mo, ::folly::detail::default_failure_memory_order(mo));
  }
  bool compare_exchange_weak(
      T& v0,
      T v1,
      std::memory_order success,
      std::memory_order failure) noexcept {
    Schedule::beforeSharedAccess();
    auto orig = v0;
    bool rv = data_.compare_exchange_weak(v0, v1, success, failure);
    FOLLY_TEST_DSCHED_VLOG(
        this << ".compare_exchange_weak(" << std::hex << orig << ", "
             << std::hex << v1 << ") -> " << rv << "," << std::hex << v0);
    Schedule::afterSharedAccess(rv);
    return rv;
  }

  T exchange(T v, std::memory_order mo = std::memory_order_seq_cst) noexcept {
    Schedule::beforeSharedAccess();
    T rv = data_.exchange(v, mo);
    FOLLY_TEST_DSCHED_VLOG(
        this << ".exchange(" << std::hex << v << ") -> " << std::hex << rv);
    Schedule::afterSharedAccess(true);
    return rv;
  }

  /* implicit */ operator T() const noexcept {
    Schedule::beforeSharedAccess();
    T rv = data_.operator T();
    FOLLY_TEST_DSCHED_VLOG(this << "() -> " << std::hex << rv);
    Schedule::afterSharedAccess(true);
    return rv;
  }

  T load(std::memory_order mo = std::memory_order_seq_cst) const noexcept {
    Schedule::beforeSharedAccess();
    T rv = data_.load(mo);
    FOLLY_TEST_DSCHED_VLOG(this << ".load() -> " << std::hex << rv);
    Schedule::afterSharedAccess(true);
    return rv;
  }

  T operator=(T v) noexcept {
    Schedule::beforeSharedAccess();
    T rv = (data_ = v);
    FOLLY_TEST_DSCHED_VLOG(this << " = " << std::hex << v);
    Schedule::afterSharedAccess(true);
    return rv;
  }

  void store(T v, std::memory_order mo = std::memory_order_seq_cst) noexcept {
    Schedule::beforeSharedAccess();
    data_.store(v, mo);
    FOLLY_TEST_DSCHED_VLOG(this << ".store(" << std::hex << v << ")");
    Schedule::afterSharedAccess(true);
  }

  T operator++() noexcept {
    Schedule::beforeSharedAccess();
    T rv = ++data_;
    FOLLY_TEST_DSCHED_VLOG(this << " pre++ -> " << std::hex << rv);
    Schedule::afterSharedAccess(true);
    return rv;
  }

  T operator++(int /* postDummy */) noexcept {
    Schedule::beforeSharedAccess();
    T rv = data_++;
    FOLLY_TEST_DSCHED_VLOG(this << " post++ -> " << std::hex << rv);
    Schedule::afterSharedAccess(true);
    return rv;
  }

  T operator--() noexcept {
    Schedule::beforeSharedAccess();
    T rv = --data_;
    FOLLY_TEST_DSCHED_VLOG(this << " pre-- -> " << std::hex << rv);
    Schedule::afterSharedAccess(true);
    return rv;
  }

  T operator--(int /* postDummy */) noexcept {
    Schedule::beforeSharedAccess();
    T rv = data_--;
    FOLLY_TEST_DSCHED_VLOG(this << " post-- -> " << std::hex << rv);
    Schedule::afterSharedAccess(true);
    return rv;
  }

  T operator+=(T v) noexcept {
    Schedule::beforeSharedAccess();
    T rv = (data_ += v);
    FOLLY_TEST_DSCHED_VLOG(
        this << " += " << std::hex << v << " -> " << std::hex << rv);
    Schedule::afterSharedAccess(true);
    return rv;
  }

  T fetch_add(T v, std::memory_order mo = std::memory_order_seq_cst) noexcept {
    Schedule::beforeSharedAccess();
    T rv = data_.fetch_add(v, mo);
    FOLLY_TEST_DSCHED_VLOG(
        this << ".fetch_add(" << std::hex << v << ") -> " << std::hex << rv);
    Schedule::afterSharedAccess(true);
    return rv;
  }

  T operator-=(T v) noexcept {
    Schedule::beforeSharedAccess();
    T rv = (data_ -= v);
    FOLLY_TEST_DSCHED_VLOG(
        this << " -= " << std::hex << v << " -> " << std::hex << rv);
    Schedule::afterSharedAccess(true);
    return rv;
  }

  T fetch_sub(T v, std::memory_order mo = std::memory_order_seq_cst) noexcept {
    Schedule::beforeSharedAccess();
    T rv = data_.fetch_sub(v, mo);
    FOLLY_TEST_DSCHED_VLOG(
        this << ".fetch_sub(" << std::hex << v << ") -> " << std::hex << rv);
    Schedule::afterSharedAccess(true);
    return rv;
  }

#if defined(__cpp_lib_atomic_min_max) && __cpp_lib_atomic_min_max >= 202403L
  T fetch_min(T v, std::memory_order mo = std::memory_order_seq_cst) noexcept
    requires(is_non_bool_integral_v<T> || std::is_pointer_v<T>)
  {
    Schedule::beforeSharedAccess();
    T rv = data_.fetch_min(v, mo);
    FOLLY_TEST_DSCHED_VLOG(
        this << ".fetch_min(" << std::hex << v << ") -> " << std::hex << rv);
    Schedule::afterSharedAccess(true);
    return rv;
  }

  T fetch_max(T v, std::memory_order mo = std::memory_order_seq_cst) noexcept
    requires(is_non_bool_integral_v<T> || std::is_pointer_v<T>)
  {
    Schedule::beforeSharedAccess();
    T rv = data_.fetch_max(v, mo);
    FOLLY_TEST_DSCHED_VLOG(
        this << ".fetch_max(" << std::hex << v << ") -> " << std::hex << rv);
    Schedule::afterSharedAccess(true);
    return rv;
  }
#endif

  T operator&=(T v) noexcept {
    Schedule::beforeSharedAccess();
    T rv = (data_ &= v);
    FOLLY_TEST_DSCHED_VLOG(
        this << " &= " << std::hex << v << " -> " << std::hex << rv);
    Schedule::afterSharedAccess(true);
    return rv;
  }

  T fetch_and(T v, std::memory_order mo = std::memory_order_seq_cst) noexcept {
    Schedule::beforeSharedAccess();
    T rv = data_.fetch_and(v, mo);
    FOLLY_TEST_DSCHED_VLOG(
        this << ".fetch_and(" << std::hex << v << ") -> " << std::hex << rv);
    Schedule::afterSharedAccess(true);
    return rv;
  }

  T operator|=(T v) noexcept {
    Schedule::beforeSharedAccess();
    T rv = (data_ |= v);
    FOLLY_TEST_DSCHED_VLOG(
        this << " |= " << std::hex << v << " -> " << std::hex << rv);
    Schedule::afterSharedAccess(true);
    return rv;
  }

  T fetch_or(T v, std::memory_order mo = std::memory_order_seq_cst) noexcept {
    Schedule::beforeSharedAccess();
    T rv = data_.fetch_or(v, mo);
    FOLLY_TEST_DSCHED_VLOG(
        this << ".fetch_or(" << std::hex << v << ") -> " << std::hex << rv);
    Schedule::afterSharedAccess(true);
    return rv;
  }

  T operator^=(T v) noexcept {
    Schedule::beforeSharedAccess();
    T rv = (data_ ^= v);
    FOLLY_TEST_DSCHED_VLOG(
        this << " ^= " << std::hex << v << " -> " << std::hex << rv);
    Schedule::afterSharedAccess(true);
    return rv;
  }

  T fetch_xor(T v, std::memory_order mo = std::memory_order_seq_cst) noexcept {
    Schedule::beforeSharedAccess();
    T rv = data_.fetch_xor(v, mo);
    FOLLY_TEST_DSCHED_VLOG(
        this << ".fetch_xor(" << std::hex << v << ") -> " << std::hex << rv);
    Schedule::afterSharedAccess(true);
    return rv;
  }

  /** Read the value of the atomic variable without context switching */
  T load_direct() const noexcept {
    return data_.load(std::memory_order_relaxed);
  }

 private:
  Atom<T> data_;
};

template <typename T>
using DeterministicAtomic = DeterministicAtomicImpl<T, DeterministicSchedule>;

} // namespace test

template <>
struct atomic_thread_fence_traits<test::DeterministicAtomic> {
  static inline constexpr auto fence =
      test::DeterministicSchedule::atomic_thread_fence;
};

template <>
struct asymmetric_thread_fence_traits<test::DeterministicAtomic> {
  using traits = atomic_thread_fence_traits<test::DeterministicAtomic>;
  static inline constexpr auto light = traits::fence;
  static inline constexpr auto heavy = traits::fence;
};

namespace test {

/* Futex extensions for DeterministicSchedule based Futexes */
int futexWakeImpl(
    const folly::detail::Futex<test::DeterministicAtomic>* futex,
    int count,
    uint32_t wakeMask);
folly::detail::FutexResult futexWaitImpl(
    const folly::detail::Futex<test::DeterministicAtomic>* futex,
    uint32_t expected,
    std::chrono::system_clock::time_point const* absSystemTime,
    std::chrono::steady_clock::time_point const* absSteadyTime,
    uint32_t waitMask);

/* Generic futex extensions to allow sharing between DeterministicAtomic and
 * BufferedDeterministicAtomic.*/
template <template <typename> class Atom>
int deterministicFutexWakeImpl(
    const folly::detail::Futex<Atom>* futex,
    std::mutex& futexLock,
    std::unordered_map<
        const folly::detail::Futex<Atom>*,
        std::list<std::pair<uint32_t, bool*>>>& futexQueues,
    int count,
    uint32_t wakeMask) {
  using namespace test;
  using namespace std::chrono;

  int rv = 0;
  DeterministicSchedule::beforeSharedAccess();
  futexLock.lock();
  if (futexQueues.count(futex) > 0) {
    auto& queue = futexQueues[futex];
    auto iter = queue.begin();
    while (iter != queue.end() && rv < count) {
      auto cur = iter++;
      if ((cur->first & wakeMask) != 0) {
        *(cur->second) = true;
        rv++;
        queue.erase(cur);
      }
    }
    if (queue.empty()) {
      futexQueues.erase(futex);
    }
  }
  futexLock.unlock();
  DeterministicSchedule::notifyFutexEvent(
      {DeterministicFutexEvent::Kind::Wake, futex, wakeMask, count, rv});
  FOLLY_TEST_DSCHED_VLOG(
      "futexWake(" << futex << ", " << count << ", " << std::hex << wakeMask
                   << ") -> " << rv);
  DeterministicSchedule::afterSharedAccess();
  return rv;
}

template <template <typename> class Atom>
folly::detail::FutexResult deterministicFutexWaitImpl(
    const folly::detail::Futex<Atom>* futex,
    std::mutex& futexLock,
    std::unordered_map<
        const folly::detail::Futex<Atom>*,
        std::list<std::pair<uint32_t, bool*>>>& futexQueues,
    uint32_t expected,
    std::chrono::system_clock::time_point const* absSystemTimeout,
    std::chrono::steady_clock::time_point const* absSteadyTimeout,
    uint32_t waitMask) {
  using namespace test;
  using namespace std::chrono;
  using namespace folly::detail;

  bool hasTimeout = absSystemTimeout != nullptr || absSteadyTimeout != nullptr;
  bool awoken = false;
  FutexResult result = FutexResult::AWOKEN;

  DeterministicSchedule::beforeSharedAccess();
  FOLLY_TEST_DSCHED_VLOG(
      "futexWait(" << futex << ", " << std::hex << expected << ", .., "
                   << std::hex << waitMask << ") beginning..");
  futexLock.lock();
  // load_direct avoids deadlock on inner call to beforeSharedAccess
  if (futex->load_direct() == expected) {
    auto& queue = futexQueues[futex];
    queue.emplace_back(waitMask, &awoken);
    auto ours = queue.end();
    ours--;
    bool notifyQueued = true;
    while (!awoken) {
      futexLock.unlock();
      if (notifyQueued) {
        notifyQueued = false;
        DeterministicSchedule::notifyFutexEvent(
            {DeterministicFutexEvent::Kind::WaitQueued, futex, waitMask, 0, 0});
      }
      DeterministicSchedule::afterSharedAccess();
      DeterministicSchedule::beforeSharedAccess();
      futexLock.lock();

      // Simulate spurious wake-ups, timeouts each time with
      // a 10% probability if we haven't been woken up already
      if (!awoken && hasTimeout &&
          DeterministicSchedule::getRandNumber(100) < 10) {
        assert(futexQueues.count(futex) != 0 && &futexQueues[futex] == &queue);
        queue.erase(ours);
        if (queue.empty()) {
          futexQueues.erase(futex);
        }
        // Simulate ETIMEDOUT 90% of the time and other failures
        // remaining time
        result = DeterministicSchedule::getRandNumber(100) >= 10
            ? FutexResult::TIMEDOUT
            : FutexResult::INTERRUPTED;
        break;
      }
    }
  } else {
    result = FutexResult::VALUE_CHANGED;
  }
  futexLock.unlock();

  char const* resultStr = "?";
  switch (result) {
    case FutexResult::AWOKEN:
      resultStr = "AWOKEN";
      break;
    case FutexResult::TIMEDOUT:
      resultStr = "TIMEDOUT";
      break;
    case FutexResult::INTERRUPTED:
      resultStr = "INTERRUPTED";
      break;
    case FutexResult::VALUE_CHANGED:
      resultStr = "VALUE_CHANGED";
      break;
  }
  FOLLY_TEST_DSCHED_VLOG(
      "futexWait(" << futex << ", " << std::hex << expected << ", .., "
                   << std::hex << waitMask << ") -> " << resultStr);
  DeterministicSchedule::afterSharedAccess();
  return result;
}

/**
 * Implementations of the atomic_wait API for DeterministicAtomic, these are
 * no-ops here.  Which for a correct implementation should not make a
 * difference because threads are required to have atomic operations around
 * waits and wakes
 */
template <typename Integer>
void tag_invoke(
    cpo_t<atomic_wait>, const DeterministicAtomic<Integer>*, Integer) {}
template <typename Integer, typename Clock, typename Duration>
std::cv_status tag_invoke(
    cpo_t<atomic_wait_until>,
    const DeterministicAtomic<Integer>*,
    Integer,
    const std::chrono::time_point<Clock, Duration>&) {
  return std::cv_status::no_timeout;
}
template <typename Integer>
void tag_invoke(cpo_t<atomic_notify_one>, const DeterministicAtomic<Integer>*) {
}
template <typename Integer>
void tag_invoke(cpo_t<atomic_notify_all>, const DeterministicAtomic<Integer>*) {
}

/**
 * DeterministicMutex is a drop-in replacement of std::mutex that
 * cooperates with DeterministicSchedule.
 */
struct DeterministicMutex {
  using Sem = DeterministicSchedule::Sem;

  std::mutex m;
  std::queue<Sem*> waiters_;
  ThreadSyncVar syncVar_;

  DeterministicMutex() = default;
  ~DeterministicMutex() = default;
  DeterministicMutex(DeterministicMutex const&) = delete;
  DeterministicMutex& operator=(DeterministicMutex const&) = delete;

  void lock() {
    FOLLY_TEST_DSCHED_VLOG(this << ".lock()");
    DeterministicSchedule::beforeSharedAccess();
    while (!m.try_lock()) {
      Sem* sem = DeterministicSchedule::descheduleCurrentThread();
      if (sem) {
        waiters_.push(sem);
      }
      DeterministicSchedule::afterSharedAccess();
      // Wait to be scheduled by unlock
      DeterministicSchedule::beforeSharedAccess();
    }
    if (DeterministicSchedule::isActive()) {
      syncVar_.acquire();
    }
    DeterministicSchedule::afterSharedAccess();
  }

  bool try_lock() {
    DeterministicSchedule::beforeSharedAccess();
    bool rv = m.try_lock();
    if (rv && DeterministicSchedule::isActive()) {
      syncVar_.acquire();
    }
    FOLLY_TEST_DSCHED_VLOG(this << ".try_lock() -> " << rv);
    DeterministicSchedule::afterSharedAccess();
    return rv;
  }

  void unlock() {
    FOLLY_TEST_DSCHED_VLOG(this << ".unlock()");
    DeterministicSchedule::beforeSharedAccess();
    m.unlock();
    if (DeterministicSchedule::isActive()) {
      syncVar_.release();
    }
    if (!waiters_.empty()) {
      Sem* sem = waiters_.front();
      DeterministicSchedule::reschedule(sem);
      waiters_.pop();
    }
    DeterministicSchedule::afterSharedAccess();
  }
};
} // namespace test

template <>
Getcpu::Func AccessSpreader<test::DeterministicAtomic>::pickGetcpuFunc();

} // namespace folly
