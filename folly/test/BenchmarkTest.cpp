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
#include <folly/detail/PerfScoped.h>
#include <folly/portability/GFlags.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <folly/test/TestUtils.h>

#include <algorithm>
#include <filesystem>
#include <string>

namespace folly {
namespace detail {
namespace {

struct TestClock {
  static std::chrono::high_resolution_clock::time_point value;
  static std::chrono::high_resolution_clock::time_point now() { return value; }

  template <typename Rep, typename Period>
  static void advance(std::chrono::duration<Rep, Period> diff) {
    value += diff;
  }
};

std::chrono::high_resolution_clock::time_point TestClock::value = {};

void doBaseline() {
  TestClock::advance(std::chrono::nanoseconds(1));
}

void doSuspenderBaseline() {
  TestClock::advance(std::chrono::nanoseconds(10));
}

struct BenchmarkingStateForTests : BenchmarkingState<TestClock> {
#if FOLLY_PERF_IS_SUPPORTED
  std::function<PerfScoped(const std::vector<std::string>&)> perfSetup;
  PerfScoped doSetUpPerfScoped(
      const std::vector<std::string>& args) const override {
    if (!perfSetup) {
      return BenchmarkingState<TestClock>::doSetUpPerfScoped(args);
    }
    return perfSetup(args);
  }
#endif
};

struct BenchmarkingStateTest : ::testing::Test {
  BenchmarkingStateTest() {
    state.addBenchmark(
        __FILE__,
        BenchmarkingState<TestClock>::getGlobalBaselineNameForTests().str(),
        [] {
          doBaseline();
          return 1;
        });
    state.addBenchmark(
        __FILE__,
        BenchmarkingState<TestClock>::getGlobalSuspenderBaselineNameForTests()
            .str(),
        [] {
          doSuspenderBaseline();
          return 1;
        });
  }

  BenchmarkingStateForTests state;
  folly::gflags::FlagSaver flagSaver;
};

TEST(BenchmarkFormattingTest, DefaultTimeUnit) {
  folly::gflags::FlagSaver saver;
  folly::gflags::SetCommandLineOption("bm_time_unit", "");
  const std::vector<BenchmarkResult> results{
      {"benchmark.cpp", "build", 330.6e9, {}},
      {"benchmark.cpp", "long_build", 10 * 365.25 * 24 * 3600 * 1e9, {}}};
  const auto output = benchmarkResultsToString(results);
  EXPECT_THAT(output, ::testing::HasSubstr("5.51min"));
  EXPECT_THAT(output, ::testing::HasSubstr("10.00years"));
}

TEST(BenchmarkFormattingTest, FixedTimeUnits) {
  folly::gflags::FlagSaver saver;
  const std::vector<BenchmarkResult> results{
      {"benchmark.cpp",
       "build",
       126e6,
       {{"elapsed", UserMetric(120, UserMetric::Type::TIME)}}},
      {"benchmark.cpp", "%relative_build", 252e6, {}}};
  struct TestCase {
    const char* unit;
    const char* buildTime;
    const char* relativeTime;
    const char* elapsed;
  };
  const TestCase cases[]{
      {"s", "0.13s", "0.25s", "120.00s"},
      {"ms", "126.00ms", "252.00ms", "120000.00ms"},
      {"us", "126000.00us", "252000.00us", "120000000.00us"},
      {"ns", "126000000.00ns", "252000000.00ns", "120000000000.00ns"},
      {"S", "0.13s", "0.25s", "120.00s"}};
  for (const auto& test : cases) {
    SCOPED_TRACE(test.unit);
    folly::gflags::SetCommandLineOption("bm_time_unit", test.unit);
    const auto output = benchmarkResultsToString(results);
    EXPECT_THAT(output, ::testing::HasSubstr(test.buildTime));
    EXPECT_THAT(output, ::testing::HasSubstr(test.relativeTime));
    EXPECT_THAT(output, ::testing::HasSubstr(test.elapsed));
  }
}

TEST(BenchmarkFormattingTest, SecondsComparison) {
  folly::gflags::FlagSaver saver;
  folly::gflags::SetCommandLineOption("bm_time_unit", "s");
  const std::vector<BenchmarkResult> base{
      {"benchmark.cpp", "build", 661.2e9, {}}};
  const std::vector<BenchmarkResult> results{
      {"benchmark.cpp", "build", 330.6e9, {}},
      {"benchmark.cpp", "new_build", 165.3e9, {}}};

  ::testing::internal::CaptureStdout();
  printResultComparison(base, results);
  const auto output = ::testing::internal::GetCapturedStdout();
  EXPECT_THAT(output, ::testing::HasSubstr("330.60s"));
  EXPECT_THAT(output, ::testing::HasSubstr("165.30s"));
}

TEST(BenchmarkFormattingTest, InvalidTimeUnit) {
  folly::gflags::FlagSaver saver;
  folly::gflags::SetCommandLineOption("bm_time_unit", "seconds");
  const std::vector<BenchmarkResult> results{
      {"benchmark.cpp", "build", 1000, {}}};
  EXPECT_EXIT(
      (void)benchmarkResultsToString(results),
      ::testing::ExitedWithCode(1),
      "Unknown --bm_time_unit='seconds'");
}

TEST_F(BenchmarkingStateTest, Basic) {
  state.addBenchmark(__FILE__, "a1ns", [&] {
    doBaseline();
    TestClock::advance(std::chrono::nanoseconds(1));
    return 1;
  });

  state.addBenchmark(__FILE__, "b2ns", [&] {
    doBaseline();
    TestClock::advance(std::chrono::nanoseconds(2));
    return 1;
  });

  {
    const std::vector<BenchmarkResult> expected{
        {__FILE__, "a1ns", 1, {}},
        {__FILE__, "b2ns", 2, {}},
    };

    EXPECT_EQ(expected, state.runBenchmarksWithResults());
  }

  // --bm_regex full match
  {
    folly::gflags::FlagSaver _;
    folly::gflags::SetCommandLineOption("bm_regex", "a1.*");

    const std::vector<BenchmarkResult> expected{
        {__FILE__, "a1ns", 1, {}},
    };

    EXPECT_EQ(expected, state.runBenchmarksWithResults());
  }

  // --bm_regex part match
  {
    folly::gflags::FlagSaver _;
    folly::gflags::SetCommandLineOption("bm_regex", "1.");

    const std::vector<BenchmarkResult> expected{
        {__FILE__, "a1ns", 1, {}},
    };

    EXPECT_EQ(expected, state.runBenchmarksWithResults());
  }

  // --bm_regex case-insensitive match
  {
    folly::gflags::FlagSaver _;
    folly::gflags::SetCommandLineOption("bm_regex", "(?i)A1");

    const std::vector<BenchmarkResult> expected{
        {__FILE__, "a1ns", 1, {}},
    };

    EXPECT_EQ(expected, state.runBenchmarksWithResults());
  }
}

TEST_F(BenchmarkingStateTest, Suspender) {
  state.addBenchmark(__FILE__, "1ns", [&] {
    doBaseline();
    {
      doSuspenderBaseline();
      BenchmarkSuspender<TestClock> suspender;
      TestClock::advance(std::chrono::microseconds(1));
    }

    TestClock::advance(std::chrono::nanoseconds(1));
    return 1;
  });

  const std::vector<BenchmarkResult> expected{
      {__FILE__, "1ns", 1, {}},
  };

  EXPECT_EQ(expected, state.runBenchmarksWithResults());
}

TEST_F(BenchmarkingStateTest, DiscardOutlier) {
  int iterationToBeExpensive = 0;
  int currentIteration = 0;

  state.addBenchmark(__FILE__, "HasOutlier", [&] {
    doBaseline();
    if (currentIteration++ == iterationToBeExpensive) {
      TestClock::advance(std::chrono::microseconds(1));
    }

    TestClock::advance(std::chrono::nanoseconds(1));
    return 1;
  });

  const std::vector<BenchmarkResult> expected{
      {__FILE__, "HasOutlier", 1, {}},
  };

  for (int i = 0; i < 10; ++i) {
    iterationToBeExpensive = i;
    currentIteration = 0;

    EXPECT_EQ(expected, state.runBenchmarksWithResults());
  }
}

TEST_F(BenchmarkingStateTest, DiscardLines) {
  state.addBenchmark(__FILE__, "1ns", [&] {
    doBaseline();
    TestClock::advance(std::chrono::nanoseconds(1));
    return 1;
  });

  // DRAW_LINE adds a "-" benchmark
  state.addBenchmark(__FILE__, "-", [&] { return 0; });

  const std::vector<BenchmarkResult> expected{
      {__FILE__, "1ns", 1, {}},
  };

  EXPECT_EQ(expected, state.runBenchmarksWithResults());
}

#if FOLLY_PERF_IS_SUPPORTED
TEST_F(BenchmarkingStateTest, PerfBasic) {
  int setUpPerfCalled = 0;
  std::vector<std::string> expectedArgs;

  state.addBenchmark(__FILE__, "a", [&] {
    doBaseline();
    TestClock::advance(std::chrono::nanoseconds(1));
    return 1;
  });

  state.perfSetup = [&](const std::vector<std::string>& args) {
    ++setUpPerfCalled;
    EXPECT_EQ(expectedArgs, args);
    return PerfScoped{};
  };

  {
    (void)state.runBenchmarksWithResults();
    EXPECT_EQ(0, setUpPerfCalled);
  }

  {
    folly::gflags::FlagSaver _;
    folly::gflags::SetCommandLineOption(
        "bm_perf_args", "stat -e cache-misses,cache-references");

    setUpPerfCalled = 0;
    expectedArgs = {"stat", "-e", "cache-misses,cache-references"};
    (void)state.runBenchmarksWithResults();
    EXPECT_EQ(1, setUpPerfCalled);
  }
}

TEST_F(BenchmarkingStateTest, PerfRejectsMultipleBenchmarks) {
  state.addBenchmark(__FILE__, "a", [&] {
    doBaseline();
    return 1;
  });
  state.addBenchmark(__FILE__, "b", [&] {
    doBaseline();
    return 1;
  });

  folly::gflags::FlagSaver _;
  folly::gflags::SetCommandLineOption("bm_perf_args", "stat");

  EXPECT_EXIT(
      (void)state.runBenchmarksWithResults(),
      ::testing::ExitedWithCode(1),
      "profiles one benchmark at a time");
}

TEST_F(BenchmarkingStateTest, PerfRejectsEmptySelection) {
  state.addBenchmark(__FILE__, "a", [&] {
    doBaseline();
    return 1;
  });

  folly::gflags::FlagSaver _;
  folly::gflags::SetCommandLineOption("bm_perf_args", "stat");
  folly::gflags::SetCommandLineOption("bm_regex", "matches-nothing");

  EXPECT_EXIT(
      (void)state.runBenchmarksWithResults(),
      ::testing::ExitedWithCode(1),
      "select no benchmarks");
}

TEST_F(BenchmarkingStateTest, PerfIgnoresTextEntries) {
  int setUpPerfCalled = 0;

  state.addBenchmark(__FILE__, "\"some text\"", [&] { return 0; });
  state.addBenchmark(__FILE__, "a", [&] {
    doBaseline();
    TestClock::advance(std::chrono::nanoseconds(1));
    return 1;
  });

  state.perfSetup = [&](const std::vector<std::string>&) {
    ++setUpPerfCalled;
    return PerfScoped{};
  };

  folly::gflags::FlagSaver _;
  folly::gflags::SetCommandLineOption("bm_perf_args", "stat");
  (void)state.runBenchmarksWithResults();

  EXPECT_EQ(1, setUpPerfCalled);
}

TEST_F(BenchmarkingStateTest, PerfSkipsAnIteration) {
  bool firstTimeSetUpDone = false;
  bool perfIsCalled = false;

  state.addBenchmark(__FILE__, "a", [&] {
    doBaseline();
    firstTimeSetUpDone = true;
    TestClock::advance(std::chrono::nanoseconds(1));
    return 1;
  });

  state.perfSetup = [&](const std::vector<std::string>&) {
    EXPECT_TRUE(firstTimeSetUpDone);
    perfIsCalled = true;
    return PerfScoped{};
  };

  folly::gflags::FlagSaver _;
  folly::gflags::SetCommandLineOption("bm_perf_args", "stat");
  (void)state.runBenchmarksWithResults();

  EXPECT_TRUE(perfIsCalled);
}

TEST_F(BenchmarkingStateTest, PerfIntegration) {
  SKIP_IF(!std::filesystem::exists(kPerfBinaryPath)) << "Missing perf binary";
  std::vector<int> in(1000, 0);

  state.addBenchmark(__FILE__, "a", [&](unsigned n) {
    for (unsigned i = n; i; --i) {
      std::reverse(in.begin(), in.end());
    }
    TestClock::advance(std::chrono::microseconds(1));
    return n;
  });

  std::string perfOuptut;

  state.perfSetup = [&](const auto& args) {
    return PerfScoped(args, &perfOuptut);
  };

  folly::gflags::FlagSaver _;
  folly::gflags::SetCommandLineOption("bm_perf_args", "stat");
  folly::gflags::SetCommandLineOption("bm_profile", "true");
  folly::gflags::SetCommandLineOption("bm_profile_iters", "1000000");
  (void)state.runBenchmarksWithResults();

  ASSERT_THAT(
      perfOuptut,
      ::testing::HasSubstr("Performance counter stats for process id"));
}

#endif // FOLLY_PERF_IS_SUPPORTED

TEST_F(BenchmarkingStateTest, SkipWarmUp) {
  std::vector<unsigned> iterNumPassed;
  state.addBenchmark(__FILE__, "a", [&](unsigned iters) {
    iterNumPassed.push_back(iters);
    TestClock::advance(std::chrono::microseconds(1));
    return iters;
  });

  folly::gflags::FlagSaver _;
  folly::gflags::SetCommandLineOption("bm_profile", "true");
  folly::gflags::SetCommandLineOption("bm_profile_iters", "1000");

  // Testing that warm up iteration is off by default and that
  // we get exactly the number of iterations passed in once.
  // A lot of places rely on this behaviour and changing it
  // will break them.
  {
    (void)state.runBenchmarksWithResults();
    ASSERT_THAT(iterNumPassed, ::testing::ElementsAre(1000));
  }

  iterNumPassed.clear();

  folly::gflags::SetCommandLineOption("bm_warm_up_iteration", "true");

  {
    (void)state.runBenchmarksWithResults();
    ASSERT_THAT(iterNumPassed, ::testing::ElementsAre(1, 1000));
  }
}

TEST_F(BenchmarkingStateTest, ListTests) {
  std::string output;

  // --bm_list enabled with no benchmarks
  {
    folly::gflags::FlagSaver _;
    folly::gflags::SetCommandLineOption("bm_list", "true");
    ::testing::internal::CaptureStdout();
    runBenchmarks();
    output = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "");
  }

  addBenchmark(__FILE__, "a", [&] {
    doBaseline();
    TestClock::advance(std::chrono::nanoseconds(1));
    return 1;
  });
  addBenchmark(__FILE__, "b", [&] {
    doBaseline();
    TestClock::advance(std::chrono::nanoseconds(1));
    return 1;
  });

  // --bm_list enabled with multiple benchmarks
  {
    folly::gflags::FlagSaver _;
    folly::gflags::SetCommandLineOption("bm_list", "true");
    ::testing::internal::CaptureStdout();
    runBenchmarks();
    output = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "a\nb\n");
  }

  // --bm_list disabled, benchmarks run as expected
  {
    ::testing::internal::CaptureStdout();
    runBenchmarks();
    output = ::testing::internal::GetCapturedStdout();
    ASSERT_THAT(output, ::testing::HasSubstr("iters/s"));
  }
}

TEST_F(BenchmarkingStateTest, UserMetricInt) {
  std::string output;

  {
    addBenchmark(__FILE__, "int_custom", [&](UserCounters& counter) {
      counter["int_custom_count"] =
          UserMetric(123456, UserMetric::Type::CUSTOM);
      doBaseline();
      TestClock::advance(std::chrono::nanoseconds(1));
      return 1;
    });

    ::testing::internal::CaptureStdout();
    runBenchmarks();
    output = ::testing::internal::GetCapturedStdout();
    ASSERT_THAT(output, ::testing::HasSubstr("123456"));
  }

  {
    addBenchmark(__FILE__, "int_metric", [&](UserCounters& counter) {
      counter["int_metric_count"] =
          UserMetric(234567, UserMetric::Type::METRIC);
      doBaseline();
      TestClock::advance(std::chrono::nanoseconds(1));
      return 1;
    });

    ::testing::internal::CaptureStdout();
    runBenchmarks();
    output = ::testing::internal::GetCapturedStdout();
    ASSERT_THAT(output, ::testing::HasSubstr("234.57K"));
  }

  {
    addBenchmark(__FILE__, "int_time", [&](UserCounters& counter) {
      counter["int_time_count"] = UserMetric(10, UserMetric::Type::TIME);
      doBaseline();
      TestClock::advance(std::chrono::nanoseconds(1));
      return 1;
    });

    ::testing::internal::CaptureStdout();
    runBenchmarks();
    output = ::testing::internal::GetCapturedStdout();
    std::cout << output << std::endl;
    ASSERT_THAT(output, ::testing::HasSubstr("10.00s"));
  }
}

TEST_F(BenchmarkingStateTest, UserMetricPrecise) {
  std::string output;

  {
    addBenchmark(__FILE__, "double_custom", [&](UserCounters& counter) {
      counter["double_custom_count"] =
          UserMetric(1234.56, UserMetric::Type::CUSTOM);
      doBaseline();
      TestClock::advance(std::chrono::nanoseconds(1));
      return 1;
    });

    ::testing::internal::CaptureStdout();
    runBenchmarks();
    output = ::testing::internal::GetCapturedStdout();
    ASSERT_THAT(output, ::testing::HasSubstr("1234"));
  }

  {
    addBenchmark(__FILE__, "double_metric", [&](UserCounters& counter) {
      counter["double_metric_count"] =
          UserMetric(23.456, UserMetric::Type::METRIC);
      doBaseline();
      TestClock::advance(std::chrono::nanoseconds(1));
      return 1;
    });

    ::testing::internal::CaptureStdout();
    runBenchmarks();
    output = ::testing::internal::GetCapturedStdout();
    ASSERT_THAT(output, ::testing::HasSubstr("23.46"));
  }

  {
    addBenchmark(__FILE__, "double_time", [&](UserCounters& counter) {
      counter["double_time_count"] = UserMetric(34.567, UserMetric::Type::TIME);
      doBaseline();
      TestClock::advance(std::chrono::nanoseconds(1));
      return 1;
    });

    ::testing::internal::CaptureStdout();
    runBenchmarks();
    output = ::testing::internal::GetCapturedStdout();
    std::cout << output << std::endl;
    ASSERT_THAT(output, ::testing::HasSubstr("34.57s"));
  }
}

} // namespace
} // namespace detail
} // namespace folly
