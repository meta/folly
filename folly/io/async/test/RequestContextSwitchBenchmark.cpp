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

#include <folly/Benchmark.h>
#include <folly/io/async/Request.h>
#include <folly/portability/GFlags.h>

#include <thread>

using namespace folly;

DEFINE_bool(
    request_context_benchmark_watcher,
    false,
    "Register an owning context watcher before running benchmarks");
DEFINE_bool(
    request_context_benchmark_callbacks,
    false,
    "Include RequestData callbacks when switching contexts");
DEFINE_bool(
    request_context_benchmark_single_threaded,
    false,
    "Keep shared-pointer operations in the single-threaded process mode");

namespace {
class CallbackData : public RequestData {
 public:
  bool hasCallback() override { return true; }
  void onSet() override { doNotOptimizeAway(++calls_); }
  void onUnset() override { doNotOptimizeAway(++calls_); }

 private:
  uint64_t calls_{0};
};

void prepareContext(RequestContext& context) {
  if (FLAGS_request_context_benchmark_callbacks) {
    context.setContextData("benchmark", std::make_unique<CallbackData>());
  }
}

void contextSwitchWatcher(
    const std::shared_ptr<RequestContext>&,
    const std::shared_ptr<RequestContext>&) {}
} // namespace

BENCHMARK(sharedGet, n) {
  BenchmarkSuspender suspend;
  RequestContextScopeGuard ambient;
  suspend.dismiss();
  while (n--) {
    doNotOptimizeAway(RequestContext::get());
  }
  suspend.rehire();
}

BENCHMARK(sharedTryGet, n) {
  BenchmarkSuspender suspend;
  RequestContextScopeGuard ambient;
  suspend.dismiss();
  while (n--) {
    doNotOptimizeAway(RequestContext::try_get());
  }
  suspend.rehire();
}

BENCHMARK(sharedCapture, n) {
  BenchmarkSuspender suspend;
  RequestContextScopeGuard ambient;
  suspend.dismiss();
  while (n--) {
    auto captured = RequestContext::saveContext();
    doNotOptimizeAway(captured);
  }
  suspend.rehire();
}

BENCHMARK(sharedSameContextSet, n) {
  BenchmarkSuspender suspend;
  RequestContextScopeGuard ambient;
  auto context = RequestContext::saveContext();
  suspend.dismiss();
  while (n--) {
    auto previous = RequestContext::setContext(context);
    doNotOptimizeAway(previous);
  }
  suspend.rehire();
}

BENCHMARK(sharedSameContextScopeGuard, n) {
  BenchmarkSuspender suspend;
  RequestContextScopeGuard ambient;
  auto context = RequestContext::saveContext();
  suspend.dismiss();
  while (n--) {
    RequestContextScopeGuard guard(context);
  }
  suspend.rehire();
}

BENCHMARK(sharedSetContext, n) {
  BenchmarkSuspender suspend;
  RequestContextScopeGuard ambient;
  auto context = std::make_shared<RequestContext>();
  prepareContext(*RequestContext::get());
  prepareContext(*context);
  suspend.dismiss();
  while (n--) {
    context = RequestContext::setContext(std::move(context));
  }
  suspend.rehire();
}

BENCHMARK(sharedScopeGuard, n) {
  BenchmarkSuspender suspend;
  RequestContextScopeGuard ambient;
  auto context = std::make_shared<RequestContext>();
  prepareContext(*RequestContext::get());
  prepareContext(*context);
  suspend.dismiss();
  while (n--) {
    RequestContextScopeGuard guard(context);
  }
  suspend.rehire();
}

BENCHMARK(sharedScopeGuardFromEmpty, n) {
  BenchmarkSuspender suspend;
  RequestContextScopeGuard ambient(std::shared_ptr<RequestContext>{});
  auto context = std::make_shared<RequestContext>();
  prepareContext(*context);
  suspend.dismiss();
  while (n--) {
    RequestContextScopeGuard guard(context);
  }
  suspend.rehire();
}

BENCHMARK(sharedCreate, n) {
  while (n--) {
    RequestContextScopeGuard guard;
  }
}

BENCHMARK(sharedCaptureAndSwitch, n) {
  BenchmarkSuspender suspend;
  RequestContextScopeGuard ambient;
  prepareContext(*RequestContext::get());
  suspend.dismiss();
  while (n--) {
    auto saved = RequestContext::saveContext();
    RequestContextScopeGuard empty(std::shared_ptr<RequestContext>{});
    RequestContextScopeGuard guard(saved);
  }
  suspend.rehire();
}

BENCHMARK(sharedCreateWithCapture, n) {
  while (n--) {
    RequestContextScopeGuard guard;
    auto captured = RequestContext::saveContext();
    doNotOptimizeAway(captured);
  }
}

BENCHMARK(localGet, n) {
  BenchmarkSuspender suspend;
  LocalRequestContextScopeGuard ambient;
  suspend.dismiss();
  while (n--) {
    doNotOptimizeAway(RequestContext::get());
  }
  suspend.rehire();
}

BENCHMARK(localTryGet, n) {
  BenchmarkSuspender suspend;
  LocalRequestContextScopeGuard ambient;
  suspend.dismiss();
  while (n--) {
    doNotOptimizeAway(RequestContext::try_get());
  }
  suspend.rehire();
}

BENCHMARK(localSave, n) {
  BenchmarkSuspender suspend;
  LocalRequestContextScopeGuard ambient;
  suspend.dismiss();
  while (n--) {
    auto handle = RequestContext::saveContextLocal();
    doNotOptimizeAway(handle);
  }
  suspend.rehire();
}

BENCHMARK(localCapture, n) {
  BenchmarkSuspender suspend;
  LocalRequestContextScopeGuard ambient;
  auto firstCapture = RequestContext::saveContext();
  suspend.dismiss();
  while (n--) {
    auto captured = RequestContext::saveContext();
    doNotOptimizeAway(captured);
  }
  suspend.rehire();
}

BENCHMARK(localHandleCapture, n) {
  BenchmarkSuspender suspend;
  LocalRequestContextScopeGuard ambient;
  auto handle = RequestContext::saveContextLocal();
  auto firstCapture = handle.capture();
  suspend.dismiss();
  while (n--) {
    auto captured = handle.capture();
    doNotOptimizeAway(captured);
  }
  suspend.rehire();
}

BENCHMARK(borrowedSameContextScopeGuard, n) {
  BenchmarkSuspender suspend;
  LocalRequestContextScopeGuard ambient;
  auto handle = RequestContext::saveContextLocal();
  suspend.dismiss();
  while (n--) {
    BorrowedRequestContextScopeGuard guard(handle);
  }
  suspend.rehire();
}

BENCHMARK(localCreate, n) {
  while (n--) {
    LocalRequestContextScopeGuard guard;
  }
}

BENCHMARK(localCreateWithCapture, n) {
  while (n--) {
    LocalRequestContextScopeGuard guard;
    auto captured = RequestContext::saveContext();
    doNotOptimizeAway(captured);
  }
}

BENCHMARK(borrowedCaptureAndSwitch, n) {
  BenchmarkSuspender suspend;
  LocalRequestContextScopeGuard ambient;
  prepareContext(*RequestContext::get());
  suspend.dismiss();
  while (n--) {
    auto saved = RequestContext::saveContextLocal();
    BorrowedRequestContextScopeGuard empty(RequestContextHandle{});
    BorrowedRequestContextScopeGuard guard(saved);
  }
  suspend.rehire();
}

BENCHMARK(borrowedScopeGuard, n) {
  BenchmarkSuspender suspend;
  LocalRequestContextScopeGuard ambient;
  prepareContext(*RequestContext::get());
  RequestContextHandle handle;
  {
    LocalRequestContextScopeGuard request;
    prepareContext(*RequestContext::get());
    handle = RequestContext::saveContextLocal();
  }
  suspend.dismiss();
  while (n--) {
    BorrowedRequestContextScopeGuard guard(handle);
  }
  suspend.rehire();
}

BENCHMARK(borrowedScopeGuardFromEmpty, n) {
  BenchmarkSuspender suspend;
  RequestContextScopeGuard ambient(std::shared_ptr<RequestContext>{});
  RequestContextHandle handle;
  {
    LocalRequestContextScopeGuard request;
    prepareContext(*RequestContext::get());
    handle = RequestContext::saveContextLocal();
  }
  suspend.dismiss();
  while (n--) {
    BorrowedRequestContextScopeGuard guard(handle);
  }
  suspend.rehire();
}

BENCHMARK(borrowedScopeGuardWithCapture, n) {
  BenchmarkSuspender suspend;
  LocalRequestContextScopeGuard ambient;
  prepareContext(*RequestContext::get());
  RequestContextHandle handle;
  {
    LocalRequestContextScopeGuard request;
    prepareContext(*RequestContext::get());
    handle = RequestContext::saveContextLocal();
  }
  suspend.dismiss();
  while (n--) {
    BorrowedRequestContextScopeGuard guard(handle);
    auto captured = RequestContext::saveContext();
    doNotOptimizeAway(captured);
  }
  suspend.rehire();
}

int main(int argc, char** argv) {
  folly::gflags::ParseCommandLineFlags(&argc, &argv, true);
  if (!FLAGS_request_context_benchmark_single_threaded) {
    std::thread([] {}).join();
  }
  if (FLAGS_request_context_benchmark_watcher) {
    RequestContext::addSetContextWatcher(contextSwitchWatcher);
  }
  runBenchmarks();
}
