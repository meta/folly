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
#include <cstring>
#include <memory>
#include <string_view>
#include <type_traits>
#include <typeinfo>

#include <folly/Portability.h>
#include <folly/lang/SafeAssert.h>

// The hooked implementation below is available for libc++ when RTTI is
// enabled (control blocks are identified by their dynamic type) and the
// compiler provides the __atomic builtins used for the shared count. It packs
// pointers into 48 bits, which Android's tagged heap pointers break, so it is
// limited to Linux other than Android; other platforms use the locked
// fallback.
#if defined(_LIBCPP_VERSION) && defined(__linux__) && !defined(__ANDROID__) && \
    FOLLY_HAS_RTTI && (defined(__clang__) || defined(__GNUC__))
#define FOLLY_HAS_ATOMIC_SHARED_PTR_HOOKED_LIBCXX 1
#else
#define FOLLY_HAS_ATOMIC_SHARED_PTR_HOOKED_LIBCXX 0
#endif

#if defined(__GLIBCXX__)

namespace folly {
namespace detail {

// This implementation is specific to libstdc++, now accepting
// diffs for other libraries.

// Specifically, this adds support for two things:
// 1) incrementing/decrementing the shared count by more than 1 at a time
// 2) Getting the thing the shared_ptr points to, which may be different from
//    the aliased pointer.

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

  template <typename T>
  static void release_shared(counted_base* base, long count);

  template <typename T>
  static std::remove_extent_t<T>* get_shared_ptr(counted_base* base);

  template <typename T>
  static std::remove_extent_t<T>* release_ptr(std::shared_ptr<T>& p);

  template <typename T>
  static std::shared_ptr<T> get_shared_ptr_from_counted_base(
      counted_base* base, bool inc = true);

 private:
  /* Accessors for private members using explicit template instantiation */
  struct access_shared_ptr {
    using type = shared_count std::__shared_ptr<const void, std::_S_atomic>::*;
    friend type fieldPtr(access_shared_ptr);
  };

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

  struct access_counted_ptr_ptr {
    using type =
        const void* std::_Sp_counted_ptr<const void*, std::_S_atomic>::*;
    friend type fieldPtr(access_counted_ptr_ptr);
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
    shared_ptr_internals::access_shared_ptr,
    &std::__shared_ptr<const void, std::_S_atomic>::_M_refcount>;
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
    shared_ptr_internals::access_counted_ptr_ptr,
    &std::_Sp_counted_ptr<const void*, std::_S_atomic>::_M_ptr>;
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
  return (ptr.*fieldPtr(access_shared_ptr{})).*fieldPtr(access_base{});
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

template <typename T>
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
inline std::remove_extent_t<T>* shared_ptr_internals::get_shared_ptr(
    counted_base* base) {
  // See if this was a make_shared allocation
  auto inplace = base->_M_get_deleter(typeid(std::_Sp_make_shared_tag));
  if (inplace) {
    return (std::remove_extent_t<T>*)inplace;
  }
  // Could also be a _Sp_counted_deleter, but the layout is the same
  using derived_type = std::_Sp_counted_ptr<const void*, std::_S_atomic>;
  auto ptr = reinterpret_cast<derived_type*>(base);
  return (std::remove_extent_t<T>*)(ptr->*fieldPtr(access_counted_ptr_ptr{}));
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
    counted_base* base, bool inc) {
  if (!base) {
    return nullptr;
  }
  std::shared_ptr<const void> newp;
  if (inc) {
    inc_shared_count(base, 1);
  }
  newp.*fieldPtr(access_shared_ptr_ptr{}) =
      get_shared_ptr<const void>(base); // _M_ptr
  (newp.*fieldPtr(access_refcount{})).*fieldPtr(access_base{}) = base;
  // reinterpret_pointer_cast<T>
  auto res = reinterpret_cast<std::shared_ptr<T>*>(&newp);
  return std::move(*res);
}

} // namespace detail
} // namespace folly

#elif FOLLY_HAS_ATOMIC_SHARED_PTR_HOOKED_LIBCXX

// libc++ 20 replaced __compressed_pair with the _LIBCPP_COMPRESSED_PAIR
// macros, and __shared_ptr_pointer's `__data_` pair with a separate `__ptr_`
// member.
#if defined(_LIBCPP_COMPRESSED_PAIR)
#define FOLLY_DETAIL_LIBCXX_PTR_BLOCK_HAS_PTR_MEMBER 1
#else
#define FOLLY_DETAIL_LIBCXX_PTR_BLOCK_HAS_PTR_MEMBER 0
#endif

namespace folly {
namespace detail {

// This implementation is specific to libc++, mirroring the libstdc++
// implementation above.
//
// It hooks into the following libc++ shared_ptr layout facts:
// - std::shared_ptr<T> holds `element_type* __ptr_` and
//   `__shared_weak_count* __cntrl_`.
// - std::__shared_weak_count derives from std::__shared_count, which holds
//   `long __shared_owners_` with use_count() == __shared_owners_ + 1.
//   Releasing the last shared owner drops the count to -1.
// - std::make_shared<T> creates a std::__shared_ptr_emplace<T, A> control
//   block. With an empty allocator the object of any pointer-aligned T sits
//   right after the __shared_weak_count base.
// - shared_ptr(p, ...) creates a std::__shared_ptr_pointer<Tp, D, A>
//   control block, which stores the owned pointer in `__data_` (as the first
//   element of nested __compressed_pairs) or, since libc++ 20, in `__ptr_`.
//
// A shared_ptr<A> can own a control block created for any B, so blocks are
// decoded from their dynamic type alone, never from T. get_shared_ptr()
// returns the pointer a known block owns and nullptr for any other block; the
// store path wraps every value whose pointer does not match, so a block whose
// layout is not as expected is wrapped rather than misread. Pointer blocks are
// read through a fixed instantiation with an empty deleter and allocator,
// since the real ones are often private nested types. That read matches
// whenever the stored pointer precedes the deleter, which holds for every
// libc++ layout except a stateful deleter with libc++ 23+.

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

  template <typename T>
  static void release_shared(counted_base* base, long count);

  template <typename T>
  static std::remove_extent_t<T>* get_shared_ptr(counted_base* base);

  template <typename T>
  static std::remove_extent_t<T>* release_ptr(std::shared_ptr<T>& p);

  template <typename T>
  static std::shared_ptr<T> get_shared_ptr_from_counted_base(
      counted_base* base, bool inc = true);

 private:
  // Verifies, once, every layout assumption above against the library in use
  // and aborts if one does not hold. Member names are fixed at compile time,
  // but their meaning is not; a libc++ copy may carry any backported change.
  static void check_layout();
  static void check_layout_once();
  static void inc_shared_count_unchecked(counted_base* base, long count);

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

  // Fixed instantiation used to read the stored pointer out of any
  // __shared_ptr_pointer block, as above. Never constructed.
  using fixed_ptr_block = std::__shared_ptr_pointer<
      const void*,
      std::default_delete<char>,
      std::allocator<char>>;
  struct access_ptr_block_data {
#if FOLLY_DETAIL_LIBCXX_PTR_BLOCK_HAS_PTR_MEMBER
    using type = const void* fixed_ptr_block::*;
#else
    using type = std::__compressed_pair<
        std::__compressed_pair<const void*, std::default_delete<char>>,
        std::allocator<char>>
        fixed_ptr_block::*;
#endif
    friend type fieldPtr(access_ptr_block_data);
  };

  template <typename Tag, typename Tag::type M>
  struct Rob {
    friend typename Tag::type fieldPtr(Tag) { return M; }
  };

  using fixed_emplace_block =
      std::__shared_ptr_emplace<void*, std::allocator<void*>>;
  static_assert(
      sizeof(fixed_emplace_block) == sizeof(counted_base) + sizeof(void*),
      "make_shared object is expected right after the control block base");

  // Mangled name up to and including the start of the template argument
  // list, e.g. "NSt3__220__shared_ptr_emplaceI".
  static std::string_view family_of(const char* name) {
    std::string_view sv{name};
    return sv.substr(0, sv.find('I') + 1);
  }
  static std::string_view emplace_family() {
    static const std::string_view family =
        family_of(typeid(fixed_emplace_block).name());
    return family;
  }
  static std::string_view pointer_family() {
    static const std::string_view family =
        family_of(typeid(fixed_ptr_block).name());
    return family;
  }
  static bool is_family(const char* name, std::string_view family) {
    // Types with internal linkage get a '*' prefix.
    if (*name == '*') {
      ++name;
    }
    return std::strncmp(name, family.data(), family.size()) == 0;
  }
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
#if FOLLY_DETAIL_LIBCXX_PTR_BLOCK_HAS_PTR_MEMBER
template struct shared_ptr_internals::Rob<
    shared_ptr_internals::access_ptr_block_data,
    &shared_ptr_internals::fixed_ptr_block::__ptr_>;
#else
template struct shared_ptr_internals::Rob<
    shared_ptr_internals::access_ptr_block_data,
    &shared_ptr_internals::fixed_ptr_block::__data_>;
#endif

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
  check_layout_once();
  inc_shared_count_unchecked(base, count);
}

inline void shared_ptr_internals::inc_shared_count_unchecked(
    counted_base* base, long count) {
  auto& shared = base->*fieldPtr(access_shared_owners{});
  // Check that we don't exceed the maximum number of atomic_shared_ptrs.
  // Consider setting EXTERNAL_COUNT lower if this CHECK is hit.
  FOLLY_SAFE_CHECK(
      __atomic_load_n(&shared, __ATOMIC_RELAXED) + 1 + count < INT_MAX,
      "atomic_shared_ptr overflow");
  __atomic_add_fetch(&shared, count, __ATOMIC_RELAXED);
}

template <typename T>
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
inline std::remove_extent_t<T>* shared_ptr_internals::get_shared_ptr(
    counted_base* base) {
  const char* name = typeid(*base).name();
  if (is_family(name, emplace_family())) {
    return reinterpret_cast<std::remove_extent_t<T>*>(
        reinterpret_cast<char*>(base) + sizeof(counted_base));
  }
  if (is_family(name, pointer_family())) {
    auto* fixed = reinterpret_cast<fixed_ptr_block*>(base);
    auto& data = fixed->*fieldPtr(access_ptr_block_data{});
#if FOLLY_DETAIL_LIBCXX_PTR_BLOCK_HAS_PTR_MEMBER
    const void* ptr = data;
#else
    const void* ptr = data.first().first();
#endif
    return static_cast<std::remove_extent_t<T>*>(const_cast<void*>(ptr));
  }
  return nullptr;
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
    counted_base* base, bool inc) {
  if (!base) {
    return nullptr;
  }
  std::shared_ptr<const void> newp;
  if (inc) {
    inc_shared_count(base, 1);
  }
  newp.*fieldPtr(access_shared_ptr_ptr{}) = get_shared_ptr<const void>(base);
  newp.*fieldPtr(access_shared_ptr_cntrl{}) = base;
  // reinterpret_pointer_cast<T>
  auto res = reinterpret_cast<std::shared_ptr<T>*>(&newp);
  return std::move(*res);
}

inline void shared_ptr_internals::check_layout() {
  constexpr const char* kMsg =
      "atomic_shared_ptr: libc++ shared_ptr layout is not as expected";
  int value = 0;
  // shared_ptr<T> is {__ptr_, __cntrl_}.
  std::shared_ptr<int> p(&value, [](int*) {});
  auto& raw = reinterpret_cast<std::shared_ptr<const void>&>(p);
  FOLLY_SAFE_CHECK(raw.*fieldPtr(access_shared_ptr_ptr{}) == &value, kMsg);
  auto* base = get_counted_base(p);
  FOLLY_SAFE_CHECK(base != nullptr, kMsg);
  // use_count() == __shared_owners_ + 1, and a copy adds exactly one.
  auto& shared = base->*fieldPtr(access_shared_owners{});
  FOLLY_SAFE_CHECK(p.use_count() == shared + 1, kMsg);
  {
    auto copy = p;
    FOLLY_SAFE_CHECK(p.use_count() == 2 && shared == 1, kMsg);
  }
  // Adding and releasing counts directly matches use_count().
  inc_shared_count_unchecked(base, 3);
  FOLLY_SAFE_CHECK(p.use_count() == 4, kMsg);
  release_shared<int>(base, 3);
  FOLLY_SAFE_CHECK(p.use_count() == 1, kMsg);
  // Pointer blocks and make_shared blocks decode to the object they own.
  auto* owned = new int(0);
  std::shared_ptr<int> q(owned); // NOLINT(facebook-hte-SharedPtrFromNew)
  FOLLY_SAFE_CHECK(get_shared_ptr<int>(get_counted_base(q)) == owned, kMsg);
  auto m = std::make_shared<void*>(nullptr);
  FOLLY_SAFE_CHECK(get_shared_ptr<void*>(get_counted_base(m)) == m.get(), kMsg);
  // With a stateful deleter the read yields the stored pointer when it
  // precedes the deleter, or the deleter's state when it follows (libc++
  // 23+); the store path wraps the latter. Anything else is unknown.
  struct StatefulDeleter {
    const void* state;
    void operator()(int* ptr) const { delete ptr; }
  };
  auto* stateful = new int(0);
  std::shared_ptr<int> d(stateful, StatefulDeleter{&value});
  auto* read = get_shared_ptr<int>(get_counted_base(d));
  FOLLY_SAFE_CHECK(
      read == stateful || static_cast<const void*>(read) == &value, kMsg);
}

inline void shared_ptr_internals::check_layout_once() {
  static const bool checked = (check_layout(), true);
  (void)checked;
}

} // namespace detail
} // namespace folly

#endif // defined(__GLIBCXX__) || FOLLY_HAS_ATOMIC_SHARED_PTR_HOOKED_LIBCXX
