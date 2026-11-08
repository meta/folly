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
#include <memory>

#include <folly/CppAttributes.h>
#include <folly/Synchronized.h>
#include <folly/observer/Observer.h>
#include <folly/observer/detail/ObserverManager.h>
#include <folly/synchronization/Hazptr.h>

namespace folly {
namespace observer {

/**
 * HazptrObserver implements a read-optimized Observer which caches an
 * Observer's snapshot and protects access to it using hazptrs. The cached
 * snapshot is kept up to date using a callback which fires when the original
 * observer changes. This implementation incurs an additional allocation
 * on updates making it less suitable for write-heavy workloads.
 *
 * There are 2 main APIs:
 * 1) getSnapshot: Returns a Snapshot containing a const pointer to T and guards
 *    access to it using folly::hazptr_holder. The pointer is only safe to use
 *    while the returned Snapshot object is alive.
 * 2) getLocalSnapshot: Same as getSnapshot but backed by folly::hazptr_local.
 *    This API is ~3ns faster than getSnapshot but is unsafe for the current
 *    thread to construct any other hazptr holder type objects (hazptr_holder,
 *    hazptr_array and other hazptr_local) while the returned snapshot exists.
 *
 * See folly/synchronization/Hazptr.h for more details on hazptrs.
 */
template <typename T, template <typename> class Atom = std::atomic>
class HazptrObserver {
  template <typename Holder>
  struct HazptrSnapshot {
    template <typename State>
    explicit HazptrSnapshot(
        const Atom<State*>& state, hazptr_domain<Atom>& domain)
        : holder_() {
      make(holder_, domain);
      ptr_ = get(holder_).protect(state)->snapshot_.get();
    }

    const T& operator*() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
      return *get();
    }
    const T* operator->() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
      return get();
    }
    const T* get() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] { return ptr_; }

   private:
    static void make(hazptr_holder<Atom>& holder, hazptr_domain<Atom>& domain) {
      holder = folly::make_hazard_pointer(domain);
    }
    static void make(hazptr_local<1, Atom>&, hazptr_domain<Atom>&) {}
    static hazptr_holder<Atom>& get(hazptr_holder<Atom>& holder) {
      return holder;
    }
    static hazptr_holder<Atom>& get(hazptr_local<1>& holder) {
      return holder[0];
    }

    Holder holder_;
    const T* ptr_;
  };

 public:
  using DefaultSnapshot = HazptrSnapshot<hazptr_holder<Atom>>;
  using LocalSnapshot = HazptrSnapshot<hazptr_local<1>>;

  explicit HazptrObserver(
      Observer<T> observer,
      hazptr_domain<Atom>& domain = default_hazptr_domain<Atom>())
      : domain_{domain},
        observer_(observer),
        updateObserver_(
            makeObserver([o = std::move(observer), alive = alive_, this]() {
              auto snapshot = o.getSnapshot();
              auto alivePtr = alive->rlock();
              if (!*alivePtr) {
                return folly::unit; // This instance has already been destroyed
              }
              auto oldState = state_.exchange(
                  new State(snapshot), std::memory_order_acq_rel);
              auto& domain = domain_;
              // Release the alive lock before retiring the old state to avoid a
              // potential deadlock due to inline hazptr reclamation
              alivePtr.unlock();
              // At this point, this instance may have already been destroyed
              if (oldState != nullptr) {
                oldState->retire(domain);
              }
              return folly::unit;
            })) {}

  // updateObserver_ captures this, so we cannot move it, hence only the copy
  // constructor is defined (moves will fall back to copy).
  HazptrObserver(const HazptrObserver& r)
      : HazptrObserver(r.observer_, r.domain_) {}
  HazptrObserver& operator=(const HazptrObserver& r) {
    if (&r != this) {
      this->~HazptrObserver();
      new (this) HazptrObserver(r);
    }
    return *this;
  }

  ~HazptrObserver() {
    *alive_->wlock() = false;
    auto* state = state_.load(std::memory_order_acquire);
    if (state) {
      state->retire(domain_);
    }
  }

  DefaultSnapshot getSnapshot() const {
    if (observer_detail::ObserverManager::inManagerThread()) [[unlikely]] {
      // Wait for updates
      updateObserver_.getSnapshot();
    }
    return DefaultSnapshot(state_, domain_);
  }

  LocalSnapshot getLocalSnapshot() const {
    if (observer_detail::ObserverManager::inManagerThread()) [[unlikely]] {
      // Wait for updates
      updateObserver_.getSnapshot();
    }
    return LocalSnapshot(state_, domain_);
  }

  /**
   * Invoke a function with the current observed value. The snapshot is held
   * alive for the duration of the call, preventing read-after-free when
   * accessing members of the observed object.
   *
   * The return type is decayed to prevent accidentally returning a reference
   * into the snapshot's data, which would dangle after the snapshot is
   * destroyed.
   *
   * See Observer::with() for semantics.
   */
  template <typename F>
  std::decay_t<std::invoke_result_t<F, const T&>> with(F&& f) const
      noexcept(noexcept(static_cast<F&&>(f)(std::declval<const T&>()))) {
    auto snapshot = getSnapshot();
    return static_cast<F&&>(f)(*snapshot);
  }

  const Observer<T>& getUnderlyingObserver() const
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return observer_;
  }

 private:
  struct State : public hazptr_obj_base<State, Atom> {
    explicit State(Snapshot<T> snapshot) : snapshot_(std::move(snapshot)) {}

    Snapshot<T> snapshot_;
  };

  Atom<State*> state_{nullptr};
  std::shared_ptr<Synchronized<bool>> alive_{
      std::make_shared<Synchronized<bool>>(true)};
  hazptr_domain<Atom>& domain_;
  Observer<T> observer_;
  Observer<folly::Unit> updateObserver_;
};

/**
 * Same as makeObserver(...), but creates HazptrObserver.
 */
template <typename T>
HazptrObserver<T> makeHazptrObserver(Observer<T> observer) {
  return HazptrObserver<T>(std::move(observer));
}

template <typename F>
auto makeHazptrObserver(F&& creator) {
  return makeHazptrObserver(makeObserver(std::forward<F>(creator)));
}

} // namespace observer
} // namespace folly
