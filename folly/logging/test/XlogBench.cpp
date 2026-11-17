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

#include <folly/logging/xlog.h>

#include <cstddef>
#include <thread>
#include <utility>

#include <folly/synchronization/test/Barrier.h>

#include <folly/Benchmark.h>
#include <folly/init/Init.h>
#include <folly/logging/Init.h>
#include <folly/logging/LogConfig.h>
#include <folly/logging/LogHandler.h>
#include <folly/logging/LogHandlerConfig.h>
#include <folly/logging/LogHandlerFactory.h>
#include <folly/portability/GFlags.h>

DEFINE_int32(num_threads, 4, "Number of threads performing logging");
DEFINE_int32(every_n, 1 << 10, "Inverse frequency for every-n logging");

namespace folly {

// a noop which the compiler cannot see through
// used to prevent some compiler optimizations which could skew benchmarks
[[gnu::weak]] void noop() noexcept {}

namespace {
class NullHandler : public LogHandler {
 public:
  void handleMessage(const LogMessage&, const LogCategory*) override {}
  void flush() override {}
  LogHandlerConfig getConfig() const override { return LogHandlerConfig{""}; }
};
class NullHandlerFactory : public LogHandlerFactory {
 public:
  StringPiece getType() const override { return "null"; }
  std::shared_ptr<LogHandler> createHandler(const Options&) override {
    return std::make_shared<NullHandler>();
  }
};
} // namespace

template <typename XlogEvery>
void runXlogEveryNBench(size_t iters, XlogEvery func) {
  BenchmarkSuspender braces;

  folly::test::Barrier barrier(1 + FLAGS_num_threads);

  std::vector<std::thread> threads(FLAGS_num_threads);

  for (auto& thread : threads) {
    thread = std::thread([&] {
      auto once = [&] {
        noop();
        func();
      };

      // in case the first call does expensive setup, such as take a global lock
      once();

      barrier.wait(); // A - wait for thread start

      barrier.wait(); // B - init the work

      for (auto i = 0u; i < iters; ++i) {
        once();
      }

      barrier.wait(); // C - join the work
    });
  }

  barrier.wait(); // A - wait for thread start

  // we want to exclude thread start/stop operations from measurement
  braces.dismissing([&] {
    barrier.wait(); // B - init the work
    barrier.wait(); // C - join the work
  });

  for (auto& thread : threads) {
    thread.join();
  }
}

BENCHMARK(xlog_every_n, iters) {
  runXlogEveryNBench(iters, [] { //
    XLOG_EVERY_N(INFO, FLAGS_every_n, "hi");
  });
}

BENCHMARK(xlog_every_n_exact, iters) {
  runXlogEveryNBench(iters, [] { //
    XLOG_EVERY_N_EXACT(INFO, FLAGS_every_n, "hi");
  });
}

BENCHMARK(xlog_every_n_thread, iters) {
  runXlogEveryNBench(iters, [] { //
    XLOG_EVERY_N_THREAD(INFO, FLAGS_every_n, "hi");
  });
}

template <std::size_t>
FOLLY_ALWAYS_INLINE void xlogEveryNThreadSite() {
  XLOG_EVERY_N_THREAD(INFO, FLAGS_every_n, "hi");
}

template <std::size_t... I>
FOLLY_ALWAYS_INLINE void xlogEveryNThreadSites(std::index_sequence<I...>) {
  (xlogEveryNThreadSite<I>(), ...);
}

BENCHMARK(xlog_every_n_thread_1_256, iters) {
  runXlogEveryNBench(iters, [] { //
    for (auto i = 0; i < 256; ++i) {
      xlogEveryNThreadSites(std::make_index_sequence<1>{});
    }
  });
}

BENCHMARK(xlog_every_n_thread_16_16, iters) {
  runXlogEveryNBench(iters, [] { //
    for (auto i = 0; i < 16; ++i) {
      xlogEveryNThreadSites(std::make_index_sequence<16>{});
    }
  });
}

BENCHMARK(xlog_every_n_thread_256_1, iters) {
  runXlogEveryNBench(iters, [] { //
    for (auto i = 0; i < 1; ++i) {
      xlogEveryNThreadSites(std::make_index_sequence<256>{});
    }
  });
}

} // namespace folly

FOLLY_INIT_LOGGING_CONFIG(".=INFO:default; default=null");

int main(int argc, char** argv) {
  folly::LoggerDB::get().registerHandlerFactory(
      std::make_unique<folly::NullHandlerFactory>(),
      /* replaceExisting = */ true);
  folly::Init init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
