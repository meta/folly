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
#include <functional>
#include <limits>

#include <folly/Function.h>
#include <folly/Indestructible.h>
#include <folly/Optional.h>
#include <folly/detail/TurnSequencer.h>
#include <folly/executors/QueuedImmediateExecutor.h>
#include <folly/synchronization/detail/ThreadCachedLists.h>
#include <folly/synchronization/detail/ThreadCachedReaders.h>

// Implementation of proposed Read-Copy-Update (RCU) C++ API
// http://open-std.org/JTC1/SC22/WG21/docs/papers/2017/p0566r3.pdf

// Overview

// This file provides a low-overhead mechanism to guarantee ordering
// between operations on shared data. In the simplest usage pattern,
// readers enter a critical section, view some state, and leave the
// critical section, while writers modify shared state and then defer
// some cleanup operations. Proper use of these classes will guarantee
// that a cleanup operation that is deferred during a reader critical
// section will not be executed until after that critical section is
// over.

// Example

// As an example, suppose we have some configuration data that gets
// periodically updated. We need to protect ourselves on *every* read
// path, even if updates are only vanishingly rare, because we don't know
// if some writer will come along and yank the data out from under us.
//
// Here's how that might look without deferral:
//
// void doSomething(IPAddress host);
//
// folly::SharedMutex sm;
// ConfigData* globalConfigData;
//
// void reader() {
//   while (true) {
//     sm.lock_shared();
//     IPAddress curManagementServer = globalConfigData->managementServerIP;
//     sm.unlock_shared();
//     doSomethingWith(curManagementServer);
//   }
// }
//
// void writer() {
//   while (true) {
//     std::this_thread::sleep_for(std::chrono::seconds(60));
//     ConfigData* oldConfigData;
//     ConfigData* newConfigData = loadConfigDataFromRemoteServer();
//     sm.lock();
//     oldConfigData = globalConfigData;
//     globalConfigData = newConfigData;
//     sm.unlock();
//     delete oldConfigData;
//   }
// }
//
// The readers have to lock_shared and unlock_shared every iteration, even
// during the overwhelming majority of the time in which there's no writer
// present. These functions are surprisingly expensive; there's around 30ns of
// overhead per critical section on my machine.
//
// Let's get rid of the locking. The readers and writer may proceed
// concurrently. Here's how this would look:
//
// void doSomething(IPAddress host);
//
// std::atomic<ConfigData*> globalConfigData;
//
// void reader() {
//   while (true) {
//     ConfigData* configData = globalConfigData.load();
//     IPAddress curManagementServer = configData->managementServerIP;
//     doSomethingWith(curManagementServer);
//  }
// }
//
// void writer() {
//   while (true) {
//     std::this_thread::sleep_for(std::chrono::seconds(60));
//     ConfigData* newConfigData = loadConfigDataFromRemoteServer();
//     globalConfigData.store(newConfigData);
//     // We can't delete the old ConfigData; we don't know when the readers
//     // will be done with it! I guess we have to leak it...
//   }
// }
//
// This works and is fast, but we don't ever reclaim the memory we
// allocated for the copy of the data. We need a way for the writer to
// know that no readers observed the old value of the pointer and are
// still using it. Tweaking this slightly, we want a way for the
// writer to say "I want some operation (deleting the old ConfigData)
// to happen, but only after I know that all readers that started
// before I requested the action have finished.". The classes in this
// namespace allow this. Here's how we'd express this idea:
//
// void doSomething(IPAddress host);
// std::atomic<ConfigData*> globalConfigData;
//
//
// void reader() {
//   while (true) {
//     IPAddress curManagementServer;
//     {
//       // We're about to do some reads we want to protect; if we read a
//       // pointer, we need to make sure that if the writer comes along and
//       // updates it, the writer's cleanup operation won't happen until we're
//       // done accessing the pointed-to data. We get a Guard on that
//       // domain; as long as it exists, no function subsequently passed to
//       // invokeEventually will execute.
//       std::scoped_lock<rcu_domain> guard(rcu_default_domain());
//       ConfigData* configData = globalConfigData.load();
//       // We created a guard before we read globalConfigData; we know that the
//       // pointer will remain valid until the guard is destroyed.
//       curManagementServer = configData->managementServerIP;
//       // RCU domain via the scoped mutex is released here; retired objects
//       // may be freed.
//     }
//     doSomethingWith(curManagementServer);
//   }
// }
//
// void writer() {
//
//   while (true) {
//     std::this_thread::sleep_for(std::chrono::seconds(60));
//     ConfigData* oldConfigData = globalConfigData.load();
//     ConfigData* newConfigData = loadConfigDataFromRemoteServer();
//     globalConfigData.store(newConfigData);
//     rcu_retire(oldConfigData);
//     // alternatively, in a blocking manner:
//     //   rcu_synchronize();
//     //   delete oldConfigData;
//   }
// }
//
// This gets us close to the speed of the second solution, without
// leaking memory. An std::scoped_lock costs about 5 ns, faster than the
// lock() / unlock() pair in the more traditional mutex-based approach from our
// first attempt, and the writer never blocks the readers.

// Notes

// This implementation does implement an rcu_domain, and provides a default
// one for use per the standard implementation.
//
// rcu_domain:
//   A "universe" of deferred execution. Each rcu_domain has an
//   executor on which deferred functions may execute. Readers enter
//   a read region in an rcu_domain by creating an instance of an
//   std::scoped_lock<folly::rcu_domain> object on the domain. The scoped
//   lock provides RAII semantics for read region protection over the domain.
//
//   It's intended that most callers should only ever use the default, global
//   domain. Multiple rcu_domains should in general be disjoint;
//   std::scoped_lock<folly::rcu_domain> locks created on one domain do not
//   prevent functions deferred on other domains from running.
//
//   You should use a custom rcu_domain if you can't avoid sleeping
//   during reader critical sections (because you don't want to block
//   all other users of the domain while you sleep), or you want to change
//   the default executor type on which retire callbacks are invoked.
//   Otherwise, users are strongly encouraged to use the default domain.
//
// API correctness limitations:
//
//  - fork():
//    Invoking fork() in a multithreaded program with any thread other than the
//    forking thread being present in a read region will result in undefined
//    behavior. Similarly, a forking thread must immediately invoke exec if
//    fork() is invoked while in a read region. Invoking fork() inside of a read
//    region, and then exiting before invoking exec(), will similarly result in
//    undefined behavior.
//
//  - Exceptions:
//    In short, nothing about this is exception safe. retire functions should
//    not throw exceptions in their destructors, move constructors or call
//    operators.
//
// Performance limitations:
//  - Blocking:
//    A blocked reader will block invocation of deferred functions until it
//    becomes unblocked. Sleeping while holding an
//    std::scoped_lock<folly::rcu_domain> lock can have bad performance
//    consequences.
//
// API questions you might have:
//  - Nested critical sections:
//    These are fine. The following is explicitly allowed by the standard, up to
//    a nesting level of 100:
//        std::scoped_lock<rcu_domain> reader1(rcu_default_domain());
//        doSomeWork();
//        std::scoped_lock<rcu_domain> reader2(rcu_default_domain());
//        doSomeMoreWork();
//  - Restrictions on retired()ed functions:
//    Any operation is safe from within a retired function's
//    execution; you can retire additional functions or add a new domain call to
//    the domain.  However, when using the default domain or the default
//    executor, it is not legal to hold a lock across rcu_retire or call
//    that is acquired by the deleter.  This is normally not a problem when
//    using the default deleter delete, which does not acquire any user locks.
//    However, even when using the default deleter, an object having a
//    user-defined destructor that acquires locks held across the corresponding
//    call to rcu_retire can still deadlock.
//  - rcu_domain destruction:
//    Destruction of a domain assumes previous synchronization: all remaining
//    call and retire calls are immediately added to the executor.

// Overheads

// Deferral latency is as low as is practical: overhead involves running
// several global memory barriers on the machine to ensure all readers are gone.
//
// Currently use of MPMCQueue is the bottleneck for deferred calls, more
// specialized queues could be used if available, since only a single reader
// reads the queue, and can splice all of the items to the executor if possible.
//
// rcu_synchronize() call latency is on the order of 10ms.  Multiple
// separate threads can share a synchronized period and should scale.
//
// rcu_retire() is a queue push, and on the order of 150 ns, however,
// the current implementation may synchronize if the retire queue is full,
// resulting in tail latencies of ~10ms.
//
// std::scoped_lock<rcu_domain> creation/destruction is ~5ns.  By comparison,
// folly::SharedMutex::lock_shared + unlock_shared pair is ~26ns

// Hazard pointers vs. RCU:
//
// Hazard pointers protect pointers, generally malloc()d pieces of memory, and
// each hazptr only protects one such block.
//
// RCU protects critical sections, *all* memory is protected as long
// as the critical section is active.
//
// For example, this has implications for linked lists: If you wish to
// free an entire linked list, you simply rcu_retire() each node with
// RCU: readers will see either an entirely valid list, or no list at
// all.
//
// Conversely with hazptrs, generally lists are walked with
// hand-over-hand hazptrs.  Only one node is protected at a time.  So
// anywhere in the middle of the list, hazptrs may read NULL, and throw
// away all current work.  Alternatively, reference counting can be used,
// (as if each node was a shared_ptr<node>), so that readers will always see
// *the remaining part* of the list as valid, however parts before its current
// hazptr may be freed.
//
// So roughly: RCU is simple, but an all-or-nothing affair.  A single
// std::scoped_lock<folly::rcu_domain> can block all reclamation. Hazptrs will
// reclaim exactly as much as possible, at the cost of extra work writing
// traversal code
//
// Reproduced from
// http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2021/n4895.pdf
//
//                              Reference Counting    RCU            Hazptr
//
// Unreclaimed objects          Bounded               Unbounded      Bounded
//
// Contention among readers     High                  None           None
//
// Traversal forward progress   lock-free             wait-free      lock-free
//
// Reclamation forward progress lock-free             blocking       wait-free
//
// Traversal speed              atomic                no-overhead    folly's is
//                                                                   no-overhead
//
// Reference acquisition        unconditional         unconditional  conditional
//
// Automatic reclamation        yes                   no             no
//
// Purpose of domains           N/A                   isolate slow configuration
//                                                    readers

namespace folly {

// Defines an RCU domain.  RCU readers within a given domain block updaters
// (rcu_synchronize, call, retire, or rcu_retire) only within that same
// domain, and have no effect on updaters associated with other rcu_domains.
//
// Custom domains are normally not necessary because the default domain works
// in most cases.  But it makes sense to create a separate domain for uses
// having unusually long read-side critical sections (many milliseconds)
// or uses that cannot tolerate moderately long read-side critical sections
// from others.
//
// The executor runs grace-period processing and invokes deleters.
// The default of QueuedImmediateExecutor is very light weight (compared
// to, say, a thread pool).  However, the flip side of this light weight
// is that the overhead of this processing and invocation is incurred within
// the executor invoking the RCU primitive, for example, rcu_retire().
//
// The domain must survive all its readers.
class rcu_domain {
  using list_head = typename detail::ThreadCachedLists::ListHead;
  using list_node = typename detail::ThreadCachedLists::Node;

 public:
  /*
   * If an executor is passed, it is used to run calls and delete
   * retired objects.
   */
  explicit rcu_domain(Executor* executor = nullptr) noexcept
      : executor_(executor ? executor : &QueuedImmediateExecutor::instance()) {}

  rcu_domain(const rcu_domain&) = delete;
  rcu_domain(rcu_domain&&) = delete;
  rcu_domain& operator=(const rcu_domain&) = delete;
  rcu_domain& operator=(rcu_domain&&) = delete;

  // Reader locks: Prevent any calls from occurring, retired memory
  // from being freed, and synchronize() calls from completing until
  // all preceding lock() sections are finished.

  /*
   * Enter a read region on the current thread.
   *
   * Note that despite the function being marked noexcept, an allocation
   * may take place in folly::ThreadLocal the first time a thread enters a read
   * region. Regardless, for now, we're marking this as noexcept to match the
   * N4895 standard proposal:
   *
   * https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2021/n4895.pdf
   */
  FOLLY_ALWAYS_INLINE void lock() noexcept {
    counters_.increment(version_.load(std::memory_order_acquire));
  }
  FOLLY_ALWAYS_INLINE void unlock() noexcept { counters_.decrement(); }

  // Invokes cbin(this) and then deletes this some time after all pre-existing
  // RCU readers have completed.  See rcu_synchronize() for more information
  // about RCU readers and domains.
  template <typename T>
  void call(T&& cbin) {
    auto node = new list_node;
    node->cb_ = [node, cb = std::forward<T>(cbin)]() {
      cb();
      delete node;
    };
    retire(node);
  }

  // Invokes node->cb_(node) some time after all pre-existing RCU readers
  // have completed.  See rcu_synchronize() for more information about RCU
  // readers and domains.
  void retire(list_node* node) noexcept {
    q_.push(node);

    // Note that it's likely we hold a read lock here,
    // so we can only half_sync(false).  half_sync(true)
    // or a synchronize() call might block forever.
    uint64_t time =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    auto syncTime = syncTime_.load(std::memory_order_relaxed);
    if (time > syncTime + syncTimePeriod_ &&
        syncTime_.compare_exchange_strong(
            syncTime, time, std::memory_order_relaxed)) {
      std::unique_lock g(syncMutex_, std::try_to_lock);
      if (g.owns_lock()) {
        list_head finished;
        half_sync(false, finished);
        g.unlock();
        // callbacks are called outside of syncMutex_
        finished.forEach([&](list_node* item) {
          executor_->add(std::move(item->cb_));
        });
      }
    }
  }

  // Ensure concurrent critical sections have finished.
  // Always waits for full synchronization.
  // read lock *must not* be held.
  void synchronize() noexcept {
    auto curr = version_.load(std::memory_order_acquire);
    // Target is two epochs away.
    auto target = curr + 2;
    while (true) {
      // Try to assign ourselves to do the sync work.
      // If someone else is already assigned, we can wait for
      // the work to be finished by waiting on turn_.
      auto work = work_.load(std::memory_order_acquire);
      auto tmp = work;
      if (work < target && work_.compare_exchange_strong(tmp, target)) {
        list_head finished;
        {
          std::lock_guard g(syncMutex_);
          while (version_.load(std::memory_order_acquire) < target) {
            half_sync(true, finished);
          }
        }
        // callbacks are called outside of syncMutex_
        finished.forEach([&](list_node* node) {
          executor_->add(std::move(node->cb_));
        });
        return;
      } else {
        if (version_.load(std::memory_order_acquire) >= target) {
          return;
        }
        std::atomic<uint32_t> cutoff{100};
        // Wait for someone to finish the work.
        turn_.tryWaitForTurn(to_narrow(work), cutoff, false);
      }
    }
  }

 private:
  detail::ThreadCachedReaders counters_;
  // Global epoch.
  std::atomic<uint64_t> version_{0};
  // Future epochs being driven by threads in synchronize
  std::atomic<uint64_t> work_{0};
  // Matches version_, for waking up threads in synchronize that are
  // sharing an epoch.
  detail::TurnSequencer<std::atomic> turn_;
  // Only one thread can drive half_sync.
  std::mutex syncMutex_;
  // Currently half_sync takes ~16ms due to heavy barriers.
  // Ensure that if used in a single thread, max GC time
  // is limited to 1% of total CPU time.
  static constexpr uint64_t syncTimePeriod_{1600 * 2 /* full sync is 2x */};
  std::atomic<uint64_t> syncTime_{0};
  // call()s waiting to move through two epochs.
  detail::ThreadCachedLists q_;
  // Executor callbacks will eventually be run on.
  Executor* executor_{nullptr};

  // Queues for callbacks waiting to go through two epochs.
  list_head queues_[2]{};

  // Move queues through one epoch (half of a full synchronize()).
  // Will block waiting for readers to exit if blocking is true.
  // blocking must *not* be true if the current thread is locked,
  // or will deadlock.
  //
  // returns a list of callbacks ready to run in finished.
  void half_sync(bool blocking, list_head& finished) {
    auto curr = version_.load(std::memory_order_acquire);
    auto next = curr + 1;

    // Push all work to a queue for moving through two epochs.  One
    // version is not enough because of late readers of the version_
    // counter in lock_shared.
    //
    // Note that for a similar reason we can't swap out the q here,
    // and instead drain it, so concurrent calls to call() are safe,
    // and will wait for the next epoch.
    q_.collect(queues_[0]);

    if (blocking) {
      counters_.waitForZero(next & 1);
    } else {
      if (!counters_.epochIsClear(next & 1)) {
        return;
      }
    }

    // Run callbacks that have been through two epochs, and swap queues
    // for those only through a single epoch.
    finished.splice(queues_[1]);
    queues_[1].splice(queues_[0]);

    version_.store(next, std::memory_order_release);
    // Notify synchronous waiters in synchronize().
    turn_.completeTurn(to_narrow(curr));
  }
};

extern folly::Indestructible<rcu_domain*> rcu_default_domain_;

inline rcu_domain& rcu_default_domain() {
  return **rcu_default_domain_;
}

// Waits for all pre-existing RCU readers to complete.
// RCU readers will normally be marked using the RAII interface
// std::scoped_lock<folly::rcu_domain>, as in:
//
// std::scoped_lock<folly::rcu_domain> rcuReader(rcu_default_domain());
//
// Other locking primitives that provide moveable semantics such as
// std::unique_lock may be used as well.
//
// Note that rcu_synchronize is not obligated to wait for RCU readers that
// start after rcu_synchronize starts.  Note also that holding a lock across
// rcu_synchronize that is acquired by any deleter (as in is passed to
// rcu_retire, retire, or call) will result in deadlock.  Note that such
// deadlock will normally only occur with user-written deleters, as in the
// default of delete will normally be immune to such deadlocks.
inline void rcu_synchronize(
    rcu_domain& domain = rcu_default_domain()) noexcept {
  domain.synchronize();
}

// Waits for all in-flight deleters to complete.
//
// An in-flight deleter is one that has already been passed to rcu_retire,
// retire, or call.  In other words, rcu_barrier is not obligated to wait
// on any deleters passed to later calls to rcu_retire, retire, or call.
//
// And yes, the current implementation is buggy, and will be fixed.
inline void rcu_barrier(rcu_domain& domain = rcu_default_domain()) noexcept {
  domain.synchronize();
}

// Free-function retire.  Always allocates.
//
// This will invoke the deleter d(p) asynchronously some time after all
// pre-existing RCU readers have completed.  See rcu_synchronize() for more
// information about RCU readers and domains.
template <typename T, typename D = std::default_delete<T>>
void rcu_retire(T* p, D d = {}, rcu_domain& domain = rcu_default_domain()) {
  domain.call([p, del = std::move(d)]() { del(p); });
}

// Base class for rcu objects.  retire() will use preallocated storage
// from rcu_obj_base, vs.  rcu_retire() which always allocates.
template <typename T, typename D = std::default_delete<T>>
class rcu_obj_base : detail::ThreadCachedListsBase::Node {
 public:
  void retire(D d = {}, rcu_domain& domain = rcu_default_domain()) {
    // This implementation assumes folly::Function has enough
    // inline storage for D, otherwise, it allocates.
    this->cb_ = [this, d = std::move(d)]() { d(static_cast<T*>(this)); };
    domain.retire(this);
  }
};

} // namespace folly
