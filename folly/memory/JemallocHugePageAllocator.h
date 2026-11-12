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

// http://www.canonware.com/download/jemalloc/jemalloc-latest/doc/jemalloc.html

#pragma once

#include <cstddef>
#include <cstdint>

namespace folly {

template <typename T>
class CxxHugePageAllocator;

/**
 * An allocator which uses Jemalloc to create a dedicated huge page arena,
 * backed by 2MB huge pages (on linux x86-64).
 *
 * This allocator is specifically intended for linux with the transparent
 * huge page support set to 'madvise' and defrag policy set to 'madvise'
 * or 'defer+madvise'.
 * These can be controller via /sys/kernel/mm/transparent_hugepage/enabled
 * and /sys/kernel/mm/transparent_hugepage/defrag.
 *
 * The allocator reserves a fixed-size area using mmap, and sets the
 * MADV_HUGEPAGE page attribute using the madvise system call.
 * A custom jemalloc hook is installed which is called when creating a new
 * extent of memory. This will allocate from the reserved area if possible,
 * and otherwise fall back to the default method.
 * Jemalloc does not use allocated extents across different arenas without
 * first unmapping them, and the advice flags are cleared on munmap.
 * A regular malloc will never end up allocating memory from this arena.
 *
 * If binary isn't linked with jemalloc, the logic falls back to malloc / free.
 *
 * Please note that as per kernel contract, page faults on an madvised region
 * will block, so we pre-allocate all the huge pages by touching the pages.
 * So, please only allocate as much you need as this will never be freed
 * during the lifetime of the application. If we run out of the free huge pages,
 * then huge page allocator falls back to the 4K regular pages.
 *
 * 1GB Huge Pages are not supported at this point.
 */
class JemallocHugePageAllocator {
 public:
  /* initialize with a default number of initial and max pages. */
  static bool default_init();

  static bool init(int initial_nr_pages, int max_nr_pages = 0);

  static void* allocate(size_t size);

  static void* reallocate(void* p, size_t size);

  static void deallocate(void* p, size_t = 0);

  static bool initialized();

  static size_t freeSpace();

  static bool addressInArena(void* address);

 private:
  template <typename T>
  friend class CxxHugePageAllocator;

  static void* allocateAligned(size_t size, size_t alignment);

  static void deallocateAligned(void* p);

  static bool hugePagesAllocSupported();

  static int flags_;
};

// STL compatible huge page allocator, for use with STL-style containers
template <typename T>
class CxxHugePageAllocator {
 private:
  using Self = CxxHugePageAllocator<T>;

 public:
  using value_type = T;

  CxxHugePageAllocator() {}

  template <typename U>
  explicit CxxHugePageAllocator(CxxHugePageAllocator<U> const&) {}

  T* allocate(std::size_t n) {
    if constexpr (alignof(T) > alignof(std::max_align_t)) {
      return static_cast<T*>(JemallocHugePageAllocator::allocateAligned(
          sizeof(T) * n, alignof(T)));
    }
    return static_cast<T*>(JemallocHugePageAllocator::allocate(sizeof(T) * n));
  }
  void deallocate(T* p, std::size_t n) {
    if constexpr (alignof(T) > alignof(std::max_align_t)) {
      JemallocHugePageAllocator::deallocateAligned(p);
      return;
    }
    JemallocHugePageAllocator::deallocate(p, sizeof(T) * n);
  }

  friend bool operator==(Self const&, Self const&) noexcept { return true; }
};

} // namespace folly
