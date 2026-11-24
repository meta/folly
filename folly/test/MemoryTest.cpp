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

#include <folly/Memory.h>
#include <folly/memory/Allocator.h>

#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

#include <glog/logging.h>

#include <folly/ConstexprMath.h>
#include <folly/String.h>
#include <folly/lang/Keep.h>
#include <folly/portability/Asm.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

using namespace folly;

static constexpr std::size_t kTooBig = folly::constexpr_max(
    std::size_t{std::numeric_limits<uint32_t>::max()},
    std::size_t{1} << (8 * sizeof(std::size_t) - 14));

struct MemoryTest : testing::Test {};

TEST(operatorNewDelete, zero) {
  auto p = ::operator new(0);
  EXPECT_TRUE(p != nullptr);
  ::operator delete(p);
}

#if __cpp_sized_deallocation
TEST(operatorNewDelete, sizedZero) {
  std::size_t n = 0;
  auto p = ::operator new(n);
  EXPECT_TRUE(p != nullptr);
  ::operator delete(p, n);
}
#endif

TEST(alignedMalloc, examples) {
  auto trial = [](size_t align) {
    auto const ptr = aligned_malloc(1, align);
    auto const addr = reinterpret_cast<uintptr_t>(ptr);
    return (aligned_free(ptr), addr);
  };

  if (!kIsSanitize) { // asan allocator raises SIGABRT instead
    EXPECT_EQ(EINVAL, (trial(2), errno)) << "too small";
    EXPECT_EQ(EINVAL, (trial(513), errno)) << "not power of two";
  }

  EXPECT_EQ(0, trial(512) % 512);
  EXPECT_EQ(0, trial(8192) % 8192);
}

TEST(toSharedPtrAliasing, example) {
  auto sp = folly::copy_to_shared_ptr(std::tuple{3, 4});
  auto a = folly::to_shared_ptr_aliasing(sp, &std::get<1>(*sp));
  EXPECT_EQ(4, *a);
}

TEST(toSharedPtrNonOwning, example) {
  int i = 3;
  auto sp = folly::to_shared_ptr_non_owning(&i);
  EXPECT_EQ(&i, sp.get());
  EXPECT_EQ(3, *sp);
  EXPECT_EQ(0, sp.use_count());
}

TEST(toWeakPtr, example) {
  auto s = std::make_shared<int>(17);
  EXPECT_EQ(1, s.use_count());
  EXPECT_EQ(2, (to_weak_ptr(s).lock(), s.use_count())) << "lvalue";
  EXPECT_EQ(3, (to_weak_ptr(decltype(s)(s)).lock(), s.use_count())) << "rvalue";
}

// These are here to make it easy to double-check the assembly
// for to_weak_ptr_aliasing
extern "C" FOLLY_KEEP void check_to_weak_ptr_aliasing(
    std::shared_ptr<void> const& s, void* a) {
  auto w = folly::to_weak_ptr_aliasing(s, a);
  asm_volatile_memory();
  asm_volatile_pause();
}
extern "C" FOLLY_KEEP void check_to_weak_ptr_aliasing_fallback(
    std::shared_ptr<void> const& s, void* a) {
  auto w = folly::to_weak_ptr(std::shared_ptr<void>(s, a));
  asm_volatile_memory();
  asm_volatile_pause();
}

TEST(toWeakPtrAliasing, active) {
  using S = std::pair<int, int>;

  auto s = std::make_shared<S>();
  s->first = 10;
  s->second = 20;
  EXPECT_EQ(s.use_count(), 1);
  auto w = folly::to_weak_ptr_aliasing(s, &s->second);
  EXPECT_EQ(s.use_count(), 1);
  EXPECT_EQ(*w.lock(), s->second);
  auto locked = w.lock();
  EXPECT_EQ(s.use_count(), 2);
  locked.reset();

  auto w2 = w;
  EXPECT_EQ(*w2.lock(), s->second);
  w2.reset();

  auto w3 = std::move(w);
  EXPECT_EQ(*w3.lock(), s->second);

  std::shared_ptr<int> s2(s, &s->second);
  std::shared_ptr<int> s3 = w3.lock();
  EXPECT_TRUE(s2 == s3);
  EXPECT_FALSE(s2.owner_before(s));
  EXPECT_FALSE(s.owner_before(s2));
  EXPECT_FALSE(w3.owner_before(s));
  EXPECT_FALSE(s.owner_before(w3));

  s.reset();
  s2.reset();
  s3.reset();
  EXPECT_TRUE(w3.expired());
}

TEST(toWeakPtrAliasing, inactive) {
  using S = std::pair<int, int>;

  std::shared_ptr<S> s;
  EXPECT_EQ(s.use_count(), 0);
  S tmp;
  auto w = folly::to_weak_ptr_aliasing(s, &tmp.second);
  EXPECT_EQ(s.use_count(), 0);
  EXPECT_EQ(w.use_count(), 0);
  EXPECT_TRUE(w.expired());
  auto locked = w.lock();
  EXPECT_EQ(locked.get(), nullptr);
  EXPECT_EQ(locked.use_count(), 0);
}

TEST(copyToUniquePtr, example) {
  std::unique_ptr<int> s = copy_to_unique_ptr(17);
  EXPECT_EQ(17, *s);
}

TEST(copyToSharedPtr, example) {
  std::shared_ptr<int> s = copy_to_shared_ptr(17);
  EXPECT_EQ(17, *s);
}

TEST(copyToSharedPtrConst, example) {
  std::shared_ptr<int const> s = copy_to_shared_ptr_const(17);
  EXPECT_EQ(17, *s);
}

TEST(copyThroughUniquePtr, example) {
  std::unique_ptr<int> p = std::make_unique<int>(17);
  std::unique_ptr<int> s = copy_through_unique_ptr(p);
  EXPECT_EQ(17, *s);
  EXPECT_EQ(17, *p);

  p.reset();
  s = copy_through_unique_ptr(p);
  EXPECT_EQ(s, nullptr);
}

TEST(copyThroughSharedPtr, example) {
  std::shared_ptr<int> p = std::make_shared<int>(17);
  std::shared_ptr<int> s = copy_through_shared_ptr(p);
  EXPECT_EQ(17, *s);
  EXPECT_EQ(17, *p);
  EXPECT_EQ(s.use_count(), 1);
  EXPECT_EQ(p.use_count(), 1);
  EXPECT_NE(s.get(), p.get());
  p.reset();
  s = copy_through_shared_ptr(p);
  EXPECT_EQ(s, nullptr);
  EXPECT_EQ(s.use_count(), 0);
  EXPECT_EQ(p.use_count(), 0);
}

TEST(toErasedUniquePtr, example) {
  erased_unique_ptr ptr = empty_erased_unique_ptr();

  ptr = to_erased_unique_ptr(new int(42));
  EXPECT_EQ(42, *static_cast<int*>(ptr.get()));

  ptr = to_erased_unique_ptr(new std::string("foo"));
  EXPECT_EQ("foo", *static_cast<std::string*>(ptr.get()));

  struct {
    int i;
  } s;
  ptr = to_erased_unique_ptr(new decltype(s){42});
  EXPECT_EQ(42, static_cast<decltype(s)*>(ptr.get())->i);

  ptr = to_erased_unique_ptr(std::make_unique<int>(42));
  EXPECT_EQ(42, *static_cast<int*>(ptr.get()));

  ptr = make_erased_unique<std::string>(7, 'a');
  EXPECT_EQ("aaaaaaa", *static_cast<std::string*>(ptr.get()));

  ptr = copy_to_erased_unique_ptr(std::string("bbbbbbb"));
  EXPECT_EQ("bbbbbbb", *static_cast<std::string*>(ptr.get()));
}

TEST(SysAllocator, equality) {
  using Alloc = SysAllocator<float>;
  Alloc const a, b;
  EXPECT_TRUE(a == b);
  EXPECT_FALSE(a != b);
}

TEST(SysAllocator, allocateUnique) {
  using Alloc = SysAllocator<float>;
  Alloc const alloc;
  auto ptr = allocate_unique<float>(alloc, 3.);
  EXPECT_EQ(3., *ptr);
}

TEST(SysAllocator, allocateUniqueDifferentType) {
  using Alloc = SysAllocator<char>;
  Alloc const alloc;
  std::unique_ptr<float, allocator_delete<SysAllocator<float>>> ptr =
      allocate_unique<float>(alloc, 3.);
  EXPECT_EQ(3., *ptr);
}

TEST(SysAllocator, vector) {
  using Alloc = SysAllocator<float>;
  Alloc const alloc;
  std::vector<float, Alloc> nums(alloc);
  nums.push_back(3.);
  nums.push_back(5.);
  EXPECT_THAT(nums, testing::ElementsAreArray({3., 5.}));
}

TEST(SysAllocator, badAlloc) {
  using Alloc = SysAllocator<float>;
  Alloc const alloc;
  std::vector<float, Alloc> nums(alloc);
  if (!kIsSanitize) {
    EXPECT_THROW(nums.reserve(kTooBig), std::bad_alloc);
  }
}

TEST(AlignedSysAllocator, equalityFixed) {
  using Alloc = AlignedSysAllocator<float, FixedAlign<1024>>;
  Alloc const a, b;
  EXPECT_TRUE(a == b);
  EXPECT_FALSE(a != b);
}

TEST(AlignedSysAllocator, allocateUniqueFixed) {
  using Alloc = AlignedSysAllocator<float, FixedAlign<1024>>;
  Alloc const alloc;
  std::unique_ptr<float, allocator_delete<Alloc>> ptr =
      allocate_unique<float>(alloc, 3.);
  EXPECT_EQ(3., *ptr);
  EXPECT_EQ(0, std::uintptr_t(ptr.get()) % 1024);
}

TEST(AlignedSysAllocator, undersizedFixed) {
  constexpr auto align = has_extended_alignment ? 1024 : max_align_v;
  struct alignas(align) Big {
    float value;
  };
  using Alloc = AlignedSysAllocator<Big, FixedAlign<sizeof(void*)>>;
  Alloc const alloc;
  auto ptr = allocate_unique<Big>(alloc, Big{3.});
  EXPECT_EQ(3., ptr->value);
  EXPECT_EQ(0, std::uintptr_t(ptr.get()) % align);
}

TEST(AlignedSysAllocator, vectorFixed) {
  using Alloc = AlignedSysAllocator<float, FixedAlign<1024>>;
  Alloc const alloc;
  std::vector<float, Alloc> nums(alloc);
  nums.push_back(3.);
  nums.push_back(5.);
  EXPECT_THAT(nums, testing::ElementsAreArray({3., 5.}));
  EXPECT_EQ(0, std::uintptr_t(nums.data()) % 1024);
}

TEST(AlignedSysAllocator, badAllocFixed) {
  using Alloc = AlignedSysAllocator<float, FixedAlign<1024>>;
  Alloc const alloc;
  std::vector<float, Alloc> nums(alloc);
  if (!kIsSanitize) {
    EXPECT_THROW(nums.reserve(kTooBig), std::bad_alloc);
  }
}

TEST(AlignedSysAllocator, equalityDefault) {
  using Alloc = AlignedSysAllocator<float>;
  Alloc const a(1024), b(1024), c(512);
  EXPECT_TRUE(a == b);
  EXPECT_FALSE(a != b);
  EXPECT_FALSE(a == c);
  EXPECT_TRUE(a != c);
}

TEST(AlignedSysAllocator, allocateUniqueDefault) {
  using Alloc = AlignedSysAllocator<float>;
  Alloc const alloc(1024);
  auto ptr = allocate_unique<float>(alloc, 3.);
  EXPECT_EQ(3., *ptr);
  EXPECT_EQ(0, std::uintptr_t(ptr.get()) % 1024);
}

TEST(AlignedSysAllocator, undersizedDefault) {
  constexpr auto align = has_extended_alignment ? 1024 : max_align_v;
  struct alignas(align) Big {
    float value;
  };
  using Alloc = AlignedSysAllocator<Big, DefaultAlign>;
  Alloc const alloc(sizeof(void*));
  auto ptr = allocate_unique<Big>(alloc, Big{3.});
  EXPECT_EQ(3., ptr->value);
  EXPECT_EQ(0, std::uintptr_t(ptr.get()) % align);
}

TEST(AlignedSysAllocator, vectorDefault) {
  using Alloc = AlignedSysAllocator<float>;
  Alloc const alloc(1024);
  std::vector<float, Alloc> nums(alloc);
  nums.push_back(3.);
  nums.push_back(5.);
  EXPECT_THAT(nums, testing::ElementsAreArray({3., 5.}));
  EXPECT_EQ(0, std::uintptr_t(nums.data()) % 1024);
}

TEST(AlignedSysAllocator, badAllocDefault) {
  using Alloc = AlignedSysAllocator<float>;
  Alloc const alloc(1024);
  std::vector<float, Alloc> nums(alloc);
  if (!kIsSanitize) {
    EXPECT_THROW(nums.reserve(kTooBig), std::bad_alloc);
  }
}

TEST(AlignedSysAllocator, convertingConstructor) {
  using Alloc1 = AlignedSysAllocator<float>;
  using Alloc2 = AlignedSysAllocator<double>;
  Alloc1 const alloc1(1024);
  Alloc2 const alloc2(alloc1);
}

TEST(allocateSysBuffer, compiles) {
  auto buf = allocate_sys_buffer(256);
  //  Freed at the end of the scope.
}

struct CountedAllocatorStats {
  size_t deallocates = 0;
};

template <typename T>
class CountedAllocator {
 private:
  CountedAllocatorStats* stats_;
  std::allocator<T> alloc;

 public:
  using value_type = T;
  constexpr CountedAllocator(CountedAllocator const&) = default;
  CountedAllocator<T>& operator=(CountedAllocator const&) = default;

  template <typename U>
    requires(!std::is_same_v<U, T>)
  /* implicit */ constexpr CountedAllocator(
      CountedAllocator<U> const& other) noexcept
      : stats_(other.stats_) {}

  explicit CountedAllocator(CountedAllocatorStats& stats) noexcept
      : stats_(&stats) {}

  T* allocate(size_t count) { return alloc.allocate(count); }
  void deallocate(T* p, size_t n) {
    alloc.deallocate(p, n);
    ++stats_->deallocates;
  }
};

template <class T, class U>
bool operator==(const CountedAllocator<T>&, const CountedAllocator<U>&) {
  return true;
}
template <class T, class U>
bool operator!=(const CountedAllocator<T>&, const CountedAllocator<U>&) {
  return false;
}

TEST(allocateUnique, ctorFailure) {
  struct CtorThrows {
    explicit CtorThrows(bool cond) {
      if (cond) {
        throw std::runtime_error("nope");
      }
    }
  };
  using Alloc = CountedAllocator<CtorThrows>;
  using Deleter = allocator_delete<CountedAllocator<CtorThrows>>;
  {
    CountedAllocatorStats stats;
    Alloc const alloc(stats);
    EXPECT_EQ(0, stats.deallocates);
    std::unique_ptr<CtorThrows, Deleter> ptr{nullptr, Deleter{alloc}};
    ptr = allocate_unique<CtorThrows>(alloc, false);
    EXPECT_NE(nullptr, ptr);
    EXPECT_EQ(0, stats.deallocates);
    ptr = nullptr;
    EXPECT_EQ(nullptr, ptr);
    EXPECT_EQ(1, stats.deallocates);
  }
  {
    CountedAllocatorStats stats;
    Alloc const alloc(stats);
    EXPECT_EQ(0, stats.deallocates);
    std::unique_ptr<CtorThrows, Deleter> ptr{nullptr, Deleter{alloc}};
    EXPECT_THROW(
        ptr = allocate_unique<CtorThrows>(alloc, true), std::runtime_error);
    EXPECT_EQ(nullptr, ptr);
    EXPECT_EQ(1, stats.deallocates);
  }
}

namespace {
template <typename T>
struct TestAlloc1 : SysAllocator<T> {
  template <typename U, typename... Args>
  void construct(U* p, Args&&... args) {
    ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
  }
};

template <typename T>
struct TestAlloc2 : TestAlloc1<T> {
  template <typename U>
  void destroy(U* p) {
    p->~U();
  }
};

template <typename T>
struct TestAlloc3 : TestAlloc2<T> {
  using folly_has_default_object_construct = std::true_type;
};

template <typename T>
struct TestAlloc4 : TestAlloc3<T> {
  using folly_has_default_object_destroy = std::true_type;
};

template <typename T>
struct TestAlloc5 : SysAllocator<T> {
  using folly_has_default_object_construct = std::true_type;
  using folly_has_default_object_destroy = std::false_type;
};
} // namespace

TEST(AllocatorObjectLifecycleTraits, compiles) {
  using A = std::allocator<int>;
  using S = std::string;

  static_assert(folly::AllocatorHasDefaultObjectConstruct<A, int, int>::value);
  static_assert(folly::AllocatorHasDefaultObjectConstruct<A, S, S>::value);

  static_assert(folly::AllocatorHasDefaultObjectDestroy<A, int>::value);
  static_assert(folly::AllocatorHasDefaultObjectDestroy<A, S>::value);

  static_assert(
      folly::AllocatorHasDefaultObjectConstruct<
          folly::AlignedSysAllocator<int>,
          int,
          int>::value);
  static_assert(
      folly::AllocatorHasDefaultObjectConstruct<
          folly::AlignedSysAllocator<int>,
          S,
          S>::value);

  static_assert(
      folly::AllocatorHasDefaultObjectDestroy<
          folly::AlignedSysAllocator<int>,
          int>::value);
  static_assert(
      folly::AllocatorHasDefaultObjectDestroy<
          folly::AlignedSysAllocator<int>,
          S>::value);

  static_assert(
      !folly::AllocatorHasDefaultObjectConstruct<TestAlloc1<S>, S, S>::value);
  static_assert(
      folly::AllocatorHasDefaultObjectDestroy<TestAlloc1<S>, S>::value);

  static_assert(
      !folly::AllocatorHasDefaultObjectConstruct<TestAlloc2<S>, S, S>::value);
  static_assert(
      !folly::AllocatorHasDefaultObjectDestroy<TestAlloc2<S>, S>::value);

  static_assert(
      folly::AllocatorHasDefaultObjectConstruct<TestAlloc3<S>, S, S>::value);
  static_assert(
      !folly::AllocatorHasDefaultObjectDestroy<TestAlloc3<S>, S>::value);

  static_assert(
      folly::AllocatorHasDefaultObjectConstruct<TestAlloc4<S>, S, S>::value);
  static_assert(
      folly::AllocatorHasDefaultObjectDestroy<TestAlloc4<S>, S>::value);

  static_assert(
      folly::AllocatorHasDefaultObjectConstruct<TestAlloc5<S>, S, S>::value);
  static_assert(
      !folly::AllocatorHasDefaultObjectDestroy<TestAlloc5<S>, S>::value);
}

template <typename T>
struct ExpectingAlloc {
  using value_type = T;

  ExpectingAlloc(std::size_t expectedSize, std::size_t expectedCount)
      : expectedSize_{expectedSize}, expectedCount_{expectedCount} {}

  template <class U>
  explicit ExpectingAlloc(ExpectingAlloc<U> const& other) noexcept
      : expectedSize_{other.expectedSize_},
        expectedCount_{other.expectedCount_} {}

  T* allocate(std::size_t n) {
    EXPECT_EQ(expectedSize_, sizeof(T));
    EXPECT_EQ(expectedSize_, alignof(T));
    EXPECT_EQ(expectedCount_, n);
    return std::allocator<T>{}.allocate(n);
  }

  void deallocate(T* p, std::size_t n) { std::allocator<T>{}.deallocate(p, n); }

  std::size_t expectedSize_;
  std::size_t expectedCount_;
};

struct alignas(64) OverAlignedType {
  std::array<char, 64> raw_;
};

TEST(allocateOverAligned, notActuallyOver) {
  // allocates 6 bytes with alignment 4, should get turned into an
  // allocation of 2 elements of something of size and alignment 4
  ExpectingAlloc<char> a(4, 2);
  auto p = folly::allocateOverAligned<decltype(a), 4>(a, 6);
  EXPECT_EQ((reinterpret_cast<uintptr_t>(p) % 4), 0);
  folly::deallocateOverAligned<decltype(a), 4>(a, p, 6);
  EXPECT_EQ((folly::allocationBytesForOverAligned<decltype(a), 4>(6)), 8);
}

TEST(allocateOverAligned, manualOverStdAlloc) {
  // allocates 6 bytes with alignment 64 using std::allocator, which will
  // result in a call to aligned_malloc underneath.  We free one directly
  // to check that this is not the padding path
  std::allocator<short> a;
  auto p = folly::allocateOverAligned<decltype(a), 64>(a, 3);
  auto p2 = folly::allocateOverAligned<decltype(a), 64>(a, 3);
  EXPECT_EQ((reinterpret_cast<uintptr_t>(p) % 64), 0);
  folly::deallocateOverAligned<decltype(a), 64>(a, p, 3);
  aligned_free(p2);
  EXPECT_EQ((folly::allocationBytesForOverAligned<decltype(a), 64>(3)), 6);
}

TEST(allocateOverAligned, manualOverCustomAlloc) {
  // allocates 6 byte with alignment 64 using non-standard allocator, which
  // will result in an allocation of 64 + alignof(max_align_t) underneath.
  ExpectingAlloc<short> a(
      alignof(folly::max_align_t), 64 / alignof(folly::max_align_t) + 1);
  auto p = folly::allocateOverAligned<decltype(a), 64>(a, 3);
  EXPECT_EQ((reinterpret_cast<uintptr_t>(p) % 64), 0);
  folly::deallocateOverAligned<decltype(a), 64>(a, p, 3);
  EXPECT_EQ(
      (folly::allocationBytesForOverAligned<decltype(a), 64>(3)),
      64 + alignof(folly::max_align_t));
}

TEST(allocateOverAligned, defaultOverCustomAlloc) {
  ExpectingAlloc<OverAlignedType> a(
      alignof(folly::max_align_t), 128 / alignof(folly::max_align_t));
  auto p = folly::allocateOverAligned(a, 1);
  EXPECT_EQ((reinterpret_cast<uintptr_t>(p) % 64), 0);
  folly::deallocateOverAligned(a, p, 1);
  EXPECT_EQ(folly::allocationBytesForOverAligned<decltype(a)>(1), 128);
}

TEST(sharedPointerMap, testMappingFunction) {
  struct Foo {
    int i;
  };

  auto owner = std::make_shared<Foo>();
  owner->i = 42;

  {
    auto subfield = folly::fmap_shared_ptr_aliasing(owner, [](const auto* foo) {
      return &foo->i;
    });
    EXPECT_EQ(subfield.use_count(), 2);
    EXPECT_EQ(*subfield, 42);
  }

  EXPECT_EQ(owner.use_count(), 1);

  {
    auto subfield = folly::fmap_shared_ptr_aliasing(
        owner, [](const auto* /* foo */) -> const int* { return nullptr; });
    EXPECT_FALSE(subfield);
    EXPECT_EQ(owner.use_count(), 1);
  }

  EXPECT_EQ(owner.use_count(), 1);
}
