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

#include <folly/compression/QuotientMultiSet.h>

#include <algorithm>

#include <fmt/format.h>

#include <folly/Benchmark.h>
#include <folly/Random.h>
#include <folly/String.h>
#include <folly/compression/elias_fano/EliasFanoCoding.h>
#include <folly/compression/test/CodingTestUtils.h>
#include <folly/container/Enumerate.h>
#include <folly/container/F14Set.h>
#include <folly/init/Init.h>

DEFINE_int64(
    key_bits,
    32,
    "The number of key bits used in quotient multiset. Should always <= 64");
DEFINE_uint64(
    num_elements,
    100000000,
    "The number of elements inserted into quotient multiset");
DEFINE_double(load_factor, 0.95, "Load factor of the multiset");

#if FOLLY_QUOTIENT_MULTI_SET_SUPPORTED

using std::string;

namespace {

static const unsigned int kRunsPerIteration = 5000000;

std::mt19937 rng;

// Uniformly distributed keys.
std::vector<uint64_t> uniform;
string qmsData;

uint64_t maxValue(uint32_t nbits) {
  return nbits == 64
      ? std::numeric_limits<uint64_t>::max()
      : (uint64_t(1) << nbits) - 1;
}

// Hash 64 bits into 1.
uint64_t mix64_to_1(uint64_t word) {
  // Constant from MurmurHash3 final mixer.
  return (word * UINT64_C(0xff51afd7ed558ccd)) >> 63;
}

void buildQuotientMultiSet(std::vector<uint64_t>& keys) {
  folly::QuotientMultiSetBuilder builder(
      FLAGS_key_bits, keys.size(), FLAGS_load_factor);
  folly::IOBufQueue buff;
  for (const auto& iter : folly::enumerate(keys)) {
    if (builder.insert(*iter)) {
      builder.setBlockPayload(iter.index);
    }
    if (builder.numReadyBlocks() >= 1) {
      builder.flush(buff);
    }
  }
  builder.close(buff);
  qmsData = buff.move()->to<string>();
}

std::vector<uint64_t> makeLookupKeys(size_t n, double hitRate) {
  folly::BenchmarkSuspender guard;
  std::vector<uint64_t> keys;
  keys.reserve(n);
  uint64_t maxKey = maxValue(FLAGS_key_bits);
  for (uint64_t idx = 0; idx < n; idx++) {
    keys.push_back(
        (folly::Random::randDouble01(rng) < hitRate)
            ? uniform[folly::Random::rand64(uniform.size(), rng)]
            : folly::Random::rand64(rng) & maxKey);
  }
  return keys;
}

const folly::F14FastSet<uint64_t>& getF14Baseline() {
  folly::BenchmarkSuspender guard;
  static const auto set = [] {
    folly::F14FastSet<uint64_t> ret(uniform.begin(), uniform.end());
    LOG(INFO) << fmt::format(
        "Built F14FastSet, size: {}, space: {}",
        ret.size(),
        folly::prettyPrint(
            ret.getAllocatedMemorySize(), folly::PrettyType::PRETTY_BYTES_IEC));
    return ret;
  }();
  return set;
}

using EFEncoder =
    folly::compression::EliasFanoEncoder<uint64_t, uint64_t, 128, 128>;

const folly::compression::MutableEliasFanoCompressedList& getEFBaseline() {
  folly::BenchmarkSuspender guard;
  static auto list = [] {
    auto ret = EFEncoder::encode(uniform.begin(), uniform.end());
    LOG(INFO) << fmt::format(
        "Built Elias-Fano list, space: {}",
        folly::prettyPrint(
            ret.data.size(), folly::PrettyType::PRETTY_BYTES_IEC));
    return ret;
  }();
  return list;
}

template <class Lookup>
size_t benchmarkHits(const Lookup& lookup) {
  auto keys = makeLookupKeys(kRunsPerIteration, /* hitRate */ 1);
  for (const auto& key : keys) {
    auto found = lookup(key);
    folly::doNotOptimizeAway(found);
    DCHECK(found);
  }
  return kRunsPerIteration;
}

template <class Lookup>
size_t benchmarkRandom(const Lookup& lookup) {
  auto keys = makeLookupKeys(kRunsPerIteration, /* hitRate */ 0);
  for (const auto& key : keys) {
    auto found = lookup(key);
    folly::doNotOptimizeAway(found);
  }
  return kRunsPerIteration;
}

template <class Lookup>
size_t benchmarkMixtureSerialized(const Lookup& lookup) {
  // Make the result unpredictable so we can use it to introduce a
  // serializing dependency in the loop.
  auto keys = makeLookupKeys(kRunsPerIteration * 2, /* hitRate */ 0.5);
  size_t unpredictableBit = 0;
  for (size_t i = 0; i < kRunsPerIteration * 2; i += 2) {
    unpredictableBit = lookup(keys[i + unpredictableBit]) ? 1 : 0;
    folly::doNotOptimizeAway(unpredictableBit);
  }
  return kRunsPerIteration;
}

template <class Lookup>
size_t benchmarkSerializedOnResultBits(const Lookup& lookup, double hitRate) {
  auto keys = makeLookupKeys(kRunsPerIteration * 2, hitRate);
  size_t unpredictableBit = 0;
  for (size_t i = 0; i < kRunsPerIteration * 2; i += 2) {
    // When all keys are hits we can use a hash of the location of the
    // found key to introduce a serializing dependency in the loop.
    unpredictableBit = mix64_to_1(lookup(keys[i + unpredictableBit]));
    folly::doNotOptimizeAway(unpredictableBit);
  }
  return kRunsPerIteration;
}

template <class Lookup>
size_t benchmarkHitsSerialized(const Lookup& lookup) {
  return benchmarkSerializedOnResultBits(lookup, /* hitRate */ 1);
}

template <class Lookup>
size_t benchmarkRandomSerialized(const Lookup& lookup) {
  return benchmarkSerializedOnResultBits(lookup, /* hitRate */ 0);
}

template <class Lookup>
size_t benchmarkHitsSmallWorkingSet(const Lookup& lookup) {
  // Loop over a small set of keys so that after the first iteration
  // all the relevant parts of the data structure are in LLC cache.
  constexpr size_t kLookupSetSize = 1 << 12; // Should be a power of 2.
  auto keys = makeLookupKeys(kLookupSetSize, /* hitRate */ 1);
  for (size_t i = 0; i < kRunsPerIteration; ++i) {
    auto found = lookup(keys[i % kLookupSetSize]);
    folly::doNotOptimizeAway(found);
    DCHECK(found);
  }
  return kRunsPerIteration;
}

} // namespace

void benchmarkSetup() {
  rng.seed(UINT64_C(12345678));

  uint64_t maxKey = maxValue(FLAGS_key_bits);
  // Uniformly distributed keys.
  const uint64_t size = FLAGS_num_elements;
  for (uint64_t idx = 0; idx < size; idx++) {
    uint64_t key = folly::Random::rand64(rng) & maxKey;
    uniform.emplace_back(key);
  }
  std::sort(uniform.begin(), uniform.end());
  buildQuotientMultiSet(uniform);

  LOG(INFO) << fmt::format(
      "Built QuotientMultiSet, space: {}",
      folly::prettyPrint(qmsData.size(), folly::PrettyType::PRETTY_BYTES_IEC));
}

BENCHMARK_MULTI(QuotientMultiSetGetHits) {
  size_t ret = 0;
  folly::compression::dispatchInstructions([&](auto instructions) {
    auto reader = folly::QuotientMultiSet<decltype(instructions)>(qmsData);
    ret = benchmarkHits([&](uint64_t key) { return reader.equalRange(key); });
  });
  return ret;
}

BENCHMARK_MULTI(QuotientMultiSetGetRandom) {
  size_t ret = 0;
  folly::compression::dispatchInstructions([&](auto instructions) {
    auto reader = folly::QuotientMultiSet<decltype(instructions)>(qmsData);
    ret = benchmarkRandom([&](uint64_t key) { return reader.equalRange(key); });
  });
  return ret;
}

BENCHMARK_MULTI(QuotientMultiSetGetHitsSmallWorkingSet) {
  size_t ret = 0;
  folly::compression::dispatchInstructions([&](auto instructions) {
    auto reader = folly::QuotientMultiSet<decltype(instructions)>(qmsData);
    ret = benchmarkHitsSmallWorkingSet([&](uint64_t key) {
      return reader.equalRange(key);
    });
  });
  return ret;
}

BENCHMARK_MULTI(QuotientMultiSetGetMixtureSerialized) {
  size_t ret = 0;
  folly::compression::dispatchInstructions([&](auto instructions) {
    auto reader = folly::QuotientMultiSet<decltype(instructions)>(qmsData);
    ret = benchmarkMixtureSerialized([&](uint64_t key) {
      return reader.equalRange(key);
    });
  });
  return ret;
}

BENCHMARK_MULTI(QuotientMultiSetGetHitsSerialized) {
  size_t ret = 0;
  folly::compression::dispatchInstructions([&](auto instructions) {
    auto reader = folly::QuotientMultiSet<decltype(instructions)>(qmsData);
    ret = benchmarkHitsSerialized([&](uint64_t key) {
      return reader.equalRange(key).begin;
    });
  });
  return ret;
}

BENCHMARK_MULTI(QuotientMultiSetGetRandomSerialized) {
  size_t ret = 0;
  folly::compression::dispatchInstructions([&](auto instructions) {
    auto reader = folly::QuotientMultiSet<decltype(instructions)>(qmsData);
    ret = benchmarkRandomSerialized([&](uint64_t key) {
      // Even without a match, begin will depend on the whole
      // computation.
      return reader.equalRange(key).begin;
    });
  });
  return ret;
}

BENCHMARK_DRAW_LINE();

BENCHMARK_MULTI(F14MapHits) {
  const auto& baseline = getF14Baseline();
  return benchmarkHits([&](uint64_t key) { return baseline.contains(key); });
}

BENCHMARK_MULTI(F14MapRandom) {
  const auto& baseline = getF14Baseline();
  return benchmarkRandom([&](uint64_t key) { return baseline.contains(key); });
}

BENCHMARK_MULTI(F14MapHitsSmallWorkingSet) {
  const auto& baseline = getF14Baseline();
  return benchmarkHitsSmallWorkingSet([&](uint64_t key) {
    return baseline.contains(key);
  });
}

BENCHMARK_MULTI(F14MapMixtureSerialized) {
  const auto& baseline = getF14Baseline();
  return benchmarkMixtureSerialized([&](uint64_t key) {
    return baseline.contains(key);
  });
}

BENCHMARK_MULTI(F14MapHitsSerialized) {
  const auto& baseline = getF14Baseline();
  return benchmarkHitsSerialized([&](uint64_t key) {
    return reinterpret_cast<uintptr_t>(&*baseline.find(key));
  });
}

// benchmarkRandomSerialized() cannot be used with F14.

BENCHMARK_DRAW_LINE();

using folly::compression::EliasFanoReader;

BENCHMARK_MULTI(EliasFanoGetHits) {
  auto list = getEFBaseline();
  size_t ret = 0;
  folly::compression::dispatchInstructions([&](auto instructions) {
    auto reader = EliasFanoReader<EFEncoder, decltype(instructions)>(list);
    ret = benchmarkHits([&](uint64_t key) {
      return reader.jumpTo(key) && reader.value() == key;
    });
  });
  return ret;
}

BENCHMARK_MULTI(EliasFanoGetRandom) {
  auto list = getEFBaseline();
  size_t ret = 0;
  folly::compression::dispatchInstructions([&](auto instructions) {
    auto reader = EliasFanoReader<EFEncoder, decltype(instructions)>(list);
    ret = benchmarkRandom([&](uint64_t key) {
      return reader.jumpTo(key) && reader.value() == key;
    });
  });
  return ret;
}

BENCHMARK_MULTI(EliasFanoGetHitsSmallWorkingSet) {
  auto list = getEFBaseline();
  size_t ret = 0;
  folly::compression::dispatchInstructions([&](auto instructions) {
    auto reader = EliasFanoReader<EFEncoder, decltype(instructions)>(list);
    ret = benchmarkHitsSmallWorkingSet([&](uint64_t key) {
      return reader.jumpTo(key) && reader.value() == key;
    });
  });
  return ret;
}

BENCHMARK_MULTI(EliasFanoGetMixtureSerialized) {
  auto list = getEFBaseline();
  size_t ret = 0;
  folly::compression::dispatchInstructions([&](auto instructions) {
    auto reader = EliasFanoReader<EFEncoder, decltype(instructions)>(list);
    ret = benchmarkMixtureSerialized([&](uint64_t key) {
      return reader.jumpTo(key) && reader.value() == key;
    });
  });
  return ret;
}

BENCHMARK_MULTI(EliasFanoGetHitsSerialized) {
  auto list = getEFBaseline();
  size_t ret = 0;
  folly::compression::dispatchInstructions([&](auto instructions) {
    auto reader = EliasFanoReader<EFEncoder, decltype(instructions)>(list);
    ret = benchmarkHitsSerialized([&](uint64_t key) {
      reader.jumpTo(key);
      DCHECK(reader.valid());
      return reader.position();
    });
  });
  return ret;
}

BENCHMARK_MULTI(EliasFanoGetRandomSerialized) {
  auto list = getEFBaseline();
  size_t ret = 0;
  folly::compression::dispatchInstructions([&](auto instructions) {
    auto reader = EliasFanoReader<EFEncoder, decltype(instructions)>(list);
    ret = benchmarkRandomSerialized([&](uint64_t key) {
      reader.jumpTo(key);
      return reader.position();
    });
  });
  return ret;
}

#endif // FOLLY_QUOTIENT_MULTI_SET_SUPPORTED

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);

#if FOLLY_QUOTIENT_MULTI_SET_SUPPORTED
  benchmarkSetup();
  folly::runBenchmarks();
#endif // FOLLY_QUOTIENT_MULTI_SET_SUPPORTED
}

#if 0
// Intel(R) Xeon(R) CPU E5-2680 v4 @ 2.40GHz, built with Clang.
$ buck run @mode/opt folly/experimental/test:quotient_multiset_benchmark -- --benchmark --bm_max_secs 3

I0917 10:28:54.497815 2874547 QuotientMultiSetBenchmark.cpp:195] Built QuotientMultiSet, space: 124.9 MiB
============================================================================
folly/experimental/test/QuotientMultiSetBenchmark.cpprelative  time/iter  iters/s
============================================================================
QuotientMultiSetGetHits                                    114.94ns    8.70M
QuotientMultiSetGetRandom                                   72.87ns   13.72M
QuotientMultiSetGetHitsSmallWorkingSet                      58.75ns   17.02M
QuotientMultiSetGetMixtureSerialized                       138.73ns    7.21M
QuotientMultiSetGetHitsSerialized                          138.56ns    7.22M
QuotientMultiSetGetRandomSerialized                        130.11ns    7.69M
----------------------------------------------------------------------------
I0917 10:29:33.808831 2874547 QuotientMultiSetBenchmark.cpp:83] Built F14FastSet, size: 98843868, space: 1 GiB
F14MapHits                                                  34.69ns   28.83M
F14MapRandom                                                47.22ns   21.18M
F14MapHitsSmallWorkingSet                                   14.42ns   69.34M
F14MapMixtureSerialized                                     94.64ns   10.57M
F14MapHitsSerialized                                       140.74ns    7.11M
----------------------------------------------------------------------------
I0917 10:29:52.722596 2874547 QuotientMultiSetBenchmark.cpp:100] Built Elias-Fano list, space: 101.5 MiB
EliasFanoGetHits                                           258.08ns    3.87M
EliasFanoGetRandom                                         247.18ns    4.05M
EliasFanoGetHitsSmallWorkingSet                             81.54ns   12.26M
EliasFanoGetMixtureSerialized                              253.63ns    3.94M
EliasFanoGetHitsSerialized                                 161.57ns    6.19M
EliasFanoGetRandomSerialized                               161.55ns    6.19M
============================================================================
#endif
