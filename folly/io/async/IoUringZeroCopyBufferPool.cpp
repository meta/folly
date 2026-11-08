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

#include <folly/io/async/IoUringZeroCopyBufferPool.h>

#include <mutex>
#include <stdexcept>

#include <folly/Conv.h>
#include <folly/GLog.h>
#include <folly/String.h>
#include <folly/container/DynamicRingQueue.h>
#include <folly/lang/Align.h>
#include <folly/portability/SysMman.h>
#include <folly/synchronization/DistributedMutex.h>

#if FOLLY_HAS_LIBURING_ZCRX

// TODO: T264228475: remove this once liburing/uapi headers are updated.
#define ZCRX_REG_NODEV 2
#define ZCRX_NOTIF_NO_BUFFER 0
#define ZCRX_NOTIF_NO_BUFFER_MASK (1 << ZCRX_NOTIF_NO_BUFFER)
#define ZCRX_FEATURE_NOTIFICATION (1 << 1)
#define ZCRX_CTRL_ARM_NOTIFICATION 2
#define IO_URING_QUERY_ZCRX_NOTIF 3
#define ZCRX_NOTIF_DESC_FLAG_STATS (1U << 0)

struct io_uring_query_zcrx_notif {
  uint32_t notif_flags;
  uint32_t notif_stats_size;
  uint32_t notif_stats_off_alignment;
  uint32_t __resv1;
  uint64_t __resv2[4];
};

struct zcrx_notification_desc {
  uint64_t user_data;
  uint32_t type_mask;
  uint32_t flags;
  uint64_t stats_offset;
  uint64_t __resv2[9];
};

struct zcrx_ctrl_arm_notif {
  uint32_t notif_type;
  uint32_t __resv[11];
};

struct t_zcrx_ctrl {
  uint32_t zcrx_id;
  uint32_t op; /* see enum zcrx_ctrl_op */
  uint64_t __resv[2];

  union {
    struct zcrx_ctrl_export zc_export;
    struct zcrx_ctrl_flush_rq zc_flush;
    struct zcrx_ctrl_arm_notif zc_arm;
  };
};

struct io_uring_zcrx_notif_stats {
  uint64_t copy_count;
  uint64_t copy_bytes;
};

namespace folly {

namespace {

constexpr size_t kScarcityReserveDivisor = 8;
constexpr uint32_t kRefillFlushFreeEntriesDivisor = 4;

} // namespace

class IoUringZeroCopyBufferPoolImpl {
 public:
  friend class IoUringZeroCopyBufferPool;

  struct Buffer {
    uint64_t off;
    uint32_t len;
    IoUringZeroCopyBufferPoolImpl* pool;
  };

  struct SupportedFeatures {
    bool importRing{false};
    bool exportRing{false};
    bool pageSize{false};
    bool flushRq{false};
    bool noDev{false};
    bool notification{false};
    uint32_t notifFlags{0};
    uint32_t notifStatsSize{0};
    uint32_t notifStatsOffAlignment{0};
  };

  explicit IoUringZeroCopyBufferPoolImpl(
      IoUringZeroCopyBufferPool::Params params,
      bool test,
      bool flushRqForTest = false);

  ~IoUringZeroCopyBufferPoolImpl();

  IoUringZeroCopyBufferPoolImpl(IoUringZeroCopyBufferPoolImpl&&) = delete;
  IoUringZeroCopyBufferPoolImpl(IoUringZeroCopyBufferPoolImpl const&) = delete;
  IoUringZeroCopyBufferPoolImpl& operator=(IoUringZeroCopyBufferPoolImpl&&) =
      delete;
  IoUringZeroCopyBufferPoolImpl& operator=(
      IoUringZeroCopyBufferPoolImpl const&) = delete;

  void destroy() noexcept;

  IoUringZeroCopyBufferPool::Result getIoBuf(
      const struct io_uring_cqe* cqe,
      const struct io_uring_zcrx_cqe* rcqe) noexcept;
  void returnBuffer(Buffer* buf) noexcept;

  // For testing
  uint32_t* getHead() const noexcept { return rqRing_.khead; }
  uint32_t getRingUsedCount() const noexcept {
    return rqTail_ - io_uring_smp_load_acquire(rqRing_.khead);
  }
  uint32_t getRingFreeCount() const noexcept {
    return rqEntries_ - getRingUsedCount();
  }
  size_t getPendingBuffersSize() const noexcept {
    return pendingBuffers_.size();
  }
  size_t getBufferSize() const noexcept { return bufferSize_; }
  uint64_t getAndResetCopyFallbackCount() noexcept {
    if (!notifStats_) {
      return 0;
    }
    return getAndResetNotifStat(notifStats_->copy_count, copyCount_);
  }
  uint64_t getAndResetCopyFallbackBytes() noexcept {
    if (!notifStats_) {
      return 0;
    }
    return getAndResetNotifStat(notifStats_->copy_bytes, copyBytes_);
  }
  uint32_t getAndResetNoBufferCount() noexcept {
    auto val = noBufferCount_;
    noBufferCount_ = 0;
    return val;
  }
  IoUringZeroCopyBufferPool::Diagnostics getDiagnostics() noexcept {
    std::unique_lock lock{mutex_};
    return {
        .numBuffers = buffers_.size(),
        .bufferSize = bufferSize_,
        .refillRingEntries = rqEntries_,
        .refillRingHead = io_uring_smp_load_acquire(rqRing_.khead),
        .refillRingTail = rqTail_,
        .buffersDispensed = bufDispensed_.load(std::memory_order_relaxed),
        .pendingBuffers = pendingBuffers_.size(),
    };
  }

 private:
  void mapMemory();
  void checkZcRxFeatures();
  uint32_t getNotifStatSize();
  void initialRegister(
      const IoUringZeroCopyBufferPool::Params& params, uint32_t pageSize);
  void delayedDestroy(uint32_t refs) noexcept;
  void refillBuffer(Buffer* buffer) noexcept;
  void requestRefillQueueFlush() noexcept;
  bool isRefillFlushPending() const noexcept {
    return refillFlushPending_.load(std::memory_order_acquire);
  }
  void flushRefillQueueIfNeeded(
      struct io_uring* ring, uint32_t zcrxId) noexcept;

  void incNoBufferCount() noexcept { noBufferCount_ += 1; }
  uint64_t getAndResetNotifStat(uint64_t& stat, uint64_t& prev) noexcept {
    uint64_t cur = IO_URING_READ_ONCE(stat);
    uint64_t delta = cur - prev;
    prev = cur;
    return delta;
  }

  struct io_uring* ring_{nullptr};
  uint32_t bufferSize_{0};
  uint32_t rqEntries_{0};
  bool noDev_{false};

  void* bufArea_{nullptr};
  size_t bufAreaSize_{0};
  std::vector<Buffer> buffers_;
  void* rqRingArea_{nullptr};
  size_t rqRingAreaSize_{0};
  size_t mmapSize_{0};
  struct io_uring_zcrx_rq rqRing_{};
  uint64_t rqAreaToken_{0};
  uint32_t rqTail_{0};
  unsigned rqMask_{0};
  uint32_t id_{0};
  std::atomic<uint32_t> bufDispensed_{0};
  std::atomic<bool> refillFlushPending_{false};

  folly::DistributedMutex mutex_;
  std::atomic<bool> wantsShutdown_{false};
  uint32_t shutdownReferences_{0};
  DynamicRingQueue<Buffer*> pendingBuffers_;
  SupportedFeatures supportedFeatures_{};
  io_uring_zcrx_notif_stats* notifStats_{nullptr};
  uint32_t noBufferCount_{0};
  uint64_t copyCount_{0};
  uint64_t copyBytes_{0};
};

struct ImplDeleter {
  void operator()(IoUringZeroCopyBufferPoolImpl* impl) const noexcept {
    if (impl) {
      impl->destroy();
    }
  }
};

namespace {
struct RefillRingHeader {
  uint32_t head;
  uint32_t tail;
};

size_t getRefillRingSize(size_t rqEntries) {
  size_t size = rqEntries * sizeof(struct io_uring_zcrx_rqe);
  // Include space for the header (head/tail/etc.)
  size_t pageSize = sysconf(_SC_PAGESIZE);
  size += pageSize;
  return folly::align_ceil(size, pageSize);
}

template <typename QueryType>
bool ioUringQuery(uint32_t queryOp, QueryType& out) {
  struct io_uring_query_hdr hdr{};
  hdr.query_op = queryOp;
  hdr.query_data = reinterpret_cast<__u64>(&out);
  hdr.size = sizeof(QueryType);

  int ret = io_uring_register(
      static_cast<unsigned int>(-1), IORING_REGISTER_QUERY, &hdr, 0);
  return ret == 0 && hdr.result == 0;
}

constexpr uint64_t kBufferMask = (1ULL << IORING_ZCRX_AREA_SHIFT) - 1;
constexpr uint32_t kMTU = 4096;

} // namespace

uint32_t IoUringZeroCopyBufferPoolImpl::getNotifStatSize() {
  return supportedFeatures_.notifStatsSize;
}

void IoUringZeroCopyBufferPoolImpl::checkZcRxFeatures() {
  struct io_uring_query_zcrx zcrxQuery{};
  if (!ioUringQuery(IO_URING_QUERY_ZCRX, zcrxQuery)) {
    return;
  }

  supportedFeatures_.importRing = zcrxQuery.register_flags & ZCRX_REG_IMPORT;
  supportedFeatures_.noDev = zcrxQuery.register_flags & ZCRX_REG_NODEV;
  supportedFeatures_.exportRing = zcrxQuery.nr_ctrl_opcodes > ZCRX_CTRL_EXPORT;
  supportedFeatures_.flushRq = zcrxQuery.nr_ctrl_opcodes > ZCRX_CTRL_FLUSH_RQ;
  supportedFeatures_.pageSize = zcrxQuery.features & ZCRX_FEATURE_RX_PAGE_SIZE;
  supportedFeatures_.notification =
      zcrxQuery.features & ZCRX_FEATURE_NOTIFICATION;
  if (!supportedFeatures_.notification) {
    return;
  }

  struct io_uring_query_zcrx_notif notifQuery{};
  if (ioUringQuery(IO_URING_QUERY_ZCRX_NOTIF, notifQuery)) {
    supportedFeatures_.notifFlags = notifQuery.notif_flags;
    supportedFeatures_.notifStatsSize = notifQuery.notif_stats_size;
    supportedFeatures_.notifStatsOffAlignment =
        notifQuery.notif_stats_off_alignment;
  }
}

bool IoUringZeroCopyBufferPool::kernelZeroCopyRxSupported() {
  struct io_uring_query_zcrx zcrxQuery{};
  return ioUringQuery(IO_URING_QUERY_ZCRX, zcrxQuery);
}

IoUringZeroCopyBufferPoolImpl::IoUringZeroCopyBufferPoolImpl(
    IoUringZeroCopyBufferPool::Params params, bool test, bool flushRqForTest)
    : ring_(params.ring),
      bufferSize_(params.bufferSizeHint),
      rqEntries_(params.rqEntries),
      noDev_(params.noDev),
      buffers_(params.numBuffers) {
  uint32_t pageSize = static_cast<uint32_t>(sysconf(_SC_PAGESIZE));
  if (noDev_ || !bufferSize_) {
    bufferSize_ = pageSize;
  }

  if (bufferSize_ < pageSize || !isPowTwo(bufferSize_)) {
    LOG_FIRST_N(WARNING, 1)
        << "IoUringZeroCopyBufferPool: invalid requested buffer size "
        << bufferSize_ << ", adjusting to " << pageSize;
    bufferSize_ = pageSize;
  }
  if (!test) {
    checkZcRxFeatures();
    if (noDev_ && !supportedFeatures_.noDev) {
      throw std::runtime_error(
          "IoUringZeroCopyBufferPool: kernel does not support ZCRX_REG_NODEV");
    }
    if (noDev_ && !supportedFeatures_.flushRq) {
      throw std::runtime_error(
          "IoUringZeroCopyBufferPool: noDev zcrx requires ZCRX_CTRL_FLUSH_RQ support");
    }
  } else {
    supportedFeatures_.flushRq = flushRqForTest;
    supportedFeatures_.notification = true;
    supportedFeatures_.pageSize = true;
    supportedFeatures_.notifStatsSize =
        sizeof(struct io_uring_zcrx_notif_stats);
    supportedFeatures_.notifStatsOffAlignment =
        alignof(struct io_uring_zcrx_notif_stats);
  }
  auto refillRingSize = getRefillRingSize(rqEntries_);
  rqRingAreaSize_ =
      refillRingSize + folly::align_ceil(getNotifStatSize(), pageSize);

  if (bufferSize_ > pageSize && !supportedFeatures_.pageSize) {
    LOG_FIRST_N(WARNING, 1)
        << "IoUringZeroCopyBufferPool: zcrx does not support buffer size, adjusting size to "
        << pageSize;
    bufferSize_ = pageSize;
  }

  bufAreaSize_ = params.numBuffers * bufferSize_;
  mapMemory();

  pendingBuffers_ = DynamicRingQueue<Buffer*>(
      static_cast<uint32_t>(params.numBuffers * (bufferSize_ / kMTU)));

  if (!test) {
    initialRegister(params, pageSize);
  } else {
    // rqRing_ is normally set up using information that the kernel fills in via
    // io_uring_register_ifq(). Unit tests do not do this, so fake it.
    rqRing_.khead = reinterpret_cast<uint32_t*>(
        (static_cast<char*>(rqRingArea_) + offsetof(RefillRingHeader, head)));
    rqRing_.ktail = reinterpret_cast<uint32_t*>(
        (static_cast<char*>(rqRingArea_) + offsetof(RefillRingHeader, tail)));
    rqRing_.rqes = reinterpret_cast<struct io_uring_zcrx_rqe*>(
        static_cast<char*>(rqRingArea_) + sizeof(RefillRingHeader));
    rqRing_.rq_tail = 0;
    rqRing_.ring_entries = rqEntries_;

    notifStats_ = reinterpret_cast<struct io_uring_zcrx_notif_stats*>(
        static_cast<char*>(rqRingArea_) + refillRingSize);
  }

  // Initialize buffers after registering with zcrx since we can fallback to a
  // lower buffer size.
  DCHECK(!buffers_.empty());
  for (size_t i = 0; i < params.numBuffers; i++) {
    auto& buf = buffers_[i];
    buf.pool = this;
    buf.off = i * bufferSize_;
    buf.len = bufferSize_;
  }
}

IoUringZeroCopyBufferPoolImpl::~IoUringZeroCopyBufferPoolImpl() {
  ::munmap(bufArea_, mmapSize_);
}

void IoUringZeroCopyBufferPoolImpl::destroy() noexcept {
  std::unique_lock lock{mutex_};
  const auto dispensed = bufDispensed_.load(std::memory_order_relaxed);
  DCHECK(dispensed >= rqTail_);
  auto remaining = dispensed - rqTail_;
  // Drain refs in overflow queue
  remaining -= pendingBuffers_.size();
  shutdownReferences_ = remaining;
  wantsShutdown_ = true;
  lock.unlock();
  delayedDestroy(remaining);
}

IoUringZeroCopyBufferPool::Result IoUringZeroCopyBufferPoolImpl::getIoBuf(
    const struct io_uring_cqe* cqe,
    const struct io_uring_zcrx_cqe* rcqe) noexcept {
  // By the time the pool is being destroyed, IoUringBackend has already drained
  // all requests so there won't be any more calls to getIoBuf().
  DCHECK(!wantsShutdown_);
  auto offset = (rcqe->off & kBufferMask);
  auto length = static_cast<uint32_t>(cqe->res);

  auto freeFn = [](void*, void* userData) {
    auto buffer =
        reinterpret_cast<IoUringZeroCopyBufferPoolImpl::Buffer*>(userData);
    DCHECK(buffer->pool);
    buffer->pool->returnBuffer(buffer);
  };

  uint32_t i = static_cast<uint32_t>(offset / bufferSize_);
  auto ret = IOBuf::takeOwnership(
      static_cast<char*>(bufArea_) + offset,
      length,
      length,
      freeFn,
      &buffers_[i]);
  // The underlying buffers are shared between userspace and kernel. The IOBufs
  // only 'wrap' the data and is read-only. Mark as shared such that downstream
  // users of this IOBuf do not try to destructively modify the data.
  ret->markExternallySharedOne();
  const auto returned = io_uring_smp_load_acquire(rqRing_.ktail);
  const auto dispensed =
      bufDispensed_.fetch_add(1, std::memory_order_relaxed) + 1;
  const auto numBuffers = buffers_.size();
  const auto reserveBuffers =
      (numBuffers + kScarcityReserveDivisor - 1) / kScarcityReserveDivisor;
  return {
      .buffer = std::move(ret),
      .isScarce = dispensed - returned >= numBuffers - reserveBuffers,
  };
}

void IoUringZeroCopyBufferPoolImpl::mapMemory() {
  // Align the mmap size to a multiple of 2MB to ensure that we are using
  // the kernel THP mmap path.
  // TODO: Detect smallest huge page size and align to that.
  mmapSize_ = folly::align_ceil(bufAreaSize_ + rqRingAreaSize_, 1 << 21);

  bufArea_ = ::mmap(
      nullptr,
      mmapSize_,
      PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE,
      -1,
      0);
  if (bufArea_ == MAP_FAILED) {
    throw std::runtime_error(
        folly::to<std::string>(
            "IoUringZeroCopyBufferPool failed to mmap size ", mmapSize_));
  }

  rqRingArea_ = static_cast<char*>(bufArea_) + bufAreaSize_;

  // Try to promote the buffers to huge pages, if that fails, it isn't
  // fatal, io_uring_register_ifq() will probably fail with a large buffer
  // size but will fallback to the default system page size.
  if (madvise(bufArea_, mmapSize_, MADV_HUGEPAGE) != 0) {
    LOG(WARNING) << "madvise(MADV_HUGEPAGE) failed: " << folly::errnoStr(errno);
  }
}

void IoUringZeroCopyBufferPoolImpl::initialRegister(
    const IoUringZeroCopyBufferPool::Params& params, uint32_t pageSize) {
  struct io_uring_region_desc regionReg{};
  regionReg.user_addr = reinterpret_cast<uint64_t>(rqRingArea_);
  regionReg.size = rqRingAreaSize_;
  regionReg.flags = IORING_MEM_REGION_TYPE_USER;

  struct io_uring_zcrx_area_reg areaReg{};
  areaReg.addr = reinterpret_cast<uint64_t>(bufArea_);
  areaReg.len = bufAreaSize_;

  struct io_uring_zcrx_ifq_reg ifqReg{};
  ifqReg.rq_entries = rqEntries_;
  ifqReg.area_ptr = reinterpret_cast<uint64_t>(&areaReg);
  ifqReg.region_ptr = reinterpret_cast<uint64_t>(&regionReg);

  if (noDev_) {
    ifqReg.if_idx = 0;
    ifqReg.if_rxq = 0;
    ifqReg.flags = ZCRX_REG_NODEV;
  } else {
    ifqReg.if_idx = params.ifindex;
    ifqReg.if_rxq = params.queueId;
  }

  struct zcrx_notification_desc notifDesc{};
  if (supportedFeatures_.notification) {
    if ((supportedFeatures_.notifFlags & ZCRX_NOTIF_NO_BUFFER_MASK) != 0) {
      notifDesc.user_data = IoUringZeroCopyBufferPool::kZcrxNotifUserData;
      notifDesc.type_mask = ZCRX_NOTIF_NO_BUFFER_MASK;
    }

    if (supportedFeatures_.notifStatsSize && !noDev_) {
      notifDesc.flags = ZCRX_NOTIF_DESC_FLAG_STATS;
      notifDesc.stats_offset = getRefillRingSize(params.rqEntries);
    }

    ifqReg.__resv[0] = reinterpret_cast<uint64_t>(&notifDesc);
  }

  if (bufferSize_ > pageSize) {
    ifqReg.rx_buf_len = bufferSize_;
  }

  auto ret = io_uring_register_ifq(ring_, &ifqReg);
  if (bufferSize_ > pageSize && (ret == -EINVAL || ret == -ERANGE)) {
    LOG(WARNING)
        << "IoUringZeroCopyBufferPool failed io_uring_register_ifq with rx_buf_len size "
        << ifqReg.rx_buf_len << ", falling back to default page size";

    ::munmap(bufArea_, mmapSize_);
    bufAreaSize_ = params.numBuffers * pageSize;
    bufferSize_ = pageSize;
    mapMemory();

    ifqReg.rx_buf_len = 0;
    areaReg.addr = reinterpret_cast<uint64_t>(bufArea_);
    areaReg.len = bufAreaSize_;
    regionReg.user_addr = reinterpret_cast<uint64_t>(rqRingArea_);

    ret = io_uring_register_ifq(ring_, &ifqReg);
  }
  if (ret) {
    ::munmap(bufArea_, mmapSize_);
    throw std::runtime_error(
        fmt::format(
            "IoUringZeroCopyBufferPool failed io_uring_register_ifq: {} {}",
            ret,
            folly::errnoStr(-ret)));
  }

  rqRing_.khead = reinterpret_cast<uint32_t*>(
      (static_cast<char*>(rqRingArea_) + ifqReg.offsets.head));
  rqRing_.ktail = reinterpret_cast<uint32_t*>(
      (static_cast<char*>(rqRingArea_) + ifqReg.offsets.tail));
  rqRing_.rqes = reinterpret_cast<struct io_uring_zcrx_rqe*>(
      static_cast<char*>(rqRingArea_) + ifqReg.offsets.rqes);
  rqRing_.rq_tail = 0;
  rqRing_.ring_entries = ifqReg.rq_entries;

  rqEntries_ = ifqReg.rq_entries;
  rqAreaToken_ = areaReg.rq_area_token;
  rqMask_ = ifqReg.rq_entries - 1;
  id_ = ifqReg.zcrx_id;

  if (notifDesc.stats_offset > 0) {
    notifStats_ = reinterpret_cast<io_uring_zcrx_notif_stats*>(
        static_cast<char*>(rqRingArea_) + notifDesc.stats_offset);
  }
}

void IoUringZeroCopyBufferPoolImpl::refillBuffer(Buffer* buffer) noexcept {
  uint32_t myTail = rqTail_++;

  struct io_uring_zcrx_rqe* rqe = &rqRing_.rqes[myTail & rqMask_];
  rqe->off = (buffer->off & ~IORING_ZCRX_AREA_MASK) | rqAreaToken_;
  rqe->len = buffer->len;
}

void IoUringZeroCopyBufferPoolImpl::returnBuffer(Buffer* buffer) noexcept {
  std::unique_lock lock{mutex_};
  if (wantsShutdown_) [[unlikely]] {
    auto refs = --shutdownReferences_;
    lock.unlock();
    delayedDestroy(refs);
    return;
  }

  uint32_t startTail = rqTail_;
  uint32_t freeEntries = getRingFreeCount();

  if (freeEntries > 0) {
    refillBuffer(buffer);
    freeEntries--;
  } else {
    pendingBuffers_.push(buffer);

    requestRefillQueueFlush();
    return;
  }

  uint32_t refillCount =
      std::min<uint64_t>(pendingBuffers_.size(), freeEntries);
  for (uint32_t i = 0; i < refillCount; i++) {
    refillBuffer(pendingBuffers_.pop());
  }
  freeEntries -= refillCount;

  if (rqTail_ != startTail) {
    io_uring_smp_store_release(rqRing_.ktail, rqTail_);
    const bool shouldFlush = supportedFeatures_.flushRq &&
        (noDev_ || freeEntries <= rqEntries_ / kRefillFlushFreeEntriesDivisor);
    if (shouldFlush) [[unlikely]] {
      requestRefillQueueFlush();
    }
  }
}

void IoUringZeroCopyBufferPoolImpl::requestRefillQueueFlush() noexcept {
  if (supportedFeatures_.flushRq) {
    refillFlushPending_.store(true, std::memory_order_release);
  }
}

void IoUringZeroCopyBufferPoolImpl::flushRefillQueueIfNeeded(
    struct io_uring* ring, uint32_t zcrxId) noexcept {
  if (!refillFlushPending_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

  if (!supportedFeatures_.flushRq) {
    return;
  }

  if (ring == nullptr) [[unlikely]] {
    io_uring_smp_store_release(rqRing_.khead, rqTail_);
    return;
  }

  struct zcrx_ctrl ctrl{};
  ctrl.zcrx_id = zcrxId;
  ctrl.op = ZCRX_CTRL_FLUSH_RQ;
  const auto ret =
      io_uring_register(ring->ring_fd, IORING_REGISTER_ZCRX_CTRL, &ctrl, 0);
  if (ret < 0) [[unlikely]] {
    FB_LOG_EVERY_MS(WARNING, 60000)
        << "Zero copy receive refill queue flush failed: "
        << folly::errnoStr(-ret);
  }
}

void IoUringZeroCopyBufferPoolImpl::delayedDestroy(uint32_t refs) noexcept {
  if (refs == 0) {
    delete this;
  }
}

/*static*/ IoUringZeroCopyBufferPool::UniquePtr
IoUringZeroCopyBufferPool::create(Params params) {
  return IoUringZeroCopyBufferPool::UniquePtr(
      new IoUringZeroCopyBufferPool(params));
}

/*static*/ IoUringZeroCopyBufferPool::UniquePtr
IoUringZeroCopyBufferPool::importHandle(
    ExportHandle handle, struct io_uring* ring) {
  return IoUringZeroCopyBufferPool::UniquePtr(
      new IoUringZeroCopyBufferPool(std::move(handle), ring));
}

IoUringZeroCopyBufferPool::IoUringZeroCopyBufferPool(Params params)
    : ring_(params.ring),
      impl_(new IoUringZeroCopyBufferPoolImpl(params, false), ImplDeleter{}) {
  zcrxId_ = impl_->id_;
}

IoUringZeroCopyBufferPool::IoUringZeroCopyBufferPool(Params params, TestTag tag)
    : ring_(params.ring),
      impl_(
          new IoUringZeroCopyBufferPoolImpl(params, true, tag.flushRq),
          ImplDeleter{}) {}

IoUringZeroCopyBufferPool::IoUringZeroCopyBufferPool(
    ExportHandle handle, struct io_uring* ring)
    : ring_(ring) {
  if (!handle.impl_->supportedFeatures_.importRing) {
    throw std::runtime_error(
        "IoUringZeroCopyBufferPool import failed: kernel does not support import");
  }

  struct io_uring_zcrx_ifq_reg ifqReg{};
  ifqReg.if_idx = static_cast<uint32_t>(handle.zcrxFd_);
  ifqReg.flags = ZCRX_REG_IMPORT;

  auto ret = io_uring_register_ifq(ring_, &ifqReg);
  if (ret) {
    throw std::runtime_error(
        fmt::format(
            "IoUringZeroCopyBufferPool failed io_uring_register_ifq: {} {}",
            ret,
            folly::errnoStr(-ret)));
  }

  zcrxId_ = ifqReg.zcrx_id;
  zcrxFd_ = handle.zcrxFd_;
  impl_ = std::move(handle.impl_);
}

IoUringZeroCopyBufferPool::~IoUringZeroCopyBufferPool() {
  if (zcrxFd_ >= 0) {
    close(zcrxFd_);
  }
}

IoUringZeroCopyBufferPool::ExportHandle
IoUringZeroCopyBufferPool::exportHandle() const {
  if (impl_->ring_ != ring_) {
    throw std::runtime_error(
        "Cannot export a handle from a non-owning IoUringZeroCopyBufferPool");
  }

  if (!impl_->supportedFeatures_.exportRing) {
    throw std::runtime_error(
        "IoUringZeroCopyBufferPool export failed: kernel does not support export");
  }

  struct zcrx_ctrl ctrl{};
  ctrl.zcrx_id = impl_->id_;
  ctrl.op = ZCRX_CTRL_EXPORT;
  auto ret =
      io_uring_register(ring_->ring_fd, IORING_REGISTER_ZCRX_CTRL, &ctrl, 0);
  if (ret < 0) {
    throw std::runtime_error(
        fmt::format(
            "IoUringZeroCopyBufferPool export failed io_uring_register: {} {}",
            ret,
            folly::errnoStr(-ret)));
  }
  auto zcrxFd = static_cast<int>(ctrl.zc_export.zcrx_fd);

  return ExportHandle(zcrxFd, impl_);
}

IoUringZeroCopyBufferPool::Result IoUringZeroCopyBufferPool::getIoBuf(
    const struct io_uring_cqe* cqe,
    const struct io_uring_zcrx_cqe* rcqe) noexcept {
  return impl_->getIoBuf(cqe, rcqe);
}

uint32_t* IoUringZeroCopyBufferPool::getHead() const noexcept {
  return impl_->getHead();
}

uint32_t IoUringZeroCopyBufferPool::getRingUsedCount() const noexcept {
  return impl_->getRingUsedCount();
}

uint32_t IoUringZeroCopyBufferPool::getRingFreeCount() const noexcept {
  return impl_->getRingFreeCount();
}

size_t IoUringZeroCopyBufferPool::getPendingBuffersSize() const noexcept {
  return impl_->getPendingBuffersSize();
}

size_t IoUringZeroCopyBufferPool::getBufferSize() const noexcept {
  return impl_->getBufferSize();
}

void IoUringZeroCopyBufferPool::processNotificationCqe(
    const struct io_uring_cqe* cqe) noexcept {
  auto notifType = cqe->res;

  if (notifType == ZCRX_NOTIF_NO_BUFFER) {
    impl_->incNoBufferCount();
  }

  struct t_zcrx_ctrl ctrl{};
  ctrl.zcrx_id = impl_->id_;
  ctrl.op = ZCRX_CTRL_ARM_NOTIFICATION;
  auto* armNotif = reinterpret_cast<struct zcrx_ctrl_arm_notif*>(&ctrl.zc_arm);
  armNotif->notif_type = notifType;
  auto ret =
      io_uring_register(ring_->ring_fd, IORING_REGISTER_ZCRX_CTRL, &ctrl, 0);
  if (ret < 0) {
    LOG_FIRST_N(WARNING, 1)
        << "IoUringZeroCopyBufferPool failed to re-arm ZCRX notification: "
        << ret << " " << folly::errnoStr(-ret);
  }
}

uint64_t IoUringZeroCopyBufferPool::getAndResetCopyFallbackCount() noexcept {
  return impl_->getAndResetCopyFallbackCount();
}

uint64_t IoUringZeroCopyBufferPool::getAndResetCopyFallbackBytes() noexcept {
  return impl_->getAndResetCopyFallbackBytes();
}

uint32_t IoUringZeroCopyBufferPool::getAndResetNoBufferCount() noexcept {
  return impl_->getAndResetNoBufferCount();
}

IoUringZeroCopyBufferPool::Diagnostics
IoUringZeroCopyBufferPool::getDiagnostics() noexcept {
  auto diagnostics = impl_->getDiagnostics();
  diagnostics.zcrxId = zcrxId_;
  return diagnostics;
}

void* IoUringZeroCopyBufferPool::getNotifStatsPtr() const noexcept {
  return impl_->notifStats_;
}

void IoUringZeroCopyBufferPool::incNoBufferCount() noexcept {
  impl_->incNoBufferCount();
}

bool IoUringZeroCopyBufferPool::isRefillFlushPending() const noexcept {
  return impl_->isRefillFlushPending();
}

void IoUringZeroCopyBufferPool::flushRefillQueueIfNeeded() noexcept {
  impl_->flushRefillQueueIfNeeded(ring_, static_cast<uint32_t>(zcrxId_));
}

} // namespace folly

#elif FOLLY_HAS_LIBURING

// TODO: T294823393: remove these stand-ins along with FOLLY_HAS_LIBURING_ZCRX.

namespace folly {

bool IoUringZeroCopyBufferPool::kernelZeroCopyRxSupported() {
  return false;
}

/* static */ IoUringZeroCopyBufferPool::UniquePtr
IoUringZeroCopyBufferPool::create(Params) {
  return nullptr;
}

/* static */ IoUringZeroCopyBufferPool::UniquePtr
IoUringZeroCopyBufferPool::importHandle(ExportHandle, struct io_uring*) {
  return nullptr;
}

IoUringZeroCopyBufferPool::ExportHandle
IoUringZeroCopyBufferPool::exportHandle() const {
  throw std::runtime_error(
      "io_uring zero-copy receive is not supported by these liburing headers");
}

IoUringZeroCopyBufferPool::~IoUringZeroCopyBufferPool() = default;

IoUringZeroCopyBufferPool::Result IoUringZeroCopyBufferPool::getIoBuf(
    const struct io_uring_cqe*, const struct io_uring_zcrx_cqe*) noexcept {
  return {nullptr, false};
}

void IoUringZeroCopyBufferPool::processNotificationCqe(
    const struct io_uring_cqe*) noexcept {}

uint64_t IoUringZeroCopyBufferPool::getAndResetCopyFallbackCount() noexcept {
  return 0;
}

uint64_t IoUringZeroCopyBufferPool::getAndResetCopyFallbackBytes() noexcept {
  return 0;
}

uint32_t IoUringZeroCopyBufferPool::getAndResetNoBufferCount() noexcept {
  return 0;
}

IoUringZeroCopyBufferPool::Diagnostics
IoUringZeroCopyBufferPool::getDiagnostics() noexcept {
  return {};
}

} // namespace folly

#endif
