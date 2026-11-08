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

#include <mutex>
#include <vector>

#include <folly/CancellationToken.h>
#include <folly/synchronization/detail/Sleeper.h>

namespace folly {
namespace fibers {

namespace detail {

template <class BatonType>
MutexWaiter<BatonType>::~MutexWaiter() {
  if (posted_.load(std::memory_order_acquire)) {
    return;
  }
  // If baton supported retrying wait() after wait()/try_wait_until(), like
  // SaturatingSemaphore, this could simply be replaced with baton_.wait(), and
  // posted_ could be removed.
  folly::detail::Sleeper sleeper;
  while (!posted_.load(std::memory_order_acquire)) {
    sleeper.wait();
  }
}

template <class BatonType>
void MutexWaiter<BatonType>::wait() {
  baton_.wait();
}

template <class BatonType>
template <class Deadline>
bool MutexWaiter<BatonType>::try_wait_until(Deadline deadline) {
  return baton_.try_wait_until(deadline);
}

template <class BatonType>
void MutexWaiter<BatonType>::wake() {
  if (coroWaiter_) {
    // For coroutine waiters, set posted_ before baton_.post(). baton_.post()
    // resumes the coroutine synchronously via h_(), and the coroutine frame
    // (containing this MutexWaiter) may be destroyed on another thread before
    // baton_.post() returns. Setting posted_ first ensures wake() never
    // accesses the waiter after the baton post.
    //
    // This ordering is NOT safe for fiber/thread waiters: the destructor
    // spin-waits on posted_, so setting it first would allow the destructor
    // to return and destroy the baton before baton_.post() completes.
    posted_.store(true, std::memory_order_release);
    baton_.post();
  } else {
    baton_.post();
    posted_.store(true, std::memory_order_release);
  }
}

} // namespace detail

//
// TimedMutex implementation
//

template <typename WaitFunc>
TimedMutex::LockResult TimedMutex::lockHelper(WaitFunc&& waitFunc) {
  std::unique_lock ulock(lock_);
  if (!locked_) {
    locked_ = true;
    return LockResult::SUCCESS;
  }

  const auto isOnFiber = options_.stealing_ && onFiber();

  if (!isOnFiber && notifiedFiber_ != nullptr) {
    // lock() was called on a thread and while some other fiber was already
    // notified, it hasn't be run yet. We steal the lock from that fiber then
    // to avoid potential deadlock.
    DCHECK(threadWaiters_.empty());
    notifiedFiber_ = nullptr;
    return LockResult::SUCCESS;
  }

  // Delay constructing the waiter until it is actually required.
  // This makes a huge difference, at least in the benchmarks,
  // when the mutex isn't locked.
  MutexWaiter waiter;
  MutexWaiterList& waiters = isOnFiber ? fiberWaiters_ : threadWaiters_;
  waiters.push_back(waiter);

  ulock.unlock();

  if (!waitFunc(waiters, waiter)) {
    return LockResult::TIMEOUT;
  }

  if (isOnFiber) {
    auto lockStolen = [&] {
      std::lock_guard lg(lock_);

      auto stolen = notifiedFiber_ != &waiter;
      if (!stolen) {
        notifiedFiber_ = nullptr;
      }
      return stolen;
    }();

    if (lockStolen) {
      return LockResult::STOLEN;
    }
  }

  return LockResult::SUCCESS;
}

inline void TimedMutex::lock() {
  LockResult result;
  do {
    result = lockHelper([](MutexWaiterList&, MutexWaiter& waiter) {
      waiter.wait();
      return true;
    });

    DCHECK(result != LockResult::TIMEOUT);
  } while (result != LockResult::SUCCESS);
}

template <typename Rep, typename Period>
bool TimedMutex::try_lock_for(
    const std::chrono::duration<Rep, Period>& timeout) {
  return try_lock_until(std::chrono::steady_clock::now() + timeout);
}

template <typename Clock, typename Duration>
bool TimedMutex::try_lock_until(
    const std::chrono::time_point<Clock, Duration>& deadline) {
  auto result = lockHelper([&](MutexWaiterList& waiters, MutexWaiter& waiter) {
    if (!waiter.try_wait_until(deadline)) {
      // We timed out. Two cases:
      // 1. We're still in the waiter list and we truly timed out
      // 2. We're not in the waiter list anymore. This could happen if the baton
      //    times out but the mutex is unlocked before we reach this code. In
      //    this
      //    case we'll pretend we got the lock on time.
      std::unique_lock lg(lock_);
      if (waiter.hook.is_linked()) {
        waiters.erase(waiters.iterator_to(waiter));
        lg.unlock();
        waiter.wake(); // Ensure that destruction doesn't block.
        return false;
      }
    }
    return true;
  });

  switch (result) {
    case LockResult::SUCCESS:
      return true;
    case LockResult::TIMEOUT:
      return false;
    case LockResult::STOLEN:
      // We don't respect the duration if lock was stolen
      lock();
      return true;
  }
  assume_unreachable();
}

inline bool TimedMutex::try_lock() {
  std::lock_guard lg(lock_);
  if (locked_) {
    return false;
  }
  locked_ = true;
  return true;
}

inline void TimedMutex::unlock() {
  std::unique_lock lg(lock_);
  if (!threadWaiters_.empty()) {
    auto& to_wake = threadWaiters_.front();
    threadWaiters_.pop_front();
    lg.unlock();
    to_wake.wake();
    return;
  }

  if (!fiberWaiters_.empty()) {
    auto& to_wake = fiberWaiters_.front();
    fiberWaiters_.pop_front();
    notifiedFiber_ = &to_wake;
    lg.unlock();
    to_wake.wake();
    return;
  }

  locked_ = false;
}

#if FOLLY_HAS_COROUTINES
inline folly::coro::Task<void> TimedMutex::co_lock() {
  {
    std::lock_guard lg(lock_);
    if (!locked_) {
      locked_ = true;
      co_return;
    }
  }

  while (true) {
    MutexWaiter waiter(/*coroWaiter=*/true);

    {
      std::lock_guard lg(lock_);
      if (!locked_) {
        locked_ = true;
        waiter.wake();
        co_return;
      }

      // Coroutine waiters use fiberWaiters_ because they share the same
      // cooperative semantics: they yield (not block), are stealable by
      // thread waiters, and get lower priority than threads in unlock().
      fiberWaiters_.push_back(waiter);
    }

    bool cancelled = false;
    {
      const auto& ct = co_await folly::coro::co_current_cancellation_token;
      folly::CancellationCallback cb{
          ct, [&] {
            {
              std::lock_guard lg(lock_);
              if (!waiter.hook.is_linked()) {
                // Cancellation fired, but unlock() already dequeued this
                // waiter and granted the lock. The lock is acquired; let
                // co_lock() return success.
                return;
              }
              cancelled = true;
              fiberWaiters_.erase(fiberWaiters_.iterator_to(waiter));
            }
            waiter.wake();
          }};

      co_await waiter.baton();
    }

    if (cancelled) {
      co_yield folly::coro::co_stopped_may_throw;
    }

    auto lockStolen = [&] {
      std::lock_guard lg(lock_);
      auto stolen = notifiedFiber_ != &waiter;
      if (!stolen) {
        notifiedFiber_ = nullptr;
      }
      return stolen;
    }();

    if (!lockStolen) {
      co_return;
    }
  }
}

inline folly::coro::Task<std::unique_lock<TimedMutex>>
TimedMutex::co_scoped_lock() {
  co_await co_lock();
  co_return std::unique_lock<TimedMutex>{*this, std::adopt_lock};
}
#endif

//
// TimedRWMutexImpl implementation
//

template <bool ReaderPriority, typename BatonType>
class TimedRWMutexImpl<ReaderPriority, BatonType>::StateLock {
 public:
  // Spin until the state is not locked to apply mutation. The callback can
  // abort at any time.
  template <class Mutation>
  FOLLY_ALWAYS_INLINE static bool tryMutate(
      std::atomic<uint64_t>& stateRef, Mutation mutation) {
    folly::detail::Sleeper sleeper;
    auto curState = stateRef.load(std::memory_order_relaxed);
    while (true) {
      while (FOLLY_UNLIKELY(curState & kStateLocked)) {
        sleeper.wait();
        curState = stateRef.load(std::memory_order_relaxed);
      }
      auto newState = curState;
      if (!mutation(newState)) {
        return false;
      }
      if (stateRef.compare_exchange_strong(
              curState,
              newState,
              std::memory_order_acq_rel,
              std::memory_order_relaxed)) {
        return true;
      }
    }
  }

  explicit StateLock(std::atomic<uint64_t>& stateRef) : stateRef_(stateRef) {
    tryMutate(stateRef_, [&](uint64_t& state) {
      state_ = (state |= kStateLocked);
#ifndef NDEBUG
      initialState_ = state_;
#endif
      return true;
    });
  }

  ~StateLock() {
    if (locked_) {
      unlock();
    }
  }

  uint64_t& state() {
    assert(locked_);
    return state_;
  }

  void unlock() {
    // All state modifications while locked should be done through state().
    assert(stateRef_.load(std::memory_order_relaxed) == initialState_);
    // Verify reader/writer exclusion.
    assert(!(state_ & kWriteLocked) || (state_ >> kReadersShift) == 0);
    stateRef_.store(state_ & ~kStateLocked, std::memory_order_release);
    locked_ = false;
  }

  StateLock(const StateLock&) = delete;
  StateLock(StateLock&&) = delete;
  StateLock& operator=(const StateLock&) = delete;
  StateLock& operator=(StateLock&&) = delete;

 private:
  std::atomic<uint64_t>& stateRef_;
  uint64_t state_;
#ifndef NDEBUG
  uint64_t initialState_;
#endif
  bool locked_ = true;
};

// invariants that must hold when the lock is not held by anyone
template <bool ReaderPriority, typename BatonType>
void TimedRWMutexImpl<ReaderPriority, BatonType>::verify_unlocked_properties(
    [[maybe_unused]] StateLock& slock) const {
  assert(slock.state() == kStateLocked); // Only bit set is the state lock.
  assert(read_waiters_.empty());
  assert(write_waiters_.empty());
}

template <bool ReaderPriority, typename BatonType>
bool TimedRWMutexImpl<ReaderPriority, BatonType>::shouldReadersWait(
    StateLock& slock) const {
  assert(!(slock.state() & kHasWriteWaiters) == write_waiters_.empty());
  return shouldReadersWait(slock.state());
}

template <bool ReaderPriority, typename BatonType>
/* static */ bool
TimedRWMutexImpl<ReaderPriority, BatonType>::shouldReadersWait(uint64_t state) {
  return (state & kWriteLocked) ||
      (!ReaderPriority && (state & kHasWriteWaiters));
}

template <bool ReaderPriority, typename BatonType>
bool TimedRWMutexImpl<ReaderPriority, BatonType>::try_lock_shared_fast() {
  return StateLock::tryMutate(state_, [](uint64_t& state) {
    if (shouldReadersWait(state)) [[unlikely]] {
      return false;
    }
    state += kReadersInc;
    return true;
  });
}

template <bool ReaderPriority, typename BatonType>
bool TimedRWMutexImpl<ReaderPriority, BatonType>::try_unlock_shared_fast() {
  return StateLock::tryMutate(state_, [](uint64_t& state) {
    auto numReaders = state >> kReadersShift;
    assert(!(state & kWriteLocked) && numReaders > 0);
    if (numReaders == 1 && (state & kHasWriteWaiters)) [[unlikely]] {
      return false; // We may need to wake up a writer.
    }
    state -= kReadersInc;
    return true;
  });
}

template <bool ReaderPriority, typename BatonType>
void TimedRWMutexImpl<ReaderPriority, BatonType>::lock_shared() {
  if (try_lock_shared_fast()) {
    return;
  }

  StateLock slock{state_};
  assert(!(slock.state() & kHasWriteWaiters) == write_waiters_.empty());
  if (shouldReadersWait(slock)) {
    MutexWaiter waiter;
    read_waiters_.push_back(waiter);
    slock.unlock();
    waiter.wait();
    assert((state_.load(std::memory_order_relaxed) >> kReadersShift) > 0);
    return;
  }
  assert(!(slock.state() & kWriteLocked));
  slock.state() += kReadersInc;
}

template <bool ReaderPriority, typename BatonType>
template <typename Rep, typename Period>
bool TimedRWMutexImpl<ReaderPriority, BatonType>::try_lock_shared_for(
    const std::chrono::duration<Rep, Period>& timeout) {
  return try_lock_shared_until(std::chrono::steady_clock::now() + timeout);
}

template <bool ReaderPriority, typename BatonType>
template <typename Clock, typename Duration>
bool TimedRWMutexImpl<ReaderPriority, BatonType>::try_lock_shared_until(
    const std::chrono::time_point<Clock, Duration>& deadline) {
  if (try_lock_shared_fast()) {
    return true;
  }
  StateLock slock{state_};
  if (shouldReadersWait(slock)) {
    MutexWaiter waiter;
    read_waiters_.push_back(waiter);
    slock.unlock();

    if (!waiter.try_wait_until(deadline)) {
      // We timed out. Two cases:
      // 1. We're still in the waiter list and we truly timed out
      // 2. We're not in the waiter list anymore. This could happen if the baton
      //    times out but the mutex is unlocked before we reach this code. In
      //    this case we'll pretend we got the lock on time.
      StateLock slock2{state_};
      if (waiter.hook.is_linked()) {
        read_waiters_.erase(read_waiters_.iterator_to(waiter));
        slock2.unlock();
        waiter.wake(); // Ensure that destruction doesn't block.
        return false;
      }
    }
    return true;
  }
  assert(!(slock.state() & kWriteLocked));
  slock.state() += kReadersInc;
  return true;
}

template <bool ReaderPriority, typename BatonType>
bool TimedRWMutexImpl<ReaderPriority, BatonType>::try_lock_shared() {
  if (try_lock_shared_fast()) {
    return true;
  }

  StateLock slock{state_};
  if (shouldReadersWait(slock)) {
    return false;
  }
  assert(!(slock.state() & kWriteLocked));
  slock.state() += kReadersInc;
  return true;
}

template <bool ReaderPriority, typename BatonType>
void TimedRWMutexImpl<ReaderPriority, BatonType>::unlock_shared() {
  if (try_unlock_shared_fast()) {
    return;
  }

  assert((state_.load(std::memory_order_relaxed) >> kReadersShift) > 0);
  unlock_();
}

template <bool ReaderPriority, typename BatonType>
bool TimedRWMutexImpl<ReaderPriority, BatonType>::try_lock_(StateLock& slock) {
  if (!(slock.state() & kWriteLocked) &&
      ((slock.state() >> kReadersShift) == 0)) {
    verify_unlocked_properties(slock);
    slock.state() |= kWriteLocked;
    return true;
  }
  return false;
}

template <bool ReaderPriority, typename BatonType>
void TimedRWMutexImpl<ReaderPriority, BatonType>::lock() {
  StateLock slock{state_};
  if (try_lock_(slock)) {
    return;
  }
  MutexWaiter waiter;
  write_waiters_.push_back(waiter);
  slock.state() |= kHasWriteWaiters;
  slock.unlock();
  waiter.wait();
}

template <bool ReaderPriority, typename BatonType>
template <typename Rep, typename Period>
bool TimedRWMutexImpl<ReaderPriority, BatonType>::try_lock_for(
    const std::chrono::duration<Rep, Period>& timeout) {
  return try_lock_until(std::chrono::steady_clock::now() + timeout);
}

template <bool ReaderPriority, typename BatonType>
template <typename Clock, typename Duration>
bool TimedRWMutexImpl<ReaderPriority, BatonType>::try_lock_until(
    const std::chrono::time_point<Clock, Duration>& deadline) {
  StateLock slock{state_};
  if (try_lock_(slock)) {
    return true;
  }
  MutexWaiter waiter;
  {
    write_waiters_.push_back(waiter);
    slock.state() |= kHasWriteWaiters;
    slock.unlock();
  }

  if (!waiter.try_wait_until(deadline)) {
    // We timed out. Two cases:
    // 1. We're still in the waiter list and we truly timed out
    // 2. We're not in the waiter list anymore. This could happen if the baton
    //    times out but the mutex is unlocked before we reach this code. In
    //    this case we'll pretend we got the lock on time.
    StateLock slock2{state_};
    if (waiter.hook.is_linked()) {
      write_waiters_.erase(write_waiters_.iterator_to(waiter));
      if (write_waiters_.empty() && (slock2.state() & kHasWriteWaiters)) {
        slock2.state() &= ~kHasWriteWaiters;
      }
      if (write_waiters_.empty() && !(slock2.state() & kWriteLocked)) {
        wake_readers_and_unlock(slock2);
      } else {
        slock2.unlock();
      }
      waiter.wake(); // Ensure that destruction doesn't block.
      return false;
    }
  }
  assert(state_.load(std::memory_order_relaxed) & kWriteLocked);
  return true;
}

template <bool ReaderPriority, typename BatonType>
bool TimedRWMutexImpl<ReaderPriority, BatonType>::try_lock() {
  StateLock slock{state_};
  return try_lock_(slock);
}

template <bool ReaderPriority, typename BatonType>
void TimedRWMutexImpl<ReaderPriority, BatonType>::unlock() {
  assert(state_.load(std::memory_order_relaxed) & kWriteLocked);
  unlock_();
}

template <bool ReaderPriority, typename BatonType>
void TimedRWMutexImpl<ReaderPriority, BatonType>::unlock_() {
  StateLock slock{state_};
  assert(
      (slock.state() & kWriteLocked) ||
      (!(slock.state() & kWriteLocked) &&
       ((slock.state() >> kReadersShift) > 0)));
  if ((slock.state() >> kReadersShift) > 0) {
    slock.state() -= kReadersInc;
  }

  if (!read_waiters_.empty() && (ReaderPriority || write_waiters_.empty())) {
    assert(
        (slock.state() & kWriteLocked) &&
        ((slock.state() >> kReadersShift) == 0) &&
        "read waiters can only accumulate while write locked");
    slock.state() &= ~kWriteLocked;
    wake_readers_and_unlock(slock);
    return;
  }

  if ((slock.state() >> kReadersShift) == 0) {
    if (!write_waiters_.empty()) {
      assert(read_waiters_.empty() || !ReaderPriority);
      assert(slock.state() & kHasWriteWaiters);
      slock.state() |= kWriteLocked;

      // Wake a single writer (after releasing the spin lock)
      MutexWaiter& to_wake = write_waiters_.front();
      write_waiters_.pop_front();
      if (write_waiters_.empty()) {
        slock.state() &= ~kHasWriteWaiters;
      }
      slock.unlock();
      to_wake.wake();
      return;
    }

    slock.state() &= ~kWriteLocked;
    verify_unlocked_properties(slock);
    return;
  }

  assert((slock.state() >> kReadersShift) > 0);
}

template <bool ReaderPriority, typename BatonType>
void TimedRWMutexImpl<ReaderPriority, BatonType>::wake_readers_and_unlock(
    StateLock& slock) {
  std::vector<MutexWaiter*> waiters_to_wake;
  waiters_to_wake.reserve(read_waiters_.size());
  slock.state() += read_waiters_.size() * kReadersInc;

  // A timed-out reader uses hook.is_linked() to decide whether it was granted
  // the lock. Unlink all granted readers under StateLock before waking them.
  while (!read_waiters_.empty()) {
    waiters_to_wake.push_back(&read_waiters_.front());
    read_waiters_.pop_front();
  }
  slock.unlock();

  for (auto* waiter : waiters_to_wake) {
    waiter->wake();
  }
}

template <bool ReaderPriority, typename BatonType>
void TimedRWMutexImpl<ReaderPriority, BatonType>::unlock_and_lock_shared() {
  StateLock slock{state_};
  assert(
      (slock.state() & kWriteLocked) &&
      ((slock.state() >> kReadersShift) == 0));
  slock.state() &= ~kWriteLocked;
  slock.state() += kReadersInc;
  wake_readers_and_unlock(slock);
}
} // namespace fibers
} // namespace folly
