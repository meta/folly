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

#include <array>
#include <cstdint>
#include <limits>
#include <ostream>
#include <random>
#include <span>
#include <type_traits>
#include <utility>

#include <folly/Likely.h>
#include <folly/Portability.h>
#include <folly/functional/Invoke.h>
#include <folly/random/seed_seq.h>
#include <folly/random/splitmix64.h>

#if FOLLY_X64
#include <immintrin.h>
#endif

namespace folly {

namespace detail {
#if defined(__AVX2__) && defined(__GNUC__)
using DefaultVectorType = __v4du; // GCC-specific unsigned vector type
#else
using DefaultVectorType = uint64_t; // Fallback for other compilers
#endif
} // namespace detail

using DefaultVectorType = detail::DefaultVectorType;

template <typename ResType, typename VectorType = DefaultVectorType>
class xoshiro256pp {
 private:
  static_assert(sizeof(VectorType) % sizeof(ResType) == 0);
  static_assert(sizeof(VectorType) >= sizeof(ResType));
  static_assert(alignof(VectorType) >= alignof(ResType));

  using state_array = VectorType[32];
  using state_span = decltype(std::span(std::declval<state_array&>()));

  template <typename SeedSeq>
  static constexpr bool is_seed_seq =
      std::is_invocable_v<seed_seq_generate_fn, state_span, SeedSeq&>;

 public:
  using result_type = ResType;
  static constexpr result_type default_seed =
      static_cast<result_type>(0x8690c864c6e0b716);

  static constexpr size_t state_size =
      sizeof(state_array) / sizeof(result_type);
  // Add static_asserts to enforce constraints on ResType
  static_assert(
      std::is_integral_v<result_type>, "ResType must be an integral type.");
  static_assert(
      std::is_unsigned_v<result_type>, "ResType must be an unsigned type.");

  xoshiro256pp(result_type value = default_seed) noexcept { seed(value); }

  template <typename SeedSeq>
    requires is_seed_seq<SeedSeq>
  explicit xoshiro256pp(SeedSeq& seq) noexcept {
    seed(seq);
  }

  result_type operator()() noexcept { return next(); }
  static constexpr result_type min() noexcept {
    return std::numeric_limits<result_type>::min();
  }
  static constexpr result_type max() noexcept {
    return std::numeric_limits<result_type>::max();
  }

  /// seed
  ///
  /// Deterministically fills the internal state with uniformly-distributed data
  /// generated from value.
  ///
  /// Note: the internal state is larger than result_type, so the entropy of the
  /// internal state will be smaller than the maximum possible entropy.
  void seed(result_type value = default_seed) noexcept {
    uint64_t seed = value;
    for (uint64_t re = 0; re < VecResCount; re++) {
      for (uint64_t stat = 0; stat < StateSize; stat++) {
        state[re][stat] = seed_vec<vector_type>(seed);
      }
    }
    cur = ResultCount;
  }

  /// seed
  ///
  /// Deterministically fills the internal state with uniformly-distributed data
  /// generated from seq.
  template <typename SeedSeq>
    requires is_seed_seq<SeedSeq>
  void seed(SeedSeq& seq) {
    state_array arr;
    std::memcpy(&arr, &state, sizeof(state_array));
    seed_seq_generate(std::span(arr), seq);
    std::memcpy(&state, &arr, sizeof(state_array));
    cur = ResultCount;
  }

 private:
  using vector_type = VectorType;
  static constexpr uint64_t StateSize = 4;
  static constexpr uint64_t VecResCount = 8;
  static constexpr uint64_t ResultCount =
      VecResCount * (sizeof(vector_type) / sizeof(result_type));
  union {
    vector_type vecRes[VecResCount]{};
    result_type res[ResultCount];
  };
  vector_type state[VecResCount][StateSize]{};
  static_assert(sizeof(state) == sizeof(state_array));
  uint64_t cur = ResultCount;

  template <typename Size, typename VType, typename CharT, typename Traits>
  friend std::basic_ostream<CharT, Traits>& operator<<(
      std::basic_ostream<CharT, Traits>& os,
      const xoshiro256pp<Size, VType>& rng);

  template <typename T>
  static inline T seed_vec(uint64_t& seed) {
    if constexpr (sizeof(T) != sizeof(uint64_t)) {
      T sbase{};
      for (uint64_t i = 0; i < sizeof(vector_type) / sizeof(uint64_t); i++) {
        sbase[i] = folly::to_narrow(splitmix64(seed));
      }
      return sbase;
    } else {
      return T(splitmix64(seed));
    }
  }

  FOLLY_ALWAYS_INLINE static vector_type rotl(
      const vector_type x, int k) noexcept {
    return (x << k) | (x >> (64 - k));
  }

  void calc() noexcept {
#if defined(__aarch64__)
    for (uint64_t i = 0; i < VecResCount; i=i+4) {

      auto& curState0 = state[i];
      auto& curState1 = state[i+1];
      auto& curState2 = state[i+2];
      auto& curState3 = state[i+3];

      // curState[0..3] are uint64x2_t
      // vecRes[i] = rotl(curState[0] + curState[3], 23) + curState[0];
      uint64x2_t s0_0 = curState0[0];
      uint64x2_t s0_1 = curState0[1];
      uint64x2_t s0_2 = curState0[2];
      uint64x2_t s0_3 = curState0[3];

      uint64x2_t s1_0 = curState1[0];
      uint64x2_t s1_1 = curState1[1];
      uint64x2_t s1_2 = curState1[2];
      uint64x2_t s1_3 = curState1[3];

      uint64x2_t s2_0 = curState2[0];
      uint64x2_t s2_1 = curState2[1];
      uint64x2_t s2_2 = curState2[2];
      uint64x2_t s2_3 = curState2[3];

      uint64x2_t s3_0 = curState3[0];
      uint64x2_t s3_1 = curState3[1];
      uint64x2_t s3_2 = curState3[2];
      uint64x2_t s3_3 = curState3[3];

      uint64x2_t sum0 = vaddq_u64(s0_0, s0_3);
      uint64x2_t rot0 = vorrq_u64(vshlq_n_u64(sum0, 23),vshrq_n_u64(sum0, 64 - 23));
      vecRes[i] = vaddq_u64(rot0, s0_0);

      uint64x2_t sum1 = vaddq_u64(s1_0, s1_3);
      uint64x2_t rot1 = vorrq_u64(vshlq_n_u64(sum1, 23),vshrq_n_u64(sum1, 64 - 23));
      vecRes[i+1] = vaddq_u64(rot1, s1_0);

      uint64x2_t sum2 = vaddq_u64(s2_0, s2_3);
      uint64x2_t rot2 = vorrq_u64(vshlq_n_u64(sum2, 23),vshrq_n_u64(sum2, 64 - 23));
      vecRes[i+2] = vaddq_u64(rot2, s2_0);

      uint64x2_t sum3 = vaddq_u64(s3_0, s3_3);
      uint64x2_t rot3 = vorrq_u64(vshlq_n_u64(sum3, 23),vshrq_n_u64(sum3, 64 - 23));
      vecRes[i+3] = vaddq_u64(rot3, s3_0);

      uint64x2_t t0 = vshlq_n_u64(s0_1, 17);
      uint64x2_t t1 = vshlq_n_u64(s1_1, 17);
      uint64x2_t t2 = vshlq_n_u64(s2_1, 17);
      uint64x2_t t3 = vshlq_n_u64(s3_1, 17);

      s0_2 = veorq_u64(s0_2, s0_0);
      s0_3 = veorq_u64(s0_3, s0_1);
      s0_1 = veorq_u64(s0_1, s0_2);
      s0_0 = veorq_u64(s0_0, s0_3);
      s0_2 = veorq_u64(s0_2, t0);
      s0_3 = vorrq_u64(vshlq_n_u64(s0_3, 45), vshrq_n_u64(s0_3, 19)); // rotl(s0_3, 45)

      s1_2 = veorq_u64(s1_2, s1_0);
      s1_3 = veorq_u64(s1_3, s1_1);
      s1_1 = veorq_u64(s1_1, s1_2);
      s1_0 = veorq_u64(s1_0, s1_3);
      s1_2 = veorq_u64(s1_2, t1);
      s1_3 = vorrq_u64(vshlq_n_u64(s1_3, 45), vshrq_n_u64(s1_3, 19)); // rotl(s1_3, 45)

      s2_2 = veorq_u64(s2_2, s2_0);
      s2_3 = veorq_u64(s2_3, s2_1);
      s2_1 = veorq_u64(s2_1, s2_2);
      s2_0 = veorq_u64(s2_0, s2_3);
      s2_2 = veorq_u64(s2_2, t2);
      s2_3 = vorrq_u64(vshlq_n_u64(s2_3, 45), vshrq_n_u64(s2_3, 19)); // rotl(s2_3, 45)

      s3_2 = veorq_u64(s3_2, s3_0);
      s3_3 = veorq_u64(s3_3, s3_1);
      s3_1 = veorq_u64(s3_1, s3_2);
      s3_0 = veorq_u64(s3_0, s3_3);
      s3_2 = veorq_u64(s3_2, t3);
      s3_3 = vorrq_u64(vshlq_n_u64(s3_3, 45), vshrq_n_u64(s3_3, 19)); // rotl(s3_3, 45)

      curState0[0] = s0_0;
      curState0[1] = s0_1;
      curState0[2] = s0_2;
      curState0[3] = s0_3;

      curState1[0] = s1_0;
      curState1[1] = s1_1;
      curState1[2] = s1_2;
      curState1[3] = s1_3;

      curState2[0] = s2_0;
      curState2[1] = s2_1;
      curState2[2] = s2_2;
      curState2[3] = s2_3;

      curState3[0] = s3_0;
      curState3[1] = s3_1;
      curState3[2] = s3_2;
      curState3[3] = s3_3;

    }
    cur = 0;
#else
   for (uint64_t i = 0; i < VecResCount; i++) {
      auto& curState = state[i];
      vecRes[i] = rotl(curState[0] + curState[3], 23) + curState[0];
      const auto t = curState[1] << 17;
      curState[2] ^= curState[0];
      curState[3] ^= curState[1];
      curState[1] ^= curState[2];
      curState[0] ^= curState[3];
      curState[2] ^= t;
      curState[3] = rotl(curState[3], 45);
    }
    cur = 0;
#endif
  }

  FOLLY_ALWAYS_INLINE result_type next() noexcept {
    if (FOLLY_UNLIKELY(cur == ResultCount)) {
      calc();
    }
    return res[cur++];
  }
};

template <typename Size, typename VectorType, typename CharT, typename Traits>
std::basic_ostream<CharT, Traits>& operator<<(
    std::basic_ostream<CharT, Traits>& os,
    const xoshiro256pp<Size, VectorType>& rng) {
  for (auto i2 : rng.res) {
    os << i2 << " ";
  }
  os << "cur: " << rng.cur;
  return os;
}

using xoshiro256pp_32 = xoshiro256pp<uint32_t>;
using xoshiro256pp_64 = xoshiro256pp<uint64_t>;

} // namespace folly
