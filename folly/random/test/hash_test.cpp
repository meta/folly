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

#include <folly/random/hash.h>

#include <functional>
#include <numeric>

#include <folly/portability/GTest.h>

struct HashCounterEngineTest : testing::Test {};

TEST_F(HashCounterEngineTest, identity) {
  using engine = folly::hash_counter_engine<std::identity, uint64_t>;
  static_assert(std::is_trivially_destructible_v<engine>);

  engine rng{345};

  EXPECT_EQ(345, rng());
  EXPECT_EQ(346, rng());
  EXPECT_EQ(347, rng());
}

TEST_F(HashCounterEngineTest, affine) {
  struct affine_fn {
    uint16_t operator()(uint16_t val) { return val * 3 + 2; }
  };
  using engine = folly::hash_counter_engine<affine_fn, uint16_t>;
  static_assert(std::is_trivially_destructible_v<engine>);

  engine rng{12};

  EXPECT_EQ(38, rng());
  EXPECT_EQ(41, rng());
  EXPECT_EQ(44, rng());
}

TEST_F(HashCounterEngineTest, byte_range) {
  struct affine_fn {
    uint8_t operator()(uint8_t const* addr, size_t size) {
      return std::accumulate(addr, addr + size, uint8_t(0));
    }
  };
  using engine = folly::hash_counter_engine<affine_fn, uint32_t>;
  static_assert(std::is_trivially_destructible_v<engine>);

  engine rng{0x01020304u};

  EXPECT_EQ(10, rng());
  EXPECT_EQ(11, rng());
  EXPECT_EQ(12, rng());
}
