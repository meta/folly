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

#include <folly/FollyMemset.h>

#include <stdint.h>
#include <stdlib.h>
#include <deque>
#include <string>
#include <fmt/core.h>
#include <folly/Benchmark.h>
#include <folly/Preprocessor.h>
#include <folly/portability/GFlags.h>

DEFINE_uint32(min_size, 1, "Minimum size to benchmark");
DEFINE_uint32(max_size, 32768, "Maximum size to benchmark");
DEFINE_bool(linear, false, "Test all sizes [min_size, max_size]");
DEFINE_uint32(step, 1, "Test sizes step");
DEFINE_uint32(page_offset, 0, "Buffer offset from page aligned size");

uint8_t* temp_buf;

size_t getPow2(size_t v) {
  assert(v != 0);
  return 1ULL << (sizeof(size_t) * 8 - __builtin_clzl(v) - 1);
}

template <void* memset_impl(void*, int, size_t)>
void bmMemset(void* buf, size_t length, size_t iters) {
#if !defined(__aarch64__)
  __asm__ volatile(".balign 64\n");
#endif
  FOLLY_PRAGMA_UNROLL_N(1)
  for (size_t i = 0; i < iters; ++i) {
    memset_impl(buf, 0xFF, length);
  }
}

template <void* memset_impl(void*, int, size_t)>
void addMemsetBenchmark(const std::string& name) {
  static std::deque<std::string> names;

  auto addBech = [&](size_t size) {
    names.emplace_back(fmt::format("{}: size={}", name, size));
    folly::addBenchmark(__FILE__, names.back().c_str(), [=](unsigned iters) {
      bmMemset<memset_impl>(temp_buf + FLAGS_page_offset, size, iters);
      return iters;
    });
  };

  if (FLAGS_linear) {
    for (size_t size = FLAGS_min_size; size <= FLAGS_max_size;
         size += FLAGS_step) {
      addBech(size);
    }
  } else {
    for (size_t size = getPow2(FLAGS_min_size); size <= getPow2(FLAGS_max_size);
         size <<= 1) {
      addBech(size);
    }
  }

  /* Draw line. */
  folly::addBenchmark(__FILE__, "-", []() { return 0; });
}

int main(int argc, char** argv) {
  folly::gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  assert(FLAGS_min_size <= FLAGS_max_size);
  assert(FLAGS_page_offset < 4096);
  assert(FLAGS_step > 0);

  size_t totalBufSize = (FLAGS_max_size + FLAGS_page_offset + 4095) & ~4095;
  temp_buf = (uint8_t*)aligned_alloc(4096, totalBufSize);
  // Make sure all pages are allocated
  for (size_t i = 0; i < totalBufSize; i++) {
    temp_buf[i] = 0;
  }

#define BENCHMARK_MEMSET(MEMSET) \
  addMemsetBenchmark<MEMSET>(FOLLY_PP_STRINGIZE(MEMSET));

  BENCHMARK_MEMSET(memset);
  BENCHMARK_MEMSET(folly::__folly_memset);

  folly::runBenchmarks();

  free(temp_buf);

  return 0;
}
