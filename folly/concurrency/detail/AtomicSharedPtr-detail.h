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

#include <limits.h>
#include <atomic>
#include <memory>
#include <type_traits>

#include <folly/Portability.h>
#include <folly/lang/SafeAssert.h>

// The libc++ hooks require compiler atomics for the shared count. Packed
// control-block pointers exclude Android's tagged heap pointers.
#if defined(_LIBCPP_VERSION) && defined(__linux__) && !defined(__ANDROID__) && \
    (defined(__clang__) || defined(__GNUC__))
#define FOLLY_HAS_ATOMIC_SHARED_PTR_HOOKED_LIBCXX 1
#else
#define FOLLY_HAS_ATOMIC_SHARED_PTR_HOOKED_LIBCXX 0
#endif

#if defined(__GLIBCXX__)

namespace folly {
namespace detail {

// libstdc++ hooks for batched counts and shared_ptr ownership transfer.
class shared_ptr_internals {
 public:
  template <typename T, typename... Args>
  static std::shared_ptr<T> make_ptr(Args&&... args) {
    return std::make_shared<T>(std::forward<Args>(args)...);
  }
  using shared_count = std::__shared_count<std::_S_atomic>;
  using counted_base = std::_Sp_counted_base<std::_S_atomic>;
  template <typename T>
  using CountedPtr = std::shared_ptr<T>;

  template <typename T>
  static counted_base* get_counted_base(const std::shared_ptr<T>& bar);

  static void inc_shared_count(counted_base* base, long count);

  static void release_shared(counted_base* base, long count);

  template <typename T>
  static std::remove_extent_t<T>* release_ptr(std::shared_ptr<T>& p);

  template <typename T>
  static std::shared_ptr<T> get_shared_ptr_from_counted_base(
      counted_base* base, std::remove_extent_t<T>* ptr, bool inc = true);

 private:
  /* Accessors for private members using explicit template instantiation */
  struct access_base {
    using type = counted_base* shared_count::*;
    friend type fieldPtr(access_base);
  };

  struct access_use_count {
    using type = _Atomic_word counted_base::*;
    friend type fieldPtr(access_use_count);
  };

  struct access_weak_count {
    using type = _Atomic_word counted_base::*;
    friend type fieldPtr(access_weak_count);
  };

  struct access_shared_ptr_ptr {
    using type = const void* std::__shared_ptr<const void, std::_S_atomic>::*;
    friend type fieldPtr(access_shared_ptr_ptr);
  };

  struct access_refcount {
    using type = shared_count std::__shared_ptr<const void, std::_S_atomic>::*;
    friend type fieldPtr(access_refcount);
  };

  template <typename Tag, typename Tag::type M>
  struct Rob {
    friend typename Tag::type fieldPtr(Tag) { return M; }
  };
};

template struct shared_ptr_internals::Rob<
    shared_ptr_internals::access_base,
    &shared_ptr_internals::shared_count::_M_pi>;
template struct shared_ptr_internals::Rob<
    shared_ptr_internals::access_use_count,
    &shared_ptr_internals::counted_base::_M_use_count>;
template struct shared_ptr_internals::Rob<
    shared_ptr_internals::access_weak_count,
    &shared_ptr_internals::counted_base::_M_weak_count>;
template struct shared_ptr_internals::Rob<
    shared_ptr_internals::access_shared_ptr_ptr,
    &std::__shared_ptr<const void, std::_S_atomic>::_M_ptr>;
template struct shared_ptr_internals::Rob<
    shared_ptr_internals::access_refcount,
    &std::__shared_ptr<const void, std::_S_atomic>::_M_refcount>;

template <typename T>
inline shared_ptr_internals::counted_base*
shared_ptr_internals::get_counted_base(const std::shared_ptr<T>& bar) {
  // reinterpret_pointer_cast<const void>
  // Not quite C++ legal, but explicit template instantiation access to
  // private members requires full type name (i.e. shared_ptr<const void>, not
  // shared_ptr<T>)
  const std::shared_ptr<const void>& ptr(
      reinterpret_cast<const std::shared_ptr<const void>&>(bar));
  return (ptr.*fieldPtr(access_refcount{})).*fieldPtr(access_base{});
}

inline void shared_ptr_internals::inc_shared_count(
    counted_base* base, long count) {
  // Check that we don't exceed the maximum number of atomic_shared_ptrs.
  // Consider setting EXTERNAL_COUNT lower if this CHECK is hit.
  FOLLY_SAFE_CHECK(
      base->_M_get_use_count() + count < INT_MAX, "atomic_shared_ptr overflow");
  __gnu_cxx::__atomic_add_dispatch(
      &(base->*fieldPtr(access_use_count{})), static_cast<int>(count));
}

inline void shared_ptr_internals::release_shared(
    counted_base* base, long count) {
  // If count == 1, this is equivalent to base->_M_release()
  if (__gnu_cxx::__exchange_and_add_dispatch(
          &(base->*fieldPtr(access_use_count{})), -static_cast<int>(count)) ==
      count) {
    base->_M_dispose();

    if (__gnu_cxx::__exchange_and_add_dispatch(
            &(base->*fieldPtr(access_weak_count{})), -1) == 1) {
      base->_M_destroy();
    }
  }
}

template <typename T>
inline std::remove_extent_t<T>* shared_ptr_internals::release_ptr(
    std::shared_ptr<T>& p) {
  auto res = p.get();
  std::shared_ptr<const void>& ptr(
      reinterpret_cast<std::shared_ptr<const void>&>(p));
  ptr.*fieldPtr(access_shared_ptr_ptr{}) = nullptr;
  (ptr.*fieldPtr(access_refcount{})).*fieldPtr(access_base{}) = nullptr;
  return res;
}

template <typename T>
inline std::shared_ptr<T>
shared_ptr_internals::get_shared_ptr_from_counted_base(
    counted_base* base, std::remove_extent_t<T>* ptr, bool inc) {
  std::shared_ptr<const void> owner;
  if (base && inc) {
    inc_shared_count(base, 1);
  }
  (owner.*fieldPtr(access_refcount{})).*fieldPtr(access_base{}) = base;
  return std::shared_ptr<T>(std::move(owner), ptr);
}

} // namespace detail
} // namespace folly

#elif FOLLY_HAS_ATOMIC_SHARED_PTR_HOOKED_LIBCXX

namespace folly {
namespace detail {

// libc++ shared_ptr stores __ptr_ and __cntrl_. Its shared count is
// __shared_owners_ + 1, with the last release dropping that count to -1.
class shared_ptr_internals {
 public:
  template <typename T, typename... Args>
  static std::shared_ptr<T> make_ptr(Args&&... args) {
    return std::make_shared<T>(std::forward<Args>(args)...);
  }
  using counted_base = std::__shared_weak_count;
  template <typename T>
  using CountedPtr = std::shared_ptr<T>;

  template <typename T>
  static counted_base* get_counted_base(const std::shared_ptr<T>& bar);

  static void inc_shared_count(counted_base* base, long count);

  static void release_shared(counted_base* base, long count);

  template <typename T>
  static std::remove_extent_t<T>* release_ptr(std::shared_ptr<T>& p);

  template <typename T>
  static std::shared_ptr<T> get_shared_ptr_from_counted_base(
      counted_base* base, std::remove_extent_t<T>* ptr, bool inc = true);

 private:
  /* Accessors for private members using explicit template instantiation */
  struct access_shared_ptr_ptr {
    using type = const void* std::shared_ptr<const void>::*;
    friend type fieldPtr(access_shared_ptr_ptr);
  };

  struct access_shared_ptr_cntrl {
    using type = counted_base* std::shared_ptr<const void>::*;
    friend type fieldPtr(access_shared_ptr_cntrl);
  };

  struct access_shared_owners {
    using type = long counted_base::*;
    friend type fieldPtr(access_shared_owners);
  };

  template <typename Tag, typename Tag::type M>
  struct Rob {
    friend typename Tag::type fieldPtr(Tag) { return M; }
  };
};

template struct shared_ptr_internals::Rob<
    shared_ptr_internals::access_shared_ptr_ptr,
    &std::shared_ptr<const void>::__ptr_>;
template struct shared_ptr_internals::Rob<
    shared_ptr_internals::access_shared_ptr_cntrl,
    &std::shared_ptr<const void>::__cntrl_>;
template struct shared_ptr_internals::Rob<
    shared_ptr_internals::access_shared_owners,
    static_cast<long shared_ptr_internals::counted_base::*>(
        &std::__shared_count::__shared_owners_)>;

template <typename T>
inline shared_ptr_internals::counted_base*
shared_ptr_internals::get_counted_base(const std::shared_ptr<T>& bar) {
  // reinterpret_pointer_cast<const void>
  // Not quite C++ legal, but explicit template instantiation access to
  // private members requires full type name (i.e. shared_ptr<const void>,
  // not shared_ptr<T>)
  const std::shared_ptr<const void>& ptr(
      reinterpret_cast<const std::shared_ptr<const void>&>(bar));
  return ptr.*fieldPtr(access_shared_ptr_cntrl{});
}

inline void shared_ptr_internals::inc_shared_count(
    counted_base* base, long count) {
  auto& shared = base->*fieldPtr(access_shared_owners{});
  // Check that we don't exceed the maximum number of atomic_shared_ptrs.
  // Consider setting EXTERNAL_COUNT lower if this CHECK is hit.
  FOLLY_SAFE_CHECK(
      __atomic_load_n(&shared, __ATOMIC_RELAXED) + 1 + count < INT_MAX,
      "atomic_shared_ptr overflow");
  __atomic_add_fetch(&shared, count, __ATOMIC_RELAXED);
}

inline void shared_ptr_internals::release_shared(
    counted_base* base, long count) {
  // If count == 1, this is equivalent to base->__release_shared().
  // Decrement all but one of our counts with a plain relaxed RMW (they
  // cannot reach zero: we hold `count` of them), then perform the final
  // decrement through the public __release_shared(), which runs libc++'s
  // -1 release protocol (dispose + weak release) if we were last.
  if (count == 0) {
    return;
  }
  if (count > 1) {
    auto& shared = base->*fieldPtr(access_shared_owners{});
    __atomic_add_fetch(&shared, 1 - count, __ATOMIC_RELAXED);
  }
  base->__release_shared();
}

template <typename T>
inline std::remove_extent_t<T>* shared_ptr_internals::release_ptr(
    std::shared_ptr<T>& p) {
  auto res = p.get();
  std::shared_ptr<const void>& ptr(
      reinterpret_cast<std::shared_ptr<const void>&>(p));
  ptr.*fieldPtr(access_shared_ptr_ptr{}) = nullptr;
  ptr.*fieldPtr(access_shared_ptr_cntrl{}) = nullptr;
  return res;
}

template <typename T>
inline std::shared_ptr<T>
shared_ptr_internals::get_shared_ptr_from_counted_base(
    counted_base* base, std::remove_extent_t<T>* ptr, bool inc) {
  std::shared_ptr<const void> owner;
  if (base && inc) {
    inc_shared_count(base, 1);
  }
  owner.*fieldPtr(access_shared_ptr_cntrl{}) = base;
  return std::shared_ptr<T>(std::move(owner), ptr);
}

} // namespace detail
} // namespace folly

#endif // defined(__GLIBCXX__) || FOLLY_HAS_ATOMIC_SHARED_PTR_HOOKED_LIBCXX
