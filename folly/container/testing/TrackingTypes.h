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

#include <cstddef>
#include <limits>
#include <memory>
#include <ostream>

#include <folly/Function.h>
#include <folly/hash/Hash.h>
#include <folly/lang/SafeAssert.h>
#include <folly/portability/Asm.h>

namespace folly {
namespace test {

struct MoveOnlyTestInt {
  int x;
  bool destroyed{false};

  MoveOnlyTestInt() noexcept : x(0) {}
  /* implicit */ MoveOnlyTestInt(int x0) : x(x0) {}
  MoveOnlyTestInt(MoveOnlyTestInt&& rhs) noexcept : x(rhs.x) {}
  MoveOnlyTestInt(MoveOnlyTestInt const&) = delete;
  MoveOnlyTestInt& operator=(MoveOnlyTestInt&& rhs) noexcept {
    FOLLY_SAFE_CHECK(!rhs.destroyed, "");
    x = rhs.x;
    return *this;
  }
  MoveOnlyTestInt& operator=(MoveOnlyTestInt const&) = delete;

  ~MoveOnlyTestInt() {
    FOLLY_SAFE_CHECK(!destroyed, "");
    destroyed = true;
    asm_volatile_memory(); // try to keep compiler from eliding the store
  }

  bool operator==(MoveOnlyTestInt const& rhs) const {
    FOLLY_SAFE_CHECK(!destroyed, "");
    FOLLY_SAFE_CHECK(!rhs.destroyed, "");
    return x == rhs.x && destroyed == rhs.destroyed;
  }
  bool operator!=(MoveOnlyTestInt const& rhs) const { return !(*this == rhs); }
};

struct ThrowOnCopyTestInt {
  int x{0};

  ThrowOnCopyTestInt() {}

  [[noreturn]] ThrowOnCopyTestInt(const ThrowOnCopyTestInt& other)
      : x(other.x) {
    throw std::exception{};
  }

  ThrowOnCopyTestInt& operator=(const ThrowOnCopyTestInt&) {
    throw std::exception{};
  }

  bool operator==(const ThrowOnCopyTestInt& other) const {
    return x == other.x;
  }

  bool operator!=(const ThrowOnCopyTestInt& other) const {
    return !(x == other.x);
  }
};

struct PermissiveConstructorTestInt {
  int x;

  PermissiveConstructorTestInt() noexcept : x(0) {}
  /* implicit */ PermissiveConstructorTestInt(int x0) : x(x0) {}

  template <typename T>
  /* implicit */ PermissiveConstructorTestInt(T&& src)
      : x(std::forward<T>(src)) {}

  PermissiveConstructorTestInt(PermissiveConstructorTestInt&& rhs) noexcept
      : x(rhs.x) {}
  PermissiveConstructorTestInt(PermissiveConstructorTestInt const&) = delete;
  PermissiveConstructorTestInt& operator=(
      PermissiveConstructorTestInt&& rhs) noexcept {
    x = rhs.x;
    return *this;
  }
  PermissiveConstructorTestInt& operator=(PermissiveConstructorTestInt const&) =
      delete;

  bool operator==(PermissiveConstructorTestInt const& rhs) const {
    return x == rhs.x;
  }
  bool operator!=(PermissiveConstructorTestInt const& rhs) const {
    return !(*this == rhs);
  }
};

// Tracked is implicitly constructible across tags
struct Counts {
  uint64_t copyConstruct{0};
  uint64_t moveConstruct{0};
  uint64_t copyConvert{0};
  uint64_t moveConvert{0};
  uint64_t copyAssign{0};
  uint64_t moveAssign{0};
  uint64_t defaultConstruct{0};
  uint64_t destroyed{0};

  explicit Counts(
      uint64_t copConstr = 0,
      uint64_t movConstr = 0,
      uint64_t copConv = 0,
      uint64_t movConv = 0,
      uint64_t copAssign = 0,
      uint64_t movAssign = 0,
      uint64_t def = 0,
      uint64_t destr = 0)
      : copyConstruct{copConstr},
        moveConstruct{movConstr},
        copyConvert{copConv},
        moveConvert{movConv},
        copyAssign{copAssign},
        moveAssign{movAssign},
        defaultConstruct{def},
        destroyed{destr} {}

  int64_t liveCount() const {
    return copyConstruct + moveConstruct + copyConvert + moveConvert +
        defaultConstruct - destroyed;
  }

  // dist ignores destroyed count
  uint64_t dist(Counts const& rhs) const {
    auto d = [](uint64_t x, uint64_t y) { return (x - y) * (x - y); };
    return d(copyConstruct, rhs.copyConstruct) +
        d(moveConstruct, rhs.moveConstruct) + d(copyConvert, rhs.copyConvert) +
        d(moveConvert, rhs.moveConvert) + d(copyAssign, rhs.copyAssign) +
        d(moveAssign, rhs.moveAssign) +
        d(defaultConstruct, rhs.defaultConstruct);
  }

  bool operator==(Counts const& rhs) const {
    return dist(rhs) == 0 && destroyed == rhs.destroyed;
  }
  bool operator!=(Counts const& rhs) const { return !(*this == rhs); }
};

inline std::ostream& operator<<(std::ostream& xo, Counts const& counts) {
  xo << "[";
  std::string glue;
  if (counts.copyConstruct > 0) {
    xo << glue << counts.copyConstruct << " copy";
    glue = ", ";
  }
  if (counts.moveConstruct > 0) {
    xo << glue << counts.moveConstruct << " move";
    glue = ", ";
  }
  if (counts.copyConvert > 0) {
    xo << glue << counts.copyConvert << " copy convert";
    glue = ", ";
  }
  if (counts.moveConvert > 0) {
    xo << glue << counts.moveConvert << " move convert";
    glue = ", ";
  }
  if (counts.copyAssign > 0) {
    xo << glue << counts.copyAssign << " copy assign";
    glue = ", ";
  }
  if (counts.moveAssign > 0) {
    xo << glue << counts.moveAssign << " move assign";
    glue = ", ";
  }
  if (counts.defaultConstruct > 0) {
    xo << glue << counts.defaultConstruct << " default construct";
    glue = ", ";
  }
  if (counts.destroyed > 0) {
    xo << glue << counts.destroyed << " destroyed";
    glue = ", ";
  }
  xo << "]";
  return xo;
}

inline Counts& sumCounts() {
  static thread_local Counts value{};
  return value;
}

template <int Tag>
struct Tracked {
  static_assert(Tag <= 5, "Need to extend Tracked<Tag> in TestUtil.cpp");

  static Counts& counts() {
    static thread_local Counts value{};
    return value;
  }

  uint64_t val_;

  Tracked() : val_{0} {
    sumCounts().defaultConstruct++;
    counts().defaultConstruct++;
  }
  /* implicit */ Tracked(uint64_t const& val) : val_{val} {
    sumCounts().copyConvert++;
    counts().copyConvert++;
  }
  /* implicit */ Tracked(uint64_t&& val) : val_{val} {
    sumCounts().moveConvert++;
    counts().moveConvert++;
  }
  Tracked(Tracked const& rhs) : val_{rhs.val_} {
    sumCounts().copyConstruct++;
    counts().copyConstruct++;
  }
  Tracked(Tracked&& rhs) noexcept : val_{rhs.val_} {
    sumCounts().moveConstruct++;
    counts().moveConstruct++;
  }
  Tracked& operator=(Tracked const& rhs) {
    val_ = rhs.val_;
    sumCounts().copyAssign++;
    counts().copyAssign++;
    return *this;
  }
  Tracked& operator=(Tracked&& rhs) noexcept {
    val_ = rhs.val_;
    sumCounts().moveAssign++;
    counts().moveAssign++;
    return *this;
  }

  template <int T>
  /* implicit */ Tracked(Tracked<T> const& rhs) : val_{rhs.val_} {
    sumCounts().copyConvert++;
    counts().copyConvert++;
  }

  template <int T>
  /* implicit */ Tracked(Tracked<T>&& rhs) : val_{rhs.val_} {
    sumCounts().moveConvert++;
    counts().moveConvert++;
  }

  ~Tracked() {
    sumCounts().destroyed++;
    counts().destroyed++;
  }

  bool operator==(Tracked const& rhs) const { return val_ == rhs.val_; }
  bool operator!=(Tracked const& rhs) const { return !(*this == rhs); }
};

template <int Tag>
struct TransparentTrackedHash {
  using is_transparent = void;

  size_t operator()(Tracked<Tag> const& tracked) const {
    return tracked.val_ ^ Tag;
  }
  size_t operator()(uint64_t v) const { return v ^ Tag; }
};

template <int Tag>
struct TransparentTrackedEqual {
  using is_transparent = void;

  uint64_t unwrap(Tracked<Tag> const& v) const { return v.val_; }
  uint64_t unwrap(uint64_t v) const { return v; }

  template <typename A, typename B>
  bool operator()(A const& lhs, B const& rhs) const {
    return unwrap(lhs) == unwrap(rhs);
  }
};

inline size_t& testAllocatedMemorySize() {
  static thread_local size_t value{0};
  return value;
}

inline size_t& testAllocatedBlockCount() {
  static thread_local size_t value{0};
  return value;
}

inline size_t& testAllocationCount() {
  static thread_local size_t value{0};
  return value;
}

inline size_t& testAllocationMaxCount() {
  static thread_local size_t value{std::numeric_limits<std::size_t>::max()};
  return value;
}

inline void limitTestAllocations(std::size_t allocationsBeforeException = 0) {
  testAllocationMaxCount() = testAllocationCount() + allocationsBeforeException;
}

inline void unlimitTestAllocations() {
  testAllocationMaxCount() = std::numeric_limits<std::size_t>::max();
}

inline void resetTracking() {
  sumCounts() = Counts{};
  Tracked<0>::counts() = Counts{};
  Tracked<1>::counts() = Counts{};
  Tracked<2>::counts() = Counts{};
  Tracked<3>::counts() = Counts{};
  Tracked<4>::counts() = Counts{};
  Tracked<5>::counts() = Counts{};
  testAllocatedMemorySize() = 0;
  testAllocatedBlockCount() = 0;
  testAllocationCount() = 0;
  testAllocationMaxCount() = std::numeric_limits<std::size_t>::max();
}

template <class T>
class SwapTrackingAlloc {
 public:
  using Alloc = std::allocator<T>;
  using AllocTraits = std::allocator_traits<Alloc>;
  using value_type = typename AllocTraits::value_type;

  using pointer = typename AllocTraits::pointer;
  using const_pointer = typename AllocTraits::const_pointer;
  using reference = value_type&;
  using const_reference = value_type const&;
  using size_type = typename AllocTraits::size_type;

  using propagate_on_container_swap = std::true_type;
  using propagate_on_container_copy_assignment = std::true_type;
  using propagate_on_container_move_assignment = std::true_type;

  SwapTrackingAlloc() {}

  template <class U>
  /* implicit */ SwapTrackingAlloc(SwapTrackingAlloc<U> const& other) noexcept
      : a_(other.a_), t_(other.t_) {}

  template <class U>
  SwapTrackingAlloc& operator=(SwapTrackingAlloc<U> const& other) noexcept {
    a_ = other.a_;
    t_ = other.t_;
    return *this;
  }

  template <class U>
  /* implicit */ SwapTrackingAlloc(SwapTrackingAlloc<U>&& other) noexcept
      : a_(std::move(other.a_)), t_(std::move(other.t_)) {}

  template <class U>
  SwapTrackingAlloc& operator=(SwapTrackingAlloc<U>&& other) noexcept {
    a_ = std::move(other.a_);
    t_ = std::move(other.t_);
    return *this;
  }

  T* allocate(size_t n) {
    if (testAllocationCount() >= testAllocationMaxCount()) {
      throw std::bad_alloc();
    }
    ++testAllocationCount();
    testAllocatedMemorySize() += n * sizeof(T);
    ++testAllocatedBlockCount();
    std::size_t extra =
        std::max<std::size_t>(1, sizeof(std::size_t) / sizeof(T));
    T* p = a_.allocate(extra + n);
    void* raw = static_cast<void*>(p);
    *static_cast<std::size_t*>(raw) = n;
    return p + extra;
  }
  void deallocate(T* p, size_t n) {
    testAllocatedMemorySize() -= n * sizeof(T);
    --testAllocatedBlockCount();
    std::size_t extra =
        std::max<std::size_t>(1, sizeof(std::size_t) / sizeof(T));
    std::size_t check;
    void* raw = static_cast<void*>(p - extra);
    check = *static_cast<std::size_t*>(raw);
    FOLLY_SAFE_CHECK(check == n, "");
    a_.deallocate(p - extra, n + extra);
  }

 private:
  std::allocator<T> a_;
  Tracked<0> t_;

  template <class U>
  friend class SwapTrackingAlloc;
};

template <class T>
void swap(SwapTrackingAlloc<T>&, SwapTrackingAlloc<T>&) noexcept {
  // For argument dependent lookup:
  // This function will be called if the custom swap functions of a container
  // is used. Otherwise, std::swap() will do 1 move construct and 2 move
  // assigns which will get tracked by t_.
}

template <class T1, class T2>
bool operator==(SwapTrackingAlloc<T1> const&, SwapTrackingAlloc<T2> const&) {
  return true;
}

template <class T1, class T2>
bool operator!=(SwapTrackingAlloc<T1> const&, SwapTrackingAlloc<T2> const&) {
  return false;
}

template <class T>
class GenericAlloc {
 public:
  using value_type = T;

  using pointer = T*;
  using const_pointer = T const*;
  using reference = T&;
  using const_reference = T const&;
  using size_type = std::size_t;

  using propagate_on_container_swap = std::true_type;
  using propagate_on_container_copy_assignment = std::true_type;
  using propagate_on_container_move_assignment = std::true_type;

  using AllocBytesFunc = folly::Function<void*(std::size_t)>;
  using DeallocBytesFunc = folly::Function<void(void*, std::size_t)>;

  GenericAlloc() = delete;

  template <typename A, typename D>
  GenericAlloc(A&& alloc, D&& dealloc)
      : alloc_{std::make_shared<AllocBytesFunc>(std::forward<A>(alloc))},
        dealloc_{std::make_shared<DeallocBytesFunc>(std::forward<D>(dealloc))} {
  }

  template <class U>
  /* implicit */ GenericAlloc(GenericAlloc<U> const& other) noexcept
      : alloc_{other.alloc_}, dealloc_{other.dealloc_} {}

  template <class U>
  GenericAlloc& operator=(GenericAlloc<U> const& other) noexcept {
    alloc_ = other.alloc_;
    dealloc_ = other.dealloc_;
    return *this;
  }

  template <class U>
  /* implicit */ GenericAlloc(GenericAlloc<U>&& other) noexcept
      : alloc_(std::move(other.alloc_)), dealloc_(std::move(other.dealloc_)) {}

  template <class U>
  GenericAlloc& operator=(GenericAlloc<U>&& other) noexcept {
    alloc_ = std::move(other.alloc_);
    dealloc_ = std::move(other.dealloc_);
    return *this;
  }

  T* allocate(size_t n) { return static_cast<T*>((*alloc_)(n * sizeof(T))); }
  void deallocate(T* p, size_t n) {
    (*dealloc_)(static_cast<void*>(p), n * sizeof(T));
  }

  template <typename U>
  bool operator==(GenericAlloc<U> const& rhs) const {
    return alloc_ == rhs.alloc_;
  }

  template <typename U>
  bool operator!=(GenericAlloc<U> const& rhs) const {
    return !(*this == rhs);
  }

 private:
  std::shared_ptr<AllocBytesFunc> alloc_;
  std::shared_ptr<DeallocBytesFunc> dealloc_;

  template <class U>
  friend class GenericAlloc;
};

template <typename T>
class GenericEqual {
 public:
  using EqualFunc = folly::Function<bool(T const&, T const&)>;

  GenericEqual() = delete;

  template <typename E>
  /* implicit */ GenericEqual(E&& equal)
      : equal_{std::make_shared<EqualFunc>(std::forward<E>(equal))} {}

  bool operator()(T const& lhs, T const& rhs) const {
    return (*equal_)(lhs, rhs);
  }

 private:
  std::shared_ptr<EqualFunc> equal_;
};

template <typename T>
class GenericHasher {
 public:
  using HasherFunc = folly::Function<std::size_t(T const&)>;

  GenericHasher() = delete;

  template <typename H>
  /* implicit */ GenericHasher(H&& hasher)
      : hasher_{std::make_shared<HasherFunc>(std::forward<H>(hasher))} {}

  std::size_t operator()(T const& val) const { return (*hasher_)(val); }

 private:
  std::shared_ptr<HasherFunc> hasher_;
};

struct HashFirst {
  template <typename P>
  std::size_t operator()(P const& p) const {
    return folly::Hash{}(p.first);
  }
};

struct EqualFirst {
  template <typename P>
  bool operator()(P const& lhs, P const& rhs) const {
    return lhs.first == rhs.first;
  }
};

} // namespace test
} // namespace folly

namespace std {
template <>
struct hash<folly::test::MoveOnlyTestInt> {
  std::size_t operator()(folly::test::MoveOnlyTestInt const& val) const {
    FOLLY_SAFE_CHECK(!val.destroyed, "");
    return val.x;
  }
};

template <>
struct hash<folly::test::ThrowOnCopyTestInt> {
  std::size_t operator()(folly::test::ThrowOnCopyTestInt const& val) const {
    return val.x;
  }
};

template <>
struct hash<folly::test::PermissiveConstructorTestInt> {
  std::size_t operator()(
      folly::test::PermissiveConstructorTestInt const& val) const {
    return val.x;
  }
};

template <int Tag>
struct hash<folly::test::Tracked<Tag>> {
  size_t operator()(folly::test::Tracked<Tag> const& tracked) const {
    return tracked.val_ ^ Tag;
  }
};
} // namespace std
