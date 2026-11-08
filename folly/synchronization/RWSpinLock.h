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

/*
 * N.B. You most likely do _not_ want to use RWSpinLock or any other
 * kind of spinlock.  Use SharedMutex instead.
 *
 * In short, spinlocks in preemptive multi-tasking operating systems
 * have serious problems and fast mutexes like SharedMutex are almost
 * certainly the better choice, because letting the OS scheduler put a
 * thread to sleep is better for system responsiveness and throughput
 * than wasting a timeslice repeatedly querying a lock held by a
 * thread that's blocked, and you can't prevent userspace
 * programs blocking.
 *
 * Spinlocks in an operating system kernel make much more sense than
 * they do in userspace.
 *
 * -------------------------------------------------------------------
 *
 * Two Read-Write spin lock implementations.
 *
 *  Ref: http://locklessinc.com/articles/locks
 *
 *  Both locks here are faster than pthread_rwlock and have very low
 *  overhead (usually 20-30ns).  They don't use any system mutexes and
 *  are very compact (4/8 bytes), so are suitable for per-instance
 *  based locking, particularly when contention is not expected.
 *
 *  For a spinlock, RWSpinLock is a reasonable choice.  (See the note
 *  about for why a spin lock is frequently a bad idea generally.)
 *  RWSpinLock has minimal overhead, and comparable contention
 *  performance when the number of competing threads is less than or
 *  equal to the number of logical CPUs.  Even as the number of
 *  threads gets larger, RWSpinLock can still be very competitive in
 *  READ, although it is slower on WRITE, and also inherently unfair
 *  to writers.
 *
 *  RWSpinLock handles 2^30 - 1 concurrent readers.
 */

#pragma once

/*
========================================================================
Benchmark on (Intel(R) Xeon(R) CPU  L5630  @ 2.13GHz)  8 cores(16 HTs)
========================================================================

------------------------------------------------------------------------------
1. Single thread benchmark (read/write lock + unlock overhead)
Benchmark                                    Iters   Total t    t/iter iter/sec
-------------------------------------------------------------------------------
*      BM_RWSpinLockRead                     100000  1.786 ms  17.86 ns   53.4M
+30.5% BM_RWSpinLockWrite                    100000  2.331 ms  23.31 ns  40.91M
+ 175% BM_PThreadRWMutexRead                 100000  4.917 ms  49.17 ns   19.4M
+ 166% BM_PThreadRWMutexWrite                100000  4.757 ms  47.57 ns  20.05M

------------------------------------------------------------------------------
2. Contention Benchmark      90% read  10% write
Benchmark                    hits       average    min       max        sigma
------------------------------------------------------------------------------
---------- 8  threads ------------
RWSpinLock       Write       142666     220ns      78ns      40.8us     269ns
RWSpinLock       Read        1282297    222ns      80ns      37.7us     248ns
pthread_rwlock_t Write       84248      2.48us     99ns      269us      8.19us
pthread_rwlock_t Read        761646     933ns      101ns     374us      3.25us

---------- 16 threads ------------
RWSpinLock       Write       124236     237ns      78ns      261us      801ns
RWSpinLock       Read        1115807    236ns      78ns      2.27ms     2.17us
pthread_rwlock_t Write       83363      7.12us     99ns      785us      28.1us
pthread_rwlock_t Read        754978     2.18us     101ns     1.02ms     14.3us

---------- 50 threads ------------
RWSpinLock       Write       131142     1.37us     82ns      7.53ms     68.2us
RWSpinLock       Read        1181240    262ns      78ns      6.62ms     12.7us
pthread_rwlock_t Write       80849      112us      103ns     4.52ms     263us
pthread_rwlock_t Read        728698     24us       101ns     7.28ms     194us

*/

#include <folly/Portability.h>
#include <folly/portability/Asm.h>

#include <algorithm>
#include <atomic>
#include <thread>

#include <folly/Likely.h>
#include <folly/synchronization/Lock.h>

namespace folly {

/*
 * A simple, small (4-bytes), but unfair rwlock.  Use it when you want
 * a nice writer and don't expect a lot of write/read contention, or
 * when you need small rwlocks since you are creating a large number
 * of them.
 *
 * Note that the unfairness here is extreme: if the lock is
 * continually accessed for read, writers will never get a chance.  If
 * the lock can be that highly contended this class is probably not an
 * ideal choice anyway.
 *
 * It currently implements most of the Lockable, SharedLockable and
 * UpgradeLockable concepts except the TimedLockable related locking/unlocking
 * interfaces.
 */
class RWSpinLock {
  enum : int32_t { SHARED = 4, UPGRADE = 2, EXCLUSIVE = 1 };
  static_assert(SHARED > EXCLUSIVE + UPGRADE);

 public:
  constexpr RWSpinLock() : bits_(0) {}

  RWSpinLock(RWSpinLock const&) = delete;
  RWSpinLock& operator=(RWSpinLock const&) = delete;

  // Lockable Concept
  void lock() {
    uint_fast32_t count = 0;
    while (!LIKELY(try_lock())) {
      if (++count > 1000) {
        std::this_thread::yield();
      }
    }
  }

  void unlock() { bits_.fetch_and(~EXCLUSIVE, std::memory_order_release); }

  // SharedLockable Concept
  void lock_shared() {
    uint_fast32_t count = 0;
    while (!LIKELY(try_lock_shared())) {
      if (++count > 1000) {
        std::this_thread::yield();
      }
    }
  }

  void unlock_shared() { bits_.fetch_add(-SHARED, std::memory_order_release); }

  // Downgrade the lock from writer status to reader status.
  void unlock_and_lock_shared() {
    bits_.fetch_add(SHARED - EXCLUSIVE, std::memory_order_acq_rel);
  }

  // UpgradeLockable Concept
  void lock_upgrade() {
    uint_fast32_t count = 0;
    while (!try_lock_upgrade()) {
      if (++count > 1000) {
        std::this_thread::yield();
      }
    }
  }

  void unlock_upgrade() {
    bits_.fetch_add(-UPGRADE, std::memory_order_release);
  }

  // unlock upgrade and try to acquire write lock
  void unlock_upgrade_and_lock() {
    int64_t count = 0;
    while (!try_unlock_upgrade_and_lock()) {
      if (++count > 1000) {
        std::this_thread::yield();
      }
    }
  }

  // unlock upgrade and read lock atomically
  void unlock_upgrade_and_lock_shared() {
    bits_.fetch_add(SHARED - UPGRADE, std::memory_order_acq_rel);
  }

  // write unlock and upgrade lock atomically
  void unlock_and_lock_upgrade() {
    // UPGRADE cannot be set while EXCLUSIVE is set. SHARED itself may be
    // non-zero though, since try_lock_shared() does fetch_add(SHARED)
    // speculatively.
    bits_.fetch_add(UPGRADE - EXCLUSIVE, std::memory_order_acq_rel);
  }

  // Attempt to acquire writer permission. Return false if we didn't get it.
  bool try_lock() {
    int32_t expect = 0;
    return bits_.compare_exchange_strong(
        expect, EXCLUSIVE, std::memory_order_acquire);
  }

  // Try to get reader permission on the lock. This can fail if we
  // find out someone is a writer or upgrader.
  // Setting the UPGRADE bit would allow a writer-to-be to indicate
  // its intention to write and block any new readers while waiting
  // for existing readers to finish and release their read locks. This
  // helps avoid starving writers (promoted from upgraders).
  bool try_lock_shared() {
    // fetch_add is considerably (100%) faster than compare_exchange,
    // so here we are optimizing for the common (lock success) case.
    int32_t value = bits_.fetch_add(SHARED, std::memory_order_acquire);
    if (value & (EXCLUSIVE | UPGRADE)) [[unlikely]] {
      bits_.fetch_add(-SHARED, std::memory_order_release);
      return false;
    }
    return true;
  }

  // try to unlock upgrade and write lock atomically
  bool try_unlock_upgrade_and_lock() {
    int32_t expect = UPGRADE;
    return bits_.compare_exchange_strong(
        expect, EXCLUSIVE, std::memory_order_acq_rel);
  }

  // try to acquire an upgradable lock.
  bool try_lock_upgrade() {
    int32_t value = bits_.load(std::memory_order_relaxed);
    while ((value & (EXCLUSIVE | UPGRADE)) == 0) {
      if (bits_.compare_exchange_weak(
              value,
              value | UPGRADE,
              std::memory_order_acquire,
              std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }

  // mainly for debugging purposes.
  int32_t bits() const { return bits_.load(std::memory_order_acquire); }

 private:
  std::atomic<int32_t> bits_;
};

} // namespace folly
