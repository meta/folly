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
#include <cassert>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <folly/concurrency/CacheLocality.h>
#include <folly/lang/Bits.h>
#include <folly/lang/New.h>

namespace folly {

/// ProducerConsumerQueue.
///
/// A wait-free single-producer single-consumer fixed-size queue.
///
/// Queue elements are stored in a single ring buffer. If the queue is full,
/// enqueue operations (writes) fail by returning false. If the queue is empty,
/// dequeue operations (reads) fail by returning false. The ring buffer is fixed
/// at its initial capacity and is never grown.
///
/// For performance, the queue capacity is a power of two. This allows the queue
/// to accelerate index math and avoid expensive integer-division. If the size
/// provided is not a power of two, the queue rounds it up.
///
/// For performance, all state is split into a producer-owned and a
/// consumer-owned cache-line (see the Side struct below): each holds that
/// side's own index, its cache of the other side's index, and read-only
/// copies of the data pointer/mask/size, so that write() and read() each
/// touch only their own line, plus the other side's line on the occasions
/// they need to refresh their cached index.
template <class T>
struct alignas(hardware_destructive_interference_size) ProducerConsumerQueue {
  using value_type = T;

  ProducerConsumerQueue(const ProducerConsumerQueue&) = delete;
  ProducerConsumerQueue& operator=(const ProducerConsumerQueue&) = delete;

  explicit ProducerConsumerQueue(uint32_t size)
      : ProducerConsumerQueue(makeState(size)) {}

  ~ProducerConsumerQueue() {
    // We need to destruct anything that may still exist in our queue.
    // (No real synchronization needed at destructor time: only one
    // thread can be doing this.) Reads producer_/consumer_'s copies of
    // records/mask, identical to each other, rather than adding back a
    // separate common copy just for this one-time cleanup.
    if (!std::is_trivially_destructible<T>::value) {
      auto readIndex = consumer_.localIndex.load(std::memory_order_relaxed);
      auto const endIndex =
          producer_.localIndex.load(std::memory_order_relaxed);
      while (readIndex != endIndex) {
        producer_.records[readIndex & producer_.mask].~T();
        ++readIndex;
      }
    }

    deallocate(producer_.records);
  }

  //  Checks a private, unsynchronized cache of the other side's cursor
  //  before touching the real cross-thread atomic. localIndex only ever
  //  increases, so a stale remoteIndexCache is always a safe (conservative)
  //  underestimate of the other side's true progress - it is always safe to
  //  trust the cache when it says there is room/data, and a fresh
  //  cross-thread read is only needed when it says there might not be. In
  //  steady state, where the two sides stay within a queue's worth of each
  //  other, that real read is rare rather than universal. This relies on the
  //  cursors being ever-increasing logical counters, wrapped via `& mask`
  //  only at the point of indexing into records, never wrapped in the
  //  stored/compared value itself - with a wrapped cursor, a stale cache can
  //  alias to the wrong answer in either direction, since "behind" is no
  //  longer a total order once values cycle. The comparisons below must be
  //  magnitude checks (`>=`/subtraction), not equality: the cache is only
  //  ever refreshed from within write()/read()/frontPtr(), so a caller that
  //  advances its own cursor another way (e.g. popFront() after an external
  //  frontPtr()) can leave the far side's cache stuck behind the local
  //  cursor by more than zero - equality would then never match again and
  //  the cache would be trusted forever after it stopped meaning anything.
  //  The same ever-increasing representation is also what lets fullness
  //  (occupancy == size) and emptiness (occupancy == 0) be told apart
  //  without reserving a slot: with wrapped indices, both look like
  //  consumer_.localIndex == producer_.localIndex, which is why the classic
  //  version of this design capped usable capacity at size - 1.
  //
  //  Reads only producer_, never consumer_ (whose records/mask are the same
  //  values, but on the other cache-line) - so write()'s hot path touches
  //  exactly one line, except for the occasional cross-read of
  //  consumer_.localIndex below.
  template <class... Args>
  bool write(Args&&... recordArgs) {
    auto const currentWrite =
        producer_.localIndex.load(std::memory_order_relaxed);
    if (currentWrite - producer_.remoteIndexCache >= producer_.size) {
      producer_.remoteIndexCache =
          consumer_.localIndex.load(std::memory_order_acquire);
      if (currentWrite - producer_.remoteIndexCache >= producer_.size) {
        return false; // queue is full
      }
    }
    new (&producer_.records[currentWrite & producer_.mask])
        T(std::forward<Args>(recordArgs)...);
    producer_.localIndex.store(currentWrite + 1, std::memory_order_release);
    return true;
  }

  // move (or copy) the value at the front of the queue to given variable.
  // Reads only consumer_; see write()'s comment.
  bool read(T& record) {
    auto const currentRead =
        consumer_.localIndex.load(std::memory_order_relaxed);
    if (currentRead >= consumer_.remoteIndexCache) {
      consumer_.remoteIndexCache =
          producer_.localIndex.load(std::memory_order_acquire);
      if (currentRead >= consumer_.remoteIndexCache) {
        return false; // queue is empty
      }
    }
    auto const idx = currentRead & consumer_.mask;
    record = std::move(consumer_.records[idx]);
    consumer_.records[idx].~T();
    consumer_.localIndex.store(currentRead + 1, std::memory_order_release);
    return true;
  }

  // pointer to the value at the front of the queue (for use in-place) or
  // nullptr if empty. Cached the same way as read(); see write()'s comment.
  T* frontPtr() {
    auto const currentRead =
        consumer_.localIndex.load(std::memory_order_relaxed);
    if (currentRead >= consumer_.remoteIndexCache) {
      consumer_.remoteIndexCache =
          producer_.localIndex.load(std::memory_order_acquire);
      if (currentRead >= consumer_.remoteIndexCache) {
        // queue is empty
        return nullptr;
      }
    }
    return &consumer_.records[currentRead & consumer_.mask];
  }

  // queue must not be empty
  void popFront() {
    auto const currentRead =
        consumer_.localIndex.load(std::memory_order_relaxed);
    assert(currentRead != producer_.localIndex.load(std::memory_order_acquire));

    auto const idx = currentRead & consumer_.mask;
    consumer_.records[idx].~T();
    consumer_.localIndex.store(currentRead + 1, std::memory_order_release);
  }

  //  An approximation of the current size.
  //  * Clamped to [0, capacity()].
  //  * May be called from any thread.
  //  * Provides no memory ordering.
  //  * If called by consumer, may undercount but not overcount.
  //  * If called by producer, may overcount but not undercount.
  size_t sizeGuess() const noexcept {
    auto const c = consumer_.localIndex.load(std::memory_order_relaxed);
    auto const p = producer_.localIndex.load(std::memory_order_relaxed);
    auto const m = capacity();
    return p < c ? 0 : m < p - c ? m : p - c;
  }

  bool isEmpty() const noexcept { return sizeGuess() == 0; }

  bool isFull() const noexcept { return sizeGuess() == producer_.size; }

  // maximum number of items in the queue.
  size_t capacity() const noexcept { return producer_.size; }

 private:
  //  Everything one side needs on its hot path, bundled into a single
  //  cache-line-sized unit: read-only copies of the pointer/mask/size
  //  (identical between the producer's and the consumer's instance - see
  //  makeState() and the delegating constructor below, which compute them
  //  once and share them between both), this side's own index (written only
  //  by this side), and its cache of the other side's index (refreshed only
  //  when it might be stale; see write()'s comment). One instance per side,
  //  each pinned to its own cache-line by the alignas on producer_/consumer_
  //  below, is what lets write()/read() each touch exactly one line, only
  //  crossing into the other for that rare refresh.
  struct Side {
    T* const records{};
    uint32_t const mask{};
    uint32_t const size{};
    std::atomic<uint64_t> localIndex{};
    uint64_t remoteIndexCache{};
  };

  //  Bundles the three values makeState() computes, so the delegating
  //  constructor below can initialize both producer_ and consumer_ from a
  //  single allocation rather than validating/allocating twice.
  struct State {
    T* records{};
    uint32_t mask{};
    uint32_t size{};
  };

  //  Upper bound on the constructor's size argument: the largest power of
  //  two that fits in uint32_t. Enforcing it up front keeps the nextPowTwo()
  //  call below entirely within uint32_t and bounds how large
  //  sizeof(T) * (mask + 1) can get before allocationSize() below has to
  //  check it against size_t's own range.
  static constexpr uint32_t kMaxSize = uint32_t{1} << 31;

  //  Throws rather than asserting: an invalid size is a caller error that
  //  can occur in release builds too, not a condition this class can allow
  //  itself to just proceed past - proceeding would feed 0 or an
  //  out-of-range value into nextPowTwo() below, which is undefined for 0
  //  and cannot represent a result above kMaxSize in a uint32_t.
  static uint32_t validateSize(uint32_t size) {
    if (size < 1 || size > kMaxSize) {
      throw std::invalid_argument(
          "ProducerConsumerQueue: size must be in [1, 1 << 31]");
    }
    return size;
  }

  //  Computed in 64 bits and checked against size_t's actual range before
  //  narrowing: sizeof(T) * (mask + 1) can legally exceed the 32-bit size_t
  //  that std::malloc() takes on a 32-bit platform, even though mask itself
  //  always fits in uint32_t. Silently truncating that byte count would
  //  allocate a smaller buffer than record indices (computed mod mask+1 in
  //  the wider sense) are entitled to touch.
  static size_t allocationSize(uint32_t mask) {
    uint64_t const bytes = sizeof(T) * (uint64_t{mask} + 1);
    if (bytes > std::numeric_limits<size_t>::max()) {
      throw std::length_error(
          "ProducerConsumerQueue: requested size overflows size_t");
    }
    return static_cast<size_t>(bytes);
  }

  //  std::malloc() only guarantees alignof(std::max_align_t), so a T with a
  //  stricter alignas needs storage from the aligned allocator; constructing
  //  using storage allocated with malloc() would be misaligned, and can fault on the aligned
  //  SIMD instructions the compiler is then entitled to use.
  static constexpr bool kOverAligned = alignof(T) > alignof(std::max_align_t);

  static T* allocate(uint32_t mask) {
    size_t const bytes = allocationSize(mask);

    if constexpr (kOverAligned) {
      return static_cast<T*>(operator_new(bytes, std::align_val_t{alignof(T)}));
    } else {
      auto* const records = static_cast<T*>(std::malloc(bytes));

      if (!records) {
        throw std::bad_alloc();
      }

      return records;
    }
  }

  //  Must mirror allocate(): each allocator must release its own storage.
  static void deallocate(T* records) noexcept {
    if constexpr (kOverAligned) {
      operator_delete(records, std::align_val_t{alignof(T)});
    } else {
      std::free(records);
    }
  }

  static State makeState(uint32_t size) {
    uint32_t const validSize = validateSize(size); // in [1, 1 << 31]
    uint32_t const mask = nextPowTwo(validSize) - 1; // in [1, 1 << 31)
    return State{allocate(mask), mask, validSize};
  }

  explicit ProducerConsumerQueue(State st) noexcept
      : producer_{st.records, st.mask, st.size},
        consumer_{st.records, st.mask, st.size} {}

  alignas(hardware_destructive_interference_size) Side producer_;
  alignas(hardware_destructive_interference_size) Side consumer_;
};

} // namespace folly
