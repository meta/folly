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

#include <folly/concurrency/AtomicSharedPtr.h>

#include <array>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <folly/Benchmark.h>
#include <folly/init/Init.h>
#include <folly/synchronization/test/Barrier.h>

namespace {

enum class pointer_kind {
  empty,
  emplace,
  pointer,
  stateful_deleter,
  aliased,
  over_aligned,
  owning_null,
  ownerless,
};

struct alignas(64) aligned_value {
  int value = 7;
};

std::shared_ptr<int> make_value(pointer_kind kind) {
  switch (kind) {
    case pointer_kind::empty:
      return {};
    case pointer_kind::emplace:
      return std::make_shared<int>(7);
    case pointer_kind::pointer:
      return std::shared_ptr<int>(new int(7)); // NOLINT
    case pointer_kind::stateful_deleter:
      return std::shared_ptr<int>(new int(7), [tag = 7](int* p) {
        folly::doNotOptimizeAway(tag);
        delete p;
      });
    case pointer_kind::aliased: {
      auto owner =
          std::make_shared<std::array<int, 2>>(std::array<int, 2>{3, 7});
      return std::shared_ptr<int>(owner, &(*owner)[1]);
    }
    case pointer_kind::over_aligned: {
      auto owner = std::make_shared<aligned_value>();
      return std::shared_ptr<int>(owner, &owner->value);
    }
    case pointer_kind::owning_null:
      return std::shared_ptr<int>(std::make_shared<int>(7), nullptr);
    case pointer_kind::ownerless: {
      static int value = 7;
      return std::shared_ptr<int>(std::shared_ptr<int>{}, &value);
    }
  }
  std::terminate();
}

template <pointer_kind Kind, std::memory_order Order>
void load_value(unsigned iters) {
  folly::BenchmarkSuspender suspend;
  const auto value = make_value(Kind);
  folly::atomic_shared_ptr<int> ptr(value);
  suspend.dismissing([&] {
    while (iters--) {
      auto loaded = ptr.load(Order);
      folly::doNotOptimizeAway(loaded.get());
    }
  });
}

template <pointer_kind Kind, std::memory_order Order>
void store_value(unsigned iters) {
  folly::BenchmarkSuspender suspend;
  const std::array values{make_value(Kind), make_value(Kind)};
  folly::atomic_shared_ptr<int> ptr(values[0]);
  suspend.dismissing([&] {
    for (unsigned i = 0; i < iters; ++i) {
      ptr.store(values[i & 1], Order);
    }
  });
}

template <pointer_kind Kind, std::memory_order Order>
void exchange_value(unsigned iters) {
  folly::BenchmarkSuspender suspend;
  const std::array values{make_value(Kind), make_value(Kind)};
  folly::atomic_shared_ptr<int> ptr(values[0]);
  suspend.dismissing([&] {
    for (unsigned i = 0; i < iters; ++i) {
      auto old = ptr.exchange(values[i & 1], Order);
      folly::doNotOptimizeAway(old.get());
    }
  });
}

template <pointer_kind Kind, bool Match>
void compare_exchange_value(unsigned iters) {
  folly::BenchmarkSuspender suspend;
  const std::array values{make_value(Kind), make_value(Kind)};
  const auto mismatch = std::make_shared<int>(11);
  folly::atomic_shared_ptr<int> ptr(values[0]);
  suspend.dismissing([&] {
    for (unsigned i = 0; i < iters; ++i) {
      auto expected = Match ? values[i & 1] : mismatch;
      const auto& desired = values[(i + 1) & 1];
      auto changed = ptr.compare_exchange_strong(
          expected,
          desired,
          std::memory_order_acq_rel,
          std::memory_order_acquire);
      folly::doNotOptimizeAway(changed);
      folly::doNotOptimizeAway(expected.get());
    }
  });
}

template <pointer_kind Kind, unsigned Readers, unsigned Writers>
unsigned contended_load(unsigned iters) {
  folly::BenchmarkSuspender suspend;
  const std::array values{make_value(Kind), make_value(Kind)};
  folly::atomic_shared_ptr<int> ptr(values[0]);
  folly::test::Barrier start(Readers + Writers + 1);
  folly::test::Barrier finish(Readers + Writers + 1);
  std::vector<std::thread> threads;
  for (unsigned reader = 0; reader < Readers; ++reader) {
    threads.emplace_back([&] {
      start.wait();
      for (unsigned i = 0; i < iters; ++i) {
        auto loaded = ptr.load(std::memory_order_acquire);
        folly::doNotOptimizeAway(loaded.get());
      }
      finish.wait();
    });
  }
  for (unsigned writer = 0; writer < Writers; ++writer) {
    threads.emplace_back([&] {
      start.wait();
      for (unsigned i = 0; i < iters; ++i) {
        ptr.store(values[i & 1], std::memory_order_release);
      }
      finish.wait();
    });
  }
  suspend.dismissing([&] {
    start.wait();
    finish.wait();
  });
  for (auto& thread : threads) {
    thread.join();
  }
  return iters * Readers;
}

#define FOLLY_ATOMIC_SHARED_PTR_BENCHMARKS(kind)                          \
  BENCHMARK(kind##_load_relaxed, iters) {                                 \
    load_value<pointer_kind::kind, std::memory_order_relaxed>(iters);     \
  }                                                                       \
  BENCHMARK(kind##_load_acquire, iters) {                                 \
    load_value<pointer_kind::kind, std::memory_order_acquire>(iters);     \
  }                                                                       \
  BENCHMARK(kind##_load_seq_cst, iters) {                                 \
    load_value<pointer_kind::kind, std::memory_order_seq_cst>(iters);     \
  }                                                                       \
  BENCHMARK(kind##_store_release, iters) {                                \
    store_value<pointer_kind::kind, std::memory_order_release>(iters);    \
  }                                                                       \
  BENCHMARK(kind##_store_seq_cst, iters) {                                \
    store_value<pointer_kind::kind, std::memory_order_seq_cst>(iters);    \
  }                                                                       \
  BENCHMARK(kind##_exchange_acq_rel, iters) {                             \
    exchange_value<pointer_kind::kind, std::memory_order_acq_rel>(iters); \
  }                                                                       \
  BENCHMARK(kind##_cas_success, iters) {                                  \
    compare_exchange_value<pointer_kind::kind, true>(iters);              \
  }                                                                       \
  BENCHMARK(kind##_cas_failure, iters) {                                  \
    compare_exchange_value<pointer_kind::kind, false>(iters);             \
  }

FOLLY_ATOMIC_SHARED_PTR_BENCHMARKS(empty)
FOLLY_ATOMIC_SHARED_PTR_BENCHMARKS(emplace)
FOLLY_ATOMIC_SHARED_PTR_BENCHMARKS(pointer)
FOLLY_ATOMIC_SHARED_PTR_BENCHMARKS(stateful_deleter)
FOLLY_ATOMIC_SHARED_PTR_BENCHMARKS(aliased)
FOLLY_ATOMIC_SHARED_PTR_BENCHMARKS(over_aligned)
FOLLY_ATOMIC_SHARED_PTR_BENCHMARKS(owning_null)
FOLLY_ATOMIC_SHARED_PTR_BENCHMARKS(ownerless)

#undef FOLLY_ATOMIC_SHARED_PTR_BENCHMARKS

BENCHMARK_MULTI(emplace_readers_2, iters) {
  return contended_load<pointer_kind::emplace, 2, 0>(iters);
}
BENCHMARK_MULTI(emplace_readers_8, iters) {
  return contended_load<pointer_kind::emplace, 8, 0>(iters);
}
BENCHMARK_MULTI(emplace_readers_8_writer_1, iters) {
  return contended_load<pointer_kind::emplace, 8, 1>(iters);
}
BENCHMARK_MULTI(aliased_readers_8, iters) {
  return contended_load<pointer_kind::aliased, 8, 0>(iters);
}
BENCHMARK_MULTI(aliased_readers_8_writer_1, iters) {
  return contended_load<pointer_kind::aliased, 8, 1>(iters);
}
BENCHMARK_MULTI(empty_readers_8, iters) {
  return contended_load<pointer_kind::empty, 8, 0>(iters);
}

} // namespace

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  folly::runBenchmarks();
}
