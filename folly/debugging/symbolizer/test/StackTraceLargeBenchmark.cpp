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

// Benchmark for stack trace capture with sdata8 .eh_frame_hdr tables.
//
// This binary is linked with eh_sdata8_large.lds which places .eh_frame_hdr
// at 0x200000000, forcing DW_EH_PE_sdata8 table entries. Compare results
// against the regular stack_trace_benchmark to measure the overhead of
// sdata8 binary search vs sdata4 binary search in libunwind.
//
// The manyFdes benchmark creates 10,000 unique template instantiations,
// each with its own FDE entry in .eh_frame, and calls getStackTrace from
// each one. This stresses libunwind's sdata8 binary search table.
// With linear search, lookup time scales with table size O(N).
// With binary search, it's O(log 10000) ~ 13 comparisons.
//
// Run:
//   buck2 run
//   fbsource//xplat/folly/debugging/symbolizer/test:stack_trace_large_benchmark

#include <folly/Benchmark.h>
#include <folly/debugging/symbolizer/StackTrace.h>
#include <folly/init/Init.h>

#include <link.h>
#include <glog/logging.h>

#include <array>

using namespace folly;
using namespace folly::symbolizer;

namespace {

constexpr uint8_t kDwEhPeSdata4 = 0x0b;
constexpr uint8_t kDwEhPeSdata8 = 0x0c;
constexpr uint8_t kDwEhPeFormatMask = 0x0f;

struct EhFrameHdrInfo {
  bool found = false;
  uint8_t tableEnc = 0;
};

int ehFrameHdrCallback(struct dl_phdr_info* info, size_t, void* data) {
  auto* result = static_cast<EhFrameHdrInfo*>(data);
  if (info->dlpi_name[0] != '\0') {
    return 0;
  }
  for (int i = 0; i < info->dlpi_phnum; i++) {
    if (info->dlpi_phdr[i].p_type == PT_GNU_EH_FRAME) {
      const auto* hdr = reinterpret_cast<const uint8_t*>(
          info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
      result->found = true;
      result->tableEnc = hdr[3];
      return 1;
    }
  }
  return 0;
}

void validateEhFrameHdr() {
  EhFrameHdrInfo info;
  dl_iterate_phdr(ehFrameHdrCallback, &info);

  if (!info.found) {
    LOG(WARNING) << "No .eh_frame_hdr found in main executable";
    return;
  }

  uint8_t format = info.tableEnc & kDwEhPeFormatMask;
  const char* formatStr = (format == kDwEhPeSdata8) ? "sdata8"
      : (format == kDwEhPeSdata4)
      ? "sdata4"
      : "unknown";

  LOG(INFO) << ".eh_frame_hdr table_enc=0x" << std::hex
            << static_cast<int>(info.tableEnc) << " (" << formatStr << ")";

  if (format != kDwEhPeSdata8) {
    LOG(FATAL)
        << "Binary uses " << formatStr << " .eh_frame_hdr table encoding (0x"
        << std::hex << static_cast<int>(info.tableEnc) << "). "
        << "Expected sdata8. Ensure the linker script "
        << "eh_sdata8_large.lds is applied correctly.";
  }
}

// --- Stack trace benchmarks at various frame depths ---

unsigned int basic(size_t nFrames) {
  unsigned int iters = 10000;
  constexpr size_t kMaxAddresses = 100;
  uintptr_t addresses[kMaxAddresses];

  CHECK_LE(nFrames, kMaxAddresses);

  for (unsigned int i = 0; i < iters; ++i) {
    ssize_t n = getStackTrace(addresses, nFrames);
    CHECK_NE(n, -1);
  }
  return iters - nFrames;
}

unsigned int Recurse(size_t nFrames, size_t remaining) {
  if (remaining == 0) {
    return basic(nFrames);
  } else {
    return Recurse(nFrames, remaining - 1) + 1;
  }
}

unsigned int SetupStackAndTest(int /* iters */, size_t nFrames) {
  return Recurse(nFrames, nFrames);
}

BENCHMARK_NAMED_PARAM_MULTI(SetupStackAndTest, 2_frames, 2)
BENCHMARK_NAMED_PARAM_MULTI(SetupStackAndTest, 4_frames, 4)
BENCHMARK_NAMED_PARAM_MULTI(SetupStackAndTest, 8_frames, 8)
BENCHMARK_NAMED_PARAM_MULTI(SetupStackAndTest, 16_frames, 16)
BENCHMARK_NAMED_PARAM_MULTI(SetupStackAndTest, 32_frames, 32)
BENCHMARK_NAMED_PARAM_MULTI(SetupStackAndTest, 64_frames, 64)

BENCHMARK_DRAW_LINE();

// --- Many unique FDEs: stress libunwind's search table size ---
// Each template instantiation creates a unique noinline function with its
// own FDE in .eh_frame. Calling getStackTrace from each unique function
// forces libunwind to look up that function's FDE in the sdata8 table.
//
// 10,000 unique functions, each with its own FDE entry.
// With linear search, lookup time scales with table size O(N).
// With binary search, it's O(log 10000) ~ 13 comparisons.

template <size_t N>
__attribute__((noinline)) ssize_t traceFromUniqueFunction() {
  constexpr size_t kMaxAddresses = 100;
  uintptr_t addresses[kMaxAddresses];
  return getStackTrace(addresses, kMaxAddresses);
}

constexpr int kNumUniqueFns = 10000;

template <size_t... Is>
constexpr auto makeFns(std::index_sequence<Is...>)
    -> std::array<ssize_t (*)(), sizeof...(Is)> {
  return {traceFromUniqueFunction<Is>...};
}

void manyFdes(unsigned int n, unsigned int) {
  auto fns = makeFns(std::make_index_sequence<kNumUniqueFns>{});
  for (unsigned int i = 0; i < n; ++i) {
    for (auto& fn : fns) {
      ssize_t frames = fn();
      CHECK_GT(frames, 0);
    }
  }
}
BENCHMARK_PARAM(manyFdes, 1)

} // namespace

int main(int argc, char* argv[]) {
  folly::Init init(&argc, &argv, true);
  validateEhFrameHdr();
  folly::runBenchmarks();
  return 0;
}
