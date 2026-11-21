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

#pragma once

#include <cstddef>
#include <ostream>
#include <type_traits>
#include <vector>

#include <folly/container/detail/F14Policy.h>
#include <folly/container/detail/F14Table.h>

namespace folly {
namespace f14 {

struct Histo {
  std::vector<std::size_t> const& data;
};

inline std::ostream& operator<<(std::ostream& xo, Histo const& histo) {
  xo << "[";
  size_t sum = 0;
  for (auto v : histo.data) {
    sum += v;
  }
  auto const dsum = static_cast<double>(sum);
  size_t partial = 0;
  for (size_t i = 0; i < histo.data.size(); ++i) {
    if (i > 0) {
      xo << ", ";
    }
    partial += histo.data[i];
    if (histo.data[i] > 0) {
      xo << i << ": " << histo.data[i] << " ("
         << (static_cast<double>(partial) * 100.0 / dsum) << "%)";
    }
  }
  xo << "]";
  return xo;
}

inline double expectedProbe(std::vector<std::size_t> const& probeLengths) {
  std::size_t sum = 0;
  std::size_t count = 0;
  for (std::size_t i = 1; i < probeLengths.size(); ++i) {
    sum += i * probeLengths[i];
    count += probeLengths[i];
  }
  return static_cast<double>(sum) / static_cast<double>(count);
}

// Returns i such that probeLengths elements 0 to i (inclusive) account
// for at least 99% of the samples.
inline std::size_t p99Probe(std::vector<std::size_t> const& probeLengths) {
  std::size_t count = 0;
  for (std::size_t i = 1; i < probeLengths.size(); ++i) {
    count += probeLengths[i];
  }
  std::size_t rv = probeLengths.size();
  std::size_t suffix = 0;
  while ((suffix + probeLengths[rv - 1]) * 100 <= count) {
    --rv;
  }
  return rv;
}

inline std::ostream& operator<<(std::ostream& xo, F14TableStats const& stats) {
  xo << "{ " << std::endl;
  xo << "  policy: " << stats.policy << std::endl;
  xo << "  size: " << stats.size << std::endl;
  xo << "  valueSize: " << stats.valueSize << std::endl;
  xo << "  bucketCount: " << stats.bucketCount << std::endl;
  xo << "  chunkCount: " << stats.chunkCount << std::endl;
  xo << "  chunkOccupancyHisto" << Histo{stats.chunkOccupancyHisto}
     << std::endl;
  xo << "  chunkOutboundOverflowHisto"
     << Histo{stats.chunkOutboundOverflowHisto} << std::endl;
  xo << "  chunkHostedOverflowHisto" << Histo{stats.chunkHostedOverflowHisto}
     << std::endl;
  xo << "  keyProbeLengthHisto" << Histo{stats.keyProbeLengthHisto}
     << std::endl;
  xo << "  missProbeLengthHisto" << Histo{stats.missProbeLengthHisto}
     << std::endl;
  xo << "  totalBytes: " << stats.totalBytes << std::endl;
  xo << "  valueBytes: " << (stats.size * stats.valueSize) << std::endl;
  xo << "  overheadBytes: " << stats.overheadBytes << std::endl;
  if (stats.size > 0) {
    xo << "  overheadBytesPerKey: "
       << (static_cast<double>(stats.overheadBytes) /
           static_cast<double>(stats.size))
       << std::endl;
  }
  xo << "}";
  return xo;
}

template <typename Container>
std::vector<typename std::decay_t<Container>::value_type> asVector(
    const Container& c) {
  return {c.begin(), c.end()};
}

} // namespace f14
} // namespace folly
