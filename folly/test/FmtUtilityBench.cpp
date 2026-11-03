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

#include <folly/FmtUtility.h>

#include <string>

#include <folly/Benchmark.h>
#include <folly/init/Init.h>

namespace {
struct GeneratedString {
  std::string_view value;
};
} // namespace

template <>
struct fmt::formatter<GeneratedString>
    : folly::fmt_sized_formatter_base<fmt::formatter<GeneratedString>> {
  static size_t size_hint(GeneratedString const& arg) {
    return arg.value.size();
  }

  template <typename Sink>
  static void append(GeneratedString const& arg, Sink& sink) {
    sink.append(arg.value);
  }
};

namespace {
template <size_t Size, unsigned Method, bool Generated = false>
void formatStrings(unsigned iters) {
  std::string value;
  BENCHMARK_SUSPEND {
    value.assign(Size, 'x');
  }
  decltype(auto) arg = [&]() -> decltype(auto) {
    if constexpr (Generated) {
      return GeneratedString{value};
    } else {
      return (value);
    }
  }();
  while (iters--) {
    if constexpr (Method == 0) {
      folly::doNotOptimizeAway(fmt::format("{}!{}!{}!{}!", arg, arg, arg, arg));
    } else if constexpr (Method == 1) {
      folly::doNotOptimizeAway(
          fmt::format(FOLLY_FMT_COMPILE("{}!{}!{}!{}!"), arg, arg, arg, arg));
    } else if constexpr (Method == 2) {
      folly::doNotOptimizeAway(
          folly::fmt_sized_format("{}!{}!{}!{}!", arg, arg, arg, arg));
    } else {
      folly::doNotOptimizeAway(
          folly::fmt_sized_format(
              FOLLY_FMT_COMPILE("{}!{}!{}!{}!"), arg, arg, arg, arg));
    }
  }
}
} // namespace

#define FOLLY_FMT_BENCHMARKS(size)                   \
  BENCHMARK(fmt_##size, iters) {                     \
    formatStrings<size, 0>(iters);                   \
  }                                                  \
  BENCHMARK_RELATIVE(compiled_##size, iters) {       \
    formatStrings<size, 1>(iters);                   \
  }                                                  \
  BENCHMARK_RELATIVE(sized_##size, iters) {          \
    formatStrings<size, 2>(iters);                   \
  }                                                  \
  BENCHMARK_RELATIVE(sized_compiled_##size, iters) { \
    formatStrings<size, 3>(iters);                   \
  }                                                  \
  BENCHMARK_DRAW_LINE()

FOLLY_FMT_BENCHMARKS(1);
FOLLY_FMT_BENCHMARKS(20);
FOLLY_FMT_BENCHMARKS(150);
#define FOLLY_FMT_GENERATED_BENCHMARKS(size)                   \
  BENCHMARK(generated_fmt_##size, iters) {                     \
    formatStrings<size, 0, true>(iters);                       \
  }                                                            \
  BENCHMARK_RELATIVE(generated_compiled_##size, iters) {       \
    formatStrings<size, 1, true>(iters);                       \
  }                                                            \
  BENCHMARK_RELATIVE(generated_sized_##size, iters) {          \
    formatStrings<size, 2, true>(iters);                       \
  }                                                            \
  BENCHMARK_RELATIVE(generated_sized_compiled_##size, iters) { \
    formatStrings<size, 3, true>(iters);                       \
  }                                                            \
  BENCHMARK_DRAW_LINE()

FOLLY_FMT_BENCHMARKS(1024);
FOLLY_FMT_GENERATED_BENCHMARKS(20);
FOLLY_FMT_GENERATED_BENCHMARKS(150);
FOLLY_FMT_GENERATED_BENCHMARKS(1024);

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
