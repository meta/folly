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

#include <string>
#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <uuid/uuid.h>

#include <folly/codec/Uuid.h>

static std::string generateRandomGuid() {
  char guid[40];
  uuid_t uuid_raw;
  uuid_generate(uuid_raw);
  uuid_unparse_lower(uuid_raw, guid);

  return guid;
}

constexpr size_t kGuidPoolSize = 1024;

const std::vector<std::string> kGuidPool = [] {
  std::vector<std::string> p(kGuidPoolSize);
  for (auto& s : p) {
    s = generateRandomGuid();
  }
  return p;
}();

template <auto folly_uuid_parse_func>
inline void folly_uuid_parse_benchmark(size_t n) {
  std::string out;
  for (size_t i = 0; i < n; ++i) {
    folly::compiler_must_not_elide(
        folly_uuid_parse_func(out, kGuidPool[i % kGuidPoolSize]));
    folly::compiler_must_not_elide(out);
  }
}

#if (FOLLY_X64 && defined(__AVX2__))
BENCHMARK(uuid_parse_folly_avx2, n) {
  folly_uuid_parse_benchmark<folly::detail::uuid_parse_avx2>(n);
}
#endif

#if (FOLLY_X64 && defined(__SSSE3__))
BENCHMARK(uuid_parse_folly_ssse3, n) {
  folly_uuid_parse_benchmark<folly::detail::uuid_parse_ssse3>(n);
}
#endif

BENCHMARK(uuid_parse_folly_scalar, n) {
  folly_uuid_parse_benchmark<folly::detail::uuid_parse_scalar>(n);
}

BENCHMARK(uuid_parse_folly, n) {
  using StrParseFunc = folly::UuidParseCode (*)(std::string&, std::string_view);
  folly_uuid_parse_benchmark<static_cast<StrParseFunc>(folly::uuid_parse)>(n);
}

BENCHMARK(uuid_parse_glibc, n) {
  uuid_t uuid;
  for (size_t i = 0; i < n; ++i) {
    folly::compiler_must_not_elide(
        uuid_parse(kGuidPool[i % kGuidPoolSize].c_str(), uuid));
    folly::compiler_must_not_elide(uuid);
  }
}

BENCHMARK(uuid_parse_boost, n) {
  boost::uuids::string_generator gen;
  boost::uuids::uuid uuid;
  for (size_t i = 0; i < n; ++i) {
    folly::compiler_must_not_elide(uuid = gen(kGuidPool[i % kGuidPoolSize]));
    folly::compiler_must_not_elide(uuid);
  }
}

// ---------------------------------------------------------------------------
// unparse (16 bytes -> 36 char text)
// ---------------------------------------------------------------------------

const std::vector<std::array<uint8_t, 16>> kUuidPool = [] {
  std::vector<std::array<uint8_t, 16>> p(kGuidPoolSize);
  for (auto& u : p) {
    uuid_generate(u.data());
  }
  return p;
}();

template <typename UnparseFunc>
inline void uuid_unparse_benchmark(size_t n, UnparseFunc unparse) {
  for (size_t i = 0; i < n; ++i) {
    unparse(kUuidPool[i % kGuidPoolSize].data());
  }
}

// Group 1: conversion only, into a caller-owned buffer.
BENCHMARK(uuid_unparse_buf_folly, n) {
  char out[37];
  uuid_unparse_benchmark(n, [&](const uint8_t* in) {
    folly::uuid_unparse_upper(out, in);
    folly::compiler_must_not_elide(out);
  });
}

BENCHMARK_RELATIVE(uuid_unparse_buf_libuuid, n) {
  char out[37];
  uuid_unparse_benchmark(n, [&](const uint8_t* in) {
    uuid_unparse_upper(in, out);
    folly::compiler_must_not_elide(out);
  });
}

BENCHMARK_RELATIVE(uuid_unparse_buf_boost, n) {
  char out[37];
  uuid_unparse_benchmark(n, [&](const uint8_t* in) {
    boost::uuids::uuid u{};
    std::memcpy(u.data, in, 16);
    boost::uuids::to_chars(u, out);
    folly::compiler_must_not_elide(out);
  });
}

// Group 2: producing a std::string.
BENCHMARK_DRAW_LINE();

BENCHMARK(uuid_unparse_str_folly, n) {
  std::string out;
  uuid_unparse_benchmark(n, [&](const uint8_t* in) {
    folly::uuid_unparse_upper(out, in);
    folly::compiler_must_not_elide(out);
  });
}

BENCHMARK_RELATIVE(uuid_unparse_str_libuuid, n) {
  std::string out;
  uuid_unparse_benchmark(n, [&](const uint8_t* in) {
    out.resize(36);
    uuid_unparse_upper(in, &out[0]);
    folly::compiler_must_not_elide(out);
  });
}

BENCHMARK_RELATIVE(uuid_unparse_str_boost, n) {
  uuid_unparse_benchmark(n, [](const uint8_t* in) {
    boost::uuids::uuid u{};
    std::memcpy(u.data, in, 16);
    folly::compiler_must_not_elide(boost::uuids::to_string(u));
  });
}

int main(int argc, char** argv) {
  folly::gflags::ParseCommandLineFlags(&argc, &argv, true);
  folly::runBenchmarks();
  return 0;
}
