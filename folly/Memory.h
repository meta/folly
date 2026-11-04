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

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <folly/ConstexprMath.h>
#include <folly/Likely.h>
#include <folly/Portability.h>
#include <folly/Traits.h>
#include <folly/Utility.h>
#include <folly/functional/Invoke.h>
#include <folly/lang/Align.h>
#include <folly/lang/Exception.h>
#include <folly/lang/Thunk.h>
#include <folly/portability/Config.h>
#include <folly/portability/Malloc.h>

namespace folly {

#if (defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L) || \
    (defined(_XOPEN_SOURCE) && _XOPEN_SOURCE >= 600) ||         \
    (defined(__ANDROID__) && (__ANDROID_API__ > 16)) ||         \
    (defined(__APPLE__)) || defined(__FreeBSD__) || defined(__wasm__)

inline void* aligned_malloc(size_t size, size_t align) {
  // Use posix_memalign, but mimic the behaviour of memalign.
  void* ptr = nullptr;
  int rc = posix_memalign(&ptr, align, size);
  return rc == 0 ? (errno = 0, ptr) : (errno = rc, nullptr);
}

inline void aligned_free(void* aligned_ptr) {
  free(aligned_ptr);
}

#elif defined(_WIN32)

inline void* aligned_malloc(size_t size, size_t align) {
  return _aligned_malloc(size, align);
}

inline void aligned_free(void* aligned_ptr) {
  _aligned_free(aligned_ptr);
}

#else

inline void* aligned_malloc(size_t size, size_t align) {
  return memalign(align, size);
}

inline void aligned_free(void* aligned_ptr) {
  free(aligned_ptr);
}

#endif

namespace detail {
template <typename Alloc, size_t kAlign, bool kAllocate>
void rawOverAlignedImpl(Alloc const& alloc, size_t n, void*& raw) {
  static_assert((kAlign & (kAlign - 1)) == 0, "Align must be a power of 2");

  using AllocTraits = std::allocator_traits<Alloc>;
  using T = typename AllocTraits::value_type;

  constexpr bool kCanBypass = std::is_same<Alloc, std::allocator<T>>::value;

  // BaseType is a type that gives us as much alignment as we need if
  // we can get it naturally, otherwise it is aligned as max_align_t.
  // kBaseAlign is both the alignment and size of this type.
  constexpr size_t kBaseAlign = constexpr_min(kAlign, alignof(max_align_t));
  using BaseType = folly::aligned_storage_t<kBaseAlign, kBaseAlign>;
  using BaseAllocTraits =
      typename AllocTraits::template rebind_traits<BaseType>;
  using BaseAlloc = typename BaseAllocTraits::allocator_type;
  static_assert(
      sizeof(BaseType) == kBaseAlign && alignof(BaseType) == kBaseAlign);

#if defined(__cpp_sized_deallocation)
  if (kCanBypass && kAlign == kBaseAlign) {
    // until std::allocator uses sized deallocation, it is worth the
    // effort to bypass it when we are able
    if (kAllocate) {
      raw = ::operator new(n * sizeof(T));
    } else {
      ::operator delete(raw, n * sizeof(T));
    }
    return;
  }
#endif

  if (kCanBypass && kAlign > kBaseAlign) {
    // allocating as BaseType isn't sufficient to get alignment, but
    // since we can bypass Alloc we can use something like posix_memalign.
    if (kAllocate) {
      raw = aligned_malloc(n * sizeof(T), kAlign);
    } else {
      aligned_free(raw);
    }
    return;
  }

  // we're not allowed to bypass Alloc, or we don't want to
  BaseAlloc a(alloc);

  // allocation size is counted in sizeof(BaseType)
  size_t quanta = (n * sizeof(T) + kBaseAlign - 1) / sizeof(BaseType);
  if (kAlign <= kBaseAlign) {
    // rebinding Alloc to BaseType is sufficient to get us the alignment
    // we want, happy path
    if (kAllocate) {
      raw = static_cast<void*>(
          std::addressof(*BaseAllocTraits::allocate(a, quanta)));
    } else {
      BaseAllocTraits::deallocate(
          a,
          std::pointer_traits<typename BaseAllocTraits::pointer>::pointer_to(
              *static_cast<BaseType*>(raw)),
          quanta);
    }
    return;
  }

  // Overaligned and custom allocator, our only option is to
  // overallocate and store a delta to the actual allocation just
  // before the returned ptr.
  //
  // If we give ourselves kAlign extra bytes, then since
  // sizeof(BaseType) divides kAlign we can meet alignment while
  // getting a prefix of one BaseType.  If we happen to get a
  // kAlign-aligned block, then we can return a pointer to underlying
  // + kAlign, otherwise there will be at least kBaseAlign bytes in
  // the unused prefix of the first kAlign-aligned block.
  if (kAllocate) {
    char* base = reinterpret_cast<char*>(std::addressof(
        *BaseAllocTraits::allocate(a, quanta + kAlign / sizeof(BaseType))));
    size_t byteDelta =
        kAlign - (reinterpret_cast<uintptr_t>(base) & (kAlign - 1));
    raw = static_cast<void*>(base + byteDelta);
    static_cast<size_t*>(raw)[-1] = byteDelta;
  } else {
    size_t byteDelta = static_cast<size_t*>(raw)[-1];
    char* base = static_cast<char*>(raw) - byteDelta;
    BaseAllocTraits::deallocate(
        a,
        std::pointer_traits<typename BaseAllocTraits::pointer>::pointer_to(
            *reinterpret_cast<BaseType*>(base)),
        quanta + kAlign / sizeof(BaseType));
  }
}
} // namespace detail

// Works like std::allocator_traits<Alloc>::allocate, but handles
// over-aligned types.  Feel free to manually specify any power of two as
// the Align template arg.  Must be matched with deallocateOverAligned.
// allocationBytesForOverAligned will give you the number of bytes that
// this function actually requests.
template <
    typename Alloc,
    size_t kAlign = alignof(typename std::allocator_traits<Alloc>::value_type)>
typename std::allocator_traits<Alloc>::pointer allocateOverAligned(
    Alloc const& alloc, size_t n) {
  void* raw = nullptr;
  detail::rawOverAlignedImpl<Alloc, kAlign, true>(alloc, n, raw);
  return std::pointer_traits<typename std::allocator_traits<Alloc>::pointer>::
      pointer_to(
          *static_cast<typename std::allocator_traits<Alloc>::value_type*>(
              raw));
}

template <
    typename Alloc,
    size_t kAlign = alignof(typename std::allocator_traits<Alloc>::value_type)>
void deallocateOverAligned(
    Alloc const& alloc,
    typename std::allocator_traits<Alloc>::pointer ptr,
    size_t n) {
  void* raw = static_cast<void*>(std::addressof(*ptr));
  detail::rawOverAlignedImpl<Alloc, kAlign, false>(alloc, n, raw);
}

template <
    typename Alloc,
    size_t kAlign = alignof(typename std::allocator_traits<Alloc>::value_type)>
size_t allocationBytesForOverAligned(size_t n) {
  static_assert((kAlign & (kAlign - 1)) == 0, "Align must be a power of 2");

  using AllocTraits = std::allocator_traits<Alloc>;
  using T = typename AllocTraits::value_type;

  constexpr size_t kBaseAlign = constexpr_min(kAlign, alignof(max_align_t));

  if (kAlign > kBaseAlign && std::is_same<Alloc, std::allocator<T>>::value) {
    return n * sizeof(T);
  } else {
    size_t quanta = (n * sizeof(T) + kBaseAlign - 1) / kBaseAlign;
    if (kAlign > kBaseAlign) {
      quanta += kAlign / kBaseAlign;
    }
    return quanta * kBaseAlign;
  }
}

/**
 * static_function_deleter
 *
 * So you can write this:
 *
 *      using RSA_deleter = folly::static_function_deleter<RSA, &RSA_free>;
 *      auto rsa = std::unique_ptr<RSA, RSA_deleter>(RSA_new());
 *      RSA_generate_key_ex(rsa.get(), bits, exponent, nullptr);
 *      rsa = nullptr;  // calls RSA_free(rsa.get())
 *
 * This would be sweet as well for BIO, but unfortunately BIO_free has signature
 * int(BIO*) while we require signature void(BIO*). So you would need to make a
 * wrapper for it:
 *
 *      inline void BIO_free_fb(BIO* bio) { CHECK_EQ(1, BIO_free(bio)); }
 *      using BIO_deleter = folly::static_function_deleter<BIO, &BIO_free_fb>;
 *      auto buf = std::unique_ptr<BIO, BIO_deleter>(BIO_new(BIO_s_mem()));
 *      buf = nullptr;  // calls BIO_free(buf.get())
 */

template <typename T, void (*f)(T*)>
struct static_function_deleter {
  void operator()(T* t) const { f(t); }
};

/**
 *  to_shared_ptr
 *
 *  Convert unique_ptr to shared_ptr without specifying the template type
 *  parameter and letting the compiler deduce it.
 *
 *  So you can write this:
 *
 *      auto sptr = to_shared_ptr(getSomethingUnique<T>());
 *
 *  Instead of this:
 *
 *      auto sptr = shared_ptr<T>(getSomethingUnique<T>());
 *
 *  Useful when `T` is long, such as:
 *
 *      using T = foobar::FooBarAsyncClient;
 */
template <typename T, typename D>
std::shared_ptr<T> to_shared_ptr(std::unique_ptr<T, D>&& ptr) {
  return std::shared_ptr<T>(std::move(ptr));
}

/**
 *  to_shared_ptr_aliasing
 */
template <typename T, typename U>
std::shared_ptr<U> to_shared_ptr_aliasing(std::shared_ptr<T> const& r, U* ptr) {
  return std::shared_ptr<U>(r, ptr);
}

template <typename T, typename U>
std::shared_ptr<U> to_shared_ptr_aliasing(std::shared_ptr<T>&& r, U* ptr) {
  return std::shared_ptr<U>(std::move(r), ptr);
}

/**
 *  to_shared_ptr_non_owning
 */
template <typename U>
std::shared_ptr<U> to_shared_ptr_non_owning(U* ptr) {
  return std::shared_ptr<U>(std::shared_ptr<void>{}, ptr);
}

/**
 *  to_weak_ptr
 *
 *  Make a weak_ptr and return it from a shared_ptr without specifying the
 *  template type parameter and letting the compiler deduce it.
 *
 *  So you can write this:
 *
 *      auto wptr = to_weak_ptr(getSomethingShared<T>());
 *
 *  Instead of this:
 *
 *      auto wptr = weak_ptr<T>(getSomethingShared<T>());
 *
 *  Useful when `T` is long, such as:
 *
 *      using T = foobar::FooBarAsyncClient;
 */
template <typename T>
std::weak_ptr<T> to_weak_ptr(const std::shared_ptr<T>& ptr) {
  return ptr;
}

#if defined(__GLIBCXX__)
namespace detail {
void weak_ptr_set_stored_ptr(std::weak_ptr<void>& w, void* ptr);

template <typename Tag, void* std::__weak_ptr<void>::* WeakPtr_Ptr_Field>
struct GenerateWeakPtrInternalsAccessor {
  friend void weak_ptr_set_stored_ptr(std::weak_ptr<void>& w, void* ptr) {
    w.*WeakPtr_Ptr_Field = ptr;
  }
};

// Each template instantiation of GenerateWeakPtrInternalsAccessor must
// be a new type, to avoid ODR problems.  We do this by tagging it with
// a type from an anon namespace.
namespace {
struct MemoryAnonTag {};
} // namespace

template struct GenerateWeakPtrInternalsAccessor<
    MemoryAnonTag,
    &std::__weak_ptr<void>::_M_ptr>;
} // namespace detail
#endif

/**
 *  to_weak_ptr_aliasing
 *
 *  Like to_weak_ptr, but arranges that lock().get() on the returned
 *  pointer points to ptr rather than r.get().
 *
 *  Equivalent to:
 *
 *      to_weak_ptr(std::shared_ptr<U>(r, ptr))
 *
 *  For libstdc++, ABI-specific tricks are used to optimize the
 *  implementation.
 */
template <typename T, typename U>
std::weak_ptr<U> to_weak_ptr_aliasing(const std::shared_ptr<T>& r, U* ptr) {
#if defined(__GLIBCXX__)
  std::weak_ptr<void> wv(r);
  detail::weak_ptr_set_stored_ptr(wv, ptr);
  FOLLY_PUSH_WARNING
  FOLLY_GCC_DISABLE_WARNING("-Wstrict-aliasing")
  return reinterpret_cast<std::weak_ptr<U>&&>(wv);
  FOLLY_POP_WARNING
#else
  return std::shared_ptr<U>(r, ptr);
#endif
}

/**
 * fmap_shared_ptr_aliasing
 *
 * This is a helper method that allows one to get aliased shared_ptr to an inner
 * object inside another shared_ptr. For example, if you have an object of type
 * T that contains field of type U, you can use this method to get a shared
 * pointer to the field U by calling `fmap_shared_ptr_aliasing(ptrToT, getU)`
 * where `getU` is a function that returns a pointer to that field.
 * @param getU a function that returns a const pointer to the field of type U by
 * taking a `const T*` as an argument.
 * @return a shared_ptr to the field U or nullptr if the owner is
 * nullptr or getU returns nullptr.
 */
template <
    typename T,
    typename GetU,
    typename U = std::remove_pointer_t<std::invoke_result_t<GetU&, const T*>>>
std::shared_ptr<U> fmap_shared_ptr_aliasing(
    const std::shared_ptr<T>& owner, GetU getU) {
  if (auto* tPtr = owner.get()) {
    if (auto* uPtr = getU(tPtr)) {
      return to_shared_ptr_aliasing(owner, uPtr);
    }
  }
  return nullptr;
}

template <
    typename T,
    typename GetU,
    typename U = std::remove_pointer_t<std::invoke_result_t<GetU&, const T*>>>
std::shared_ptr<U> fmap_shared_ptr_aliasing(
    std::shared_ptr<T>&& owner, GetU getU) {
  if (auto* tPtr = owner.get()) {
    if (auto* uPtr = getU(tPtr)) {
      return to_shared_ptr_aliasing(owner, uPtr);
    }
  }
  return nullptr;
}

template <typename GetU>
auto fmap_shared_ptr_aliasing(std::nullptr_t owner, GetU&& getU) = delete;

/**
 *  copy_to_unique_ptr
 *
 *  Move or copy the argument to the heap and return it owned by a unique_ptr.
 *
 *  Like std::make_unique, but deduces the type of the owned object.
 */
template <typename T>
std::unique_ptr<std::remove_cvref_t<T>> copy_to_unique_ptr(T&& t) {
  return std::make_unique<std::remove_cvref_t<T>>(static_cast<T&&>(t));
}

/**
 *  copy_to_shared_ptr
 *
 *  Move or copy the argument to the heap and return it owned by a shared_ptr.
 *
 *  Like make_shared, but deduces the type of the owned object.
 */
template <typename T>
std::shared_ptr<std::remove_cvref_t<T>> copy_to_shared_ptr(T&& t) {
  return std::make_shared<std::remove_cvref_t<T>>(static_cast<T&&>(t));
}

/**
 *  copy_to_shared_ptr_const
 *
 *  Move or copy the argument to the heap and return it owned by a
 *  shared_ptr<T const>.
 *
 *  Like copy_to_shared_ptr, but returns a shared_ptr to const.
 */
template <typename T>
std::shared_ptr<std::remove_cvref_t<T> const> copy_to_shared_ptr_const(T&& t) {
  return std::make_shared<std::remove_cvref_t<T> const>(static_cast<T&&>(t));
}

/**
 *  copy_through_unique_ptr
 *
 *  If the argument is nonnull, allocates a copy of its pointee.
 */
template <typename T>
std::unique_ptr<T> copy_through_unique_ptr(const std::unique_ptr<T>& t) {
  static_assert(
      !std::is_polymorphic<T>::value || std::is_final<T>::value,
      "possibly slicing");
  return t ? std::make_unique<T>(*t) : nullptr;
}

/**
 *  copy_through_shared_ptr
 *
 *  If the argument is nonnull, allocates a copy of its pointee.
 */
template <typename T>
std::shared_ptr<T> copy_through_shared_ptr(const std::shared_ptr<T>& t) {
  static_assert(
      !std::is_polymorphic<T>::value || std::is_final<T>::value,
      "possibly slicing");
  return t ? std::make_shared<T>(*t) : nullptr;
}

//  erased_unique_ptr
//
//  A type-erased smart-ptr with unique ownership to a heap-allocated object.
using erased_unique_ptr = std::unique_ptr<void, void (*)(void*)>;

namespace detail {
// for erased_unique_ptr with types that specialize default_delete
template <typename T>
void erased_unique_ptr_delete(void* ptr) {
  std::default_delete<T>()(static_cast<T*>(ptr));
}
} // namespace detail

//  to_erased_unique_ptr
//
//  Converts an owning pointer to an object to an erased_unique_ptr.
template <typename T>
erased_unique_ptr to_erased_unique_ptr(T* const ptr) noexcept {
  return {ptr, detail::erased_unique_ptr_delete<T>};
}

//  to_erased_unique_ptr
//
//  Converts an owning std::unique_ptr to an erased_unique_ptr.
template <typename T>
erased_unique_ptr to_erased_unique_ptr(std::unique_ptr<T> ptr) noexcept {
  return to_erased_unique_ptr(ptr.release());
}

//  make_erased_unique
//
//  Allocate an object of the T on the heap, constructed with a..., and return
//  an owning erased_unique_ptr to it.
template <typename T, typename... A>
erased_unique_ptr make_erased_unique(A&&... a) {
  return to_erased_unique_ptr(std::make_unique<T>(static_cast<A&&>(a)...));
}

//  copy_to_erased_unique_ptr
//
//  Copy an object to the heap and return an owning erased_unique_ptr to it.
template <typename T>
erased_unique_ptr copy_to_erased_unique_ptr(T&& obj) {
  return to_erased_unique_ptr(copy_to_unique_ptr(static_cast<T&&>(obj)));
}

//  empty_erased_unique_ptr
//
//  Return an empty erased_unique_ptr.
inline erased_unique_ptr empty_erased_unique_ptr() {
  return {nullptr, nullptr};
}

/*
 * allocator_delete
 *
 * A deleter which automatically works with a given allocator.
 *
 * Derives from the allocator to take advantage of the empty base
 * optimization when possible.
 */
template <typename Alloc>
class allocator_delete : private std::remove_reference<Alloc>::type {
 private:
  using allocator_type = typename std::remove_reference<Alloc>::type;
  using allocator_traits = std::allocator_traits<allocator_type>;
  using value_type = typename allocator_traits::value_type;
  using pointer = typename allocator_traits::pointer;

 public:
  allocator_delete() = default;
  allocator_delete(allocator_delete const&) = default;
  allocator_delete(allocator_delete&&) = default;
  allocator_delete& operator=(allocator_delete const&) = default;
  allocator_delete& operator=(allocator_delete&&) = default;

  explicit allocator_delete(const allocator_type& alloc)
      : allocator_type(alloc) {}

  explicit allocator_delete(allocator_type&& alloc)
      : allocator_type(std::move(alloc)) {}

  template <typename U>
  allocator_delete(const allocator_delete<U>& other)
      : allocator_type(other.get_allocator()) {}

  allocator_type const& get_allocator() const { return *this; }

  void operator()(pointer p) const {
    auto alloc = get_allocator();
    allocator_traits::destroy(alloc, p);
    allocator_traits::deallocate(alloc, p, 1);
  }
};

/**
 * allocate_unique, like std::allocate_shared but for std::unique_ptr
 */
template <typename T, typename Alloc, typename... Args>
std::unique_ptr<
    T,
    allocator_delete<
        typename std::allocator_traits<Alloc>::template rebind_alloc<T>>>
allocate_unique(Alloc const& alloc, Args&&... args) {
  using TAlloc =
      typename std::allocator_traits<Alloc>::template rebind_alloc<T>;

  using traits = std::allocator_traits<TAlloc>;
  struct DeferCondDeallocate {
    bool& cond;
    TAlloc& copy;
    T* p;
    ~DeferCondDeallocate() {
      if (FOLLY_UNLIKELY(!cond)) {
        traits::deallocate(copy, p, 1);
      }
    }
  };
  auto copy = TAlloc(alloc);
  auto const p = traits::allocate(copy, 1);
  {
    bool constructed = false;
    DeferCondDeallocate handler{constructed, copy, p};
    traits::construct(copy, p, static_cast<Args&&>(args)...);
    constructed = true;
  }
  return {p, allocator_delete<TAlloc>(std::move(copy))};
}

struct SysBufferDeleter {
  void operator()(void* ptr) { std::free(ptr); }
};
using SysBufferUniquePtr = std::unique_ptr<void, SysBufferDeleter>;

inline SysBufferUniquePtr allocate_sys_buffer(std::size_t size) {
  auto p = std::malloc(size);
  if (!p) {
    throw_exception<std::bad_alloc>();
  }
  return {p, {}};
}

} // namespace folly
