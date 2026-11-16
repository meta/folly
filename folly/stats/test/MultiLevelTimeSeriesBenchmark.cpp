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
#include <folly/init/Init.h>
#include <folly/stats/MultiLevelTimeSeries.h>

namespace {
template <typename CT>
void addValueEveryMillisecond(
    int iters,
    size_t numBuckets,
    std::initializer_list<typename CT::duration> durations,
    size_t stepSizeMs = 1) {
  folly::BenchmarkSuspender suspend;
  folly::MultiLevelTimeSeries<int64_t, CT> mlts(numBuckets, durations);
  std::chrono::seconds start(0);
  suspend.dismiss();

  for (int i = 0; i < iters; ++i) {
    folly::doNotOptimizeAway(mlts);
    mlts.addValue(
        std::chrono::duration_cast<typename CT::duration>(
            start + std::chrono::milliseconds(i * stepSizeMs)) /* now */,
        42 /* value */);
  }
}

/*
 * The read always takes place every 60s, while the write takes place every
 * 100ms if `doWrite` is true.
 */
template <typename CT, bool ConstRead, bool DoWrite, bool InitialWrite = false>
void mixWorkload(unsigned int iters) {
  folly::BenchmarkSuspender suspend;
  folly::MultiLevelTimeSeries<int64_t, CT> mlts(
      60,
      {std::chrono::seconds(60),
       std::chrono::seconds(600),
       std::chrono::seconds(3600),
       std::chrono::seconds(0)});
  auto start = std::chrono::milliseconds(0);
  auto now = [&](std::chrono::milliseconds delta = {}) {
    return typename CT::time_point(
        std::chrono::duration_cast<typename CT::duration>(start + delta));
  };
  if constexpr (InitialWrite) {
    mlts.addValue(now(), 42 /* value */);
  }
  suspend.dismiss();

  for (auto delta = std::chrono::milliseconds(0);
       delta < std::chrono::milliseconds(100 * iters);
       delta += std::chrono::milliseconds(100)) {
    folly::doNotOptimizeAway(mlts);
    if constexpr (DoWrite) {
      mlts.addValue(now(delta), 42 /* value */);
    }
    // do read every 60s
    if ((start + delta).count() % 60'000 == 0) {
      for (size_t l = 0; l < mlts.numLevels(); ++l) {
        if constexpr (ConstRead) {
          auto avg = mlts.avgBy(l, now(delta));
          folly::doNotOptimizeAway(avg);
          auto sum = mlts.sumBy(l, now(delta));
          folly::doNotOptimizeAway(sum);
        } else {
          mlts.update(now(delta));
          auto avg = mlts.avg(l);
          folly::doNotOptimizeAway(avg);
          mlts.update(now(delta));
          auto sum = mlts.sum(l);
          folly::doNotOptimizeAway(sum);
        }
      }
    }
  }
}
} // namespace

BENCHMARK(add_value_seconds_all_time_only, iters) {
  addValueEveryMillisecond<folly::LegacyStatsClock<std::chrono::seconds>>(
      iters, 60, {std::chrono::seconds{0}});
}

BENCHMARK(add_value_milliseconds_all_time_only, iters) {
  addValueEveryMillisecond<folly::LegacyStatsClock<std::chrono::milliseconds>>(
      iters, 60, {std::chrono::seconds{0}});
}

BENCHMARK_DRAW_LINE();

BENCHMARK(add_value_seconds, iters) {
  addValueEveryMillisecond<folly::LegacyStatsClock<std::chrono::seconds>>(
      iters,
      60,
      {std::chrono::seconds(60),
       std::chrono::seconds(600),
       std::chrono::seconds(3600)});
}

BENCHMARK(add_value_milliseconds, iters) {
  addValueEveryMillisecond<folly::LegacyStatsClock<std::chrono::milliseconds>>(
      iters,
      60,
      {std::chrono::seconds(60),
       std::chrono::seconds(600),
       std::chrono::seconds(3600)});
}

BENCHMARK_DRAW_LINE();

BENCHMARK(add_value_high_resolution_seconds, iters) {
  addValueEveryMillisecond<folly::LegacyStatsClock<std::chrono::seconds>>(
      iters,
      20,
      {
          std::chrono::seconds(1),
          std::chrono::seconds(5),
          std::chrono::seconds(10),
          std::chrono::seconds(60),
          std::chrono::seconds(3600),
          std::chrono::seconds(0),
      });
}

BENCHMARK(add_value_high_resolution_milliseconds, iters) {
  addValueEveryMillisecond<folly::LegacyStatsClock<std::chrono::milliseconds>>(
      iters,
      20,
      {
          std::chrono::seconds(1),
          std::chrono::seconds(5),
          std::chrono::seconds(10),
          std::chrono::seconds(60),
          std::chrono::seconds(3600),
          std::chrono::seconds(0),
      });
}

BENCHMARK_DRAW_LINE();
BENCHMARK(add_value_seconds_100ms_step_size, iters) {
  addValueEveryMillisecond<folly::LegacyStatsClock<std::chrono::seconds>>(
      iters,
      60,
      {std::chrono::seconds(60),
       std::chrono::seconds(600),
       std::chrono::seconds(3600)},
      100);
}

BENCHMARK(add_value_milliseconds_100ms_step_size, iters) {
  addValueEveryMillisecond<folly::LegacyStatsClock<std::chrono::milliseconds>>(
      iters,
      60,
      {std::chrono::seconds(60),
       std::chrono::seconds(600),
       std::chrono::seconds(3600)},
      100);
}

BENCHMARK_DRAW_LINE();

BENCHMARK(add_value_hires_seconds_100ms_step, iters) {
  addValueEveryMillisecond<folly::LegacyStatsClock<std::chrono::seconds>>(
      iters,
      20,
      {
          std::chrono::seconds(1),
          std::chrono::seconds(5),
          std::chrono::seconds(10),
          std::chrono::seconds(60),
          std::chrono::seconds(3600),
          std::chrono::seconds(0),
      },
      100);
}

BENCHMARK(add_value_hires_milliseconds_100ms_step, iters) {
  addValueEveryMillisecond<folly::LegacyStatsClock<std::chrono::milliseconds>>(
      iters,
      20,
      {
          std::chrono::seconds(1),
          std::chrono::seconds(5),
          std::chrono::seconds(10),
          std::chrono::seconds(60),
          std::chrono::seconds(3600),
          std::chrono::seconds(0),
      },
      100);
}

BENCHMARK_DRAW_LINE();

BENCHMARK(mix_const_read_write_millisecond_clock, iters) {
  mixWorkload<
      folly::LegacyStatsClock<std::chrono::milliseconds>,
      true /* const read */,
      true /* do write */>(iters);
}

BENCHMARK(mix_non_const_read_write_millisecond_clock, iters) {
  mixWorkload<
      folly::LegacyStatsClock<std::chrono::milliseconds>,
      false /* const read */,
      true /* do write */>(iters);
}

BENCHMARK_DRAW_LINE();

BENCHMARK(mix_const_read_write_second_clock, iters) {
  mixWorkload<
      folly::LegacyStatsClock<std::chrono::seconds>,
      true /* const read */,
      true /* do write */>(iters);
}

BENCHMARK(mix_non_const_read_write_second_clock, iters) {
  mixWorkload<
      folly::LegacyStatsClock<std::chrono::seconds>,
      false /* const read */,
      true /* do write */>(iters);
}

BENCHMARK_DRAW_LINE();

BENCHMARK(const_read_only_millisecond_clock, iters) {
  mixWorkload<
      folly::LegacyStatsClock<std::chrono::milliseconds>,
      true /* const read */,
      false /* do write */>(iters);
}

BENCHMARK(non_const_read_only_millisecond_clock, iters) {
  mixWorkload<
      folly::LegacyStatsClock<std::chrono::milliseconds>,
      false /* const read */,
      false /* do write */>(iters);
}

BENCHMARK_DRAW_LINE();

BENCHMARK(const_read_only_second_clock, iters) {
  mixWorkload<
      folly::LegacyStatsClock<std::chrono::seconds>,
      true /* const read */,
      false /* do write */>(iters);
}

BENCHMARK(non_const_read_only_second_clock, iters) {
  mixWorkload<
      folly::LegacyStatsClock<std::chrono::seconds>,
      false /* const read */,
      false /* do write */>(iters);
}

BENCHMARK_DRAW_LINE();

BENCHMARK(const_read_only_orphaned_millisecond_clock, iters) {
  mixWorkload<
      folly::LegacyStatsClock<std::chrono::milliseconds>,
      true /* const read */,
      false /* do write */,
      true /* initial write */>(iters);
}

BENCHMARK(non_const_read_only_orphaned_millisecond_clock, iters) {
  mixWorkload<
      folly::LegacyStatsClock<std::chrono::milliseconds>,
      false /* const read */,
      false /* do write */,
      true /* initial write */>(iters);
}

BENCHMARK_DRAW_LINE();

BENCHMARK(const_read_only_orphaned_second_clock, iters) {
  mixWorkload<
      folly::LegacyStatsClock<std::chrono::seconds>,
      true /* const read */,
      false /* do write */,
      true /* initial write */>(iters);
}

BENCHMARK(non_const_read_only_orphaned_second_clock, iters) {
  mixWorkload<
      folly::LegacyStatsClock<std::chrono::seconds>,
      false /* const read */,
      false /* do write */,
      true /* initial write */>(iters);
}

int main(int argc, char* argv[]) {
  folly::Init init(&argc, &argv, true);
  folly::runBenchmarks();
  return 0;
}

#if 0
Intel(R) Xeon(R) Gold 6138 CPU @ 2.00GHz
buck run @mode/opt fbsource//xplat/folly/stats/test:multi_level_time_series_benchmark
============================================================================
[...]est/MultiLevelTimeSeriesBenchmark.cpp     relative  time/iter   iters/s
============================================================================
add_value_seconds_all_time_only                             1.29ns   774.55M
add_value_milliseconds_all_time_only                        1.36ns   735.24M
----------------------------------------------------------------------------
add_value_seconds                                           1.96ns   510.57M
add_value_milliseconds                                      1.50ns   667.27M
----------------------------------------------------------------------------
add_value_high_resolution_seconds                           2.04ns   490.48M
add_value_high_resolution_milliseconds                      3.72ns   268.85M
----------------------------------------------------------------------------
add_value_seconds_100ms_step_size                           8.76ns   114.20M
add_value_milliseconds_100ms_step_size                      8.11ns   123.33M
----------------------------------------------------------------------------
add_value_hires_seconds_100ms_step                         15.21ns    65.76M
add_value_hires_milliseconds_100ms_step                   110.22ns     9.07M
----------------------------------------------------------------------------
mix_const_read_write_millisecond_clock                      9.07ns   110.22M
mix_non_const_read_write_millisecond_clock                  9.00ns   111.15M
----------------------------------------------------------------------------
mix_const_read_write_second_clock                           9.90ns   100.99M
mix_non_const_read_write_second_clock                       9.46ns   105.73M
----------------------------------------------------------------------------
const_read_only_millisecond_clock                         623.56ps     1.60G
non_const_read_only_millisecond_clock                     793.39ps     1.26G
----------------------------------------------------------------------------
const_read_only_second_clock                                1.15ns   871.10M
non_const_read_only_second_clock                            1.29ns   774.76M
----------------------------------------------------------------------------
const_read_only_orphaned_millisecond_clock                715.63ps     1.40G
non_const_read_only_orphaned_millisecond_clock            826.26ps     1.21G
----------------------------------------------------------------------------
const_read_only_orphaned_second_clock                       1.27ns   787.10M
non_const_read_only_orphaned_second_clock                   1.35ns   738.05M
#endif
