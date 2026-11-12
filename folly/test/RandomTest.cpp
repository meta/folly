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

#include <folly/Random.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <thread>
#include <unordered_set>
#include <vector>

#include <glog/logging.h>

#include <folly/portability/GTest.h>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#if FOLLY_HAVE_EXTRANDOM_SFMT19937
#include <ext/random>
#endif

using namespace folly;

TEST(Random, Simple) {
  uint32_t prev = 0, seed = 0;
  for (int i = 0; i < 1024; ++i) {
    EXPECT_NE(seed = randomNumberSeed(), prev);
    prev = seed;
  }
}

TEST(Random, FixedSeed) {
  // clang-format off
  struct ConstantRNG {
    using result_type = uint32_t;
    result_type operator()() {
      return 4; // chosen by fair dice roll.
                // guaranteed to be random.
    }
    static constexpr result_type min() {
      return std::numeric_limits<result_type>::min();
    }
    static constexpr result_type max() {
      return std::numeric_limits<result_type>::max();
    }
  };
  // clang-format on

  ConstantRNG gen;

  // Pick a constant random number...
  auto value = Random::rand32(10, gen);

  // Loop to make sure it really is constant.
  for (int i = 0; i < 1024; ++i) {
    auto result = Random::rand32(10, gen);
    EXPECT_EQ(value, result);
  }
}

TEST(Random, MultiThreaded) {
  const int n = 100;
  std::vector<uint32_t> seeds(n);
  std::vector<std::thread> threads;
  for (int i = 0; i < n; ++i) {
    threads.emplace_back([i, &seeds] { seeds[i] = randomNumberSeed(); });
  }
  for (auto& t : threads) {
    t.join();
  }
  std::sort(seeds.begin(), seeds.end());
  for (int i = 0; i < n - 1; ++i) {
    EXPECT_LT(seeds[i], seeds[i + 1]);
  }
}

TEST(Random, sanity) {
  // edge cases
  EXPECT_EQ(folly::Random::rand32(0), 0);
  EXPECT_EQ(folly::Random::rand32(12, 12), 12);
  EXPECT_EQ(folly::Random::rand64(0), 0);
  EXPECT_EQ(folly::Random::rand64(12, 12), 12);

  // 32-bit repeatability, uniqueness
  constexpr int kTestSize = 1000;
  {
    std::vector<uint32_t> vals;
    folly::Random::DefaultGenerator rng;
    rng.seed(0xdeadbeef);
    for (int i = 0; i < kTestSize; ++i) {
      vals.push_back(folly::Random::rand32(rng));
    }
    rng.seed(0xdeadbeef);
    for (int i = 0; i < kTestSize; ++i) {
      EXPECT_EQ(vals[i], folly::Random::rand32(rng));
    }
    EXPECT_EQ(
        vals.size(),
        std::unordered_set<uint32_t>(vals.begin(), vals.end()).size());
  }

  // 64-bit repeatability, uniqueness
  {
    std::vector<uint64_t> vals;
    folly::Random::DefaultGenerator rng;
    rng.seed(0xdeadbeef);
    for (int i = 0; i < kTestSize; ++i) {
      vals.push_back(folly::Random::rand64(rng));
    }
    rng.seed(0xdeadbeef);
    for (int i = 0; i < kTestSize; ++i) {
      EXPECT_EQ(vals[i], folly::Random::rand64(rng));
    }
    EXPECT_EQ(
        vals.size(),
        std::unordered_set<uint64_t>(vals.begin(), vals.end()).size());
  }

  // Support for common generators.
  folly::Random::rand32(std::mt19937{});
  folly::Random::rand32(std::mt19937_64{});
  folly::Random::rand32(std::minstd_rand{});
#if FOLLY_HAVE_EXTRANDOM_SFMT19937
  folly::Random::rand32(__gnu_cxx::sfmt19937{});
  folly::Random::rand32(__gnu_cxx::sfmt19937_64{});
#endif

  folly::Random::rand64(std::mt19937{});
  folly::Random::rand64(std::mt19937_64{});
  folly::Random::rand64(std::minstd_rand{});
#if FOLLY_HAVE_EXTRANDOM_SFMT19937
  folly::Random::rand64(__gnu_cxx::sfmt19937{});
  folly::Random::rand64(__gnu_cxx::sfmt19937_64{});
#endif
}

TEST(Random, oneIn) {
  for (auto i = 0; i < 10; ++i) {
    EXPECT_FALSE(folly::Random::oneIn(0));
    EXPECT_FALSE(folly::Random::secureOneIn(0));
    EXPECT_TRUE(folly::Random::oneIn(1));
    EXPECT_TRUE(folly::Random::secureOneIn(1));
  }

  // When using higher sampling rates, we'll just ensure that we see both
  // outcomes. We won't worry about statistical validity since we defer that to
  // folly::Random.
  auto constexpr kSeenTrue{1};
  auto constexpr kSeenFalse{2};
  auto constexpr kSeenBoth{kSeenTrue | kSeenFalse};

  auto seenSoFar{0};
  for (auto i = 0; i < 1000 && seenSoFar != kSeenBoth; ++i) {
    seenSoFar |= (folly::Random::oneIn(10) ? kSeenTrue : kSeenFalse);
  }

  EXPECT_EQ(kSeenBoth, seenSoFar);
}

TEST(Random, oneIn64) {
  for (auto i = 0; i < 10; ++i) {
    EXPECT_FALSE(folly::Random::oneIn64(0));
    EXPECT_FALSE(folly::Random::secureOneIn64(0));
    EXPECT_TRUE(folly::Random::oneIn64(1));
    EXPECT_TRUE(folly::Random::secureOneIn64(1));
  }

  // When using higher sampling rates, we'll just ensure that we see both
  // outcomes. We won't worry about statistical validity since we defer that to
  // folly::Random.
  auto constexpr kSeenTrue{1};
  auto constexpr kSeenFalse{2};
  auto constexpr kSeenBoth{kSeenTrue | kSeenFalse};

  auto seenSoFar{0};
  for (auto i = 0; i < 1000 && seenSoFar != kSeenBoth; ++i) {
    seenSoFar |= (folly::Random::oneIn64(10) ? kSeenTrue : kSeenFalse);
  }

  EXPECT_EQ(kSeenBoth, seenSoFar);

  // For a 64 bit space this is effectively always false. Using 2^63 + 1 was
  // suggested to guard against a future possible bug that truncates down to
  // a 32 bit value. If this were to happen then this test might evaluate
  // as oneIn(1), which always returns true, which would fail.
  //
  //   >>> (2 ** 64 - 1) & 0xFFFFFFFF
  //   4294967295
  //   >>> (2 ** 63 + 1) & 0xFFFFFFFF
  //   1
  uint64_t kAlmostMax = (uint64_t(1) << 63) + 1;
  EXPECT_FALSE(folly::Random::oneIn64(kAlmostMax));
  EXPECT_TRUE(folly::Random::oneIn(kAlmostMax));
}

TEST(Random, randBool) {
  for (auto i = 0; i < 10; ++i) {
    EXPECT_FALSE(folly::Random::randBool(0));
    EXPECT_TRUE(folly::Random::randBool(1));
  }

  // When using higher sampling rates, we'll just ensure that we see both
  // outcomes. We won't worry about statistical validity since we defer that to
  // folly::Random.
  auto constexpr kSeenTrue{1};
  auto constexpr kSeenFalse{2};
  auto constexpr kSeenBoth{kSeenTrue | kSeenFalse};

  auto seenSoFar{0};
  for (auto i = 0; i < 1000 && seenSoFar != kSeenBoth; ++i) {
    seenSoFar |= (folly::Random::randBool(0.5) ? kSeenTrue : kSeenFalse);
  }

  EXPECT_EQ(kSeenBoth, seenSoFar);
}

TEST(Random, randDouble01) {
  // Very basic test that we see at least one number < 0.1 and one > 0.9, to
  // verify that the output is not constant and the mantissa is not misaligned.
  auto constexpr kSeenHigh{1};
  auto constexpr kSeenLow{2};
  auto constexpr kSeenBoth{kSeenHigh | kSeenLow};

  auto seenSoFar{0};
  for (auto i = 0; i < 1000; ++i) {
    auto value = folly::Random::randDouble01();
    ASSERT_GE(value, 0);
    ASSERT_LT(value, 1);
    if (value > 0.9) {
      seenSoFar |= kSeenHigh;
    } else if (value < 0.1) {
      seenSoFar |= kSeenLow;
    }
  }

  EXPECT_EQ(kSeenBoth, seenSoFar);
}

TEST(Random, randDouble) {
  // Very basic test that we see at least one number in the bottom tenth of the
  // range and one in the top tenth, to verify that the output spans the
  // requested range and is not constant.
  auto constexpr kSeenHigh{1};
  auto constexpr kSeenLow{2};
  auto constexpr kSeenBoth{kSeenHigh | kSeenLow};

  auto constexpr kMin{-5.0};
  auto constexpr kMax{17.0};

  auto seenSoFar{0};
  for (auto i = 0; i < 1000; ++i) {
    auto value = folly::Random::randDouble(kMin, kMax);
    ASSERT_GE(value, kMin);
    ASSERT_LT(value, kMax);
    if (value > kMax - 0.1 * (kMax - kMin)) {
      seenSoFar |= kSeenHigh;
    } else if (value < kMin + 0.1 * (kMax - kMin)) {
      seenSoFar |= kSeenLow;
    }
  }

  EXPECT_EQ(kSeenBoth, seenSoFar);
}

TEST(Random, randDoubleReversedRange) {
  std::mt19937 rng(123);
  auto copy = rng;
  bool seenLow = false;
  bool seenHigh = false;
  for (int i = 0; i < 100; ++i) {
    auto value = Random::randDouble(10, 0, rng);
    EXPECT_EQ(value, std::lerp(10.0, 0.0, Random::randDouble01(copy)));
    ASSERT_GT(value, 0);
    ASSERT_LE(value, 10);
    seenLow |= value < 1;
    seenHigh |= value > 9;
  }
  EXPECT_EQ(rng, copy);
  EXPECT_TRUE(seenLow);
  EXPECT_TRUE(seenHigh);

  rng.seed(123);
  copy = rng;
  double max = std::numeric_limits<double>::max();
  auto value = Random::randDouble(max, -max, rng);
  EXPECT_EQ(value, std::lerp(max, -max, Random::randDouble01(copy)));
  EXPECT_TRUE(std::isfinite(value));
  EXPECT_GT(value, -max);
  EXPECT_LE(value, max);

  auto adjacent = std::nextafter(1.0, 2.0);
  EXPECT_EQ(Random::randDouble(adjacent, 1, rng), adjacent);
}

TEST(Random, randDoubleEmptyRange) {
  EXPECT_EQ(3.5, folly::Random::randDouble(3.5, 3.5));
  EXPECT_EQ(0.0, folly::Random::randDouble(0.0, 0.0));
}

TEST(Random, randDoubleOverflowingSpan) {
  std::mt19937 rng(123);
  auto copy = rng;
  auto max = std::numeric_limits<double>::max();
  for (int i = 0; i < 100; ++i) {
    auto value = Random::randDouble(-max, max, rng);
    EXPECT_EQ(value, std::lerp(-max, max, Random::randDouble01(copy)));
    EXPECT_TRUE(std::isfinite(value));
    EXPECT_GE(value, -max);
    EXPECT_LT(value, max);
  }
  EXPECT_EQ(rng, copy);
  EXPECT_TRUE(std::isfinite(Random::randDouble(-max, max, std::mt19937{})));
}

TEST(Random, randDoubleSharesCanonicalGenerator) {
  std::mt19937 rng(123);
  auto copy = rng;
  for (int i = 0; i < 100; ++i) {
    auto expected = (Random::rand64(copy) >> 11) * 0x1p-53;
    EXPECT_EQ(Random::randDouble01(rng), expected);
  }
  EXPECT_EQ(rng, copy);

  copy = rng;
  uniform_real_distribution<double> dist(-5, 17);
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(Random::randDouble(-5, 17, rng), dist(copy));
  }
  EXPECT_EQ(rng, copy);
  Random::randDouble01(std::mt19937{});
  Random::randDouble(-5, 17, std::mt19937{});
}

#ifndef _WIN32
TEST(Random, SecureFork) {
  // Random buffer size is 128, must be less than that.
  int retries = 100;
  while (true) {
    unsigned char buffer = 0;
    // Init random buffer
    folly::Random::secureRandom(&buffer, 1);

    auto pid = fork();
    EXPECT_NE(pid, -1);
    if (pid) {
      // parent
      int status = 0;
      folly::Random::secureRandom(&buffer, 1);
      auto pid2 = wait(&status);
      EXPECT_EQ(pid, pid2);
      // Since this *is* random data, we could randomly end up with
      // the same byte.  Try again a few times if so before assuming
      // real failure.
      if (WEXITSTATUS(status) == buffer && retries-- > 0) {
        continue;
      }
      EXPECT_NE(WEXITSTATUS(status), buffer);
      return;
    } else {
      // child
      folly::Random::secureRandom(&buffer, 1);
      exit(buffer); // Do not print gtest results
    }
  }
}
#endif
