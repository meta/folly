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

/**
 * Benchmark comparing frame-pointer unwinder vs libunwind stack trace
 * performance.
 *
 * This benchmark measures the performance of getStackTrace(),
 * getStackTraceSafe(), and getStackTraceHeap() with the frame-pointer
 * unwinder enabled vs disabled (falling back to libunwind).
 *
 * Run with:
 *   FOLLY_FB_UNWINDER_ENABLED=1 buck run \
 *     fbsource//xplat/folly/debugging/symbolizer/test:frame_pointer_unwinder_benchmark
 *
 * To compare FP unwinder vs libunwind, run the benchmark twice:
 *   # With FP unwinder
 *   FOLLY_FB_UNWINDER_ENABLED=1 buck run \
 *     fbsource//xplat/folly/debugging/symbolizer/test:frame_pointer_unwinder_benchmark
 *
 *   # Without FP unwinder (libunwind only)
 *   FOLLY_FB_UNWINDER_ENABLED=0 buck run \
 *     fbsource//xplat/folly/debugging/symbolizer/test:frame_pointer_unwinder_benchmark
 */

#include <folly/Benchmark.h>
#include <folly/CPortability.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Task.h>
#include <folly/debugging/symbolizer/StackTrace.h>
#include <folly/fibers/FiberManager.h>
#include <folly/fibers/SimpleLoopController.h>
#include <folly/init/Init.h>

#include <services_efficiency/backtrace/mapping_range.h>

using namespace folly;
using namespace folly::symbolizer;

namespace {

constexpr size_t kMaxAddresses = 300;

// =============================================================================
// getStackTrace benchmarks
// =============================================================================

unsigned int benchGetStackTrace(size_t nFrames) {
  constexpr unsigned int iters = 1000000;
  uintptr_t addresses[kMaxAddresses];

  CHECK_LE(nFrames, kMaxAddresses);

  for (unsigned int i = 0; i < iters; ++i) {
    ssize_t n = getStackTrace(addresses, nFrames);
    CHECK_NE(n, -1);
    doNotOptimizeAway(n);
  }
  return iters - nFrames;
}

FOLLY_NOINLINE unsigned int recurseGetStackTrace(
    size_t nFrames, size_t remaining) {
  if (remaining == 0) {
    return benchGetStackTrace(nFrames);
  } else {
    return recurseGetStackTrace(nFrames, remaining - 1) + 1;
  }
}

unsigned int setupGetStackTrace(int /* iters */, size_t nFrames) {
  return recurseGetStackTrace(nFrames, nFrames);
}

// =============================================================================
// getStackTraceSafe benchmarks
// =============================================================================

unsigned int benchGetStackTraceSafe(size_t nFrames) {
  constexpr unsigned int iters = 1000000;
  uintptr_t addresses[kMaxAddresses];

  CHECK_LE(nFrames, kMaxAddresses);

  for (unsigned int i = 0; i < iters; ++i) {
    ssize_t n = getStackTraceSafe(addresses, nFrames);
    CHECK_NE(n, -1);
    doNotOptimizeAway(n);
  }
  return iters - nFrames;
}

FOLLY_NOINLINE unsigned int recurseGetStackTraceSafe(
    size_t nFrames, size_t remaining) {
  if (remaining == 0) {
    return benchGetStackTraceSafe(nFrames);
  } else {
    return recurseGetStackTraceSafe(nFrames, remaining - 1) + 1;
  }
}

unsigned int setupGetStackTraceSafe(int /* iters */, size_t nFrames) {
  return recurseGetStackTraceSafe(nFrames, nFrames);
}

// =============================================================================
// getStackTraceHeap benchmarks
// =============================================================================

unsigned int benchGetStackTraceHeap(size_t nFrames) {
  constexpr unsigned int iters = 1000000;
  uintptr_t addresses[kMaxAddresses];

  CHECK_LE(nFrames, kMaxAddresses);

  for (unsigned int i = 0; i < iters; ++i) {
    ssize_t n = getStackTraceHeap(addresses, nFrames);
    CHECK_NE(n, -1);
    doNotOptimizeAway(n);
  }
  return iters - nFrames;
}

FOLLY_NOINLINE unsigned int recurseGetStackTraceHeap(
    size_t nFrames, size_t remaining) {
  if (remaining == 0) {
    return benchGetStackTraceHeap(nFrames);
  } else {
    return recurseGetStackTraceHeap(nFrames, remaining - 1) + 1;
  }
}

unsigned int setupGetStackTraceHeap(int /* iters */, size_t nFrames) {
  return recurseGetStackTraceHeap(nFrames, nFrames);
}

// =============================================================================
// Fiber benchmarks - getStackTraceSafe inside a fiber
// =============================================================================

unsigned int benchFiberGetStackTraceSafe(size_t nFrames) {
  constexpr unsigned int iters = 100000;
  uintptr_t addresses[kMaxAddresses];

  CHECK_LE(nFrames, kMaxAddresses);

  folly::fibers::FiberManager::Options opts;
  folly::fibers::FiberManager manager(
      std::make_unique<folly::fibers::SimpleLoopController>(), opts);
  auto& loopController = dynamic_cast<folly::fibers::SimpleLoopController&>(
      manager.loopController());

  unsigned int successCount = 0;
  manager.addTask([&]() {
    for (unsigned int i = 0; i < iters; ++i) {
      ssize_t n = getStackTraceSafe(addresses, nFrames);
      if (n > 0) {
        ++successCount;
      }
      doNotOptimizeAway(n);
    }
  });

  loopController.loop([&]() { loopController.stop(); });

  CHECK_EQ(successCount, iters);
  return iters - nFrames;
}

FOLLY_NOINLINE unsigned int recurseFiberGetStackTraceSafe(
    size_t nFrames, size_t remaining) {
  if (remaining == 0) {
    return benchFiberGetStackTraceSafe(nFrames);
  } else {
    return recurseFiberGetStackTraceSafe(nFrames, remaining - 1) + 1;
  }
}

unsigned int setupFiberGetStackTraceSafe(int /* iters */, size_t nFrames) {
  return recurseFiberGetStackTraceSafe(nFrames, nFrames);
}

// =============================================================================
// Coroutine benchmarks - getStackTraceSafe inside a coroutine
// =============================================================================

FOLLY_NOINLINE folly::coro::Task<ssize_t> coroGetStackTraceSafe(
    uintptr_t* addresses, size_t maxAddresses) {
  co_return getStackTraceSafe(addresses, maxAddresses);
}

FOLLY_NOINLINE folly::coro::Task<unsigned int> coroRecurseAndBench(
    size_t nFrames, size_t remaining, uintptr_t* addresses) {
  if (remaining == 0) {
    constexpr unsigned int iters = 100000;
    unsigned int successCount = 0;

    for (unsigned int i = 0; i < iters; ++i) {
      ssize_t n = co_await coroGetStackTraceSafe(addresses, nFrames);
      if (n > 0) {
        ++successCount;
      }
      doNotOptimizeAway(n);
    }

    CHECK_EQ(successCount, iters);
    co_return iters - nFrames;
  } else {
    auto result =
        co_await coroRecurseAndBench(nFrames, remaining - 1, addresses);
    co_return result + 1;
  }
}

unsigned int benchCoroGetStackTraceSafe(size_t nFrames) {
  uintptr_t addresses[kMaxAddresses];
  CHECK_LE(nFrames, kMaxAddresses);
  return folly::coro::blockingWait(
      coroRecurseAndBench(nFrames, nFrames, addresses));
}

unsigned int setupCoroGetStackTraceSafe(int /* iters */, size_t nFrames) {
  return benchCoroGetStackTraceSafe(nFrames);
}

} // namespace

// =============================================================================
// Benchmark definitions - getStackTrace (standard)
// =============================================================================
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTrace, 2_frames, 2)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTrace, 4_frames, 4)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTrace, 8_frames, 8)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTrace, 16_frames, 16)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTrace, 32_frames, 32)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTrace, 64_frames, 64)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTrace, 128_frames, 128)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTrace, 256_frames, 256)

BENCHMARK_DRAW_LINE();

// =============================================================================
// Benchmark definitions - getStackTraceSafe (standard)
// =============================================================================
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTraceSafe, 2_frames, 2)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTraceSafe, 4_frames, 4)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTraceSafe, 8_frames, 8)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTraceSafe, 16_frames, 16)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTraceSafe, 32_frames, 32)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTraceSafe, 64_frames, 64)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTraceSafe, 128_frames, 128)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTraceSafe, 256_frames, 256)

BENCHMARK_DRAW_LINE();

// =============================================================================
// Benchmark definitions - getStackTraceHeap (standard)
// =============================================================================
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTraceHeap, 2_frames, 2)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTraceHeap, 4_frames, 4)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTraceHeap, 8_frames, 8)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTraceHeap, 16_frames, 16)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTraceHeap, 32_frames, 32)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTraceHeap, 64_frames, 64)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTraceHeap, 128_frames, 128)
BENCHMARK_NAMED_PARAM_MULTI(setupGetStackTraceHeap, 256_frames, 256)

BENCHMARK_DRAW_LINE();

// =============================================================================
// Benchmark definitions - Fiber context
// =============================================================================
BENCHMARK_NAMED_PARAM_MULTI(setupFiberGetStackTraceSafe, 8_frames, 8)
BENCHMARK_NAMED_PARAM_MULTI(setupFiberGetStackTraceSafe, 32_frames, 32)
BENCHMARK_NAMED_PARAM_MULTI(setupFiberGetStackTraceSafe, 64_frames, 64)
BENCHMARK_NAMED_PARAM_MULTI(setupFiberGetStackTraceSafe, 128_frames, 128)

BENCHMARK_DRAW_LINE();

// =============================================================================
// Benchmark definitions - Coroutine context
// =============================================================================
BENCHMARK_NAMED_PARAM_MULTI(setupCoroGetStackTraceSafe, 8_frames, 8)
BENCHMARK_NAMED_PARAM_MULTI(setupCoroGetStackTraceSafe, 32_frames, 32)
BENCHMARK_NAMED_PARAM_MULTI(setupCoroGetStackTraceSafe, 64_frames, 64)
BENCHMARK_NAMED_PARAM_MULTI(setupCoroGetStackTraceSafe, 128_frames, 128)

int main(int argc, char* argv[]) {
  if (have_proc_map_query()) {
    LOG(INFO) << "PROCMAP_QUERY is available (kernel 6.11+)";
  } else {
    LOG(WARNING)
        << "PROCMAP_QUERY is NOT available - FP unwinder will fall back to libunwind";
  }
  if (std::getenv("FOLLY_FB_UNWINDER_ENABLED")) {
    LOG(INFO) << "FOLLY_FB_UNWINDER_ENABLED="
              << std::getenv("FOLLY_FB_UNWINDER_ENABLED");
  } else {
    LOG(WARNING) << "FOLLY_FB_UNWINDER_ENABLED is not set";
  }

  folly::Init init(&argc, &argv, true);
  folly::runBenchmarks();
  return 0;
}
