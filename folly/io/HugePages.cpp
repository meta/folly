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

#include <folly/portability/Windows.h>

#include <folly/io/HugePages.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <cctype>
#include <cstring>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <system_error>

#include <folly/Conv.h>
#include <folly/Exception.h>
#include <folly/Range.h>
#include <folly/String.h>

#include <folly/gen/Base.h>
#include <folly/gen/File.h>
#include <folly/gen/String.h>

namespace folly {

namespace {

bool isAsciiSpace(char c) {
  return c == ' ' || (c >= '\t' && c <= '\r');
}

bool isAsciiDigit(char c) {
  return c >= '0' && c <= '9';
}

StringPiece trimAsciiSpace(StringPiece s) {
  while (!s.empty() && isAsciiSpace(s.front())) {
    s.pop_front();
  }
  while (!s.empty() && isAsciiSpace(s.back())) {
    s.pop_back();
  }
  return s;
}

// Converts a nonempty string of decimal digits in KiB to bytes.
std::optional<size_t> parseKiB(StringPiece digits) {
  if (digits.empty() ||
      !std::all_of(digits.begin(), digits.end(), isAsciiDigit)) {
    return std::nullopt;
  }
  return to<size_t>(digits) * 1024;
}

// Parses the value of a hugetlbfs pagesize mount option: decimal digits, an
// optional case-insensitive K/M/G/T suffix, and any trailing text.
size_t parsePageSizeValue(StringPiece value) {
  auto const digitsEnd =
      std::find_if_not(value.begin(), value.end(), isAsciiDigit);
  if (digitsEnd == value.begin()) {
    throw std::runtime_error("Invalid pagesize option");
  }
  auto const size = to<size_t>(StringPiece(value.begin(), digitsEnd));
  char const c = digitsEnd == value.end()
      ? '\0'
      : char(std::tolower(static_cast<unsigned char>(*digitsEnd)));
  auto const mult = [c] {
    switch (c) {
      case 't':
        return 1ull << 40;
      case 'g':
        return 1ull << 30;
      case 'm':
        return 1ull << 20;
      case 'k':
        return 1ull << 10;
      default:
        return 1ull << 0;
    }
  }();
  return size * mult;
}

// Get the default huge page size
size_t getDefaultHugePageSize() {
  // We need to parse /proc/meminfo
  size_t pageSize = 0;

  bool error = gen::byLine("/proc/meminfo") | [&](StringPiece line) -> bool {
    if (!line.removePrefix("Hugepagesize:") || !line.removeSuffix("kB")) {
      return true;
    }
    if (auto size = parseKiB(trimAsciiSpace(line))) {
      pageSize = *size;
      return false; // stop
    }
    return true;
  };

  if (error) {
    throw std::runtime_error("Can't find default huge page size");
  }
  return pageSize;
}

// Get raw huge page sizes (without mount points, they'll be filled later)
HugePageSizeVec readRawHugePageSizes() {
  // We need to parse file names from /sys/kernel/mm/hugepages
  HugePageSizeVec vec;
  fs::path path("/sys/kernel/mm/hugepages");
  if (fs::exists(path)) {
    for (fs::directory_iterator it(path); it != fs::directory_iterator();
         ++it) {
      std::string filename(it->path().filename().string());
      StringPiece name(filename);
      if (!name.removePrefix("hugepages-") || !name.removeSuffix("kB")) {
        continue;
      }
      if (auto size = parseKiB(name)) {
        vec.emplace_back(*size);
      }
    }
  }
  return vec;
}

/**
 * Get list of supported huge page sizes and their mount points, if
 * hugetlbfs file systems are mounted for those sizes.
 */
HugePageSizeVec readHugePageSizes() {
  HugePageSizeVec sizeVec = readRawHugePageSizes();
  if (sizeVec.empty()) {
    return sizeVec; // nothing to do
  }
  std::sort(sizeVec.begin(), sizeVec.end());

  size_t defaultHugePageSize = getDefaultHugePageSize();

  struct PageSizeLess {
    bool operator()(const HugePageSize& a, size_t b) const {
      return a.size < b;
    }
  };

  // Read and parse /proc/mounts
  std::vector<StringPiece> parts;
  std::vector<StringPiece> options;

  gen::byLine("/proc/mounts") | gen::eachAs<StringPiece>() |
      [&](StringPiece line) {
        parts.clear();
        split(' ', line, parts);
        // device path fstype options uid gid
        if (parts.size() != 6) {
          throw std::runtime_error("Invalid /proc/mounts line");
        }
        if (parts[2] != "hugetlbfs") {
          return; // we only care about hugetlbfs
        }

        options.clear();
        split(',', parts[3], options);
        size_t pageSize = defaultHugePageSize;
        // Search for the "pagesize" option, which must have a value
        for (auto& option : options) {
          // key=value
          auto p = static_cast<const char*>(
              memchr(option.data(), '=', option.size()));
          if (!p) {
            continue;
          }
          if (StringPiece(option.data(), p) != "pagesize") {
            continue;
          }
          pageSize = parsePageSizeValue(StringPiece(p + 1, option.end()));
          break;
        }

        auto pos = std::lower_bound(
            sizeVec.begin(), sizeVec.end(), pageSize, PageSizeLess());
        if (pos == sizeVec.end() || pos->size != pageSize) {
          throw std::runtime_error("Mount page size not found");
        }
        if (!pos->mountPoint.empty()) {
          // Only one mount point per page size is allowed
          return;
        }

        // Store mount point
        fs::path path(parts[1].begin(), parts[1].end());
        struct stat st;
        const int ret = stat(path.string().c_str(), &st);
        if (ret == -1 && errno == ENOENT) {
          return;
        }
        checkUnixError(ret, "stat hugepage mountpoint failed");
        pos->mountPoint = fs::canonical(path);
        pos->device = st.st_dev;
      };

  return sizeVec;
}

} // namespace

const HugePageSizeVec& getHugePageSizes() {
  static HugePageSizeVec sizes = readHugePageSizes();
  return sizes;
}

const HugePageSize* getHugePageSize(size_t size) {
  // Linear search is just fine.
  for (auto& p : getHugePageSizes()) {
    if (p.mountPoint.empty()) {
      continue;
    }
    if (size == 0 || size == p.size) {
      return &p;
    }
  }
  return nullptr;
}

const HugePageSize* getHugePageSizeForDevice(dev_t device) {
  // Linear search is just fine.
  for (auto& p : getHugePageSizes()) {
    if (p.mountPoint.empty()) {
      continue;
    }
    if (device == p.device) {
      return &p;
    }
  }
  return nullptr;
}

} // namespace folly
