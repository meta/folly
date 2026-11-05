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

#include <folly/concurrency/CacheLocality.h>
#include <folly/synchronization/Hazptr-fwd.h>

namespace folly {

/**
 *  hazptr_rec:
 *
 *  Contains the actual hazard pointer.
 */
template <template <typename> class Atom>
class alignas(hardware_destructive_interference_size) hazptr_rec {
  Atom<const void*> hazptr_{nullptr}; // the hazard pointer
  hazptr_domain<Atom>* domain_;
  hazptr_rec* nextAvail_{nullptr}; // Next available hazard pointer.

  friend class hazptr_domain<Atom>;
  friend class hazptr_holder<Atom>;
  friend class hazptr_tc<Atom>;
  template <uint8_t, template <typename> class>
  friend class hazptr_array;
  friend hazptr_holder<Atom> make_hazard_pointer<Atom>(hazptr_domain<Atom>&);
  template <uint8_t M, template <typename> class A>
  friend hazptr_array<M, A> make_hazard_pointer_array();

  explicit hazptr_rec(hazptr_domain<Atom>* domain) noexcept : domain_{domain} {}

  const void* hazptr(std::memory_order order) const noexcept {
    return hazptr_.load(order);
  }

  FOLLY_ALWAYS_INLINE void reset_hazptr(const void* p = nullptr) noexcept {
    //  A call to this function with a null value always ends a reader critical
    //  section. A call with a not-null value always begins a reader critical
    //  section and, if the call occurs in a previous reader critical section,
    //  also ends it.
    //
    //  In the reclamation path, the domain:
    //  - Extracts the list of retired pointers.
    //  - Walks the recs and collects the set of protected pointers.
    //  - Calculates the list of retired unprotected pointers.
    //  - Reclaims the retired unprotected pointers.
    //
    //  When beginning a reader critical section, the pointer is not-null and no
    //  memory order is needed. The store-release does not synchronize-with any
    //  load-acquire.
    //
    //  When ending a reader critical section, the pointer may be either null or
    //  not-null and a memory order is needed. The store-release synchronizes-
    //  with the load-acquire on this same rec in subsequent reclamation path
    //  rec walks. When that load-acquire reads-from this store-release, the
    //  object which was previously protected by this rec and is now unprotected
    //  by this store-release becomes eligible for reclamation, by not being
    //  removed from the list of retired pointers. From this synchronization,
    //  all modifications to that object during the critical section in the
    //  reader thread happen-before reclamation in the reclaimer thread.
    if constexpr (detail::hazptr_prefer_fence_light) {
      asymmetric_thread_fence_traits<Atom>::light(std::memory_order_release);
      hazptr_.store(p, std::memory_order_relaxed);
    } else {
      hazptr_.store(p, std::memory_order_release);
    }
  }

  hazptr_rec<Atom>* next_avail() { return nextAvail_; }

  void set_next_avail(hazptr_rec<Atom>* rec) { nextAvail_ = rec; }

  FOLLY_ALWAYS_INLINE hazptr_domain<Atom>* domain() { return domain_; }
}; // hazptr_rec

} // namespace folly
