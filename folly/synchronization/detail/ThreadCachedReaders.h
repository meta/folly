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
#include <cstdint>
#include <limits>
#include <utility>
#include <folly/Function.h>
#include <folly/ThreadLocal.h>
#include <folly/Utility.h>
#include <folly/synchronization/AsymmetricThreadFence.h>
#include <folly/synchronization/AtomicTraits.h>
#include <folly/synchronization/detail/ThreadCachedTag.h>

namespace folly {

namespace detail {

// Prefer asymmetric fences over acquire/release operations, mirroring
// hazptr_prefer_fence_light: everywhere except TSAN (which ignores fences
// but tracks operations), TSO hardware, and where asymmetric fences fall
// back to symmetric fences (off Linux), relaxed operations are paired
// with asymmetric fences, cheap on readers with the heavy fence on the
// reclaimer.
constexpr inline bool thread_cached_readers_prefer_fence_light =
    !kIsSanitizeThread && !atomic_arch_mm_is_tso_v && kIsLinux;

// Counter access orders: relaxed where fences carry the ordering, with
// release/acquire operations where they must carry it themselves. Exit
// stores need release outside the fence strategy; observing loads need
// acquire only where TSAN watches, since everywhere else a fence follows
// the scan.
constexpr inline std::memory_order thread_cached_readers_store_order =
    thread_cached_readers_prefer_fence_light
    ? std::memory_order_relaxed
    : std::memory_order_release;
constexpr inline std::memory_order thread_cached_readers_load_order =
    kIsSanitizeThread ? std::memory_order_acquire : std::memory_order_relaxed;

// A data structure that keeps a per-thread cache of a bitfield that contains
// the current active epoch for readers in the thread, and the number of active
// readers:
//
//                _______________________________________
//                |   Current Epoch    |    # Readers   |
// epoch_readers: | 63 62 ... 34 33 32 | 31 30 ... 2 1 0|
//                o--------------------|----------------o
//
// There are several important implications with this data structure:
//
// 1. Read regions must be entered and exited on the same thread.
// 2. Read regions are fully nested. That is, two read regions in a single
//    thread may not overlap across two epochs.
//
// These implications afford us debugging opportunities, such
// as being able to detect long-running readers (T113951078).
class ThreadCachedReaders {
  using atomic_type = std::atomic<uint64_t>;
  atomic_type orphan_epoch_readers_{0};
  folly::detail::Futex<> waiting_{0};

  class EpochCount {
   public:
    ThreadCachedReaders* readers_;
    explicit constexpr EpochCount(ThreadCachedReaders* readers) noexcept
        : readers_(readers), epoch_readers_{} {}
    atomic_type epoch_readers_;
    ~EpochCount() noexcept {
      // Set the cached epoch and readers.
      uint64_t epoch_readers =
          epoch_readers_.load(std::memory_order_relaxed); // own thread
      readers_->orphan_epoch_readers_.store(
          epoch_readers, thread_cached_readers_store_order);
      folly::asymmetric_thread_fence_light(std::memory_order_seq_cst); // B
      detail::futexWake(&readers_->waiting_);
    }
  };
  folly::ThreadLocalPtr<EpochCount, ThreadCachedTag> cs_;

  // may abort if internal allocations fail
  void init() {
    auto ret = new EpochCount(this);
    cs_.reset(ret);
  }

  static uint32_t readers_from_epoch_reader(uint64_t epoch_reader) {
    return static_cast<uint32_t>(epoch_reader);
  }

  static uint32_t epoch_from_epoch_reader(uint64_t epoch_reader) {
    return static_cast<uint32_t>(epoch_reader >> 32);
  }

  static uint64_t create_epoch_reader(uint64_t epoch, uint32_t readers) {
    return (epoch << 32) + readers;
  }

 public:
  FOLLY_ALWAYS_INLINE void increment(uint64_t epoch) {
    auto tls_cache = cs_.get();
    if (tls_cache == nullptr) {
      init();
      tls_cache = cs_.get();
    }
    uint64_t epoch_reader =
        tls_cache->epoch_readers_.load(std::memory_order_relaxed); // own thread
    if (readers_from_epoch_reader(epoch_reader) != 0) {
      DCHECK(
          readers_from_epoch_reader(epoch_reader) <
          std::numeric_limits<uint32_t>::max());
      tls_cache->epoch_readers_.store(
          epoch_reader + 1, thread_cached_readers_store_order);
    } else {
      tls_cache->epoch_readers_.store(
          create_epoch_reader(epoch, 1), thread_cached_readers_store_order);
    }
    folly::asymmetric_thread_fence_light(std::memory_order_seq_cst); // A
  }

  FOLLY_ALWAYS_INLINE void decrement() {
    folly::asymmetric_thread_fence_light(std::memory_order_seq_cst); // B

    auto tls_cache = cs_.get();
    DCHECK(tls_cache != nullptr);
    uint64_t epoch_reader =
        tls_cache->epoch_readers_.load(std::memory_order_relaxed); // own thread
    DCHECK(readers_from_epoch_reader(epoch_reader) > 0);
    tls_cache->epoch_readers_.store(
        epoch_reader - 1, thread_cached_readers_store_order);

    folly::asymmetric_thread_fence_light(std::memory_order_seq_cst); // C
    if (waiting_.load(std::memory_order_acquire)) {
      waiting_.store(0, std::memory_order_release);
      detail::futexWake(&waiting_);
    }
  }

  static bool epochHasReaders(uint8_t epoch, uint64_t epoch_readers) {
    bool has_readers = readers_from_epoch_reader(epoch_readers) != 0;
    bool same_epoch = (epoch_from_epoch_reader(epoch_readers) & 0x1) == epoch;
    return has_readers && same_epoch;
  }

  bool epochIsClear(uint8_t epoch) {
    uint64_t orphaned =
        orphan_epoch_readers_.load(thread_cached_readers_load_order);
    if (epochHasReaders(epoch, orphaned)) {
      return false;
    }

    // Matches A and B - ensure all threads have seen new value of version,
    // *and* that we see current values of readers for every thread below.
    //
    // Note that in lock_shared if a reader is currently between the
    // version load and counter increment, they may update the wrong
    // epoch.  However, this is ok - they started concurrently *after*
    // any callbacks that will run, and therefore it is safe to run
    // the callbacks.
    folly::asymmetric_thread_fence_heavy(std::memory_order_seq_cst);
    auto access = cs_.accessAllThreads();
    bool const clear = !std::any_of(access.begin(), access.end(), [&](auto& i) {
      uint64_t const epoch_readers =
          i.epoch_readers_.load(thread_cached_readers_load_order);
      return epochHasReaders(epoch, epoch_readers);
    });
    copy(std::move(access)); // drop
    if (clear) {
      // Pairs the release exit stores observed above with teardown. The
      // scan loads are relaxed everywhere except TSAN, amortizing the
      // acquire into one trailing fence: heavy where fences carry the
      // ordering, plain acquire fence otherwise (free on TSO, and under
      // TSAN the acquire loads already order it). Orders only this
      // thread's accesses, so it runs after the registry guard above
      // is released.
      if constexpr (thread_cached_readers_prefer_fence_light) {
        folly::asymmetric_thread_fence_heavy(std::memory_order_acquire);
      } else {
        std::atomic_thread_fence(std::memory_order_acquire);
      }
    }
    return clear;
  }

  void waitForZero(uint8_t epoch) {
    // Try reading before futex sleeping.
    if (epochIsClear(epoch)) {
      return;
    }

    while (true) {
      // Matches C.  Ensure either decrement sees waiting_,
      // or we see their decrement and can safely sleep.
      waiting_.store(1, std::memory_order_release);
      folly::asymmetric_thread_fence_heavy(std::memory_order_seq_cst);
      if (epochIsClear(epoch)) {
        break;
      }
      detail::futexWait(&waiting_, 1);
    }
    waiting_.store(0, std::memory_order_relaxed);
  }
};

} // namespace detail
} // namespace folly
