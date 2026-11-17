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

#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <folly/io/async/IoUringZeroCopyBufferPool.h>
#include <folly/portability/Unistd.h>

using namespace ::testing;
using namespace ::std;

namespace folly {
class IoUringZeroCopyBufferPoolTestHelper {
 public:
  static IoUringZeroCopyBufferPool::UniquePtr create(
      IoUringZeroCopyBufferPool::Params params, bool flushRq = false) {
    return IoUringZeroCopyBufferPool::UniquePtr(new IoUringZeroCopyBufferPool(
        params, IoUringZeroCopyBufferPool::TestTag{flushRq}));
  }

  explicit IoUringZeroCopyBufferPoolTestHelper(IoUringZeroCopyBufferPool& pool)
      : pool(pool) {}

  uint32_t* getHead() { return pool.getHead(); }
  uint32_t getRingUsedCount() { return pool.getRingUsedCount(); }
  uint32_t getRingFreeCount() { return pool.getRingFreeCount(); }
  size_t getPendingBuffersSize() { return pool.getPendingBuffersSize(); }
  size_t getBufferSize() { return pool.getBufferSize(); }
  void* getNotifStatsPtr() { return pool.getNotifStatsPtr(); }
  uint64_t getNotifUserData() { return pool.kZcrxNotifUserData; }
  void incNoBufferCount() { pool.incNoBufferCount(); }
  void setZcrxId(int zcrxId) { pool.zcrxId_ = zcrxId; }
  bool isRefillFlushPending() { return pool.isRefillFlushPending(); }
  void flushRefillQueueIfNeeded() { pool.flushRefillQueueIfNeeded(); }
  void consumeRefillRingEntries(uint32_t numEntries) {
    *getHead() += numEntries;
  }
  IoUringZeroCopyBufferPool& pool;
};
} // namespace folly

using namespace ::folly;

// TODO: T264228475: remove this once liburing/uapi headers are updated.
struct TestZcrxNotifStats {
  uint64_t copy_count;
  uint64_t copy_bytes;
};

TEST(IoUringZeroCopyBufferPoolTest, GetBuf) {
  IoUringZeroCopyBufferPool::Params params = {
      .ring = nullptr,
      .numBuffers = 32,
      .bufferSizeHint = 4096,
      .rqEntries = 8,
      .ifindex = 0,
      .queueId = 0,
  };
  auto pool = IoUringZeroCopyBufferPoolTestHelper::create(params);
  io_uring_cqe cqe{};
  cqe.res = 2048;
  io_uring_zcrx_cqe zcqe{};
  zcqe.off = 0;
  auto buf = pool->getIoBuf(&cqe, &zcqe).buffer;
  ASSERT_EQ(buf->length(), 2048);
  EXPECT_EQ(buf->capacity() >= buf->length(), true);
}

TEST(IoUringZeroCopyBufferPoolTest, DelayedDestruction) {
  IoUringZeroCopyBufferPool::Params params = {
      .ring = nullptr,
      .numBuffers = 32,
      .bufferSizeHint = 4096,
      .rqEntries = 8,
      .ifindex = 0,
      .queueId = 0,
  };
  auto pool = IoUringZeroCopyBufferPoolTestHelper::create(params);
  io_uring_cqe cqe{};
  cqe.res = 2048;
  io_uring_zcrx_cqe zcqe{};
  zcqe.off = 0;
  auto buf1 = pool->getIoBuf(&cqe, &zcqe).buffer;
  cqe.res = 2048;
  zcqe.off = 4096;
  auto buf2 = pool->getIoBuf(&cqe, &zcqe).buffer;
  buf1.reset();
  pool.reset();
  buf2.reset();
}

TEST(IoUringZeroCopyBufferPoolTest, BuffersScarce) {
  IoUringZeroCopyBufferPool::Params params = {
      .ring = nullptr,
      .numBuffers = 20,
      .bufferSizeHint = 4096,
      .rqEntries = 20,
      .ifindex = 0,
      .queueId = 0,
  };
  auto pool = IoUringZeroCopyBufferPoolTestHelper::create(params);
  io_uring_cqe cqe{};
  cqe.res = 2048;
  io_uring_zcrx_cqe zcqe{};
  vector<unique_ptr<IOBuf>> buffers;

  for (uint32_t i = 0; i < 16; ++i) {
    zcqe.off = i * 4096;
    auto result = pool->getIoBuf(&cqe, &zcqe);
    EXPECT_FALSE(result.isScarce);
    buffers.push_back(std::move(result.buffer));
  }

  zcqe.off = 16 * 4096;
  auto scarceResult = pool->getIoBuf(&cqe, &zcqe);
  EXPECT_TRUE(scarceResult.isScarce);
  buffers.push_back(std::move(scarceResult.buffer));

  buffers.pop_back();
  buffers.pop_back();
  zcqe.off = 17 * 4096;
  auto recoveredResult = pool->getIoBuf(&cqe, &zcqe);
  EXPECT_FALSE(recoveredResult.isScarce);
}

TEST(IoUringZeroCopyBufferPoolTest, SmallPoolReservesOneBuffer) {
  IoUringZeroCopyBufferPool::Params params = {
      .ring = nullptr,
      .numBuffers = 8,
      .bufferSizeHint = 4096,
      .rqEntries = 8,
      .ifindex = 0,
      .queueId = 0,
  };
  auto pool = IoUringZeroCopyBufferPoolTestHelper::create(params);
  io_uring_cqe cqe{};
  cqe.res = 2048;
  io_uring_zcrx_cqe zcqe{};
  vector<unique_ptr<IOBuf>> buffers;

  for (uint32_t i = 0; i < 6; ++i) {
    zcqe.off = i * 4096;
    auto result = pool->getIoBuf(&cqe, &zcqe);
    EXPECT_FALSE(result.isScarce);
    buffers.push_back(std::move(result.buffer));
  }

  zcqe.off = 6 * 4096;
  auto result = pool->getIoBuf(&cqe, &zcqe);
  EXPECT_TRUE(result.isScarce);
  buffers.push_back(std::move(result.buffer));
}

TEST(IoUringZeroCopyBufferPoolTest, RefillTest) {
  IoUringZeroCopyBufferPool::Params params = {
      .ring = nullptr,
      .numBuffers = 8,
      .bufferSizeHint = 4096,
      .rqEntries = 2,
      .ifindex = 0,
      .queueId = 0,
  };
  auto pool = IoUringZeroCopyBufferPoolTestHelper::create(params);
  IoUringZeroCopyBufferPoolTestHelper helper(*pool);
  EXPECT_EQ(helper.getRingUsedCount(), 0);
  EXPECT_EQ(helper.getRingFreeCount(), 2);

  io_uring_cqe cqe{};
  io_uring_zcrx_cqe zcqe{};

  cqe.res = 2048;
  zcqe.off = 0;
  auto buf1 = pool->getIoBuf(&cqe, &zcqe).buffer;

  zcqe.off += 4096;
  auto buf2 = pool->getIoBuf(&cqe, &zcqe).buffer;

  zcqe.off += 4096;
  auto buf3 = pool->getIoBuf(&cqe, &zcqe).buffer;

  buf1.reset();
  buf2.reset();
  buf3.reset();
  EXPECT_EQ(helper.getRingUsedCount(), 2);
  EXPECT_EQ(helper.getRingFreeCount(), 0);
  EXPECT_EQ(helper.getPendingBuffersSize(), 1);

  helper.consumeRefillRingEntries(2);
  EXPECT_EQ(helper.getRingUsedCount(), 0);
  EXPECT_EQ(helper.getRingFreeCount(), 2);
  EXPECT_EQ(helper.getPendingBuffersSize(), 1);

  zcqe.off += 4096;
  auto buf4 = pool->getIoBuf(&cqe, &zcqe).buffer;
  buf4.reset();
  EXPECT_EQ(helper.getRingUsedCount(), 2);
  EXPECT_EQ(helper.getRingFreeCount(), 0);
  EXPECT_EQ(helper.getPendingBuffersSize(), 0);
}

TEST(IoUringZeroCopyBufferPoolTest, RefillFlushesBeforeRingExhaustion) {
  IoUringZeroCopyBufferPool::Params params = {
      .ring = nullptr,
      .numBuffers = 16,
      .bufferSizeHint = 4096,
      .rqEntries = 8,
      .ifindex = 0,
      .queueId = 0,
  };
  auto pool = IoUringZeroCopyBufferPoolTestHelper::create(params, true);
  IoUringZeroCopyBufferPoolTestHelper helper(*pool);

  io_uring_cqe cqe{};
  cqe.res = 2048;
  io_uring_zcrx_cqe zcqe{};
  vector<unique_ptr<IOBuf>> buffers;
  for (uint32_t i = 0; i < 6; ++i) {
    zcqe.off = i * 4096;
    auto result = pool->getIoBuf(&cqe, &zcqe);
    buffers.push_back(std::move(result.buffer));
  }

  thread returnThread([buffers = std::move(buffers)]() mutable {
    buffers.clear();
  });
  returnThread.join();

  EXPECT_TRUE(helper.isRefillFlushPending());
  EXPECT_EQ(helper.getRingUsedCount(), 6);
  EXPECT_EQ(helper.getRingFreeCount(), 2);

  helper.flushRefillQueueIfNeeded();

  EXPECT_FALSE(helper.isRefillFlushPending());
  EXPECT_EQ(helper.getRingUsedCount(), 0);
  EXPECT_EQ(helper.getRingFreeCount(), 8);
  EXPECT_EQ(helper.getPendingBuffersSize(), 0);
}

TEST(IoUringZeroCopyBufferPoolTest, RequestedFlushDrainsRingBelowThreshold) {
  IoUringZeroCopyBufferPool::Params params = {
      .ring = nullptr,
      .numBuffers = 16,
      .bufferSizeHint = 4096,
      .rqEntries = 8,
      .ifindex = 0,
      .queueId = 0,
  };
  auto pool = IoUringZeroCopyBufferPoolTestHelper::create(params, true);
  IoUringZeroCopyBufferPoolTestHelper helper(*pool);

  io_uring_cqe cqe{};
  cqe.res = 2048;
  io_uring_zcrx_cqe zcqe{};
  for (uint32_t i = 0; i < 5; ++i) {
    zcqe.off = i * helper.getBufferSize();
    pool->getIoBuf(&cqe, &zcqe).buffer.reset();
  }

  EXPECT_FALSE(helper.isRefillFlushPending());
  EXPECT_EQ(helper.getRingUsedCount(), 5);
  EXPECT_EQ(helper.getRingFreeCount(), 3);
  EXPECT_EQ(helper.getPendingBuffersSize(), 0);

  pool->requestRefillQueueFlush();
  EXPECT_TRUE(helper.isRefillFlushPending());

  helper.flushRefillQueueIfNeeded();

  EXPECT_FALSE(helper.isRefillFlushPending());
  EXPECT_EQ(helper.getRingUsedCount(), 0);
  EXPECT_EQ(helper.getRingFreeCount(), 8);
}

TEST(IoUringZeroCopyBufferPoolTest, DiagnosticsSnapshot) {
  IoUringZeroCopyBufferPool::Params params = {
      .ring = nullptr,
      .numBuffers = 8,
      .bufferSizeHint = 4096,
      .rqEntries = 2,
      .ifindex = 0,
      .queueId = 0,
  };
  auto pool = IoUringZeroCopyBufferPoolTestHelper::create(params);
  IoUringZeroCopyBufferPoolTestHelper helper(*pool);
  helper.setZcrxId(7);

  io_uring_cqe cqe{};
  cqe.res = 2048;
  io_uring_zcrx_cqe zcqe{};
  vector<unique_ptr<IOBuf>> buffers;
  for (uint32_t i = 0; i < 3; ++i) {
    zcqe.off = i * 4096;
    buffers.push_back(pool->getIoBuf(&cqe, &zcqe).buffer);
  }
  buffers.clear();

  const auto diagnostics = pool->getDiagnostics();
  EXPECT_EQ(diagnostics.zcrxId, 7);
  EXPECT_EQ(diagnostics.numBuffers, 8);
  EXPECT_EQ(diagnostics.bufferSize, sysconf(_SC_PAGESIZE));
  EXPECT_EQ(diagnostics.refillRingEntries, 2);
  EXPECT_EQ(diagnostics.refillRingHead, 0);
  EXPECT_EQ(diagnostics.refillRingTail, 2);
  EXPECT_EQ(diagnostics.buffersDispensed, 3);
  EXPECT_EQ(diagnostics.pendingBuffers, 1);
}

TEST(IoUringZeroCopyBufferPoolTest, RefillWithLargePendingQueue) {
  IoUringZeroCopyBufferPool::Params params = {
      .ring = nullptr,
      .numBuffers = 200,
      .bufferSizeHint = 4096,
      .rqEntries = 8,
      .ifindex = 0,
      .queueId = 0,
  };
  auto pool = IoUringZeroCopyBufferPoolTestHelper::create(params);
  IoUringZeroCopyBufferPoolTestHelper helper(*pool);

  EXPECT_EQ(helper.getRingUsedCount(), 0);
  EXPECT_EQ(helper.getRingFreeCount(), 8);
  EXPECT_EQ(helper.getPendingBuffersSize(), 0);

  io_uring_cqe cqe{};
  io_uring_zcrx_cqe zcqe{};
  cqe.res = 2048;
  zcqe.off = 0;

  for (int i = 0; i < 150; i++) {
    auto buf = pool->getIoBuf(&cqe, &zcqe).buffer;
    zcqe.off += 4096;
  }

  EXPECT_EQ(helper.getRingUsedCount(), 8);
  EXPECT_EQ(helper.getRingFreeCount(), 0);
  EXPECT_EQ(helper.getPendingBuffersSize(), 142);

  helper.consumeRefillRingEntries(8);

  EXPECT_EQ(helper.getRingUsedCount(), 0);
  EXPECT_EQ(helper.getRingFreeCount(), 8);
  EXPECT_EQ(helper.getPendingBuffersSize(), 142);

  {
    auto buf = pool->getIoBuf(&cqe, &zcqe).buffer;
    zcqe.off += 4096;
  }

  EXPECT_EQ(helper.getRingUsedCount(), 8);
  EXPECT_EQ(helper.getRingFreeCount(), 0);
  EXPECT_EQ(helper.getPendingBuffersSize(), 135);
  helper.consumeRefillRingEntries(8);

  for (int i = 0; i < 10; i++) {
    auto buf = pool->getIoBuf(&cqe, &zcqe).buffer;
    zcqe.off += 4096;
  }

  EXPECT_EQ(helper.getRingUsedCount(), 8);
  EXPECT_EQ(helper.getPendingBuffersSize(), 137);

  helper.consumeRefillRingEntries(8);

  while (helper.getPendingBuffersSize() > 0) {
    auto buf = pool->getIoBuf(&cqe, &zcqe).buffer;
    zcqe.off += 4096;
    if (helper.getRingUsedCount() == 8) {
      helper.consumeRefillRingEntries(8);
    }
  }
  EXPECT_EQ(helper.getPendingBuffersSize(), 0);

  pool.reset();
}

TEST(IoUringZeroCopyBufferPoolTest, SharedBufferGetIoBuf) {
  IoUringZeroCopyBufferPool::Params params = {
      .ring = nullptr,
      .numBuffers = 32,
      .bufferSizeHint = 4096,
      .rqEntries = 8,
      .ifindex = 0,
      .queueId = 0,
  };
  auto pool = IoUringZeroCopyBufferPoolTestHelper::create(params);
  IoUringZeroCopyBufferPoolTestHelper helper(*pool);

  io_uring_cqe cqe{};
  io_uring_zcrx_cqe zcqe{};

  // Dispense 3 IOBufs: two sharing buffers_[0], one on buffers_[1].
  cqe.res = 1500;
  zcqe.off = 0;
  auto buf1 = pool->getIoBuf(&cqe, &zcqe).buffer;

  cqe.res = 800;
  zcqe.off = 1600;
  auto buf2 = pool->getIoBuf(&cqe, &zcqe).buffer;

  EXPECT_EQ(buf1->length(), 1500);
  EXPECT_EQ(buf2->length(), 800);
  EXPECT_EQ(buf2->data() - buf1->data(), 1600);

  cqe.res = 2048;
  zcqe.off = 4096;
  auto buf3 = pool->getIoBuf(&cqe, &zcqe).buffer;
  EXPECT_EQ(buf3->length(), 2048);

  EXPECT_EQ(helper.getRingUsedCount(), 0);
  EXPECT_EQ(helper.getPendingBuffersSize(), 0);

  buf1.reset();
  EXPECT_EQ(helper.getRingUsedCount(), 1);
  EXPECT_EQ(helper.getPendingBuffersSize(), 0);

  buf2.reset();
  EXPECT_EQ(helper.getRingUsedCount(), 2);
  EXPECT_EQ(helper.getPendingBuffersSize(), 0);

  buf3.reset();
  EXPECT_EQ(helper.getRingUsedCount(), 3);
  EXPECT_EQ(helper.getPendingBuffersSize(), 0);
}

TEST(IoUringZeroCopyBufferPoolTest, SharedBufferRefillWithPending) {
  IoUringZeroCopyBufferPool::Params params = {
      .ring = nullptr,
      .numBuffers = 8,
      .bufferSizeHint = 4096,
      .rqEntries = 2,
      .ifindex = 0,
      .queueId = 0,
  };
  auto pool = IoUringZeroCopyBufferPoolTestHelper::create(params);
  IoUringZeroCopyBufferPoolTestHelper helper(*pool);

  io_uring_cqe cqe{};
  io_uring_zcrx_cqe zcqe{};

  // Dispense 3 IOBufs: two sharing buffers_[0], one on buffers_[1].
  // Each uses a distinct length to avoid false positives.
  // This results in 3 returnBuffer() calls when freed.
  cqe.res = 1500;
  zcqe.off = 0;
  auto buf1 = pool->getIoBuf(&cqe, &zcqe).buffer;
  cqe.res = 2048;
  zcqe.off = 4096;
  auto buf2 = pool->getIoBuf(&cqe, &zcqe).buffer;
  cqe.res = 800;
  zcqe.off = 1600;
  auto buf3 = pool->getIoBuf(&cqe, &zcqe).buffer;

  buf1.reset();
  buf2.reset();
  buf3.reset();
  EXPECT_EQ(helper.getRingUsedCount(), 2);
  EXPECT_EQ(helper.getRingFreeCount(), 0);
  EXPECT_EQ(helper.getPendingBuffersSize(), 1);

  helper.consumeRefillRingEntries(2);
  EXPECT_EQ(helper.getRingFreeCount(), 2);

  zcqe.off = 2 * 4096;
  auto buf4 = pool->getIoBuf(&cqe, &zcqe).buffer;
  buf4.reset();

  EXPECT_EQ(helper.getRingUsedCount(), 2);
  EXPECT_EQ(helper.getPendingBuffersSize(), 0);
}

TEST(IoUringZeroCopyBufferPoolTest, CqeIsNotif) {
  IoUringZeroCopyBufferPool::Params params = {
      .ring = nullptr,
      .numBuffers = 8,
      .bufferSizeHint = 4096,
      .rqEntries = 4,
      .ifindex = 0,
      .queueId = 0,
  };
  auto pool = IoUringZeroCopyBufferPoolTestHelper::create(params);
  IoUringZeroCopyBufferPoolTestHelper helper(*pool);

  io_uring_cqe cqe{};

  // Notification sentinel value.
  cqe.user_data = helper.getNotifUserData();
  EXPECT_TRUE(pool->cqeIsNotif(&cqe));

  // Regular CQE with zero user_data.
  cqe.user_data = 0;
  EXPECT_FALSE(pool->cqeIsNotif(&cqe));

  // Regular CQE with arbitrary user_data.
  cqe.user_data = 42;
  EXPECT_FALSE(pool->cqeIsNotif(&cqe));
}

TEST(IoUringZeroCopyBufferPoolTest, SqePrepZcSetsIfqIdx) {
  IoUringZeroCopyBufferPool::Params params = {
      .ring = nullptr,
      .numBuffers = 8,
      .bufferSizeHint = 4096,
      .rqEntries = 4,
      .ifindex = 0,
      .queueId = 0,
  };
  auto pool = IoUringZeroCopyBufferPoolTestHelper::create(params);
  IoUringZeroCopyBufferPoolTestHelper helper(*pool);
  constexpr int kZcrxId = 3;
  helper.setZcrxId(kZcrxId);

  io_uring_sqe sqe{};
  pool->sqePrepZc(&sqe);

  EXPECT_EQ(sqe.zcrx_ifq_idx, kZcrxId);
}

TEST(IoUringZeroCopyBufferPoolTest, NotifStats) {
  IoUringZeroCopyBufferPool::Params params = {
      .ring = nullptr,
      .numBuffers = 8,
      .bufferSizeHint = 4096,
      .rqEntries = 4,
      .ifindex = 0,
      .queueId = 0,
  };
  auto pool = IoUringZeroCopyBufferPoolTestHelper::create(params);
  IoUringZeroCopyBufferPoolTestHelper helper(*pool);
  auto* stats = static_cast<TestZcrxNotifStats*>(helper.getNotifStatsPtr());
  ASSERT_NE(stats, nullptr);

  EXPECT_EQ(pool->getAndResetCopyFallbackCount(), 0);
  EXPECT_EQ(pool->getAndResetCopyFallbackBytes(), 0);

  __atomic_store_n(&stats->copy_count, 5, __ATOMIC_RELAXED);
  __atomic_store_n(&stats->copy_bytes, 65536, __ATOMIC_RELAXED);
  EXPECT_EQ(pool->getAndResetCopyFallbackCount(), 5);
  EXPECT_EQ(pool->getAndResetCopyFallbackBytes(), 65536);

  EXPECT_EQ(pool->getAndResetCopyFallbackCount(), 0);
  EXPECT_EQ(pool->getAndResetCopyFallbackBytes(), 0);

  __atomic_add_fetch(&stats->copy_count, 20, __ATOMIC_RELAXED);
  __atomic_add_fetch(&stats->copy_bytes, 131072, __ATOMIC_RELAXED);
  EXPECT_EQ(pool->getAndResetCopyFallbackCount(), 20);
  EXPECT_EQ(pool->getAndResetCopyFallbackBytes(), 131072);
}

TEST(IoUringZeroCopyBufferPoolTest, NoBufferCount) {
  IoUringZeroCopyBufferPool::Params params = {
      .ring = nullptr,
      .numBuffers = 8,
      .bufferSizeHint = 4096,
      .rqEntries = 4,
      .ifindex = 0,
      .queueId = 0,
  };
  auto pool = IoUringZeroCopyBufferPoolTestHelper::create(params);
  IoUringZeroCopyBufferPoolTestHelper helper(*pool);

  // Initially zero.
  EXPECT_EQ(pool->getAndResetNoBufferCount(), 0);

  // Single increment.
  helper.incNoBufferCount();
  EXPECT_EQ(pool->getAndResetNoBufferCount(), 1);

  // Reset back to zero after read.
  EXPECT_EQ(pool->getAndResetNoBufferCount(), 0);

  // Multiple increments accumulate correctly.
  helper.incNoBufferCount();
  helper.incNoBufferCount();
  helper.incNoBufferCount();
  EXPECT_EQ(pool->getAndResetNoBufferCount(), 3);

  // Resets again after read.
  EXPECT_EQ(pool->getAndResetNoBufferCount(), 0);

  // No-buffer counting works correctly interleaved with buffer ops.
  io_uring_cqe cqe{};
  io_uring_zcrx_cqe zcqe{};
  cqe.res = 2048;
  zcqe.off = 0;

  auto buf1 = pool->getIoBuf(&cqe, &zcqe).buffer;
  helper.incNoBufferCount();
  EXPECT_EQ(pool->getAndResetNoBufferCount(), 1);

  buf1.reset();
  zcqe.off = 4096;
  auto buf2 = pool->getIoBuf(&cqe, &zcqe).buffer;
  helper.incNoBufferCount();
  helper.incNoBufferCount();
  EXPECT_EQ(pool->getAndResetNoBufferCount(), 2);

  // Buffer operations don't affect the counter after reset.
  buf2.reset();
  EXPECT_EQ(pool->getAndResetNoBufferCount(), 0);
}

TEST(IoUringZeroCopyBufferPoolTest, BufferSizeHintFallback) {
  size_t pageSize = sysconf(_SC_PAGESIZE);

  IoUringZeroCopyBufferPool::Params params = {
      .ring = nullptr,
      .numBuffers = 8,
      .bufferSizeHint = 0,
      .rqEntries = 4,
      .ifindex = 0,
      .queueId = 0,
  };

  // Hint too small: falls back to page size.
  params.bufferSizeHint = 512;
  auto pool = IoUringZeroCopyBufferPoolTestHelper::create(params);
  IoUringZeroCopyBufferPoolTestHelper helper(*pool);
  EXPECT_EQ(helper.getBufferSize(), pageSize);

  // Non-power-of-two hint: falls back to page size.
  params.bufferSizeHint = 6000;
  pool = IoUringZeroCopyBufferPoolTestHelper::create(params);
  IoUringZeroCopyBufferPoolTestHelper helper2(*pool);
  EXPECT_EQ(helper2.getBufferSize(), pageSize);

  // Exact page size hint: accepted as-is.
  params.bufferSizeHint = pageSize;
  pool = IoUringZeroCopyBufferPoolTestHelper::create(params);
  IoUringZeroCopyBufferPoolTestHelper helper3(*pool);
  EXPECT_EQ(helper3.getBufferSize(), pageSize);
}

TEST(IoUringZeroCopyBufferPoolTest, RefillMoreThanCapacity) {
  IoUringZeroCopyBufferPool::Params params = {
      .ring = nullptr,
      .numBuffers = 8,
      .bufferSizeHint = 4096,
      .rqEntries = 2,
      .ifindex = 0,
      .queueId = 0,
  };
  auto pool = IoUringZeroCopyBufferPoolTestHelper::create(params);
  IoUringZeroCopyBufferPoolTestHelper helper(*pool);
  EXPECT_EQ(helper.getRingUsedCount(), 0);
  EXPECT_EQ(helper.getRingFreeCount(), 2);

  io_uring_cqe cqe{};
  io_uring_zcrx_cqe zcqe{};

  cqe.res = 2048;
  zcqe.off = 0;
  for (int i = 0; i < 8; i++) {
    auto buf = pool->getIoBuf(&cqe, &zcqe).buffer;
    zcqe.off += 4096;
  }

  EXPECT_EQ(helper.getRingUsedCount(), 2);
  EXPECT_EQ(helper.getRingFreeCount(), 0);
  EXPECT_EQ(helper.getPendingBuffersSize(), 6);

  helper.consumeRefillRingEntries(2);
  zcqe.off = 0;
  for (int i = 0; i < 2; i++) {
    auto buf = pool->getIoBuf(&cqe, &zcqe).buffer;
    zcqe.off += 4096;
  }
  EXPECT_EQ(helper.getRingUsedCount(), 2);
  EXPECT_EQ(helper.getRingFreeCount(), 0);
  EXPECT_EQ(helper.getPendingBuffersSize(), 6);
  pool.reset();
}

// A noDev pool has no netdev driving the page pool, but from the pool's own
// point of view buffers must still be dispensed and recycled exactly as they
// are for a device-backed pool.
TEST(IoUringZeroCopyBufferPoolTest, NoDevRefillRecyclesBuffers) {
  IoUringZeroCopyBufferPool::Params params = {
      .ring = nullptr,
      .numBuffers = 8,
      .bufferSizeHint = 4096,
      .rqEntries = 2,
      .ifindex = 0,
      .queueId = 0,
      .noDev = true,
  };
  auto pool = IoUringZeroCopyBufferPoolTestHelper::create(params);
  IoUringZeroCopyBufferPoolTestHelper helper(*pool);

  io_uring_cqe cqe{};
  io_uring_zcrx_cqe zcqe{};
  cqe.res = 2048;
  zcqe.off = 0;

  auto buf1 = pool->getIoBuf(&cqe, &zcqe).buffer;
  zcqe.off += 4096;
  auto buf2 = pool->getIoBuf(&cqe, &zcqe).buffer;
  zcqe.off += 4096;
  auto buf3 = pool->getIoBuf(&cqe, &zcqe).buffer;

  buf1.reset();
  buf2.reset();
  buf3.reset();

  // Two entries fit the refill ring, the third overflows to the pending queue.
  EXPECT_EQ(helper.getRingUsedCount(), 2);
  EXPECT_EQ(helper.getRingFreeCount(), 0);
  EXPECT_EQ(helper.getPendingBuffersSize(), 1);

  helper.consumeRefillRingEntries(2);
  zcqe.off += 4096;
  auto buf4 = pool->getIoBuf(&cqe, &zcqe).buffer;
  buf4.reset();

  EXPECT_EQ(helper.getRingUsedCount(), 2);
  EXPECT_EQ(helper.getPendingBuffersSize(), 0);
}

// The kernel rejects a noDev registration whose rx_buf_len is not exactly one
// page (io_uring/zcrx.c: !ifq->dev && buf_size_shift != PAGE_SHIFT), so an
// oversized hint has to be clamped even when the kernel reports that it
// supports configurable rx page sizes.
TEST(IoUringZeroCopyBufferPoolTest, NoDevClampsBufferSizeToPageSize) {
  size_t pageSize = sysconf(_SC_PAGESIZE);
  uint32_t largeHint = static_cast<uint32_t>(pageSize * 4);

  IoUringZeroCopyBufferPool::Params params = {
      .ring = nullptr,
      .numBuffers = 8,
      .bufferSizeHint = largeHint,
      .rqEntries = 4,
      .ifindex = 0,
      .queueId = 0,
      .noDev = false,
  };

  // With a device the oversized hint is honoured.
  auto devPool = IoUringZeroCopyBufferPoolTestHelper::create(params);
  IoUringZeroCopyBufferPoolTestHelper devHelper(*devPool);
  EXPECT_EQ(devHelper.getBufferSize(), largeHint);

  // Without one it is clamped down to a single page.
  params.noDev = true;
  auto noDevPool = IoUringZeroCopyBufferPoolTestHelper::create(params);
  IoUringZeroCopyBufferPoolTestHelper noDevHelper(*noDevPool);
  EXPECT_EQ(noDevHelper.getBufferSize(), pageSize);
}

// noDev must not change how a dispensed buffer maps onto the underlying area.
TEST(IoUringZeroCopyBufferPoolTest, NoDevGetBuf) {
  size_t pageSize = sysconf(_SC_PAGESIZE);
  IoUringZeroCopyBufferPool::Params params = {
      .ring = nullptr,
      .numBuffers = 32,
      .bufferSizeHint = static_cast<uint32_t>(pageSize),
      .rqEntries = 8,
      .ifindex = 0,
      .queueId = 0,
      .noDev = true,
  };
  auto pool = IoUringZeroCopyBufferPoolTestHelper::create(params);

  io_uring_cqe cqe{};
  cqe.res = 2048;
  io_uring_zcrx_cqe zcqe{};
  zcqe.off = static_cast<uint64_t>(pageSize);

  auto buf = pool->getIoBuf(&cqe, &zcqe).buffer;
  EXPECT_EQ(buf->length(), 2048);
  EXPECT_TRUE(buf->isSharedOne());
}
