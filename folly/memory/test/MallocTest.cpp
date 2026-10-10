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

#include <folly/memory/Malloc.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

#include <folly/portability/GTest.h>
#include <folly/portability/Malloc.h>

namespace folly {

TEST(MallocTest, standardAllocationAndSizedFree) {
  for (size_t requested : {1, 64, 4096, 65536}) {
    size_t size = goodMallocSize(requested);
    ASSERT_GE(size, requested);
    void* ptr = std::malloc(size);
    ASSERT_NE(ptr, nullptr);
    std::memset(ptr, 0xA5, size);
    sizedFree(ptr, size);
  }
}

TEST(MallocTest, getJEMallocMallctlArenasAll) {
#if FOLLY_HAS_JEMALLOC_DEFS
  EXPECT_EQ(4096, getJEMallocMallctlArenasAll());
#else
  EXPECT_THROW(getJEMallocMallctlArenasAll(), std::logic_error);
#endif
}

TEST(MallocTest, checkedAlignedMallocReturnsAlignedBuffer) {
  for (size_t align : {alignof(std::max_align_t), size_t{64}, size_t{4096}}) {
    void* p = checkedAlignedMalloc(align, align * 4);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % align, 0u);
    sizedAlignedFree(p, align, align * 4);
  }
}

TEST(MallocTest, checkedArrayMallocRoundTrip) {
  struct alignas(64) Over {
    int x;
  };
  constexpr size_t kN = 17;
  Over* p = checkedArrayMalloc<Over>(kN);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % alignof(Over), 0u);
  for (size_t i = 0; i < kN; ++i) {
    p[i].x = static_cast<int>(i);
  }
  for (size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(p[i].x, static_cast<int>(i));
  }
  sizedArrayFree(p, kN);
}

TEST(MallocTest, checkedArrayMallocOverflowThrows) {
  constexpr size_t kHuge =
      std::numeric_limits<size_t>::max() / sizeof(int64_t) + 1;
  EXPECT_THROW(checkedArrayMalloc<int64_t>(kHuge), std::bad_array_new_length);
}

TEST(MallocTest, checkedArrayMallocSmallAlign) {
  // alignof(char) < sizeof(void*), so kAlign is bumped to sizeof(void*).
  // Without rounding totalBytes up, this would violate checkedAlignedMalloc's
  // assert(size % align == 0) precondition.
  char* p = checkedArrayMalloc<char>(3);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % sizeof(void*), 0u);
  p[0] = 'a';
  p[1] = 'b';
  p[2] = 'c';
  EXPECT_EQ(p[0], 'a');
  sizedArrayFree(p, 3);
}

TEST(MallocTest, checkedArrayMallocZero) {
  int* p = checkedArrayMalloc<int>(0);
  EXPECT_NO_THROW(sizedArrayFree(p, 0));
}

} // namespace folly
