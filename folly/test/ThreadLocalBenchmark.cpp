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

#include <folly/ThreadLocal.h>

#include <sys/types.h>

#include <numeric>
#include <thread>

#include <glog/logging.h>

#include <folly/Benchmark.h>
#include <folly/lang/Keep.h>
#include <folly/portability/GFlags.h>
#include <folly/synchronization/Latch.h>

using namespace folly;

extern "C" FOLLY_KEEP int* check_folly_thread_local_ptr_get(
    ThreadLocalPtr<int>* tlp) {
  return tlp->get();
}

extern "C" FOLLY_KEEP int* check_folly_thread_local_get(ThreadLocal<int>* tl) {
  return tl->get();
}

struct check_folly_thread_local_iterate_tag;
extern "C" FOLLY_KEEP int check_folly_thread_local_iterate(
    ThreadLocalPtr<int, check_folly_thread_local_iterate_tag> tlp) {
  auto accessor = tlp.accessAllThreads();
  folly::detail::keep_sink_nx();
  auto sum = std::accumulate(accessor.begin(), accessor.end(), 0);
  folly::detail::keep_sink_nx();
  return sum;
}

// Simple reference implementation using pthread_get_specific
template <typename T>
class PThreadGetSpecific {
 public:
  PThreadGetSpecific() : key_(0) { pthread_key_create(&key_, OnThreadExit); }

  T* get() const { return static_cast<T*>(pthread_getspecific(key_)); }

  void reset(T* t) {
    delete get();
    pthread_setspecific(key_, t);
  }
  static void OnThreadExit(void* obj) { delete static_cast<T*>(obj); }

 private:
  pthread_key_t key_;
};

DEFINE_int32(num_threads, 8, "Number simultaneous threads for benchmarks.");

#define REG(var)                                          \
  BENCHMARK(FB_CONCATENATE(BM_mt_, var), iters) {         \
    const int itersPerThread = iters / FLAGS_num_threads; \
    std::vector<std::thread> threads;                     \
    for (int i = 0; i < FLAGS_num_threads; ++i) {         \
      threads.push_back(std::thread([&]() {               \
        var.reset(new int(0));                            \
        for (int j = 0; j < itersPerThread; ++j) {        \
          ++(*var.get());                                 \
        }                                                 \
      }));                                                \
    }                                                     \
    for (auto& t : threads) {                             \
      t.join();                                           \
    }                                                     \
  }

ThreadLocalPtr<int> tlp;
REG(tlp)
PThreadGetSpecific<int> pthread_get_specific;
REG(pthread_get_specific)
BENCHMARK_DRAW_LINE();

struct foo {
  int a{0};
  int b{0};
};

template <typename TL>
void run_multi(uint32_t iters) {
  const int itersPerThread = iters / FLAGS_num_threads;
  std::vector<std::thread> threads;
  TL var;
  for (int i = 0; i < FLAGS_num_threads; ++i) {
    threads.push_back(std::thread([&]() {
      var.reset(new foo);
      for (int j = 0; j < itersPerThread; ++j) {
        ++var.get()->a;
        var.get()->b += var.get()->a;
        --var.get()->a;
        var.get()->b += var.get()->a;
      }
    }));
  }
  for (auto& t : threads) {
    t.join();
  }
}

BENCHMARK(BM_mt_tlp_multi, iters) {
  run_multi<ThreadLocalPtr<foo>>(iters);
}
BENCHMARK(BM_mt_pthread_get_specific_multi, iters) {
  run_multi<PThreadGetSpecific<foo>>(iters);
}
BENCHMARK_DRAW_LINE();

BENCHMARK(BM_tlp_access_all_threads_iterate, iters) {
  struct tag {};
  folly::ThreadLocalPtr<int, tag> var;
  folly::BenchmarkSuspender braces;
  std::vector<std::jthread> threads{folly::to_unsigned(FLAGS_num_threads)};
  folly::Latch prep(FLAGS_num_threads);
  folly::Latch work(FLAGS_num_threads + 1);
  folly::Latch done(1);
  for (auto& th : threads) {
    th = std::jthread([&] {
      var.reset(new int(3));
      prep.count_down();
      work.arrive_and_wait();
      done.wait();
    });
  }
  prep.wait();
  work.arrive_and_wait();
  int sum = 0;
  while (iters--) {
    auto accessor = var.accessAllThreads();
    braces.dismissing([&] { //
      sum += std::accumulate(accessor.begin(), accessor.end(), 0);
    });
  }
  folly::doNotOptimizeAway(sum);
  done.count_down();
}
BENCHMARK_DRAW_LINE();

int main(int argc, char** argv) {
  folly::gflags::ParseCommandLineFlags(&argc, &argv, true);
  folly::gflags::SetCommandLineOptionWithMode(
      "bm_max_iters", "100000000", folly::gflags::SET_FLAG_IF_DEFAULT);
  folly::runBenchmarks();
  return 0;
}

/*
./buck-out/gen/folly/test/thread_local_benchmark --bm_min_iters=10000000
--num_threads=1

============================================================================
folly/test/ThreadLocalBenchmark.cpp             relative  time/iter  iters/s
============================================================================
BM_mt_tlp                                                   2.10ns   476.69M
BM_mt_pthread_get_specific                                  1.81ns   553.11M
----------------------------------------------------------------------------
BM_mt_tlp_multi                                            11.91ns    83.94M
BM_mt_pthread_get_specific_multi                           13.34ns    74.96M
----------------------------------------------------------------------------
BM_tlp_access_all_threads_iterate                          22.82ns    43.82M
----------------------------------------------------------------------------
*/
