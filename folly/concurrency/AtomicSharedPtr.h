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

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <thread>

#include <folly/PackedSyncPtr.h>
#include <folly/Portability.h>
#include <folly/concurrency/detail/AtomicSharedPtr-detail.h>
#include <folly/memory/SanitizeLeak.h>
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
 * The backends store the object pointer alongside a packed control-block
 * pointer and local count in one 16-byte atomic. This preserves aliases
 * directly without inspecting control-block types or allocating a wrapper.
 * Reconstruction takes both the control block and the stored object pointer.
 *
 * Batching avoids a separate control-block reference-count update on most
 * owning loads. Each owning load seeds a snapshot, then reserves a local
 * reference with a validating CAS. Keeping enough local-count bits makes
 * these batches worthwhile.
 */

template <
    typename T,
    template <typename> class Atom = std::atomic,
    typename CountedDetail = detail::shared_ptr_internals>
class atomic_shared_ptr {
  using SharedPtr = typename CountedDetail::template CountedPtr<T>;
  using BasePtr = typename CountedDetail::counted_base;
  using ValuePtr = decltype(std::declval<const SharedPtr&>().get());

  struct alignas(16) stored_pointer {
    folly::PackedSyncPtr<BasePtr> counted;
    ValuePtr value;

    void init(
        BasePtr* base = nullptr,
        std::uint16_t count = 0,
        ValuePtr ptr = nullptr) {
      counted.init(base, count);
      value = ptr;
    }
    BasePtr* get() const { return counted.get(); }
    std::uint16_t extra() const { return counted.extra(); }
    void setExtra(std::uint16_t count) { counted.setExtra(count); }

    template <typename Stream>
    friend Stream& operator<<(Stream& out, const stored_pointer& p) {
      return out << p.counted << ' ' << p.value;
    }
  };
  static_assert(sizeof(stored_pointer) == 16);
  using PackedPtr = stored_pointer;
  using AtomicPtr = Atom<PackedPtr>;
  static constexpr bool kNativeLockFreeAtomic =
      std::is_same_v<AtomicPtr, std::atomic<PackedPtr>> &&
      std::atomic<PackedPtr>::is_always_lock_free;
  static constexpr bool kUseArmRelaxedFullWidthSeed =
      kIsArchAArch64Lse2 && kNativeLockFreeAtomic;
  static constexpr bool kSplitLoad = (kIsArchAArch64 || kIsArchAmd64) &&
      !kIsSanitizeThread && !kUseArmRelaxedFullWidthSeed &&
      kNativeLockFreeAtomic;

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
    // Reports lock freedom of the stored-pointer atomic and selected backend.
    // Even when true, load() can spin at the EXTERNAL_OFFSET local-count limit.
    // TODO: A lock-free external map could avoid this local-count limit.
    return ptr_.is_lock_free();
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

    if (!is_empty(old)) {
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
      if (!is_empty(oldptr)) {
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
  // Marks a non-null stored pointer without a control block.
  static constexpr std::uint16_t OWNERLESS_PTR{0x4000};

  static std::memory_order update_order(std::memory_order order) {
    return order == std::memory_order_seq_cst
        ? order
        : std::memory_order_acq_rel;
  }
  static bool is_empty(const PackedPtr& p) {
    // Canonical empty has both words zero. A non-null value without an owner
    // always carries OWNERLESS_PTR, so the counted word alone identifies empty.
    return p.get() == nullptr && p.extra() == 0;
  }
  static bool is_ownerless(const PackedPtr& p) {
    return p.get() == nullptr && (p.extra() & OWNERLESS_PTR) != 0;
  }
  static bool values_eq(const SharedPtr& a, const SharedPtr& b) {
    return a.get() == b.get() &&
        CountedDetail::get_counted_base(a) ==
        CountedDetail::get_counted_base(b);
  }

  template <bool kLoadSecond>
  std::array<std::uint64_t, 2> load_words_unsafe(
      std::memory_order order) const noexcept {
    static_assert(sizeof(AtomicPtr) == 16 && alignof(AtomicPtr) == 16);
    // Deliberate C++ UB: these atomic<uint64_t> objects do not exist.
    // Full-width validation does not make these reads defined.
    //
    // Native x86 owning loads benefit from avoiding SIMD-to-GPR transfers
    // for CAS, even with AVX2; older x86 loads may themselves use CAS.
    // ARM LSE2 targets use defined relaxed full-width seeds instead, with
    // first-word empty confirmation avoiding a full-width ordered-load barrier.
    // Keep splitting on older ARM targets without cheap untorn loads.
    // Retire splitting when defined loads avoid these costs.
    //
    // Only the zero discriminator is final. Owned snapshots reserve a
    // reference by CAS; ownerless snapshots need a full-width ordered reload.
    const auto* words =
        reinterpret_cast<const std::atomic<std::uint64_t>*>(&ptr_);
    std::array<std::uint64_t, 2> result{words[0].load(order), 0};
    if constexpr (kLoadSecond) {
      if (result[0] != 0) {
        result[1] = words[1].load(std::memory_order_relaxed);
      }
    }
    return result;
  }

  PackedPtr load_ptr(std::memory_order order) const noexcept {
    if constexpr (kUseArmRelaxedFullWidthSeed) {
      if (kIsArchAArch64Lrcpc3 && order != std::memory_order_seq_cst) {
        return ptr_.load(order);
      }
      // Owned seeds acquire through the validating CAS. Null-owner seeds need
      // an ordered observation before returning without CAS.
      const auto seed = ptr_.load(std::memory_order_relaxed);
      if (seed.get()) {
        return seed;
      }
      if constexpr (!kIsSanitizeThread) {
        if (is_empty(seed) && load_words_unsafe<false>(order)[0] == 0) {
          return {};
        }
      }
      // Ownerless or a nonempty discriminator observed after the empty seed.
      return ptr_.load(order);
    } else if constexpr (kSplitLoad) {
      return std::bit_cast<PackedPtr>(load_words_unsafe<true>(order));
    } else {
      return ptr_.load(order);
    }
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
    int64_t count = res.extra() + c;
    int64_t diff = EXTERNAL_OFFSET - count;
    assert(diff >= 0);
    CountedDetail::release_shared(res.get(), diff);
  }

  static PackedPtr get_newptr(const SharedPtr& n) {
    return get_newptr_impl<false>(n);
  }
  static PackedPtr get_newptr(SharedPtr&& n) {
    return get_newptr_impl<true>(std::move(n));
  }
  template <bool kOwn, class S>
  static PackedPtr get_newptr_impl(S&& n) {
    BasePtr* newval = CountedDetail::get_counted_base(n);
    auto* value = n.get();
    if (newval) {
      add_external(newval, kOwn ? -1 : 0);
    }
    if constexpr (kOwn) {
      CountedDetail::release_ptr(n);
    }
    PackedPtr newptr;
    newptr.init(newval, !newval && value ? OWNERLESS_PTR : 0, value);
    return newptr;
  }

  void init() {
    PackedPtr data;
    data.init();
    ptr_.store(data);
  }

  bool owners_eq(const PackedPtr& p, const SharedPtr& expected) {
    auto* base = CountedDetail::get_counted_base(expected);
    return p.get() == base && p.value == expected.get();
  }

  SharedPtr get_shared_ptr(const PackedPtr& p, bool inc = true) const {
    return CountedDetail::template get_shared_ptr_from_counted_base<T>(
        p.get(), p.value, inc);
  }

  // Returns the control block, stored object pointer and local count or
  // ownerless discriminator. Owning snapshots reserve a reference; empty and
  // ownerless snapshots do not. Split ownerless seeds need an ordered reload.
  PackedPtr takeOwnedBase(std::memory_order order) const noexcept {
    const auto acquire = memory_order_load(update_order(order));
    PackedPtr local, newlocal;
    local = load_ptr(acquire);
    while (true) {
      if (!local.get()) {
        if constexpr (kSplitLoad) {
          if (is_ownerless(local)) {
            local = ptr_.load(acquire);
            if (local.get()) {
              continue;
            }
          }
        }
        return local;
      }

      if (local.extra() + 1 > EXTERNAL_OFFSET) {
        // spinlock in the rare case we have more than
        // EXTERNAL_OFFSET threads trying to access at once.
        std::this_thread::yield();
        // Force DeterministicSchedule to choose a different thread
        local = load_ptr(acquire);
        continue;
      }

      newlocal = local;
      newlocal.setExtra(newlocal.extra() + 1);
      assert(newlocal.extra() > 0);
      if (ptr_.compare_exchange_weak(local, newlocal, acquire)) {
        break;
      }
    }

    // Check if we need to push a batch from local -> global
    std::uint16_t batchcount = EXTERNAL_OFFSET / 2;
    if (newlocal.extra() > batchcount) {
      CountedDetail::inc_shared_count(newlocal.get(), batchcount);
      putOwnedBase(newlocal.get(), batchcount, order);
    }

    return newlocal;
  }

  void putOwnedBase(
      BasePtr* p, std::uint16_t count, std::memory_order mo) const noexcept {
    PackedPtr local = load_ptr(std::memory_order_acquire);
    while (true) {
      if (local.get() != p) {
        break;
      }
      auto newlocal = local;
      if (local.extra() > count) {
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

    CountedDetail::release_shared(p, count);
  }

  mutable AtomicPtr ptr_;
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
