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

#include <folly/random/xoshiro256pp.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <type_traits>
#include <vector>

#include <folly/portability/GTest.h>

namespace folly {
namespace {

// Test that the xoshiro256pp class satisfies the UniformRandomBitGenerator
// concept
TEST(Xoshiro256ppTest, SatisfiesUniformRandomBitGeneratorConcept) {
  // Check for 32-bit version
  static_assert(
      std::is_same<xoshiro256pp_32::result_type, uint32_t>::value,
      "xoshiro256pp_32 result_type should be uint32_t");

  // Check for 64-bit version
  static_assert(
      std::is_same<xoshiro256pp_64::result_type, uint64_t>::value,
      "xoshiro256pp_64 result_type should be uint64_t");

  // Verify min and max values
  EXPECT_EQ(xoshiro256pp_32::min(), std::numeric_limits<uint32_t>::min());
  EXPECT_EQ(xoshiro256pp_32::max(), std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(xoshiro256pp_64::min(), std::numeric_limits<uint64_t>::min());
  EXPECT_EQ(xoshiro256pp_64::max(), std::numeric_limits<uint64_t>::max());
}

// Test that the generator produces deterministic sequences with the same seed
TEST(Xoshiro256ppTest, DeterministicWithSameSeed) {
  constexpr uint64_t kTestSeed = 0x1234567890ABCDEF;

  // Test 32-bit version
  {
    xoshiro256pp_32 rng1(folly::to_narrow(kTestSeed));
    xoshiro256pp_32 rng2(folly::to_narrow(kTestSeed));

    // Generate and compare a sequence of values
    for (int i = 0; i < 1000; ++i) {
      EXPECT_EQ(rng1(), rng2()) << "Generators diverged at iteration " << i;
    }
  }

  // Test 64-bit version
  {
    xoshiro256pp_64 rng1(kTestSeed);
    xoshiro256pp_64 rng2(kTestSeed);

    // Generate and compare a sequence of values
    for (int i = 0; i < 1000; ++i) {
      EXPECT_EQ(rng1(), rng2()) << "Generators diverged at iteration " << i;
    }
  }
}

// Test that different seeds produce different sequences
TEST(Xoshiro256ppTest, DifferentSeedsProduceDifferentSequences) {
  constexpr uint64_t kTestSeed1 = 0x1234567890ABCDEF;
  constexpr uint64_t kTestSeed2 = 0xFEDCBA0987654321;

  // Test 32-bit version
  {
    xoshiro256pp_32 rng1(folly::to_narrow(kTestSeed1));
    xoshiro256pp_32 rng2(folly::to_narrow(kTestSeed2));

    // Check if sequences are different
    bool foundDifference = false;
    for (int i = 0; i < 100; ++i) {
      if (rng1() != rng2()) {
        foundDifference = true;
        break;
      }
    }
    EXPECT_TRUE(foundDifference)
        << "Different seeds should produce different sequences";
  }

  // Test 64-bit version
  {
    xoshiro256pp_64 rng1(kTestSeed1);
    xoshiro256pp_64 rng2(kTestSeed2);

    // Check if sequences are different
    bool foundDifference = false;
    for (int i = 0; i < 100; ++i) {
      if (rng1() != rng2()) {
        foundDifference = true;
        break;
      }
    }
    EXPECT_TRUE(foundDifference)
        << "Different seeds should produce different sequences";
  }
}

// Test reseeding functionality
TEST(Xoshiro256ppTest, ReseedingChangesSequence) {
  constexpr uint64_t kInitialSeed = 0x1234567890ABCDEF;
  constexpr uint64_t kNewSeed = 0xFEDCBA0987654321;

  // Test 32-bit version
  {
    xoshiro256pp_32 rng(folly::to_narrow(kInitialSeed));

    // Generate some initial values
    std::vector<uint32_t> initialSequence;
    initialSequence.reserve(10);
    for (int i = 0; i < 10; ++i) {
      initialSequence.push_back(rng());
    }

    // Reseed and generate a new sequence
    rng.seed(folly::to_narrow(kNewSeed));
    std::vector<uint32_t> newSequence;
    initialSequence.reserve(10);
    newSequence.reserve(10);
    for (int i = 0; i < 10; ++i) {
      newSequence.push_back(rng());
    }

    // Compare sequences
    EXPECT_NE(initialSequence, newSequence)
        << "Reseeding should change the sequence";

    // Verify that reseeding with the initial seed reproduces the initial
    // sequence
    rng.seed(folly::to_narrow(kInitialSeed));
    for (int i = 0; i < 10; ++i) {
      EXPECT_EQ(rng(), initialSequence[i])
          << "Reseeding with initial seed should reproduce initial sequence";
    }
  }

  // Test 64-bit version
  {
    xoshiro256pp_64 rng(kInitialSeed);

    // Generate some initial values
    std::vector<uint64_t> initialSequence;
    initialSequence.reserve(10);
    for (int i = 0; i < 10; ++i) {
      initialSequence.push_back(rng());
    }

    // Reseed and generate a new sequence
    rng.seed(kNewSeed);
    std::vector<uint64_t> newSequence;
    newSequence.reserve(10);
    for (int i = 0; i < 10; ++i) {
      newSequence.push_back(rng());
    }

    // Compare sequences
    EXPECT_NE(initialSequence, newSequence)
        << "Reseeding should change the sequence";

    // Verify that reseeding with the initial seed reproduces the initial
    // sequence
    rng.seed(kInitialSeed);
    for (int i = 0; i < 10; ++i) {
      EXPECT_EQ(rng(), initialSequence[i])
          << "Reseeding with initial seed should reproduce initial sequence";
    }
  }
}

// Test seeding with std::seed_seq
TEST(Xoshiro256ppTest, SeedSeqInitialization) {
  // Create a seed sequence
  std::array<uint32_t, 10> seedData = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  std::seed_seq seedSeq(seedData.begin(), seedData.end());

  // Initialize generators with the seed sequence
  xoshiro256pp_32 rng32(seedSeq);
  xoshiro256pp_64 rng64(seedSeq);

  // Just verify that we can generate values without crashing
  // (The actual values are implementation-defined)
  EXPECT_NO_THROW({
    for (int i = 0; i < 100; ++i) {
      rng32();
      rng64();
    }
  });
}

// Test distribution of values
TEST(Xoshiro256ppTest, ValueDistribution) {
  constexpr uint64_t kTestSeed = 0x1234567890ABCDEF;
  constexpr int kNumSamples = 100000;
  constexpr int kNumBins = 10;

  // Test 32-bit version
  {
    xoshiro256pp_32 rng(folly::to_narrow(kTestSeed));
    std::array<int, kNumBins> bins = {};

    // Generate samples and count occurrences in each bin
    for (int i = 0; i < kNumSamples; ++i) {
      uint32_t value = rng();
      int binIndex = static_cast<int>(
          (static_cast<double>(value) / std::numeric_limits<uint32_t>::max()) *
          kNumBins);
      if (binIndex == kNumBins) {
        binIndex = kNumBins - 1; // Handle the case where value == max
      }
      bins[binIndex]++;
    }

    // Check that each bin has a reasonable number of samples
    // (roughly kNumSamples / kNumBins, with some tolerance)
    int expectedPerBin = kNumSamples / kNumBins;
    double tolerance = 0.2; // Allow 20% deviation

    for (int i = 0; i < kNumBins; ++i) {
      EXPECT_GT(bins[i], expectedPerBin * (1.0 - tolerance))
          << "Bin " << i << " has too few samples";
      EXPECT_LT(bins[i], expectedPerBin * (1.0 + tolerance))
          << "Bin " << i << " has too many samples";
    }
  }

  // Test 64-bit version
  {
    xoshiro256pp_64 rng(kTestSeed);
    std::array<int, kNumBins> bins = {};

    // Generate samples and count occurrences in each bin
    for (int i = 0; i < kNumSamples; ++i) {
      uint64_t value = rng();
      // Use modulo to avoid precision issues with double conversion
      int binIndex = static_cast<int>(value % kNumBins);
      bins[binIndex]++;
    }

    // Check that each bin has a reasonable number of samples
    int expectedPerBin = kNumSamples / kNumBins;
    double tolerance = 0.2; // Allow 20% deviation

    for (int i = 0; i < kNumBins; ++i) {
      EXPECT_GT(bins[i], expectedPerBin * (1.0 - tolerance))
          << "Bin " << i << " has too few samples";
      EXPECT_LT(bins[i], expectedPerBin * (1.0 + tolerance))
          << "Bin " << i << " has too many samples";
    }
  }
}

// Test compatibility with standard library algorithms and distributions
TEST(Xoshiro256ppTest, StdLibCompatibility) {
  constexpr uint64_t kTestSeed = 0x1234567890ABCDEF;

  // Test with std::uniform_int_distribution
  {
    xoshiro256pp_32 rng(folly::to_narrow(kTestSeed));
    std::uniform_int_distribution<int> dist(1, 100);

    // Generate some values and check they're in range
    for (int i = 0; i < 1000; ++i) {
      int value = dist(rng);
      EXPECT_GE(value, 1);
      EXPECT_LE(value, 100);
    }
  }

  // Test with std::shuffle
  {
    xoshiro256pp_64 rng(kTestSeed);
    std::vector<int> values(100);
    std::iota(values.begin(), values.end(), 1); // Fill with 1..100

    // Shuffle the vector
    std::shuffle(values.begin(), values.end(), rng);

    // Check that all values are still present
    std::set<int> uniqueValues(values.begin(), values.end());
    EXPECT_EQ(uniqueValues.size(), 100);
    EXPECT_EQ(*uniqueValues.begin(), 1);
    EXPECT_EQ(*uniqueValues.rbegin(), 100);
  }
}

// Helper function for random resizing of vectors using xoshiro256pp as RNG
template <typename T, typename RNG>
void random_resize(std::vector<T>& collection, size_t max, RNG&& rng) {
  if (max == 0) {
    collection.clear();
    return;
  }

  if (max >= collection.size() || max == static_cast<size_t>(-1)) {
    return; // No resizing needed
  }

  while (collection.size() > max) {
    // Use the RNG to generate a random index
    uint64_t value = rng();
    uint64_t index = value % collection.size();

    // Swap with the last element and pop
    std::swap(collection[index], collection.back());
    collection.pop_back();
  }
}

TEST(Xoshiro256ppTest, StreamOperator) {
  constexpr uint64_t kTestSeed = 0x1234567890ABCDEF;

  // Test 32-bit version
  {
    xoshiro256pp_32 rng(folly::to_narrow(kTestSeed));
    std::stringstream ss;
    ss << rng;

    // Verify that the output contains the expected format
    std::string output = ss.str();
    EXPECT_FALSE(output.empty());
    EXPECT_NE(output.find("cur:"), std::string::npos)
        << "Output should contain 'cur:' marker";

    // Create a second identical RNG and verify they produce the same string
    // representation
    xoshiro256pp_32 rng2(folly::to_narrow(kTestSeed));
    std::stringstream ss2;
    ss2 << rng2;
    EXPECT_EQ(ss.str(), ss2.str())
        << "Same RNG state should produce the same string representation";

    // Generate some values to change the internal state
    for (int i = 0; i < 10; ++i) {
      rng();
    }

    // Verify that the string representation changes after generating values
    std::stringstream ss3;
    ss3 << rng;
    EXPECT_NE(ss.str(), ss3.str())
        << "RNG state should change after generating values";
  }

  // Test 64-bit version
  {
    xoshiro256pp_64 rng(kTestSeed);
    std::stringstream ss;
    ss << rng;

    // Verify that the output contains the expected format
    std::string output = ss.str();
    EXPECT_FALSE(output.empty());
    EXPECT_NE(output.find("cur:"), std::string::npos)
        << "Output should contain 'cur:' marker";

    // Create a second identical RNG and verify they produce the same string
    // representation
    xoshiro256pp_64 rng2(kTestSeed);
    std::stringstream ss2;
    ss2 << rng2;
    EXPECT_EQ(ss.str(), ss2.str())
        << "Same RNG state should produce the same string representation";

    // Generate some values to change the internal state
    for (int i = 0; i < 10; ++i) {
      rng();
    }

    // Verify that the string representation changes after generating values
    std::stringstream ss3;
    ss3 << rng;
    EXPECT_NE(ss.str(), ss3.str())
        << "RNG state should change after generating values";
  }
}

} // namespace
} // namespace folly
