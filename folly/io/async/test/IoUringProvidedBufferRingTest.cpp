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

#include <folly/io/async/IoUringProvidedBufferRing.h>

#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <folly/portability/SysMman.h>

#if FOLLY_HAS_LIBURING

using namespace ::testing;
using namespace ::std;
using namespace ::folly;

struct IoUringProvidedBufferRingTest : testing::Test {
  void SetUp() override {
    const auto ret = ::io_uring_queue_init(2, &ring_, 0);
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
class IoUringProvidedBufferRingTestHelper {
 public:
  explicit IoUringProvidedBufferRingTestHelper(IoUringProvidedBufferRing& ring)
      : ring(ring) {}

  uint32_t ringBufferCount() { return ring.ringBufferCount_; }
  uint32_t returnedBuffers() { return ring.bufferReturnedCount; }
  uint32_t bufferMemSize() { return ring.bufferMemSize_; }

  IoUringProvidedBufferRing& ring;
};
} // namespace folly

TEST_F(IoUringProvidedBufferRingTest, Create) {
  IoUringProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 1024,
      .bufferSize = 4096,
  };
  auto bufRing = IoUringProvidedBufferRing::create(&ring_, options);
  EXPECT_EQ(bufRing->count(), 1024);
  EXPECT_TRUE(bufRing->available());
  EXPECT_EQ(bufRing->sizePerBuffer(), 4096);
  IoUringProvidedBufferRingTestHelper helper(*bufRing);
  EXPECT_EQ(helper.ringBufferCount(), 1024);
}

TEST_F(IoUringProvidedBufferRingTest, CreateNoHugepages) {
  IoUringProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 2048,
      .bufferSize = 4096,
  };
  auto bufRing = IoUringProvidedBufferRing::create(&ring_, options);
  EXPECT_EQ(bufRing->count(), 2048);
  EXPECT_TRUE(bufRing->available());
  EXPECT_EQ(bufRing->sizePerBuffer(), 4096);
  IoUringProvidedBufferRingTestHelper helper(*bufRing);
  EXPECT_EQ(helper.ringBufferCount(), 2048);
}

TEST_F(IoUringProvidedBufferRingTest, KernelAllocatedRing) {
  IoUringProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 512,
      .bufferSize = 4096,
  };
  auto bufRing = IoUringProvidedBufferRing::create(&ring_, options);
  IoUringProvidedBufferRingTestHelper helper(*bufRing);
  EXPECT_EQ(helper.bufferMemSize(), 2 * 1024 * 1024);

  auto ringMemSize = sizeof(struct io_uring_buf_ring) * options.bufferCount;
  off_t offset = IORING_OFF_PBUF_RING |
      (static_cast<uint64_t>(options.gid) << IORING_OFF_PBUF_SHIFT);
  auto* ringMem = ::mmap(
      nullptr,
      ringMemSize,
      PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_POPULATE,
      ring_.ring_fd,
      offset);
  ASSERT_NE(ringMem, MAP_FAILED);
  auto* descriptors = static_cast<struct io_uring_buf_ring*>(ringMem);
  EXPECT_EQ(descriptors->tail, options.bufferCount);

  auto buf = bufRing->getIoBuf(0, 1024, false);
  EXPECT_EQ(descriptors->bufs[0].addr, reinterpret_cast<uint64_t>(buf->data()));
  EXPECT_EQ(descriptors->bufs[0].len, options.bufferSize);
  buf.reset();
  EXPECT_EQ(descriptors->tail, options.bufferCount + 1);

  EXPECT_EQ(::munmap(ringMem, ringMemSize), 0);
}

TEST_F(IoUringProvidedBufferRingTest, BufferMinSize) {
  IoUringProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 16,
      .bufferSize = 8,
  };
  auto bufRing = IoUringProvidedBufferRing::create(&ring_, options);
  EXPECT_EQ(bufRing->count(), 16);
  EXPECT_TRUE(bufRing->available());
  // constexpr size_t kMinBufferSize = 32;
  EXPECT_EQ(bufRing->sizePerBuffer(), 32);
  IoUringProvidedBufferRingTestHelper helper(*bufRing);
  EXPECT_EQ(helper.ringBufferCount(), 16);
}

TEST_F(IoUringProvidedBufferRingTest, BufferCountCheck) {
  uint16_t bgid = 0;

  auto makeOptions = [&bgid](uint32_t bufferCount) {
    return IoUringProvidedBufferRing::Options{
        .gid = bgid++,
        .bufferCount = bufferCount,
        .bufferSize = 32,
    };
  };

  auto minRing = IoUringProvidedBufferRing::create(&ring_, makeOptions(2));
  EXPECT_EQ(minRing->count(), 2);

  auto bufRing = IoUringProvidedBufferRing::create(&ring_, makeOptions(1000));
  EXPECT_EQ(bufRing->count(), 1024);

  auto roundedRing = IoUringProvidedBufferRing::create(&ring_, makeOptions(1));
  EXPECT_EQ(roundedRing->count(), 2);

  EXPECT_THROW(
      IoUringProvidedBufferRing::create(&ring_, makeOptions(0)),
      std::runtime_error);

  EXPECT_THROW(
      IoUringProvidedBufferRing::create(&ring_, makeOptions(32769)),
      std::runtime_error);
}

TEST_F(IoUringProvidedBufferRingTest, DelayedDestruction) {
  IoUringProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 1024,
      .bufferSize = 4096,
  };
  auto bufRing = IoUringProvidedBufferRing::create(&ring_, options);
  auto buf1 = bufRing->getIoBuf(0, 1024, false);
  auto buf2 = bufRing->getIoBuf(1, 1024, false);
  buf1.reset();
  bufRing.reset();
  buf2.reset();
}

TEST_F(IoUringProvidedBufferRingTest, ConcurrentDecBufferState) {
  constexpr size_t kBufsPerThread = 64;
  constexpr int kNumThreads = 16;
  constexpr uint32_t kBufferCount = kBufsPerThread * kNumThreads;

  IoUringProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = kBufferCount,
      .bufferSize = 64,
  };
  auto bufRing = IoUringProvidedBufferRing::create(&ring_, options);

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
      // Each reset triggers free_fn -> decBufferState
      for (auto& buf : threadBufs) {
        buf.reset();
      }
    });
  }

  go.store(true, std::memory_order_release);
  for (auto& t : threads) {
    t.join();
  }

  IoUringProvidedBufferRingTestHelper helper(*bufRing);
  EXPECT_EQ(helper.returnedBuffers(), kBufferCount);
}

TEST_F(
    IoUringProvidedBufferRingTest, IncrementalPartiallyConsumedSingleBuffer) {
  IoUringProvidedBufferRing::Options options = {
      .gid = 1,
      .bufferCount = 4,
      .bufferSize = 64,
      .useIncrementalBuffers = true,
  };
  auto bufRing = IoUringProvidedBufferRing::create(&ring_, options);

  auto first = bufRing->getIoBuf(0, 30, true);
  EXPECT_EQ(first->length(), 30);

  auto second = bufRing->getIoBuf(0, 40, false);
  EXPECT_TRUE(second->isChained());
  EXPECT_EQ(second->computeChainDataLength(), 40);
  EXPECT_EQ(second->length(), 34);
  EXPECT_EQ(second->next()->length(), 6);
}

#endif
