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

#include <array>
#include <cstdint>
#include <utility>

#include <folly/io/async/IoUringBackend.h>
#include <folly/io/async/IoUringOptions.h>
#include <folly/portability/GTest.h>
#include <folly/test/TestUtils.h>

namespace {

class RingStatsBackend : public folly::IoUringBackend {
 public:
  using IoUringBackend::IoUringBackend;

  void queueCancel() { cancel(&cancelTarget_); }

 private:
  struct CancelTarget : folly::IoSqeBase {
    void processSubmit(io_uring_sqe*) noexcept override {}
    void callback(const io_uring_cqe*) noexcept override {}
    void callbackCancelled(const io_uring_cqe*) noexcept override {}
  } cancelTarget_;
};

class IoUringRingStatsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    SKIP_IF(!folly::IoUringBackend::isAvailable()) << "Backend not available";
  }

  static folly::IoUringOptions options(uint32_t capacity = 32) {
    folly::IoUringOptions opts;
    opts.setCapacity(capacity)
        .setSqeSize(8)
        .setMaxSubmit(8)
        .setMaxGet(2)
        .setUseRegisteredFds(0);
    return opts;
  }
};

TEST_F(IoUringRingStatsTest, GetAndResetStatsReturnsAndResetsRecordedValues) {
  RingStatsBackend backend(options());
  backend.submitOutstanding();
  backend.getAndResetRingStats();

  for (int i = 0; i < 3; ++i) {
    backend.queueCancel();
  }
  const auto first = backend.getAndResetRingStats();
  EXPECT_EQ(first.sqPeak, 0);

  backend.queueCancel();
  backend.queueCancel();
  backend.submitOutstanding();
  backend.processCompleted();
  const auto second = backend.getAndResetStats().ring;
  EXPECT_EQ(second.sqPeak, 5);
  EXPECT_EQ(second.cqPeak, 5);

  const auto reset = backend.getAndResetRingStats();
  EXPECT_EQ(reset.sqPeak, 0);
  EXPECT_EQ(reset.cqPeak, 0);
}

TEST_F(IoUringRingStatsTest, CountsInitialSqFullAndForcedSubmission) {
  RingStatsBackend backend(options());
  backend.submitOutstanding();
  backend.getAndResetRingStats();
  const auto capacity = backend.params().sq_entries;
  for (uint32_t i = 0; i <= capacity; ++i) {
    backend.queueCancel();
  }
  backend.submitOutstanding();
  backend.processCompleted();
  const auto stats = backend.getAndResetRingStats();
  EXPECT_EQ(stats.sqPeak, capacity);
  EXPECT_EQ(stats.sqFullCount, 1);
  const auto reset = backend.getAndResetRingStats();
  EXPECT_EQ(reset.sqFullCount, 0);
}

TEST_F(IoUringRingStatsTest, FullSqWithoutFailedAcquisition) {
  RingStatsBackend backend(options());
  backend.submitOutstanding();
  backend.getAndResetRingStats();
  const auto capacity = backend.params().sq_entries;
  for (uint32_t i = 0; i < capacity; ++i) {
    backend.queueCancel();
  }
  backend.submitOutstanding();
  backend.processCompleted();
  const auto stats = backend.getAndResetRingStats();
  EXPECT_EQ(stats.sqPeak, capacity);
  EXPECT_EQ(stats.sqFullCount, 0);
}

TEST_F(IoUringRingStatsTest, CqPeakPrecedesReapLimitAndDeferredTaskrun) {
  struct Nop : folly::IoSqeBase {
    void processSubmit(io_uring_sqe* sqe) noexcept override {
      ::io_uring_prep_nop(sqe);
    }
    void callback(const io_uring_cqe* cqe) noexcept override {
      EXPECT_EQ(cqe->res, 0);
    }
    void callbackCancelled(const io_uring_cqe*) noexcept override {}
  };

  for (bool deferTaskrun : {false, true}) {
    SCOPED_TRACE(deferTaskrun);
    auto opts = options();
    opts.setDeferTaskRun(deferTaskrun).setMaxSubmit(4);
    RingStatsBackend backend(std::move(opts));
    backend.submitOutstanding();
    backend.getAndResetRingStats();
    std::array<Nop, 6> nops;
    for (auto& nop : nops) {
      backend.submitNextLoop(nop);
    }
    backend.submitOutstanding();
    backend.loopPoll();
    const auto first = backend.getAndResetRingStats();
    EXPECT_EQ(first.sqPeak, 4);
    EXPECT_EQ(first.cqPeak, 6);

    backend.processCompleted();
    const auto second = backend.getAndResetRingStats();
    EXPECT_EQ(second.cqPeak, 4);

    backend.loopPoll();
    const auto third = backend.getAndResetRingStats();
    EXPECT_EQ(third.cqPeak, 2);
    EXPECT_EQ(backend.getAndResetRingStats().cqPeak, 0);
  }
}

TEST_F(IoUringRingStatsTest, CountsCqOverflowObservations) {
  RingStatsBackend backend(options(8));
  SKIP_IF(!(backend.params().features & IORING_FEAT_NODROP))
      << "Kernel cannot retain overflow completions";
  backend.submitOutstanding();
  backend.getAndResetRingStats();
  const auto capacity = backend.params().cq_entries;
  for (uint32_t i = 0; i < capacity; ++i) {
    backend.queueCancel();
  }
  backend.submitOutstanding();
  backend.queueCancel();
  backend.submitOutstanding();
  ASSERT_TRUE(::io_uring_cq_has_overflow(backend.ioRingPtr()));
  backend.submitOutstanding();
  backend.submitOutstanding();
  EXPECT_EQ(backend.getAndResetRingStats().cqOverflowCount, 2);
  backend.submitOutstanding();
  EXPECT_EQ(backend.getAndResetRingStats().cqOverflowCount, 1);
  backend.loopPoll();
  EXPECT_FALSE(::io_uring_cq_has_overflow(backend.ioRingPtr()));
  const auto stats = backend.getAndResetRingStats();
  EXPECT_EQ(stats.cqPeak, capacity);
  EXPECT_GT(stats.cqOverflowCount, 0);
  EXPECT_EQ(backend.getAndResetRingStats().cqOverflowCount, 0);
}

} // namespace
