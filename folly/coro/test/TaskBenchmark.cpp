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
#include <folly/CancellationToken.h>
#include <folly/Portability.h>

#include <folly/coro/AsyncGenerator.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Coroutine.h>
#include <folly/coro/CurrentExecutor.h>
#include <folly/coro/Task.h>
#include <folly/coro/WithCancellation.h>
#include <folly/coro/safe/NowTask.h>
#include <folly/tracing/AsyncStack.h>

#include <memory>

#if FOLLY_HAS_COROUTINES

namespace {

class ResumeBenchmarkCoroutine {
 public:
  struct promise_type {
    ResumeBenchmarkCoroutine get_return_object() noexcept {
      return ResumeBenchmarkCoroutine{
          folly::coro::coroutine_handle<promise_type>::from_promise(*this)};
    }

    folly::coro::suspend_always initial_suspend() noexcept { return {}; }
    folly::coro::suspend_always final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    [[noreturn]] void unhandled_exception() noexcept { std::terminate(); }
  };

  explicit ResumeBenchmarkCoroutine(
      folly::coro::coroutine_handle<promise_type> handle) noexcept
      : handle_(handle) {}

  ResumeBenchmarkCoroutine(const ResumeBenchmarkCoroutine&) = delete;
  ResumeBenchmarkCoroutine& operator=(const ResumeBenchmarkCoroutine&) = delete;

  ~ResumeBenchmarkCoroutine() { handle_.destroy(); }

  folly::coro::coroutine_handle<> handle() const noexcept { return handle_; }

 private:
  folly::coro::coroutine_handle<promise_type> handle_;
};

FOLLY_NOINLINE ResumeBenchmarkCoroutine
resumeAndSuspend(size_t& count, folly::AsyncStackFrame* frame = nullptr) {
  while (true) {
    ++count;
    folly::doNotOptimizeAway(count);
    if (frame != nullptr) {
      folly::deactivateAsyncStackFrame(*frame);
    }
    co_await folly::coro::suspend_always{};
  }
}

FOLLY_NOINLINE folly::coro::Task<void> completeSynchronouslyNoInline(
    size_t& count) {
  ++count;
  folly::doNotOptimizeAway(count);
  co_return;
}

template <typename ReturnTask>
struct InlineCounterTask {
  static ReturnTask run(size_t& count) {
    ++count;
    folly::doNotOptimizeAway(count);
    co_return;
  }
};

template <typename ReturnTask>
struct NoInlineCounterTask {
  FOLLY_NOINLINE static ReturnTask run(size_t& count) {
    ++count;
    folly::doNotOptimizeAway(count);
    co_return;
  }
};

enum class CancellationMode { Inherited, Disabled, Overridden };

template <typename Factory, CancellationMode Mode>
void benchCancellation(size_t iters) {
  folly::BenchmarkSuspender suspender;
  folly::CancellationSource parentSource;
  folly::CancellationSource childSource;
  auto parentToken = parentSource.getToken();
  auto childToken = childSource.getToken();
  size_t count = 0;
  suspender.dismiss();
  folly::coro::blockingWait(
      folly::coro::co_withCancellation(
          parentToken, [&, iters]() -> folly::coro::Task<void> {
            for (size_t i = 0; i < iters; ++i) {
              if constexpr (Mode == CancellationMode::Inherited) {
                co_await Factory::run(count);
              } else if constexpr (Mode == CancellationMode::Disabled) {
                co_await folly::coro::co_withCancellation(
                    folly::CancellationToken{}, Factory::run(count));
              } else {
                co_await folly::coro::co_withCancellation(
                    childToken, Factory::run(count));
              }
            }
          }()));
  suspender.rehire();
  CHECK_EQ(count, iters);
}

using InlineTaskCounter = InlineCounterTask<folly::coro::Task<void>>;
using NoInlineTaskCounter = NoInlineCounterTask<folly::coro::Task<void>>;
using InlineNowTaskCounter = InlineCounterTask<folly::coro::NowTask<void>>;

FOLLY_NOINLINE folly::coro::AsyncGenerator<size_t> counterStream(
    size_t iters, size_t& count) {
  for (size_t i = 0; i < iters; ++i) {
    ++count;
    folly::doNotOptimizeAway(count);
    co_yield i;
  }
}

} // namespace

BENCHMARK(CoroutineHandleResume, iters) {
  folly::BenchmarkSuspender suspender;
  size_t count = 0;
  auto coroutine = resumeAndSuspend(count);
  auto handle = coroutine.handle();
  suspender.dismiss();
  for (size_t i = 0; i < iters; ++i) {
    handle.resume();
  }
  suspender.rehire();
  CHECK_EQ(count, iters);
}

BENCHMARK(CoroutineHandleResumeWithNewAsyncStackRoot, iters) {
  folly::BenchmarkSuspender suspender;
  size_t count = 0;
  folly::AsyncStackFrame frame;
  auto coroutine = resumeAndSuspend(count, &frame);
  auto handle = coroutine.handle();
  suspender.dismiss();
  for (size_t i = 0; i < iters; ++i) {
    folly::resumeCoroutineWithNewAsyncStackRoot(handle, frame);
  }
  suspender.rehire();
  CHECK_EQ(count, iters);
}

BENCHMARK(SingleVoidSynchronousTaskInLoop, iters) {
  size_t count = 0;
  folly::coro::blockingWait([iters, &count]() -> folly::coro::Task<void> {
    auto completeSynchronously =
        [](size_t& counter) -> folly::coro::Task<void> {
      ++counter;
      folly::doNotOptimizeAway(counter);
      co_return;
    };
    for (std::size_t i = 0; i < iters; ++i) {
      co_await completeSynchronously(count);
    }
  }());
  CHECK_EQ(count, iters);
}

BENCHMARK(SingleVoidSynchronousTaskNoInlineInLoop, iters) {
  size_t count = 0;
  folly::coro::blockingWait([iters, &count]() -> folly::coro::Task<void> {
    for (std::size_t i = 0; i < iters; ++i) {
      co_await completeSynchronouslyNoInline(count);
    }
  }());
  CHECK_EQ(count, iters);
}

BENCHMARK(InlineTaskInheritedCancellation, iters) {
  benchCancellation<InlineTaskCounter, CancellationMode::Inherited>(iters);
}

BENCHMARK(InlineTaskDisabledCancellation, iters) {
  benchCancellation<InlineTaskCounter, CancellationMode::Disabled>(iters);
}

BENCHMARK(InlineTaskOverriddenCancellation, iters) {
  benchCancellation<InlineTaskCounter, CancellationMode::Overridden>(iters);
}

BENCHMARK(NoInlineTaskInheritedCancellation, iters) {
  benchCancellation<NoInlineTaskCounter, CancellationMode::Inherited>(iters);
}

BENCHMARK(NoInlineTaskDisabledCancellation, iters) {
  benchCancellation<NoInlineTaskCounter, CancellationMode::Disabled>(iters);
}

BENCHMARK(NoInlineTaskOverriddenCancellation, iters) {
  benchCancellation<NoInlineTaskCounter, CancellationMode::Overridden>(iters);
}

BENCHMARK(NowTaskInheritedCancellation, iters) {
  benchCancellation<InlineNowTaskCounter, CancellationMode::Inherited>(iters);
}

BENCHMARK(NowTaskDisabledCancellation, iters) {
  benchCancellation<InlineNowTaskCounter, CancellationMode::Disabled>(iters);
}

BENCHMARK(NowTaskOverriddenCancellation, iters) {
  benchCancellation<InlineNowTaskCounter, CancellationMode::Overridden>(iters);
}

BENCHMARK(AsyncGeneratorInheritedCancellation, iters) {
  folly::BenchmarkSuspender suspender;
  folly::CancellationSource source;
  auto token = source.getToken();
  size_t count = 0;
  size_t consumed = 0;
  suspender.dismiss();
  folly::coro::blockingWait(
      folly::coro::co_withCancellation(
          token, [iters, &count, &consumed]() -> folly::coro::Task<void> {
            auto generator = counterStream(iters, count);
            while (auto item = co_await generator.next()) {
              folly::doNotOptimizeAway(*item);
              ++consumed;
            }
          }()));
  suspender.rehire();
  CHECK_EQ(count, iters);
  CHECK_EQ(consumed, iters);
}

BENCHMARK(AsyncTaskInLoop, iters) {
  folly::coro::blockingWait([iters]() -> folly::coro::Task<void> {
    auto asyncReschedule = []() -> folly::coro::Task<void> {
      co_await folly::coro::co_reschedule_on_current_executor;
    };
    for (std::size_t i = 0; i < iters; ++i) {
      co_await asyncReschedule();
    }
  }());
}

BENCHMARK(AsyncRescheduleNoTask, iters) {
  folly::coro::blockingWait([iters]() -> folly::coro::Task<void> {
    for (std::size_t i = 0; i < iters; ++i) {
      co_await folly::coro::co_reschedule_on_current_executor;
    }
  }());
}

class Base {
 public:
  virtual ~Base() {}

  virtual folly::coro::Task<int> virtualMethod() = 0;
};

template <int Tag>
class Derived : public Base {
 public:
  FOLLY_NOINLINE
  folly::coro::Task<int> virtualMethod() override { co_return Tag; }
};

FOLLY_NOINLINE
static std::unique_ptr<Base> makeBase(size_t iters) {
  switch (iters % 3) {
    case 0:
      return std::make_unique<Derived<1>>();
    case 1:
      return std::make_unique<Derived<2>>();
    default:
      return std::make_unique<Derived<3>>();
  }
}

BENCHMARK(VirtualTaskMethod, iters) {
  folly::coro::blockingWait([iters]() -> folly::coro::Task<void> {
    auto base = makeBase(iters);
    size_t count = 0;
    for (std::size_t i = 0; i < iters; ++i) {
      count += co_await base->virtualMethod();
    }
    if (count != iters * ((iters % 3) + 1)) {
      std::terminate();
    }
  }());
}

template <size_t N>
static folly::coro::Task<void> staticNestedCalls() {
  if constexpr (N > 0) {
    co_await staticNestedCalls<N - 1>();
  }
  co_return;
}

template <size_t N>
static void benchStaticNestedCalls(size_t iters) {
  folly::coro::blockingWait([iters]() -> folly::coro::Task<void> {
    for (size_t i = 0; i < iters; ++i) {
      co_await staticNestedCalls<N>();
    }
  }());
}

BENCHMARK(StaticNestedCalls3, iters) {
  benchStaticNestedCalls<3>(iters / 3);
}

BENCHMARK(StaticNestedCalls10, iters) {
  benchStaticNestedCalls<10>(iters / 10);
}

static folly::coro::Task<void> nestedCalls(size_t depth) {
  if (depth > 0) {
    co_await nestedCalls(depth - 1);
  }
  co_return;
}

static void benchNestedCalls(size_t depth, size_t iters) {
  folly::coro::blockingWait([depth, iters]() -> folly::coro::Task<void> {
    for (size_t i = 0; i < iters; ++i) {
      co_await nestedCalls(depth);
    }
  }());
}

BENCHMARK(NestedCalls3, iters) {
  benchNestedCalls(3, iters / 3);
}

BENCHMARK(NestedCalls10, iters) {
  benchNestedCalls(10, iters / 10);
}

static void benchNestedCallsWithCancellation(size_t depth, size_t iters) {
  folly::CancellationSource cancelSource;
  folly::coro::blockingWait(
      folly::coro::co_withCancellation(
          cancelSource.getToken(), [depth, iters]() -> folly::coro::Task<void> {
            for (size_t i = 0; i < iters; ++i) {
              co_await nestedCalls(depth);
            }
          }()));
}

BENCHMARK(NestedCallsWithCancellation3, iters) {
  benchNestedCallsWithCancellation(3, iters / 3);
}

BENCHMARK(NestedCallsWithCancellation10, iters) {
  benchNestedCallsWithCancellation(10, iters / 10);
}

#endif // FOLLY_HAS_COROUTINES

int main(int argc, char** argv) {
  folly::gflags::ParseCommandLineFlags(&argc, &argv, true);
  folly::runBenchmarks();
  return 0;
}
