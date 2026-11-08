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

/*
 * For high-level documentation and usage examples see
 * folly/docs/small_vector.md
 */

#pragma once

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <folly/ConstexprMath.h>
#include <folly/CppAttributes.h>
#include <folly/Likely.h>
#include <folly/Portability.h>
#include <folly/ScopeGuard.h>
#include <folly/Traits.h>
#include <folly/functional/Invoke.h>
#include <folly/hash/Hash.h>
#include <folly/lang/Align.h>
#include <folly/lang/Assume.h>
#include <folly/lang/CheckedMath.h>
#include <folly/lang/Exception.h>
#include <folly/memory/Malloc.h>
#include <folly/memory/SanitizeLeak.h>
#include <folly/portability/Malloc.h>

#if (FOLLY_X64 || FOLLY_PPC64 || FOLLY_AARCH64 || FOLLY_RISCV64)
#define FOLLY_SV_PACK_ATTR FOLLY_PACK_ATTR
#define FOLLY_SV_PACK_PUSH FOLLY_PACK_PUSH
#define FOLLY_SV_PACK_POP FOLLY_PACK_POP
#else
#define FOLLY_SV_PACK_ATTR
#define FOLLY_SV_PACK_PUSH
#define FOLLY_SV_PACK_POP
#endif

// Ignore shadowing warnings within this file, so includers can use -Wshadow.
FOLLY_PUSH_WARNING
FOLLY_GNU_DISABLE_WARNING("-Wshadow")

namespace folly {

namespace small_vector_policy {

namespace detail {

struct item_size_type {
  template <typename T>
  using get = typename T::size_type;
  template <typename T>
  struct set {
    using size_type = T;
  };
};

struct item_in_situ_only {
  template <typename T>
  using get = typename T::in_situ_only;
  template <typename T>
  struct set {
    using in_situ_only = T;
  };
};

template <template <typename> class F, typename... T>
constexpr size_t last_matching_() {
  bool const values[] = {is_detected_v<F, T>..., false};
  for (size_t i = 0; i < sizeof...(T); ++i) {
    auto const j = sizeof...(T) - 1 - i;
    if (values[j]) {
      return j;
    }
  }
  return sizeof...(T);
}

template <size_t M, typename I, typename... P>
struct merge_ //
    : I::template set<typename I::template get<
          type_pack_element_t<sizeof...(P) - M, P...>>> {};
template <typename I, typename... P>
struct merge_<0, I, P...> {};
template <typename I, typename... P>
using merge =
    merge_<sizeof...(P) - last_matching_<I::template get, P...>(), I, P...>;

} // namespace detail

template <typename... Policy>
struct merge //
    : detail::merge<detail::item_size_type, Policy...>,
      detail::merge<detail::item_in_situ_only, Policy...> {};

template <typename SizeType>
struct policy_size_type {
  using size_type = SizeType;
};

template <bool Value>
struct policy_in_situ_only {
  using in_situ_only = std::bool_constant<Value>;
};

} // namespace small_vector_policy

//////////////////////////////////////////////////////////////////////

template <class T, std::size_t M, class P>
class small_vector;

//////////////////////////////////////////////////////////////////////

namespace detail {

namespace small_vector_detail {

/*
 * Just because a type is trivially copyable doesn't mean we should copy
 * it. A copy constructor that is deleted is still considered trivial.
 *
 * If a type is not move constructible, it shouldn't be relocated. We
 * should avoid resizing in general.
 */
template <typename T>
inline constexpr bool should_trivially_copy =
    std::is_trivially_copyable_v<T> && std::is_move_constructible_v<T>;

/*
 * Move objects in memory to the right into some uninitialized memory, where
 * the region overlaps. Then call create() for each hole in reverse order.
 *
 * This doesn't just use std::move_backward because move_backward only works
 * if all the memory is initialized to type T already.
 *
 * The create function should return a reference type, to avoid
 * extra copies and moves for non-trivial types.
 */
template <class T, class Create>
  requires(!should_trivially_copy<T>)
void moveObjectsRightAndCreate(
    T* const first,
    T* const lastConstructed,
    T* const realLast,
    Create&& create) {
  if (lastConstructed == realLast) {
    return;
  }

  T* out = realLast;
  T* in = lastConstructed;
  {
    auto rollback = makeGuard([&] {
      // We want to make sure the same stuff is uninitialized memory
      // if we exit via an exception (this is to make sure we provide
      // the basic exception safety guarantee for insert functions).
      if (out < lastConstructed) {
        out = lastConstructed - 1;
      }
      std::destroy(out + 1, realLast);
    });
    // Decrement the pointers only when it is known that the resulting pointer
    // is within the boundaries of the object. Decrementing past the beginning
    // of the object is UB. Note that this is asymmetric wrt forward iteration,
    // as past-the-end pointers are explicitly allowed.
    for (; in != first && out > lastConstructed;) {
      // Out must be decremented before an exception can be thrown so that
      // the rollback guard knows where to start.
      --out;
      new (out) T(std::move(*(--in)));
    }
    for (; in != first;) {
      --out;
      *out = std::move(*(--in));
    }
    for (; out > lastConstructed;) {
      --out;
      new (out) T(create());
    }
    for (; out != first;) {
      --out;
      *out = create();
    }
    rollback.dismiss();
  }
}

// Specialization for trivially copyable types.  The call to
// std::move_backward here will just turn into a memmove.
// This must only be used with trivially copyable types because some of the
// memory may be uninitialized, and std::move_backward() won't work when it
// can't memmove().
template <class T, class Create>
  requires(should_trivially_copy<T>)
void moveObjectsRightAndCreate(
    T* const first,
    T* const lastConstructed,
    T* const realLast,
    Create&& create) {
  std::move_backward(first, lastConstructed, realLast);
  T* const end = first - 1;
  T* out = first + (realLast - lastConstructed) - 1;
  for (; out != end; --out) {
    *out = create();
  }
}

/*
 * Populate a region of memory using `op' to construct elements.  If
 * anything throws, undo what we did.
 */
template <class T, class Function>
void populateMemForward(T* mem, std::size_t n, Function const& op) {
  std::size_t idx = 0;
  {
    auto rollback = makeGuard([&] { std::destroy_n(mem, idx); });
    for (size_t i = 0; i < n; ++i) {
      op(&mem[idx]);
      ++idx;
    }
    rollback.dismiss();
  }
}

/*
 * Copies `fromSize` elements from `from' to `to', where `to' is only
 * initialized up to `toSize`, but has enough storage for `fromSize'. If
 * `toSize' > `fromSize', the extra elements are destructed.
 */
template <class Iterator1, class Iterator2>
void partiallyUninitializedCopy(
    Iterator1 from, size_t fromSize, Iterator2 to, size_t toSize) {
  const size_t minSize = std::min(fromSize, toSize);
  std::copy(from, from + minSize, to);
  if (fromSize > toSize) {
    std::uninitialized_copy(from + minSize, from + fromSize, to + minSize);
  } else {
    std::destroy(to + minSize, to + toSize);
  }
}

} // namespace small_vector_detail

template <class SizeType, bool ShouldUseHeap, bool AlwaysUseHeap>
struct IntegralSizePolicyBase {
  using InternalSizeType = SizeType;

  IntegralSizePolicyBase() : size_(0) {}

 protected:
  static constexpr std::size_t policyMaxSize() {
    return AlwaysUseHeap
        ? std::numeric_limits<SizeType>::max()
        : SizeType(~kClearMask);
  }

  std::size_t doSize() const {
    return AlwaysUseHeap ? size_ : size_ & ~kClearMask;
  }

  std::size_t isExtern() const { return AlwaysUseHeap || kExternMask & size_; }

  void setExtern(bool b) {
    if (AlwaysUseHeap) {
      return;
    }
    if (b) {
      size_ |= kExternMask;
    } else {
      size_ &= ~kExternMask;
    }
  }

  void setSize(std::size_t sz) {
    assert(sz <= policyMaxSize());
    size_ = AlwaysUseHeap ? sz : (kClearMask & size_) | SizeType(sz);
  }

  void incrementSize(std::size_t n) {
    // We can safely increment size without overflowing into mask bits because
    // we always check new size is less than maxPolicySize (see
    // makeSizeInternal). To be sure, added assertion to verify it.
    assert(doSize() + n <= policyMaxSize());
    size_ += SizeType(n);
  }
  std::size_t getInternalSize() { return size_; }

  void swapSizePolicy(IntegralSizePolicyBase& o) { std::swap(size_, o.size_); }

  void resetSizePolicy() { size_ = 0; }

 protected:
  static bool constexpr kShouldUseHeap = ShouldUseHeap || AlwaysUseHeap;
  static bool constexpr kAlwaysUseHeap = AlwaysUseHeap;

 private:
  // We reserve the most significant bit of size_ for the extern flag.
  static SizeType constexpr kExternMask =
      kShouldUseHeap ? SizeType(1) << (sizeof(SizeType) * 8 - 1) : 0;

  static SizeType constexpr kClearMask = kExternMask;

  SizeType size_;
};

template <class SizeType, bool ShouldUseHeap, bool AlwaysUseHeap>
struct IntegralSizePolicy;

template <class SizeType, bool AlwaysUseHeap>
struct IntegralSizePolicy<SizeType, true, AlwaysUseHeap>
    : public IntegralSizePolicyBase<SizeType, true, AlwaysUseHeap> {
 public:
  /*
   * Move a range to a range of uninitialized memory.  Assumes the
   * ranges don't overlap.
   */
  template <class T>
    requires(!detail::small_vector_detail::should_trivially_copy<T>)
  void moveToUninitialized(T* first, T* last, T* out) {
    std::size_t idx = 0;
    {
      auto rollback = makeGuard([&] {
        // Even for callers trying to give the strong guarantee
        // (e.g. push_back) it's ok to assume here that we don't have to
        // move things back and that it was a copy constructor that
        // threw: if someone throws from a move constructor the effects
        // are unspecified.
        std::destroy_n(out, idx);
      });
      for (; first != last; ++first, ++idx) {
        new (&out[idx]) T(std::move(*first));
      }
      rollback.dismiss();
    }
  }

  // Specialization for trivially copyable types.
  template <class T>
    requires(detail::small_vector_detail::should_trivially_copy<T>)
  void moveToUninitialized(T* first, T* last, T* out) {
    std::memmove(
        static_cast<void*>(out),
        static_cast<void const*>(first),
        (last - first) * sizeof *first);
  }

  /*
   * Move a range to a range of uninitialized memory. Assumes the
   * ranges don't overlap. Inserts an element at out + pos using
   * emplaceFunc(). out will contain (end - begin) + 1 elements on success and
   * none on failure. If emplaceFunc() throws [begin, end) is unmodified.
   */
  template <class T, class EmplaceFunc>
  void moveToUninitializedEmplace(
      T* begin, T* end, T* out, SizeType pos, EmplaceFunc&& emplaceFunc) {
    // Must be called first so that if it throws [begin, end) is unmodified.
    // We have to support the strong exception guarantee for emplace_back().
    emplaceFunc(out + pos);
    // move old elements to the left of the new one
    FOLLY_PUSH_WARNING
    FOLLY_MSVC_DISABLE_WARNING(4702) {
      auto rollback = makeGuard([&] { //
        std::destroy_at(out + pos);
      });
      if (begin) {
        this->moveToUninitialized(begin, begin + pos, out);
      }
      rollback.dismiss();
    }
    // move old elements to the right of the new one
    {
      auto rollback = makeGuard([&] { std::destroy_n(out, pos + 1); });
      if (begin + pos < end) {
        this->moveToUninitialized(begin + pos, end, out + pos + 1);
      }
      rollback.dismiss();
    }
    FOLLY_POP_WARNING
  }
};

template <class SizeType, bool AlwaysUseHeap>
struct IntegralSizePolicy<SizeType, false, AlwaysUseHeap>
    : public IntegralSizePolicyBase<SizeType, false, AlwaysUseHeap> {
 public:
  template <class T>
  void moveToUninitialized(T* /*first*/, T* /*last*/, T* /*out*/) {
    assume_unreachable();
  }
  template <class T, class EmplaceFunc>
  void moveToUninitializedEmplace(
      T* /* begin */,
      T* /* end */,
      T* /* out */,
      SizeType /* pos */,
      EmplaceFunc&& /* emplaceFunc */) {
    assume_unreachable();
  }
};

/*
 * If you're just trying to use this class, ignore everything about
 * this next small_vector_base class thing.
 *
 * The purpose of this junk is to minimize sizeof(small_vector<>)
 * and allow specifying the template parameters in whatever order is
 * convenient for the user.  There's a few extra steps here to try
 * to keep the error messages at least semi-reasonable.
 *
 * Apologies for all the black magic.
 */
template <class Value, std::size_t RequestedMaxInline, class InPolicy>
struct small_vector_base {
  static_assert(!std::is_integral<InPolicy>::value, "legacy");
  using Policy = small_vector_policy::merge<
      small_vector_policy::policy_size_type<size_t>,
      small_vector_policy::policy_in_situ_only<false>,
      conditional_t<std::is_void<InPolicy>::value, tag_t<>, InPolicy>>;

  /*
   * Make the real policy base classes.
   */
  using ActualSizePolicy = IntegralSizePolicy<
      typename Policy::size_type,
      !Policy::in_situ_only::value,
      RequestedMaxInline == 0>;

  using type = ActualSizePolicy;
};

namespace small_vector_detail {

inline void* unshiftPointer(void* p, size_t sizeBytes) {
  return static_cast<char*>(p) - sizeBytes;
}

} // namespace small_vector_detail

namespace small_vector_detail {

inline void* shiftPointer(void* p, size_t sizeBytes) {
  return static_cast<char*>(p) + sizeBytes;
}

} // namespace small_vector_detail

// No backward compatibility using declarations needed
} // namespace detail

//////////////////////////////////////////////////////////////////////
template <class Value, std::size_t RequestedMaxInline = 1, class Policy = void>
class small_vector
    : public detail::small_vector_base<Value, RequestedMaxInline, Policy>::
          type {
  using BaseType = typename detail::
      small_vector_base<Value, RequestedMaxInline, Policy>::type;
  using InternalSizeType = typename BaseType::InternalSizeType;

  /*
   * Figure out the max number of elements we should inline.  (If
   * the user asks for less inlined elements than we can fit unioned
   * into our value_type*, we will inline more than they asked.)
   */
  static constexpr auto kSizeOfValuePtr = sizeof(Value*);
  static constexpr auto kSizeOfValue = sizeof(Value);
  static constexpr std::size_t MaxInline{
      RequestedMaxInline == 0
          ? 0
          : constexpr_max(kSizeOfValuePtr / kSizeOfValue, RequestedMaxInline)};

 public:
  using size_type = std::size_t;
  using value_type = Value;
  using allocator_type = std::allocator<Value>;
  using reference = value_type&;
  using const_reference = value_type const&;
  using iterator = value_type*;
  using pointer = value_type*;
  using const_iterator = value_type const*;
  using const_pointer = value_type const*;
  using difference_type = std::ptrdiff_t;

  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  small_vector() = default;
  // Allocator is unused here. It is taken in for compatibility with std::vector
  // interface, but it will be ignored.
  small_vector(const std::allocator<Value>&) {}

  small_vector(small_vector const& o) {
    if constexpr (kShouldCopyWholeInlineStorageTrivial) {
      if (!o.isExtern()) {
        copyWholeInlineStorageTrivial(o);
        return;
      }
    }

    auto n = o.size();
    makeSize</* IgnoreExistingData */ true>(n);
    {
      auto rollback = makeGuard([&] { freeHeap(); });
      std::uninitialized_copy(o.begin(), o.begin() + n, begin());
      rollback.dismiss();
    }
    this->setSize(n);
  }

  small_vector(small_vector&& o) noexcept(
      std::is_nothrow_move_constructible<Value>::value) {
    if (o.isExtern()) {
      this->u.pdata_.heap_ = o.u.pdata_.heap_;
      o.u.pdata_.heap_ = nullptr;
      this->swapSizePolicy(o);
      if (kHasInlineCapacity) {
        this->u.setCapacity(o.u.getCapacity());
      }
    } else {
      if constexpr (kShouldCopyWholeInlineStorageTrivial) {
        copyWholeInlineStorageTrivial(o);
        o.resetSizePolicy();
      } else if constexpr (IsRelocatable<Value>::value) {
        moveInlineStorageRelocatable(std::move(o));
      } else {
        auto n = o.size();
        std::uninitialized_copy(
            std::make_move_iterator(o.begin()),
            std::make_move_iterator(o.end()),
            begin());
        this->setSize(n);
        o.clear();
      }
    }
  }

  small_vector(std::initializer_list<value_type> il) {
    constructImpl(il.begin(), il.end(), std::false_type());
  }

  explicit small_vector(size_type n) {
    FOLLY_PUSH_WARNING
    FOLLY_GCC_DISABLE_WARNING("-Warray-bounds")
    doConstruct(n, [&](void* p) { new (p) value_type(); });
    FOLLY_POP_WARNING
  }

  small_vector(size_type n, value_type const& t) {
    FOLLY_PUSH_WARNING
    FOLLY_GCC_DISABLE_WARNING("-Warray-bounds")
    doConstruct(n, [&](void* p) { new (p) value_type(t); });
    FOLLY_POP_WARNING
  }

  template <class Arg>
  explicit small_vector(Arg arg1, Arg arg2) {
    // Forward using std::is_arithmetic to get to the proper
    // implementation; this disambiguates between the iterators and
    // (size_t, value_type) meaning for this constructor.
    constructImpl(arg1, arg2, std::is_arithmetic<Arg>());
  }

  ~small_vector() { destroy(); }

  small_vector& operator=(small_vector const& o) {
    if (this != &o) [[likely]] {
      if constexpr (kShouldCopyWholeInlineStorageTrivial) {
        if (!this->isExtern() && !o.isExtern()) {
          copyWholeInlineStorageTrivial(o);
          return *this;
        }
      }
      if (o.size() < capacity()) {
        const size_t oSize = o.size();
        detail::small_vector_detail::partiallyUninitializedCopy(
            o.begin(), oSize, begin(), size());
        this->setSize(oSize);
      } else {
        assign(o.begin(), o.end());
      }
    }
    return *this;
  }

  small_vector& operator=(small_vector&& o) noexcept(
      std::is_nothrow_move_constructible<Value>::value) {
    if (this != &o) [[likely]] {
      // If either is external, reduce to the default-constructed case for this,
      // since there is nothing that we can move in-place.
      if (this->isExtern() || o.isExtern()) {
        reset();
      }

      if (!o.isExtern()) {
        if constexpr (kShouldCopyWholeInlineStorageTrivial) {
          copyWholeInlineStorageTrivial(o);
          o.resetSizePolicy();
        } else if constexpr (IsRelocatable<Value>::value) {
          std::destroy_n(u.buffer(), size());
          moveInlineStorageRelocatable(std::move(o));
        } else {
          const size_t oSize = o.size();
          detail::small_vector_detail::partiallyUninitializedCopy(
              std::make_move_iterator(o.u.buffer()),
              oSize,
              this->u.buffer(),
              size());
          this->setSize(oSize);
          o.clear();
        }
      } else {
        this->u.pdata_.heap_ = o.u.pdata_.heap_;
        o.u.pdata_.heap_ = nullptr;
        // this was already reset above, so it's empty and internal.
        this->swapSizePolicy(o);
        if (kHasInlineCapacity) {
          this->u.setCapacity(o.u.getCapacity());
        }
      }
    }
    return *this;
  }

  bool operator==(small_vector const& o) const {
    return size() == o.size() && std::equal(begin(), end(), o.begin());
  }

  bool operator<(small_vector const& o) const {
    return std::lexicographical_compare(begin(), end(), o.begin(), o.end());
  }

  friend bool operator!=(small_vector const& a, small_vector const& b) {
    return !(a == b);
  }
  friend bool operator>(small_vector const& a, small_vector const& b) {
    return b < a;
  }
  friend bool operator<=(small_vector const& a, small_vector const& b) {
    return !(b < a);
  }
  friend bool operator>=(small_vector const& a, small_vector const& b) {
    return !(a < b);
  }

#if defined(__cpp_lib_three_way_comparison)
  template <typename U = value_type>
  friend auto operator<=>(const small_vector& lhs, const small_vector& rhs)
      -> decltype(std::declval<const U&>() <=> std::declval<const U&>()) {
    return std::lexicographical_compare_three_way(
        lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
  }
#endif // defined(__cpp_lib_three_way_comparison)

  static constexpr size_type max_size() {
    return !BaseType::kShouldUseHeap
        ? static_cast<size_type>(MaxInline)
        : BaseType::policyMaxSize();
  }

  allocator_type get_allocator() const { return {}; }

  size_type size() const { return this->doSize(); }
  bool empty() const { return !size(); }

  iterator begin() [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] { return data(); }
  iterator end() [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] { return data() + size(); }
  const_iterator begin() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return data();
  }
  const_iterator end() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return data() + size();
  }
  const_iterator cbegin() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return begin();
  }
  const_iterator cend() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return end();
  }

  reverse_iterator rbegin() [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return reverse_iterator(end());
  }
  reverse_iterator rend() [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return reverse_iterator(begin());
  }

  const_reverse_iterator rbegin() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return const_reverse_iterator(end());
  }

  const_reverse_iterator rend() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return const_reverse_iterator(begin());
  }

  const_reverse_iterator crbegin() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return rbegin();
  }
  const_reverse_iterator crend() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return rend();
  }

  /*
   * Usually one of the simplest functions in a Container-like class
   * but a bit more complex here.  We have to handle all combinations
   * of in-place vs. heap between this and o.
   */
  void swap(small_vector& o) noexcept(
      std::is_nothrow_move_constructible<Value>::value &&
      std::is_nothrow_swappable_v<Value>) {
    using std::swap; // Allow ADL on swap for our value_type.

    if (this->isExtern() && o.isExtern()) {
      this->swapSizePolicy(o);

      // Cannot use std::swap() because pdata_ is packed.
      auto* tmp = u.pdata_.heap_;
      u.pdata_.heap_ = o.u.pdata_.heap_;
      o.u.pdata_.heap_ = tmp;

      if (kHasInlineCapacity) {
        const auto currentCapacity = this->u.getCapacity();
        this->setCapacity(o.u.getCapacity());
        o.u.setCapacity(currentCapacity);
      }

      return;
    }

    if (!this->isExtern() && !o.isExtern()) {
      auto& oldSmall = size() < o.size() ? *this : o;
      auto& oldLarge = size() < o.size() ? o : *this;

      for (size_type i = 0; i < oldSmall.size(); ++i) {
        swap(oldSmall[i], oldLarge[i]);
      }

      size_type i = oldSmall.size();
      const size_type ci = i;
      {
        auto rollback = makeGuard([&] {
          oldSmall.setSize(i);
          std::destroy(oldLarge.begin() + i, oldLarge.end());
          oldLarge.setSize(ci);
        });
        for (; i < oldLarge.size(); ++i) {
          auto addr = oldSmall.begin() + i;
          new (addr) value_type(std::move(oldLarge[i]));
          std::destroy_at(oldLarge.data() + i);
        }
        rollback.dismiss();
      }
      oldSmall.setSize(i);
      oldLarge.setSize(ci);
      return;
    }

    // isExtern != o.isExtern()
    auto& oldExtern = o.isExtern() ? o : *this;
    auto& oldIntern = o.isExtern() ? *this : o;

    auto oldExternCapacity = oldExtern.capacity();
    auto oldExternHeap = oldExtern.u.pdata_.heap_;

    auto buff = oldExtern.u.buffer();
    size_type i = 0;
    {
      auto rollback = makeGuard([&] {
        std::destroy_n(buff, i);
        std::destroy(oldIntern.begin() + i, oldIntern.end());
        oldIntern.resetSizePolicy();
        oldExtern.u.pdata_.heap_ = oldExternHeap;
        oldExtern.setCapacity(oldExternCapacity);
      });
      for (; i < oldIntern.size(); ++i) {
        new (&buff[i]) value_type(std::move(oldIntern[i]));
        std::destroy_at(oldIntern.data() + i);
      }
      rollback.dismiss();
    }
    oldIntern.u.pdata_.heap_ = oldExternHeap;
    this->swapSizePolicy(o);
    oldIntern.setCapacity(oldExternCapacity);
  }

  void resize(size_type sz) {
    if (sz <= size()) {
      downsize(sz);
      return;
    }
    auto extra = sz - size();
    makeSize(sz);
    detail::small_vector_detail::populateMemForward(
        begin() + size(), extra, [&](void* p) { new (p) value_type(); });
    this->incrementSize(extra);
  }

  void resize(size_type sz, value_type const& v) {
    if (sz < size()) {
      FOLLY_PUSH_WARNING
      FOLLY_GCC_DISABLE_WARNING("-Warray-bounds")
      erase(begin() + sz, end());
      FOLLY_POP_WARNING
      return;
    }
    auto extra = sz - size();
    makeSize(sz);
    detail::small_vector_detail::populateMemForward(
        begin() + size(), extra, [&](void* p) { new (p) value_type(v); });
    this->incrementSize(extra);
  }

  // Grows without initializing the new elements.
  // See folly::resizeWithoutInitialization.
  void resize_without_initialization(size_type sz) {
    static_assert(
        std::is_trivially_destructible_v<value_type>,
        "resize_without_initialization requires a trivially destructible value_type");
    if (sz <= size()) {
      downsize(sz);
      return;
    }
    makeSize(sz);
    this->setSize(sz);
  }

  value_type* data() noexcept {
    return this->isExtern() ? u.heap() : u.buffer();
  }

  value_type const* data() const noexcept {
    return this->isExtern() ? u.heap() : u.buffer();
  }

  template <class... Args>
  iterator emplace(const_iterator p, Args&&... args) {
    if (p == cend()) {
      emplace_back(std::forward<Args>(args)...);
      return end() - 1;
    }

    /*
     * We implement emplace at places other than at the back with a
     * temporary for exception safety reasons.  It is possible to
     * avoid having to do this, but it becomes hard to maintain the
     * basic exception safety guarantee (unless you respond to a copy
     * constructor throwing by clearing the whole vector).
     *
     * The reason for this is that otherwise you have to destruct an
     * element before constructing this one in its place---if the
     * constructor throws, you either need a nothrow default
     * constructor or a nothrow copy/move to get something back in the
     * "gap", and the vector requirements don't guarantee we have any
     * of these.  Clearing the whole vector is a legal response in
     * this situation, but it seems like this implementation is easy
     * enough and probably better.
     */
    return insert(p, value_type(std::forward<Args>(args)...));
  }

  void reserve(size_type sz) { makeSize(sz); }

  size_type capacity() const {
    if (this->isExtern()) {
      return u.getCapacity();
    }
    return MaxInline;
  }

  // Returns the number of bytes used by the heap allocation, including any
  // capacity prefix. Returns 0 when data is stored inline or not yet allocated.
  size_t heap_allocation_size() const {
    if (!this->isExtern() || !u.pdata_.heap_) {
      return 0;
    }
    return u.getCapacity() * sizeof(value_type) +
        u.pdata_.allocationExtraBytes();
  }

  void shrink_to_fit() {
    if (!this->isExtern() || size() == capacity()) {
      return;
    }

    small_vector old = std::exchange(*this, {});
    reserve(old.size());
    std::move(old.begin(), old.end(), std::back_inserter(*this));
  }

  template <class... Args>
  reference emplace_back(Args&&... args) {
    auto isize_ = this->getInternalSize();
    if (isize_ < MaxInline) {
      new (u.buffer() + isize_) value_type(std::forward<Args>(args)...);
      this->incrementSize(1);
      return *(u.buffer() + isize_);
    }
    if (!BaseType::kShouldUseHeap) {
      throw_exception<std::length_error>("max_size exceeded in small_vector");
    }
    auto currentSize = size();
    auto currentCapacity = capacity();
    if (currentCapacity == currentSize) {
      // Any of args may be references into the vector.
      // When we are reallocating, we have to be careful to construct the new
      // element before modifying the data in the old buffer.
      makeSize(
          currentSize + 1,
          [&](void* p) { new (p) value_type(std::forward<Args>(args)...); },
          currentSize);
    } else {
      // We know the vector is stored in the heap.
      new (u.heap() + currentSize) value_type(std::forward<Args>(args)...);
    }
    this->incrementSize(1);
    return *(u.heap() + currentSize);
  }

  void push_back(value_type&& t) { emplace_back(std::move(t)); }

  void push_back(value_type const& t) { emplace_back(t); }

  void pop_back() {
    // ideally this would be implemented in terms of erase(end() - 1) to reuse
    // the higher-level abstraction, but neither Clang or GCC are able to
    // optimize it away. if you change this, please verify (with disassembly)
    // that the generated code on -O3 (and ideally -O2) stays short
    downsize(size() - 1);
  }

  iterator insert(const_iterator constp, value_type&& t) {
    iterator p = unconst(constp);
    if (p == end()) {
      push_back(std::move(t));
      return end() - 1;
    }

    auto offset = p - begin();
    auto currentSize = size();
    if (capacity() == currentSize) {
      makeSize(
          currentSize + 1,
          [&t](void* ptr) { new (ptr) value_type(std::move(t)); },
          offset);
      this->incrementSize(1);
    } else {
      detail::small_vector_detail::moveObjectsRightAndCreate(
          data() + offset,
          data() + currentSize,
          data() + currentSize + 1,
          [&]() mutable -> value_type&& { return std::move(t); });
      this->incrementSize(1);
    }
    return begin() + offset;
  }

  iterator insert(const_iterator p, value_type const& t) {
    // Make a copy and forward to the rvalue value_type&& overload
    // above.
    //
    // std::move() is necessary to avoid an MSVC compiler bug which will
    // issue this warning when used with unsigned int:
    // warning C4717 : 'folly::small_vector<unsigned int,192,void>::insert':
    // recursive on all control paths, function will cause runtime stack
    // overflow
    return insert(p, std::move(value_type(t)));
  }

  iterator insert(const_iterator pos, size_type n, value_type const& val) {
    auto offset = pos - begin();
    if (n != 0) {
      auto currentSize = size();
      makeSize(currentSize + n);
      detail::small_vector_detail::moveObjectsRightAndCreate(
          data() + offset,
          data() + currentSize,
          data() + currentSize + n,
          [&]() mutable -> value_type const& { return val; });
      this->incrementSize(n);
    }
    return begin() + offset;
  }

  template <class Arg>
  iterator insert(const_iterator p, Arg arg1, Arg arg2) {
    // Forward using std::is_arithmetic to get to the proper
    // implementation; this disambiguates between the iterators and
    // (size_t, value_type) meaning for this function.
    return insertImpl(unconst(p), arg1, arg2, std::is_arithmetic<Arg>());
  }

  iterator insert(const_iterator p, std::initializer_list<value_type> il) {
    return insert(p, il.begin(), il.end());
  }

  iterator erase(const_iterator q) {
    // ideally this would be implemented in terms of erase(q, q + 1) to reuse
    // the higher-level abstraction, but neither Clang or GCC are able to
    // optimize it away. if you change this, please verify (with disassembly)
    // that the generated code on -O3 (and ideally -O2) stays short
    std::move(unconst(q) + 1, end(), unconst(q));
    downsize(size() - 1);
    return unconst(q);
  }

  iterator erase(const_iterator q1, const_iterator q2) {
    if (q1 == q2) {
      return unconst(q1);
    }
    std::move(unconst(q2), end(), unconst(q1));
    downsize(size() - std::distance(q1, q2));
    return unconst(q1);
  }

  void clear() {
    // ideally this would be implemented in terms of erase(begin(), end()) to
    // reuse the higher-level abstraction, but neither Clang or GCC are able to
    // optimize it away. if you change this, please verify (with disassembly)
    // that the generated code on -O3 (and ideally -O2) stays short
    downsize(0);
  }

  template <class Arg>
  void assign(Arg first, Arg last) {
    clear();
    insert(end(), first, last);
  }

  void assign(std::initializer_list<value_type> il) {
    assign(il.begin(), il.end());
  }

  void assign(size_type n, const value_type& t) {
    clear();
    insert(end(), n, t);
  }

  reference front() [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    assert(!empty());
    return *begin();
  }
  reference back() [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    assert(!empty());
    return *(end() - 1);
  }
  const_reference front() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    assert(!empty());
    return *begin();
  }
  const_reference back() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    assert(!empty());
    return *(end() - 1);
  }

  reference operator[](size_type i) [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    assert(i < size());
    return *(begin() + i);
  }

  const_reference operator[](size_type i) const
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    assert(i < size());
    return *(begin() + i);
  }

  reference at(size_type i) [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    if (i >= size()) {
      throw_exception<std::out_of_range>("index out of range");
    }
    return (*this)[i];
  }

  const_reference at(size_type i) const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    if (i >= size()) {
      throw_exception<std::out_of_range>("index out of range");
    }
    return (*this)[i];
  }

 private:
  static iterator unconst(const_iterator it) {
    return const_cast<iterator>(it);
  }

  void downsize(size_type sz) {
    assert(sz <= size());
    std::destroy(begin() + sz, end());
    this->setSize(sz);
  }

  void copyWholeInlineStorageTrivial(small_vector const& o) {
    static_assert(detail::small_vector_detail::should_trivially_copy<Value>);
    FOLLY_PUSH_WARNING
    FOLLY_GCC_DISABLE_WARNING("-Warray-bounds")
    std::copy(o.u.buffer(), o.u.buffer() + MaxInline, u.buffer());
    FOLLY_POP_WARNING
    this->setSize(o.size());
  }

  void moveInlineStorageRelocatable(small_vector&& o) {
    static_assert(IsRelocatable<Value>::value);
    const auto n = o.size();
    FOLLY_PUSH_WARNING
    FOLLY_GCC_DISABLE_WARNING("-Wclass-memaccess")
    if constexpr (kMayCopyWholeInlineStorage) {
      std::memcpy(
          reinterpret_cast<void*>(u.buffer()),
          reinterpret_cast<const void*>(o.u.buffer()),
          MaxInline * kSizeOfValue);
    } else {
      std::memcpy(
          reinterpret_cast<void*>(u.buffer()),
          reinterpret_cast<const void*>(o.u.buffer()),
          n * kSizeOfValue);
    }
    FOLLY_POP_WARNING
    this->setSize(n);
    o.resetSizePolicy();
  }

  void reset() {
    clear();
    freeHeap();
    this->resetSizePolicy();
  }

  // The std::false_type argument is part of disambiguating the
  // iterator insert functions from integral types (see insert().)
  template <class It>
  iterator insertImpl(iterator pos, It first, It last, std::false_type) {
    if (first == last) {
      return pos;
    }
    using categ = typename std::iterator_traits<It>::iterator_category;
    using it_ref = typename std::iterator_traits<It>::reference;
    if constexpr (std::is_base_of_v<std::bidirectional_iterator_tag, categ>) {
      auto const distance = std::distance(first, last);
      auto const offset = pos - begin();
      auto currentSize = size();
      assert(distance >= 0);
      assert(offset >= 0);
      makeSize(currentSize + distance);
      detail::small_vector_detail::moveObjectsRightAndCreate(
          data() + offset,
          data() + currentSize,
          data() + currentSize + distance,
          [&, in = last]() mutable -> it_ref { return *--in; });
      this->incrementSize(distance);
      return begin() + offset;
    } else {
      auto offset = pos - begin();
      while (first != last) {
        pos = insert(pos, *first++);
        ++pos;
      }
      return begin() + offset;
    }
  }

  iterator insertImpl(
      iterator pos, size_type n, const value_type& val, std::true_type) {
    // The true_type means this should call the size_t,value_type
    // overload.  (See insert().)
    return insert(pos, n, val);
  }

  void destroy() {
    std::destroy(begin(), end());
    freeHeap();
  }

  // The std::false_type argument came from std::is_arithmetic as part
  // of disambiguating an overload (see the comment in the
  // constructor).
  template <class It>
  void constructImpl(It first, It last, std::false_type) {
    using categ = typename std::iterator_traits<It>::iterator_category;
    if (std::is_same<categ, std::input_iterator_tag>::value) {
      // With iterators that only allow a single pass, we can't really
      // do anything sane here.
      auto rollback = makeGuard([&] { destroy(); });
      while (first != last) {
        emplace_back(*first++);
      }
      rollback.dismiss();
      return;
    }
    size_type distance = std::distance(first, last);
    if (distance <= MaxInline) {
      this->incrementSize(distance);
      detail::small_vector_detail::populateMemForward(
          u.buffer(), distance, [&](void* p) { new (p) value_type(*first++); });
      return;
    }
    makeSize(distance);
    this->incrementSize(distance);
    {
      auto rollback = makeGuard([&] { freeHeap(); });
      detail::small_vector_detail::populateMemForward(
          u.heap(), distance, [&](void* p) { new (p) value_type(*first++); });
      rollback.dismiss();
    }
  }

  template <typename InitFunc>
  void doConstruct(size_type n, InitFunc&& func) {
    makeSize</* IgnoreExistingData */ true>(n);
    assert(size() == 0);
    this->incrementSize(n);
    {
      auto rollback = makeGuard([&] { freeHeap(); });
      detail::small_vector_detail::populateMemForward(
          data(), n, std::forward<InitFunc>(func));
      rollback.dismiss();
    }
  }

  // The true_type means we should forward to the size_t,value_type
  // overload.
  void constructImpl(size_type n, value_type const& val, std::true_type) {
    FOLLY_PUSH_WARNING
    FOLLY_GCC_DISABLE_WARNING("-Warray-bounds")
    doConstruct(n, [&](void* p) { new (p) value_type(val); });
    FOLLY_POP_WARNING
  }

  /*
   * Compute the size after growth.
   */
  size_type computeNewSize() const {
    size_t c = capacity();
    if (!checked_mul(&c, c, size_t(3))) {
      throw_exception<std::length_error>(
          "Requested new size exceeds size representable by size_type");
    }
    c = (c / 2) + 1;
    return static_cast<size_type>(std::min<size_t>(c, max_size()));
  }

  template <bool IgnoreExistingData = false>
  void makeSize(size_type newSize) {
    if (newSize <= capacity()) {
      return;
    }
    auto emplaceFunc = [](void*) { assume_unreachable(); };
    makeSizeInternal<
        /* Insert */ false,
        IgnoreExistingData,
        decltype(emplaceFunc)>(newSize, std::move(emplaceFunc), 0);
  }

  template <typename EmplaceFunc>
  void makeSize(size_type newSize, EmplaceFunc&& emplaceFunc, size_type pos) {
    assert(size() == capacity());

    makeSizeInternal<
        /* Insert */ true,
        /* IgnoreExistingData */ false,
        EmplaceFunc>(newSize, std::forward<EmplaceFunc>(emplaceFunc), pos);
  }

  /*
   * Ensure we have a large enough memory region to be size `newSize'.
   * Will move/copy elements if we are spilling to heap_ or needed to
   * allocate a new region, but if resized in place doesn't initialize
   * anything in the new region.  In any case doesn't change size().
   * Supports insertion of new element during reallocation by given
   * pointer to new element and position of new element.
   * NOTE: If reallocation is not needed, insert must be false,
   * because we only know how to emplace elements into new memory.
   */
  template <bool Insert, bool IgnoreExistingData, typename EmplaceFunc>
  void makeSizeInternal(
      size_type newSize, EmplaceFunc&& emplaceFunc, size_type pos) {
    if (newSize > max_size()) {
      throw_exception<std::length_error>("max_size exceeded in small_vector");
    }
    assert(this->kShouldUseHeap);
    // This branch isn't needed for correctness, but allows the optimizer to
    // skip generating code for the rest of this function in in-situ-only
    // small_vectors.
    if (!this->kShouldUseHeap) {
      return;
    }

    newSize = std::max(newSize, computeNewSize());

    size_t needBytes = newSize;
    if (!checked_mul(&needBytes, needBytes, sizeof(value_type))) {
      throw_exception<std::length_error>(
          "Requested new size exceeds size representable by size_type");
    }
    // Store capacity at the front of the heap allocation when it doesn't
    // fit inline in the union.
    const bool heapifyCapacity = !kHasInlineCapacity;
    const size_t allocationExtraBytes =
        heapifyCapacity ? kHeapifyCapacitySize : 0;
    size_t needAllocSizeBytes = needBytes;
    if (!checked_add(
            &needAllocSizeBytes, needAllocSizeBytes, allocationExtraBytes)) {
      throw_exception<std::length_error>(
          "Requested new size exceeds size representable by size_type");
    }
    const size_t goodAllocationSizeBytes = goodMallocSize(needAllocSizeBytes);
    const size_t goodAllocationNewCapacity =
        (goodAllocationSizeBytes - allocationExtraBytes) / sizeof(value_type);
    const size_t newCapacity = std::min(goodAllocationNewCapacity, max_size());
    // Make sure that the allocation request has a size computable from the
    // capacity, instead of using goodAllocationSizeBytes, so that we can do
    // sized deallocation. If goodMallocSize() gives us extra bytes that are not
    // a multiple of the value size we cannot use them anyway.
    const size_t sizeBytes =
        newCapacity * sizeof(value_type) + allocationExtraBytes;
    void* newh = checkedMalloc(sizeBytes);
    value_type* newp = static_cast<value_type*>(
        heapifyCapacity
            ? detail::small_vector_detail::shiftPointer(
                  newh, kHeapifyCapacitySize)
            : newh);

    {
      auto rollback = makeGuard([&] { //
        sizedFree(newh, sizeBytes);
      });
      if constexpr (Insert) {
        static_assert(!IgnoreExistingData);
        // move and insert the new element
        this->moveToUninitializedEmplace(
            begin(), end(), newp, pos, std::forward<EmplaceFunc>(emplaceFunc));
      } else if constexpr (!IgnoreExistingData) {
        // move without inserting new element
        if (data()) {
          this->moveToUninitialized(begin(), end(), newp);
        }
      }
      rollback.dismiss();
    }
    annotate_object_leaked(newh);
    std::destroy(begin(), end());
    freeHeap();
    u.pdata_.heap_ = newp;
    this->setExtern(true);
    this->setCapacity(newCapacity);
  }

  /*
   * This will set the capacity field, stored inline in the storage_ field
   * if there is sufficient room to store it.
   */
  void setCapacity(size_type newCapacity) {
    assert(this->isExtern());
    assert(newCapacity <= std::numeric_limits<InternalSizeType>::max());
    u.setCapacity(newCapacity);
  }

 private:
  // These internal classes are packed to minimize total memory usage,
  // however, it is important that we don't pack the class as a whole
  // otherwise the inline storage may not have the correct alignment
  // for the value type.
  FOLLY_SV_PACK_PUSH
  struct HeapPtrWithCapacity {
    value_type* heap_;
    InternalSizeType capacity_;

    InternalSizeType getCapacity() const {
      FOLLY_PUSH_WARNING
      FOLLY_GCC_DISABLE_WARNING("-Wmaybe-uninitialized")
      return capacity_;
      FOLLY_POP_WARNING
    }
    void setCapacity(InternalSizeType c) { capacity_ = c; }
    size_t allocationExtraBytes() const { return 0; }
  } FOLLY_SV_PACK_ATTR;
  FOLLY_SV_PACK_POP

  FOLLY_SV_PACK_PUSH
  struct HeapPtr {
    // heap[-kHeapifyCapacitySize] contains capacity
    value_type* heap_;

    InternalSizeType getCapacity() const {
      return heap_
          ? *static_cast<InternalSizeType*>(
                detail::small_vector_detail::unshiftPointer(
                    heap_, kHeapifyCapacitySize))
          : 0;
    }
    void setCapacity(InternalSizeType c) {
      *static_cast<InternalSizeType*>(
          detail::small_vector_detail::unshiftPointer(
              heap_, kHeapifyCapacitySize)) = c;
    }
    size_t allocationExtraBytes() const { return kHeapifyCapacitySize; }
  } FOLLY_SV_PACK_ATTR;
  FOLLY_SV_PACK_POP

  static constexpr size_t kMaxInlineNonZero = MaxInline ? MaxInline : 1u;
  using InlineStorageDataType =
      aligned_storage_for_t<value_type[kMaxInlineNonZero]>;

  using InlineStorageType = typename std::conditional<
      sizeof(value_type) * MaxInline != 0,
      InlineStorageDataType,
      char>::type;

  // If the storage is small enough, it is usually faster to copy it entirely,
  // instead of just size() values, to make the loop fixed-size and
  // unrollable. Limit is half of a cache line, to minimize probability of
  // crossing a cache line and thus introducing an unnecessary cache miss.
  static constexpr bool kMayCopyWholeInlineStorage =
      sizeof(InlineStorageType) <= hardware_constructive_interference_size / 2;

  static constexpr bool kShouldCopyWholeInlineStorageTrivial =
      detail::small_vector_detail::should_trivially_copy<Value> &&
      kMayCopyWholeInlineStorage;

  // Store capacity inline when HeapPtrWithCapacity fits within the inline
  // storage without increasing the union size. When the two are equal, the
  // union is already sized by InlineStorageType, so storing capacity is free.
  static bool constexpr kHasInlineCapacity = !BaseType::kAlwaysUseHeap &&
      sizeof(HeapPtrWithCapacity) <= sizeof(InlineStorageType);

  // This value should we multiple of word size.
  static size_t constexpr kHeapifyCapacitySize = sizeof(
      folly::aligned_storage_t<sizeof(InternalSizeType), alignof(value_type)>);

  using PointerType = typename std::
      conditional<kHasInlineCapacity, HeapPtrWithCapacity, HeapPtr>::type;

  void freeHeap() {
    if (!this->isExtern() || !u.pdata_.heap_) {
      assert(heap_allocation_size() == 0);
      return;
    }

    auto extraBytes = u.pdata_.allocationExtraBytes();
    auto vp =
        detail::small_vector_detail::unshiftPointer(u.pdata_.heap_, extraBytes);
    auto bytes = u.getCapacity() * sizeof(value_type) + extraBytes;
    assert(heap_allocation_size() == bytes);
    annotate_object_collected(vp);
    sizedFree(vp, bytes);
  }

  union Data {
    explicit Data() { pdata_.heap_ = nullptr; }

    PointerType pdata_;
    InlineStorageType storage_;

    value_type* buffer() noexcept {
      void* vp = &storage_;
      return static_cast<value_type*>(vp);
    }
    value_type const* buffer() const noexcept {
      return const_cast<Data*>(this)->buffer();
    }
    value_type* heap() noexcept { return pdata_.heap_; }
    value_type const* heap() const noexcept { return pdata_.heap_; }

    InternalSizeType getCapacity() const { return pdata_.getCapacity(); }
    void setCapacity(InternalSizeType c) { pdata_.setCapacity(c); }

  } u;
};

//////////////////////////////////////////////////////////////////////

// Basic guarantee only, or provides the nothrow guarantee iff T has a
// nothrow move or copy constructor.
template <class T, std::size_t MaxInline, class P>
void swap(small_vector<T, MaxInline, P>& a, small_vector<T, MaxInline, P>& b) {
  a.swap(b);
}

template <class T, std::size_t MaxInline, class P, class U>
void erase(small_vector<T, MaxInline, P>& v, U value) {
  v.erase(std::remove(v.begin(), v.end(), value), v.end());
}

template <class T, std::size_t MaxInline, class P, class Predicate>
void erase_if(small_vector<T, MaxInline, P>& v, Predicate predicate) {
  v.erase(std::remove_if(v.begin(), v.end(), std::ref(predicate)), v.end());
}

//////////////////////////////////////////////////////////////////////

template <typename>
struct is_small_vector : std::false_type {};

template <class Value, size_t N, class Policy>
struct is_small_vector<small_vector<Value, N, Policy>> : std::true_type {};

template <typename T>
inline constexpr bool is_small_vector_v = is_small_vector<T>::value;

} // namespace folly

FOLLY_POP_WARNING

#undef FOLLY_SV_PACK_ATTR
#undef FOLLY_SV_PACK_PUSH
#undef FOLLY_SV_PACK_POP

namespace std {

template <class T, std::size_t M, class P>
struct hash<folly::small_vector<T, M, P>> {
  size_t operator()(const folly::small_vector<T, M, P>& v) const {
    return folly::hash::hash_range(v.begin(), v.end());
  }
};

} // namespace std
