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

#include <folly/Portability.h>

#include <folly/CancellationToken.h>
#include <folly/ScopeGuard.h>
#include <folly/coro/AsyncGenerator.h>
#include <folly/coro/Baton.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/coro/CurrentExecutor.h>
#include <folly/coro/Invoke.h>
#include <folly/coro/Merge.h>
#include <folly/coro/Task.h>
#include <folly/executors/CPUThreadPoolExecutor.h>

#include <folly/portability/GTest.h>

#if FOLLY_HAS_COROUTINES

using namespace folly::coro;
using namespace std::chrono_literals;

class MergeTest : public testing::Test {};

TEST_F(MergeTest, SimpleMerge) {
  blockingWait([]() -> Task<void> {
    auto generator = merge(
        co_await co_current_executor,
        []() -> AsyncGenerator<AsyncGenerator<int>> {
          auto makeGenerator = [](int start, int count) -> AsyncGenerator<int> {
            for (int i = start; i < start + count; ++i) {
              co_yield i;
              co_await co_reschedule_on_current_executor;
            }
          };

          co_yield makeGenerator(0, 3);
          co_yield makeGenerator(3, 2);
        }());

    const std::array<int, 5> expectedValues = {{0, 3, 1, 4, 2}};

    auto item = co_await generator.next();
    for (int expectedValue : expectedValues) {
      CHECK(!!item);
      CHECK_EQ(expectedValue, *item);
      item = co_await generator.next();
    }
    CHECK(!item);
  }());
}

TEST_F(MergeTest, TruncateStream) {
  blockingWait([]() -> Task<void> {
    int started = 0;
    int completed = 0;
    {
      auto generator = merge(
          co_await co_current_executor,
          co_invoke([&]() -> AsyncGenerator<AsyncGenerator<int>> {
            auto makeGenerator = [&]() -> AsyncGenerator<int> {
              ++started;
              SCOPE_EXIT {
                ++completed;
              };
              co_yield 1;
              co_await co_reschedule_on_current_executor;
              co_yield 2;
            };

            co_yield co_invoke(makeGenerator);
            co_yield co_invoke(makeGenerator);
            co_yield co_invoke(makeGenerator);
          }));

      auto item = co_await generator.next();
      CHECK_EQ(1, *item);
      item = co_await generator.next();
      CHECK_EQ(1, *item);
      CHECK_EQ(3, started);
      // Truncate the stream after consuming only 2 of the 6 values it
      // would have produced.
    }

    // Spin the executor until the generators finish responding to cancellation.
    for (int i = 0; completed != started && i < 10; ++i) {
      co_await co_reschedule_on_current_executor;
    }

    CHECK_EQ(3, completed);
  }());
}

TEST_F(MergeTest, TruncateStreamMultiThreaded) {
  blockingWait([]() -> Task<void> {
    std::atomic<int> started = 0;
    std::atomic<int> completed = 0;
    folly::coro::Baton allStarted;
    folly::coro::Baton allCompleted;
    {
      auto generator = merge(
          folly::getGlobalCPUExecutor(),
          co_invoke([&]() -> AsyncGenerator<AsyncGenerator<int>> {
            auto makeGenerator = [&]() -> AsyncGenerator<int> {
              SCOPE_EXIT {
                if (++completed == 3) {
                  allCompleted.post();
                }
              };
              if (++started == 3) {
                allStarted.post();
              }
              co_yield 1;
              co_yield 2;
            };

            co_yield co_invoke(makeGenerator);
            co_yield co_invoke(makeGenerator);
            co_yield co_invoke(makeGenerator);
          }));

      auto item = co_await generator.next();
      CHECK_EQ(1, *item);
      co_await generator.next();
      co_await allStarted;
      CHECK_EQ(3, started.load());
      // Truncate the stream after consuming only 2 of the 6 values it
      // would have produced.
    }

    co_await allCompleted;
    CHECK_EQ(3, completed.load());
  }());
}

TEST_F(MergeTest, SequencesOfRValueReferences) {
  blockingWait([]() -> Task<void> {
    auto makeStreamOfStreams =
        []() -> AsyncGenerator<AsyncGenerator<std::vector<int>&&>> {
      auto makeStreamOfVectors = []() -> AsyncGenerator<std::vector<int>&&> {
        co_yield std::vector{1, 2, 3};
        co_await co_reschedule_on_current_executor;
        co_yield std::vector{2, 4, 6};
      };

      co_yield makeStreamOfVectors();
      co_yield makeStreamOfVectors();
    };

    auto gen = merge(co_await co_current_executor, makeStreamOfStreams());
    int resultCount = 0;
    while (auto item = co_await gen.next()) {
      ++resultCount;
      std::vector<int>&& v = *item;
      CHECK_EQ(3, v.size());
    }
    CHECK_EQ(4, resultCount);
  }());
}

TEST_F(MergeTest, SequencesOfLValueReferences) {
  blockingWait([]() -> Task<void> {
    auto makeStreamOfStreams =
        []() -> AsyncGenerator<AsyncGenerator<std::vector<int>&>> {
      auto makeStreamOfVectors = []() -> AsyncGenerator<std::vector<int>&> {
        std::vector<int> v{1, 2, 3};
        co_yield v;
        CHECK_EQ(4, v.size());
        co_await co_reschedule_on_current_executor;
        v.push_back(v.back());
        co_yield v;
      };

      co_yield makeStreamOfVectors();
      co_yield makeStreamOfVectors();
    };

    auto gen = merge(co_await co_current_executor, makeStreamOfStreams());
    int resultCount = 0;
    while (auto item = co_await gen.next()) {
      ++resultCount;
      std::vector<int>& v = *item;
      if (v.size() == 3) {
        CHECK_EQ(1, v[0]);
        CHECK_EQ(2, v[1]);
        CHECK_EQ(3, v[2]);
        v.push_back(7);
      } else {
        CHECK_EQ(5, v.size());
        CHECK_EQ(1, v[0]);
        CHECK_EQ(2, v[1]);
        CHECK_EQ(3, v[2]);
        CHECK_EQ(7, v[3]);
        CHECK_EQ(7, v[4]);
      }
    }
    CHECK_EQ(4, resultCount);
  }());
}

template <typename Ref, typename Value = std::remove_cvref_t<Ref>>
folly::coro::AsyncGenerator<Ref, Value> neverStream() {
  folly::coro::Baton baton;
  folly::CancellationCallback cb{
      co_await folly::coro::co_current_cancellation_token,
      [&] { baton.post(); }};
  co_await baton;
}

TEST_F(MergeTest, CancellationTokenPropagatesToOuterFromConsumer) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    folly::CancellationSource cancelSource;
    bool suspended = false;
    bool done = false;
    co_await folly::coro::collectAll(
        folly::coro::co_withCancellation(
            cancelSource.getToken(),
            [&]() -> folly::coro::Task<void> {
              auto stream = merge(
                  co_await co_current_executor,
                  neverStream<AsyncGenerator<int>>());
              suspended = true;
              auto result = co_await stream.next();
              CHECK(!result.has_value());
              done = true;
            }()),
        [&]() -> folly::coro::Task<void> {
          co_await folly::coro::co_reschedule_on_current_executor;
          co_await folly::coro::co_reschedule_on_current_executor;
          co_await folly::coro::co_reschedule_on_current_executor;
          CHECK(suspended);
          CHECK(!done);
          cancelSource.requestCancellation();
        }());
    CHECK(done);
  }());
}

TEST_F(MergeTest, CancellationTokenPropagatesToInnerFromConsumer) {
  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    folly::CancellationSource cancelSource;
    bool suspended = false;
    bool done = false;
    auto makeStreamOfStreams = []() -> AsyncGenerator<AsyncGenerator<int>> {
      co_yield neverStream<int>();
      co_yield neverStream<int>();
    };

    co_await folly::coro::collectAll(
        folly::coro::co_withCancellation(
            cancelSource.getToken(),
            [&]() -> folly::coro::Task<void> {
              auto stream =
                  merge(co_await co_current_executor, makeStreamOfStreams());
              suspended = true;
              auto result = co_await stream.next();
              CHECK(!result.has_value());
              done = true;
            }()),
        [&]() -> folly::coro::Task<void> {
          co_await folly::coro::co_reschedule_on_current_executor;
          co_await folly::coro::co_reschedule_on_current_executor;
          co_await folly::coro::co_reschedule_on_current_executor;
          CHECK(suspended);
          CHECK(!done);
          cancelSource.requestCancellation();
        }());
    CHECK(done);
  }());
}

// Check that by the time merged generator's next() returns an empty value
// (end of stream) or throws an exception all source generators are destroyed.
TEST_F(MergeTest, SourcesAreDestroyedBeforeEof) {
  std::atomic<int> runningSourceGenerators = 0;
  std::atomic<int> runningListGenerators = 0;

  auto sourceGenerator =
      [&](bool shouldThrow) -> folly::coro::AsyncGenerator<int> {
    ++runningSourceGenerators;
    SCOPE_EXIT {
      --runningSourceGenerators;
    };
    co_await folly::coro::co_reschedule_on_current_executor;
    co_yield 42;
    co_await folly::coro::co_reschedule_on_current_executor;
    if (shouldThrow) {
      throw std::runtime_error("test exception");
    }
  };

  auto listGenerator = [&](bool shouldThrow)
      -> folly::coro::AsyncGenerator<folly::coro::AsyncGenerator<int>> {
    CHECK(runningListGenerators == 0);
    ++runningListGenerators;
    SCOPE_EXIT {
      /* sleep override */
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      --runningListGenerators;
    };
    for (int i = 0;; ++i) {
      co_await folly::coro::co_reschedule_on_current_executor;
      co_yield sourceGenerator(shouldThrow && (i % 2 == 1));
    }
  };

  folly::CPUThreadPoolExecutor exec(4);

  // Stream interrupted by cancellation.
  auto future =
      co_withExecutor(
          &exec, folly::coro::co_invoke([&]() -> folly::coro::Task<void> {
            auto gen = folly::coro::merge(
                &exec, listGenerator(/* shouldThrow */ false));
            folly::CancellationSource cancelSource;
            auto r = co_await folly::coro::co_withCancellation(
                cancelSource.getToken(), gen.next());
            CHECK(r.has_value());
            CHECK_EQ(*r, 42);
            CHECK_GT(
                runningSourceGenerators.load() + runningListGenerators.load(),
                0);
            cancelSource.requestCancellation();
            // Currently the merged generator discards items produced
            // after cancellation. But this behavior is not important, and
            // it would probably be equally fine to return them (but stop
            // calling source generators for more), so this test accepts
            // either behavior.
            while (true) {
              r = co_await folly::coro::co_withCancellation(
                  cancelSource.getToken(), gen.next());
              if (!r.has_value()) {
                break;
              }
              CHECK_EQ(*r, 42);
            }
            CHECK_EQ(runningSourceGenerators.load(), 0);
            CHECK_EQ(runningListGenerators.load(), 0);
          }))
          .start();
  std::move(future).get();

  // Stream interrupted by exception.
  future =
      co_withExecutor(
          &exec, folly::coro::co_invoke([&]() -> folly::coro::Task<void> {
            auto gen = folly::coro::merge(
                &exec, listGenerator(/* shouldThrow */ true));
            auto r = co_await gen.next();
            CHECK(r.has_value());
            CHECK_EQ(*r, 42);
            CHECK_GT(
                runningSourceGenerators.load() + runningListGenerators.load(),
                0);
            while (true) {
              auto r2 = co_await folly::coro::co_awaitTry(gen.next());
              if (!r2.hasValue()) {
                CHECK(
                    r2.exception().what().find("test exception") !=
                    std::string::npos);
                break;
              }
              CHECK(r2->has_value());
              CHECK_EQ(r2->value(), 42);
            }
            CHECK_EQ(runningSourceGenerators.load(), 0);
            CHECK_EQ(runningListGenerators.load(), 0);
          }))
          .start();
  std::move(future).get();
}

TEST_F(MergeTest, DontLeakRequestContext) {
  class TestData : public folly::RequestData {
   public:
    explicit TestData() noexcept {}
    bool hasCallback() override { return false; }

    static void set() {
      folly::RequestContext::get()->setContextData(
          "test", std::make_unique<TestData>());
    }
    static auto get() {
      return folly::RequestContext::get()->getContextData("test");
    }
  };
  blockingWait([]() -> Task<void> {
    folly::RequestContextScopeGuard requestScope;

    TestData::set();
    auto initialContextData = TestData::get();
    CHECK(initialContextData != nullptr);

    auto generator = merge(
        co_await co_current_executor,
        co_invoke([&]() -> AsyncGenerator<AsyncGenerator<int>> {
          auto makeGenerator = [&]() -> AsyncGenerator<int> {
            for (int i = 0; i < 10; ++i) {
              CHECK(TestData::get() == initialContextData);
              folly::RequestContextScopeGuard childScope;
              CHECK(TestData::get() == nullptr);
              co_await co_reschedule_on_current_executor;
              CHECK(TestData::get() == nullptr);
              TestData::set();
              auto newContextData = TestData::get();
              CHECK(newContextData != nullptr);
              CHECK(newContextData != initialContextData);
              co_await co_reschedule_on_current_executor;
              CHECK(TestData::get() == newContextData);
            }
          };
          for (int i = 0; i < 5; ++i) {
            co_yield makeGenerator();
          }
        }));

    while (auto val = co_await generator.next()) {
    }

    CHECK(TestData::get() == initialContextData);
  }());
}

TEST_F(MergeTest, SimpleMergeWithRValueGeneratorType) {
  blockingWait([]() -> Task<void> {
    auto generator = merge(
        co_await co_current_executor,
        []() -> AsyncGenerator<AsyncGenerator<int>&&> {
          auto makeGenerator = [](int start, int count) -> AsyncGenerator<int> {
            for (int i = start; i < start + count; ++i) {
              co_yield i;
              co_await co_reschedule_on_current_executor;
            }
          };

          co_yield makeGenerator(0, 3);
          co_yield makeGenerator(3, 2);
        }());

    const std::array<int, 5> expectedValues = {{0, 3, 1, 4, 2}};

    auto item = co_await generator.next();
    for (int expectedValue : expectedValues) {
      CHECK(!!item);
      CHECK_EQ(expectedValue, *item);
      item = co_await generator.next();
    }
    CHECK(!item);
  }());
}

#endif
