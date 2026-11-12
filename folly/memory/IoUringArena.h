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

namespace folly {

class IoUringArena {
 public:
  static constexpr unsigned kFixedRegionArenaIndex =
      std::numeric_limits<unsigned>::max();

  static bool init(size_t size);

  static void* allocate(size_t size);

  static void* reallocate(void* p, size_t size);

  static void deallocate(void* p, size_t size = 0);

  static void deallocateOwnedBuffer(void* p, void*) noexcept;

  static bool initialized();

  static bool addressInArena(void* address);

  static void* base();

  static size_t regionSize();

  static size_t freeSpace();

  /**
   * Returns 0 before initialization, the jemalloc arena index when jemalloc is
   * used, or kFixedRegionArenaIndex for the fixed-region fallback.
   */
  static unsigned arenaIndex();

  static int flags();
};

template <typename T>
class CxxIoUringAllocator {
 public:
  using value_type = T;

  CxxIoUringAllocator() = default;

  template <typename U>
  explicit CxxIoUringAllocator(CxxIoUringAllocator<U> const&) {}

  T* allocate(std::size_t n) {
    return static_cast<T*>(IoUringArena::allocate(sizeof(T) * n));
  }

  void deallocate(T* p, std::size_t n) {
    IoUringArena::deallocate(p, sizeof(T) * n);
  }

  friend bool operator==(
      CxxIoUringAllocator const&, CxxIoUringAllocator const&) noexcept {
    return true;
  }
};

} // namespace folly
