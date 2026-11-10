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

#include <folly/memory/Arena.h>

#include <scoped_allocator>
#include <set>
#include <vector>

#include <glog/logging.h>

#include <folly/Random.h>
#include <folly/memory/Allocator.h>
#include <folly/memory/MallctlHelper.h>
#include <folly/memory/Malloc.h>
#include <folly/portability/GFlags.h>
#include <folly/portability/GTest.h>

using namespace folly;

static_assert(AllocatorHasTrivialDeallocate<SysArena>::value);

void* alloc(SysArena& arena, size_t size) {
  void* const mem = arena.allocate(size);
  // Fill with garbage to detect heap corruption.
  if (size != 0) {
    memset(mem, 0xff, size);
  }
  return mem;
}

TEST(Arena, SizeSanity) {
  std::set<size_t*> allocatedItems;

  static const size_t requestedBlockSize = 64;
  SysArena arena(requestedBlockSize);
  size_t minimum_size = sizeof(SysArena), maximum_size = minimum_size;
  EXPECT_EQ(arena.totalSize(), minimum_size);

  // Insert a single small element to get a new block
  size_t* ptr = static_cast<size_t*>(alloc(arena, sizeof(long)));
  allocatedItems.insert(ptr);
  minimum_size += requestedBlockSize;
  maximum_size += goodMallocSize(requestedBlockSize + SysArena::kBlockOverhead);
  EXPECT_GE(arena.totalSize(), minimum_size);
  EXPECT_LE(arena.totalSize(), maximum_size);
  VLOG(4) << minimum_size << " < " << arena.totalSize() << " < "
          << maximum_size;

  // Insert a larger element, size should be the same
  ptr = static_cast<size_t*>(alloc(arena, requestedBlockSize / 2));
  allocatedItems.insert(ptr);
  EXPECT_GE(arena.totalSize(), minimum_size);
  EXPECT_LE(arena.totalSize(), maximum_size);
  VLOG(4) << minimum_size << " < " << arena.totalSize() << " < "
          << maximum_size;

  // Insert 10 full block sizes to get 10 new blocks
  for (int i = 0; i < 10; i++) {
    ptr = static_cast<size_t*>(alloc(arena, requestedBlockSize));
    allocatedItems.insert(ptr);
  }
  minimum_size += 10 * requestedBlockSize;
  maximum_size +=
      10 * goodMallocSize(requestedBlockSize + SysArena::kBlockOverhead);
  EXPECT_GE(arena.totalSize(), minimum_size);
  EXPECT_LE(arena.totalSize(), maximum_size);
  VLOG(4) << minimum_size << " < " << arena.totalSize() << " < "
          << maximum_size;

  // Insert something huge
  ptr = static_cast<size_t*>(alloc(arena, 10 * requestedBlockSize));
  allocatedItems.insert(ptr);
  minimum_size += 10 * requestedBlockSize;
  maximum_size +=
      goodMallocSize(10 * requestedBlockSize + SysArena::kBlockOverhead);
#if defined(__APPLE__) && defined(FOLLY_AARCH64)
  // expectation is a bit too small
  maximum_size += 8;
#endif
  EXPECT_GE(arena.totalSize(), minimum_size);
  EXPECT_LE(arena.totalSize(), maximum_size);
  VLOG(4) << minimum_size << " < " << arena.totalSize() << " < "
          << maximum_size;

  // Nuke 'em all
  for (const auto& item : allocatedItems) {
    arena.deallocate(item, 0 /* unused */);
  }
  // The total size should be the same
  EXPECT_GE(arena.totalSize(), minimum_size);
  EXPECT_LE(arena.totalSize(), maximum_size);
  VLOG(4) << minimum_size << " < " << arena.totalSize() << " < "
          << maximum_size;
}

TEST(Arena, BytesUsedSanity) {
  static const size_t smallChunkSize = 1024;
  static const size_t blockSize = goodMallocSize(16 * smallChunkSize);
  const size_t bigChunkSize = blockSize - 4 * smallChunkSize;

  size_t bytesUsed = 0;

  SysArena arena(blockSize);
  EXPECT_EQ(arena.bytesUsed(), bytesUsed);

  // Insert 2 small chunks
  alloc(arena, smallChunkSize);
  alloc(arena, smallChunkSize);
  bytesUsed += 2 * smallChunkSize;
  EXPECT_EQ(arena.bytesUsed(), bytesUsed);
  EXPECT_GE(arena.totalSize(), blockSize);
  EXPECT_LE(arena.totalSize(), 2 * blockSize);

  // Insert big chunk, should still fit in one block
  alloc(arena, bigChunkSize);
  bytesUsed += bigChunkSize;
  EXPECT_EQ(arena.bytesUsed(), bytesUsed);
  EXPECT_GE(arena.totalSize(), blockSize);
  EXPECT_LE(arena.totalSize(), 2 * blockSize);

  // Insert big chunk once more, should trigger new block allocation
  alloc(arena, bigChunkSize);
  bytesUsed += bigChunkSize;
  EXPECT_EQ(arena.bytesUsed(), bytesUsed);
  EXPECT_GE(arena.totalSize(), 2 * blockSize);
  EXPECT_LE(arena.totalSize(), 3 * blockSize);

  // Test that bytesUsed() accounts for alignment
  static const size_t tinyChunkSize = 7;
  alloc(arena, tinyChunkSize);
  EXPECT_GE(arena.bytesUsed(), bytesUsed + tinyChunkSize);
  size_t delta = arena.bytesUsed() - bytesUsed;
  EXPECT_EQ(delta & (delta - 1), 0);
}

TEST(Arena, Vector) {
  static const size_t requestedBlockSize = 64;
  SysArena arena(requestedBlockSize);

  EXPECT_EQ(arena.totalSize(), sizeof(SysArena));

  std::vector<size_t, SysArenaAllocator<size_t>> vec{
      {}, SysArenaAllocator<size_t>(arena)};

  for (size_t i = 0; i < 1000; i++) {
    vec.push_back(i);
  }

  for (size_t i = 0; i < 1000; i++) {
    EXPECT_EQ(i, vec[i]);
  }
}

TEST(Arena, FallbackArenaIsDefaultConstructible) {
  std::vector<size_t, FallbackSysArenaAllocator<size_t>> vec;
  EXPECT_NO_THROW(vec.push_back(42));
}

namespace {
static inline int64_t getMallocBytes() {
  uint64_t mallocBytes = 0;
  folly::mallctlRead("thread.allocated", &mallocBytes);
  return mallocBytes;
}

#define EXPECT_HEAP_ALLOC(vec, expect)  \
  {                                     \
    const int64_t c = getMallocBytes(); \
    vec.push_back(42);                  \
    vec.reserve(4);                     \
    if (expect) {                       \
      EXPECT_GT(getMallocBytes(), c);   \
    } else {                            \
      EXPECT_EQ(getMallocBytes(), c);   \
    }                                   \
  }
#define EXPECT_ALLOC(x) EXPECT_HEAP_ALLOC(x, true)
#define EXPECT_NO_ALLOC(x) EXPECT_HEAP_ALLOC(x, false)
} // namespace

TEST(Arena, FallbackSysArenaDoesFallbackToHeap) {
  if (!folly::usingJEMalloc()) {
    return; // Cannot count amount of mallocs :-(
  }
  using FallBackIntAlloc = FallbackSysArenaAllocator<int>;
  using ScopedFbIntAlloc = std::scoped_allocator_adaptor<FallBackIntAlloc>;
  using AdaptedIntAlloc = CxxAllocatorAdaptor<int, SysArena>;

  // Regular arena, uninitialize fallback and initialized fallback
  SysArena arena0;
  FallBackIntAlloc f_no_init;
  FallBackIntAlloc f_do_init(arena0);
  alloc(arena0, 1); // First allocation to prime the arena

  std::vector<int, FallBackIntAlloc> vec_arg_empty__fallback;
  std::vector<int, FallBackIntAlloc> vec_arg_noinit_fallback(f_no_init);
  std::vector<int, FallBackIntAlloc> vec_arg_doinit_fallback(f_do_init);
  std::vector<int, ScopedFbIntAlloc> vec_arg_empty__scoped;
  std::vector<int, ScopedFbIntAlloc> vec_arg_doinit_scoped(arena0);

  auto a0_adapted = AdaptedIntAlloc(arena0);
  std::vector<int, AdaptedIntAlloc> vec_arena_cxx_adapted(a0_adapted);

  EXPECT_ALLOC(vec_arg_empty__fallback);
  EXPECT_ALLOC(vec_arg_noinit_fallback);
  EXPECT_NO_ALLOC(vec_arg_doinit_fallback);
  EXPECT_NO_ALLOC(vec_arena_cxx_adapted);
  EXPECT_ALLOC(vec_arg_empty__scoped);
  EXPECT_NO_ALLOC(vec_arg_doinit_scoped);

  // Compilation of std::vector<T, FallbackSysArenaAllocator<T>> vec
  // 1. vec(arena0); - Should not Compile! ambigous constructor, use explicit
  //    casting vec(FallbackSysArenaAllocator<T>(arena0)) instead
  // 2. std::vector<T, Arena> vec, std::vector<T, SysArena> vec - both do not
  //     compile, use CxxAdaptor or scoped_alloc wrapper for arena as above.
}

TEST(Arena, Compare) {
  SysArena arena1;
  SysArenaAllocator<size_t> alloc1(arena1);
  SysArenaAllocator<size_t> alloc2(arena1);

  EXPECT_EQ(alloc1, alloc2);

  SysArena arena2;
  SysArenaAllocator<size_t> alloc3(arena2);

  EXPECT_NE(alloc1, alloc3);
}

TEST(Arena, SizeLimit) {
  static const size_t requestedBlockSize = sizeof(size_t);
  static const size_t maxSize = 10 * requestedBlockSize;

  SysArena arena(requestedBlockSize, maxSize);

  void* a = alloc(arena, sizeof(size_t));
  EXPECT_TRUE(a != nullptr);
  EXPECT_THROW(alloc(arena, maxSize + 1), std::bad_alloc);
}

TEST(Arena, ExtremeSize) {
  static const size_t requestedBlockSize = sizeof(size_t);

  SysArena arena(requestedBlockSize);

  void* a = alloc(arena, sizeof(size_t));
  EXPECT_TRUE(a != nullptr);
  EXPECT_THROW(alloc(arena, SIZE_MAX - 2), std::bad_alloc);
}

TEST(Arena, MaxAlign) {
  // Results are still aligned with nonstandard size and allocations
  static const size_t blockSize = 123;

  for (const size_t maxAlign : {4, 8, 16, 32, 64}) {
    SCOPED_TRACE(maxAlign);
    SysArena arena(blockSize, SysArena::kNoSizeLimit, maxAlign);

    for (int i = 0; i < 100; i++) {
      void* ptr = alloc(arena, Random::rand32(100));
      EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) & (maxAlign - 1), 0);
    }

    // Reusing blocks also respects alignment
    arena.clear();
    for (int i = 0; i < 100; i++) {
      void* ptr = alloc(arena, Random::rand32(100));
      EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) & (maxAlign - 1), 0);
    }
  }
}

// This used to cause heap corruption due to incorrect allocation size.
TEST(Arena, AllocFullBlock) {
  static const size_t blockSize = 128;

  for (const size_t maxAlign : {4, 8, 16, 32, 64}) {
    SCOPED_TRACE(maxAlign);
    SysArena arena(blockSize, SysArena::kNoSizeLimit, maxAlign);
    alloc(arena, blockSize);
  }
}

TEST(Arena, Clear) {
  static const size_t blockSize = 1024;
  SysArena arena(blockSize);

  // Prime the arena so that at least one regular block exists before the loop.
  // Without this, the first iteration's first pass starts with ptr_=nullptr
  // (empty arena), causing large allocations to go to LargeBlocks. After
  // clear(), ptr_ points to a full-capacity regular block, and those same
  // large allocations go through the fast path instead, shifting addresses.
  alloc(arena, 1);
  arena.clear();

  for (int i = 0; i < 10; ++i) {
    std::vector<size_t> sizes(1000);
    std::generate(sizes.begin(), sizes.end(), []() {
      // Avoid zero-sized allocations: the arena returns nullptr before any
      // block exists, which breaks the address reuse check below.
      return Random::rand32(1, blockSize) * 2;
    });

    std::vector<void*> addresses;
    for (auto s : sizes) {
      addresses.push_back(alloc(arena, s));
    }

    const size_t totalSize = arena.totalSize();
    const size_t bytesUsed = arena.bytesUsed();

    arena.clear();

    int j = 0;
    for (auto s : sizes) {
      auto addr = alloc(arena, s);
      if (s <= blockSize) {
        EXPECT_EQ(addr, addresses[j]);
      }
      j++;
    }

    EXPECT_EQ(arena.bytesUsed(), bytesUsed);
    EXPECT_EQ(arena.totalSize(), totalSize);

    arena.clear();
  }
}

TEST(Arena, ClearAfterLarge) {
  constexpr size_t blockSize = 1024;
  constexpr size_t mult = 10;
  SysArena arena(blockSize);
  EXPECT_EQ(0, arena.bytesUsed());
  alloc(arena, blockSize * mult);
  EXPECT_EQ(blockSize * mult, arena.bytesUsed());
  arena.clear();
  EXPECT_EQ(0, arena.bytesUsed());
}

TEST(Arena, Merge) {
  constexpr size_t blockSize = 1024;
  size_t blockAllocSize = goodMallocSize(blockSize + SysArena::kBlockOverhead);

  SysArena arena1(blockSize);
  SysArena arena2(blockSize);

  alloc(arena1, 16);
  alloc(arena2, 32);

  EXPECT_EQ(blockAllocSize + sizeof(SysArena), arena1.totalSize());
  EXPECT_EQ(blockAllocSize + sizeof(SysArena), arena2.totalSize());
  EXPECT_EQ(16, arena1.bytesUsed());
  EXPECT_EQ(32, arena2.bytesUsed());

  arena1.merge(std::move(arena2));

  EXPECT_EQ(blockAllocSize * 2 + sizeof(SysArena), arena1.totalSize());
  EXPECT_EQ(blockAllocSize * 0 + sizeof(SysArena), arena2.totalSize());
  EXPECT_EQ(48, arena1.bytesUsed());
  EXPECT_EQ(0, arena2.bytesUsed());
}

TEST(Arena, MergeIntoUntouchedArena) {
  SysArena src;
  auto* live = static_cast<char*>(alloc(src, 1));
  *live = 'x';

  // dst has never allocated, so its currentBlock_ is still before_begin().
  SysArena dst;
  dst.merge(std::move(src));

  // The merged block holds live memory; it must not be handed out again, and
  // the allocation below (which fills with 0xff) must not land on top of it.
  EXPECT_NE(live, alloc(dst, 1));
  EXPECT_EQ('x', *live);
}

int main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  folly::gflags::ParseCommandLineFlags(&argc, &argv, true);
  auto ret = RUN_ALL_TESTS();
  return ret;
}
