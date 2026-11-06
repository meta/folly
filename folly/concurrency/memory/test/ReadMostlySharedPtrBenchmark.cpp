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

#include <folly/concurrency/memory/ReadMostlySharedPtr.h>

#include <functional>
#include <iostream>
#include <thread>

#include <folly/Benchmark.h>
#include <folly/Memory.h>
#include <folly/portability/GFlags.h>

template <
    template <typename> class MainPtr,
    template <typename> class WeakPtr,
    size_t threadCount>
void benchmark(size_t n) {
  MainPtr<int> mainPtr(std::make_unique<int>(42));

  std::vector<std::thread> ts;

  for (size_t t = 0; t < threadCount; ++t) {
    ts.emplace_back([&]() {
      WeakPtr<int> weakPtr(mainPtr);
      // Prevent the compiler from hoisting code out of the loop.
      auto op = [&]() FOLLY_NOINLINE { weakPtr.lock(); };

      for (size_t i = 0; i < n; ++i) {
        op();
      }
    });
  }

  for (auto& t : ts) {
    t.join();
  }
}

template <template <typename> class MainPtr>
void constructorBenchmark(size_t n) {
  folly::BenchmarkSuspender braces;
  using deleter_fn = std::identity; // noop deleter
  using uptr = std::unique_ptr<int, deleter_fn>;
  int data = 42;
  std::vector<MainPtr<int>> ptrs;
  ptrs.reserve(n);

  // Only measure the cost of constructing the MainPtr
  braces.dismissing([&] {
    for (size_t i = 0; i < n; ++i) {
      ptrs.push_back(MainPtr<int>(uptr(&data)));
    }
  });
}

template <template <typename> class MainPtr>
void destructorBenchmark(size_t n) {
  folly::BenchmarkSuspender braces;
  std::vector<MainPtr<int>> ptrs;
  using deleter_fn = std::identity; // noop deleter
  using uptr = std::unique_ptr<int, deleter_fn>;
  int data = 42;

  ptrs.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    ptrs.push_back(MainPtr<int>(uptr(&data)));
  }
  // We only want to measure cost of destructing the MainPtr
  braces.dismissing([&] { ptrs.clear(); });
}

template <typename T>
using TLMainPtr = folly::ReadMostlyMainPtr<T, folly::TLRefCount>;
template <typename T>
using TLWeakPtr = folly::ReadMostlyWeakPtr<T, folly::TLRefCount>;

BENCHMARK(WeakPtrOneThread, n) {
  benchmark<std::shared_ptr, std::weak_ptr, 1>(n);
}

BENCHMARK_RELATIVE(TLReadMostlyWeakPtrOneThread, n) {
  benchmark<TLMainPtr, TLWeakPtr, 1>(n);
}

BENCHMARK(WeakPtrFourThreads, n) {
  benchmark<std::shared_ptr, std::weak_ptr, 4>(n);
}

BENCHMARK_RELATIVE(TLReadMostlyWeakPtrFourThreads, n) {
  benchmark<TLMainPtr, TLWeakPtr, 4>(n);
}

/**
 * ReadMostlyMainPtr construction/destruction is significantly more expensive
 * than std::shared_ptr. You should consider using ReadMostly pointers if the
 * MainPtr is created infrequently but shared pointers are copied frequently.
 */

BENCHMARK(SharedPtrCtor, n) {
  constructorBenchmark<std::shared_ptr>(n);
}

BENCHMARK_RELATIVE(TLReadMostlyMainPtrCtor, n) {
  constructorBenchmark<TLMainPtr>(n);
}

BENCHMARK(SharedPtrDtor, n) {
  destructorBenchmark<std::shared_ptr>(n);
}

BENCHMARK_RELATIVE(TLReadMostlyMainPtrDtor, n) {
  destructorBenchmark<TLMainPtr>(n);
}

int main(int argc, char** argv) {
  folly::gflags::ParseCommandLineFlags(&argc, &argv, true);
  folly::gflags::SetCommandLineOptionWithMode(
      "bm_slice_usec", "100000", folly::gflags::SET_FLAG_IF_DEFAULT);

  folly::runBenchmarks();

  return 0;
}
