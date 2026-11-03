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

#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

#include <folly/Conv.h>
#include <folly/container/Array.h>
#include <folly/io/async/test/RequestContextHelper.h>
#include <folly/portability/GFlags.h>
#include <folly/synchronization/test/Barrier.h>

DEFINE_int32(reps, 10, "number of reps");
DEFINE_int32(ops, 1000000, "number of operations per rep");

using namespace folly;

RequestToken token("test");

template <typename Func>
inline uint64_t run_once(int nthr, const Func& fn) {
  folly::test::Barrier b1(nthr + 1);
  std::vector<std::thread> thr(nthr);

  for (int tid = 0; tid < nthr; ++tid) {
    thr[tid] = std::thread([&, tid] {
      b1.wait();
      fn(tid);
    });
  }

  b1.wait();
  /* begin time measurement */
  auto const tbegin = std::chrono::steady_clock::now();

  /* wait for completion */
  for (int i = 0; i < nthr; ++i) {
    thr[i].join();
  }

  /* end time measurement */
  auto const tend = std::chrono::steady_clock::now();

  auto const dur = tend - tbegin;
  return std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
}

template <typename Func>
uint64_t runBench(uint64_t ops, int nthr, const Func& fn) {
  uint64_t reps = FLAGS_reps;
  uint64_t min = UINTMAX_MAX;
  uint64_t max = 0;
  uint64_t sum = 0;
  std::vector<uint64_t> durs(reps);

  for (uint64_t r = 0; r < reps; ++r) {
    uint64_t dur = run_once(nthr, fn);
    durs[r] = dur;
    sum += dur;
    min = std::min(min, dur);
    max = std::max(max, dur);
    // if each rep takes too long run at least 3 reps
    const uint64_t minute = 60000000000ULL;
    if (sum > minute && r >= 2) {
      reps = r + 1;
      break;
    }
  }

  const std::string ns_unit = " ns";
  uint64_t avg = sum / reps;
  uint64_t res = min;
  uint64_t varsum = 0;
  for (uint64_t r = 0; r < reps; ++r) {
    auto term = int64_t(reps * durs[r]) - int64_t(sum);
    varsum += term * term;
  }
  uint64_t dev = uint64_t(std::sqrt(varsum) * std::pow(reps, -1.5));
  std::cout << "   " << std::setw(4) << max / ops << ns_unit;
  std::cout << "   " << std::setw(4) << avg / ops << ns_unit;
  std::cout << "   " << std::setw(4) << dev / ops << ns_unit;
  std::cout << "   " << std::setw(4) << res / ops << ns_unit;
  std::cout << std::endl;
  return res;
}

uint64_t bench_set_clearContextData(int nthr, uint64_t ops) {
  auto fn = [&](int tid) {
    RequestContextScopeGuard g;
    for (uint64_t i = tid; i < ops; i += nthr) {
      RequestContext::get()->setContextData(
          token, std::make_unique<TestData>(tid));
      RequestContext::get()->clearContextData(token);
    }
  };
  return runBench(ops, nthr, fn);
}

uint64_t bench_hasContextData(int nthr, uint64_t ops, bool hit) {
  auto fn = [&](int tid) {
    RequestContextScopeGuard g;
    if (hit) {
      RequestContext::get()->setContextData(
          token, std::make_unique<TestData>(tid));
    }
    for (uint64_t i = tid; i < ops; i += nthr) {
      RequestContext::get()->hasContextData(token);
    }
  };
  return runBench(ops, nthr, fn);
}

uint64_t bench_getContextData(int nthr, uint64_t ops, bool hit) {
  auto fn = [&](int tid) {
    RequestContextScopeGuard g;
    if (hit) {
      RequestContext::get()->setContextData(
          token, std::make_unique<TestData>(tid));
    }
    for (uint64_t i = tid; i < ops; i += nthr) {
      RequestContext::get()->getContextData(token);
    }
  };
  return runBench(ops, nthr, fn);
}

uint64_t bench_onSet(int nthr, uint64_t ops, bool nonempty) {
  auto fn = [&](int tid) {
    RequestContextScopeGuard g;
    if (nonempty) {
      RequestContext::get()->setContextData(
          token, std::make_unique<TestData>(tid));
    }
    for (uint64_t i = tid; i < ops; i += nthr) {
      RequestContext::get()->onSet();
    }
  };
  return runBench(ops, nthr, fn);
}

uint64_t bench_onUnset(int nthr, uint64_t ops, bool nonempty) {
  auto fn = [&](int tid) {
    RequestContextScopeGuard g;
    if (nonempty) {
      RequestContext::get()->setContextData(
          token, std::make_unique<TestData>(tid));
    }
    for (uint64_t i = tid; i < ops; i += nthr) {
      RequestContext::get()->onUnset();
    }
  };
  return runBench(ops, nthr, fn);
}

uint64_t bench_setContext(int nthr, uint64_t ops, bool nonempty) {
  auto fn = [&](int tid) {
    auto ctx = std::make_shared<RequestContext>();
    if (nonempty) {
      ctx->setContextData(token, std::make_unique<TestData>(1));
    }
    RequestContext::setContext(std::move(ctx));
    ctx = std::make_shared<RequestContext>();
    if (nonempty) {
      ctx->setContextData(token, std::make_unique<TestData>(2));
    }
    for (uint64_t i = tid; i < ops; i += nthr) {
      ctx = RequestContext::setContext(std::move(ctx));
    }
    RequestContext::setContext(nullptr);
  };
  return runBench(ops, nthr, fn);
}

uint64_t bench_RequestContextScopeGuard(int nthr, uint64_t ops, bool nonempty) {
  auto fn = [&](int tid) {
    RequestContextScopeGuard g1;
    if (nonempty) {
      RequestContext::get()->setContextData(
          token, std::make_unique<TestData>(1));
    }
    auto ctx = std::make_shared<RequestContext>();
    if (nonempty) {
      ctx->setContextData(token, std::make_unique<TestData>(2));
    }
    for (uint64_t i = tid; i < ops; i += nthr) {
      RequestContextScopeGuard g2(ctx);
    }
  };
  return runBench(ops, nthr, fn);
}

uint64_t bench_BorrowedRequestContextScopeGuard(
    int nthr, uint64_t ops, bool capture) {
  auto fn = [&](int tid) {
    LocalRequestContextScopeGuard ambientGuard;
    RequestContextHandle handle;
    {
      LocalRequestContextScopeGuard requestGuard;
      RequestContext::try_get()->setContextData(
          token, std::make_unique<TestData>(tid));
      handle = RequestContext::saveContextLocal();
    }
    for (uint64_t i = tid; i < ops; i += nthr) {
      BorrowedRequestContextScopeGuard guard(handle);
      if (capture) {
        auto saved = RequestContext::saveContext();
      }
    }
  };
  return runBench(ops, nthr, fn);
}

uint64_t bench_ShallowCopyRequestContextScopeGuard(
    int nthr, uint64_t ops, int keep, bool replace) {
  auto fn = [&](int tid) {
    RequestContextScopeGuard g1;
    auto ctx = RequestContext::get();
    for (int i = 0; i < keep; ++i) {
      ctx->setContextData(
          folly::to<std::string>(1000 + i), std::make_unique<TestData>(i));
    }
    if (replace) {
      ctx->setContextData(token, std::make_unique<TestData>(1));
      for (uint64_t i = tid; i < ops; i += nthr) {
        ShallowCopyRequestContextScopeGuard g2(
            token, std::make_unique<TestData>(2));
      }
    } else {
      for (uint64_t i = tid; i < ops; i += nthr) {
        ShallowCopyRequestContextScopeGuard g2;
      }
    }
  };
  return runBench(ops, nthr, fn);
}

void dottedLine() {
  std::cout
      << "........................................................................"
      << std::endl;
}

void doubleLine() {
  std::cout
      << "========================================================================"
      << std::endl;
}

constexpr auto nthr = folly::make_array<int>(1, 10);

void benches() {
  doubleLine();
  std::cout << std::setw(2) << FLAGS_reps << " reps of " << std::setw(8)
            << FLAGS_ops << " operations\n";
  dottedLine();
  std::cout << "$ numactl -N 1 $dir/request_context_benchmark\n";
  doubleLine();
  std::cout
      << "Test name                         Max time  Avg time  Dev time  Min time"
      << std::endl;
  for (int i : nthr) {
    std::cout << "============================== " << std::setw(2) << i
              << " threads " << "==============================" << std::endl;
    const uint64_t ops = FLAGS_ops;
    std::cout << "hasContextData                  ";
    bench_hasContextData(i, ops, true);
    std::cout << "getContextData                  ";
    bench_getContextData(i, ops, true);
    std::cout << "onSet                           ";
    bench_onSet(i, ops, true);
    std::cout << "onUnset                         ";
    bench_onUnset(i, ops, true);
    std::cout << "setContext                      ";
    bench_setContext(i, ops, true);
    std::cout << "RequestContextScopeGuard        ";
    bench_RequestContextScopeGuard(i, ops, true);
    std::cout << "BorrowedRequestContextScopeGuard";
    bench_BorrowedRequestContextScopeGuard(i, ops, false);
    std::cout << "BorrowedScopeGuard+saveContext   ";
    bench_BorrowedRequestContextScopeGuard(i, ops, true);
    std::cout << "ShallowCopyRequestC...-replace  ";
    bench_ShallowCopyRequestContextScopeGuard(i, ops, 0, true);
    std::cout << "ShallowCopyReq...-keep&replace  ";
    bench_ShallowCopyRequestContextScopeGuard(i, ops, 12, true);
  }
  doubleLine();
}

int main(int argc, char** argv) {
  folly::gflags::ParseCommandLineFlags(&argc, &argv, true);
  benches();
}

/*
========================================================================
10 reps of  1000000 operations
........................................................................
$ numactl -N 1 $dir/request_context_benchmark
========================================================================
Test name                         Max time  Avg time  Dev time  Min time
==============================  1 threads ==============================
hasContextData                        7 ns      7 ns      0 ns      7 ns
getContextData                        7 ns      7 ns      0 ns      7 ns
onSet                                12 ns     12 ns      0 ns     12 ns
onUnset                              12 ns     12 ns      0 ns     12 ns
setContext                           46 ns     44 ns      1 ns     42 ns
RequestContextScopeGuard            113 ns    103 ns      3 ns    101 ns
ShallowCopyRequestC...-replace      213 ns    201 ns      5 ns    196 ns
ShallowCopyReq...-keep&replace      883 ns    835 ns     20 ns    814 ns
============================== 10 threads ==============================
hasContextData                        1 ns      1 ns      0 ns      1 ns
getContextData                        2 ns      1 ns      0 ns      1 ns
onSet                                 2 ns      2 ns      0 ns      1 ns
onUnset                               2 ns      2 ns      0 ns      1 ns
setContext                           11 ns      7 ns      2 ns      5 ns
RequestContextScopeGuard             22 ns     15 ns      5 ns     11 ns
ShallowCopyRequestC...-replace       48 ns     30 ns     11 ns     21 ns
ShallowCopyReq...-keep&replace       98 ns     93 ns      2 ns     91 ns
========================================================================
 */
