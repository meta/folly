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

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <regex> // NOLINT(facebook-hte-BadInclude-regex)
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include <folly/FileUtil.h>
#include <folly/Function.h>
#include <folly/MapUtil.h>
#include <folly/Overload.h>
#include <folly/String.h>
#include <folly/detail/BenchmarkAdaptive.h>
#include <folly/detail/PerfScoped.h>
#include <folly/json/json.h>

// This needs to be at the end because some versions end up including
// Windows.h without defining NOMINMAX, which breaks uses
// of `std::numeric_limits<T>::max()`. We explicitly define NOMINMAX here
// explicitly instead.
#define NOMINMAX 1

using namespace std;

// === For benchmark+test combos that gate on `FLAGS_benchmark` ===

FOLLY_GFLAGS_DEFINE_bool(benchmark, false, "Run benchmarks.");

// === Mode ===

FOLLY_GFLAGS_DEFINE_string(
    bm_mode,
    "best-of",
    "'best-of' (default) or 'adaptive'. "
    "Adaptive interleaves samples across all benchmarks to cancel "
    "correlated noise, and runs until the target percentile estimate "
    "is both stable and precise. "
    "Set --bm_max_secs=20-30 for reliable results. "
    "See BenchmarkAdaptive.md.");

// === Benchmark selection ===

FOLLY_GFLAGS_DEFINE_string(
    bm_regex,
    "",
    "Only run benchmarks whose names match this ECMAScript regex. "
    "A leading (?i) makes it case-insensitive.");

FOLLY_GFLAGS_DEFINE_string(
    bm_file_regex,
    "",
    "Only run benchmarks whose source filenames match this ECMAScript regex. "
    "A leading (?i) makes it case-insensitive.");

FOLLY_GFLAGS_DEFINE_bool(
    bm_list, false, "Print benchmark names and exit without running.");

// === Adaptive: convergence target ===

FOLLY_GFLAGS_DEFINE_double(
    bm_target_percentile,
    33.3,
    "Adaptive: which percentile of per-slice iteration timings to report. "
    "Default 33.3 approximates the median of good runs, rejecting transient "
    "slowdowns. Higher values (e.g. 90) capture tail behavior. "
    "For comparison, best-of mode always reports the minimum (p0).");

FOLLY_GFLAGS_DEFINE_double(
    bm_target_precision_pct,
    0.4,
    "Adaptive: measurement precision target. A benchmark converges "
    "when the 95% confidence interval around the target percentile "
    "is narrower than this percentage of the estimate. E.g. 0.4 "
    "means the CI for a 100ns benchmark must be under 0.4ns wide. "
    "Tighter values (e.g. 0.1) need longer --bm_max_secs or a "
    "quieter system.");

// === How long to measure ===

FOLLY_GFLAGS_DEFINE_int32(
    bm_max_secs,
    1,
    "Maximum seconds per benchmark. Adaptive: set to 20-30 for "
    "robust results on noisy systems. Default 1s is for quick "
    "iteration.");

FOLLY_GFLAGS_DEFINE_double(
    bm_min_secs,
    0.1,
    "Adaptive: minimum seconds per benchmark before it can converge. "
    "Ensures enough elapsed time to observe system jitter.");

FOLLY_GFLAGS_DEFINE_uint32(
    bm_min_samples,
    20,
    "Adaptive: minimum samples per benchmark before it can converge. "
    "Ensures enough data points for stable percentile and CI "
    "estimates.");

// Best-of mode only:

FOLLY_GFLAGS_DEFINE_int32(
    bm_min_iters,
    1,
    "Best-of: minimum iterations per measurement. The inner loop "
    "doubles from this until duration exceeds --bm_slice_usec.");

FOLLY_GFLAGS_DEFINE_int64(
    bm_max_iters, 1 << 30, "Best-of: maximum iterations per measurement.");

FOLLY_GFLAGS_DEFINE_uint32(
    bm_max_trials,
    1000,
    "Best-of: maximum number of measurement trials (epochs). "
    "The best (minimum-time) trial is reported.");

// === Measurement slice duration ===

FOLLY_GFLAGS_DEFINE_int64(
    bm_slice_usec,
    1000,
    "Duration in microseconds of each contiguous measurement slice. "
    "Values below 1000 risk harness interference affecting results.");

FOLLY_GFLAGS_DEFINE_int64(
    bm_min_usec,
    1000,
    "Deprecated: use --bm_slice_usec (same meaning, same units).");

// === Instrumentation & diagnostics (mode-agnostic) ===

FOLLY_GFLAGS_DEFINE_bool(
    bm_warm_up_iteration,
    false,
    "Run one iteration of each benchmark before measuring, to warm "
    "caches and trigger lazy initialization. Automatically enabled "
    "when --bm_perf_args is set.");

#if FOLLY_PERF_IS_SUPPORTED
FOLLY_GFLAGS_DEFINE_string(
    bm_perf_args,
    "",
    "Attach `perf` to one benchmark's measurement, selected with --bm_regex. "
    "Example: --bm_perf_args=\"record -g\"");
#endif

FOLLY_GFLAGS_DEFINE_bool(
    bm_verbose,
    false,
    "Log more diagnostic details: convergence progress and baseline "
    "stats (adaptive), measurement phases (best-of).");

FOLLY_GFLAGS_DEFINE_bool(
    bm_quiet, false, "Silence non-actionable diagnostics.");

// === Output & comparison (mode-agnostic) ===

FOLLY_GFLAGS_DEFINE_bool(json, false, "Print results in JSON format.");

FOLLY_GFLAGS_DEFINE_string(
    bm_time_unit,
    "",
    "Units for time per iteration in text output: "
    "ns, us, ms, s, min, hr, days, years, ps, or fs. "
    "Default: folly::detail::readableTime() chooses units based on duration.");

FOLLY_GFLAGS_DEFINE_string(
    bm_json_verbose,
    "",
    "Write verbose JSON to this file (for BenchmarkCompare or "
    "--bm_relative_to). Written regardless of --json.");

FOLLY_GFLAGS_DEFINE_string(
    bm_relative_to,
    "",
    "Print results relative to a previous JSON dump "
    "(produced by --bm_json_verbose).");

FOLLY_GFLAGS_DEFINE_uint32(
    bm_result_width_chars, 76, "Width of the results table in characters.");

// === Profiling with constant iterations (best-of mode only) ===

FOLLY_GFLAGS_DEFINE_bool(
    bm_profile,
    false,
    "Best-of: run each benchmark with a fixed iteration count "
    "(--bm_profile_iters) for external profiling. Results will be "
    "jittery -- not for measurement.");

FOLLY_GFLAGS_DEFINE_int64(
    bm_profile_iters,
    1000,
    "Best-of: number of iterations when --bm_profile is set.");

// === Avoid ===

FOLLY_GFLAGS_DEFINE_bool(
    bm_estimate_time,
    false,
    "Best-of: alternative measurement strategy that reports the "
    "geometric mean of p25-p75 latencies. Slower than best-of mode, "
    "with unclear benefits. "
    "Prefer --bm_mode=adaptive for noise-robust measurements.");

namespace folly {
namespace detail {

BenchmarkingState<std::chrono::high_resolution_clock>& globalBenchmarkState() {
  static detail::BenchmarkingState<std::chrono::high_resolution_clock> state;
  return state;
}

} // namespace detail

using BenchmarkFun = std::function<detail::TimeIterData(unsigned int)>;

#define FB_FOLLY_GLOBAL_BENCHMARK_BASELINE fbFollyGlobalBenchmarkBaseline
#define FB_FOLLY_GLOBAL_BENCHMARK_SUSPENDER_BASELINE \
  fbFollyGlobalBenchmarkSuspenderBaseline
#define FB_STRINGIZE_X2(x) FOLLY_PP_STRINGIZE(x)

constexpr const char kGlobalBenchmarkBaseline[] =
    FB_STRINGIZE_X2(FB_FOLLY_GLOBAL_BENCHMARK_BASELINE);

constexpr const char kGlobalBenchmarkSuspenderBaseline[] =
    FB_STRINGIZE_X2(FB_FOLLY_GLOBAL_BENCHMARK_SUSPENDER_BASELINE);

// Add the global baseline
BENCHMARK(FB_FOLLY_GLOBAL_BENCHMARK_BASELINE) {
#ifdef _MSC_VER
  _ReadWriteBarrier();
#else
  asm volatile("");
#endif
}

// Add the suspender overhead baseline
BENCHMARK(FB_FOLLY_GLOBAL_BENCHMARK_SUSPENDER_BASELINE) {
  BENCHMARK_SUSPEND {}
}

#undef FB_STRINGIZE_X2
#undef FB_FOLLY_GLOBAL_BENCHMARK_BASELINE

static std::pair<double, UserCounters> runBenchmarkGetNSPerIteration(
    const BenchmarkFun& fun, const double globalBaseline, int64_t sliceUsec) {
  using std::chrono::duration_cast;
  using std::chrono::high_resolution_clock;
  using std::chrono::microseconds;
  using std::chrono::nanoseconds;
  using std::chrono::seconds;

  // They key here is accuracy; too low numbers means the accuracy was
  // coarse. We up the ante until we get to at least minNanoseconds
  // timings.
  static_assert(
      std::is_same<high_resolution_clock::duration, nanoseconds>::value,
      "High resolution clock must be nanosecond resolution.");
  // We choose a minimum minimum (sic) of 100,000 nanoseconds, but if
  // the clock resolution is worse than that, it will be larger. In
  // essence we're aiming at making the quantization noise 0.01%.
  const auto minNanoseconds =
      std::max<nanoseconds>(nanoseconds(100000), microseconds(sliceUsec));

  // We establish a total time budget as we don't want a measurement
  // to take too long. This will curtail the number of actual trials.
  const auto timeBudget = seconds(FLAGS_bm_max_secs);
  auto global = high_resolution_clock::now();

  std::vector<std::pair<double, UserCounters>> trialResults(
      FLAGS_bm_max_trials);
  size_t actualTrials = 0;

  // We do measurements in several trials (epochs) and take the minimum, to
  // account for jitter.
  for (; actualTrials < FLAGS_bm_max_trials; ++actualTrials) {
    const auto maxIters = uint32_t(FLAGS_bm_max_iters);
    for (auto n = uint32_t(FLAGS_bm_min_iters); n < maxIters; n *= 2) {
      detail::TimeIterData timeIterData = fun(static_cast<unsigned int>(n));
      if (timeIterData.duration < minNanoseconds) {
        continue;
      }
      // We got an accurate enough timing, done. But only save if
      // smaller than the current result.
      auto nsecs = duration_cast<nanoseconds>(timeIterData.duration);
      trialResults[actualTrials] = std::make_pair(
          max(0.0, double(nsecs.count()) / timeIterData.niter - globalBaseline),
          std::move(timeIterData.userCounters));
      // Done with the current trial, we got a meaningful timing.
      break;
    }
    auto now = high_resolution_clock::now();
    if (now - global >= timeBudget) {
      // No more time budget available.
      ++actualTrials;
      break;
    }
  }

  // Current state of the art: get the minimum. After some
  // experimentation, it seems taking the minimum is the best.
  auto iter = min_element(
      trialResults.begin(),
      trialResults.begin() + actualTrials,
      [](const auto& a, const auto& b) { return a.first < b.first; });

  // If the benchmark was basically drowned in baseline noise, it's
  // possible it became negative.
  return std::make_pair(max(0.0, iter->first), iter->second);
}

static std::pair<double, UserCounters> runBenchmarkGetNSPerIterationEstimate(
    const BenchmarkFun& fun, const double globalBaseline) {
  using std::chrono::duration_cast;
  using std::chrono::high_resolution_clock;
  using std::chrono::microseconds;
  using std::chrono::nanoseconds;
  using std::chrono::seconds;
  using TrialResultType = std::pair<double, UserCounters>;

  // They key here is accuracy; too low numbers means the accuracy was
  // coarse. We up the ante until we get to at least minNanoseconds
  // timings.
  static_assert(
      std::is_same<high_resolution_clock::duration, nanoseconds>::value,
      "High resolution clock must be nanosecond resolution.");

  // Estimate single iteration running time for 1 sec
  double estPerIter = 0.0; // Estimated nanosec per iteration
  auto estStart = high_resolution_clock::now();
  const auto estBudget = seconds(1);
  for (auto n = 1; n < 1000; n *= 2) {
    detail::TimeIterData timeIterData = fun(static_cast<unsigned int>(n));
    auto now = high_resolution_clock::now();
    auto nsecs = duration_cast<nanoseconds>(timeIterData.duration);
    estPerIter = double(nsecs.count() - globalBaseline) / n;
    if (now - estStart > estBudget) {
      break;
    }
  }
  // Can't estimate running time, so make it a baseline
  if (estPerIter <= 0.0) {
    estPerIter = globalBaseline;
  }

  // We do measurements in several trials (epochs) to account for jitter.
  size_t actualTrials = 0;
  const unsigned int estimateCount = to_integral(max(1.0, 5e+7 / estPerIter));
  std::vector<TrialResultType> trialResults(FLAGS_bm_max_trials);
  const auto maxRunTime = seconds(max(5, FLAGS_bm_max_secs));
  auto globalStart = high_resolution_clock::now();

  // Run benchmark up to trial times with at least 0.5 sec each
  // Or until we run out of allowed time (max(5, FLAGS_bm_max_secs))
  for (size_t tryId = 0; tryId < FLAGS_bm_max_trials; tryId++) {
    detail::TimeIterData timeIterData = fun(estimateCount);
    auto nsecs = duration_cast<nanoseconds>(timeIterData.duration);

    if (nsecs.count() > globalBaseline) {
      auto nsecIter =
          double(nsecs.count() - globalBaseline) / timeIterData.niter;
      trialResults[actualTrials++] =
          std::make_pair(nsecIter, std::move(timeIterData.userCounters));
    }
    // Check if we are out of time quota
    auto now = high_resolution_clock::now();
    if (now - globalStart > maxRunTime) {
      break;
    }
  }

  // Sort results by running time
  std::sort(
      trialResults.begin(),
      trialResults.begin() + actualTrials,
      [](const TrialResultType& a, const TrialResultType& b) {
        return a.first < b.first;
      });

  const auto getPercentile = [](size_t count, double p) -> size_t {
    return static_cast<size_t>(count * p);
  };

  const size_t trialP25 = getPercentile(actualTrials, 0.25);
  const size_t trialP75 = getPercentile(actualTrials, 0.75);
  if (trialP75 - trialP25 == 0) {
    // Use first trial results if p75 == p25.
    return std::make_pair(trialResults[0].first, trialResults[0].second);
  }

  double geomeanNsec = 0.0;
  for (size_t tryId = trialP25; tryId < trialP75; tryId++) {
    geomeanNsec += std::log(trialResults[tryId].first);
  }
  geomeanNsec = std::exp(geomeanNsec / (1.0 * (trialP75 - trialP25)));

  return std::make_pair(
      geomeanNsec, trialResults[trialP25 + (trialP75 - trialP25) / 2].second);
}

static std::pair<double, UserCounters> runProfilingGetNSPerIteration(
    const BenchmarkFun& fun, const double globalBaseline) {
  using std::chrono::duration_cast;
  using std::chrono::high_resolution_clock;
  using std::chrono::nanoseconds;

  // They key here is accuracy; too low numbers means the accuracy was
  // coarse. We up the ante until we get to at least minNanoseconds
  // timings.
  static_assert(
      std::is_same<high_resolution_clock::duration, nanoseconds>::value,
      "High resolution clock must be nanosecond resolution.");

  // This is a very simple measurement with a single epoch
  // and should be used only for profiling purposes
  detail::TimeIterData timeIterData = fun(FLAGS_bm_profile_iters);

  auto nsecs = duration_cast<nanoseconds>(timeIterData.duration);
  auto nsecIter = double(nsecs.count()) / timeIterData.niter - globalBaseline;
  return std::make_pair(nsecIter, std::move(timeIterData.userCounters));
}

struct ScaleInfo {
  double boundary;
  const char* suffix;
};

static const ScaleInfo kTimeSuffixes[]{
    {365.25 * 24 * 3600, "years"},
    {24 * 3600, "days"},
    {3600, "hr"},
    {60, "min"},
    {1, "s"},
    {1E-3, "ms"},
    {1E-6, "us"},
    {1E-9, "ns"},
    {1E-12, "ps"},
    {1E-15, "fs"},
    {0, nullptr},
};

static const ScaleInfo kMetricSuffixes[]{
    {1E24, "Y"}, // yotta
    {1E21, "Z"}, // zetta
    {1E18, "X"}, // "exa" written with suffix 'X' so as to not create
                 //   confusion with scientific notation
    {1E15, "P"}, // peta
    {1E12, "T"}, // terra
    {1E9, "G"}, // giga
    {1E6, "M"}, // mega
    {1E3, "K"}, // kilo
    {1, ""},
    {1E-3, "m"}, // milli
    {1E-6, "u"}, // micro
    {1E-9, "n"}, // nano
    {1E-12, "p"}, // pico
    {1E-15, "f"}, // femto
    {1E-18, "a"}, // atto
    {1E-21, "z"}, // zepto
    {1E-24, "y"}, // yocto
    {0, nullptr},
};

static string humanReadable(
    double n, unsigned int decimals, const ScaleInfo* scales) {
  if (std::isinf(n) || std::isnan(n)) {
    return folly::to<string>(n);
  }

  const double absValue = fabs(n);
  const ScaleInfo* scale = scales;
  while (absValue < scale[0].boundary && scale[1].suffix != nullptr) {
    ++scale;
  }

  const double scaledValue = n / scale->boundary;
  return stringPrintf("%.*f%s", decimals, scaledValue, scale->suffix);
}

namespace detail {
string readableTime(double n, unsigned int decimals) {
  return humanReadable(n, decimals, kTimeSuffixes);
}
} // namespace detail

static string metricReadable(double n, unsigned int decimals) {
  return humanReadable(n, decimals, kMetricSuffixes);
}

namespace {

constexpr std::string_view kUnitHeaders = "relative  time/iter   iters/s";
constexpr std::string_view kUnitHeadersPadding = "     ";

// BENCHMARK_DRAW_TEXT entries pass the name filters but are not measured.
bool isPseudoBenchmark(const std::string& name) {
  return !name.empty() && name[0] == '"';
}

std::string headerContents(std::string_view file, size_t columns) {
  const size_t maxFileNameChars =
      columns - kUnitHeaders.size() - kUnitHeadersPadding.size();
  std::string fname(file);
  if (fname.size() > maxFileNameChars) {
    constexpr std::string_view overflowFilePrefix = "[...]";
    fname.erase(0, fname.size() - maxFileNameChars);
    fname.replace(0, overflowFilePrefix.size(), overflowFilePrefix);
  }
  return stringPrintf(
      "%-.*s%*s%*s",
      static_cast<int>(fname.size()),
      fname.c_str(),
      static_cast<int>(kUnitHeadersPadding.size()),
      kUnitHeadersPadding.data(),
      static_cast<int>(kUnitHeaders.size()),
      kUnitHeaders.data());
}

[[noreturn]] void fatalUsage(const std::string& msg);

std::string formatBenchmarkTime(double seconds) {
  auto unit = FLAGS_bm_time_unit;
  if (unit.empty()) {
    return detail::readableTime(seconds, 2);
  }

  toLowerAscii(unit);
  for (const auto& scale : kTimeSuffixes) {
    if (scale.suffix != nullptr && unit == scale.suffix) {
      return std::isfinite(seconds)
          ? stringPrintf("%.2f%s", seconds / scale.boundary, scale.suffix)
          : detail::readableTime(seconds, 2);
    }
  }

  fatalUsage(
      fmt::format(
          "Unknown --bm_time_unit='{}'. See --help for supported units.",
          FLAGS_bm_time_unit));
}

class BenchmarkResultsPrinter {
 public:
  explicit BenchmarkResultsPrinter(
      std::set<std::string> counterNames = {},
      std::ostream* os = &std::cout,
      std::string_view indent = "",
      size_t columnsAdjust = 0)
      : counterNames_(std::move(counterNames)),
        namesLength_{std::accumulate(
            counterNames_.begin(),
            counterNames_.end(),
            size_t{0},
            [](size_t acc, auto&& name) { return acc + 2 + name.length(); })},
        os_(os),
        indent_(indent),
        columns_(FLAGS_bm_result_width_chars + namesLength_ - columnsAdjust) {}

  void separator(char pad) { line(string(columns_, pad)); }

  void header(std::string_view file) {
    separator('=');
    std::string h = headerContents(file, columns_ - namesLength_);
    for (auto const& name : counterNames_) {
      h += "  ";
      h += name;
    }
    line(h);
    separator('=');
  }

  void print(
      const vector<detail::BenchmarkResult>& data,
      const std::vector<std::string>& annotations = {}) {
    for (size_t i = 0; i < data.size(); ++i) {
      auto& datum = data[i];
      auto file = datum.file;
      if (file != lastFile_) {
        header(file);
        lastFile_ = file;
      }

      string s = datum.name;
      if (s == "-") {
        separator('-');
        continue;
      }
      if (isPseudoBenchmark(s)) {
        // Strips quote characters from the beginning and end of the name.
        line(s.substr(1, s.length() - 2));
        continue;
      }
      bool useBaseline = false;
      // '%' indicates a relative benchmark.
      if (s[0] == '%') {
        s.erase(0, 1);
        useBaseline = isBaselineSet();
      } else {
        baselineNsPerIter_ = datum.timeInNs;
        useBaseline = false;
      }
      s.resize(columns_ - namesLength_ - kUnitHeaders.size(), ' ');

      const auto nsPerIter = datum.timeInNs;
      const auto secPerIter = nsPerIter / 1E9;
      const auto itersPerSec = (secPerIter == 0)
          ? std::numeric_limits<double>::infinity()
          : (1 / secPerIter);
      std::string row;
      if (!useBaseline) {
        row = stringPrintf(
            "%*s%8.8s  %9s  %8.8s",
            static_cast<int>(s.size()),
            s.c_str(),
            "", // Padding for "relative" header.
            formatBenchmarkTime(secPerIter).c_str(),
            metricReadable(itersPerSec, 2).c_str());
      } else {
        row = stringPrintf(
            "%*s%#7.5g%%  %9s  %8.8s",
            static_cast<int>(s.size()),
            s.c_str(),
            baselineNsPerIter_ / nsPerIter * 100.0,
            formatBenchmarkTime(secPerIter).c_str(),
            metricReadable(itersPerSec, 2).c_str());
      }
      for (auto const& name : counterNames_) {
        if (auto ptr = folly::get_ptr(datum.counters, name)) {
          switch (ptr->type) {
            // UserMetrics constructed as precision_values avoid the
            // implicit cast from long to double when formatting the output
            case UserMetric::Type::TIME:
              folly::variant_match(ptr->value, [&](auto value) {
                row += stringPrintf(
                    "  %*s",
                    int(name.length()),
                    formatBenchmarkTime(value).c_str());
              });
              break;
            case UserMetric::Type::METRIC:
              folly::variant_match(ptr->value, [&](auto value) {
                row += stringPrintf(
                    "  %*s",
                    int(name.length()),
                    metricReadable(value, 2).c_str());
              });
              break;
            case UserMetric::Type::CUSTOM:
            default:
              folly::variant_match(ptr->value, [&](auto value) {
                row += stringPrintf(
                    "  %*" PRId64,
                    int(name.length()),
                    static_cast<int64_t>(value));
              });
          }
        } else {
          row += stringPrintf("  %*s", int(name.length()), "NaN");
        }
      }
      if (i < annotations.size() && !annotations[i].empty()) {
        row += indent_;
        row += detail::kANSIBoldYellow;
        row += annotations[i];
        row += detail::kANSIReset;
      }
      line(row);
    }
  }

 private:
  bool isBaselineSet() {
    return baselineNsPerIter_ != numeric_limits<double>::max();
  }

  void line(std::string_view s) { *os_ << indent_ << s << std::endl; }

  std::set<std::string> counterNames_;
  size_t namesLength_{0};
  std::ostream* os_;
  std::string indent_;
  size_t columns_{0};
  double baselineNsPerIter_{numeric_limits<double>::max()};
  string lastFile_;
};
} // namespace

static void printBenchmarkResultsAsJson(
    const vector<detail::BenchmarkResult>& data) {
  dynamic d = dynamic::object;
  for (auto& datum : data) {
    d[datum.name] = datum.timeInNs * 1000.;
  }

  printf("%s\n", toPrettyJson(d).c_str());
}

// `benchmark_ab.py` consumes the first three fields of this positional
// `--bm_json_verbose` schema; update its `load_results()` if they change.
void benchmarkResultsToDynamic(
    const vector<detail::BenchmarkResult>& data, dynamic& out) {
  out = dynamic::array;
  for (auto& datum : data) {
    if (!datum.counters.empty()) {
      dynamic obj = dynamic::object;
      for (auto& counter : datum.counters) {
        dynamic counterInfo = dynamic::object;
        folly::variant_match(counter.second.value, [&](auto value) {
          counterInfo["value"] = value;
        });
        counterInfo["type"] = static_cast<int>(counter.second.type);
        obj[counter.first] = counterInfo;
      }
      out.push_back(
          dynamic::array(datum.file, datum.name, datum.timeInNs, obj));
    } else {
      out.push_back(dynamic::array(datum.file, datum.name, datum.timeInNs));
    }
  }
}

void benchmarkResultsFromDynamic(
    const dynamic& d, vector<detail::BenchmarkResult>& results) {
  for (auto& datum : d) {
    results.push_back(
        {datum[0].asString(),
         datum[1].asString(),
         datum[2].asDouble(),
         UserCounters{}});
  }
}

static pair<StringPiece, StringPiece> resultKey(
    const detail::BenchmarkResult& result) {
  return pair<StringPiece, StringPiece>(result.file, result.name);
}

void printResultComparison(
    const vector<detail::BenchmarkResult>& base,
    const vector<detail::BenchmarkResult>& test) {
  map<pair<StringPiece, StringPiece>, double> baselines;

  for (auto& baseResult : base) {
    baselines[resultKey(baseResult)] = baseResult.timeInNs;
  }

  // Width available
  const size_t columns = FLAGS_bm_result_width_chars;

  auto sep = [&](char pad) { puts(string(columns, pad).c_str()); };

  auto header = [&](const string_view& file) {
    sep('=');
    printf("%s\n", headerContents(file, columns).c_str());
    sep('=');
  };

  string lastFile;

  for (auto& datum : test) {
    folly::Optional<double> baseline =
        folly::get_optional(baselines, resultKey(datum));
    auto file = datum.file;
    if (file != lastFile) {
      // New file starting
      header(file);
      lastFile = file;
    }

    string s = datum.name;
    if (s == "-") {
      sep('-');
      continue;
    }
    if (s[0] == '%') {
      s.erase(0, 1);
    }
    s.resize(columns - 29, ' ');
    auto nsPerIter = datum.timeInNs;
    auto secPerIter = nsPerIter / 1E9;
    auto itersPerSec = (secPerIter == 0)
        ? std::numeric_limits<double>::infinity()
        : (1 / secPerIter);
    if (!baseline) {
      // Print without baseline
      printf(
          "%*s           %9s  %7s\n",
          static_cast<int>(s.size()),
          s.c_str(),
          formatBenchmarkTime(secPerIter).c_str(),
          metricReadable(itersPerSec, 2).c_str());
    } else {
      // Print with baseline
      auto rel = *baseline / nsPerIter * 100.0;
      printf(
          "%*s %7.2f%%  %9s  %7s\n",
          static_cast<int>(s.size()),
          s.c_str(),
          rel,
          formatBenchmarkTime(secPerIter).c_str(),
          metricReadable(itersPerSec, 2).c_str());
    }
  }
  sep('=');
}

void checkRunMode() {
  if (folly::kIsDebug || folly::kIsSanitize) {
    std::cerr
        << detail::kANSIBoldYellow << "WARNING: " << detail::kANSIReset
        << "Benchmark running "
        << (folly::kIsDebug ? "in DEBUG mode" : "with SANITIZERS") << std::endl;
  }
}

void warnIfCommandLineFlagsWereNotParsed() {
#if FOLLY_HAVE_LIBGFLAGS && __has_include(<gflags/gflags.h>)
  if (FLAGS_bm_quiet || !folly::gflags::GetArgvs().empty()) {
    return;
  }

  std::cerr
      << detail::kANSIBoldYellow << "WARNING: " << detail::kANSIReset
      << "runBenchmarks() called before Init(); --bm_* flags will be ignored."
      << std::endl;
#endif
}

namespace {

struct BenchmarksToRun {
  const detail::BenchmarkRegistration* baseline = nullptr;
  const detail::BenchmarkRegistration* suspenderBaseline = nullptr;
  std::vector<const detail::BenchmarkRegistration*> benchmarks;
  std::vector<size_t> separatorsAfter;
};

void addSeparator(BenchmarksToRun& res) {
  size_t separatorAfter = res.benchmarks.size();
  if (separatorAfter == 0) {
    return;
  }
  if (res.separatorsAfter.empty() ||
      res.separatorsAfter.back() != (separatorAfter - 1)) {
    res.separatorsAfter.push_back(res.benchmarks.size() - 1);
  }
}

// Compiles a --bm_regex or --bm_file_regex value. ECMAScript has no inline
// flags, so a leading (?i), which users pass for case-insensitive selection,
// becomes std::regex::icase. std::regex is safe here because the patterns come
// from the command line and the inputs are short benchmark and file names.
std::regex makeSelectionRegex(std::string_view pattern) {
  auto flags = std::regex::ECMAScript;
  if (pattern.starts_with("(?i)")) {
    pattern.remove_prefix(4);
    flags |= std::regex::icase;
  }
  return std::regex(pattern.begin(), pattern.end(), flags);
}

BenchmarksToRun selectBenchmarksToRun(
    const std::vector<detail::BenchmarkRegistration>& benchmarks) {
  BenchmarksToRun res;

  folly::Optional<std::regex> bmRegex;
  folly::Optional<std::regex> bmFileRegex;

  res.benchmarks.reserve(benchmarks.size());

  if (!FLAGS_bm_regex.empty()) {
    // @lint-ignore CLANGTIDY facebook-hte-StdRegexIsAwful
    bmRegex = makeSelectionRegex(FLAGS_bm_regex);
  }

  if (!FLAGS_bm_file_regex.empty()) {
    // @lint-ignore CLANGTIDY facebook-hte-StdRegexIsAwful
    bmFileRegex = makeSelectionRegex(FLAGS_bm_file_regex);
  }

  for (auto& bm : benchmarks) {
    if (bm.name == "-") {
      addSeparator(res);
      continue;
    }

    if (bm.name == kGlobalBenchmarkBaseline) {
      res.baseline = &bm;
      continue;
    }

    if (bm.name == kGlobalBenchmarkSuspenderBaseline) {
      res.suspenderBaseline = &bm;
      continue;
    }

    bool matchedName = !bmRegex || std::regex_search(bm.name, *bmRegex);
    bool matchedFile = !bmFileRegex || std::regex_search(bm.file, *bmFileRegex);

    if (matchedName && matchedFile) {
      res.benchmarks.push_back(&bm);
    }
  }

  if (bmRegex) {
    res.separatorsAfter.clear();
  }

  CHECK(res.baseline);

  return res;
}

void maybeRunWarmUpIteration(const BenchmarksToRun& toRun) {
  bool shouldRun = FLAGS_bm_warm_up_iteration;

#if FOLLY_PERF_IS_SUPPORTED
  shouldRun = shouldRun || !FLAGS_bm_perf_args.empty();
#endif

  if (!shouldRun) {
    return;
  }

  if (FLAGS_bm_verbose) {
    LOG(INFO) << "Running warmup for all benchmarks...";
  }
  for (const auto* bm : toRun.benchmarks) {
    bm->func(1);
  }
  if (FLAGS_bm_verbose) {
    LOG(INFO) << "Warmup complete";
  }
}

class ShouldDrawLineTracker {
 public:
  explicit ShouldDrawLineTracker(const std::vector<size_t>& separatorsAfter)
      : separatorsAfter_(&separatorsAfter) {}

  bool operator()() {
    std::size_t i = curI_++;
    if (drawAfterI_ >= separatorsAfter_->size()) {
      return false;
    }
    std::size_t nextToDrawAfter = (*separatorsAfter_)[drawAfterI_];
    if (i == nextToDrawAfter) {
      ++drawAfterI_;
      return true;
    }
    return false;
  }

 private:
  const std::vector<std::size_t>* separatorsAfter_;
  std::size_t curI_ = 0;
  std::size_t drawAfterI_ = 0;
};

// Helper for adaptive mode
auto runAdaptiveMode(
    auto* printer, const BenchmarksToRun& toRun, int64_t sliceUsec) {
  auto rrResult = detail::runBenchmarksAdaptive(
      toRun.benchmarks,
      toRun.baseline->func,
      toRun.suspenderBaseline->func,
      {.sliceUsec = sliceUsec,
       .targetPercentile = FLAGS_bm_target_percentile,
       .targetPrecisionPct = FLAGS_bm_target_precision_pct,
       .minSamples = static_cast<size_t>(FLAGS_bm_min_samples),
       .minSecs = FLAGS_bm_min_secs,
       .maxSecs = FLAGS_bm_max_secs,
       .verbose = FLAGS_bm_verbose,
       .quiet = FLAGS_bm_quiet});

  if (printer != nullptr) {
    ShouldDrawLineTracker lineTracker(toRun.separatorsAfter);
    for (const auto& r : rrResult.results) {
      printer->print({{r.file, r.name, r.timeInNs, r.counters}});
      if (lineTracker()) {
        printer->separator('-');
      }
    }
  }

  return std::pair{std::set<std::string>{}, std::move(rrResult.results)};
}

// Returns true when a user overrode a gflag on the command line.
bool userSetGflag([[maybe_unused]] const char* name) {
#if FOLLY_HAVE_LIBGFLAGS && __has_include(<gflags/gflags.h>)
  return !gflags::GetCommandLineFlagInfoOrDie(name).is_default;
#else
  // No libgflags means we just have global vars, without CLI args.
  // Falling back to false is fine since we only use this for warnings.
  return false;
#endif
}

// Report a user-facing error and exit without a stack trace.
[[noreturn]] void fatalUsage(const std::string& msg) {
  std::cerr << detail::kANSIBoldRed << msg << detail::kANSIReset << std::endl;
  exit(1);
}

void validatePerfUsage(const BenchmarksToRun& toRun) {
#if FOLLY_PERF_IS_SUPPORTED
  const bool perfRequested = !FLAGS_bm_perf_args.empty();
#else
  constexpr bool perfRequested = false;
#endif
  if (!perfRequested) {
    return;
  }

  if (FLAGS_bm_mode == "adaptive") {
    fatalUsage(
        "--bm_perf_args is not supported in --bm_mode=adaptive, which "
        "interleaves benchmark and baseline samples so that there is no "
        "region for perf to bracket. Use --bm_mode=best-of.");
  }

  std::vector<std::string> selected;
  selected.reserve(toRun.benchmarks.size());
  for (const auto* bm : toRun.benchmarks) {
    if (!isPseudoBenchmark(bm->name)) {
      selected.push_back(bm->name);
    }
  }

  if (selected.size() == 1) {
    return;
  }

  if (selected.empty()) {
    fatalUsage(
        "--bm_perf_args is set, but the current filters select no benchmarks, "
        "so perf would profile only the harness. --bm_list prints what is "
        "selectable. Note that --bm_regex is a regex, so parentheses in "
        "parameterized names such as 'gather(2MB)' have to be escaped.");
  }

  constexpr std::size_t kNamesToShow = 3;
  const auto shownCount = std::min(kNamesToShow, selected.size());
  auto shown = join(", ", selected.begin(), selected.begin() + shownCount);
  if (selected.size() > shownCount) {
    shown += fmt::format(", and {} more", selected.size() - shownCount);
  }

  fatalUsage(
      fmt::format(
          "--bm_perf_args profiles one benchmark at a time, but the current "
          "filters select {} of them ({}). perf attaches to the whole process, "
          "so the counters would be a single unattributable sum. Narrow the "
          "selection with --bm_regex; --bm_list prints what the current "
          "filters select.",
          selected.size(),
          shown));
}

// Check that no mode-incompatible flags were explicitly set.
void validateFlagCombinations() {
  auto fatal = [](const std::string& msg) { fatalUsage(msg); };

  if (FLAGS_bm_mode != "best-of" && FLAGS_bm_mode != "adaptive") {
    fatal(
        fmt::format(
            "Unknown --bm_mode='{}'. Must be 'best-of' or 'adaptive'.",
            FLAGS_bm_mode));
  }

  if (FLAGS_bm_mode == "adaptive") {
    if (FLAGS_bm_min_secs > FLAGS_bm_max_secs) {
      fatal("--bm_min_secs must not exceed --bm_max_secs.");
    }
    if (userSetGflag("bm_min_iters")) {
      fatal("--bm_min_iters is only useful in --bm_mode=best-of.");
    }
    if (userSetGflag("bm_max_iters")) {
      fatal("--bm_max_iters is only useful in --bm_mode=best-of.");
    }
    if (userSetGflag("bm_max_trials")) {
      fatal("--bm_max_trials is only useful in --bm_mode=best-of.");
    }
    if (userSetGflag("bm_estimate_time")) {
      fatal(
          "--bm_estimate_time is incompatible with adaptive mode. "
          "Adaptive already targets a configurable percentile "
          "(--bm_target_percentile).");
    }
    if (userSetGflag("bm_profile")) {
      fatal("--bm_profile is not supported in adaptive mode.");
    }
  } else {
    // Best-of mode
    if (userSetGflag("bm_target_percentile")) {
      fatal("--bm_target_percentile requires --bm_mode=adaptive.");
    }
    if (userSetGflag("bm_target_precision_pct")) {
      fatal("--bm_target_precision_pct requires --bm_mode=adaptive.");
    }
    if (userSetGflag("bm_min_secs")) {
      fatal("--bm_min_secs requires --bm_mode=adaptive.");
    }
    if (userSetGflag("bm_min_samples")) {
      fatal("--bm_min_samples requires --bm_mode=adaptive.");
    }
    if (FLAGS_bm_estimate_time && !FLAGS_bm_quiet) {
      LOG(WARNING)
          << detail::kANSIBoldYellow
          << "--bm_estimate_time is slow, with odd semantics (geometric mean "
          << "of p25-p75). Consider --bm_mode=adaptive (BenchmarkAdaptive.md)."
          << detail::kANSIReset;
    }
  }
}

// Resolve `--bm_slice_usec` / `--bm_min_usec` (deprecated alias).
int64_t resolveSliceUsec() {
  bool minUsecSet = userSetGflag("bm_min_usec");
  bool sliceUsecSet = userSetGflag("bm_slice_usec");
  if (minUsecSet && sliceUsecSet) {
    LOG(ERROR)
        << detail::kANSIBoldRed
        << "Cannot set both --bm_min_usec and --bm_slice_usec. "
        << "Use --bm_slice_usec only (--bm_min_usec is deprecated)."
        << detail::kANSIReset;
    exit(1);
  }
  if (minUsecSet) {
    LOG(ERROR) << detail::kANSIBoldRed << "--bm_min_usec is deprecated; "
               << "use --bm_slice_usec instead." << detail::kANSIReset;
    return FLAGS_bm_min_usec;
  }
  return FLAGS_bm_slice_usec;
}

std::pair<std::set<std::string>, std::vector<detail::BenchmarkResult>>
runBenchmarksWithPrinterImpl(
    BenchmarkResultsPrinter* FOLLY_NULLABLE printer,
    const BenchmarksToRun& toRun,
    FunctionRef<detail::PerfScoped()> setUpPerf) {
  vector<detail::BenchmarkResult> results;
  results.reserve(toRun.benchmarks.size());

  auto const sliceUsec = resolveSliceUsec();
  if (sliceUsec < 1000) {
    LOG(WARNING) << detail::kANSIBoldYellow << "--bm_slice_usec=" << sliceUsec
                 << " is below 1000; benchmark harness overhead may "
                 << "interfere with results." << detail::kANSIReset;
  }

  validateFlagCombinations();

  if (!FLAGS_bm_quiet) {
    std::cerr << "For before/after comparisons, try "
                 "`folly/tool/benchmark_ab.py --help`.\n";
  }

  // PLEASE KEEP QUIET. MEASUREMENTS IN PROGRESS.

  // Adaptive mode: interleaved sampling with paired correction for both
  // baseline and suspender overhead. Does NOT set suspenderOverhead static;
  // instead uses late correction based on suspensionCount.
  if (FLAGS_bm_mode == "adaptive") {
    return runAdaptiveMode(printer, toRun, sliceUsec);
  }
  // Encourage users to try `adaptive` when `--bm_mode` was not passed.
  if (!userSetGflag("bm_mode")) {
    std::cerr
        << "NOTE: Default may change to " << detail::kANSIBold
        << "faster & more robust `--bm_mode=adaptive`." << detail::kANSIReset
        << "\n"
        << "      Details in BenchmarkAdaptive.md.\n"
        << "Pass `--bm_mode=best-of` to keep current behavior -- report trial "
        << "with lowest\n"
        << "iteration cost, each `1-2 * bm_slice_usec` long, repeated per "
        << "benchmark up to\n"
        << "`bm_max_trials` or `bm_max_secs`.\n";
  }
  // Best-of mode: measure suspender overhead upfront and set static for
  // immediate correction in tally().
  if (FLAGS_bm_verbose) {
    LOG(INFO) << "Measuring suspenderBaseline...";
  }
  auto const globalSuspenderBaseline = runBenchmarkGetNSPerIteration(
      toRun.suspenderBaseline->func, 0, sliceUsec);

  BenchmarkSuspender::suspenderOverhead = chrono::nanoseconds(
      static_cast<chrono::high_resolution_clock::rep>(
          globalSuspenderBaseline.first));

  if (FLAGS_bm_verbose) {
    LOG(INFO) << "suspenderOverhead=" << globalSuspenderBaseline.first
              << " ns/iter";
  }

  if (FLAGS_bm_verbose) {
    LOG(INFO) << "Measuring globalBaseline...";
  }
  auto const globalBaseline =
      runBenchmarkGetNSPerIteration(toRun.baseline->func, 0, sliceUsec);
  if (FLAGS_bm_verbose) {
    LOG(INFO) << "globalBaseline=" << globalBaseline.first << " ns/iter";
  }

  std::set<std::string> counterNames;
  ShouldDrawLineTracker shouldDrawLineTracker(toRun.separatorsAfter);
  for (std::size_t i = 0; i != toRun.benchmarks.size(); ++i) {
    std::pair<double, UserCounters> elapsed;
    const detail::BenchmarkRegistration& bm = *toRun.benchmarks[i];
    bool shouldDrawLineAfter = shouldDrawLineTracker();

    {
      detail::PerfScoped perf =
          isPseudoBenchmark(bm.name) ? detail::PerfScoped{} : setUpPerf();

      if (FLAGS_bm_profile) {
        elapsed = runProfilingGetNSPerIteration(bm.func, globalBaseline.first);
      } else {
        elapsed = FLAGS_bm_estimate_time
            ? runBenchmarkGetNSPerIterationEstimate(
                  bm.func, globalBaseline.first)
            : runBenchmarkGetNSPerIteration(
                  bm.func, globalBaseline.first, sliceUsec);
      }
    }

    // if customized user counters is used, it cannot print the result in real
    // time as it needs to run all cases first to know the complete set of
    // counters have been used, then the header can be printed out properly
    if (printer != nullptr) {
      printer->print({{bm.file, bm.name, elapsed.first, elapsed.second}});
      if (shouldDrawLineAfter) {
        printer->separator('-');
      }
    }
    results.push_back({bm.file, bm.name, elapsed.first, elapsed.second});

    // get all counter names
    for (auto const& kv : elapsed.second) {
      counterNames.insert(kv.first);
    }
  }

  // MEASUREMENTS DONE.

  return std::make_pair(std::move(counterNames), std::move(results));
}

std::vector<detail::BenchmarkResult> resultsFromFile(
    const std::string& filename) {
  std::string content;
  readFile(filename.c_str(), content);
  std::vector<detail::BenchmarkResult> ret;
  if (!content.empty()) {
    benchmarkResultsFromDynamic(parseJson(content), ret);
  }
  return ret;
}

bool writeResultsToFile(
    const std::vector<detail::BenchmarkResult>& results,
    const std::string& filename) {
  dynamic d;
  benchmarkResultsToDynamic(results, d);
  return writeFile(toPrettyJson(d), filename.c_str());
}

} // namespace

namespace detail {

std::ostream& operator<<(std::ostream& os, const BenchmarkResult& x) {
  folly::dynamic r;
  benchmarkResultsToDynamic({x}, r);
  return os << r[0];
}

bool operator==(const BenchmarkResult& x, const BenchmarkResult& y) {
  auto xtime = static_cast<std::uint64_t>(x.timeInNs * 1000);
  auto ytime = static_cast<std::uint64_t>(y.timeInNs * 1000);
  return x.name == y.name && x.file == y.file && xtime == ytime &&
      x.counters == y.counters;
}

std::chrono::high_resolution_clock::duration BenchmarkSuspenderBase::timeSpent;
std::chrono::high_resolution_clock::duration
    BenchmarkSuspenderBase::suspenderOverhead;
size_t BenchmarkSuspenderBase::suspensionCount;

void BenchmarkingStateBase::addBenchmarkImpl(
    std::string file, std::string name, BenchmarkFun fun, bool useCounter) {
  std::lock_guard guard(mutex_);
  benchmarks_.push_back(
      {std::move(file), std::move(name), std::move(fun), useCounter});
}

bool BenchmarkingStateBase::useCounters() const {
  std::lock_guard guard(mutex_);
  return std::any_of(
      benchmarks_.begin(), benchmarks_.end(), [](const auto& bm) {
        return bm.useCounter;
      });
}

std::vector<std::string> BenchmarkingStateBase::getBenchmarkList() {
  std::vector<std::string> bmNames;
  auto toRun = selectBenchmarksToRun(benchmarks_);
  bmNames.reserve(toRun.benchmarks.size());
  for (auto benchmarkRegistration : toRun.benchmarks) {
    bmNames.push_back(benchmarkRegistration->name);
  }

  return bmNames;
}

// static
folly::StringPiece BenchmarkingStateBase::getGlobalBaselineNameForTests() {
  return kGlobalBenchmarkBaseline;
}

folly::StringPiece
BenchmarkingStateBase::getGlobalSuspenderBaselineNameForTests() {
  return kGlobalBenchmarkSuspenderBaseline;
}

PerfScoped BenchmarkingStateBase::doSetUpPerfScoped(
    const std::vector<std::string>& args) const {
  return PerfScoped{args};
}

PerfScoped BenchmarkingStateBase::setUpPerfScoped() const {
#if FOLLY_PERF_IS_SUPPORTED
  std::vector<std::string> perfArgs;
  folly::split(' ', FLAGS_bm_perf_args, perfArgs, true);
  if (perfArgs.empty()) {
    return PerfScoped{};
  }
  try {
    return doSetUpPerfScoped(perfArgs);
  } catch (const std::exception& e) {
    fatalUsage(
        fmt::format(
            "--bm_perf_args=\"{}\" could not be started: {}",
            FLAGS_bm_perf_args,
            e.what()));
  }
#else
  return PerfScoped{};
#endif
}

template <typename Printer>
std::pair<std::set<std::string>, std::vector<BenchmarkResult>>
BenchmarkingStateBase::runBenchmarksWithPrinter(Printer* printer) const {
  if (FLAGS_bm_verbose) {
    LOG(INFO) << "Benchmark run starting...";
  }
  std::lock_guard guard(mutex_);
  BenchmarksToRun toRun = selectBenchmarksToRun(benchmarks_);
  validatePerfUsage(toRun);
  maybeRunWarmUpIteration(toRun);

  return runBenchmarksWithPrinterImpl(printer, toRun, [this] {
    return setUpPerfScoped();
  });
}

std::vector<BenchmarkResult> BenchmarkingStateBase::runBenchmarksWithResults()
    const {
  return runBenchmarksWithPrinter(
             static_cast<BenchmarkResultsPrinter*>(nullptr))
      .second;
}

std::vector<BenchmarkResult> runBenchmarksWithResults() {
  return globalBenchmarkState().runBenchmarksWithResults();
}

std::string benchmarkResultsToString(
    const std::vector<BenchmarkResult>& results,
    std::string_view indent,
    const std::vector<std::string>& annotations) {
  size_t maxAnnotationWidth = 0;
  for (const auto& a : annotations) {
    if (!a.empty()) {
      maxAnnotationWidth =
          std::max(maxAnnotationWidth, indent.size() + a.size());
    }
  }
  std::set<std::string> counterNames;
  for (const auto& r : results) {
    for (const auto& [key, _] : r.counters) {
      counterNames.insert(key);
    }
  }
  std::ostringstream oss;
  BenchmarkResultsPrinter printer(
      std::move(counterNames), &oss, indent, maxAnnotationWidth);
  printer.print(results, annotations);
  printer.separator('=');
  return oss.str();
}

} // namespace detail

void runBenchmarks() {
  warnIfCommandLineFlagsWereNotParsed();

  auto& state = detail::globalBenchmarkState();

  if (FLAGS_bm_list) {
    auto bmNames = state.getBenchmarkList();
    for (const auto& testName : bmNames) {
      std::cout << testName << std::endl;
    }
    return;
  }

  if (FLAGS_bm_profile) {
    printf(
        "WARNING: Running with constant number of iterations. Results might be jittery.\n");
  }

  if (FLAGS_bm_min_iters >= FLAGS_bm_max_iters) {
    std::cerr << "WARNING: bm_min_iters > bm_max_iters; increasing the max"
              << std::endl;
    FLAGS_bm_max_iters = FLAGS_bm_min_iters + 1;
  }

  checkRunMode();

  BenchmarkResultsPrinter printer;
  bool useCounter = state.useCounters();

  // PLEASE KEEP QUIET. MEASUREMENTS IN PROGRESS.

  const bool shouldPrintInline =
      FLAGS_bm_relative_to.empty() && !FLAGS_json && !useCounter;
  auto benchmarkResults =
      state.runBenchmarksWithPrinter(shouldPrintInline ? &printer : nullptr);

  // PLEASE MAKE NOISE. MEASUREMENTS DONE.

  if (FLAGS_json) {
    printBenchmarkResultsAsJson(benchmarkResults.second);
  } else if (!FLAGS_bm_relative_to.empty()) {
    printResultComparison(
        resultsFromFile(FLAGS_bm_relative_to), benchmarkResults.second);
  } else if (!shouldPrintInline) {
    printer = BenchmarkResultsPrinter{std::move(benchmarkResults.first)};
    printer.print(benchmarkResults.second);
    printer.separator('=');
  }

  if (!FLAGS_bm_json_verbose.empty()) {
    writeResultsToFile(benchmarkResults.second, FLAGS_bm_json_verbose);
  }

  checkRunMode();
}

} // namespace folly
