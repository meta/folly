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

#ifndef FOLLY_RANDOM_H_
#error This file may only be included from folly/Random.h
#endif

namespace folly {

namespace detail {

struct SeedSeqSecureRandom {
  using result_type = uint32_t;
  template <typename Word>
  void generate(Word* b, Word* e) {
    static_assert(is_non_bool_integral_v<Word>);
    static_assert(sizeof(Word) >= sizeof(result_type));
    Random::secureRandom(b, (e - b) * sizeof(Word));
  }
};

} // namespace detail

template <class RNG>
  requires detail::RandomValidRNG<RNG>
void Random::seed(RNG& rng) {
  detail::SeedSeqSecureRandom seq;
  rng.seed(seq);
}

template <class RNG>
  requires detail::RandomValidRNG<RNG>
auto Random::create() -> RNG {
  detail::SeedSeqSecureRandom seq;
  return RNG(seq);
}

} // namespace folly
