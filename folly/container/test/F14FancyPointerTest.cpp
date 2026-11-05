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

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <scoped_allocator>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <folly/portability/GTest.h>

namespace {

// A pointer that stores the distance from itself to its target, so that a
// container using it stays valid when its memory is moved to another address.
// A distance of 1, which no object other than the pointer itself can be at,
// means null.
template <typename T>
class OffsetPtr {
 public:
  using element_type = T;
  using value_type = std::remove_cv_t<T>;
  using difference_type = std::ptrdiff_t;
  using pointer = T*;
  using reference = std::add_lvalue_reference_t<T>;
  using iterator_category = std::random_access_iterator_tag;

  OffsetPtr() noexcept = default;
  /* implicit */ OffsetPtr(std::nullptr_t) noexcept {}
  /* implicit */ OffsetPtr(T* p) noexcept { set(p); }
  OffsetPtr(OffsetPtr const& other) noexcept { set(other.get()); }
  template <typename U>
    requires std::is_convertible_v<U*, T*>
  /* implicit */ OffsetPtr(OffsetPtr<U> const& other) noexcept {
    set(other.get());
  }

  OffsetPtr& operator=(OffsetPtr const& other) noexcept {
    set(other.get());
    return *this;
  }

  template <typename U = T>
    requires(!std::is_void_v<U>)
  static OffsetPtr pointer_to(U& r) noexcept {
    return OffsetPtr(std::addressof(r));
  }

  T* get() const noexcept {
    if (offset_ == kNull) {
      return nullptr;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<T*>(
        reinterpret_cast<std::uintptr_t>(this) + offset_);
  }

  T* operator->() const noexcept { return get(); }
  reference operator*() const noexcept { return *get(); }
  reference operator[](difference_type n) const noexcept { return get()[n]; }
  explicit operator bool() const noexcept { return offset_ != kNull; }

  OffsetPtr& operator+=(difference_type n) noexcept {
    set(get() + n);
    return *this;
  }
  OffsetPtr& operator-=(difference_type n) noexcept {
    set(get() - n);
    return *this;
  }
  OffsetPtr& operator++() noexcept { return *this += 1; }
  OffsetPtr& operator--() noexcept { return *this -= 1; }
  OffsetPtr operator++(int) noexcept {
    OffsetPtr result = *this;
    ++*this;
    return result;
  }
  OffsetPtr operator--(int) noexcept {
    OffsetPtr result = *this;
    --*this;
    return result;
  }

  friend OffsetPtr operator+(OffsetPtr p, difference_type n) noexcept {
    return p += n;
  }
  friend OffsetPtr operator+(difference_type n, OffsetPtr p) noexcept {
    return p += n;
  }
  friend OffsetPtr operator-(OffsetPtr p, difference_type n) noexcept {
    return p -= n;
  }
  friend difference_type operator-(
      OffsetPtr const& a, OffsetPtr const& b) noexcept {
    return a.get() - b.get();
  }

 private:
  static constexpr std::uintptr_t kNull = 1;

  void set(T* p) noexcept {
    offset_ = p
        ? reinterpret_cast<std::uintptr_t>(p) -
            reinterpret_cast<std::uintptr_t>(this)
        : kNull;
  }

  std::uintptr_t offset_ = kNull;
};

template <typename T, typename U>
bool operator==(OffsetPtr<T> const& a, OffsetPtr<U> const& b) noexcept {
  return a.get() == b.get();
}

template <typename T>
bool operator==(OffsetPtr<T> const& a, std::nullptr_t) noexcept {
  return !a;
}

template <typename T, typename U>
std::strong_ordering operator<=>(
    OffsetPtr<T> const& a, OffsetPtr<U> const& b) noexcept {
  return std::compare_three_way{}(a.get(), b.get());
}

// F14 needs its chunks aligned to 16 bytes.
constexpr std::size_t kArenaAlign =
    std::max<std::size_t>(16, alignof(std::max_align_t));

struct ArenaHeader {
  std::size_t size;
  std::size_t used;
};

// Allocates from an arena that starts with an ArenaHeader and never reuses
// memory, which is enough for the few small containers in these tests.
template <typename T>
class ArenaAllocator {
 public:
  using value_type = T;
  using pointer = OffsetPtr<T>;
  using const_pointer = OffsetPtr<T const>;
  using void_pointer = OffsetPtr<void>;
  using const_void_pointer = OffsetPtr<void const>;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;

  explicit ArenaAllocator(ArenaHeader* arena) noexcept : arena_(arena) {}
  template <typename U>
  /* implicit */ ArenaAllocator(ArenaAllocator<U> const& other) noexcept
      : arena_(other.arena_) {}

  pointer allocate(size_type n) {
    static_assert(alignof(T) <= kArenaAlign);
    auto& arena = *arena_;
    auto begin = (arena.used + kArenaAlign - 1) / kArenaAlign * kArenaAlign;
    if (begin > arena.size || n > (arena.size - begin) / sizeof(T)) {
      throw std::bad_alloc();
    }
    arena.used = begin + n * sizeof(T);
    return pointer(
        reinterpret_cast<T*>(reinterpret_cast<char*>(&arena) + begin));
  }

  void deallocate(pointer, size_type) noexcept {}

  friend bool operator==(
      ArenaAllocator const& a, ArenaAllocator const& b) noexcept {
    return a.arena_ == b.arena_;
  }

 private:
  template <typename U>
  friend class ArenaAllocator;

  OffsetPtr<ArenaHeader> arena_;
};

// An arena in heap memory that can be copied to another address.
class Segment {
 public:
  Segment() { new (arena()) ArenaHeader{kSize, sizeof(ArenaHeader)}; }

  // Copies the arena's bytes to another address, as if the same memory were
  // mapped there. Using objects in the copy is outside the C++ object model;
  // this only checks that F14 stores no addresses of arena memory.
  Segment(Segment const& other) {
    std::memcpy(storage_.get(), other.storage_.get(), kSize);
  }
  Segment& operator=(Segment const&) = delete;

  // Zeroes the arena, so that a container reading it through a stale pointer
  // finds only empty chunks.
  void clobber() noexcept { std::memset(storage_.get(), 0, kSize); }

  ArenaHeader* arena() const noexcept {
    return reinterpret_cast<ArenaHeader*>(storage_.get());
  }

  bool contains(void const* p, std::size_t size) const noexcept {
    auto begin = reinterpret_cast<std::uintptr_t>(storage_.get());
    auto addr = reinterpret_cast<std::uintptr_t>(p);
    return begin <= addr && addr + size <= begin + kSize;
  }

  template <typename T, typename... Args>
  T* construct(Args&&... args) {
    auto p = ArenaAllocator<T>(arena()).allocate(1);
    return new (p.get()) T(std::forward<Args>(args)...);
  }

  // Returns where `p`, which points into `other`, is in this arena.
  template <typename T>
  T* translate(Segment const& other, T* p) const noexcept {
    return reinterpret_cast<T*>(
        reinterpret_cast<char*>(storage_.get()) +
        (reinterpret_cast<char*>(p) -
         reinterpret_cast<char*>(other.storage_.get())));
  }

 private:
  struct alignas(kArenaAlign) Block {
    std::byte bytes[kArenaAlign];
  };
  static constexpr std::size_t kSize = 1 << 20;

  std::unique_ptr<Block[]> storage_ =
      std::make_unique_for_overwrite<Block[]>(kSize / sizeof(Block));
};

template <typename K, typename M>
using ArenaF14ValueMap = folly::F14ValueMap<
    K,
    M,
    folly::f14::DefaultHasher<K>,
    folly::f14::DefaultKeyEqual<K>,
    ArenaAllocator<std::pair<K const, M>>>;

template <typename K, typename M>
using ArenaF14NodeMap = folly::F14NodeMap<
    K,
    M,
    folly::f14::DefaultHasher<K>,
    folly::f14::DefaultKeyEqual<K>,
    ArenaAllocator<std::pair<K const, M>>>;

template <typename K, typename M>
using ArenaF14VectorMap = folly::F14VectorMap<
    K,
    M,
    folly::f14::DefaultHasher<K>,
    folly::f14::DefaultKeyEqual<K>,
    ArenaAllocator<std::pair<K const, M>>>;

template <typename K>
using ArenaF14ValueSet = folly::F14ValueSet<
    K,
    folly::f14::DefaultHasher<K>,
    folly::f14::DefaultKeyEqual<K>,
    ArenaAllocator<K>>;

template <typename K>
using ArenaF14NodeSet = folly::F14NodeSet<
    K,
    folly::f14::DefaultHasher<K>,
    folly::f14::DefaultKeyEqual<K>,
    ArenaAllocator<K>>;

template <typename K>
using ArenaF14VectorSet = folly::F14VectorSet<
    K,
    folly::f14::DefaultHasher<K>,
    folly::f14::DefaultKeyEqual<K>,
    ArenaAllocator<K>>;

using ArenaVI = std::vector<int, ArenaAllocator<int>>;
using ArenaVVI = std::
    vector<ArenaVI, std::scoped_allocator_adaptor<ArenaAllocator<ArenaVI>>>;

using ArenaF14ValueI2VVI = folly::F14ValueMap<
    int,
    ArenaVVI,
    folly::f14::DefaultHasher<int>,
    folly::f14::DefaultKeyEqual<int>,
    std::scoped_allocator_adaptor<
        ArenaAllocator<std::pair<int const, ArenaVVI>>>>;

using ArenaF14NodeI2VVI = folly::F14NodeMap<
    int,
    ArenaVVI,
    folly::f14::DefaultHasher<int>,
    folly::f14::DefaultKeyEqual<int>,
    std::scoped_allocator_adaptor<
        ArenaAllocator<std::pair<int const, ArenaVVI>>>>;

using ArenaF14VectorI2VVI = folly::F14VectorMap<
    int,
    ArenaVVI,
    folly::f14::DefaultHasher<int>,
    folly::f14::DefaultKeyEqual<int>,
    std::scoped_allocator_adaptor<
        ArenaAllocator<std::pair<int const, ArenaVVI>>>>;

} // namespace

template <typename M>
void runSimpleMapTest() {
  Segment segment;
  ArenaAllocator<std::pair<int const, int>> alloc{segment.arena()};
  M m{alloc};
  for (int i = 0; i < 20; ++i) {
    m[i] = i * 10;
  }
  EXPECT_EQ(m.size(), 20);
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(m[i], i * 10);
  }
}

template <typename S>
void runSimpleSetTest() {
  Segment segment;
  S s{typename S::allocator_type{segment.arena()}};
  for (int i = 0; i < 20; ++i) {
    s.insert(i);
  }
  EXPECT_EQ(s.size(), 20);
  for (int i = 0; i < 40; ++i) {
    EXPECT_EQ(s.contains(i), (i < 20 ? 1 : 0));
  }
}

TEST(ArenaF14ValueMap, simple) {
  runSimpleMapTest<ArenaF14ValueMap<int, int>>();
}
TEST(ArenaF14NodeMap, simple) {
  runSimpleMapTest<ArenaF14NodeMap<int, int>>();
}
TEST(ArenaF14VectorMap, simple) {
  runSimpleMapTest<ArenaF14VectorMap<int, int>>();
}
TEST(ArenaF14ValueSet, simple) {
  runSimpleSetTest<ArenaF14ValueSet<int>>();
}
TEST(ArenaF14NodeSet, simple) {
  runSimpleSetTest<ArenaF14NodeSet<int>>();
}
TEST(ArenaF14VectorSet, simple) {
  runSimpleSetTest<ArenaF14VectorSet<int>>();
}

template <typename M>
void runRelocatedMapTest() {
  using namespace folly::f14::detail;

  // fallback std::unordered_map on libstdc++ doesn't pass this test
  if (getF14IntrinsicsMode() != F14IntrinsicsMode::None) {
    Segment segment1;
    auto m1 =
        segment1.construct<M>(typename M::allocator_type{segment1.arena()});
    (*m1)[1] = 10;
    (*m1)[2] = 20;

    Segment segment2 = segment1;
    auto m2 = segment2.translate(segment1, m1);
    EXPECT_NE(m1, m2);

    // A pointer into the first arena now finds nothing.
    segment1.clobber();

    EXPECT_EQ(m2->size(), 2);
    EXPECT_EQ(m2->contains(0), 0);
    EXPECT_EQ((*m2)[1], 10);
    EXPECT_FALSE(m2->emplace(std::make_pair(2, 22)).second);
    // Growing allocates through the copied allocator.
    for (int i = 3; i < 40; ++i) {
      (*m2)[i] = i * 10;
    }
    EXPECT_EQ(m2->size(), 39);
    for (int i = 1; i < 40; ++i) {
      EXPECT_EQ(m2->find(i)->second, i * 10);
    }
  }
}

TEST(ArenaF14ValueMap, relocated) {
  runRelocatedMapTest<ArenaF14ValueMap<int, int>>();
}
TEST(ArenaF14NodeMap, relocated) {
  runRelocatedMapTest<ArenaF14NodeMap<int, int>>();
}
TEST(ArenaF14VectorMap, relocated) {
  runRelocatedMapTest<ArenaF14VectorMap<int, int>>();
}

template <typename T>
void checkSingleLocation(
    std::string const& name, Segment const& segment, T const& val) {
  EXPECT_TRUE(segment.contains(&val, sizeof(T))) << name;
}

void checkLocation(
    std::string const& name, Segment const& segment, int const& val) {
  checkSingleLocation(name, segment, val);
}

template <typename A, typename B>
void checkLocation(
    std::string const& name,
    Segment const& segment,
    std::pair<A, B> const& val);

template <typename T>
auto checkLocation(
    std::string const& name, Segment const& segment, T const& val)
    -> std::void_t<decltype(val.begin())>;

template <typename A, typename B>
void checkLocation(
    std::string const& name,
    Segment const& segment,
    std::pair<A, B> const& val) {
  checkSingleLocation(name, segment, val);
  checkLocation(name + ".first", segment, val.first);
  checkLocation(name + ".second", segment, val.second);
}

template <typename T>
auto checkLocation(
    std::string const& name, Segment const& segment, T const& val)
    -> std::void_t<decltype(val.begin())> {
  typename T::allocator_type alloc{segment.arena()};
  EXPECT_TRUE(alloc == val.get_allocator());
  checkSingleLocation(name, segment, val);
  for (auto&& v : val) {
    checkLocation(name + "[]", segment, v);
  }
}

template <typename M>
void runScopedAllocatorTest() {
  Segment segment;
  auto mgr = segment.arena();

  auto vi = segment.construct<ArenaVI>(typename ArenaVI::allocator_type{mgr});
  vi->push_back(10);
  checkLocation("vi", segment, *vi);

  auto vvi =
      segment.construct<ArenaVVI>(typename ArenaVVI::allocator_type{mgr});
  vvi->resize(1);
  vvi->at(0).push_back(2);
  checkLocation("vvi", segment, *vvi);

  auto m = segment.construct<M>(typename M::allocator_type{mgr});
  (*m)[1].emplace_back();
  (*m)[1][0].push_back(3);

  checkLocation("m", segment, *m);
  m->clear();
}

TEST(ArenaF14ValueI2VVI, scopedAllocator) {
  runScopedAllocatorTest<ArenaF14ValueI2VVI>();
}
TEST(ArenaF14NodeI2VVI, scopedAllocator) {
  runScopedAllocatorTest<ArenaF14NodeI2VVI>();
}
TEST(ArenaF14VectorI2VVI, scopedAllocator) {
  runScopedAllocatorTest<ArenaF14VectorI2VVI>();
}

template <typename M>
void runMultiScopeTest() {
  Segment segment1;
  auto mgr1 = segment1.arena();

  Segment segment2;
  auto mgr2 = segment2.arena();

  auto a1 = segment1.construct<M>(typename M::allocator_type{mgr1});
  (*a1)[1].emplace_back();
  (*a1)[1][0].push_back(3);
  auto b1 = segment1.construct<M>(*a1);
  auto c1 = segment1.construct<M>(std::move(*a1));

  auto d2 = segment2.construct<M>(typename M::allocator_type{mgr2});
  (*d2)[10].emplace_back();
  (*d2)[10][0].push_back(6);
  auto e2 = segment2.construct<M>(*d2);
  auto f2 = segment2.construct<M>(*b1, typename M::allocator_type{mgr2});

  checkLocation("a1", segment1, *a1);
  checkLocation("b1", segment1, *b1);
  checkLocation("c1", segment1, *c1);
  checkLocation("d2", segment2, *d2);
  checkLocation("e2", segment2, *e2);
  checkLocation("f2", segment2, *f2);

  EXPECT_EQ(a1->size(), 0);
  EXPECT_FALSE(*a1 == *b1);
  EXPECT_TRUE(*b1 == *c1);
  EXPECT_TRUE(*b1 == *f2);
  EXPECT_EQ(d2->size(), 1);
  EXPECT_EQ(e2->size(), 1);
  EXPECT_FALSE(*e2 == *f2);

  checkLocation("d2", segment2, *d2);

  EXPECT_TRUE(*d2 == *e2);

  f2->clear();
  *f2 = std::move(*d2);

  checkLocation("d2", segment2, *d2);
  checkLocation("f2", segment2, *f2);

  EXPECT_TRUE(*f2 == *e2);
  EXPECT_TRUE(d2->empty());

  {
    using std::swap;
    swap(*a1, *b1);
  }
  checkLocation("a1", segment1, *a1);
  checkLocation("b1", segment1, *b1);
  EXPECT_TRUE(*a1 == *c1);

  *a1 = std::move(*e2);

  EXPECT_TRUE(*f2 == *a1);

  checkLocation("a1", segment1, *a1);
  checkLocation("e2", segment2, *e2);

  auto g2 =
      segment2.construct<M>(std::move(*a1), typename M::allocator_type{mgr2});

  EXPECT_TRUE(*f2 == *g2);

  checkLocation("f2", segment2, *f2);
  checkLocation("g2", segment2, *g2);

  for (auto p : {a1, b1, c1, d2, e2, f2, g2}) {
    p->~M();
  }
}

TEST(ArenaF14ValueI2VVI, multiScope) {
  runMultiScopeTest<ArenaF14ValueI2VVI>();
}
TEST(ArenaF14NodeI2VVI, multiScope) {
  runMultiScopeTest<ArenaF14NodeI2VVI>();
}
TEST(ArenaF14VectorI2VVI, multiScope) {
  runMultiScopeTest<ArenaF14VectorI2VVI>();
}
