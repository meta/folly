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

#include <memory>
#include <folly/io/IOBuf.h>
#include <folly/io/async/Liburing.h>

#if FOLLY_HAS_LIBURING
FOLLY_PUSH_WARNING
FOLLY_CLANG_DISABLE_WARNING("-Wnested-anon-types")
FOLLY_CLANG_DISABLE_WARNING("-Wzero-length-array")
FOLLY_GCC_DISABLE_WARNING("-Wshadow")
#include <liburing.h> // @manual
FOLLY_POP_WARNING

namespace folly {

class IoUringZeroCopyBufferPoolImpl;
class IoUringBackend;

class IoUringZeroCopyBufferPool {
 public:
  struct Params {
    struct io_uring* ring;
    size_t numBuffers;
    uint32_t bufferSizeHint;
    uint32_t rqEntries;
    uint32_t ifindex;
    uint16_t queueId;
    bool noDev{false};
  };

  struct ExportHandle {
    explicit ExportHandle(
        int zcrxFd, std::shared_ptr<IoUringZeroCopyBufferPoolImpl> impl)
        : zcrxFd_(zcrxFd), impl_(std::move(impl)) {}

    ~ExportHandle() = default;

    ExportHandle(ExportHandle&&) = default;
    ExportHandle& operator=(ExportHandle&&) = default;
    ExportHandle(const ExportHandle&) = delete;
    ExportHandle& operator=(const ExportHandle&) = delete;

    int napiId_{-1};
    int queueId_{-1};

   private:
    friend class IoUringZeroCopyBufferPool;

    int zcrxFd_;
    std::shared_ptr<IoUringZeroCopyBufferPoolImpl> impl_;
  };

  using UniquePtr = std::unique_ptr<IoUringZeroCopyBufferPool>;
  static UniquePtr create(Params params);
  static UniquePtr importHandle(ExportHandle handle, struct io_uring* ring);

  static bool kernelZeroCopyRxSupported();

  ExportHandle exportHandle() const;

  ~IoUringZeroCopyBufferPool();

  struct Result {
    std::unique_ptr<IOBuf> buffer;
    bool isScarce;
  };

  Result getIoBuf(
      const struct io_uring_cqe* cqe,
      const struct io_uring_zcrx_cqe* rcqe) noexcept;

  void processNotificationCqe(const struct io_uring_cqe* cqe) noexcept;

  void sqePrepZc(struct io_uring_sqe* sqe) const noexcept {
    sqe->zcrx_ifq_idx = static_cast<uint32_t>(zcrxId_);
  }

  static constexpr uint64_t kZcrxNotifUserData = ~uint64_t{0x0};
  bool cqeIsNotif(const struct io_uring_cqe* cqe) const noexcept {
    return cqe->user_data == kZcrxNotifUserData;
  }
  uint64_t getAndResetCopyFallbackCount() noexcept;
  uint64_t getAndResetCopyFallbackBytes() noexcept;
  uint32_t getAndResetNoBufferCount() noexcept;

  // Point-in-time queue state for failure diagnostics. Stats contains interval
  // counters and advances their publication baselines when read; this snapshot
  // must not affect those counters.
  struct Diagnostics {
    int zcrxId{-1};
    size_t numBuffers{0};
    uint32_t bufferSize{0};
    uint32_t refillRingEntries{0};
    uint32_t refillRingHead{0};
    uint32_t refillRingTail{0};
    uint32_t buffersDispensed{0};
    size_t pendingBuffers{0};
  };

  Diagnostics getDiagnostics() noexcept;

  void requestRefillQueueFlush() noexcept;

  struct Stats {
    uint64_t copyFallbackCount{0};
    uint64_t copyFallbackBytes{0};
    uint32_t noBufferCount{0};

    auto operator<=>(const Stats&) const = default;
  };

  void getStats(Stats& stats) noexcept {
    stats.copyFallbackCount = getAndResetCopyFallbackCount();
    stats.copyFallbackBytes = getAndResetCopyFallbackBytes();
    stats.noBufferCount = getAndResetNoBufferCount();
  }

 private:
  friend class IoUringBackend;
  explicit IoUringZeroCopyBufferPool(Params params);

  struct TestTag {
    bool flushRq{false};
  };
  explicit IoUringZeroCopyBufferPool(Params params, TestTag);

  explicit IoUringZeroCopyBufferPool(
      ExportHandle handle, struct io_uring* ring);

  IoUringZeroCopyBufferPool(IoUringZeroCopyBufferPool&&) = delete;
  IoUringZeroCopyBufferPool(IoUringZeroCopyBufferPool const&) = delete;
  IoUringZeroCopyBufferPool& operator=(IoUringZeroCopyBufferPool&&) = delete;
  IoUringZeroCopyBufferPool& operator=(IoUringZeroCopyBufferPool const&) =
      delete;

  // For testing
  friend class IoUringZeroCopyBufferPoolTestHelper;
  uint32_t* getHead() const noexcept;
  uint32_t getRingUsedCount() const noexcept;
  uint32_t getRingFreeCount() const noexcept;
  size_t getPendingBuffersSize() const noexcept;
  size_t getBufferSize() const noexcept;
  void* getNotifStatsPtr() const noexcept;
  void incNoBufferCount() noexcept;
  bool isRefillFlushPending() const noexcept;
  void flushRefillQueueIfNeeded() noexcept;

  struct io_uring* ring_{nullptr};
  std::shared_ptr<IoUringZeroCopyBufferPoolImpl> impl_;
  int zcrxId_{-1};
  int zcrxFd_{-1};
};

} // namespace folly

#endif
