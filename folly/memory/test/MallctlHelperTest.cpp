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

#include <folly/portability/GTest.h>

#if defined(FOLLY_USE_JEMALLOC) && (!defined(FOLLY_SANITIZE) || !FOLLY_SANITIZE)
#include <folly/memory/MallctlHelper.h>

#if defined(__FreeBSD__)
#include <malloc_np.h>
#else
#include <jemalloc/jemalloc.h>
#endif

using namespace folly;

#if defined(JEMALLOC_VERSION_MAJOR) && JEMALLOC_VERSION_MAJOR > 4
static constexpr char const* kDecayCmd = "arena.0.dirty_decay_ms";
#else
static constexpr char const* kDecayCmd = "arena.0.decay_time";
#endif
static constexpr char const* kNoArgCmd = "arena.0.decay";
static constexpr char const* kInvalidCmd = "invalid";

class MallctlHelperTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Reset decay_time of arena 0 to 10 seconds.
    ssize_t decayTime = 10;
    EXPECT_NO_THROW(mallctlWrite(kDecayCmd, decayTime));
  }

  static ssize_t readArena0DecayTime() {
    ssize_t decayTime = 0;
    EXPECT_NO_THROW(mallctlRead(kDecayCmd, &decayTime));
    return decayTime;
  }
};

TEST_F(MallctlHelperTest, valid_read) {
  ssize_t decayTime = 0;
  EXPECT_NO_THROW(mallctlRead(kDecayCmd, &decayTime));
  EXPECT_EQ(10, decayTime);
}

TEST_F(MallctlHelperTest, invalid_read) {
  ssize_t decayTime = 0;
  EXPECT_THROW(mallctlRead(kInvalidCmd, &decayTime), std::runtime_error);
  EXPECT_EQ(0, decayTime);
}

TEST_F(MallctlHelperTest, valid_write) {
  ssize_t decayTime = 20;
  EXPECT_NO_THROW(mallctlWrite(kDecayCmd, decayTime));
  EXPECT_EQ(20, readArena0DecayTime());
}

TEST_F(MallctlHelperTest, invalid_write) {
  ssize_t decayTime = 20;
  EXPECT_THROW(mallctlWrite(kInvalidCmd, decayTime), std::runtime_error);
  EXPECT_EQ(10, readArena0DecayTime());
}

TEST_F(MallctlHelperTest, valid_read_write) {
  ssize_t oldDecayTime = 0;
  ssize_t newDecayTime = 20;
  EXPECT_NO_THROW(mallctlReadWrite(kDecayCmd, &oldDecayTime, newDecayTime));
  EXPECT_EQ(10, oldDecayTime);
  EXPECT_EQ(20, readArena0DecayTime());
}

TEST_F(MallctlHelperTest, invalid_read_write) {
  ssize_t oldDecayTime = 0;
  ssize_t newDecayTime = 20;
  EXPECT_THROW(
      mallctlReadWrite(kInvalidCmd, &oldDecayTime, newDecayTime),
      std::runtime_error);
  EXPECT_EQ(0, oldDecayTime);
  EXPECT_EQ(10, readArena0DecayTime());
}

TEST_F(MallctlHelperTest, valid_call) {
  EXPECT_NO_THROW(mallctlCall(kNoArgCmd));
}

TEST_F(MallctlHelperTest, invalid_call) {
  EXPECT_THROW(mallctlCall(kInvalidCmd), std::runtime_error);
}

TEST_F(MallctlHelperTest, read_write_cache_init) {
  EXPECT_NO_THROW((MallctlMibReadWriteCache<ssize_t, ssize_t>(kDecayCmd)));
  EXPECT_THROW(
      (MallctlMibReadWriteCache<ssize_t, ssize_t>(kInvalidCmd)),
      std::runtime_error);
  EXPECT_NO_THROW((MallctlMibExchangeCache<ssize_t>(kDecayCmd)));
  EXPECT_THROW(
      (MallctlMibExchangeCache<ssize_t>(kInvalidCmd)), std::runtime_error);
}

TEST_F(MallctlHelperTest, read_cache_init) {
  EXPECT_NO_THROW((MallctlMibReadCache<ssize_t>(kDecayCmd)));
  EXPECT_THROW((MallctlMibReadCache<ssize_t>(kInvalidCmd)), std::runtime_error);
}

TEST_F(MallctlHelperTest, write_cache_init) {
  EXPECT_NO_THROW((MallctlMibWriteCache<ssize_t>(kDecayCmd)));
  EXPECT_THROW(
      (MallctlMibWriteCache<ssize_t>(kInvalidCmd)), std::runtime_error);
}

TEST_F(MallctlHelperTest, call_cache_init) {
  EXPECT_NO_THROW((MallctlMibCallCache(kNoArgCmd)));
  EXPECT_THROW((MallctlMibCallCache(kInvalidCmd)), std::runtime_error);
}

TEST_F(MallctlHelperTest, valid_read_via_cache) {
  MallctlMibReadCache<ssize_t> read(kDecayCmd);
  ssize_t decayTime = 0;
  EXPECT_NO_THROW(decayTime = read());
  EXPECT_EQ(10, decayTime);
}

TEST_F(MallctlHelperTest, valid_write_via_cache) {
  MallctlMibWriteCache<ssize_t> write(kDecayCmd);
  ssize_t decayTime = 20;
  EXPECT_NO_THROW(write(decayTime));
  EXPECT_EQ(20, readArena0DecayTime());
}

TEST_F(MallctlHelperTest, valid_read_write_via_cache) {
  MallctlMibReadWriteCache<ssize_t, ssize_t> read_write(kDecayCmd);
  ssize_t oldDecayTime = 0;
  ssize_t newDecayTime = 20;
  EXPECT_NO_THROW(oldDecayTime = read_write(newDecayTime));
  EXPECT_EQ(10, oldDecayTime);
  EXPECT_EQ(20, readArena0DecayTime());

  MallctlMibExchangeCache<ssize_t> exchange(kDecayCmd);
  newDecayTime = 30;
  EXPECT_NO_THROW(oldDecayTime = exchange(newDecayTime));
  EXPECT_EQ(20, oldDecayTime);
  EXPECT_EQ(30, readArena0DecayTime());
}

TEST_F(MallctlHelperTest, valid_call_via_cache) {
  MallctlMibCallCache call(kNoArgCmd);
  EXPECT_NO_THROW(call());
}

#endif
