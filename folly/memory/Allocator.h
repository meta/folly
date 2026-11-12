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
#include <memory>
#include <type_traits>

#include <folly/Likely.h>
#include <folly/Memory.h>
#include <folly/Traits.h>
#include <folly/functional/Invoke.h>
#include <folly/lang/Exception.h>
#include <folly/memory/Malloc.h>

namespace folly {

/**
 * SysAllocator
 *
 * Resembles std::allocator, the default Allocator, but wraps std::malloc and
 * std::free.
 */
template <typename T>
class SysAllocator {
 private:
  using Self = SysAllocator<T>;

 public:
  using value_type = T;

  constexpr SysAllocator() = default;

  constexpr SysAllocator(SysAllocator const&) = default;

  template <typename U, std::enable_if_t<!std::is_same<U, T>::value, int> = 0>
  constexpr SysAllocator(SysAllocator<U> const&) noexcept {}

  T* allocate(size_t count) {
    auto const p = std::malloc(sizeof(T) * count);
    if (!p) {
      throw_exception<std::bad_alloc>();
    }
    return static_cast<T*>(p);
  }
  void deallocate(T* p, size_t count) { sizedFree(p, count * sizeof(T)); }

  friend bool operator==(Self const&, Self const&) noexcept { return true; }
};

class DefaultAlign {
 private:
  using Self = DefaultAlign;
  std::size_t align_;

 public:
  explicit DefaultAlign(std::size_t align) noexcept : align_(align) {
    assert(!(align_ < sizeof(void*)) && bool("bad align: too small"));
    assert(!(align_ & (align_ - 1)) && bool("bad align: not power-of-two"));
  }
  std::size_t operator()() const noexcept { return align_; }

  friend bool operator==(Self const& a, Self const& b) noexcept {
    return a.align_ == b.align_;
  }
};

template <std::size_t Align>
class FixedAlign {
 private:
  static_assert(!(Align < sizeof(void*)), "bad align: too small");
  static_assert(!(Align & (Align - 1)), "bad align: not power-of-two");
  using Self = FixedAlign<Align>;

 public:
  constexpr std::size_t operator()() const noexcept { return Align; }

  friend bool operator==(Self const&, Self const&) noexcept { return true; }
};

/**
 * AlignedSysAllocator
 *
 * Resembles std::allocator, the default Allocator, but wraps aligned_malloc and
 * aligned_free.
 *
 * Accepts a policy parameter for providing the alignment, which must:
 *   * be invocable as std::size_t(std::size_t) noexcept
 *     * taking the type alignment and returning the allocation alignment
 *   * be noexcept-copy-constructible
 *   * have noexcept operator==
 *   * not be final
 *
 * DefaultAlign and FixedAlign<std::size_t>, provided above, are valid policies.
 */
template <typename T, typename Align = DefaultAlign>
class AlignedSysAllocator : private Align {
 private:
  using Self = AlignedSysAllocator<T, Align>;

  template <typename, typename>
  friend class AlignedSysAllocator;

  constexpr Align const& align() const { return *this; }

 public:
  static_assert(std::is_nothrow_copy_constructible<Align>::value);
  static_assert(is_nothrow_invocable_r_v<std::size_t, Align>);

  using value_type = T;

  using propagate_on_container_copy_assignment = std::true_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;

  using Align::Align;

  // TODO: remove this ctor, which is is no longer required as of under gcc7
  template <
      typename S = Align,
      std::enable_if_t<std::is_default_constructible<S>::value, int> = 0>
  constexpr AlignedSysAllocator() noexcept(noexcept(Align())) : Align() {}

  constexpr AlignedSysAllocator(AlignedSysAllocator const&) = default;

  template <typename U, std::enable_if_t<!std::is_same<U, T>::value, int> = 0>
  constexpr AlignedSysAllocator(
      AlignedSysAllocator<U, Align> const& other) noexcept
      : Align(other.align()) {}

  T* allocate(size_t count) {
    auto const a = align()() < alignof(T) ? alignof(T) : align()();
    auto const p = aligned_malloc(sizeof(T) * count, a);
    if (!p) {
      if (errno != ENOMEM) [[unlikely]] {
        std::terminate();
      }
      throw_exception<std::bad_alloc>();
    }
    return static_cast<T*>(p);
  }
  void deallocate(T* p, size_t /* count */) { aligned_free(p); }

  friend bool operator==(Self const& a, Self const& b) noexcept {
    return a.align() == b.align();
  }
};

/**
 * CxxAllocatorAdaptor
 *
 * A type conforming to C++ concept Allocator, delegating operations to an
 * unowned Inner which has this required interface:
 *
 *   void* allocate(std::size_t)
 *   void deallocate(void*, std::size_t)
 *
 * Note that Inner is *not* a C++ Allocator.
 */
template <typename T, class Inner, bool FallbackToStdAlloc = false>
class CxxAllocatorAdaptor : private std::allocator<T> {
 private:
  using Self = CxxAllocatorAdaptor<T, Inner, FallbackToStdAlloc>;

  template <typename U, typename UInner, bool UFallback>
  friend class CxxAllocatorAdaptor;

  Inner* inner_ = nullptr;

 public:
  using value_type = T;

  using propagate_on_container_copy_assignment = std::true_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;

  template <bool X = FallbackToStdAlloc, std::enable_if_t<X, int> = 0>
  constexpr explicit CxxAllocatorAdaptor() {}

  constexpr explicit CxxAllocatorAdaptor(Inner& ref) : inner_(&ref) {}

  constexpr CxxAllocatorAdaptor(CxxAllocatorAdaptor const&) = default;

  template <typename U, std::enable_if_t<!std::is_same<U, T>::value, int> = 0>
  constexpr CxxAllocatorAdaptor(
      CxxAllocatorAdaptor<U, Inner, FallbackToStdAlloc> const& other)
      : inner_(other.inner_) {}

  CxxAllocatorAdaptor& operator=(CxxAllocatorAdaptor const& other) = default;

  template <typename U, std::enable_if_t<!std::is_same<U, T>::value, int> = 0>
  CxxAllocatorAdaptor& operator=(
      CxxAllocatorAdaptor<U, Inner, FallbackToStdAlloc> const& other) noexcept {
    inner_ = other.inner_;
    return *this;
  }

  T* allocate(std::size_t n) {
    if (FallbackToStdAlloc && inner_ == nullptr) {
      return std::allocator<T>::allocate(n);
    }
    return static_cast<T*>(inner_->allocate(sizeof(T) * n));
  }

  void deallocate(T* p, std::size_t n) {
    if (inner_ != nullptr) {
      inner_->deallocate(p, sizeof(T) * n);
    } else {
      assert(FallbackToStdAlloc);
      std::allocator<T>::deallocate(p, n);
    }
  }

  friend bool operator==(Self const& a, Self const& b) noexcept {
    return a.inner_ == b.inner_;
  }

  template <typename U>
  struct rebind {
    using other = CxxAllocatorAdaptor<U, Inner, FallbackToStdAlloc>;
  };
};

/**
 * AllocatorHasTrivialDeallocate
 *
 * Unambiguously inherits std::integral_constant<bool, V> for some bool V.
 *
 * Describes whether a C++ Aallocator has trivial, i.e. no-op, deallocate().
 *
 * Also may be used to describe types which may be used with
 * CxxAllocatorAdaptor.
 */
template <typename Alloc>
struct AllocatorHasTrivialDeallocate : std::false_type {};

template <typename T, class Alloc>
struct AllocatorHasTrivialDeallocate<CxxAllocatorAdaptor<T, Alloc>>
    : AllocatorHasTrivialDeallocate<Alloc> {};

namespace detail {
// note that construct and destroy here are methods, not short names for
// the constructor and destructor
FOLLY_CREATE_MEMBER_INVOKER(AllocatorConstruct_, construct);
FOLLY_CREATE_MEMBER_INVOKER(AllocatorDestroy_, destroy);

template <typename Alloc, typename... Args>
struct AllocatorCustomizesConstruct_
    : folly::is_invocable<AllocatorConstruct_, Alloc, Args...> {};

template <typename Alloc, typename... Args>
  requires requires { typename Alloc::folly_has_default_object_construct; }
struct AllocatorCustomizesConstruct_<Alloc, Args...>
    : std::negation<typename Alloc::folly_has_default_object_construct> {};

template <typename Alloc, typename... Args>
struct AllocatorCustomizesDestroy_
    : folly::is_invocable<AllocatorDestroy_, Alloc, Args...> {};

template <typename Alloc, typename... Args>
  requires requires { typename Alloc::folly_has_default_object_destroy; }
struct AllocatorCustomizesDestroy_<Alloc, Args...>
    : std::negation<typename Alloc::folly_has_default_object_destroy> {};
} // namespace detail

/**
 * AllocatorHasDefaultObjectConstruct
 *
 * AllocatorHasDefaultObjectConstruct<A, T, Args...> unambiguously
 * inherits std::integral_constant<bool, V>, where V will be true iff
 * the effect of std::allocator_traits<A>::construct(a, p, args...) is
 * the same as new (static_cast<void*>(p)) T(args...).  If true then
 * any optimizations applicable to object construction (relying on
 * std::is_trivially_copyable<T>, for example) can be applied to objects
 * in an allocator-aware container using an allocation of type A.
 *
 * Allocator types can override V by declaring a type alias for
 * folly_has_default_object_construct.  It is helpful to do this if you
 * define a custom allocator type that defines a construct method, but
 * that method doesn't do anything except call placement new.
 */
template <typename Alloc, typename T, typename... Args>
struct AllocatorHasDefaultObjectConstruct
    : std::negation<detail::AllocatorCustomizesConstruct_<Alloc, T*, Args...>> {
};

template <typename Value, typename T, typename... Args>
struct AllocatorHasDefaultObjectConstruct<std::allocator<Value>, T, Args...>
    : std::true_type {};

/**
 * AllocatorHasDefaultObjectDestroy
 *
 * AllocatorHasDefaultObjectDestroy<A, T> unambiguously inherits
 * std::integral_constant<bool, V>, where V will be true iff the effect
 * of std::allocator_traits<A>::destroy(a, p) is the same as p->~T().
 * If true then optimizations applicable to object destruction (relying
 * on std::is_trivially_destructible<T>, for example) can be applied to
 * objects in an allocator-aware container using an allocator of type A.
 *
 * Allocator types can override V by declaring a type alias for
 * folly_has_default_object_destroy.  It is helpful to do this if you
 * define a custom allocator type that defines a destroy method, but that
 * method doesn't do anything except call the object's destructor.
 */
template <typename Alloc, typename T>
struct AllocatorHasDefaultObjectDestroy
    : std::negation<detail::AllocatorCustomizesDestroy_<Alloc, T*>> {};

template <typename Value, typename T>
struct AllocatorHasDefaultObjectDestroy<std::allocator<Value>, T>
    : std::true_type {};

} // namespace folly
