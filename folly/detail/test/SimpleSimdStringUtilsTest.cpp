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

#include <folly/detail/SimpleSimdStringUtils.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <folly/ScopeGuard.h>
#include <folly/algorithm/simd/detail/SimdPlatform.h>
#include <folly/detail/SimpleSimdStringUtilsImpl.h>
#include <folly/portability/GTest.h>
#include <folly/portability/SysMman.h>
#include <folly/portability/Unistd.h>

namespace folly {
namespace detail {
namespace {

template <typename Platform>
bool hasSpaceOrCntrlSymbolsForPlatform(std::string_view s) {
  return SimpleSimdStringUtilsImpl<Platform>::hasSpaceOrCntrlSymbols(s);
}

void testHasSpaceOrCntrlSymbols(std::string_view s, bool r) {
  ASSERT_EQ(r, simdHasSpaceOrCntrlSymbols(s)) << s;

  using namespace simd::detail;
  ASSERT_EQ(r, hasSpaceOrCntrlSymbolsForPlatform<void>(s)) << s;

#if FOLLY_SSE_PREREQ(4, 2)
  ASSERT_EQ(
      r, hasSpaceOrCntrlSymbolsForPlatform<SimdSse42Platform<std::uint8_t>>(s))
      << s;
#if defined(__AVX2__)
  ASSERT_EQ(
      r, hasSpaceOrCntrlSymbolsForPlatform<SimdAvx2Platform<std::uint8_t>>(s))
      << s;
#endif
#endif

#if FOLLY_AARCH64
  ASSERT_EQ(
      r,
      hasSpaceOrCntrlSymbolsForPlatform<SimdAarch64Platform<std::uint8_t>>(s))
      << s;
#endif
}

// We also substantially rely on fuzzers for our testing

TEST(SpaceOrCntrl, One) {
  testHasSpaceOrCntrlSymbols("!", false);
}

TEST(SpaceOrCntrl, EachSymbol) {
  for (std::uint16_t uChar = 0;
       uChar <= std::numeric_limits<std::uint8_t>::max();
       ++uChar) {
    bool expected = std::isspace(uChar) || std::iscntrl(uChar);

    char c = static_cast<char>(uChar);
    ASSERT_NO_FATAL_FAILURE(testHasSpaceOrCntrlSymbols({&c, 1u}, expected));
  }
}

TEST(SpaceOrCntrl, ExplicitExamples) {
  using TestCase = std::pair<std::string, bool>;

  std::pair<std::string, bool> testCases[] = {
      TestCase{" ", true},
      TestCase{"123\t", true},
      TestCase{"123", false},
      TestCase{"abA__11", false},
      TestCase{std::string("123") + char(1), true},
  };
  for (const auto& testCase : testCases) {
    ASSERT_NO_FATAL_FAILURE(
        testHasSpaceOrCntrlSymbols(testCase.first, testCase.second));
  }
}

TEST(SpaceOrCntrl, EmptyViews) {
  testHasSpaceOrCntrlSymbols(std::string_view{}, false);
  testHasSpaceOrCntrlSymbols("", false);
  const char control = '\0';
  testHasSpaceOrCntrlSymbols(std::string_view(&control, 0), false);
}

TEST(SpaceOrCntrl, BoundedAlignedSlices) {
  alignas(64) std::array<char, 256> buffer{};
  for (std::size_t offset = 0; offset != 64; ++offset) {
    for (const std::size_t length :
         {1, 15, 16, 17, 31, 32, 33, 127, 128, 129}) {
      buffer.fill('\x80');
      if (offset != 0) {
        buffer[offset - 1] = '\0';
      }
      buffer[offset + length] = '\x7F';
      const std::string_view slice(buffer.data() + offset, length);
      ASSERT_NO_FATAL_FAILURE(testHasSpaceOrCntrlSymbols(slice, false));
      for (const char control : {'\0', '\t', '\x7F'}) {
        buffer[offset + length / 2] = control;
        ASSERT_NO_FATAL_FAILURE(testHasSpaceOrCntrlSymbols(slice, true));
      }
    }
  }
}

#if !defined(__wasi__)
TEST(SpaceOrCntrl, PageBoundary) {
  const auto pageSize = sysconf(_SC_PAGESIZE);
  ASSERT_GT(pageSize, 129);
  const auto size = static_cast<std::size_t>(pageSize);
  void* mapping = mmap(
      nullptr,
      2 * size,
      PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS,
      -1,
      0);
  ASSERT_NE(MAP_FAILED, mapping);
  SCOPE_EXIT {
    munmap(mapping, 2 * size);
  };
  auto* data = static_cast<char*>(mapping);
  std::fill_n(data, size, '\xFF');
  ASSERT_EQ(0, mprotect(data + size, size, PROT_NONE));
  for (std::size_t length = 1; length <= 129; ++length) {
    const std::string_view slice(data + size - length, length);
    ASSERT_NO_FATAL_FAILURE(testHasSpaceOrCntrlSymbols(slice, false));
    data[size - 1] = '\0';
    ASSERT_NO_FATAL_FAILURE(testHasSpaceOrCntrlSymbols(slice, true));
    data[size - 1] = '\xFF';
  }
}
#endif

} // namespace
} // namespace detail
} // namespace folly
