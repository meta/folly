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

#include <folly/io/async/IoUringDynamicProvidedBufferRing.h>
#include <folly/io/async/IoUringSetupRetry.h>
#include <folly/portability/SysMman.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#if FOLLY_HAS_LIBURING

using namespace ::testing;
using namespace ::std;
using namespace ::folly;

namespace {

IoUringDynamicProvidedBufferRing::UniquePtr createBufferRing(
    io_uring& ioRing, IoUringDynamicProvidedBufferRing::Options options) {
  return folly::detail::retryTransientIoUringSetup<
      IoUringDynamicProvidedBufferRing::OutOfMemory>([&] {
    return IoUringDynamicProvidedBufferRing::create(&ioRing, options);
  });
}

} // namespace

struct IoUringDynamicProvidedBufferRingTest : testing::Test {
  void SetUp() override {
    const auto ret = folly::detail::retryTransientIoUringSetup([&] {
      return ::io_uring_queue_init(2, &ring_, 0);
    });
    ASSERT_EQ(0, ret);
    ringInitialized_ = true;
  }

  void TearDown() override {
    if (ringInitialized_) {
      ::io_uring_queue_exit(&ring_);
    }
  }

  io_uring ring_{};

 private:
  bool ringInitialized_{false};
};

namespace folly {
class IoUringDynamicProvidedBufferRingTestHelper {
 public:
  explicit IoUringDynamicProvidedBufferRingTestHelper(
      IoUringDynamicProvidedBufferRing& ring)
      : ring(ring) {}

  uint32_t ringBufferCount() { return ring.ringBufferCount_; }
  uint32_t areaCount() { return ring.areaCount_; }

  const char* areaData(uint32_t area, uint16_t bid) {
    return ring.getData(*ring.areas_[area], bid);
  }
  uint32_t areaOutstanding(uint32_t area) {
    return ring.areas_[area]->outstanding.load();
  }

  uint32_t activeArea() { return areaIndexOf(ring.bufferActiveArea_); }
  uint32_t refillArea() { return areaIndexOf(ring.bufferRefillArea_); }

  bool available() { return ring.available(); }
  uint32_t enobufCount() { return ring.getAndResetEnobufCount(); }

  uint32_t ringAvailable() { return ring.ringTail_ - ring.ringHead_; }
  uint64_t outstandingSum() { return ring.areasOutstandingSum(); }
  uint16_t headBid() { return ring.ringBuf(ring.ringHead_)->bid; }
  const unsigned char* headAddr() {
    return reinterpret_cast<const unsigned char*>(
        ring.ringBuf(ring.ringHead_)->addr);
  }

  void setRingRefillThreshold(uint16_t threshold) {
    ring.ringRefillThreshold_ = threshold;
  }

  int areaOfPtr(const void* p) {
    for (uint32_t a = 0; a < ring.areaCount_; a++) {
      const char* base = ring.areas_[a]->buffers;
      const char* end = base +
          static_cast<size_t>(ring.ringBufferCount_) * ring.sizePerBuffer_;
      if (p >= base && p < end) {
        return static_cast<int>(a);
      }
    }
    return -1;
  }

  IoUringDynamicProvidedBufferRing& ring;

 private:
  uint32_t areaIndexOf(
      const IoUringDynamicProvidedBufferRing::BufferArea* area) {
    for (uint32_t a = 0; a < ring.areaCount_; a++) {
      if (ring.areas_[a].get() == area) {
        return a;
      }
    }
    return std::numeric_limits<uint32_t>::max();
  }
};
} // namespace folly

TEST_F(IoUringDynamicProvidedBufferRingTest, Create) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 1024,
      .bufferSize = 4096,
  };
  auto bufRing = createBufferRing(ring_, options);
  EXPECT_EQ(bufRing->count(), 1024);
  EXPECT_TRUE(bufRing->available());
  EXPECT_EQ(bufRing->sizePerBuffer(), 4096);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);
  EXPECT_EQ(helper.ringBufferCount(), 1024);
}

TEST_F(IoUringDynamicProvidedBufferRingTest, CreateNoHugepages) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 2048,
      .bufferSize = 4096,
  };
  auto bufRing = createBufferRing(ring_, options);
  EXPECT_EQ(bufRing->count(), 2048);
  EXPECT_TRUE(bufRing->available());
  EXPECT_EQ(bufRing->sizePerBuffer(), 4096);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);
  EXPECT_EQ(helper.ringBufferCount(), 2048);
}

TEST_F(IoUringDynamicProvidedBufferRingTest, BufferMinSize) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 16,
      .bufferSize = 8,
  };
  auto bufRing = createBufferRing(ring_, options);
  EXPECT_EQ(bufRing->count(), 16);
  EXPECT_TRUE(bufRing->available());
  // constexpr size_t kMinBufferSize = 32;
  EXPECT_EQ(bufRing->sizePerBuffer(), 32);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);
  EXPECT_EQ(helper.ringBufferCount(), 16);
}

TEST_F(IoUringDynamicProvidedBufferRingTest, BufferCountCheck) {
  uint16_t bgid = 0;

  auto makeOptions = [&bgid](uint32_t bufferCount) {
    return IoUringDynamicProvidedBufferRing::Options{
        .gid = bgid++,
        .bufferCount = bufferCount,
        .bufferSize = 32,
    };
  };

  // Each ring is destroyed before the next is created so registrations don't
  // accumulate: io_uring_register_buf_ring() counts against RLIMIT_MEMLOCK.
  {
    auto minRing = createBufferRing(ring_, makeOptions(2));
    EXPECT_EQ(minRing->count(), 2);
  }

  {
    auto bufRing = createBufferRing(ring_, makeOptions(1000));
    EXPECT_EQ(bufRing->count(), 1024);
  }

  {
    auto roundedRing = createBufferRing(ring_, makeOptions(1));
    EXPECT_EQ(roundedRing->count(), 2);
  }

  EXPECT_THROW(createBufferRing(ring_, makeOptions(0)), std::runtime_error);

  EXPECT_THROW(createBufferRing(ring_, makeOptions(32769)), std::runtime_error);
}

TEST_F(IoUringDynamicProvidedBufferRingTest, FailedCreateReleasesRing) {
  constexpr uint16_t kGid = 7;

  IoUringDynamicProvidedBufferRing::Options failing = {
      .gid = kGid,
      .bufferCount = 32768,
      .bufferSize = std::numeric_limits<uint32_t>::max(),
  };
  EXPECT_THROW(createBufferRing(ring_, failing), std::runtime_error);

  IoUringDynamicProvidedBufferRing::Options valid = {
      .gid = kGid,
      .bufferCount = 64,
      .bufferSize = 1024,
  };
  IoUringDynamicProvidedBufferRing::UniquePtr bufRing;
  ASSERT_NO_THROW(bufRing = createBufferRing(ring_, valid));
  EXPECT_EQ(bufRing->count(), 64);
}

TEST_F(IoUringDynamicProvidedBufferRingTest, DelayedDestruction) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 1024,
      .bufferSize = 4096,
  };
  auto bufRing = createBufferRing(ring_, options);
  auto buf1 = bufRing->getIoBuf(0, 1024, false);
  auto buf2 = bufRing->getIoBuf(1, 1024, false);
  buf1.reset();
  bufRing.reset();
  buf2.reset();
}

TEST_F(IoUringDynamicProvidedBufferRingTest, ConcurrentDecBufferState) {
  constexpr size_t kBufsPerThread = 64;
  constexpr int kNumThreads = 16;
  constexpr uint32_t kBufferCount = kBufsPerThread * kNumThreads;

  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = kBufferCount,
      .bufferSize = 64,
  };
  auto bufRing = createBufferRing(ring_, options);

  // Acquire all buffers
  std::vector<std::unique_ptr<IOBuf>> bufs;
  bufs.reserve(kBufferCount);
  for (uint32_t i = 0; i < kBufferCount; i++) {
    bufs.push_back(bufRing->getIoBuf(i, 32, false));
  }

  // Distribute buffers across threads
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < kNumThreads; t++) {
    size_t start = t * kBufsPerThread;
    std::vector<std::unique_ptr<IOBuf>> threadBufs;
    threadBufs.reserve(kBufsPerThread);
    for (size_t i = start; i < start + kBufsPerThread; i++) {
      threadBufs.push_back(std::move(bufs[i]));
    }

    threads.emplace_back([&go, threadBufs = std::move(threadBufs)]() mutable {
      while (!go.load(std::memory_order_acquire)) {
      }
      // Each reset triggers bufFreeFn
      for (auto& buf : threadBufs) {
        buf.reset();
      }
    });
  }

  go.store(true, std::memory_order_release);
  for (auto& t : threads) {
    t.join();
  }

  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);
  EXPECT_EQ(helper.outstandingSum(), helper.ringAvailable())
      << "once every buffer is returned, the only outstanding entries are the "
         "ones still posted in the ring";
}

TEST_F(
    IoUringDynamicProvidedBufferRingTest,
    IncrementalPartiallyConsumedSingleBuffer) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 4,
      .bufferSize = 64,
      .useIncrementalBuffers = true,
  };
  auto bufRing = createBufferRing(ring_, options);

  auto first = bufRing->getIoBuf(0, 30, true);
  EXPECT_EQ(first->length(), 30);

  auto second = bufRing->getIoBuf(0, 40, false);
  EXPECT_TRUE(second->isChained());
  EXPECT_EQ(second->computeChainDataLength(), 40);
  EXPECT_EQ(second->length(), 34);
  EXPECT_EQ(second->next()->length(), 6);
}

static std::unique_ptr<folly::IOBuf> consumeOne(
    folly::IoUringDynamicProvidedBufferRing& ring,
    folly::IoUringDynamicProvidedBufferRingTestHelper& helper,
    size_t len) {
  uint16_t bid = helper.headBid();
  return ring.getIoBuf(bid, len, false);
}

TEST_F(IoUringDynamicProvidedBufferRingTest, GrowsOnExhaustion) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 8,
      .bufferSize = 64,
  };
  auto bufRing = createBufferRing(ring_, options);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);
  ASSERT_EQ(helper.areaCount(), 2u);

  std::vector<std::unique_ptr<folly::IOBuf>> held;
  for (int i = 0; i < 8; i++) {
    held.push_back(consumeOne(*bufRing, helper, 64));
  }
  EXPECT_EQ(helper.areaCount(), 2u);

  for (int i = 0; i < 8; i++) {
    held.push_back(consumeOne(*bufRing, helper, 64));
  }
  EXPECT_EQ(helper.areaCount(), 3u);
}

TEST_F(
    IoUringDynamicProvidedBufferRingTest,
    DestroyWithBuffersHeldInSeveralAreas) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 8,
      .bufferSize = 64,
  };
  auto bufRing = createBufferRing(ring_, options);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);

  std::vector<std::unique_ptr<folly::IOBuf>> held;
  held.reserve(16);
  for (int i = 0; i < 16; i++) {
    held.push_back(consumeOne(*bufRing, helper, 64));
  }
  ASSERT_EQ(helper.areaCount(), 3u);
  for (size_t i = 0; i < held.size(); i += 2) {
    held[i].reset();
  }

  bufRing.reset();

  for (auto& buf : held) {
    if (buf) {
      EXPECT_EQ(std::count(buf->data(), buf->tail(), 0), 64)
          << "held buffers must stay mapped after the ring is destroyed";
      buf.reset();
    }
  }
}

static void expectUnmapped(const void* address) {
  unsigned char resident = 0;
  const auto ret = ::mincore(const_cast<void*>(address), 1, &resident);
  const auto error = errno;
  EXPECT_EQ(ret, -1);
  EXPECT_EQ(error, ENOMEM);
}

TEST_F(IoUringDynamicProvidedBufferRingTest, DestroyUnusedIncrementalRing) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 4,
      .bufferSize = 64,
      .useIncrementalBuffers = true,
  };
  auto bufRing = createBufferRing(ring_, options);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);
  EXPECT_EQ(helper.outstandingSum(), options.bufferCount);

  std::vector<const void*> areas;
  areas.reserve(helper.areaCount());
  for (uint32_t area = 0; area < helper.areaCount(); ++area) {
    areas.push_back(helper.areaData(area, 0));
  }
  bufRing.reset();

  for (const auto* area : areas) {
    expectUnmapped(area);
  }
}

TEST_F(
    IoUringDynamicProvidedBufferRingTest,
    DestroyWithIncrementalSliceOutstanding) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 4,
      .bufferSize = 64,
      .useIncrementalBuffers = true,
  };
  auto bufRing = createBufferRing(ring_, options);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);
  const auto* area = helper.areaData(helper.activeArea(), 0);

  // Buffer 0 stays in the ring, partially consumed by this slice.
  auto slice = bufRing->getIoBuf(0, 30, true);
  EXPECT_EQ(helper.outstandingSum(), options.bufferCount);
  bufRing.reset();

  EXPECT_EQ(std::count(slice->data(), slice->tail(), 0), 30)
      << "slice must stay mapped after the ring is destroyed";
  slice.reset();
  expectUnmapped(area);
}

TEST_F(
    IoUringDynamicProvidedBufferRingTest, BundleFollowsRefillOrderAcrossReuse) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 4,
      .bufferSize = 64,
  };
  auto bufRing = createBufferRing(ring_, options);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);
  helper.setRingRefillThreshold(1);

  std::vector<std::unique_ptr<folly::IOBuf>> held;
  bool tested = false;
  for (int step = 0; step < 300 && !tested; step++) {
    uint16_t headBid = helper.headBid();
    uint32_t front = helper.activeArea();
    uint32_t next = helper.refillArea();
    if (headBid != 0 && next != front) {
      uint32_t avail = helper.ringAvailable();
      uint32_t tailOfFront = options.bufferCount - headBid; // entries in front
      if (avail > tailOfFront) {
        // A bundle starting at headBid straddles from the active area into the
        // refill area. Its segments must follow refill's posted area order
        // regardless of the areas' absolute indices in areas_.
        auto buf =
            bufRing->getIoBuf(headBid, options.bufferSize * avail, false);
        std::vector<int> segAreas;
        const folly::IOBuf* cur = buf.get();
        do {
          segAreas.push_back(helper.areaOfPtr(cur->data()));
          cur = cur->next();
        } while (cur != buf.get());

        std::vector<int> expected;
        for (uint32_t i = 0; i < avail; i++) {
          expected.push_back(
              i < tailOfFront ? static_cast<int>(front)
                              : static_cast<int>(next));
        }
        EXPECT_EQ(segAreas, expected)
            << "bundle segments must follow refill's posted area order "
               "(front="
            << front << ", next=" << next << ")";
        held.push_back(std::move(buf));
        tested = true;
        break;
      }
    }
    if (helper.ringAvailable() == 0) {
      break;
    }
    auto b = consumeOne(*bufRing, helper, 64);
    if (helper.areaOfPtr(b->data()) == static_cast<int>(helper.refillArea())) {
      b.reset();
    } else {
      held.push_back(std::move(b));
    }
  }
  EXPECT_TRUE(tested)
      << "expected a straddling bundle to exercise the area boundary";
}

TEST_F(
    IoUringDynamicProvidedBufferRingTest,
    ReuseStabilizesWithoutUnboundedGrowth) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 8,
      .bufferSize = 64,
  };
  auto bufRing = createBufferRing(ring_, options);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);
  helper.setRingRefillThreshold(1);

  for (int i = 0; i < 200; i++) {
    auto buf = consumeOne(*bufRing, helper, 64);
    buf.reset();
  }
  EXPECT_EQ(helper.areaCount(), 2u)
      << "prompt returns must let areas be reused, not grow unboundedly";
}

TEST_F(IoUringDynamicProvidedBufferRingTest, GrowthIsCappedWhenAllBuffersHeld) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 2,
      .bufferSize = 64,
  };
  auto bufRing = createBufferRing(ring_, options);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);

  // A reader that never releases buffers forces the pool to grow, since no
  // area ever drains. Growth must stop at the cap (kMaxAreaCount) rather than
  // allocating without bound.
  std::vector<std::unique_ptr<folly::IOBuf>> held;
  for (int i = 0; i < 10000 && helper.available(); i++) {
    if (helper.ringAvailable() == 0) {
      bufRing->enobuf();
      continue;
    }
    held.push_back(consumeOne(*bufRing, helper, 64));
  }

  EXPECT_LE(helper.areaCount(), 64u) << "area growth must be capped";
  EXPECT_FALSE(helper.available())
      << "capped pool with all buffers held must signal ENOBUFS backpressure";
}

TEST_F(IoUringDynamicProvidedBufferRingTest, ReuseOnlyWhenFullyDrained) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 8,
      .bufferSize = 64,
  };
  auto bufRing = createBufferRing(ring_, options);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);

  std::vector<std::unique_ptr<folly::IOBuf>> tmp;
  const uint32_t consumedArea = helper.activeArea();
  for (int i = 0; i < 4; i++) {
    tmp.push_back(consumeOne(*bufRing, helper, 64));
  }
  tmp.clear(); // return the 4 held buffers
  EXPECT_GT(helper.areaOutstanding(consumedArea), 0u)
      << "some entries still queued in ring keep the area non-drained";

  std::vector<std::unique_ptr<folly::IOBuf>> held;
  for (int i = 0; i < 4; i++) {
    held.push_back(consumeOne(*bufRing, helper, 64));
  }
  EXPECT_EQ(helper.areaCount(), 2u)
      << "consumed area has outstanding > 0, must not be reused; pool must grow";
}

TEST_F(IoUringDynamicProvidedBufferRingTest, activeAreaMatchesRefillOrder) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 8,
      .bufferSize = 64,
  };
  auto bufRing = createBufferRing(ring_, options);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);

  std::vector<std::unique_ptr<folly::IOBuf>> held;
  for (int pass = 0; pass < 3; pass++) {
    for (int i = 0; i < 8; i++) {
      uint32_t expectedArea = helper.activeArea();

      uint16_t bid = helper.headBid();
      auto buf = bufRing->getIoBuf(bid, 64, false);
      EXPECT_EQ(helper.areaOfPtr(buf->data()), static_cast<int>(expectedArea))
          << "consumed buffer must belong to the area refill posted";
      held.push_back(std::move(buf));
    }
  }
  EXPECT_GT(helper.areaCount(), 1u);
}

TEST_F(IoUringDynamicProvidedBufferRingTest, ContiguityWithinFullRingLoad) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 8,
      .bufferSize = 64,
  };
  auto bufRing = createBufferRing(ring_, options);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);

  uint16_t bid = helper.headBid();
  ASSERT_EQ(bid, 0);
  auto buf = bufRing->getIoBuf(bid, 64 * 3, false);
  ASSERT_TRUE(buf->isChained());

  const uint8_t* prevEnd = nullptr;
  const folly::IOBuf* cur = buf.get();
  do {
    if (prevEnd) {
      EXPECT_EQ(cur->data(), prevEnd) << "segments must be contiguous";
    }
    prevEnd = cur->data() + cur->length();
    cur = cur->next();
  } while (cur != buf.get());
}

TEST_F(IoUringDynamicProvidedBufferRingTest, BundleAcrossAreaBoundary) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 4,
      .bufferSize = 64,
  };
  auto bufRing = createBufferRing(ring_, options);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);
  helper.setRingRefillThreshold(1);

  auto held0 = consumeOne(*bufRing, helper, 64);
  ASSERT_EQ(helper.areaCount(), 2u);
  const uint32_t front = helper.activeArea();
  const uint32_t next = helper.refillArea();
  EXPECT_NE(front, next);

  uint16_t bid = helper.headBid();
  ASSERT_EQ(bid, 1);
  auto buf = bufRing->getIoBuf(bid, 64 * 4, false);
  ASSERT_TRUE(buf->isChained());

  std::vector<int> segAreas;
  const folly::IOBuf* cur = buf.get();
  do {
    segAreas.push_back(helper.areaOfPtr(cur->data()));
    cur = cur->next();
  } while (cur != buf.get());

  const std::vector<int> expected{
      static_cast<int>(front),
      static_cast<int>(front),
      static_cast<int>(front),
      static_cast<int>(next)};
  EXPECT_EQ(segAreas, expected)
      << "bundle segments must follow refill's posted area order";
}

static void growHolding(
    folly::IoUringDynamicProvidedBufferRing& ring,
    folly::IoUringDynamicProvidedBufferRingTestHelper& helper,
    uint32_t count,
    std::vector<std::unique_ptr<folly::IOBuf>>& held,
    std::vector<int>& heldArea) {
  for (uint32_t i = 0; i < count; i++) {
    auto buf = consumeOne(ring, helper, 64);
    heldArea.push_back(helper.areaOfPtr(buf->data()));
    held.push_back(std::move(buf));
  }
}

TEST_F(IoUringDynamicProvidedBufferRingTest, ShrinkAfterGrowthDownToFloor) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 8,
      .bufferSize = 64,
  };
  auto bufRing = createBufferRing(ring_, options);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);
  helper.setRingRefillThreshold(1);
  ASSERT_EQ(helper.areaCount(), 2u);

  std::vector<std::unique_ptr<folly::IOBuf>> held;
  std::vector<int> heldArea;
  while (helper.areaCount() < 4u && helper.ringAvailable() > 0) {
    growHolding(*bufRing, helper, 1, held, heldArea);
  }
  const uint32_t grownCount = helper.areaCount();
  ASSERT_GT(grownCount, 2u) << "precondition: pool must have grown";

  held.clear();
  heldArea.clear();

  uint32_t prev = helper.areaCount();
  for (int i = 0; i < 500 && helper.ringAvailable() > 0; i++) {
    auto buf = consumeOne(*bufRing, helper, 64);
    buf.reset();
    bufRing->enobuf();
    const uint32_t now = helper.areaCount();
    ASSERT_LE(now, prev) << "pool must never grow while shrinking";
    ASSERT_LE(prev - now, 1u) << "at most one area freed per pass";
    ASSERT_GE(now, 2u) << "must never drop below the floor";
    prev = now;
  }
  EXPECT_EQ(helper.areaCount(), 2u)
      << "pool must shrink back down to the floor of kInitialAreaCount";
}

TEST_F(
    IoUringDynamicProvidedBufferRingTest, NoShrinkWhenLessThanHalfFreeSlots) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 8,
      .bufferSize = 64,
  };
  auto bufRing = createBufferRing(ring_, options);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);
  helper.setRingRefillThreshold(1);

  std::vector<std::unique_ptr<folly::IOBuf>> held;
  std::vector<int> heldArea;
  growHolding(*bufRing, helper, 32, held, heldArea);
  ASSERT_EQ(helper.areaCount(), 5u);

  for (size_t i = 0; i < held.size(); i++) {
    if (heldArea[i] == 0) {
      held[i].reset();
    }
  }
  ASSERT_EQ(helper.areaOutstanding(0), 0u);

  uint64_t totalSlots = helper.areaCount() * options.bufferCount;
  uint64_t halfTotal = totalSlots >> 1;
  ASSERT_GT(helper.outstandingSum(), halfTotal)
      << "precondition: outstanding must exceed 50% of total slots";

  uint32_t areaCountBefore = helper.areaCount();
  bufRing->enobuf();
  EXPECT_EQ(helper.areaCount(), areaCountBefore)
      << "must not shrink when less than 50% of slots are free";
}

TEST_F(IoUringDynamicProvidedBufferRingTest, ShrinkWhenMoreThanHalfFreeSlots) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 8,
      .bufferSize = 64,
  };
  auto bufRing = createBufferRing(ring_, options);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);

  std::vector<std::unique_ptr<folly::IOBuf>> held;
  std::vector<int> heldArea;
  growHolding(*bufRing, helper, 32, held, heldArea);
  ASSERT_EQ(helper.areaCount(), 5u);

  for (size_t i = 0; i < held.size(); i++) {
    if (heldArea[i] == 0 || heldArea[i] == 1 || heldArea[i] == 2) {
      held[i].reset();
    }
  }
  ASSERT_EQ(helper.areaOutstanding(0), 0u);
  ASSERT_EQ(helper.areaOutstanding(1), 0u);
  ASSERT_EQ(helper.areaOutstanding(2), 0u);

  uint64_t totalSlots = helper.areaCount() * options.bufferCount;
  uint64_t halfTotal = totalSlots >> 1;
  ASSERT_LE(helper.outstandingSum(), halfTotal)
      << "precondition: outstanding must be at most 50% of total slots";

  bufRing->enobuf();
  EXPECT_LT(helper.areaCount(), 5u)
      << "must shrink when more than 50% of slots are free";
}

TEST_F(
    IoUringDynamicProvidedBufferRingTest,
    ShrinkNeverRemovesActiveOrRefillArea) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 8,
      .bufferSize = 64,
  };
  auto bufRing = createBufferRing(ring_, options);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);
  helper.setRingRefillThreshold(1);

  std::vector<std::unique_ptr<folly::IOBuf>> held;
  std::vector<int> heldArea;
  growHolding(*bufRing, helper, 32, held, heldArea);
  ASSERT_EQ(helper.areaCount(), 5u);

  for (size_t i = 0; i < held.size(); i++) {
    if (heldArea[i] == 0 || heldArea[i] == 1 || heldArea[i] == 2) {
      held[i].reset();
    }
  }

  bufRing->enobuf();
  ASSERT_EQ(helper.areaCount(), 4u)
      << "a non-refill drained area must be freed";
  const uint32_t refill = helper.refillArea();
  ASSERT_LT(refill, helper.areaCount())
      << "refill area must still be a live area";

  for (int i = 0; i < 100 && helper.ringAvailable() > 0; i++) {
    auto buf = consumeOne(*bufRing, helper, 64);
    EXPECT_GE(helper.areaOfPtr(buf->data()), 0)
        << "every handed-out buffer must resolve to a live area";
    EXPECT_GE(helper.areaCount(), 2u);
    buf.reset();
  }
}

TEST_F(
    IoUringDynamicProvidedBufferRingTest, ringRefillThresholdBatchesRefills) {
  constexpr uint32_t kBufferCount = 16;
  constexpr uint8_t kThreshold = 4;
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = kBufferCount,
      .bufferSize = 64,
  };
  auto bufRing = createBufferRing(ring_, options);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);
  helper.setRingRefillThreshold(kThreshold);
  ASSERT_EQ(helper.ringAvailable(), kBufferCount);

  std::vector<std::unique_ptr<folly::IOBuf>> held;

  for (uint8_t i = 1; i < kThreshold; i++) {
    held.push_back(consumeOne(*bufRing, helper, 64));
    EXPECT_EQ(helper.ringAvailable(), kBufferCount - i)
        << "refill must be batched, not run per consumption (i=" << i << ")";
  }

  held.push_back(consumeOne(*bufRing, helper, 64));
  EXPECT_EQ(helper.ringAvailable(), kBufferCount)
      << "threshold consumption should trigger a batched refill";
}

TEST_F(
    IoUringDynamicProvidedBufferRingTest, RefillAfterAreaCapUsesRecycledArea) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 2,
      .bufferSize = 64,
  };
  auto bufRing = createBufferRing(ring_, options);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);

  std::vector<std::unique_ptr<folly::IOBuf>> held;
  for (int i = 0; i < 1000 && helper.ringAvailable() > 0; i++) {
    held.push_back(consumeOne(*bufRing, helper, 64));
  }
  ASSERT_EQ(helper.areaCount(), 64u);
  ASSERT_EQ(helper.ringAvailable(), 0u);

  held.erase(held.begin(), held.begin() + options.bufferCount);

  bufRing->enobuf();
  ASSERT_EQ(helper.ringAvailable(), options.bufferCount);

  for (uint32_t i = 0; i < options.bufferCount; i++) {
    const auto* posted = helper.headAddr();
    auto buf = consumeOne(*bufRing, helper, 64);
    EXPECT_EQ(buf->data(), posted);
  }
}

// The producer (getIoBuf) runs on a single thread while returns land on
// short-lived threads. Those returns must never destroy the live ring.
TEST_F(
    IoUringDynamicProvidedBufferRingTest, ConsumerThreadsExitWhileRingAlive) {
  constexpr uint32_t kBufferCount = 64;
  constexpr int kRounds = 50;

  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = kBufferCount,
      .bufferSize = 64,
  };
  auto bufRing = createBufferRing(ring_, options);
  IoUringDynamicProvidedBufferRingTestHelper helper(*bufRing);

  for (int round = 0; round < kRounds; round++) {
    // Producer stays on this thread, as it would on the io_uring thread.
    bufRing->enobuf(); // refill the ring after the previous round drained it
    std::vector<std::unique_ptr<IOBuf>> bufs;
    while (helper.ringAvailable() > 0) {
      bufs.push_back(consumeOne(*bufRing, helper, 64));
    }
    ASSERT_FALSE(bufs.empty()) << "round " << round;

    // Each returner is a fresh, short-lived thread: it only ever decrements,
    // then exits and folds a negative local count into the global count.
    std::vector<std::thread> returners;
    size_t perThread = (bufs.size() + 3) / 4;
    for (size_t start = 0; start < bufs.size(); start += perThread) {
      size_t end = std::min(start + perThread, bufs.size());
      std::vector<std::unique_ptr<IOBuf>> owned;
      for (size_t i = start; i < end; i++) {
        owned.push_back(std::move(bufs[i]));
      }
      returners.emplace_back([owned = std::move(owned)]() mutable {
        owned.clear();
      });
    }
    for (auto& returner : returners) {
      returner.join();
    }
  }

  // The ring must still be alive and usable after all those thread exits.
  auto buf = consumeOne(*bufRing, helper, 64);
  EXPECT_NE(buf, nullptr);
  buf.reset();
}

static void runDestroyRaceIteration(
    io_uring& ring,
    uint16_t gid,
    uint32_t bufferCount,
    int numThreads,
    bool incremental = false) {
  IoUringDynamicProvidedBufferRing::Options options = {
      .gid = gid,
      .bufferCount = bufferCount,
      .bufferSize = 64,
      .useIncrementalBuffers = incremental,
  };
  auto bufRing = createBufferRing(ring, options);

  std::vector<std::unique_ptr<IOBuf>> bufs;
  bufs.reserve(bufferCount);
  for (uint32_t bufIdx = 0; bufIdx < bufferCount; bufIdx++) {
    const bool hasMore = incremental && bufIdx == bufferCount - 1;
    bufs.push_back(bufRing->getIoBuf(bufIdx, 32, hasMore));
  }

  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  threads.reserve(numThreads);
  uint32_t bufsPerThread = bufferCount / numThreads;

  for (int threadIdx = 0; threadIdx < numThreads; threadIdx++) {
    uint32_t start = threadIdx * bufsPerThread;
    std::vector<std::unique_ptr<IOBuf>> threadBufs;
    threadBufs.reserve(bufsPerThread);
    for (uint32_t bufIdx = start; bufIdx < start + bufsPerThread; bufIdx++) {
      threadBufs.push_back(std::move(bufs[bufIdx]));
    }

    threads.emplace_back([&go, threadBufs = std::move(threadBufs)]() mutable {
      while (!go.load(std::memory_order_acquire)) {
      }
      for (auto& buf : threadBufs) {
        buf.reset();
      }
    });
  }

  go.store(true, std::memory_order_release);
  bufRing.reset();

  for (auto& thread : threads) {
    thread.join();
  }
}

TEST_F(
    IoUringDynamicProvidedBufferRingTest, ConcurrentDecBufferStateWithDestroy) {
  constexpr int kIterations = 100;
  for (int iter = 0; iter < kIterations; iter++) {
    runDestroyRaceIteration(ring_, iter % 65536, 64, 8);
  }
}

TEST_F(
    IoUringDynamicProvidedBufferRingTest,
    ConcurrentIncrementalSliceReleaseWithDestroy) {
  constexpr int kIterations = 100;
  for (int iter = 0; iter < kIterations; iter++) {
    runDestroyRaceIteration(ring_, iter, 4, 1, true);
  }
}

TEST_F(
    IoUringDynamicProvidedBufferRingTest,
    ConcurrentDecBufferStateWithDestroyStress) {
  constexpr uint32_t kBufferCounts[] = {2, 4, 8, 16, 64};
  constexpr int kThreadCounts[] = {1, 2, 4, 8};

  int iter = 0;
  for (auto bufferCount : kBufferCounts) {
    for (auto numThreads : kThreadCounts) {
      if (bufferCount < static_cast<uint32_t>(numThreads)) {
        continue;
      }
      for (int run = 0; run < 10; run++) {
        runDestroyRaceIteration(ring_, iter % 65536, bufferCount, numThreads);
        iter++;
      }
    }
  }
}

#endif
