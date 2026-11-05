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

#include <algorithm>
#include <atomic>
#include <vector>

#include <glog/logging.h>

#include <folly/lang/Align.h>
#include <folly/lang/Bits.h>
#include <folly/synchronization/Hazptr-fwd.h>
#include <folly/synchronization/HazptrObj.h>
#include <folly/synchronization/HazptrRec.h>

#if FOLLY_HAZPTR_THR_LOCAL

#include <folly/SingletonThreadLocal.h>

#endif // FOLLY_HAZPTR_THR_LOCAL

/**
 *  Thread local classes and singletons
 */

namespace folly {

/**
 *  hazptr_tc_entry
 *
 *  Thread cache entry.
 */
template <template <typename> class Atom>
class hazptr_tc_entry {
  hazptr_rec<Atom>* hprec_;

  template <uint8_t, template <typename> class>
  friend class hazptr_array;
  template <uint8_t, template <typename> class>
  friend class hazptr_local;
  friend class hazptr_tc<Atom>;
  template <uint8_t M, template <typename> class A>
  friend hazptr_array<M, A> make_hazard_pointer_array();

  FOLLY_ALWAYS_INLINE void fill(hazptr_rec<Atom>* hprec) noexcept {
    hprec_ = hprec;
  }

  FOLLY_ALWAYS_INLINE hazptr_rec<Atom>* get() const noexcept { return hprec_; }
}; // hazptr_tc_entry

/**
 *  hazptr_tc:
 *
 *  Thread cache of hazptr_rec-s that belong to the default domain.
 *
 *  Uses a single dynamically-sized vector as the backing store. The
 *  hot paths (try_get, try_put) touch two cache lines for pure
 *  thread-local work with no atomics or contention: one for the
 *  vector metadata (data pointer, size, capacity) and count, and one
 *  for the actual entry being read or written.
 *
 *  A usage-aware decay mechanism gradually releases excess records
 *  (those beyond kCapacity) back to the domain when demand has
 *  subsided. Every kDecayThreshold fast-path puts when count exceeds
 *  kCapacity, the algorithm checks whether the excess was actively
 *  used during the window. If underutilized, both the record count
 *  and the allocation are shrunk, reclaiming memory. Active use
 *  (via try_put_slow) resets the tick counter and starts a new decay
 *  window.
 */
template <template <typename> class Atom>
class alignas(hardware_constructive_interference_size) hazptr_tc {
  static constexpr size_t kCapacity = 16;
  static constexpr size_t kDecayThreshold = 1024;

  using Rec = hazptr_rec<Atom>;
  using Entry = hazptr_tc_entry<Atom>;

  size_t count_{0};
  size_t window_start_{0};
  uint32_t put_tick_{0};
  bool local_{false}; // for debug mode only
  std::vector<Entry> vec_;

 public:
  hazptr_tc() : vec_(kCapacity) {}
  ~hazptr_tc() { evict(); }

  hazptr_tc(const hazptr_tc&) = delete;
  hazptr_tc(hazptr_tc&&) = delete;
  hazptr_tc& operator=(const hazptr_tc&) = delete;
  hazptr_tc& operator=(hazptr_tc&&) = delete;

  static constexpr size_t min_capacity() noexcept { return kCapacity; }

  FOLLY_ALWAYS_INLINE size_t count() const noexcept { return count_; }

  size_t allocated_capacity() const noexcept { return vec_.capacity(); }

 private:
  template <uint8_t, template <typename> class>
  friend class hazptr_array;
  friend class hazptr_holder<Atom>;
  template <uint8_t, template <typename> class>
  friend class hazptr_local;
  friend hazptr_holder<Atom> make_hazard_pointer<Atom>();
  friend hazptr_holder<Atom> make_hazard_pointer<Atom>(hazptr_domain<Atom>&);
  template <uint8_t M, template <typename> class A>
  friend hazptr_array<M, A> make_hazard_pointer_array();
  friend void hazptr_tc_evict<Atom>();
  template <template <typename> class A>
  friend struct hazptr_tc_tester;

  FOLLY_ALWAYS_INLINE
  Entry& operator[](size_t i) noexcept {
    DCHECK(i < vec_.size());
    return vec_[i];
  }

  FOLLY_ALWAYS_INLINE Rec* try_get() noexcept {
    if (FOLLY_LIKELY(count_ > 0)) {
      return vec_[--count_].get();
    }
    return nullptr;
  }

  FOLLY_ALWAYS_INLINE bool try_put(Rec* hprec) noexcept {
    if (FOLLY_LIKELY(count_ < vec_.size())) {
      vec_[count_++].fill(hprec);
      if (FOLLY_UNLIKELY(
              count_ > kCapacity && ++put_tick_ >= kDecayThreshold)) {
        decay();
      }
      return true;
    }
    return try_put_slow(hprec);
  }

  FOLLY_NOINLINE bool try_put_slow(Rec* hprec) noexcept {
    put_tick_ = 0;
    size_t new_cap = std::max(kCapacity, folly::nextPowTwo(count_ + 1));
    vec_.resize(new_cap);
    vec_[count_++].fill(hprec);
    window_start_ = count_;
    return true;
  }

  FOLLY_NOINLINE void decay() {
    put_tick_ = 0;
    if (count_ <= kCapacity) {
      return;
    }

    size_t cap = vec_.capacity();
    size_t excess = count_ - kCapacity;
    size_t excess_cap = cap - kCapacity;
    size_t start_excess =
        window_start_ > kCapacity ? window_start_ - kCapacity : 0;
    size_t drawn = start_excess > excess ? start_excess - excess : 0;

    if (drawn <= excess_cap / 4) {
      size_t new_cap = std::max(kCapacity, cap / 2);
      size_t new_excess_cap = new_cap > kCapacity ? new_cap - kCapacity : 0;
      size_t new_excess = std::min(excess, new_excess_cap / 2);
      size_t new_count = kCapacity + new_excess;
      if (new_count < count_ || new_cap < cap) {
        shrink_to(new_count, new_cap);
      }
    }
    window_start_ = count_;
  }

  void shrink_to(size_t new_count, size_t new_cap) {
    release_hprecs(new_count, count_);
    count_ = new_count;
    if (new_cap < vec_.capacity()) {
      std::vector<Entry> new_vec(vec_.begin(), vec_.begin() + count_);
      new_vec.resize(new_cap);
      vec_ = std::move(new_vec);
    }
  }

  FOLLY_ALWAYS_INLINE void set_count(size_t val) noexcept { count_ = val; }

  FOLLY_NOINLINE void fill(size_t num) {
    if (count_ + num > vec_.size()) {
      size_t new_cap = std::max(kCapacity, folly::nextPowTwo(count_ + num));
      vec_.resize(new_cap);
    }
    auto& domain = default_hazptr_domain<Atom>();
    Rec* hprec = domain.acquire_hprecs(num);
    for (size_t i = 0; i < num; ++i) {
      DCHECK(hprec);
      Rec* next = hprec->next_avail();
      hprec->set_next_avail(nullptr);
      vec_[count_++].fill(hprec);
      hprec = next;
    }
    DCHECK(hprec == nullptr);
  }

  FOLLY_NOINLINE void evict(size_t num) {
    DCHECK_GE(count_, num);
    if (num == 0) {
      return;
    }
    size_t new_count = count_ - num;
    release_hprecs(new_count, count_);
    count_ = new_count;
  }

  void evict() {
    release_hprecs(0, count_);
    count_ = 0;
  }

  void release_hprecs(size_t from, size_t to) {
    if (from >= to) {
      return;
    }
    Rec* head = nullptr;
    Rec* tail = nullptr;
    for (size_t i = from; i < to; ++i) {
      Rec* rec = vec_[i].get();
      rec->set_next_avail(head);
      head = rec;
      if (!tail) {
        tail = rec;
      }
    }
    if (head) {
      hazard_pointer_default_domain<Atom>().release_hprecs(head, tail);
    }
  }

  bool local() const noexcept { // for debugging only
    return local_;
  }

  void set_local(bool b) noexcept { // for debugging only
    local_ = b;
  }
}; // hazptr_tc

#if FOLLY_HAZPTR_THR_LOCAL

struct hazptr_tc_tls_tag {};
/** hazptr_tc_tls */
template <template <typename> class Atom>
FOLLY_ALWAYS_INLINE hazptr_tc<Atom>& hazptr_tc_tls() {
  return folly::SingletonThreadLocal<hazptr_tc<Atom>, hazptr_tc_tls_tag>::get();
}

/** hazptr_tc_evict -- Used only for benchmarking */
template <template <typename> class Atom>
void hazptr_tc_evict() {
  hazptr_tc_tls<Atom>().evict();
}

#endif // FOLLY_HAZPTR_THR_LOCAL

} // namespace folly
