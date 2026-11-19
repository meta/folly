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

#include <folly/BenchmarkUtil.h>

#include <array> // @manual
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

#include <fmt/format.h>

#include <folly/lang/Align.h>
#include <folly/lang/Exception.h>
#include <folly/portability/Sched.h>
#include <folly/portability/Windows.h>
#include <folly/system/arch/x86.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif

namespace folly {

namespace detail {

size_t bm_llc_size_fallback() {
#if defined(__linux__)

  /// sysctl is deprecated on Linux; walk sysfs

  int cpu = sched_getcpu();
  if (cpu < 0) {
    return 0;
  }

  auto dir = std::filesystem::path(
      fmt::format("/sys/devices/system/cpu/cpu{}/cache", cpu));

  size_t size = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().filename().native().starts_with("index")) {
      std::ifstream size_ifs(entry.path() / "size");
      std::string size_str;
      size_ifs >> size_str;
      size_t size_num = std::stoul(size_str);
      if (size_str.back() == 'K') {
        size_num *= 1024;
      } else if (size_str.back() == 'M') {
        size_num *= 1024 * 1024;
      }
      size = std::max(size, size_num);
    }
  }

  return size;

#elif defined(__APPLE__) || defined(__FreeBSD__)

  auto names = std::array{
      "hw.l3cachesize", // FreeBSD, Intel Mac
      "hw.l2cachesize", // Apple Silicon
  };
  for (auto name : names) {
    uint64_t size = 0;
    size_t len = sizeof(size);
    if (sysctlbyname(name, &size, &len, NULL, 0) == 0 && size > 0) {
      return size;
    }
  }

  return 0;

#elif defined(_WIN32)

  DWORD length = 0;
  GetLogicalProcessorInformation(nullptr, &length);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0) {
    return 0;
  }
  if (length % sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) != 0) {
    return 0;
  }

  size_t const count = length / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
  std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> infos(count);
  if (!GetLogicalProcessorInformation(infos.data(), &length)) {
    return 0;
  }

  size_t size = 0;
  for (auto const& info : infos) {
    if (info.Relationship == RelationCache) {
      size = std::max<size_t>(size, info.Cache.Size);
    }
  }
  return size;

#else

  return 0;

#endif
}

} // namespace detail

size_t bm_llc_size() {
  return kIsArchX86 || kIsArchAmd64
      ? folly::x86_cpuid_get_llc_cache_info().cache_size()
      : detail::bm_llc_size_fallback();
}

void bm_llc_evict(size_t value) {
  constexpr auto words_per_line =
      folly::hardware_constructive_interference_size / sizeof(size_t);
  static_assert(words_per_line > 0);

  static auto const word_count = [] {
    auto const llc_size = bm_llc_size();
    if (llc_size == 0) {
      throw_exception<std::runtime_error>("Unable to determine LLC size");
    }
    auto const max_llc_bytes =
        align_floor(std::numeric_limits<size_t>::max() / 2, sizeof(size_t));
    if (llc_size > max_llc_bytes) {
      throw_exception<std::length_error>("LLC eviction region is too large");
    }
    auto const llc_words =
        align_ceil(llc_size, sizeof(size_t)) / sizeof(size_t);
    return 2 * llc_words;
  }();
  static std::unique_ptr<size_t volatile[]> const region{
      new size_t[word_count]};
  detail::bm_llc_evict_touch({region.get(), word_count}, words_per_line, value);
}

void detail::bm_llc_evict_touch(
    std::span<size_t volatile> region, size_t words_per_line, size_t value) {
  for (size_t i = 0; i < region.size(); i += words_per_line) {
    region[i] = value;
  }
  if (!region.empty()) {
    region.back() = value;
  }
}

} // namespace folly
