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
#include <thread>

#include <folly/PackedSyncPtr.h>
#include <folly/Portability.h>
#include <folly/concurrency/detail/AtomicSharedPtr-detail.h>
#include <folly/memory/SanitizeLeak.h>
#include <folly/synchronization/AtomicStruct.h>
#include <folly/synchronization/AtomicUtil.h>

#if (defined(__GLIBCXX__) || FOLLY_HAS_ATOMIC_SHARED_PTR_HOOKED_LIBCXX) && \
    FOLLY_HAS_PACKED_SYNC_PTR
#define FOLLY_HAS_ATOMIC_SHARED_PTR_HOOKED 1
#else
#define FOLLY_HAS_ATOMIC_SHARED_PTR_HOOKED 0
#endif

namespace folly {

#if FOLLY_HAS_ATOMIC_SHARED_PTR_HOOKED

/*
 * This is an implementation of the std::atomic_shared_ptr TS
 * http://en.cppreference.com/w/cpp/experimental/atomic_shared_ptr
 * https://isocpp.org/files/papers/N4162.pdf
 *
 * AFAIK, the only other implementation is Anthony Williams from
 * Just::thread library:
 *
 * https://github.com/anthonywilliams/atomic_shared_ptr
 *
 * implementation details:
 *
 * Basically, three things need to be atomically exchanged to make this work:
 * * the local count
 * * the pointer to the control block
 * * the aliased pointer, if any.
 *
 * The Williams version does it with DWcas: 32 bits for local count, 64
 * bits for control block ptr, and he changes the shared_ptr
 * implementation to also store the aliased pointers using a linked list
 * like structure, and provides 32-bit index accessors to them (like
 * IndexedMemPool trick).
 *
 * This version instead stores the 48 bits of address, plus 16 bits of
 * local count in a single 8byte pointer.  This avoids 'lock cmpxchg16b',
 * which is much slower than 'lock xchg' in the normal 'store' case.  In
 * the less-common aliased pointer scenario, we just allocate it in a new
 * block, and store a pointer to that instead.
 *
 * Note that even if we only want to use the 3-bits of pointer alignment,
 * this trick should still work - Any more than 4 concurrent accesses
 * will have to go to an external map count instead (slower, but lots of
 * concurrent access will be slow anyway due to bouncing cachelines).
 *
 * As a perf optimization, we currently batch up local count and only
 * move it global every once in a while.  This means load() is usually
 * only a single atomic operation, instead of 3.  For this trick to work,
 * we probably need at least 8 bits to make batching worth it.
 */

// A note on noexcept: If the pointer is an aliased pointer,
// store() will allocate.  Otherwise is noexcept.

template <
    typename T,
    template <typename> class Atom = std::atomic,
    typename CountedDetail = detail::shared_ptr_internals>
class atomic_shared_ptr {
  using SharedPtr = typename CountedDetail::template CountedPtr<T>;
  using BasePtr = typename CountedDetail::counted_base;
  using PackedPtr = folly::PackedSyncPtr<BasePtr>;

 public:
  atomic_shared_ptr() noexcept { init(); }
  explicit atomic_shared_ptr(SharedPtr foo) /* noexcept */
      : atomic_shared_ptr() {
    store(std::move(foo));
  }
  atomic_shared_ptr(const atomic_shared_ptr<T>&) = delete;

  ~atomic_shared_ptr() { store(SharedPtr(nullptr)); }
  void operator=(SharedPtr desired) /* noexcept */ {
    store(std::move(desired));
  }
  void operator=(const atomic_shared_ptr<T>&) = delete;

  bool is_lock_free() const noexcept {
    // lock free unless more than EXTERNAL_OFFSET threads are
    // contending and they all get unlucky and scheduled out during
    // load().
    //
    // TODO: Could use a lock-free external map to fix this
    // corner case.
    return true;
  }

  SharedPtr load(
      std::memory_order order = std::memory_order_seq_cst) const noexcept {
    auto local = takeOwnedBase(order);
    return get_shared_ptr(local, false);
  }

  /* implicit */ operator SharedPtr() const { return load(); }

  void store(
      SharedPtr n,
      std::memory_order order = std::memory_order_seq_cst) /* noexcept */ {
    auto newptr = get_newptr(std::move(n));
    auto old = ptr_.exchange(newptr, update_order(order));
    release_external(old);
  }

  SharedPtr exchange(
      SharedPtr n,
      std::memory_order order = std::memory_order_seq_cst) /* noexcept */ {
    auto newptr = get_newptr(std::move(n));
    auto old = ptr_.exchange(newptr, update_order(order));

    SharedPtr old_ptr;

    if (old.get()) {
      old_ptr = get_shared_ptr(old);
      release_external(old);
    }

    return old_ptr;
  }

  bool compare_exchange_weak(
      SharedPtr& expected,
      const SharedPtr& n,
      std::memory_order mo = std::memory_order_seq_cst) noexcept {
    return compare_exchange_weak(expected, n, mo, memory_order_load(mo));
  }
  bool compare_exchange_weak(
      SharedPtr& expected,
      const SharedPtr& n,
      std::memory_order success,
      std::memory_order failure) /* noexcept */ {
    auto newptr = get_newptr(n);
    PackedPtr oldptr, expectedptr;

    oldptr = takeOwnedBase(success);
    if (!owners_eq(oldptr, expected)) {
      expected = get_shared_ptr(oldptr, false);
      release_external(newptr);
      return false;
    }
    expectedptr = oldptr; // Need oldptr to release if failed
    const auto success_order = update_order(success);
    const auto failure_order = memory_order_load(update_order(failure));
    if (ptr_.compare_exchange_weak(
            expectedptr, newptr, success_order, failure_order)) {
      if (oldptr.get()) {
        release_external(oldptr, -1);
      }
      return true;
    } else {
      if (oldptr.get()) {
        expected = get_shared_ptr(oldptr, false);
      } else {
        expected = SharedPtr(nullptr);
      }
      release_external(newptr);
      return false;
    }
  }
  bool compare_exchange_weak(
      SharedPtr& expected,
      SharedPtr&& desired,
      std::memory_order mo = std::memory_order_seq_cst) noexcept {
    return compare_exchange_weak(expected, desired, mo, memory_order_load(mo));
  }
  bool compare_exchange_weak(
      SharedPtr& expected,
      SharedPtr&& desired,
      std::memory_order success,
      std::memory_order failure) /* noexcept */ {
    return compare_exchange_weak(expected, desired, success, failure);
  }
  bool compare_exchange_strong(
      SharedPtr& expected,
      const SharedPtr& n,
      std::memory_order mo = std::memory_order_seq_cst) noexcept {
    return compare_exchange_strong(expected, n, mo, memory_order_load(mo));
  }
  bool compare_exchange_strong(
      SharedPtr& expected,
      const SharedPtr& n,
      std::memory_order success,
      std::memory_order failure) /* noexcept */ {
    auto local_expected = expected;
    do {
      if (compare_exchange_weak(expected, n, success, failure)) {
        return true;
      }
    } while (values_eq(local_expected, expected));

    return false;
  }
  bool compare_exchange_strong(
      SharedPtr& expected,
      SharedPtr&& desired,
      std::memory_order mo = std::memory_order_seq_cst) noexcept {
    return compare_exchange_strong(
        expected, desired, mo, memory_order_load(mo));
  }
  bool compare_exchange_strong(
      SharedPtr& expected,
      SharedPtr&& desired,
      std::memory_order success,
      std::memory_order failure) /* noexcept */ {
    return compare_exchange_strong(expected, desired, success, failure);
  }

 private:
  // Matches packed_sync_pointer.  Must be > max number of local
  // counts.  This is the max number of threads that can access this
  // atomic_shared_ptr at once before we start blocking.
  static constexpr std::uint16_t EXTERNAL_OFFSET{0x2000};
  // Bit signifying aliased constructor
  static constexpr std::uint16_t ALIASED_PTR{0x4000};

  static std::memory_order update_order(std::memory_order order) {
    return order == std::memory_order_seq_cst
        ? order
        : std::memory_order_acq_rel;
  }
  static bool values_eq(const SharedPtr& a, const SharedPtr& b) {
    return a.get() == b.get() &&
        CountedDetail::get_counted_base(a) ==
        CountedDetail::get_counted_base(b);
  }
  static std::uint16_t get_local_count(const PackedPtr& p) {
    return p.extra() & ~ALIASED_PTR;
  }

  static void add_external(BasePtr* res, int64_t c = 0) {
    assert(res);
    CountedDetail::inc_shared_count(res, EXTERNAL_OFFSET + c);
    annotate_object_leaked(res);
  }
  static void release_external(PackedPtr& res, int64_t c = 0) {
    if (!res.get()) {
      return;
    }
    annotate_object_collected(res.get());
    int64_t count = get_local_count(res) + c;
    int64_t diff = EXTERNAL_OFFSET - count;
    assert(diff >= 0);
    CountedDetail::template release_shared<T>(res.get(), diff);
  }

  static PackedPtr get_newptr(const SharedPtr& n) {
    return get_newptr_impl<false>(n);
  }
  static PackedPtr get_newptr(SharedPtr&& n) {
    return get_newptr_impl<true>(std::move(n));
  }
  template <bool kOwn, class S>
  static PackedPtr get_newptr_impl(S&& n) {
    std::uint16_t count = 0;
    BasePtr* newval = CountedDetail::get_counted_base(n);
    if (!n && newval == nullptr) {
      // n is default-constructed, nothing to do.
    } else if (
        newval == nullptr ||
        n.get() != CountedDetail::template get_shared_ptr<T>(newval)) {
      // This is an aliased sharedptr.  Make an un-aliased one by wrapping in
      // *another* shared_ptr.
      auto data =
          CountedDetail::template make_ptr<SharedPtr>(std::forward<S>(n));
      newval = CountedDetail::get_counted_base(data);
      count = ALIASED_PTR;
      CountedDetail::release_ptr(data);
      add_external(newval, -1);
    } else {
      if constexpr (kOwn) {
        CountedDetail::release_ptr(n);
      }
      add_external(newval, kOwn ? -1 : 0);
    }

    PackedPtr newptr;
    newptr.init(newval, count);

    return newptr;
  }

  void init() {
    PackedPtr data;
    data.init();
    ptr_.store(data);
  }

  bool owners_eq(const PackedPtr& p, const SharedPtr& expected) {
    if (p.extra() & ALIASED_PTR) {
      // The caller's local count keeps the wrapper alive. Read the stored
      // shared_ptr in place without taking or dropping a reference.
      const auto* wrapped =
          CountedDetail::template get_shared_ptr<SharedPtr>(p.get());
      return values_eq(*wrapped, expected);
    }
    auto* base = CountedDetail::get_counted_base(expected);
    if (p.get() != base) {
      return false;
    }
    if (!base) {
      return expected.get() == nullptr;
    }
    return CountedDetail::template get_shared_ptr<T>(base) == expected.get();
  }

  SharedPtr get_shared_ptr(const PackedPtr& p, bool inc = true) const {
    bool aliased = p.extra() & ALIASED_PTR;

    auto res = CountedDetail::template get_shared_ptr_from_counted_base<T>(
        p.get(), inc);
    if (aliased) {
      auto aliasedp =
          CountedDetail::template get_shared_ptr_from_counted_base<SharedPtr>(
              p.get());
      res = *aliasedp;
    }
    return res;
  }

  /* Get a reference to the pointer, either from the local batch or
   * from the global count.
   *
   * return is the base ptr, and the previous local count, if it is
   * needed for compare_and_swap later.
   */
  PackedPtr takeOwnedBase(std::memory_order order) const noexcept {
    const auto acquire = memory_order_load(update_order(order));
    PackedPtr local, newlocal;
    local = ptr_.load(acquire);
    while (true) {
      if (!local.get()) {
        return local;
      }
      newlocal = local;
      if (get_local_count(newlocal) + 1 > EXTERNAL_OFFSET) {
        // spinlock in the rare case we have more than
        // EXTERNAL_OFFSET threads trying to access at once.
        std::this_thread::yield();
        // Force DeterministicSchedule to choose a different thread
        local = ptr_.load(acquire);
      } else {
        newlocal.setExtra(newlocal.extra() + 1);
        assert(get_local_count(newlocal) > 0);
        if (ptr_.compare_exchange_weak(local, newlocal, acquire)) {
          break;
        }
      }
    }

    // Check if we need to push a batch from local -> global
    std::uint16_t batchcount = EXTERNAL_OFFSET / 2;
    if (get_local_count(newlocal) > batchcount) {
      CountedDetail::inc_shared_count(newlocal.get(), batchcount);
      putOwnedBase(newlocal.get(), batchcount, order);
    }

    return newlocal;
  }

  void putOwnedBase(
      BasePtr* p, std::uint16_t count, std::memory_order mo) const noexcept {
    PackedPtr local = ptr_.load(std::memory_order_acquire);
    while (true) {
      if (local.get() != p) {
        break;
      }
      auto newlocal = local;
      if (get_local_count(local) > count) {
        newlocal.setExtra(local.extra() - count);
      } else {
        // Otherwise it may be the same pointer, but someone else won
        // the compare_exchange below, local count was already made
        // global.  We decrement the global count directly instead of
        // the local one.
        break;
      }
      if (ptr_.compare_exchange_weak(local, newlocal, update_order(mo))) {
        return;
      }
    }

    CountedDetail::template release_shared<T>(p, count);
  }

  mutable AtomicStruct<PackedPtr, Atom> ptr_;
};

#else

template <typename T>
class atomic_shared_ptr {
 private:
  std::shared_ptr<T> rep_;

 public:
  using value_type = std::shared_ptr<T>;

  atomic_shared_ptr() = default;
  atomic_shared_ptr(std::nullptr_t) noexcept {}
  atomic_shared_ptr(std::shared_ptr<T> desired) noexcept
      : rep_{std::move(desired)} {}

  atomic_shared_ptr(atomic_shared_ptr const&) = delete;
  atomic_shared_ptr(atomic_shared_ptr&&) = delete;

  void operator=(std::nullptr_t) noexcept { store(nullptr); }
  void operator=(std::shared_ptr<T> desired) noexcept {
    store(std::move(desired));
  }

  void operator=(atomic_shared_ptr const&) = delete;
  void operator=(atomic_shared_ptr&&) = delete;

  /* implicit */ operator std::shared_ptr<T>() const noexcept { return load(); }

  bool is_lock_free() const noexcept { return atomic_is_lock_free(&rep_); }

  std::shared_ptr<T> load(
      std::memory_order order = std::memory_order_seq_cst) const noexcept {
    return atomic_load_explicit(&rep_, order);
  }

  void store(
      std::shared_ptr<T> desired,
      std::memory_order order = std::memory_order_seq_cst) noexcept {
    atomic_store_explicit(&rep_, std::move(desired), order);
  }

  std::shared_ptr<T> exchange(
      std::shared_ptr<T> desired,
      std::memory_order order = std::memory_order_seq_cst) noexcept {
    return atomic_exchange_explicit(&rep_, std::move(desired), order);
  }

  bool compare_exchange_weak(
      std::shared_ptr<T>& expected,
      std::shared_ptr<T> desired,
      std::memory_order success,
      std::memory_order failure) noexcept {
    return atomic_compare_exchange_weak_explicit(
        &rep_, &expected, std::move(desired), success, failure);
  }

  bool compare_exchange_weak(
      std::shared_ptr<T>& expected,
      std::shared_ptr<T> desired,
      std::memory_order order = std::memory_order_seq_cst) noexcept {
    return atomic_compare_exchange_weak_explicit(
        &rep_, &expected, std::move(desired), order, memory_order_load(order));
  }

  bool compare_exchange_strong(
      std::shared_ptr<T>& expected,
      std::shared_ptr<T> desired,
      std::memory_order success,
      std::memory_order failure) noexcept {
    return atomic_compare_exchange_strong_explicit(
        &rep_, &expected, std::move(desired), success, failure);
  }

  bool compare_exchange_strong(
      std::shared_ptr<T>& expected,
      std::shared_ptr<T> desired,
      std::memory_order order = std::memory_order_seq_cst) noexcept {
    return atomic_compare_exchange_strong_explicit(
        &rep_, &expected, std::move(desired), order, memory_order_load(order));
  }
};

#endif // FOLLY_HAS_ATOMIC_SHARED_PTR_HOOKED

template <typename T>
atomic_shared_ptr(std::shared_ptr<T>) -> atomic_shared_ptr<T>;

} // namespace folly
