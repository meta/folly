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

#include <folly/SharedMutex.h>

#include <stdlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <random>
#include <shared_mutex> // std::shared_lock
#include <system_error>
#include <thread>
#include <vector>

#include <folly/Benchmark.h>
#include <folly/MPMCQueue.h>
#include <folly/fibers/Baton.h>
#include <folly/fibers/TimedMutex.h>
#include <folly/portability/GFlags.h>
#include <folly/portability/GTest.h>
#include <folly/synchronization/RWSpinLock.h>
#include <folly/test/DeterministicSchedule.h>
#include <folly/test/TestUtils.h>

using namespace folly;
using namespace folly::test;
using namespace std;
using namespace std::chrono;

struct DSharedMutexPolicy : SharedMutexPolicyDefault {
  static constexpr uint64_t max_spin_cycles = 0;
  static constexpr uint32_t max_soft_yield_count = 0;
};
using DSched = DeterministicSchedule;
using DSharedMutexReadPriority =
    SharedMutexImpl<true, void, DeterministicAtomic, DSharedMutexPolicy>;
using DSharedMutexWritePriority =
    SharedMutexImpl<false, void, DeterministicAtomic, DSharedMutexPolicy>;

namespace {

constexpr auto kLifecycleHookTestTimeout = std::chrono::seconds(10);
using HookEvent = SharedMutexHookEvent;

class SharedMutexHookObserver {
 public:
  virtual ~SharedMutexHookObserver() = default;
  virtual void on_event(HookEvent event) noexcept = 0;
};

std::atomic<SharedMutexHookObserver*> sharedMutexHookObserver{nullptr};

struct SharedMutexHookPolicy : SharedMutexPolicyDefault {
  static void on_event(HookEvent event) noexcept {
    if (auto* observer =
            sharedMutexHookObserver.load(std::memory_order_acquire)) {
      observer->on_event(event);
    }
  }
};

struct SharedMutexHookTag;
using HookedSharedMutex = SharedMutexImpl<
    false,
    SharedMutexHookTag,
    std::atomic,
    SharedMutexHookPolicy>;
using HookedSharedMutexReadPriority = SharedMutexImpl<
    true,
    SharedMutexHookTag,
    std::atomic,
    SharedMutexHookPolicy>;

class SharedMutexHookRegistration {
 public:
  explicit SharedMutexHookRegistration(SharedMutexHookObserver& observer) {
    sharedMutexHookObserver.store(&observer, std::memory_order_release);
  }

  ~SharedMutexHookRegistration() {
    sharedMutexHookObserver.store(nullptr, std::memory_order_release);
  }
};

class SharedMutexHookRecorder : public SharedMutexHookObserver {
 public:
  void on_event(HookEvent event) noexcept override {
    auto index = static_cast<size_t>(event);
    std::unique_lock lock(mutex_);
    ++counts_[index];
    cv_.notify_all();
    const bool didTimeOut =
        blocked_[index] && !cv_.wait_for(lock, kLifecycleHookTestTimeout, [&] {
          return !blocked_[index];
        });
    if (didTimeOut) {
      timedOut_ = true;
    }
  }

  void block(HookEvent event) {
    std::lock_guard lock(mutex_);
    blocked_[static_cast<size_t>(event)] = true;
  }

  void unblock(HookEvent event) {
    std::lock_guard lock(mutex_);
    blocked_[static_cast<size_t>(event)] = false;
    cv_.notify_all();
  }

  bool waitFor(HookEvent event, size_t count = 1) {
    auto index = static_cast<size_t>(event);
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, kLifecycleHookTestTimeout, [&] {
      return counts_[index] >= count;
    });
  }

  size_t count(HookEvent event) {
    std::lock_guard lock(mutex_);
    return counts_[static_cast<size_t>(event)];
  }

  bool completedWithoutTimeout() {
    std::lock_guard lock(mutex_);
    return !timedOut_;
  }

 private:
  static constexpr size_t kEventCount =
      static_cast<size_t>(HookEvent::BeforeDestroy) + 1;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::array<size_t, kEventCount> counts_{};
  std::array<bool, kEventCount> blocked_{};
  bool timedOut_{false};
};

constexpr auto kLifetimeTestTimeout = std::chrono::seconds(10);

class LifetimeTestState : public SharedMutexHookObserver {
 public:
  LifetimeTestState(bool blockWaiters, bool pauseRelease)
      : releaser_(std::this_thread::get_id()),
        blockWaiters_(blockWaiters),
        pauseRelease_(pauseRelease) {}

  void on_event(HookEvent event) noexcept override {
    switch (event) {
      case HookEvent::BeforeParkExclusive:
      case HookEvent::BeforeParkUpgrade:
      case HookEvent::BeforeParkShared:
      case HookEvent::BeforeParkReadersToDrain:
        afterWaiterRegistered();
        return;
      case HookEvent::AfterReleaseExclusive:
      case HookEvent::AfterReleaseUpgrade:
      case HookEvent::AfterReleaseShared:
        afterRelease();
        return;
      case HookEvent::BeforeDestroy:
        beforeDestroy();
        return;
    }
  }

  void afterWaiterRegistered() {
    std::unique_lock lock(mutex_);
    auto id = std::this_thread::get_id();
    if (std::find(waiters_.begin(), waiters_.end(), id) == waiters_.end()) {
      waiters_.push_back(id);
      cv_.notify_all();
    }
    const bool didTimeOut = blockWaiters_ &&
        !cv_.wait_for(lock, kLifetimeTestTimeout, [&] { return released_; });
    if (didTimeOut) {
      timedOut_ = true;
    }
  }

  void afterRelease() {
    std::unique_lock lock(mutex_);
    if (std::this_thread::get_id() != releaser_) {
      return;
    }
    released_ = true;
    cv_.notify_all();
    const bool didTimeOut =
        pauseRelease_ && !cv_.wait_for(lock, kLifetimeTestTimeout, [&] {
          return releaseMayFinish_;
        });
    if (didTimeOut) {
      timedOut_ = true;
    }
  }

  void beforeDestroy() {
    std::lock_guard lock(mutex_);
    destructorWaiting_ = true;
    cv_.notify_all();
  }

  void setReleaser(std::thread::id releaser) {
    std::lock_guard lock(mutex_);
    releaser_ = releaser;
  }

  void ownerReady() {
    std::lock_guard lock(mutex_);
    ownerReady_ = true;
    cv_.notify_all();
  }

  bool waitForOwner() {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, kLifetimeTestTimeout, [&] {
      return ownerReady_;
    });
  }

  void requestRelease() {
    std::lock_guard lock(mutex_);
    releaseRequested_ = true;
    cv_.notify_all();
  }

  void waitForReleaseRequest() {
    std::unique_lock lock(mutex_);
    const bool didTimeOut = !cv_.wait_for(lock, kLifetimeTestTimeout, [&] {
      return releaseRequested_;
    });
    if (didTimeOut) {
      timedOut_ = true;
    }
  }

  bool waitForWaiters(size_t count) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, kLifetimeTestTimeout, [&] {
      return waiters_.size() >= count;
    });
  }

  bool waitForRelease() {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, kLifetimeTestTimeout, [&] { return released_; });
  }

  bool waitForDestructor() {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, kLifetimeTestTimeout, [&] {
      return destructorWaiting_;
    });
  }

  void allowReleaseToFinish() {
    std::lock_guard lock(mutex_);
    releaseMayFinish_ = true;
    cv_.notify_all();
  }

  void objectDeleted() {
    std::lock_guard lock(mutex_);
    deleted_ = true;
    releaseMayFinish_ = true;
    cv_.notify_all();
  }

  bool completedWithoutTimeout() {
    std::lock_guard lock(mutex_);
    return released_ && deleted_ && !timedOut_;
  }

 private:
  std::thread::id releaser_;
  const bool blockWaiters_;
  const bool pauseRelease_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::thread::id> waiters_;
  bool released_{false};
  bool deleted_{false};
  bool releaseMayFinish_{false};
  bool destructorWaiting_{false};
  bool ownerReady_{false};
  bool releaseRequested_{false};
  bool timedOut_{false};
};

using LifetimeTestRegistration = SharedMutexHookRegistration;

enum class LifetimeReleaseMode { Exclusive, Shared, Upgrade };

template <LifetimeReleaseMode Mode, typename Mutex>
void acquireInitialOwnership(Mutex& mutex) {
  if constexpr (Mode == LifetimeReleaseMode::Exclusive) {
    mutex.lock();
  } else if constexpr (Mode == LifetimeReleaseMode::Shared) {
    mutex.lock_shared();
  } else {
    mutex.lock_upgrade();
  }
}

template <LifetimeReleaseMode Mode, typename Mutex>
void releaseInitialOwnership(Mutex& mutex) {
  if constexpr (Mode == LifetimeReleaseMode::Exclusive) {
    mutex.unlock();
  } else if constexpr (Mode == LifetimeReleaseMode::Shared) {
    mutex.unlock_shared();
  } else {
    mutex.unlock_upgrade();
  }
}

template <typename Mutex, LifetimeReleaseMode Mode>
void runSelfDestructionAfterReleaseTest() {
  auto* mutex = new Mutex;
  acquireInitialOwnership<Mode>(*mutex);

  LifetimeTestState state(/* blockWaiters */ true, /* pauseRelease */ true);
  LifetimeTestRegistration registration(state);
  std::thread deleter([&] {
    mutex->lock();
    mutex->unlock();
    delete mutex;
    state.objectDeleted();
  });

  const bool didWaiterPark = state.waitForWaiters(1);
  releaseInitialOwnership<Mode>(*mutex);
  deleter.join();

  ASSERT_TRUE(didWaiterPark);
  ASSERT_TRUE(state.completedWithoutTimeout());
}

template <typename Mutex, LifetimeReleaseMode Mode>
void runSaturatedExclusiveWaitersTest() {
  constexpr size_t kWaiters = 8;
  Mutex mutex;
  acquireInitialOwnership<Mode>(mutex);

  LifetimeTestState state(/* blockWaiters */ false, /* pauseRelease */ false);
  LifetimeTestRegistration registration(state);
  std::atomic<size_t> acquired{0};
  std::vector<std::thread> waiters;
  waiters.reserve(kWaiters);
  for (size_t i = 0; i < kWaiters; ++i) {
    waiters.emplace_back([&] {
      if (mutex.try_lock_for(3 * kLifetimeTestTimeout)) {
        ++acquired;
        mutex.unlock();
      }
    });
  }

  const bool didWaitersPark = state.waitForWaiters(kWaiters);
  releaseInitialOwnership<Mode>(mutex);
  for (auto& waiter : waiters) {
    waiter.join();
  }

  ASSERT_TRUE(didWaitersPark);
  ASSERT_EQ(kWaiters, acquired.load());
}

template <typename Mutex>
void runSelfDestructionDuringSaturatedReleaseTest() {
  constexpr size_t kWaiters = 2;
  auto* mutex = new Mutex;

  LifetimeTestState state(/* blockWaiters */ false, /* pauseRelease */ true);
  LifetimeTestRegistration registration(state);
  std::thread releaser([&] {
    mutex->lock();
    state.setReleaser(std::this_thread::get_id());
    state.ownerReady();
    state.waitForReleaseRequest();
    mutex->unlock();
  });
  const bool didOwnerBecomeReady = state.waitForOwner();

  std::vector<std::thread> waiters;
  waiters.reserve(kWaiters);
  for (size_t i = 0; i < kWaiters; ++i) {
    waiters.emplace_back([&] {
      if (mutex->try_lock_for(kLifetimeTestTimeout / 10)) {
        mutex->unlock();
      }
    });
  }
  const bool didWaitersPark = state.waitForWaiters(kWaiters);

  state.requestRelease();
  const bool didReleaseStart = state.waitForRelease();
  for (auto& waiter : waiters) {
    waiter.join();
  }

  constexpr size_t kSuccessors = 8;
  std::atomic<size_t> successorReleases{0};
  std::vector<std::thread> successors;
  successors.reserve(kSuccessors);
  for (size_t i = 0; i < kSuccessors; ++i) {
    successors.emplace_back([&] {
      mutex->lock();
      mutex->unlock();
      ++successorReleases;
    });
  }
  for (auto& successor : successors) {
    successor.join();
  }
  const bool didAllSuccessorsRelease = successorReleases.load() == kSuccessors;

  std::atomic<bool> deleted{false};
  std::thread deleter([&] {
    mutex->lock();
    mutex->unlock();
    delete mutex;
    deleted = true;
    state.objectDeleted();
  });

  const bool didDestructorStart = state.waitForDestructor();
  const bool deletedBeforeReleaseFinished = deleted.load();
  state.allowReleaseToFinish();
  releaser.join();
  deleter.join();

  ASSERT_TRUE(didOwnerBecomeReady);
  ASSERT_TRUE(didWaitersPark);
  ASSERT_TRUE(didReleaseStart);
  ASSERT_TRUE(didAllSuccessorsRelease);
  ASSERT_TRUE(didDestructorStart);
  ASSERT_FALSE(deletedBeforeReleaseFinished);
  ASSERT_TRUE(deleted.load());
  ASSERT_TRUE(state.completedWithoutTimeout());
}

enum class ExclusiveConversion { Upgrade, Shared };

template <typename Mutex, ExclusiveConversion Conversion>
void runReaderDrainWaitBitConversionTest() {
  SharedMutexHookRecorder events;
  SharedMutexHookRegistration registration(events);
  Mutex mutex;
  mutex.lock_shared();

  std::thread writer([&] {
    mutex.lock();
    if constexpr (Conversion == ExclusiveConversion::Upgrade) {
      mutex.unlock_and_lock_upgrade();
      mutex.unlock_upgrade();
    } else {
      mutex.unlock_and_lock_shared();
      mutex.unlock_shared();
      mutex.lock_upgrade();
      mutex.unlock_upgrade();
    }
  });

  const bool didWriterPark =
      events.waitFor(HookEvent::BeforeParkReadersToDrain);
  mutex.unlock_shared();
  writer.join();

  ASSERT_TRUE(didWriterPark);
}

} // namespace

static_assert(sizeof(SharedMutex) == sizeof(uint32_t));
static_assert(alignof(SharedMutex) == alignof(uint32_t));

TEST(SharedMutex, lifecycleHooksReportReleaseAndDestroy) {
  SharedMutexHookRecorder events;
  SharedMutexHookRegistration registration(events);
  {
    HookedSharedMutex mutex;
    mutex.lock();
    mutex.unlock();
    mutex.lock_upgrade();
    mutex.unlock_upgrade();

    SharedMutexToken inlineToken;
    SharedMutexToken deferredToken;
    mutex.lock_shared(inlineToken);
    mutex.lock_shared(deferredToken);
    EXPECT_EQ(
        SharedMutexToken::State::LockedDeferredShared, deferredToken.state_);
    mutex.unlock_shared(deferredToken);
    mutex.unlock_shared(inlineToken);
  }

  EXPECT_EQ(1, events.count(HookEvent::AfterReleaseExclusive));
  EXPECT_EQ(1, events.count(HookEvent::AfterReleaseUpgrade));
  EXPECT_EQ(2, events.count(HookEvent::AfterReleaseShared));
  EXPECT_EQ(1, events.count(HookEvent::BeforeDestroy));
  ASSERT_TRUE(events.completedWithoutTimeout());
}

TEST(SharedMutex, lifecycleHooksReportContendedParks) {
  SharedMutexHookRecorder events;
  SharedMutexHookRegistration registration(events);
  HookedSharedMutex mutex;
  mutex.lock();

  constexpr std::array kParkEvents{
      HookEvent::BeforeParkExclusive,
      HookEvent::BeforeParkUpgrade,
      HookEvent::BeforeParkShared,
  };
  for (auto event : kParkEvents) {
    events.block(event);
  }

  std::thread exclusive([&] {
    mutex.lock();
    mutex.unlock();
  });
  std::thread upgrade([&] {
    mutex.lock_upgrade();
    mutex.unlock_upgrade();
  });
  std::thread shared([&] {
    mutex.lock_shared();
    mutex.unlock_shared();
  });
  std::array<bool, kParkEvents.size()> didPark{};
  for (size_t i = 0; i < kParkEvents.size(); ++i) {
    didPark[i] = events.waitFor(kParkEvents[i]);
  }

  mutex.unlock();
  for (auto event : kParkEvents) {
    events.unblock(event);
  }
  exclusive.join();
  upgrade.join();
  shared.join();

  for (size_t i = 0; i < kParkEvents.size(); ++i) {
    ASSERT_TRUE(didPark[i]);
    ASSERT_GE(events.count(kParkEvents[i]), 1);
  }
  ASSERT_TRUE(events.completedWithoutTimeout());
}

TEST(SharedMutex, lifecycleHooksReportReaderDrainPark) {
  SharedMutexHookRecorder events;
  SharedMutexHookRegistration registration(events);
  HookedSharedMutex mutex;
  mutex.lock_shared();
  events.block(HookEvent::BeforeParkReadersToDrain);

  std::thread writer([&] {
    mutex.lock();
    mutex.unlock();
  });
  const bool didPark = events.waitFor(HookEvent::BeforeParkReadersToDrain);
  mutex.unlock_shared();
  events.unblock(HookEvent::BeforeParkReadersToDrain);
  writer.join();

  ASSERT_TRUE(didPark);
  ASSERT_GE(events.count(HookEvent::BeforeParkReadersToDrain), 1);
  ASSERT_TRUE(events.completedWithoutTimeout());
}

TEST(SharedMutex, lifecycleHooksStressConcurrentSharedRelease) {
  constexpr size_t kReaders = 32;
  SharedMutexHookRecorder events;
  SharedMutexHookRegistration registration(events);
  bool allReadersReady = false;
  {
    HookedSharedMutex mutex;
    std::mutex gateMutex;
    std::condition_variable gateCv;
    size_t ready = 0;
    bool release = false;
    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (size_t i = 0; i < kReaders; ++i) {
      readers.emplace_back([&] {
        mutex.lock_shared();
        {
          std::unique_lock lock(gateMutex);
          ++ready;
          gateCv.notify_all();
          gateCv.wait(lock, [&] { return release; });
        }
        mutex.unlock_shared();
      });
    }
    {
      std::unique_lock lock(gateMutex);
      allReadersReady = gateCv.wait_for(lock, kLifecycleHookTestTimeout, [&] {
        return ready == kReaders;
      });
      release = true;
      gateCv.notify_all();
    }
    for (auto& reader : readers) {
      reader.join();
    }
  }

  ASSERT_TRUE(allReadersReady);
  ASSERT_EQ(kReaders, events.count(HookEvent::AfterReleaseShared));
  ASSERT_EQ(1, events.count(HookEvent::BeforeDestroy));
  ASSERT_TRUE(events.completedWithoutTimeout());
}

TEST(SharedMutex, selfDestructionAfterExclusiveRelease) {
  {
    SCOPED_TRACE("write priority");
    runSelfDestructionAfterReleaseTest<
        HookedSharedMutex,
        LifetimeReleaseMode::Exclusive>();
  }
  {
    SCOPED_TRACE("reader priority");
    runSelfDestructionAfterReleaseTest<
        HookedSharedMutexReadPriority,
        LifetimeReleaseMode::Exclusive>();
  }
}

TEST(SharedMutex, selfDestructionAfterLastSharedRelease) {
  {
    SCOPED_TRACE("write priority");
    runSelfDestructionAfterReleaseTest<
        HookedSharedMutex,
        LifetimeReleaseMode::Shared>();
  }
  {
    SCOPED_TRACE("reader priority");
    runSelfDestructionAfterReleaseTest<
        HookedSharedMutexReadPriority,
        LifetimeReleaseMode::Shared>();
  }
}

TEST(SharedMutex, selfDestructionAfterUpgradeRelease) {
  {
    SCOPED_TRACE("write priority");
    runSelfDestructionAfterReleaseTest<
        HookedSharedMutex,
        LifetimeReleaseMode::Upgrade>();
  }
  {
    SCOPED_TRACE("reader priority");
    runSelfDestructionAfterReleaseTest<
        HookedSharedMutexReadPriority,
        LifetimeReleaseMode::Upgrade>();
  }
}

TEST(SharedMutex, saturatedExclusiveWaitersAfterExclusiveRelease) {
  {
    SCOPED_TRACE("write priority");
    runSaturatedExclusiveWaitersTest<
        HookedSharedMutex,
        LifetimeReleaseMode::Exclusive>();
  }
  {
    SCOPED_TRACE("reader priority");
    runSaturatedExclusiveWaitersTest<
        HookedSharedMutexReadPriority,
        LifetimeReleaseMode::Exclusive>();
  }
}

TEST(SharedMutex, saturatedExclusiveWaitersAfterUpgradeRelease) {
  {
    SCOPED_TRACE("write priority");
    runSaturatedExclusiveWaitersTest<
        HookedSharedMutex,
        LifetimeReleaseMode::Upgrade>();
  }
  {
    SCOPED_TRACE("reader priority");
    runSaturatedExclusiveWaitersTest<
        HookedSharedMutexReadPriority,
        LifetimeReleaseMode::Upgrade>();
  }
}

TEST(SharedMutex, selfDestructionDuringSaturatedRelease) {
  {
    SCOPED_TRACE("write priority");
    runSelfDestructionDuringSaturatedReleaseTest<HookedSharedMutex>();
  }
  {
    SCOPED_TRACE("reader priority");
    runSelfDestructionDuringSaturatedReleaseTest<
        HookedSharedMutexReadPriority>();
  }
}

TEST(SharedMutex, readerDrainWaitBitClearedByExclusiveToUpgradeConversion) {
  runReaderDrainWaitBitConversionTest<
      HookedSharedMutex,
      ExclusiveConversion::Upgrade>();
  runReaderDrainWaitBitConversionTest<
      HookedSharedMutexReadPriority,
      ExclusiveConversion::Upgrade>();
}

TEST(SharedMutex, readerDrainWaitBitClearedByExclusiveToSharedConversion) {
  runReaderDrainWaitBitConversionTest<
      HookedSharedMutex,
      ExclusiveConversion::Shared>();
  runReaderDrainWaitBitConversionTest<
      HookedSharedMutexReadPriority,
      ExclusiveConversion::Shared>();
}

template <typename Lock>
void expectLockState(const Lock& lock, bool unique, bool shared, bool upgrade) {
  const auto state = lock.get_lock_state();
  EXPECT_EQ(unique, state.unique);
  EXPECT_EQ(shared, state.shared);
  EXPECT_EQ(upgrade, state.upgrade);
}

template <typename Lock>
void runLockStateTest() {
  {
    Lock lock;
    expectLockState(lock, false, false, false);
    lock.lock();
    expectLockState(lock, true, false, false);
    lock.unlock();
  }
  {
    Lock lock;
    lock.lock_shared();
    expectLockState(lock, false, true, false);
    lock.unlock_shared();
  }
  {
    Lock lock;
    lock.lock_upgrade();
    expectLockState(lock, false, false, true);
    lock.lock_shared();
    expectLockState(lock, false, true, true);
    lock.unlock_shared();
    lock.unlock_upgrade();
  }
}

TEST(SharedMutex, lockState) {
  runLockStateTest<SharedMutexReadPriority>();
  runLockStateTest<SharedMutexWritePriority>();
  runLockStateTest<SharedMutexSuppressTSAN>();
  runLockStateTest<SharedMutexTracked>();
}

template <typename Lock>
void runBasicTest() {
  Lock lock;
  SharedMutexToken token1;
  SharedMutexToken token2;
  SharedMutexToken token3;

  EXPECT_TRUE(lock.try_lock());
  EXPECT_FALSE(lock.try_lock());
  EXPECT_FALSE(lock.try_lock_shared(token1));
  lock.unlock();

  EXPECT_TRUE(lock.try_lock_shared(token1));
  EXPECT_FALSE(lock.try_lock());
  EXPECT_TRUE(lock.try_lock_shared(token2));
  lock.lock_shared(token3);
  lock.unlock_shared(token3);
  lock.unlock_shared(token2);
  lock.unlock_shared(token1);

  lock.lock();
  lock.unlock();

  lock.lock_shared(token1);
  lock.lock_shared(token2);
  lock.unlock_shared(token1);
  lock.unlock_shared(token2);

  lock.lock();
  lock.unlock_and_lock_shared(token1);
  lock.lock_shared(token2);
  lock.unlock_shared(token2);
  lock.unlock_shared(token1);
}

TEST(SharedMutex, basic) {
  runBasicTest<SharedMutexReadPriority>();
  runBasicTest<SharedMutexWritePriority>();
  runBasicTest<SharedMutexSuppressTSAN>();
  runBasicTest<SharedMutexTracked>();
}

template <typename Lock>
void runBasicHoldersTest() {
  Lock lock;
  SharedMutexToken token;

  {
    // create an exclusive write lock via holder
    std::unique_lock holder(lock);
    EXPECT_FALSE(lock.try_lock());
    EXPECT_FALSE(lock.try_lock_shared(token));

    // move ownership to another write holder via move constructor
    std::unique_lock holder2(std::move(holder));
    EXPECT_FALSE(lock.try_lock());
    EXPECT_FALSE(lock.try_lock_shared(token));

    // move ownership to another write holder via assign operator
    std::unique_lock holder3(lock, std::defer_lock);
    holder3 = std::move(holder2);
    EXPECT_FALSE(lock.try_lock());
    EXPECT_FALSE(lock.try_lock_shared(token));

    // downgrade from exclusive to upgrade lock via move constructor
    folly::upgrade_lock holder4(
        folly::transition_lock<folly::upgrade_lock>(holder3));

    // ensure we can lock from a shared source
    EXPECT_FALSE(lock.try_lock());
    EXPECT_TRUE(lock.try_lock_shared(token));
    lock.unlock_shared(token);

    // promote from upgrade to exclusive lock via move constructor
    std::unique_lock holder5(folly::transition_lock<std::unique_lock>(holder4));
    EXPECT_FALSE(lock.try_lock());
    EXPECT_FALSE(lock.try_lock_shared(token));

    // downgrade exclusive to shared lock via move constructor
    std::shared_lock holder6(folly::transition_lock<std::shared_lock>(holder5));

    // ensure we can lock from another shared source
    EXPECT_FALSE(lock.try_lock());
    EXPECT_TRUE(lock.try_lock_shared(token));
    lock.unlock_shared(token);
  }

  {
    std::unique_lock holder(lock);
    EXPECT_FALSE(lock.try_lock());
  }

  {
    std::shared_lock holder(lock);
    std::shared_lock holder2(lock);
    folly::upgrade_lock holder3(lock);
  }

  {
    folly::upgrade_lock holder(lock);
    std::shared_lock holder2(lock);
    std::shared_lock holder3(folly::transition_lock<std::shared_lock>(holder));
  }
}

TEST(SharedMutex, basicHolders) {
  runBasicHoldersTest<SharedMutexReadPriority>();
  runBasicHoldersTest<SharedMutexWritePriority>();
  runBasicHoldersTest<SharedMutexSuppressTSAN>();
  runBasicHoldersTest<SharedMutexTracked>();
}

#if !FOLLY_SANITIZE_THREAD
// Thse tests fail in an assertion in the TSAN library because there are too
// many mutexes

template <typename Lock>
void runManyReadLocksTestWithTokens() {
  Lock lock;

  vector<SharedMutexToken> tokens;
  for (int i = 0; i < 1000; ++i) {
    tokens.emplace_back();
    EXPECT_TRUE(lock.try_lock_shared(tokens.back()));
  }
  for (auto& token : tokens) {
    lock.unlock_shared(token);
  }

  EXPECT_TRUE(lock.try_lock());
  lock.unlock();
}

TEST(SharedMutex, manyReadLocksWithTokens) {
  runManyReadLocksTestWithTokens<SharedMutexReadPriority>();
  runManyReadLocksTestWithTokens<SharedMutexWritePriority>();
  runManyReadLocksTestWithTokens<SharedMutexSuppressTSAN>();
  runManyReadLocksTestWithTokens<SharedMutexTracked>();
}

template <typename Lock>
void runManyReadLocksTestWithoutTokens() {
  Lock lock;

  for (int i = 0; i < 1000; ++i) {
    EXPECT_TRUE(lock.try_lock_shared());
  }
  for (int i = 0; i < 1000; ++i) {
    lock.unlock_shared();
  }

  EXPECT_TRUE(lock.try_lock());
  lock.unlock();
}

TEST(SharedMutex, manyReadLocksWithoutTokens) {
  // This test fails in an assertion in the TSAN library because there are too
  // many mutexes
  runManyReadLocksTestWithoutTokens<SharedMutexReadPriority>();
  runManyReadLocksTestWithoutTokens<SharedMutexWritePriority>();
  runManyReadLocksTestWithoutTokens<SharedMutexSuppressTSAN>();
  runManyReadLocksTestWithoutTokens<SharedMutexTracked>();
}

#endif

template <typename Lock>
void runTimeoutInPastTest() {
  Lock lock;

  EXPECT_TRUE(lock.try_lock_for(milliseconds(0)));
  lock.unlock();
  EXPECT_TRUE(lock.try_lock_for(milliseconds(-1)));
  lock.unlock();
  EXPECT_TRUE(lock.try_lock_shared_for(milliseconds(0)));
  lock.unlock_shared();
  EXPECT_TRUE(lock.try_lock_shared_for(milliseconds(-1)));
  lock.unlock_shared();
  EXPECT_TRUE(lock.try_lock_until(system_clock::now() - milliseconds(1)));
  lock.unlock();
  EXPECT_TRUE(
      lock.try_lock_shared_until(system_clock::now() - milliseconds(1)));
  lock.unlock_shared();
  EXPECT_TRUE(lock.try_lock_until(steady_clock::now() - milliseconds(1)));
  lock.unlock();
  EXPECT_TRUE(
      lock.try_lock_shared_until(steady_clock::now() - milliseconds(1)));
  lock.unlock_shared();
}

TEST(SharedMutex, timeoutInPast) {
  runTimeoutInPastTest<SharedMutexReadPriority>();
  runTimeoutInPastTest<SharedMutexWritePriority>();
  runTimeoutInPastTest<SharedMutexSuppressTSAN>();
  runTimeoutInPastTest<SharedMutexTracked>();
}

template <class Func>
bool funcHasDuration(milliseconds expectedDuration, Func func) {
  // elapsed time should eventually fall within expectedDuration +- 25%
  for (int tries = 0; tries < 100; ++tries) {
    auto start = steady_clock::now();
    func();
    auto elapsed = steady_clock::now() - start;
    if (elapsed > expectedDuration - expectedDuration / 4 &&
        elapsed < expectedDuration + expectedDuration / 4) {
      return true;
    }
  }
  return false;
}

template <typename Lock>
void runFailingTryTimeoutTest() {
  // TODO: Investigate what's up here. A state invariant within
  // SharedMutex underflows on Windows.
  SKIP_IF(folly::kIsWindows);

  Lock lock;
  lock.lock();
  EXPECT_TRUE(funcHasDuration(milliseconds(10), [&] {
    EXPECT_FALSE(lock.try_lock_for(milliseconds(10)));
  }));
  EXPECT_TRUE(funcHasDuration(milliseconds(10), [&] {
    typename Lock::Token token;
    EXPECT_FALSE(lock.try_lock_shared_for(milliseconds(10), token));
  }));
  EXPECT_TRUE(funcHasDuration(milliseconds(10), [&] {
    EXPECT_FALSE(lock.try_lock_upgrade_for(milliseconds(10)));
  }));
  EXPECT_TRUE(funcHasDuration(milliseconds(10), [&] {
    EXPECT_FALSE(lock.try_lock_until(steady_clock::now() + milliseconds(10)));
  }));
  EXPECT_TRUE(funcHasDuration(milliseconds(10), [&] {
    typename Lock::Token token;
    EXPECT_FALSE(lock.try_lock_shared_until(
        steady_clock::now() + milliseconds(10), token));
  }));
  EXPECT_TRUE(funcHasDuration(milliseconds(10), [&] {
    EXPECT_FALSE(
        lock.try_lock_upgrade_until(steady_clock::now() + milliseconds(10)));
  }));
  EXPECT_TRUE(funcHasDuration(milliseconds(10), [&] {
    EXPECT_FALSE(lock.try_lock_until(system_clock::now() + milliseconds(10)));
  }));
  EXPECT_TRUE(funcHasDuration(milliseconds(10), [&] {
    typename Lock::Token token;
    EXPECT_FALSE(lock.try_lock_shared_until(
        system_clock::now() + milliseconds(10), token));
  }));
  EXPECT_TRUE(funcHasDuration(milliseconds(10), [&] {
    EXPECT_FALSE(
        lock.try_lock_upgrade_until(system_clock::now() + milliseconds(10)));
  }));
  lock.unlock();

  lock.lock_shared();
  EXPECT_TRUE(funcHasDuration(milliseconds(10), [&] {
    EXPECT_FALSE(lock.try_lock_for(milliseconds(10)));
  }));
  EXPECT_TRUE(funcHasDuration(milliseconds(10), [&] {
    EXPECT_FALSE(lock.try_lock_until(steady_clock::now() + milliseconds(10)));
  }));
  EXPECT_TRUE(funcHasDuration(milliseconds(10), [&] {
    EXPECT_FALSE(lock.try_lock_until(system_clock::now() + milliseconds(10)));
  }));
  lock.unlock_shared();

  lock.lock();
  for (int p = 0; p < 8; ++p) {
    EXPECT_FALSE(lock.try_lock_for(nanoseconds(1 << p)));
  }
  lock.unlock();

  for (int p = 0; p < 8; ++p) {
    typename std::shared_lock holder1(lock);
    typename std::shared_lock holder2(lock);
    typename std::shared_lock holder3(lock);
    EXPECT_FALSE(lock.try_lock_for(nanoseconds(1 << p)));
  }
}

TEST(SharedMutex, failingTryTimeout) {
  runFailingTryTimeoutTest<SharedMutexReadPriority>();
  runFailingTryTimeoutTest<SharedMutexWritePriority>();
  runFailingTryTimeoutTest<SharedMutexSuppressTSAN>();
  runFailingTryTimeoutTest<SharedMutexTracked>();
}

template <typename Lock>
void runBasicUpgradeTest() {
  Lock lock;
  typename Lock::Token token1;
  typename Lock::Token token2;

  lock.lock_upgrade();
  EXPECT_FALSE(lock.try_lock());
  EXPECT_TRUE(lock.try_lock_shared(token1));
  lock.unlock_shared(token1);
  lock.unlock_upgrade();

  lock.lock_upgrade();
  lock.unlock_upgrade_and_lock();
  EXPECT_FALSE(lock.try_lock_shared(token1));
  lock.unlock();

  lock.lock_upgrade();
  lock.unlock_upgrade_and_lock_shared(token1);
  lock.lock_upgrade();
  lock.unlock_upgrade_and_lock_shared(token2);
  lock.unlock_shared(token1);
  lock.unlock_shared(token2);

  lock.lock();
  lock.unlock_and_lock_upgrade();
  EXPECT_TRUE(lock.try_lock_shared(token1));
  lock.unlock_upgrade();
  lock.unlock_shared(token1);
}

TEST(SharedMutex, basicUpgradeTests) {
  runBasicUpgradeTest<SharedMutexReadPriority>();
  runBasicUpgradeTest<SharedMutexWritePriority>();
  runBasicUpgradeTest<SharedMutexSuppressTSAN>();
  runBasicUpgradeTest<SharedMutexTracked>();
}

TEST(SharedMutex, readHasPrio) {
  SharedMutexReadPriority lock;
  SharedMutexToken token1;
  SharedMutexToken token2;
  lock.lock_shared(token1);
  bool exclusiveAcquired = false;
  auto writer = thread([&] {
    lock.lock();
    exclusiveAcquired = true;
    lock.unlock();
  });

  // lock() can't complete until we unlock token1, but it should stake
  // its claim with regards to other exclusive or upgrade locks.  We can
  // use try_lock_upgrade to poll for that eventuality.
  while (lock.try_lock_upgrade()) {
    lock.unlock_upgrade();
    this_thread::yield();
  }
  EXPECT_FALSE(exclusiveAcquired);

  // Even though lock() is stuck we should be able to get token2
  EXPECT_TRUE(lock.try_lock_shared(token2));
  lock.unlock_shared(token1);
  lock.unlock_shared(token2);
  writer.join();
  EXPECT_TRUE(exclusiveAcquired);
}

TEST(SharedMutex, writeHasPrio) {
  SharedMutexWritePriority lock;
  SharedMutexToken token1;
  SharedMutexToken token2;
  lock.lock_shared(token1);
  auto writer = thread([&] {
    lock.lock();
    lock.unlock();
  });

  // eventually lock() should block readers
  while (lock.try_lock_shared(token2)) {
    lock.unlock_shared(token2);
    this_thread::yield();
  }

  lock.unlock_shared(token1);
  writer.join();
}

struct TokenLocker {
  SharedMutexToken token;

  template <typename T>
  void lock(T* lockable) {
    lockable->lock();
  }

  template <typename T>
  void unlock(T* lockable) {
    lockable->unlock();
  }

  template <typename T>
  void lock_shared(T* lockable) {
    lockable->lock_shared(token);
  }

  template <typename T>
  void unlock_shared(T* lockable) {
    lockable->unlock_shared(token);
  }
};

struct Locker {
  template <typename T>
  void lock(T* lockable) {
    lockable->lock();
  }

  template <typename T>
  void unlock(T* lockable) {
    lockable->unlock();
  }

  template <typename T>
  void lock_shared(T* lockable) {
    lockable->lock_shared();
  }

  template <typename T>
  void unlock_shared(T* lockable) {
    lockable->unlock_shared();
  }
};

struct EnterLocker {
  template <typename T>
  void lock(T* lockable) {
    lockable->lock(0);
  }

  template <typename T>
  void unlock(T* lockable) {
    lockable->unlock();
  }

  template <typename T>
  void lock_shared(T* lockable) {
    lockable->enter(0);
  }

  template <typename T>
  void unlock_shared(T* lockable) {
    lockable->leave();
  }
};

struct PosixRWLock {
  pthread_rwlock_t lock_;

  PosixRWLock() { pthread_rwlock_init(&lock_, nullptr); }

  ~PosixRWLock() { pthread_rwlock_destroy(&lock_); }

  void lock() { pthread_rwlock_wrlock(&lock_); }

  void unlock() { pthread_rwlock_unlock(&lock_); }

  void lock_shared() { pthread_rwlock_rdlock(&lock_); }

  void unlock_shared() { pthread_rwlock_unlock(&lock_); }
};

struct PosixMutex {
  pthread_mutex_t lock_;

  PosixMutex() { pthread_mutex_init(&lock_, nullptr); }

  ~PosixMutex() { pthread_mutex_destroy(&lock_); }

  void lock() { pthread_mutex_lock(&lock_); }

  void unlock() { pthread_mutex_unlock(&lock_); }

  void lock_shared() { pthread_mutex_lock(&lock_); }

  void unlock_shared() { pthread_mutex_unlock(&lock_); }
};

// In TSAN, tests run a lot slower. To avoid test timeouts, adjust the number
// of repetitions we need for tests.
static std::size_t adjustReps(std::size_t reps) {
  if (folly::kIsSanitizeThread) {
    return reps / 10;
  }
  return reps;
}

template <template <typename> class Atom, typename Lock, typename Locker>
static void runContendedReaders(
    size_t numOps, size_t numThreads, bool useSeparateLocks) {
  struct alignas(folly::hardware_destructive_interference_size)
      GlobalLockAndProtectedValue {
    Lock globalLock;
    int valueProtectedByLock = 10;
  };
  GlobalLockAndProtectedValue padded;
  Atom<bool> go(false);
  Atom<bool>* goPtr = &go; // workaround for clang bug
  vector<thread> threads(numThreads);

  BENCHMARK_SUSPEND {
    for (size_t t = 0; t < numThreads; ++t) {
      threads[t] = DSched::thread([&, t, numThreads] {
        Lock privateLock;
        Lock* lock = useSeparateLocks ? &privateLock : &(padded.globalLock);
        Locker locker;
        while (!goPtr->load()) {
          this_thread::yield();
        }
        for (size_t op = t; op < numOps; op += numThreads) {
          locker.lock_shared(lock);
          // note: folly::doNotOptimizeAway reads and writes to its arg,
          // so the following two lines are very different than a call
          // to folly::doNotOptimizeAway(valueProtectedByLock);
          auto copy = padded.valueProtectedByLock;
          folly::doNotOptimizeAway(copy);
          locker.unlock_shared(lock);
        }
      });
    }
  }

  go.store(true);
  for (auto& thr : threads) {
    DSched::join(thr);
  }
}

static void folly_rwspin_reads(
    uint32_t numOps, size_t numThreads, bool useSeparateLocks) {
  runContendedReaders<atomic, RWSpinLock, Locker>(
      numOps, numThreads, useSeparateLocks);
}

static void shmtx_wr_pri_reads(
    uint32_t numOps, size_t numThreads, bool useSeparateLocks) {
  runContendedReaders<atomic, SharedMutexWritePriority, TokenLocker>(
      numOps, numThreads, useSeparateLocks);
}

static void shmtx_w_bare_reads(
    uint32_t numOps, size_t numThreads, bool useSeparateLocks) {
  runContendedReaders<atomic, SharedMutexWritePriority, Locker>(
      numOps, numThreads, useSeparateLocks);
}

static void shmtx_rd_pri_reads(
    uint32_t numOps, size_t numThreads, bool useSeparateLocks) {
  runContendedReaders<atomic, SharedMutexReadPriority, TokenLocker>(
      numOps, numThreads, useSeparateLocks);
}

static void shmtx_r_bare_reads(
    uint32_t numOps, size_t numThreads, bool useSeparateLocks) {
  runContendedReaders<atomic, SharedMutexReadPriority, Locker>(
      numOps, numThreads, useSeparateLocks);
}

static void std_shared_reads(
    uint32_t numOps, size_t numThreads, bool useSeparateLocks) {
  runContendedReaders<atomic, std::shared_mutex, Locker>(
      numOps, numThreads, useSeparateLocks);
}

static void pthrd_rwlock_reads(
    uint32_t numOps, size_t numThreads, bool useSeparateLocks) {
  runContendedReaders<atomic, PosixRWLock, Locker>(
      numOps, numThreads, useSeparateLocks);
}

static void timed_wr_pri_reads(
    uint32_t numOps, size_t numThreads, bool useSeparateLocks) {
  runContendedReaders<
      atomic,
      fibers::TimedRWMutexWritePriority<fibers::Baton>,
      Locker>(numOps, numThreads, useSeparateLocks);
}

static void timed_rd_pri_reads(
    uint32_t numOps, size_t numThreads, bool useSeparateLocks) {
  runContendedReaders<
      atomic,
      fibers::TimedRWMutexReadPriority<fibers::Baton>,
      Locker>(numOps, numThreads, useSeparateLocks);
}

template <template <typename> class Atom, typename Lock, typename Locker>
static void runMixed(
    size_t numOps,
    size_t numThreads,
    double writeFraction,
    bool useSeparateLocks) {
  struct alignas(folly::hardware_destructive_interference_size)
      GlobalLockAndProtectedValue {
    Lock globalLock;
    int valueProtectedByLock = 0;
  };
  GlobalLockAndProtectedValue padded;
  Atom<bool> go(false);
  Atom<bool>* goPtr = &go; // workaround for clang bug
  vector<thread> threads(numThreads);

  BENCHMARK_SUSPEND {
    for (size_t t = 0; t < numThreads; ++t) {
      threads[t] = DSched::thread([&, t, numThreads] {
        std::minstd_rand engine;
        engine.seed(t);

        long writeThreshold = to_integral(writeFraction * 0x7fffffff);
        Lock privateLock;
        Lock* lock = useSeparateLocks ? &privateLock : &(padded.globalLock);
        Locker locker;
        while (!goPtr->load()) {
          this_thread::yield();
        }
        for (size_t op = t; op < numOps; op += numThreads) {
          long randVal = engine();
          bool writeOp = randVal < writeThreshold;
          if (writeOp) {
            locker.lock(lock);
            if (!useSeparateLocks) {
              ++(padded.valueProtectedByLock);
            }
            locker.unlock(lock);
          } else {
            locker.lock_shared(lock);
            auto v = padded.valueProtectedByLock;
            folly::doNotOptimizeAway(v);
            locker.unlock_shared(lock);
          }
        }
      });
    }
  }

  go.store(true);
  for (auto& thr : threads) {
    DSched::join(thr);
  }
}

static void folly_rwspin(
    size_t numOps,
    size_t numThreads,
    double writeFraction,
    bool useSeparateLocks) {
  runMixed<atomic, RWSpinLock, Locker>(
      numOps, numThreads, writeFraction, useSeparateLocks);
}

static void shmtx_wr_pri(
    uint32_t numOps,
    size_t numThreads,
    double writeFraction,
    bool useSeparateLocks) {
  runMixed<atomic, SharedMutexWritePriority, TokenLocker>(
      numOps, numThreads, writeFraction, useSeparateLocks);
}

static void shmtx_w_bare(
    uint32_t numOps,
    size_t numThreads,
    double writeFraction,
    bool useSeparateLocks) {
  runMixed<atomic, SharedMutexWritePriority, Locker>(
      numOps, numThreads, writeFraction, useSeparateLocks);
}

static void shmtx_rd_pri(
    uint32_t numOps,
    size_t numThreads,
    double writeFraction,
    bool useSeparateLocks) {
  runMixed<atomic, SharedMutexReadPriority, TokenLocker>(
      numOps, numThreads, writeFraction, useSeparateLocks);
}

static void shmtx_r_bare(
    uint32_t numOps,
    size_t numThreads,
    double writeFraction,
    bool useSeparateLocks) {
  runMixed<atomic, SharedMutexReadPriority, Locker>(
      numOps, numThreads, writeFraction, useSeparateLocks);
}

static void std_shared(
    size_t numOps,
    size_t numThreads,
    double writeFraction,
    bool useSeparateLocks) {
  runMixed<atomic, std::shared_mutex, Locker>(
      numOps, numThreads, writeFraction, useSeparateLocks);
}

static void pthrd_rwlock(
    size_t numOps,
    size_t numThreads,
    double writeFraction,
    bool useSeparateLocks) {
  runMixed<atomic, PosixRWLock, Locker>(
      numOps, numThreads, writeFraction, useSeparateLocks);
}

static void pthrd_mutex_(
    size_t numOps,
    size_t numThreads,
    double writeFraction,
    bool useSeparateLocks) {
  runMixed<atomic, PosixMutex, Locker>(
      numOps, numThreads, writeFraction, useSeparateLocks);
}

static void timed_wr_pri(
    size_t numOps,
    size_t numThreads,
    double writeFraction,
    bool useSeparateLocks) {
  runMixed<atomic, fibers::TimedRWMutexWritePriority<fibers::Baton>, Locker>(
      numOps, numThreads, writeFraction, useSeparateLocks);
}

static void timed_rd_pri(
    size_t numOps,
    size_t numThreads,
    double writeFraction,
    bool useSeparateLocks) {
  runMixed<atomic, fibers::TimedRWMutexReadPriority<fibers::Baton>, Locker>(
      numOps, numThreads, writeFraction, useSeparateLocks);
}

template <typename Lock, template <typename> class Atom>
static void runAllAndValidate(size_t numOps, size_t numThreads) {
  Lock globalLock;
  Atom<int> globalExclusiveCount(0);
  Atom<int> globalUpgradeCount(0);
  Atom<int> globalSharedCount(0);

  Atom<bool> go(false);

  // clang crashes on access to Atom<> captured by ref in closure
  Atom<int>* globalExclusiveCountPtr = &globalExclusiveCount;
  Atom<int>* globalUpgradeCountPtr = &globalUpgradeCount;
  Atom<int>* globalSharedCountPtr = &globalSharedCount;
  Atom<bool>* goPtr = &go;

  vector<thread> threads(numThreads);

  BENCHMARK_SUSPEND {
    for (size_t t = 0; t < numThreads; ++t) {
      threads[t] = DSched::thread([&, t, numThreads] {
        std::minstd_rand engine;
        engine.seed(t);

        bool exclusive = false;
        bool upgrade = false;
        bool shared = false;
        bool ourGlobalTokenUsed = false;
        SharedMutexToken ourGlobalToken;

        Lock privateLock;
        vector<SharedMutexToken> privateTokens;

        while (!goPtr->load()) {
          this_thread::yield();
        }
        for (size_t op = t; op < numOps; op += numThreads) {
          // randVal in [0,1000)
          long randVal = engine();
          randVal = (long)((randVal * (uint64_t)1000) / 0x7fffffff);

          // make as many assertions as possible about the global state
          if (exclusive) {
            EXPECT_EQ(1, globalExclusiveCountPtr->load(memory_order_acquire));
            EXPECT_EQ(0, globalUpgradeCountPtr->load(memory_order_acquire));
            EXPECT_EQ(0, globalSharedCountPtr->load(memory_order_acquire));
          }
          if (upgrade) {
            EXPECT_EQ(0, globalExclusiveCountPtr->load(memory_order_acquire));
            EXPECT_EQ(1, globalUpgradeCountPtr->load(memory_order_acquire));
          }
          if (shared) {
            EXPECT_EQ(0, globalExclusiveCountPtr->load(memory_order_acquire));
            EXPECT_TRUE(globalSharedCountPtr->load(memory_order_acquire) > 0);
          } else {
            EXPECT_FALSE(ourGlobalTokenUsed);
          }

          // independent 20% chance we do something to the private lock
          if (randVal < 200) {
            // it's okay to take multiple private shared locks because
            // we never take an exclusive lock, so reader versus writer
            // priority doesn't cause deadlocks
            if (randVal < 100 && privateTokens.size() > 0) {
              auto i = randVal % privateTokens.size();
              privateLock.unlock_shared(privateTokens[i]);
              privateTokens.erase(privateTokens.begin() + i);
            } else {
              SharedMutexToken token;
              privateLock.lock_shared(token);
              privateTokens.push_back(token);
            }
            continue;
          }

          // if we've got a lock, the only thing we can do is release it
          // or transform it into a different kind of lock
          if (exclusive) {
            exclusive = false;
            --*globalExclusiveCountPtr;
            if (randVal < 500) {
              globalLock.unlock();
            } else if (randVal < 700) {
              globalLock.unlock_and_lock_shared();
              ++*globalSharedCountPtr;
              shared = true;
            } else if (randVal < 900) {
              globalLock.unlock_and_lock_shared(ourGlobalToken);
              ++*globalSharedCountPtr;
              shared = true;
              ourGlobalTokenUsed = true;
            } else {
              globalLock.unlock_and_lock_upgrade();
              ++*globalUpgradeCountPtr;
              upgrade = true;
            }
          } else if (upgrade) {
            upgrade = false;
            --*globalUpgradeCountPtr;
            if (randVal < 500) {
              globalLock.unlock_upgrade();
            } else if (randVal < 700) {
              globalLock.unlock_upgrade_and_lock_shared();
              ++*globalSharedCountPtr;
              shared = true;
            } else if (randVal < 900) {
              globalLock.unlock_upgrade_and_lock_shared(ourGlobalToken);
              ++*globalSharedCountPtr;
              shared = true;
              ourGlobalTokenUsed = true;
            } else {
              globalLock.unlock_upgrade_and_lock();
              ++*globalExclusiveCountPtr;
              exclusive = true;
            }
          } else if (shared) {
            shared = false;
            --*globalSharedCountPtr;
            if (ourGlobalTokenUsed) {
              globalLock.unlock_shared(ourGlobalToken);
              ourGlobalTokenUsed = false;
            } else {
              globalLock.unlock_shared();
            }
          } else if (randVal < 400) {
            // 40% chance of shared lock with token, 5 ways to get it

            // delta t goes from -1 millis to 7 millis
            auto dt = microseconds(10 * (randVal - 100));

            if (randVal < 400) {
              globalLock.lock_shared(ourGlobalToken);
              shared = true;
            } else if (randVal < 500) {
              shared = globalLock.try_lock_shared(ourGlobalToken);
            } else if (randVal < 600) {
              shared = globalLock.try_lock_shared_for(dt, ourGlobalToken);
            } else if (randVal < 800) {
              shared = globalLock.try_lock_shared_until(
                  system_clock::now() + dt, ourGlobalToken);
            }
            if (shared) {
              ourGlobalTokenUsed = true;
              ++*globalSharedCountPtr;
            }
          } else if (randVal < 800) {
            // 40% chance of shared lock without token
            auto dt = microseconds(10 * (randVal - 100));
            if (randVal < 400) {
              globalLock.lock_shared();
              shared = true;
            } else if (randVal < 500) {
              shared = globalLock.try_lock_shared();
            } else if (randVal < 600) {
              shared = globalLock.try_lock_shared_for(dt);
            } else if (randVal < 800) {
              shared =
                  globalLock.try_lock_shared_until(system_clock::now() + dt);
            }
            if (shared) {
              ++*globalSharedCountPtr;
            }
          } else if (randVal < 900) {
            // 10% change of upgrade lock
            globalLock.lock_upgrade();
            upgrade = true;
            ++*globalUpgradeCountPtr;
          } else {
            // 10% chance of exclusive lock, 5 ways to get it

            // delta t goes from -1 millis to 9 millis
            auto dt = microseconds(100 * (randVal - 910));

            if (randVal < 400) {
              globalLock.lock();
              exclusive = true;
            } else if (randVal < 500) {
              exclusive = globalLock.try_lock();
            } else if (randVal < 600) {
              exclusive = globalLock.try_lock_for(dt);
            } else if (randVal < 700) {
              exclusive = globalLock.try_lock_until(steady_clock::now() + dt);
            } else {
              exclusive = globalLock.try_lock_until(system_clock::now() + dt);
            }
            if (exclusive) {
              ++*globalExclusiveCountPtr;
            }
          }
        }

        if (exclusive) {
          --*globalExclusiveCountPtr;
          globalLock.unlock();
        }
        if (upgrade) {
          --*globalUpgradeCountPtr;
          globalLock.unlock_upgrade();
        }
        if (shared) {
          --*globalSharedCountPtr;
          if (ourGlobalTokenUsed) {
            globalLock.unlock_shared(ourGlobalToken);
            ourGlobalTokenUsed = false;
          } else {
            globalLock.unlock_shared();
          }
        }
        for (auto& token : privateTokens) {
          privateLock.unlock_shared(token);
        }
      });
    }
  }

  go.store(true);
  for (auto& thr : threads) {
    DSched::join(thr);
  }
}

TEST(SharedMutex, deterministicConcurrentReadersOfOneLockReadPrio) {
  for (int pass = 0; pass < 3; ++pass) {
    DSched sched(DSched::uniform(pass));
    runContendedReaders<DeterministicAtomic, DSharedMutexReadPriority, Locker>(
        1000, 3, false);
  }
}

TEST(SharedMutex, deterministicConcurrentReadersOfOneLockWritePrio) {
  for (int pass = 0; pass < 3; ++pass) {
    DSched sched(DSched::uniform(pass));
    runContendedReaders<DeterministicAtomic, DSharedMutexWritePriority, Locker>(
        1000, 3, false);
  }
}

TEST(SharedMutex, concurrentReadersOfOneLockReadPrio) {
  for (int pass = 0; pass < 10; ++pass) {
    runContendedReaders<atomic, SharedMutexReadPriority, Locker>(
        adjustReps(100000), adjustReps(32), false);
  }
}

TEST(SharedMutex, concurrentReadersOfOneLockWritePrio) {
  for (int pass = 0; pass < 10; ++pass) {
    runContendedReaders<atomic, SharedMutexWritePriority, Locker>(
        adjustReps(100000), adjustReps(32), false);
  }
}

TEST(SharedMutex, deterministicReadersOfConcurrentLocksReadPrio) {
  for (int pass = 0; pass < 3; ++pass) {
    DSched sched(DSched::uniform(pass));
    runContendedReaders<DeterministicAtomic, DSharedMutexReadPriority, Locker>(
        1000, 3, true);
  }
}

TEST(SharedMutex, deterministicReadersOfConcurrentLocksWritePrio) {
  for (int pass = 0; pass < 3; ++pass) {
    DSched sched(DSched::uniform(pass));
    runContendedReaders<DeterministicAtomic, DSharedMutexWritePriority, Locker>(
        1000, 3, true);
  }
}

TEST(SharedMutex, readersOfConcurrentLocksReadPrio) {
  for (int pass = 0; pass < 10; ++pass) {
    runContendedReaders<atomic, SharedMutexReadPriority, TokenLocker>(
        adjustReps(100000), adjustReps(32), true);
  }
}

TEST(SharedMutex, readersOfConcurrentLocksWritePrio) {
  for (int pass = 0; pass < 10; ++pass) {
    runContendedReaders<atomic, SharedMutexWritePriority, TokenLocker>(
        adjustReps(100000), adjustReps(32), true);
  }
}

TEST(SharedMutex, deterministicMixedMostlyReadReadPrio) {
  for (int pass = 0; pass < 3; ++pass) {
    DSched sched(DSched::uniform(pass));
    runMixed<DeterministicAtomic, DSharedMutexReadPriority, Locker>(
        1000, 3, 0.1, false);
  }
}

TEST(SharedMutex, deterministicMixedMostlyReadWritePrio) {
  for (int pass = 0; pass < 3; ++pass) {
    DSched sched(DSched::uniform(pass));
    runMixed<DeterministicAtomic, DSharedMutexWritePriority, Locker>(
        1000, 3, 0.1, false);
  }
}

TEST(SharedMutex, mixedMostlyReadReadPrio) {
  for (int pass = 0; pass < 5; ++pass) {
    runMixed<atomic, SharedMutexReadPriority, TokenLocker>(
        adjustReps(10000), adjustReps(32), 0.1, false);
  }
}

TEST(SharedMutex, mixedMostlyReadWritePrio) {
  for (int pass = 0; pass < 5; ++pass) {
    runMixed<atomic, SharedMutexWritePriority, TokenLocker>(
        adjustReps(10000), adjustReps(32), 0.1, false);
  }
}

TEST(SharedMutex, deterministicMixedMostlyWriteReadPrio) {
  for (int pass = 0; pass < 1; ++pass) {
    DSched sched(DSched::uniform(pass));
    runMixed<DeterministicAtomic, DSharedMutexReadPriority, TokenLocker>(
        1000, 10, 0.9, false);
  }
}

TEST(SharedMutex, deterministicMixedMostlyWriteWritePrio) {
  for (int pass = 0; pass < 1; ++pass) {
    DSched sched(DSched::uniform(pass));
    runMixed<DeterministicAtomic, DSharedMutexWritePriority, TokenLocker>(
        1000, 10, 0.9, false);
  }
}

TEST(SharedMutex, deterministicLostWakeupWritePrio) {
  for (int pass = 0; pass < 10; ++pass) {
    DSched sched(DSched::uniformSubset(pass, 2, 200));
    runMixed<DeterministicAtomic, DSharedMutexWritePriority, TokenLocker>(
        1000, 3, 1.0, false);
  }
}

TEST(SharedMutex, mixedMostlyWriteReadPrio) {
  for (int pass = 0; pass < (folly::kIsSanitizeAddress ? 1 : 5); ++pass) {
    runMixed<atomic, SharedMutexReadPriority, TokenLocker>(
        adjustReps(50000), adjustReps(300), 0.9, false);
  }
}

TEST(SharedMutex, mixedMostlyWriteWritePrio) {
  for (int pass = 0; pass < (folly::kIsSanitizeAddress ? 1 : 5); ++pass) {
    runMixed<atomic, SharedMutexWritePriority, TokenLocker>(
        adjustReps(50000), adjustReps(300), 0.9, false);
  }
}

TEST(SharedMutex, deterministicAllOpsReadPrio) {
  for (int pass = 0; pass < 5; ++pass) {
    DSched sched(DSched::uniform(pass));
    runAllAndValidate<DSharedMutexReadPriority, DeterministicAtomic>(1000, 8);
  }
}

#if !FOLLY_SANITIZE_THREAD
// This test fails in TSAN because of noisy lock ordering inversions.

TEST(SharedMutex, deterministicAllOpsWritePrio) {
  for (int pass = 0; pass < 5; ++pass) {
    DSched sched(DSched::uniform(pass));
    runAllAndValidate<DSharedMutexWritePriority, DeterministicAtomic>(1000, 8);
  }
}

#endif

TEST(SharedMutex, allOpsReadPrio) {
  for (int pass = 0; pass < 5; ++pass) {
    runAllAndValidate<SharedMutexReadPriority, atomic>(adjustReps(100000), 32);
  }
}

#if !FOLLY_SANITIZE_THREAD
// This test fails in TSAN because of noisy lock ordering inversions.

TEST(SharedMutex, allOpsWritePrio) {
  for (int pass = 0; pass < 5; ++pass) {
    runAllAndValidate<SharedMutexWritePriority, atomic>(100000, 32);
  }
}

#endif

FOLLY_ASSUME_FBVECTOR_COMPATIBLE(std::optional<std::optional<SharedMutexToken>>)

// Setup is a set of threads that either grab a shared lock, or exclusive
// and then downgrade it, or upgrade then upgrade and downgrade, then
// enqueue the shared lock to a second set of threads that just performs
// unlocks.  Half of the shared locks use tokens, the others don't.
template <typename Lock, template <typename> class Atom>
static void runRemoteUnlock(
    size_t numOps,
    double preWriteFraction,
    double preUpgradeFraction,
    size_t numSendingThreads,
    size_t numReceivingThreads) {
  Lock globalLock;
  MPMCQueue<std::optional<std::optional<SharedMutexToken>>, Atom> queue(10);
  auto queuePtr = &queue; // workaround for clang crash

  Atom<bool> go(false);
  auto goPtr = &go; // workaround for clang crash
  Atom<int> pendingSenders(numSendingThreads);
  auto pendingSendersPtr = &pendingSenders; // workaround for clang crash
  vector<thread> threads(numSendingThreads + numReceivingThreads);

  BENCHMARK_SUSPEND {
    for (size_t t = 0; t < threads.size(); ++t) {
      threads[t] = DSched::thread([&, t, numSendingThreads] {
        if (t >= numSendingThreads) {
          // we're a receiver
          typename decltype(queue)::value_type elem;
          while (true) {
            queuePtr->blockingRead(elem);
            if (!elem) {
              // EOF, pass the EOF token
              queuePtr->blockingWrite(std::move(elem));
              break;
            }
            if (*elem) {
              globalLock.unlock_shared(**elem);
            } else {
              globalLock.unlock_shared();
            }
          }
          return;
        }
        // else we're a sender

        std::minstd_rand engine;
        engine.seed(t);

        while (!goPtr->load()) {
          this_thread::yield();
        }
        for (size_t op = t; op < numOps; op += numSendingThreads) {
          long unscaledRandVal = engine();

          // randVal in [0,1]
          double randVal = ((double)unscaledRandVal) / 0x7fffffff;

          // extract a bit and rescale
          bool useToken = randVal >= 0.5;
          randVal = (randVal - (useToken ? 0.5 : 0.0)) * 2;

          std::optional<SharedMutexToken> maybeToken;

          if (useToken) {
            SharedMutexToken token;
            if (randVal < preWriteFraction) {
              globalLock.lock();
              globalLock.unlock_and_lock_shared(token);
            } else if (randVal < preWriteFraction + preUpgradeFraction / 2) {
              globalLock.lock_upgrade();
              globalLock.unlock_upgrade_and_lock_shared(token);
            } else if (randVal < preWriteFraction + preUpgradeFraction) {
              globalLock.lock_upgrade();
              globalLock.unlock_upgrade_and_lock();
              globalLock.unlock_and_lock_shared(token);
            } else {
              globalLock.lock_shared(token);
            }
            maybeToken = token;
          } else {
            if (randVal < preWriteFraction) {
              globalLock.lock();
              globalLock.unlock_and_lock_shared();
            } else if (randVal < preWriteFraction + preUpgradeFraction / 2) {
              globalLock.lock_upgrade();
              globalLock.unlock_upgrade_and_lock_shared();
            } else if (randVal < preWriteFraction + preUpgradeFraction) {
              globalLock.lock_upgrade();
              globalLock.unlock_upgrade_and_lock();
              globalLock.unlock_and_lock_shared();
            } else {
              globalLock.lock_shared();
            }
          }

          // blockingWrite is emplace-like, so this automatically adds
          // another level of wrapping
          queuePtr->blockingWrite(std::make_optional(maybeToken));
        }
        if (--*pendingSendersPtr == 0) {
          queuePtr->blockingWrite(std::nullopt);
        }
      });
    }
  }

  go.store(true);
  for (auto& thr : threads) {
    DSched::join(thr);
  }
}

#if !FOLLY_SANITIZE_THREAD
// This test fails in an assertion in the TSAN library because there are too
// many mutexes

TEST(SharedMutex, deterministicRemoteWritePrio) {
  for (int pass = 0; pass < 1; ++pass) {
    DSched sched(DSched::uniform(pass));
    runRemoteUnlock<DSharedMutexWritePriority, DeterministicAtomic>(
        500, 0.1, 0.1, 5, 5);
  }
}

#endif

TEST(SharedMutex, deterministicRemoteReadPrio) {
  for (int pass = 0; pass < 1; ++pass) {
    DSched sched(DSched::uniform(pass));
    runRemoteUnlock<DSharedMutexReadPriority, DeterministicAtomic>(
        500, 0.1, 0.1, 5, 5);
  }
}

#if !FOLLY_SANITIZE_THREAD
// Thse tests fail in an assertion in the TSAN library because there are too
// many mutexes

TEST(SharedMutex, remoteWritePrio) {
  for (int pass = 0; pass < 10; ++pass) {
    runRemoteUnlock<SharedMutexWritePriority, atomic>(100000, 0.1, 0.1, 5, 5);
  }
}

TEST(SharedMutex, remoteReadPrio) {
  for (int pass = 0; pass < (folly::kIsSanitizeAddress ? 1 : 100); ++pass) {
    runRemoteUnlock<SharedMutexReadPriority, atomic>(100000, 0.1, 0.1, 5, 5);
  }
}

#endif

static void burn(size_t n) {
  for (size_t i = 0; i < n; ++i) {
    folly::doNotOptimizeAway(i);
  }
}

// Two threads and three locks, arranged so that they have to proceed
// in turn with reader/writer conflict
template <typename Lock, template <typename> class Atom = atomic>
static void runPingPong(size_t numRounds, size_t burnCount) {
  struct alignas(folly::hardware_destructive_interference_size) PaddedLock {
    Lock lock_;
  };
  array<PaddedLock, 3> paddedLocks;
  Atom<int> avail(0);
  auto availPtr = &avail; // workaround for clang crash
  Atom<bool> go(false);
  auto goPtr = &go; // workaround for clang crash
  vector<thread> threads(2);

  paddedLocks[0].lock_.lock();
  paddedLocks[1].lock_.lock();
  paddedLocks[2].lock_.lock_shared();

  BENCHMARK_SUSPEND {
    threads[0] = DSched::thread([&] {
      ++*availPtr;
      while (!goPtr->load()) {
        this_thread::yield();
      }
      for (size_t i = 0; i < numRounds; ++i) {
        paddedLocks[i % 3].lock_.unlock();
        paddedLocks[(i + 2) % 3].lock_.lock();
        burn(burnCount);
      }
    });
    threads[1] = DSched::thread([&] {
      ++*availPtr;
      while (!goPtr->load()) {
        this_thread::yield();
      }
      for (size_t i = 0; i < numRounds; ++i) {
        paddedLocks[i % 3].lock_.lock_shared();
        burn(burnCount);
        paddedLocks[(i + 2) % 3].lock_.unlock_shared();
      }
    });

    while (avail.load() < 2) {
      this_thread::yield();
    }
  }

  go.store(true);
  for (auto& thr : threads) {
    DSched::join(thr);
  }
  paddedLocks[numRounds % 3].lock_.unlock();
  paddedLocks[(numRounds + 1) % 3].lock_.unlock();
  paddedLocks[(numRounds + 2) % 3].lock_.unlock_shared();
}

template <class F>
static void parallelRun(size_t numThreads, F f) {
  std::vector<thread> threads;
  for (size_t tid = 0; tid < numThreads; ++tid) {
    threads.emplace_back([tid, &f] { f(tid); });
  }
  for (auto& t : threads) {
    t.join();
  }
}

static void folly_rwspin_ping_pong(size_t n, size_t scale, size_t burnCount) {
  runPingPong<RWSpinLock>(n / scale, burnCount);
}

static void shmtx_w_bare_ping_pong(size_t n, size_t scale, size_t burnCount) {
  runPingPong<SharedMutexWritePriority>(n / scale, burnCount);
}

static void shmtx_r_bare_ping_pong(size_t n, size_t scale, size_t burnCount) {
  runPingPong<SharedMutexReadPriority>(n / scale, burnCount);
}

static void std_shared_ping_pong(size_t n, size_t scale, size_t burnCount) {
  runPingPong<std::shared_mutex>(n / scale, burnCount);
}

static void pthrd_rwlock_ping_pong(size_t n, size_t scale, size_t burnCount) {
  runPingPong<PosixRWLock>(n / scale, burnCount);
}

static void timed_wr_pri_ping_pong(size_t n, size_t scale, size_t burnCount) {
  runPingPong<fibers::TimedRWMutexWritePriority<fibers::Baton>>(
      n / scale, burnCount);
}

static void timed_rd_pri_ping_pong(size_t n, size_t scale, size_t burnCount) {
  runPingPong<fibers::TimedRWMutexReadPriority<fibers::Baton>>(
      n / scale, burnCount);
}

#if !FOLLY_SANITIZE_THREAD
// This test fails in TSAN because some mutexes are lock_shared() in one
// thread and unlock_shared() in a different thread.

TEST(SharedMutex, deterministicPingPongWritePrio) {
  for (int pass = 0; pass < 1; ++pass) {
    DSched sched(DSched::uniform(pass));
    runPingPong<DSharedMutexWritePriority, DeterministicAtomic>(500, 0);
  }
}

#endif

TEST(SharedMutex, deterministicPingPongReadPrio) {
  for (int pass = 0; pass < 1; ++pass) {
    DSched sched(DSched::uniform(pass));
    runPingPong<DSharedMutexReadPriority, DeterministicAtomic>(500, 0);
  }
}

#if !FOLLY_SANITIZE_THREAD
// This test fails in TSAN because some mutexes are lock_shared() in one
// thread and unlock_shared() in a different thread.

TEST(SharedMutex, pingPongWritePrio) {
  for (int pass = 0; pass < 1; ++pass) {
    runPingPong<SharedMutexWritePriority, atomic>(50000, 0);
  }
}

#endif

TEST(SharedMutex, pingPongReadPrio) {
  for (int pass = 0; pass < 1; ++pass) {
    runPingPong<SharedMutexReadPriority, atomic>(50000, 0);
  }
}

TEST(SharedMutex, releaseToken) {
  {
    SharedMutex mutex;
    // Ensure sufficient contention that we get deferred locks.
    parallelRun(8, [&](size_t) {
      for (size_t i = 0; i < 100; ++i) {
        SharedMutexToken token;
        mutex.lock_shared(token);
        /* sleep override */ std::this_thread::sleep_for(1ms);
        mutex.release_token(token);
        mutex.unlock_shared();
      }
    });
  }
  {
    SharedMutex mutex;
    parallelRun(8, [&](size_t) {
      for (size_t i = 0; i < 100; ++i) {
        std::shared_lock lock{mutex};
        /* sleep override */ std::this_thread::sleep_for(1ms);
        lock.release();
        mutex.unlock_shared();
      }
    });
  }
}

TEST(SharedMutex, sharedLockBasic) {
  SharedMutex mutex;

  std::shared_lock shared_lock{mutex};
  EXPECT_TRUE(shared_lock.owns_lock());
  EXPECT_EQ(&mutex, shared_lock.mutex());

  shared_lock.unlock();
  EXPECT_FALSE(shared_lock.owns_lock());
  EXPECT_EQ(&mutex, shared_lock.mutex());

  std::unique_lock unique_lock{mutex};
  EXPECT_TRUE(unique_lock.owns_lock());
}

TEST(SharedMutex, sharedLockDefaultConstructor) {
  std::shared_lock<SharedMutex> lock;
  EXPECT_FALSE(lock.owns_lock());
  EXPECT_EQ(nullptr, lock.mutex());
  EXPECT_EQ(lock.release(), nullptr);
}

TEST(SharedMutex, sharedLockMutexConstructor) {
  SharedMutex mutex;
  std::shared_lock lock{mutex};

  EXPECT_TRUE(lock.owns_lock());
  EXPECT_EQ(&mutex, lock.mutex());
}

TEST(SharedMutex, sharedLockDeferConstructor) {
  SharedMutex mutex;
  std::shared_lock lock{mutex, std::defer_lock};

  EXPECT_FALSE(lock.owns_lock());
  EXPECT_EQ(&mutex, lock.mutex());

  EXPECT_EQ(&mutex, lock.release());
}

TEST(SharedMutex, sharedLockTryToConstructor) {
  SharedMutex mutex;

  {
    std::shared_lock lock{mutex, std::try_to_lock};

    EXPECT_TRUE(lock.owns_lock());
    EXPECT_EQ(&mutex, lock.mutex());
  }

  {
    std::unique_lock unique{mutex};
    EXPECT_TRUE(unique.owns_lock());

    std::shared_lock shared{mutex, std::try_to_lock};

    EXPECT_FALSE(shared.owns_lock());
    EXPECT_EQ(&mutex, shared.mutex());
  }
}

TEST(SharedMutex, sharedLockAdoptConstructor) {
  SharedMutex mutex;

  {
    mutex.lock_shared();
    std::shared_lock lock{mutex, std::adopt_lock};

    EXPECT_TRUE(lock.owns_lock());
    EXPECT_EQ(&mutex, lock.mutex());
  }

  {
    SharedMutexToken token;
    mutex.lock_shared(token);
    std::shared_lock lock{mutex, std::adopt_lock};

    EXPECT_TRUE(lock.owns_lock());
    EXPECT_EQ(&mutex, lock.mutex());
  }
}

TEST(SharedMutex, sharedLockDeadlineConstructor) {
  using namespace std::chrono_literals;
  const std::chrono::time_point<std::chrono::system_clock> deadline =
      std::chrono::system_clock::now() + 1ms;
  SharedMutex mutex;
  std::shared_lock lock{mutex, deadline};

  EXPECT_TRUE(lock.owns_lock());
  EXPECT_EQ(&mutex, lock.mutex());
}

TEST(SharedMutex, sharedLockTimeoutConstructor) {
  using namespace std::chrono_literals;
  SharedMutex mutex;
  std::shared_lock lock{mutex, /* timeout = */ 1ms};

  EXPECT_TRUE(lock.owns_lock());
  EXPECT_EQ(&mutex, lock.mutex());
}

TEST(SharedMutex, sharedLockDoubleUnlock) {
  SharedMutex mutex;
  std::shared_lock lock{mutex};
  lock.unlock();

  EXPECT_THROW(lock.unlock(), std::system_error);
}

TEST(SharedMutex, sharedLockLockNoMutex) {
  std::shared_lock<SharedMutex> lock;

  EXPECT_THROW(lock.lock(), std::system_error);
}

TEST(SharedMutex, sharedLockLockAlreadyHeld) {
  SharedMutex mutex;
  std::shared_lock lock{mutex};

  EXPECT_TRUE(lock.owns_lock());
  EXPECT_THROW(lock.lock(), std::system_error);
}

static void upgrade_lock_unlock_with_readers(size_t iters, size_t numReaders) {
  SharedMutex mutex;
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::atomic<size_t> ready{0};
  std::atomic<size_t> started{0};
  std::vector<std::thread> readers;

  BENCHMARK_SUSPEND {
    readers.reserve(numReaders);
    for (size_t i = 0; i < numReaders; ++i) {
      readers.emplace_back([&] {
        ++ready;
        while (!go.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        mutex.lock_shared();
        mutex.unlock_shared();
        ++started;
        while (!stop.load(std::memory_order_acquire)) {
          mutex.lock_shared();
          mutex.unlock_shared();
        }
      });
    }
    while (ready.load(std::memory_order_acquire) != numReaders) {
      std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    while (started.load(std::memory_order_acquire) != numReaders) {
      std::this_thread::yield();
    }
  }

  for (size_t i = 0; i < iters; ++i) {
    mutex.lock_upgrade();
    mutex.unlock_upgrade();
  }

  BENCHMARK_SUSPEND {
    stop.store(true, std::memory_order_release);
    for (auto& reader : readers) {
      reader.join();
    }
  }
}

// This is here so you can tell how much of the runtime reported by the
// more complex harnesses is due to the harness, although due to the
// magic of compiler optimization it may also be slower
BENCHMARK(single_thread_lock_shared_unlock_shared, iters) {
  SharedMutex lock;
  for (size_t n = 0; n < iters; ++n) {
    SharedMutex::Token token;
    lock.lock_shared(token);
    folly::doNotOptimizeAway(0);
    lock.unlock_shared(token);
  }
}

BENCHMARK(single_thread_lock_unlock, iters) {
  SharedMutex lock;
  for (size_t n = 0; n < iters; ++n) {
    lock.lock();
    folly::doNotOptimizeAway(0);
    lock.unlock();
  }
}

BENCHMARK_DRAW_LINE();
BENCHMARK_NAMED_PARAM(upgrade_lock_unlock_with_readers, 1reader, 1)
BENCHMARK_NAMED_PARAM(upgrade_lock_unlock_with_readers, 4readers, 4)

#define BENCH_BASE(...) FB_VA_GLUE(BENCHMARK_NAMED_PARAM, (__VA_ARGS__))
#define BENCH_REL(...) FB_VA_GLUE(BENCHMARK_RELATIVE_NAMED_PARAM, (__VA_ARGS__))

// 100% reads.  Best-case scenario for deferred locks.  Lock is colocated
// with read data, so inline lock takes cache miss every time but deferred
// lock has only cache hits and local access.
BENCHMARK_DRAW_LINE();
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin_reads, 1thread, 1, false)
BENCH_REL(shmtx_wr_pri_reads, 1thread, 1, false)
BENCH_REL(shmtx_w_bare_reads, 1thread, 1, false)
BENCH_REL(shmtx_rd_pri_reads, 1thread, 1, false)
BENCH_REL(shmtx_r_bare_reads, 1thread, 1, false)
BENCH_REL(std_shared_reads, 1thread, 1, false)
BENCH_REL(pthrd_rwlock_reads, 1thread, 1, false)
BENCH_REL(timed_wr_pri_reads, 1thread, 1, false)
BENCH_REL(timed_rd_pri_reads, 1thread, 1, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin_reads, 2thread, 2, false)
BENCH_REL(shmtx_wr_pri_reads, 2thread, 2, false)
BENCH_REL(shmtx_w_bare_reads, 2thread, 2, false)
BENCH_REL(shmtx_rd_pri_reads, 2thread, 2, false)
BENCH_REL(shmtx_r_bare_reads, 2thread, 2, false)
BENCH_REL(std_shared_reads, 2thread, 2, false)
BENCH_REL(pthrd_rwlock_reads, 2thread, 2, false)
BENCH_REL(timed_wr_pri_reads, 2thread, 2, false)
BENCH_REL(timed_rd_pri_reads, 2thread, 2, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin_reads, 4thread, 4, false)
BENCH_REL(shmtx_wr_pri_reads, 4thread, 4, false)
BENCH_REL(shmtx_w_bare_reads, 4thread, 4, false)
BENCH_REL(shmtx_rd_pri_reads, 4thread, 4, false)
BENCH_REL(shmtx_r_bare_reads, 4thread, 4, false)
BENCH_REL(std_shared_reads, 4thread, 4, false)
BENCH_REL(pthrd_rwlock_reads, 4thread, 4, false)
BENCH_REL(timed_wr_pri_reads, 4thread, 4, false)
BENCH_REL(timed_rd_pri_reads, 4thread, 4, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin_reads, 8thread, 8, false)
BENCH_REL(shmtx_wr_pri_reads, 8thread, 8, false)
BENCH_REL(shmtx_w_bare_reads, 8thread, 8, false)
BENCH_REL(shmtx_rd_pri_reads, 8thread, 8, false)
BENCH_REL(shmtx_r_bare_reads, 8thread, 8, false)
BENCH_REL(std_shared_reads, 8thread, 8, false)
BENCH_REL(pthrd_rwlock_reads, 8thread, 8, false)
BENCH_REL(timed_wr_pri_reads, 8thread, 8, false)
BENCH_REL(timed_rd_pri_reads, 8thread, 8, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin_reads, 16thread, 16, false)
BENCH_REL(shmtx_wr_pri_reads, 16thread, 16, false)
BENCH_REL(shmtx_w_bare_reads, 16thread, 16, false)
BENCH_REL(shmtx_rd_pri_reads, 16thread, 16, false)
BENCH_REL(shmtx_r_bare_reads, 16thread, 16, false)
BENCH_REL(std_shared_reads, 16thread, 16, false)
BENCH_REL(pthrd_rwlock_reads, 16thread, 16, false)
BENCH_REL(timed_wr_pri_reads, 16thread, 16, false)
BENCH_REL(timed_rd_pri_reads, 16thread, 16, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin_reads, 32thread, 32, false)
BENCH_REL(shmtx_wr_pri_reads, 32thread, 32, false)
BENCH_REL(shmtx_w_bare_reads, 32thread, 32, false)
BENCH_REL(shmtx_rd_pri_reads, 32thread, 32, false)
BENCH_REL(shmtx_r_bare_reads, 32thread, 32, false)
BENCH_REL(std_shared_reads, 32thread, 32, false)
BENCH_REL(pthrd_rwlock_reads, 32thread, 32, false)
BENCH_REL(timed_wr_pri_reads, 32thread, 32, false)
BENCH_REL(timed_rd_pri_reads, 32thread, 32, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin_reads, 64thread, 64, false)
BENCH_REL(shmtx_wr_pri_reads, 64thread, 64, false)
BENCH_REL(shmtx_w_bare_reads, 64thread, 64, false)
BENCH_REL(shmtx_rd_pri_reads, 64thread, 64, false)
BENCH_REL(shmtx_r_bare_reads, 64thread, 64, false)
BENCH_REL(std_shared_reads, 64thread, 64, false)
BENCH_REL(pthrd_rwlock_reads, 64thread, 64, false)
BENCH_REL(timed_wr_pri_reads, 64thread, 64, false)
BENCH_REL(timed_rd_pri_reads, 64thread, 64, false)

// 1 lock used by everybody, 100% writes.  Threads only hurt, but it is
// good to not fail catastrophically.  Compare to single_thread_lock_unlock
// to see the overhead of the generic driver (and its pseudo-random number
// generator).  pthrd_mutex_ is a pthread_mutex_t (default, not adaptive),
// which is better than any of the reader-writer locks for this scenario.
BENCHMARK_DRAW_LINE();
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 1thread_all_write, 1, 1.0, false)
BENCH_REL(shmtx_wr_pri, 1thread_all_write, 1, 1.0, false)
BENCH_REL(shmtx_rd_pri, 1thread_all_write, 1, 1.0, false)
BENCH_REL(std_shared, 1thread_all_write, 1, 1.0, false)
BENCH_REL(pthrd_rwlock, 1thread_all_write, 1, 1.0, false)
BENCH_REL(pthrd_mutex_, 1thread_all_write, 1, 1.0, false)
BENCH_REL(timed_wr_pri, 1thread_all_write, 1, 1.0, false)
BENCH_REL(timed_rd_pri, 1thread_all_write, 1, 1.0, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 2thread_all_write, 2, 1.0, false)
BENCH_REL(shmtx_wr_pri, 2thread_all_write, 2, 1.0, false)
BENCH_REL(shmtx_rd_pri, 2thread_all_write, 2, 1.0, false)
BENCH_REL(std_shared, 2thread_all_write, 2, 1.0, false)
BENCH_REL(pthrd_rwlock, 2thread_all_write, 2, 1.0, false)
BENCH_REL(pthrd_mutex_, 2thread_all_write, 2, 1.0, false)
BENCH_REL(timed_wr_pri, 2thread_all_write, 2, 1.0, false)
BENCH_REL(timed_rd_pri, 2thread_all_write, 2, 1.0, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 4thread_all_write, 4, 1.0, false)
BENCH_REL(shmtx_wr_pri, 4thread_all_write, 4, 1.0, false)
BENCH_REL(shmtx_rd_pri, 4thread_all_write, 4, 1.0, false)
BENCH_REL(std_shared, 4thread_all_write, 4, 1.0, false)
BENCH_REL(pthrd_rwlock, 4thread_all_write, 4, 1.0, false)
BENCH_REL(pthrd_mutex_, 4thread_all_write, 4, 1.0, false)
BENCH_REL(timed_wr_pri, 4thread_all_write, 4, 1.0, false)
BENCH_REL(timed_rd_pri, 4thread_all_write, 4, 1.0, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 8thread_all_write, 8, 1.0, false)
BENCH_REL(shmtx_wr_pri, 8thread_all_write, 8, 1.0, false)
BENCH_REL(shmtx_rd_pri, 8thread_all_write, 8, 1.0, false)
BENCH_REL(std_shared, 8thread_all_write, 8, 1.0, false)
BENCH_REL(pthrd_rwlock, 8thread_all_write, 8, 1.0, false)
BENCH_REL(pthrd_mutex_, 8thread_all_write, 8, 1.0, false)
BENCH_REL(timed_wr_pri, 8thread_all_write, 8, 1.0, false)
BENCH_REL(timed_rd_pri, 8thread_all_write, 8, 1.0, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 16thread_all_write, 16, 1.0, false)
BENCH_REL(shmtx_wr_pri, 16thread_all_write, 16, 1.0, false)
BENCH_REL(shmtx_rd_pri, 16thread_all_write, 16, 1.0, false)
BENCH_REL(std_shared, 16thread_all_write, 16, 1.0, false)
BENCH_REL(pthrd_rwlock, 16thread_all_write, 16, 1.0, false)
BENCH_REL(pthrd_mutex_, 16thread_all_write, 16, 1.0, false)
BENCH_REL(timed_wr_pri, 16thread_all_write, 16, 1.0, false)
BENCH_REL(timed_rd_pri, 16thread_all_write, 16, 1.0, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 32thread_all_write, 32, 1.0, false)
BENCH_REL(shmtx_wr_pri, 32thread_all_write, 32, 1.0, false)
BENCH_REL(shmtx_rd_pri, 32thread_all_write, 32, 1.0, false)
BENCH_REL(std_shared, 32thread_all_write, 32, 1.0, false)
BENCH_REL(pthrd_rwlock, 32thread_all_write, 32, 1.0, false)
BENCH_REL(pthrd_mutex_, 32thread_all_write, 32, 1.0, false)
BENCH_REL(timed_wr_pri, 32thread_all_write, 32, 1.0, false)
BENCH_REL(timed_rd_pri, 32thread_all_write, 32, 1.0, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 64thread_all_write, 64, 1.0, false)
BENCH_REL(shmtx_wr_pri, 64thread_all_write, 64, 1.0, false)
BENCH_REL(shmtx_rd_pri, 64thread_all_write, 64, 1.0, false)
BENCH_REL(std_shared, 64thread_all_write, 64, 1.0, false)
BENCH_REL(pthrd_rwlock, 64thread_all_write, 64, 1.0, false)
BENCH_REL(pthrd_mutex_, 64thread_all_write, 64, 1.0, false)
BENCH_REL(timed_wr_pri, 64thread_all_write, 64, 1.0, false)
BENCH_REL(timed_rd_pri, 64thread_all_write, 64, 1.0, false)

// 1 lock used by everybody, 10% writes.  Not much scaling to be had.  Perf
// is best at 1 thread, once you've got multiple threads > 8 threads hurts.
BENCHMARK_DRAW_LINE();
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 1thread_10pct_write, 1, 0.10, false)
BENCH_REL(shmtx_wr_pri, 1thread_10pct_write, 1, 0.10, false)
BENCH_REL(shmtx_rd_pri, 1thread_10pct_write, 1, 0.10, false)
BENCH_REL(std_shared, 1thread_10pct_write, 1, 0.10, false)
BENCH_REL(pthrd_rwlock, 1thread_10pct_write, 1, 0.10, false)
BENCH_REL(timed_wr_pri, 1thread_10pct_write, 1, 0.10, false)
BENCH_REL(timed_rd_pri, 1thread_10pct_write, 1, 0.10, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 2thread_10pct_write, 2, 0.10, false)
BENCH_REL(shmtx_wr_pri, 2thread_10pct_write, 2, 0.10, false)
BENCH_REL(shmtx_rd_pri, 2thread_10pct_write, 2, 0.10, false)
BENCH_REL(std_shared, 2thread_10pct_write, 2, 0.10, false)
BENCH_REL(pthrd_rwlock, 2thread_10pct_write, 2, 0.10, false)
BENCH_REL(timed_wr_pri, 2thread_10pct_write, 2, 0.10, false)
BENCH_REL(timed_rd_pri, 2thread_10pct_write, 2, 0.10, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 4thread_10pct_write, 4, 0.10, false)
BENCH_REL(shmtx_wr_pri, 4thread_10pct_write, 4, 0.10, false)
BENCH_REL(shmtx_rd_pri, 4thread_10pct_write, 4, 0.10, false)
BENCH_REL(std_shared, 4thread_10pct_write, 4, 0.10, false)
BENCH_REL(pthrd_rwlock, 4thread_10pct_write, 4, 0.10, false)
BENCH_REL(timed_wr_pri, 4thread_10pct_write, 4, 0.10, false)
BENCH_REL(timed_rd_pri, 4thread_10pct_write, 4, 0.10, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 8thread_10pct_write, 8, 0.10, false)
BENCH_REL(shmtx_wr_pri, 8thread_10pct_write, 8, 0.10, false)
BENCH_REL(shmtx_rd_pri, 8thread_10pct_write, 8, 0.10, false)
BENCH_REL(std_shared, 8thread_10pct_write, 8, 0.10, false)
BENCH_REL(pthrd_rwlock, 8thread_10pct_write, 8, 0.10, false)
BENCH_REL(timed_wr_pri, 8thread_10pct_write, 8, 0.10, false)
BENCH_REL(timed_rd_pri, 8thread_10pct_write, 8, 0.10, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 16thread_10pct_write, 16, 0.10, false)
BENCH_REL(shmtx_wr_pri, 16thread_10pct_write, 16, 0.10, false)
BENCH_REL(shmtx_rd_pri, 16thread_10pct_write, 16, 0.10, false)
BENCH_REL(std_shared, 16thread_10pct_write, 16, 0.10, false)
BENCH_REL(pthrd_rwlock, 16thread_10pct_write, 16, 0.10, false)
BENCH_REL(timed_wr_pri, 16thread_10pct_write, 16, 0.10, false)
BENCH_REL(timed_rd_pri, 16thread_10pct_write, 16, 0.10, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 32thread_10pct_write, 32, 0.10, false)
BENCH_REL(shmtx_wr_pri, 32thread_10pct_write, 32, 0.10, false)
BENCH_REL(shmtx_rd_pri, 32thread_10pct_write, 32, 0.10, false)
BENCH_REL(std_shared, 32thread_10pct_write, 32, 0.10, false)
BENCH_REL(pthrd_rwlock, 32thread_10pct_write, 32, 0.10, false)
BENCH_REL(timed_wr_pri, 32thread_10pct_write, 32, 0.10, false)
BENCH_REL(timed_rd_pri, 32thread_10pct_write, 32, 0.10, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 64thread_10pct_write, 64, 0.10, false)
BENCH_REL(shmtx_wr_pri, 64thread_10pct_write, 64, 0.10, false)
BENCH_REL(shmtx_rd_pri, 64thread_10pct_write, 64, 0.10, false)
BENCH_REL(std_shared, 64thread_10pct_write, 64, 0.10, false)
BENCH_REL(pthrd_rwlock, 64thread_10pct_write, 64, 0.10, false)
BENCH_REL(timed_wr_pri, 64thread_10pct_write, 64, 0.10, false)
BENCH_REL(timed_rd_pri, 64thread_10pct_write, 64, 0.10, false)

// 1 lock used by everybody, 1% writes.  This is a more realistic example
// than the concurrent_*_reads benchmark, but still shows SharedMutex locks
// winning over all of the others
BENCHMARK_DRAW_LINE();
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 1thread_1pct_write, 1, 0.01, false)
BENCH_REL(shmtx_wr_pri, 1thread_1pct_write, 1, 0.01, false)
BENCH_REL(shmtx_w_bare, 1thread_1pct_write, 1, 0.01, false)
BENCH_REL(shmtx_rd_pri, 1thread_1pct_write, 1, 0.01, false)
BENCH_REL(shmtx_r_bare, 1thread_1pct_write, 1, 0.01, false)
BENCH_REL(std_shared, 1thread_1pct_write, 1, 0.01, false)
BENCH_REL(pthrd_rwlock, 1thread_1pct_write, 1, 0.01, false)
BENCH_REL(timed_wr_pri, 1thread_1pct_write, 1, 0.01, false)
BENCH_REL(timed_rd_pri, 1thread_1pct_write, 1, 0.01, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 2thread_1pct_write, 2, 0.01, false)
BENCH_REL(shmtx_wr_pri, 2thread_1pct_write, 2, 0.01, false)
BENCH_REL(shmtx_w_bare, 2thread_1pct_write, 2, 0.01, false)
BENCH_REL(shmtx_rd_pri, 2thread_1pct_write, 2, 0.01, false)
BENCH_REL(shmtx_r_bare, 2thread_1pct_write, 2, 0.01, false)
BENCH_REL(std_shared, 2thread_1pct_write, 2, 0.01, false)
BENCH_REL(pthrd_rwlock, 2thread_1pct_write, 2, 0.01, false)
BENCH_REL(timed_wr_pri, 2thread_1pct_write, 2, 0.01, false)
BENCH_REL(timed_rd_pri, 2thread_1pct_write, 2, 0.01, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 4thread_1pct_write, 4, 0.01, false)
BENCH_REL(shmtx_wr_pri, 4thread_1pct_write, 4, 0.01, false)
BENCH_REL(shmtx_w_bare, 4thread_1pct_write, 4, 0.01, false)
BENCH_REL(shmtx_rd_pri, 4thread_1pct_write, 4, 0.01, false)
BENCH_REL(shmtx_r_bare, 4thread_1pct_write, 4, 0.01, false)
BENCH_REL(std_shared, 4thread_1pct_write, 4, 0.01, false)
BENCH_REL(pthrd_rwlock, 4thread_1pct_write, 4, 0.01, false)
BENCH_REL(timed_wr_pri, 4thread_1pct_write, 4, 0.01, false)
BENCH_REL(timed_rd_pri, 4thread_1pct_write, 4, 0.01, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 8thread_1pct_write, 8, 0.01, false)
BENCH_REL(shmtx_wr_pri, 8thread_1pct_write, 8, 0.01, false)
BENCH_REL(shmtx_w_bare, 8thread_1pct_write, 8, 0.01, false)
BENCH_REL(shmtx_rd_pri, 8thread_1pct_write, 8, 0.01, false)
BENCH_REL(shmtx_r_bare, 8thread_1pct_write, 8, 0.01, false)
BENCH_REL(std_shared, 8thread_1pct_write, 8, 0.01, false)
BENCH_REL(pthrd_rwlock, 8thread_1pct_write, 8, 0.01, false)
BENCH_REL(timed_wr_pri, 8thread_1pct_write, 8, 0.01, false)
BENCH_REL(timed_rd_pri, 8thread_1pct_write, 8, 0.01, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 16thread_1pct_write, 16, 0.01, false)
BENCH_REL(shmtx_wr_pri, 16thread_1pct_write, 16, 0.01, false)
BENCH_REL(shmtx_w_bare, 16thread_1pct_write, 16, 0.01, false)
BENCH_REL(shmtx_rd_pri, 16thread_1pct_write, 16, 0.01, false)
BENCH_REL(shmtx_r_bare, 16thread_1pct_write, 16, 0.01, false)
BENCH_REL(std_shared, 16thread_1pct_write, 16, 0.01, false)
BENCH_REL(pthrd_rwlock, 16thread_1pct_write, 16, 0.01, false)
BENCH_REL(timed_wr_pri, 16thread_1pct_write, 16, 0.01, false)
BENCH_REL(timed_rd_pri, 16thread_1pct_write, 16, 0.01, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 32thread_1pct_write, 32, 0.01, false)
BENCH_REL(shmtx_wr_pri, 32thread_1pct_write, 32, 0.01, false)
BENCH_REL(shmtx_w_bare, 32thread_1pct_write, 32, 0.01, false)
BENCH_REL(shmtx_rd_pri, 32thread_1pct_write, 32, 0.01, false)
BENCH_REL(shmtx_r_bare, 32thread_1pct_write, 32, 0.01, false)
BENCH_REL(std_shared, 32thread_1pct_write, 32, 0.01, false)
BENCH_REL(pthrd_rwlock, 32thread_1pct_write, 32, 0.01, false)
BENCH_REL(timed_wr_pri, 32thread_1pct_write, 32, 0.01, false)
BENCH_REL(timed_rd_pri, 32thread_1pct_write, 32, 0.01, false)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 64thread_1pct_write, 64, 0.01, false)
BENCH_REL(shmtx_wr_pri, 64thread_1pct_write, 64, 0.01, false)
BENCH_REL(shmtx_w_bare, 64thread_1pct_write, 64, 0.01, false)
BENCH_REL(shmtx_rd_pri, 64thread_1pct_write, 64, 0.01, false)
BENCH_REL(shmtx_r_bare, 64thread_1pct_write, 64, 0.01, false)
BENCH_REL(std_shared, 64thread_1pct_write, 64, 0.01, false)
BENCH_REL(pthrd_rwlock, 64thread_1pct_write, 64, 0.01, false)
BENCH_REL(timed_wr_pri, 64thread_1pct_write, 64, 0.01, false)
BENCH_REL(timed_rd_pri, 64thread_1pct_write, 64, 0.01, false)

// Worst case scenario for deferred locks. No actual sharing, likely that
// read operations will have to first set the kDeferredReadersPossibleBit,
// and likely that writers will have to scan deferredReaders[].
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 2thr_2lock_50pct_write, 2, 0.50, true)
BENCH_REL(shmtx_wr_pri, 2thr_2lock_50pct_write, 2, 0.50, true)
BENCH_REL(shmtx_rd_pri, 2thr_2lock_50pct_write, 2, 0.50, true)
BENCH_BASE(folly_rwspin, 4thr_4lock_50pct_write, 4, 0.50, true)
BENCH_REL(shmtx_wr_pri, 4thr_4lock_50pct_write, 4, 0.50, true)
BENCH_REL(shmtx_rd_pri, 4thr_4lock_50pct_write, 4, 0.50, true)
BENCH_BASE(folly_rwspin, 8thr_8lock_50pct_write, 8, 0.50, true)
BENCH_REL(shmtx_wr_pri, 8thr_8lock_50pct_write, 8, 0.50, true)
BENCH_REL(shmtx_rd_pri, 8thr_8lock_50pct_write, 8, 0.50, true)
BENCH_BASE(folly_rwspin, 16thr_16lock_50pct_write, 16, 0.50, true)
BENCH_REL(shmtx_wr_pri, 16thr_16lock_50pct_write, 16, 0.50, true)
BENCH_REL(shmtx_rd_pri, 16thr_16lock_50pct_write, 16, 0.50, true)
BENCH_BASE(folly_rwspin, 32thr_32lock_50pct_write, 32, 0.50, true)
BENCH_REL(shmtx_wr_pri, 32thr_32lock_50pct_write, 32, 0.50, true)
BENCH_REL(shmtx_rd_pri, 32thr_32lock_50pct_write, 32, 0.50, true)
BENCH_BASE(folly_rwspin, 64thr_64lock_50pct_write, 64, 0.50, true)
BENCH_REL(shmtx_wr_pri, 64thr_64lock_50pct_write, 64, 0.50, true)
BENCH_REL(shmtx_rd_pri, 64thr_64lock_50pct_write, 64, 0.50, true)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 2thr_2lock_10pct_write, 2, 0.10, true)
BENCH_REL(shmtx_wr_pri, 2thr_2lock_10pct_write, 2, 0.10, true)
BENCH_REL(shmtx_rd_pri, 2thr_2lock_10pct_write, 2, 0.10, true)
BENCH_BASE(folly_rwspin, 4thr_4lock_10pct_write, 4, 0.10, true)
BENCH_REL(shmtx_wr_pri, 4thr_4lock_10pct_write, 4, 0.10, true)
BENCH_REL(shmtx_rd_pri, 4thr_4lock_10pct_write, 4, 0.10, true)
BENCH_BASE(folly_rwspin, 8thr_8lock_10pct_write, 8, 0.10, true)
BENCH_REL(shmtx_wr_pri, 8thr_8lock_10pct_write, 8, 0.10, true)
BENCH_REL(shmtx_rd_pri, 8thr_8lock_10pct_write, 8, 0.10, true)
BENCH_BASE(folly_rwspin, 16thr_16lock_10pct_write, 16, 0.10, true)
BENCH_REL(shmtx_wr_pri, 16thr_16lock_10pct_write, 16, 0.10, true)
BENCH_REL(shmtx_rd_pri, 16thr_16lock_10pct_write, 16, 0.10, true)
BENCH_BASE(folly_rwspin, 32thr_32lock_10pct_write, 32, 0.10, true)
BENCH_REL(shmtx_wr_pri, 32thr_32lock_10pct_write, 32, 0.10, true)
BENCH_REL(shmtx_rd_pri, 32thr_32lock_10pct_write, 32, 0.10, true)
BENCH_BASE(folly_rwspin, 64thr_64lock_10pct_write, 64, 0.10, true)
BENCH_REL(shmtx_wr_pri, 64thr_64lock_10pct_write, 64, 0.10, true)
BENCH_REL(shmtx_rd_pri, 64thr_64lock_10pct_write, 64, 0.10, true)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin, 2thr_2lock_1pct_write, 2, 0.01, true)
BENCH_REL(shmtx_wr_pri, 2thr_2lock_1pct_write, 2, 0.01, true)
BENCH_REL(shmtx_rd_pri, 2thr_2lock_1pct_write, 2, 0.01, true)
BENCH_BASE(folly_rwspin, 4thr_4lock_1pct_write, 4, 0.01, true)
BENCH_REL(shmtx_wr_pri, 4thr_4lock_1pct_write, 4, 0.01, true)
BENCH_REL(shmtx_rd_pri, 4thr_4lock_1pct_write, 4, 0.01, true)
BENCH_BASE(folly_rwspin, 8thr_8lock_1pct_write, 8, 0.01, true)
BENCH_REL(shmtx_wr_pri, 8thr_8lock_1pct_write, 8, 0.01, true)
BENCH_REL(shmtx_rd_pri, 8thr_8lock_1pct_write, 8, 0.01, true)
BENCH_BASE(folly_rwspin, 16thr_16lock_1pct_write, 16, 0.01, true)
BENCH_REL(shmtx_wr_pri, 16thr_16lock_1pct_write, 16, 0.01, true)
BENCH_REL(shmtx_rd_pri, 16thr_16lock_1pct_write, 16, 0.01, true)
BENCH_BASE(folly_rwspin, 32thr_32lock_1pct_write, 32, 0.01, true)
BENCH_REL(shmtx_wr_pri, 32thr_32lock_1pct_write, 32, 0.01, true)
BENCH_REL(shmtx_rd_pri, 32thr_32lock_1pct_write, 32, 0.01, true)
BENCH_BASE(folly_rwspin, 64thr_64lock_1pct_write, 64, 0.01, true)
BENCH_REL(shmtx_wr_pri, 64thr_64lock_1pct_write, 64, 0.01, true)
BENCH_REL(shmtx_rd_pri, 64thr_64lock_1pct_write, 64, 0.01, true)

// Ping-pong tests have a scaled number of iterations, because their burn
// loop would make them too slow otherwise.  Ping-pong with burn count of
// 100k or 300k shows the advantage of soft-spin, reducing the cost of
// each wakeup by about 20 usec.  (Take benchmark reported difference,
// ~400 nanos, multiply by the scale of 100, then divide by 2 because
// each round has two wakeups.)
BENCHMARK_DRAW_LINE();
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin_ping_pong, burn0, 1, 0)
BENCH_REL(shmtx_w_bare_ping_pong, burn0, 1, 0)
BENCH_REL(shmtx_r_bare_ping_pong, burn0, 1, 0)
BENCH_REL(std_shared_ping_pong, burn0, 1, 0)
BENCH_REL(pthrd_rwlock_ping_pong, burn0, 1, 0)
BENCH_REL(timed_wr_pri_ping_pong, burn0, 1, 0)
BENCH_REL(timed_rd_pri_ping_pong, burn0, 1, 0)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin_ping_pong, burn100k, 100, 100000)
BENCH_REL(shmtx_w_bare_ping_pong, burn100k, 100, 100000)
BENCH_REL(shmtx_r_bare_ping_pong, burn100k, 100, 100000)
BENCH_REL(std_shared_ping_pong, burn100k, 100, 100000)
BENCH_REL(pthrd_rwlock_ping_pong, burn100k, 100, 100000)
BENCH_REL(timed_wr_pri_ping_pong, burn100k, 100, 100000)
BENCH_REL(timed_rd_pri_ping_pong, burn100k, 100, 100000)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin_ping_pong, burn300k, 100, 300000)
BENCH_REL(shmtx_w_bare_ping_pong, burn300k, 100, 300000)
BENCH_REL(shmtx_r_bare_ping_pong, burn300k, 100, 300000)
BENCH_REL(std_shared_ping_pong, burn300k, 100, 300000)
BENCH_REL(pthrd_rwlock_ping_pong, burn300k, 100, 300000)
BENCH_REL(timed_wr_pri_ping_pong, burn300k, 100, 300000)
BENCH_REL(timed_rd_pri_ping_pong, burn300k, 100, 300000)
BENCHMARK_DRAW_LINE();
BENCH_BASE(folly_rwspin_ping_pong, burn1M, 1000, 1000000)
BENCH_REL(shmtx_w_bare_ping_pong, burn1M, 1000, 1000000)
BENCH_REL(shmtx_r_bare_ping_pong, burn1M, 1000, 1000000)
BENCH_REL(std_shared_ping_pong, burn1M, 1000, 1000000)
BENCH_REL(pthrd_rwlock_ping_pong, burn1M, 1000, 1000000)
BENCH_REL(timed_wr_pri_ping_pong, burn1M, 1000, 1000000)
BENCH_REL(timed_rd_pri_ping_pong, burn1M, 1000, 1000000)

// Reproduce with 10 minutes and
//   sudo nice -n -20
//     shared_mutex_test --benchmark --bm_min_iters=1000000
//
// Comparison use folly::RWSpinLock as the baseline, with the
// following row being the default SharedMutex (using *Holder or
// Token-ful methods).
//
// Following results on 2-socket Intel(R) Xeon(R) CPU E5-2660 0 @ 2.20GHz
//
// ============================================================================
// folly/test/SharedMutexTest.cpp                  relative  time/iter  iters/s
// ============================================================================
// single_thread_lock_shared_unlock_shared                     25.17ns   39.74M
// single_thread_lock_unlock                                   25.88ns   38.64M
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// folly_rwspin_reads(1thread)                                 15.16ns   65.95M
// shmtx_wr_pri_reads(1thread)                       69.18%    21.92ns   45.63M
// shmtx_w_bare_reads(1thread)                       56.07%    27.04ns   36.98M
// shmtx_rd_pri_reads(1thread)                       69.06%    21.95ns   45.55M
// shmtx_r_bare_reads(1thread)                       56.36%    26.90ns   37.17M
// pthrd_rwlock_reads(1thread)                       39.61%    38.28ns   26.12M
// ----------------------------------------------------------------------------
// folly_rwspin_reads(2thread)                                 45.05ns   22.20M
// shmtx_wr_pri_reads(2thread)                      379.98%    11.86ns   84.34M
// shmtx_w_bare_reads(2thread)                      319.27%    14.11ns   70.87M
// shmtx_rd_pri_reads(2thread)                      385.59%    11.68ns   85.59M
// shmtx_r_bare_reads(2thread)                      306.56%    14.70ns   68.04M
// pthrd_rwlock_reads(2thread)                       34.22%   131.65ns    7.60M
// ----------------------------------------------------------------------------
// folly_rwspin_reads(4thread)                                 62.19ns   16.08M
// shmtx_wr_pri_reads(4thread)                     1022.82%     6.08ns  164.48M
// shmtx_w_bare_reads(4thread)                      875.37%     7.10ns  140.76M
// shmtx_rd_pri_reads(4thread)                     1060.46%     5.86ns  170.53M
// shmtx_r_bare_reads(4thread)                      879.88%     7.07ns  141.49M
// pthrd_rwlock_reads(4thread)                       25.01%   248.65ns    4.02M
// ----------------------------------------------------------------------------
// folly_rwspin_reads(8thread)                                 64.09ns   15.60M
// shmtx_wr_pri_reads(8thread)                     2191.99%     2.92ns  342.03M
// shmtx_w_bare_reads(8thread)                     1804.92%     3.55ns  281.63M
// shmtx_rd_pri_reads(8thread)                     2194.60%     2.92ns  342.44M
// shmtx_r_bare_reads(8thread)                     1800.53%     3.56ns  280.95M
// pthrd_rwlock_reads(8thread)                       28.19%   227.31ns    4.40M
// ----------------------------------------------------------------------------
// folly_rwspin_reads(16thread)                                70.06ns   14.27M
// shmtx_wr_pri_reads(16thread)                    4970.09%     1.41ns  709.38M
// shmtx_w_bare_reads(16thread)                    4143.75%     1.69ns  591.44M
// shmtx_rd_pri_reads(16thread)                    5009.31%     1.40ns  714.98M
// shmtx_r_bare_reads(16thread)                    4067.36%     1.72ns  580.54M
// pthrd_rwlock_reads(16thread)                      35.05%   199.90ns    5.00M
// ----------------------------------------------------------------------------
// folly_rwspin_reads(32thread)                                58.83ns   17.00M
// shmtx_wr_pri_reads(32thread)                    5158.37%     1.14ns  876.79M
// shmtx_w_bare_reads(32thread)                    4246.03%     1.39ns  721.72M
// shmtx_rd_pri_reads(32thread)                    4845.97%     1.21ns  823.69M
// shmtx_r_bare_reads(32thread)                    4721.44%     1.25ns  802.52M
// pthrd_rwlock_reads(32thread)                      30.01%   196.02ns    5.10M
// ----------------------------------------------------------------------------
// folly_rwspin_reads(64thread)                                59.19ns   16.89M
// shmtx_wr_pri_reads(64thread)                    3804.54%     1.56ns  642.76M
// shmtx_w_bare_reads(64thread)                    3625.06%     1.63ns  612.43M
// shmtx_rd_pri_reads(64thread)                    3418.19%     1.73ns  577.48M
// shmtx_r_bare_reads(64thread)                    3416.98%     1.73ns  577.28M
// pthrd_rwlock_reads(64thread)                      31.35%   188.81ns    5.30M
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// folly_rwspin(1thread_all_write)                             23.77ns   42.06M
// shmtx_wr_pri(1thread_all_write)                   85.09%    27.94ns   35.79M
// shmtx_rd_pri(1thread_all_write)                   85.32%    27.87ns   35.89M
// pthrd_rwlock(1thread_all_write)                   53.99%    44.04ns   22.71M
// pthrd_mutex_(1thread_all_write)                   86.05%    27.63ns   36.20M
// ----------------------------------------------------------------------------
// folly_rwspin(2thread_all_write)                             76.05ns   13.15M
// shmtx_wr_pri(2thread_all_write)                   60.67%   125.35ns    7.98M
// shmtx_rd_pri(2thread_all_write)                   60.36%   125.99ns    7.94M
// pthrd_rwlock(2thread_all_write)                   40.90%   185.92ns    5.38M
// pthrd_mutex_(2thread_all_write)                  127.37%    59.71ns   16.75M
// ----------------------------------------------------------------------------
// folly_rwspin(4thread_all_write)                            207.17ns    4.83M
// shmtx_wr_pri(4thread_all_write)                  119.42%   173.49ns    5.76M
// shmtx_rd_pri(4thread_all_write)                  117.68%   176.05ns    5.68M
// pthrd_rwlock(4thread_all_write)                   27.50%   753.25ns    1.33M
// pthrd_mutex_(4thread_all_write)                  117.75%   175.95ns    5.68M
// ----------------------------------------------------------------------------
// folly_rwspin(8thread_all_write)                            326.50ns    3.06M
// shmtx_wr_pri(8thread_all_write)                  125.47%   260.22ns    3.84M
// shmtx_rd_pri(8thread_all_write)                  124.73%   261.76ns    3.82M
// pthrd_rwlock(8thread_all_write)                   38.54%   847.09ns    1.18M
// pthrd_mutex_(8thread_all_write)                  166.31%   196.32ns    5.09M
// ----------------------------------------------------------------------------
// folly_rwspin(16thread_all_write)                           729.89ns    1.37M
// shmtx_wr_pri(16thread_all_write)                 219.91%   331.91ns    3.01M
// shmtx_rd_pri(16thread_all_write)                 220.09%   331.62ns    3.02M
// pthrd_rwlock(16thread_all_write)                 113.90%   640.84ns    1.56M
// pthrd_mutex_(16thread_all_write)                 401.97%   181.58ns    5.51M
// ----------------------------------------------------------------------------
// folly_rwspin(32thread_all_write)                             1.55us  645.01K
// shmtx_wr_pri(32thread_all_write)                 415.05%   373.54ns    2.68M
// shmtx_rd_pri(32thread_all_write)                 258.45%   599.88ns    1.67M
// pthrd_rwlock(32thread_all_write)                 254.16%   610.00ns    1.64M
// pthrd_mutex_(32thread_all_write)                 852.51%   181.86ns    5.50M
// ----------------------------------------------------------------------------
// folly_rwspin(64thread_all_write)                             2.03us  492.00K
// shmtx_wr_pri(64thread_all_write)                 517.65%   392.64ns    2.55M
// shmtx_rd_pri(64thread_all_write)                 288.20%   705.24ns    1.42M
// pthrd_rwlock(64thread_all_write)                 326.75%   622.04ns    1.61M
// pthrd_mutex_(64thread_all_write)                1231.57%   165.04ns    6.06M
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// folly_rwspin(1thread_10pct_write)                           19.39ns   51.58M
// shmtx_wr_pri(1thread_10pct_write)                 93.87%    20.65ns   48.42M
// shmtx_rd_pri(1thread_10pct_write)                 93.60%    20.71ns   48.28M
// pthrd_rwlock(1thread_10pct_write)                 44.15%    43.92ns   22.77M
// ----------------------------------------------------------------------------
// folly_rwspin(2thread_10pct_write)                          227.88ns    4.39M
// shmtx_wr_pri(2thread_10pct_write)                321.08%    70.98ns   14.09M
// shmtx_rd_pri(2thread_10pct_write)                280.65%    81.20ns   12.32M
// pthrd_rwlock(2thread_10pct_write)                112.68%   202.23ns    4.94M
// ----------------------------------------------------------------------------
// folly_rwspin(4thread_10pct_write)                          444.94ns    2.25M
// shmtx_wr_pri(4thread_10pct_write)                470.35%    94.60ns   10.57M
// shmtx_rd_pri(4thread_10pct_write)                349.08%   127.46ns    7.85M
// pthrd_rwlock(4thread_10pct_write)                100.59%   442.31ns    2.26M
// ----------------------------------------------------------------------------
// folly_rwspin(8thread_10pct_write)                          424.67ns    2.35M
// shmtx_wr_pri(8thread_10pct_write)                337.53%   125.82ns    7.95M
// shmtx_rd_pri(8thread_10pct_write)                232.32%   182.79ns    5.47M
// pthrd_rwlock(8thread_10pct_write)                 78.58%   540.42ns    1.85M
// ----------------------------------------------------------------------------
// folly_rwspin(16thread_10pct_write)                         727.04ns    1.38M
// shmtx_wr_pri(16thread_10pct_write)               400.60%   181.49ns    5.51M
// shmtx_rd_pri(16thread_10pct_write)               312.94%   232.33ns    4.30M
// pthrd_rwlock(16thread_10pct_write)               131.28%   553.82ns    1.81M
// ----------------------------------------------------------------------------
// folly_rwspin(32thread_10pct_write)                         810.61ns    1.23M
// shmtx_wr_pri(32thread_10pct_write)               429.61%   188.68ns    5.30M
// shmtx_rd_pri(32thread_10pct_write)               321.13%   252.42ns    3.96M
// pthrd_rwlock(32thread_10pct_write)               144.28%   561.85ns    1.78M
// ----------------------------------------------------------------------------
// folly_rwspin(64thread_10pct_write)                           1.10us  912.30K
// shmtx_wr_pri(64thread_10pct_write)               486.68%   225.22ns    4.44M
// shmtx_rd_pri(64thread_10pct_write)               412.96%   265.43ns    3.77M
// pthrd_rwlock(64thread_10pct_write)               198.81%   551.34ns    1.81M
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// folly_rwspin(1thread_1pct_write)                            19.02ns   52.57M
// shmtx_wr_pri(1thread_1pct_write)                  94.46%    20.14ns   49.66M
// shmtx_w_bare(1thread_1pct_write)                  76.60%    24.83ns   40.27M
// shmtx_rd_pri(1thread_1pct_write)                  93.83%    20.27ns   49.33M
// shmtx_r_bare(1thread_1pct_write)                  77.04%    24.69ns   40.50M
// pthrd_rwlock(1thread_1pct_write)                  42.85%    44.39ns   22.53M
// ----------------------------------------------------------------------------
// folly_rwspin(2thread_1pct_write)                           110.63ns    9.04M
// shmtx_wr_pri(2thread_1pct_write)                 442.12%    25.02ns   39.96M
// shmtx_w_bare(2thread_1pct_write)                 374.65%    29.53ns   33.86M
// shmtx_rd_pri(2thread_1pct_write)                 371.08%    29.81ns   33.54M
// shmtx_r_bare(2thread_1pct_write)                 138.02%    80.15ns   12.48M
// pthrd_rwlock(2thread_1pct_write)                  95.48%   115.87ns    8.63M
// ----------------------------------------------------------------------------
// folly_rwspin(4thread_1pct_write)                           140.62ns    7.11M
// shmtx_wr_pri(4thread_1pct_write)                 627.13%    22.42ns   44.60M
// shmtx_w_bare(4thread_1pct_write)                 552.94%    25.43ns   39.32M
// shmtx_rd_pri(4thread_1pct_write)                 226.06%    62.21ns   16.08M
// shmtx_r_bare(4thread_1pct_write)                  77.61%   181.19ns    5.52M
// pthrd_rwlock(4thread_1pct_write)                  45.55%   308.72ns    3.24M
// ----------------------------------------------------------------------------
// folly_rwspin(8thread_1pct_write)                           166.23ns    6.02M
// shmtx_wr_pri(8thread_1pct_write)                 687.09%    24.19ns   41.33M
// shmtx_w_bare(8thread_1pct_write)                 611.80%    27.17ns   36.80M
// shmtx_rd_pri(8thread_1pct_write)                 140.37%   118.43ns    8.44M
// shmtx_r_bare(8thread_1pct_write)                  80.32%   206.97ns    4.83M
// pthrd_rwlock(8thread_1pct_write)                  49.84%   333.55ns    3.00M
// ----------------------------------------------------------------------------
// folly_rwspin(16thread_1pct_write)                          419.79ns    2.38M
// shmtx_wr_pri(16thread_1pct_write)               1397.92%    30.03ns   33.30M
// shmtx_w_bare(16thread_1pct_write)               1324.60%    31.69ns   31.55M
// shmtx_rd_pri(16thread_1pct_write)                278.12%   150.94ns    6.63M
// shmtx_r_bare(16thread_1pct_write)                194.25%   216.11ns    4.63M
// pthrd_rwlock(16thread_1pct_write)                131.96%   318.12ns    3.14M
// ----------------------------------------------------------------------------
// folly_rwspin(32thread_1pct_write)                          395.99ns    2.53M
// shmtx_wr_pri(32thread_1pct_write)               1332.76%    29.71ns   33.66M
// shmtx_w_bare(32thread_1pct_write)               1208.86%    32.76ns   30.53M
// shmtx_rd_pri(32thread_1pct_write)                252.97%   156.54ns    6.39M
// shmtx_r_bare(32thread_1pct_write)                193.79%   204.35ns    4.89M
// pthrd_rwlock(32thread_1pct_write)                129.88%   304.89ns    3.28M
// ----------------------------------------------------------------------------
// folly_rwspin(64thread_1pct_write)                          424.07ns    2.36M
// shmtx_wr_pri(64thread_1pct_write)               1297.89%    32.67ns   30.61M
// shmtx_w_bare(64thread_1pct_write)               1228.88%    34.51ns   28.98M
// shmtx_rd_pri(64thread_1pct_write)                270.40%   156.83ns    6.38M
// shmtx_r_bare(64thread_1pct_write)                218.05%   194.48ns    5.14M
// pthrd_rwlock(64thread_1pct_write)                143.80%   294.91ns    3.39M
// ----------------------------------------------------------------------------
// folly_rwspin(2thr_2lock_50pct_write)                        10.87ns   91.99M
// shmtx_wr_pri(2thr_2lock_50pct_write)              83.71%    12.99ns   77.01M
// shmtx_rd_pri(2thr_2lock_50pct_write)              84.08%    12.93ns   77.34M
// folly_rwspin(4thr_4lock_50pct_write)                         5.32ns  188.12M
// shmtx_wr_pri(4thr_4lock_50pct_write)              82.21%     6.47ns  154.65M
// shmtx_rd_pri(4thr_4lock_50pct_write)              81.20%     6.55ns  152.75M
// folly_rwspin(8thr_8lock_50pct_write)                         2.64ns  379.06M
// shmtx_wr_pri(8thr_8lock_50pct_write)              81.26%     3.25ns  308.03M
// shmtx_rd_pri(8thr_8lock_50pct_write)              80.95%     3.26ns  306.86M
// folly_rwspin(16thr_16lock_50pct_write)                       1.52ns  656.77M
// shmtx_wr_pri(16thr_16lock_50pct_write)            86.24%     1.77ns  566.41M
// shmtx_rd_pri(16thr_16lock_50pct_write)            83.72%     1.82ns  549.82M
// folly_rwspin(32thr_32lock_50pct_write)                       1.19ns  841.03M
// shmtx_wr_pri(32thr_32lock_50pct_write)            85.08%     1.40ns  715.55M
// shmtx_rd_pri(32thr_32lock_50pct_write)            86.44%     1.38ns  727.00M
// folly_rwspin(64thr_64lock_50pct_write)                       1.46ns  684.28M
// shmtx_wr_pri(64thr_64lock_50pct_write)            84.53%     1.73ns  578.43M
// shmtx_rd_pri(64thr_64lock_50pct_write)            82.80%     1.76ns  566.58M
// ----------------------------------------------------------------------------
// folly_rwspin(2thr_2lock_10pct_write)                        10.01ns   99.85M
// shmtx_wr_pri(2thr_2lock_10pct_write)              92.02%    10.88ns   91.88M
// shmtx_rd_pri(2thr_2lock_10pct_write)              92.35%    10.84ns   92.22M
// folly_rwspin(4thr_4lock_10pct_write)                         4.81ns  207.87M
// shmtx_wr_pri(4thr_4lock_10pct_write)              89.32%     5.39ns  185.67M
// shmtx_rd_pri(4thr_4lock_10pct_write)              88.96%     5.41ns  184.93M
// folly_rwspin(8thr_8lock_10pct_write)                         2.39ns  417.62M
// shmtx_wr_pri(8thr_8lock_10pct_write)              91.17%     2.63ns  380.76M
// shmtx_rd_pri(8thr_8lock_10pct_write)              89.53%     2.67ns  373.92M
// folly_rwspin(16thr_16lock_10pct_write)                       1.16ns  860.47M
// shmtx_wr_pri(16thr_16lock_10pct_write)            74.35%     1.56ns  639.77M
// shmtx_rd_pri(16thr_16lock_10pct_write)            91.34%     1.27ns  785.97M
// folly_rwspin(32thr_32lock_10pct_write)                       1.15ns  866.23M
// shmtx_wr_pri(32thr_32lock_10pct_write)            92.32%     1.25ns  799.72M
// shmtx_rd_pri(32thr_32lock_10pct_write)            94.40%     1.22ns  817.71M
// folly_rwspin(64thr_64lock_10pct_write)                       1.41ns  710.54M
// shmtx_wr_pri(64thr_64lock_10pct_write)            94.14%     1.50ns  668.88M
// shmtx_rd_pri(64thr_64lock_10pct_write)            94.80%     1.48ns  673.56M
// ----------------------------------------------------------------------------
// folly_rwspin(2thr_2lock_1pct_write)                          9.58ns  104.36M
// shmtx_wr_pri(2thr_2lock_1pct_write)               92.00%    10.42ns   96.01M
// shmtx_rd_pri(2thr_2lock_1pct_write)               91.79%    10.44ns   95.79M
// folly_rwspin(4thr_4lock_1pct_write)                          4.71ns  212.30M
// shmtx_wr_pri(4thr_4lock_1pct_write)               90.37%     5.21ns  191.85M
// shmtx_rd_pri(4thr_4lock_1pct_write)               89.94%     5.24ns  190.95M
// folly_rwspin(8thr_8lock_1pct_write)                          2.33ns  429.91M
// shmtx_wr_pri(8thr_8lock_1pct_write)               90.67%     2.57ns  389.80M
// shmtx_rd_pri(8thr_8lock_1pct_write)               90.61%     2.57ns  389.55M
// folly_rwspin(16thr_16lock_1pct_write)                        1.10ns  905.23M
// shmtx_wr_pri(16thr_16lock_1pct_write)             91.96%     1.20ns  832.46M
// shmtx_rd_pri(16thr_16lock_1pct_write)             92.29%     1.20ns  835.42M
// folly_rwspin(32thr_32lock_1pct_write)                        1.14ns  879.85M
// shmtx_wr_pri(32thr_32lock_1pct_write)             93.41%     1.22ns  821.86M
// shmtx_rd_pri(32thr_32lock_1pct_write)             94.18%     1.21ns  828.66M
// folly_rwspin(64thr_64lock_1pct_write)                        1.34ns  748.83M
// shmtx_wr_pri(64thr_64lock_1pct_write)             94.39%     1.41ns  706.84M
// shmtx_rd_pri(64thr_64lock_1pct_write)             94.02%     1.42ns  704.06M
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// folly_rwspin_ping_pong(burn0)                              605.63ns    1.65M
// shmtx_w_bare_ping_pong(burn0)                    102.17%   592.76ns    1.69M
// shmtx_r_bare_ping_pong(burn0)                     88.75%   682.44ns    1.47M
// pthrd_rwlock_ping_pong(burn0)                      7.88%     7.68us  130.15K
// ----------------------------------------------------------------------------
// folly_rwspin_ping_pong(burn100k)                           727.76ns    1.37M
// shmtx_w_bare_ping_pong(burn100k)                 100.79%   722.09ns    1.38M
// shmtx_r_bare_ping_pong(burn100k)                 101.98%   713.61ns    1.40M
// pthrd_rwlock_ping_pong(burn100k)                  71.05%     1.02us  976.30K
// ----------------------------------------------------------------------------
// folly_rwspin_ping_pong(burn300k)                             2.11us  473.46K
// shmtx_w_bare_ping_pong(burn300k)                 100.06%     2.11us  473.72K
// shmtx_r_bare_ping_pong(burn300k)                  98.93%     2.13us  468.39K
// pthrd_rwlock_ping_pong(burn300k)                  84.62%     2.50us  400.66K
// ----------------------------------------------------------------------------
// folly_rwspin_ping_pong(burn1M)                             709.70ns    1.41M
// shmtx_w_bare_ping_pong(burn1M)                   100.28%   707.73ns    1.41M
// shmtx_r_bare_ping_pong(burn1M)                    99.63%   712.37ns    1.40M
// pthrd_rwlock_ping_pong(burn1M)                    96.32%   736.82ns    1.36M
// ============================================================================

int main(int argc, char** argv) {
  (void)folly_rwspin_reads;
  (void)shmtx_wr_pri_reads;
  (void)shmtx_w_bare_reads;
  (void)shmtx_rd_pri_reads;
  (void)shmtx_r_bare_reads;
  (void)std_shared_reads;
  (void)pthrd_rwlock_reads;
  (void)timed_wr_pri_reads;
  (void)timed_rd_pri_reads;
  (void)folly_rwspin;
  (void)shmtx_wr_pri;
  (void)shmtx_w_bare;
  (void)shmtx_rd_pri;
  (void)shmtx_r_bare;
  (void)std_shared;
  (void)pthrd_rwlock;
  (void)pthrd_mutex_;
  (void)timed_wr_pri;
  (void)timed_rd_pri;
  (void)folly_rwspin_ping_pong;
  (void)shmtx_w_bare_ping_pong;
  (void)shmtx_r_bare_ping_pong;
  (void)std_shared_ping_pong;
  (void)pthrd_rwlock_ping_pong;
  (void)timed_wr_pri_ping_pong;
  (void)timed_rd_pri_ping_pong;

  testing::InitGoogleTest(&argc, argv);
  folly::gflags::ParseCommandLineFlags(&argc, &argv, true);
  int rv = RUN_ALL_TESTS();
  folly::runBenchmarksOnFlag();
  return rv;
}
