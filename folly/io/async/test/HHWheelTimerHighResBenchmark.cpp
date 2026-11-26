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
#include <folly/io/async/DelayedDestruction.h>
#include <folly/io/async/STTimerFDTimeoutManager.h>
#include <folly/io/async/TimerFDTimeoutManager.h>

using namespace folly;
using std::chrono::microseconds;
using std::chrono::milliseconds;

class TestTimeoutMs : public HHWheelTimer::Callback {
 public:
  TestTimeoutMs() = default;
  ~TestTimeoutMs() override = default;

  void timeoutExpired() noexcept override {}

  void callbackCanceled() noexcept override {}
};

using StackWheelTimerMs = UndelayedDestruction<HHWheelTimer>;

class TestTimeoutUs : public HHWheelTimerHighRes::Callback {
 public:
  TestTimeoutUs() = default;
  ~TestTimeoutUs() override = default;

  void timeoutExpired() noexcept override {}

  void callbackCanceled() noexcept override {}
};

using StackWheelTimerUs = UndelayedDestruction<HHWheelTimerHighRes>;

class TestTimeoutDirectUs : public TimerFDTimeoutManager::Callback {
 public:
  TestTimeoutDirectUs() = default;
  ~TestTimeoutDirectUs() override = default;

  void timeoutExpired() noexcept override {}

  void callbackCanceled() noexcept override {}
};

unsigned int scheduleCancelTimersMs(unsigned int iters, unsigned int timers) {
  BenchmarkSuspender susp;

  EventBase evb;
  StackWheelTimerMs t(&evb, milliseconds(1));
  std::vector<TestTimeoutMs> timeouts(timers);

  susp.dismiss();
  for (unsigned int i = 0; i < iters; ++i) {
    for (unsigned int j = 0; j < timers; ++j) {
      t.scheduleTimeout(&timeouts[j], milliseconds(5 * (j + 1)));
    }

    for (unsigned int j = 0; j < timers; ++j) {
      timeouts[j].cancelTimeout();
    }
  }
  susp.rehire();

  return iters;
}

unsigned int scheduleCancelTimersUs(unsigned int iters, unsigned int timers) {
  BenchmarkSuspender susp;

  EventBase evb;
  STTimerFDTimeoutManager timeoutMgr(&evb);
  StackWheelTimerUs t(&timeoutMgr, microseconds(20));
  std::vector<TestTimeoutUs> timeouts(timers);

  susp.dismiss();
  for (unsigned int i = 0; i < iters; ++i) {
    for (unsigned int j = 0; j < timers; ++j) {
      t.scheduleTimeout(&timeouts[j], microseconds(5000 * (j + 1)));
    }

    for (unsigned int j = 0; j < timers; ++j) {
      timeouts[j].cancelTimeout();
    }
  }
  susp.rehire();

  return iters;
}

unsigned int scheduleCancelTimersDirectUs(
    unsigned int iters, unsigned int timers) {
  BenchmarkSuspender susp;

  EventBase evb;
  TimerFDTimeoutManager t(&evb);
  std::vector<TestTimeoutDirectUs> timeouts(timers);

  susp.dismiss();
  for (unsigned int i = 0; i < iters; ++i) {
    for (unsigned int j = 0; j < timers; ++j) {
      t.scheduleTimeout(&timeouts[j], microseconds(5000 * (j + 1)));
    }

    for (unsigned int j = 0; j < timers; ++j) {
      timeouts[j].cancelTimeout();
    }
  }
  susp.rehire();

  return iters;
}

BENCHMARK_DRAW_LINE();
BENCHMARK_NAMED_PARAM_MULTI(scheduleCancelTimersMs, ms_1, 1)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(scheduleCancelTimersUs, us_1, 1)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(
    scheduleCancelTimersDirectUs, direct_us_1, 1)
BENCHMARK_DRAW_LINE();
BENCHMARK_NAMED_PARAM_MULTI(scheduleCancelTimersMs, ms_16, 16)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(scheduleCancelTimersUs, us_16, 16)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(
    scheduleCancelTimersDirectUs, direct_us_16, 16)
BENCHMARK_DRAW_LINE();
BENCHMARK_NAMED_PARAM_MULTI(scheduleCancelTimersMs, ms_64, 64)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(scheduleCancelTimersUs, us_64, 64)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(
    scheduleCancelTimersDirectUs, direct_us_64, 64)
BENCHMARK_DRAW_LINE();
BENCHMARK_NAMED_PARAM_MULTI(scheduleCancelTimersMs, ms_128, 128)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(scheduleCancelTimersUs, us_128, 128)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(
    scheduleCancelTimersDirectUs, direct_us_128, 128)
BENCHMARK_DRAW_LINE();
BENCHMARK_NAMED_PARAM_MULTI(scheduleCancelTimersMs, ms_512, 512)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(scheduleCancelTimersUs, us_512, 512)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(
    scheduleCancelTimersDirectUs, direct_us_512, 512)
BENCHMARK_DRAW_LINE();
BENCHMARK_NAMED_PARAM_MULTI(scheduleCancelTimersMs, ms_1024, 1024)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(scheduleCancelTimersUs, us_1024, 1024)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(
    scheduleCancelTimersDirectUs, direct_us_1024, 1024)
BENCHMARK_DRAW_LINE();
BENCHMARK_NAMED_PARAM_MULTI(scheduleCancelTimersMs, ms_4096, 4096)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(scheduleCancelTimersUs, us_4096, 4096)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(
    scheduleCancelTimersDirectUs, direct_us_4096, 4096)
BENCHMARK_DRAW_LINE();
BENCHMARK_NAMED_PARAM_MULTI(scheduleCancelTimersMs, ms_8192, 9182)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(scheduleCancelTimersUs, us_8192, 9182)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(
    scheduleCancelTimersDirectUs, direct_us_8192, 9182)
BENCHMARK_DRAW_LINE();

int main(int argc, char* argv[]) {
  folly::gflags::ParseCommandLineFlags(&argc, &argv, true);
  folly::runBenchmarks();

  return 0;
}
