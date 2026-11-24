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

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

#include <folly/SharedMutex.h>
#include <folly/ThreadLocal.h>
#include <folly/Utility.h>
#include <folly/lang/Access.h>
#include <folly/synchronization/Lock.h>
#include <folly/synchronization/RelaxedAtomic.h>

namespace folly {

//  thread_cached_synchronized
//
//  Roughly equivalent to Synchronized, but with a per-thread cache for
//  acceleration.
//
//  Use in hot code when Synchronized alone, with its shared lock and unlock,
//  would be too costly.
//
//  Avoid when acceleration is marginal since per-thread caches are expensive.
//
//  Example:
//
//    struct writer_and_readers {
//      folly::thread_cached_synchronized<std::shared_ptr<data>> obj_;
//      std::jthread background_writer_{std::bind(loop_update, this)};
//
//      std::shared_ptr<data> get_recent_data_fast() { return obj; }
//
//      data fetch_recent_data();
//      bool needs_data_and_not_signaled_done();
//      void loop_update() {
//        while (needs_data_and_not_signaled_done()) {
//          obj.exchange(folly::copy_to_shared_ptr(fetch_recent_data()));
//        }
//      }
//    };
//
//  Note:
//    A singleton variation of this with SingletonThreadLocal would remove one
//    of the branches when looking up the per-thread cache but would introduce
//    a new branch when looking up the global version.
template <typename T, typename Mutex = SharedMutex>
class thread_cached_synchronized {
  static_assert(std::is_same<std::decay_t<T>, T>::value, "not decayed");
  static_assert(std::is_copy_constructible<T>::value, "not copy-constructible");

 public:
  using value_type = T;

 private:
  using version_type = std::uint64_t; // 64 bits will not overflow

  struct truth_state {
    relaxed_atomic<version_type> version{0}; // tiny optimization if first field
    Mutex mutex{}; // protects value and sometimes version
    value_type value;

    template <typename... A>
    truth_state(A&&... a) noexcept(
        std::is_nothrow_constructible<Mutex>{} &&
        std::is_nothrow_constructible<value_type, A...>{})
        : value{static_cast<A&&>(a)...} {}
  };

  struct cache_state {
    version_type version{0};
    value_type value;

    template <typename... A>
    cache_state(A&&... a) //
        noexcept(std::is_nothrow_constructible<value_type, A...>{})
        : value{static_cast<A&&>(a)...} {}
  };
  using tlp_cache_state = ThreadLocalPtr<cache_state>;

  template <typename... A>
  static constexpr bool nx =
      noexcept(truth_state{std::in_place, FOLLY_DECLVAL(A)...});

  using swap_fn = access::swap_fn;

 public:
  template <typename A = value_type>
    requires std::is_constructible_v<A>
  thread_cached_synchronized() noexcept(nx<>) : truth_{std::in_place} {}
  explicit thread_cached_synchronized(value_type const& a) //
      noexcept(nx<value_type const&>)
      : truth_{a} {}
  explicit thread_cached_synchronized(value_type&& a) noexcept(nx<value_type&&>)
      : truth_{static_cast<value_type&&>(a)} {}
  template <typename A>
    requires std::is_constructible_v<value_type, A>
  explicit thread_cached_synchronized(A&& a) noexcept(nx<A&&>)
      : truth_{static_cast<A&&>(a)} {}
  template <typename... A>
    requires std::is_constructible_v<value_type, A...>
  explicit thread_cached_synchronized(std::in_place_t, A&&... a) noexcept(
      nx<A&&...>)
      : truth_{static_cast<A&&>(a)...} {}

  template <typename A>
    requires std::is_assignable_v<value_type&, A>
  thread_cached_synchronized& operator=(A&& a) noexcept(false) {
    store(static_cast<A&&>(a));
    return *this;
  }

  template <typename A = value_type>
  void store(A&& a = A{}) {
    mutate([&](auto& value) { value = static_cast<A&&>(a); });
  }

  template <typename A = value_type>
  value_type exchange(A&& a = A{}) {
    return mutate([&](auto& value) { //
      return std::exchange(value, static_cast<A&&>(a));
    });
  }

  template <typename A>
  bool compare_exchange(value_type& expected, A&& desired) {
    return mutate_cx(expected, static_cast<A&&>(desired));
  }

  template <typename A>
  void swap(A& that) noexcept(false) {
    mutate([&](auto& value) { access::swap(value, that); });
  }

  template <typename A>
    requires std::is_invocable_v<swap_fn, value_type&, A&>
  friend void swap(thread_cached_synchronized& self, A& that) noexcept(false) {
    self.swap(that);
  }

  value_type const& operator*() const { return ref(); }
  value_type const* operator->() const { return std::addressof(ref()); }
  value_type load() const { return std::as_const(ref()); }
  /* implicit */ operator value_type() const { return load(); }

 private:
  void invalidate_caches() {
    truth_.version = truth_.version + 1; // intentionally not +=
  }

  template <typename F, typename R = invoke_result_t<F, value_type&>>
  R mutate_locked(F f) {
    if constexpr (std::is_void_v<R>) {
      f(truth_.value); // value first: mutation may throw
      invalidate_caches();
    } else {
      decltype(auto) ret = f(truth_.value); // value first: mutation may throw
      invalidate_caches();
      return ret;
    }
  }
  template <typename F, typename R = invoke_result_t<F, value_type&>>
  R mutate(F f) {
    std::unique_lock<Mutex> lock{truth_.mutex};
    return mutate_locked(f);
  }

  template <typename A>
  bool mutate_cx(value_type& expected, A&& desired) {
    std::unique_lock<Mutex> lock{truth_.mutex};
    auto const eq = std::as_const(truth_.value) == std::as_const(expected);
    if (eq) {
      truth_.value = // value first: mutation may throw
          static_cast<A&&>(desired);
      invalidate_caches();
    } else {
      expected = std::as_const(truth_.value);
    }
    return eq;
  }

  FOLLY_ERASE value_type& ref() const {
    auto const cache = cache_.get();
    auto const unexpired = cache && cache->version == truth_.version;
    return FOLLY_LIKELY(unexpired) ? cache->value : get_slow();
  }

  FOLLY_NOINLINE value_type& get_slow() const {
    hybrid_lock<Mutex> lock{truth_.mutex};
    auto cache = cache_.get();
    if (cache == nullptr) {
      cache = new cache_state{truth_.value}; // value first: copy may throw
      cache_.reset(cache);
    } else {
      cache->value = truth_.value; // value first: copy may throw
    }
    cache->version = truth_.version;
    return cache->value;
  }

  mutable truth_state truth_;
  mutable tlp_cache_state cache_;
};

} // namespace folly
