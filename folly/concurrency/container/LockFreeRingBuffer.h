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
#include <compare>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>

#include <folly/Portability.h>
#include <folly/Traits.h>
#include <folly/detail/TurnSequencer.h>
#include <folly/portability/Unistd.h>
#include <folly/synchronization/SanitizeThread.h>

namespace folly {
namespace detail {

template <
    typename T,
    template <typename> class Atom,
    template <typename> class Storage>
class RingBufferSlot;
template <typename T>
class RingBufferTrivialStorage;
template <typename T>
class RingBufferBrokenStorage;

} // namespace detail

/// LockFreeRingBuffer<T> is a fixed-size, concurrent ring buffer with the
/// following semantics:
///
///  1. Writers cannot block on other writers UNLESS they are <capacity> writes
///     apart from each other (writing to the same slot after a wrap-around)
///  2. Writers cannot block on readers
///  3. Readers can wait for writes that haven't occurred yet
///  4. Readers can detect if they are lagging behind
///
/// In this sense, reads from this buffer are best-effort but writes
/// are guaranteed.
///
/// Another way to think about this is as an unbounded stream of writes. The
/// buffer contains the last <capacity> writes but readers can attempt to read
/// any part of the stream, even outside this window. The read API takes a
/// Cursor that can point anywhere in this stream of writes. Reads from the
/// "future" can optionally block but reads from the "past" will always fail.
///

template <
    typename T,
    template <typename> class Atom = std::atomic,
    template <typename> class Storage = detail::RingBufferTrivialStorage>
class LockFreeRingBuffer {
  static_assert(
      std::is_nothrow_default_constructible<T>::value,
      "Element type must be nothrow default constructible");

 public:
  /// Opaque pointer to a past or future write.
  /// Can be moved relative to its current location but not in absolute terms.
  struct Cursor {
    explicit Cursor(uint64_t initialTicket) noexcept : ticket(initialTicket) {}

    /// Returns true if this cursor now points to a different
    /// write, false otherwise.
    bool moveForward(uint64_t steps = 1) noexcept {
      uint64_t prevTicket = ticket;
      ticket += steps;
      return prevTicket != ticket;
    }

    /// Returns true if this cursor now points to a previous
    /// write, false otherwise.
    bool moveBackward(uint64_t steps = 1) noexcept {
      uint64_t prevTicket = ticket;
      if (steps > ticket) {
        ticket = 0;
      } else {
        ticket -= steps;
      }
      return prevTicket != ticket;
    }

    std::strong_ordering operator<=>(const Cursor& that) const noexcept =
        default;

   protected: // for test visibility reasons
    uint64_t ticket;
    friend class LockFreeRingBuffer;
  };

  explicit LockFreeRingBuffer(uint32_t capacity) noexcept
      : capacity_(capacity), slots_(new Slot[capacity]), ticket_(0) {}

  LockFreeRingBuffer(const LockFreeRingBuffer&) = delete;
  LockFreeRingBuffer& operator=(const LockFreeRingBuffer&) = delete;

  uint32_t capacity() const noexcept { return capacity_; }

  /// Perform a single write of an object of type T.
  /// Writes can block iff a previous writer has not yet completed a write
  /// for the same slot (before the most recent wrap-around).
  template <typename V>
  void write(const V& value) noexcept {
    uint64_t ticket = ticket_.fetch_add(1);
    slots_[idx(ticket)].write(turn(ticket), value);
  }

  /// Perform a single write of an object of type T.
  /// Writes can block iff a previous writer has not yet completed a write
  /// for the same slot (before the most recent wrap-around).
  /// Returns a Cursor pointing to the just-written T.
  template <typename V>
  Cursor writeAndGetCursor(const V& value) noexcept {
    uint64_t ticket = ticket_.fetch_add(1);
    slots_[idx(ticket)].write(turn(ticket), value);
    return Cursor(ticket);
  }

  /// Read the value at the cursor.
  /// Returns true if the read succeeded, false otherwise. If the return
  /// value is false, dest is to be considered partially read and in an
  /// inconsistent state. Readers are advised to discard it.
  template <typename V>
  bool tryRead(V& dest, const Cursor& cursor) const noexcept {
    return slots_[idx(cursor.ticket)].tryRead(dest, turn(cursor.ticket));
  }

  /// Read the value at the cursor or block if the write has not occurred yet.
  /// Returns true if the read succeeded, false otherwise. If the return
  /// value is false, dest is to be considered partially read and in an
  /// inconsistent state. Readers are advised to discard it.
  template <typename V>
  bool waitAndTryRead(V& dest, const Cursor& cursor) noexcept {
    return slots_[idx(cursor.ticket)].waitAndTryRead(dest, turn(cursor.ticket));
  }

  /// Returns a Cursor pointing to the first write that has not occurred yet.
  Cursor currentHead() const noexcept { return Cursor(ticket_.load()); }

  /// Returns a Cursor pointing to the earliest readable write.
  Cursor currentTail() const noexcept {
    uint64_t ticket = ticket_.load();

    // can't go back more steps than we've taken
    uint64_t backStep = std::min<uint64_t>(ticket, capacity_);

    return Cursor(ticket - backStep);
  }

  /// Returns the address and length of the internal buffer.
  /// Unsafe to inspect this region at runtime. And not useful.
  /// Useful when using LockFreeRingBuffer to store data which must be retrieved
  /// from a core dump after a crash if the given region is added to the list of
  /// dumped memory regions.
  std::pair<void const*, size_t> internalBufferLocation() const {
    return std::make_pair(
        static_cast<void const*>(slots_.get()), capacity_ * sizeof(Slot));
  }

 private:
  using Slot = detail::RingBufferSlot<T, Atom, Storage>;

  const uint32_t capacity_;

  const std::unique_ptr<Slot[]> slots_;

  Atom<uint64_t> ticket_;

  uint32_t idx(uint64_t ticket) const noexcept { return ticket % capacity_; }

  uint32_t turn(uint64_t ticket) const noexcept {
    return (uint32_t)(ticket / capacity_);
  }
}; // LockFreeRingBuffer

namespace detail {
template <
    typename T,
    template <typename> class Atom,
    template <typename> class Storage>
class RingBufferSlot {
 public:
  explicit RingBufferSlot() noexcept {}

  template <typename V>
  void write(const uint32_t turn, const V& value) noexcept {
    Atom<uint32_t> cutoff(0);
    sequencer_.waitForTurn(turn * 2, cutoff, false);

    // Change to an odd-numbered turn to indicate write in process
    sequencer_.completeTurn(turn * 2);

    storage_.store(value);
    sequencer_.completeTurn(turn * 2 + 1);
    // At (turn + 1) * 2
  }

  template <typename V>
  bool waitAndTryRead(V& dest, uint32_t turn) noexcept {
    uint32_t desired_turn = (turn + 1) * 2;
    Atom<uint32_t> cutoff(0);
    if (sequencer_.tryWaitForTurn(desired_turn, cutoff, false) !=
        TurnSequencer<Atom>::TryWaitResult::SUCCESS) {
      return false;
    }
    storage_.load(dest);

    // if it's still the same turn, we read the value successfully
    return sequencer_.isTurn(desired_turn);
  }

  template <typename V>
  bool tryRead(V& dest, uint32_t turn) const noexcept {
    // The write that started at turn 0 ended at turn 2
    if (!sequencer_.isTurn((turn + 1) * 2)) {
      return false;
    }
    storage_.load(dest);

    // if it's still the same turn, we read the value successfully
    return sequencer_.isTurn((turn + 1) * 2);
  }

 private:
  TurnSequencer<Atom> sequencer_;
  Storage<T> storage_;
};

template <typename T>
class RingBufferTrivialStorage {
  static_assert(std::is_trivially_copyable_v<T>, "T must trivially copyable");

  // Note: If T fits in 8 bytes, folly::AtomicStruct could be used instead.

 public:
  RingBufferTrivialStorage() noexcept {
    annotate_benign_race_sized(
        &data_,
        sizeof(T),
        "T is trivial and sequencer is checked to determine validity",
        __FILE__,
        __LINE__);
  }

  void store(const T& src) {
    // technically undefined behavior: once p1478 is accepted in a future c++,
    // this memcpy may be replaced with atomic_store_per_byte_memcpy
    std::memcpy(&data_, &src, sizeof(T));
    // The sequencer protects this store with its own state_ store-release
  }

  void load(T& dest) const {
    // the sequencer protects this load with its own state_ load-acquire
    // technically undefined behavior: once p1478 is accepted in a future c++,
    // this memcpy may be replaced with atomic_store_per_byte_memcpy
    std::memcpy(&dest, &data_, sizeof(T));
  }

 private:
  // No initialization is necessary because the sequencer is checked before data
  // is returned.
  T data_;
};

template <typename T>
class [[deprecated(
    "It is UB to race loads and stores across multiple threads. "
    "Use RingBufferTrivialStorage.")]] RingBufferBrokenStorage {
 public:
  void store(const T& src) { data_ = src; }

  void load(T& dest) const { dest = data_; }

 private:
  T data_{};
};

} // namespace detail
} // namespace folly
