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

#include <algorithm>
#include <array>
#include <new>
#include <optional>
#include <utility>

#include <folly/Conv.h>
#include <folly/String.h>
#include <folly/lang/Align.h>
#include <folly/portability/SysMman.h>

#if FOLLY_HAS_LIBURING

namespace {
constexpr uint32_t kMinBufferSize = 32;
constexpr uint32_t kHugePageSizeBytes = 1024 * 1024 * 2;
constexpr uint32_t kMaxRingRefillEntries = 32768;
constexpr uint32_t kInitialAreaCount = 2;
constexpr uint32_t kRingRefillFreeThreshold = 256;
constexpr uint32_t kMaxAreaCount = 64;
} // namespace

namespace folly {

void IoUringDynamicProvidedBufferRing::checkInvariants() {
  // This object is carefully packed into two cache lines holding the hottest
  // fields accessed during hot code, i.e. getIoBuf().
  static_assert(
      sizeof(IoUringDynamicProvidedBufferRing) ==
      2 * folly::hardware_constructive_interference_size);

  static_assert(
      alignof(IoUringDynamicProvidedBufferRing) ==
      folly::hardware_constructive_interference_size);
}

IoUringDynamicProvidedBufferRing::UniquePtr
IoUringDynamicProvidedBufferRing::create(io_uring* ioRingPtr, Options options) {
  return IoUringDynamicProvidedBufferRing::UniquePtr(
      new IoUringDynamicProvidedBufferRing(ioRingPtr, options));
}

void IoUringDynamicProvidedBufferRing::mapRing() {
  auto memSize = ringMemSize();
  off_t offset = IORING_OFF_PBUF_RING |
      (static_cast<uint64_t>(gid_) << IORING_OFF_PBUF_SHIFT);

  auto ringMem = ::mmap(
      nullptr,
      memSize,
      PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_POPULATE,
      ringIoPtr->ring_fd,
      offset);

  if (ringMem == MAP_FAILED) {
    auto errnoCopy = errno;
    ::io_uring_unregister_buf_ring(ringIoPtr, gid_);
    throw std::runtime_error(
        folly::to<std::string>(
            "unable to map ring memory of size ",
            memSize,
            ": ",
            folly::errnoStr(errnoCopy)));
  }

  ringPtr_ = static_cast<struct io_uring_buf_ring*>(ringMem);
}

std::unique_ptr<IoUringDynamicProvidedBufferRing::BufferArea>
IoUringDynamicProvidedBufferRing::createArea() noexcept {
  try {
    return std::make_unique<BufferArea>(
        ringBufferCount_, sizePerBuffer_, useIncremental_);
  } catch (const std::bad_alloc& ex) {
    LOG(ERROR) << "unable to create area: " << ex.what();
    return nullptr;
  }
}

IoUringDynamicProvidedBufferRing::BufferArea*
IoUringDynamicProvidedBufferRing::addArea() noexcept {
  if (areaCount_ == std::numeric_limits<decltype(areaCount_)>::max()) {
    LOG(ERROR) << "cannot add area: areaCount_ would exceed "
               << std::numeric_limits<decltype(areaCount_)>::max();
    return nullptr;
  }

  auto area = createArea();
  if (area == nullptr) {
    return nullptr;
  }

  BufferArea* areaPtr = area.get();
  areas_.push_back(std::move(area));
  areaCount_++;
  return areaPtr;
}

IoUringDynamicProvidedBufferRing::IoUringDynamicProvidedBufferRing(
    io_uring* ioRingPtr, Options options)
    : sizePerBuffer_(std::max(options.bufferSize, kMinBufferSize)),
      ringBufferCount_(options.bufferCount),
      gid_(options.gid),
      ringIoPtr(ioRingPtr),
      useIncremental_(options.useIncrementalBuffers) {
  if (ringBufferCount_ > kMaxRingRefillEntries) {
    throw std::runtime_error(
        folly::to<std::string>(
            "bufferCount cannot be larger than ", kMaxRingRefillEntries));
  }
  if (ringBufferCount_ == 0) {
    throw std::runtime_error("bufferCount cannot be 0");
  }
  if (ringBufferCount_ < 2) {
    LOG(WARNING) << folly::to<std::string>(
        "Buffer count is too small, adjusting to 2");
    ringBufferCount_ = 2;
  }
  if (!folly::isPowTwo(ringBufferCount_)) {
    auto adjustedBufferCount = folly::nextPowTwo(ringBufferCount_);
    LOG(WARNING) << folly::to<std::string>(
        "Buffer count isn't a power of 2: ",
        ringBufferCount_,
        ", adjusting to: ",
        adjustedBufferCount);
    ringBufferCount_ = adjustedBufferCount;
  }

  areas_.reserve(kMaxAreaCount);

  if (ringBufferCount_ <= kRingRefillFreeThreshold) {
    ringRefillThreshold_ = (ringBufferCount_ >> 1);
  } else {
    ringRefillThreshold_ = kRingRefillFreeThreshold;
  }

  for (uint32_t i = 0; i < kInitialAreaCount; i++) {
    if (addArea() == nullptr) {
      throw std::runtime_error(
          "unable to allocate initial provided buffer areas");
    }
  }

  initialRegister();
  mapRing();

  bufferActiveArea_ = areas_[0].get();
  bufferRefillArea_ = areas_[0].get();

  ringRefill();
}

void IoUringDynamicProvidedBufferRing::enobuf() noexcept {
  ringRefill();
  if (ringFillLevel() > 0) {
    return;
  }
  if (!enobuf_) {
    enobuf_ = true;
    enobufCount_++;
  }
}

uint32_t IoUringDynamicProvidedBufferRing::getAndResetEnobufCount() noexcept {
  auto count = enobufCount_;
  enobufCount_ = 0;
  return count;
}

void IoUringDynamicProvidedBufferRing::destroy() noexcept {
  ::io_uring_unregister_buf_ring(ringIoPtr, gid_);
  orphanAreas();
  ::munmap(ringPtr_, ringMemSize());
  delete this;
}

bool IoUringDynamicProvidedBufferRing::getNewRefillArea() noexcept {
  // areas_ are sorted so that recently used area at move to the end of the
  // vector. This allows to keep the potentially free areas to the front
  auto drained =
      std::find_if(areas_.begin(), areas_.end(), [this](const auto& area) {
        return areaIsDrained(*area);
      });
  if (drained != areas_.end()) {
    auto area = drained->get();
    DCHECK(area != bufferActiveArea_ && area != bufferRefillArea_)
        << "active and refill areas must not be drained";
    bufferRefillArea_ = area;
    std::rotate(drained, drained + 1, areas_.end());
    tryReclaimArea();
    return true;
  }

  if (areaCount_ >= kMaxAreaCount) {
    return false;
  }

  // No drained area available and cap not reached. Grow the pool by one and
  // refill from it.
  auto newArea = addArea();
  if (newArea == nullptr) {
    return false;
  }
  bufferRefillArea_ = newArea;
  return true;
}

void IoUringDynamicProvidedBufferRing::ringRefill() noexcept {
  auto startTail = ringTail_;

  // ringIndex(ringTail_) == 0 means we are at an area boundary: either the
  // previous ringRefill consumed all of the refill area, or we are using a
  // newly allocated area. In the former case grab a new refill area.
  if (ringIndex(ringTail_) == 0 && !areaIsDrained(*bufferRefillArea_) &&
      !getNewRefillArea()) {
    return;
  }

  if (ringFillLevel() == 0) {
    bufferActiveArea_ = bufferRefillArea_;
  }

  uint16_t pendingOutstanding = 0;
  auto freeEntries = ringFreeEntries();
  while (freeEntries--) {
    uint16_t bid = ringIndex(ringTail_);
    BufferState& state = bufferRefillArea_->states[bid];
    if (useIncremental_) {
      state.offset = 0;
      state.refCount.store(1);
    }

    auto* r = ringBuf(ringTail_);
    ringTail_++;
    r->addr = reinterpret_cast<__u64>(getData(*bufferRefillArea_, bid));
    r->len = sizePerBuffer_;
    r->bid = bid;
    pendingOutstanding++;

    // We reached the end of the refill area, flush the pendingOutstanding
    // counter at once and get a new area if there are still freeEntries to
    // refill
    if (ringIndex(ringTail_) == 0) {
      bufferRefillArea_->outstanding.fetch_add(
          pendingOutstanding, std::memory_order_relaxed);
      pendingOutstanding = 0;
      if (freeEntries && !getNewRefillArea()) {
        break;
      }
    }
  }
  if (pendingOutstanding > 0) {
    bufferRefillArea_->outstanding.fetch_add(
        pendingOutstanding, std::memory_order_relaxed);
  }

  // If we refilled entries, then try to update the shared tail
  if (ringTail_ != startTail) {
    if (tryPublish(startTail, ringTail_)) {
      enobuf_ = false;
    }
  }
}

void IoUringDynamicProvidedBufferRing::ringMaybeRefill() noexcept {
  if (ringFreeEntries() >= ringRefillThreshold_) {
    ringRefill();
  }
}

void IoUringDynamicProvidedBufferRing::tryReclaimArea() noexcept {
  if (areaCount_ <= kInitialAreaCount ||
      areasOutstandingSum() > ((areaCount_ * ringBufferCount_) >> 1)) {
    return;
  }

  std::optional<uint32_t> victimIndex;
  for (uint32_t areaIndex = 0; areaIndex < areaCount_; ++areaIndex) {
    const BufferArea* area = areas_[areaIndex].get();
    if (!areaIsDrained(*area)) {
      continue;
    }
    if (area == bufferActiveArea_ || area == bufferRefillArea_) {
      continue;
    }
    victimIndex = areaIndex;
    break;
  }

  if (!victimIndex) {
    return;
  }

  areas_.erase(areas_.begin() + *victimIndex);
  areaCount_--;
}

void IoUringDynamicProvidedBufferRing::bufFreeFn(
    void* /* buf */, void* userData) noexcept {
  auto* bufferState = static_cast<BufferState*>(userData);
  // @lint-ignore NULLSAFECLANG: takeOwnership always passes a live state/area.
  BufferArea& area = *bufferState->area;
  if (area.incremental && bufferState->refCount.fetch_sub(1) != 1) {
    return;
  }
  // Nothing in `area` may be touched after this decrement unless it was the
  // last reference of an orphaned area: the ring may reclaim it concurrently.
  if (area.outstanding.fetch_sub(1, std::memory_order_acq_rel) ==
      (kAreaOrphaned | 1)) {
    delete &area;
  }
}

void IoUringDynamicProvidedBufferRing::orphanAreas() noexcept {
  // Buffers still published to the ring were never handed out, so no free will
  // ever return them. Drop them from each area's count and orphan the area in
  // the same atomic update; whoever brings the count to kAreaOrphaned deletes.
  std::array<uint32_t, kMaxAreaCount> inRing{};
  for (uint16_t i = ringHead_; i != ringTail_; ++i) {
    const auto addr = ringBuf(i)->addr;
    for (size_t a = 0; a < areas_.size(); ++a) {
      BufferArea& area = *areas_[a];
      const auto base = reinterpret_cast<uintptr_t>(area.buffers);
      if (addr < base || addr >= base + area.memSize) {
        continue;
      }
      // A partially consumed incremental buffer is released by its last
      // outstanding slice instead.
      if (!useIncremental_ ||
          area.states[ringBuf(i)->bid].refCount.fetch_sub(1) == 1) {
        inRing[a]++;
      }
      break;
    }
  }

  for (size_t a = 0; a < areas_.size(); ++a) {
    BufferArea* area = areas_[a].release();
    uint32_t delta = kAreaOrphaned - inRing[a];
    // @lint-ignore NULLSAFECLANG: areas_ contains only live owners.
    if (area->outstanding.fetch_add(delta, std::memory_order_acq_rel) + delta ==
        kAreaOrphaned) {
      delete area;
    }
  }
  areas_.clear();
}

std::unique_ptr<IOBuf> IoUringDynamicProvidedBufferRing::getIoBufSingle(
    uint16_t bid, size_t length, bool hasMore) noexcept {
  std::unique_ptr<IOBuf> ret;
  DCHECK_LT(bid, ringBufferCount_)
      << "Buffer index " << bid << " exceeds buffer count " << ringBufferCount_;

  auto* bufferStart = getData(*bufferActiveArea_, bid);
  BufferState* info = &bufferActiveArea_->states[bid];
  if (useIncremental_) {
    unsigned int currentOffset = info->offset;
    auto* dataPtr = bufferStart + currentOffset;
    ret = IOBuf::takeOwnership(
        static_cast<void*>(dataPtr), length, length, bufFreeFn, info);
  } else {
    ret = IOBuf::takeOwnership(
        static_cast<void*>(bufferStart),
        sizePerBuffer_,
        length,
        bufFreeFn,
        info);
  }

  ret->markExternallySharedOne();
  incBufferState(*bufferActiveArea_, bid, hasMore, length);

  ringMaybeRefill();
  return ret;
}

std::unique_ptr<IOBuf> IoUringDynamicProvidedBufferRing::getIoBuf(
    uint16_t startBufId, size_t totalLength, bool hasMore) noexcept {
  DCHECK_EQ(ringBuf(ringHead_)->bid, startBufId)
      << "ringHead_ out of sync with kernel ring head";

  size_t currentAvailable = sizePerBuffer_;
  if (useIncremental_) {
    currentAvailable -= bufferActiveArea_->states[startBufId].offset;
  }
  if (totalLength <= currentAvailable) {
    return getIoBufSingle(startBufId, totalLength, hasMore);
  }

  std::unique_ptr<IOBuf> head;
  size_t remainingLength = totalLength;
  uint16_t bid = startBufId;

  while (remainingLength > 0) {
    DCHECK_LT(bid, ringBufferCount_)
        << "Buffer index " << bid << " exceeds buffer count "
        << ringBufferCount_;

    BufferState* bufferState = &bufferActiveArea_->states[bid];
    char* bufferStart = getData(*bufferActiveArea_, bid);
    unsigned int currentOffset = 0;
    size_t availableInBuffer = sizePerBuffer_;

    if (useIncremental_) {
      currentOffset = bufferState->offset;
      availableInBuffer = sizePerBuffer_ - currentOffset;
    }

    char* dataPtr = bufferStart + currentOffset;
    size_t currentChunkSize = std::min(remainingLength, availableInBuffer);
    bool isLastChunk = (remainingLength <= availableInBuffer);

    std::unique_ptr<IOBuf> chunk;
    chunk = IOBuf::takeOwnership(
        static_cast<void*>(dataPtr),
        useIncremental_ ? currentChunkSize : sizePerBuffer_,
        currentChunkSize,
        bufFreeFn,
        bufferState);

    chunk->markExternallySharedOne();
    if (!head) {
      head = std::move(chunk);
    } else {
      head->appendToChain(std::move(chunk));
    }

    bool bufHasMore = hasMore && isLastChunk;
    incBufferState(*bufferActiveArea_, bid, bufHasMore, currentChunkSize);
    remainingLength -= currentChunkSize;

    bid = ringIndex(bid + 1);
  }

  ringMaybeRefill();
  return head;
}

std::unique_ptr<IOBuf> IoUringDynamicProvidedBufferRing::getIoBuf(
    const struct io_uring_cqe* cqe) noexcept {
  auto bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
  auto res = cqe->res;
  bool hasMore = (cqe->flags & IORING_CQE_F_BUF_MORE) != 0;
  return getIoBuf(bid, res, hasMore);
}

void IoUringDynamicProvidedBufferRing::initialRegister() {
  struct io_uring_buf_reg reg{};
  memset(&reg, 0, sizeof(reg));
  reg.ring_entries = ringBufferCount_;
  reg.bgid = gid_;

  int flags = IOU_PBUF_RING_MMAP | (useIncremental_ ? IOU_PBUF_RING_INC : 0);
  int ret = ::io_uring_register_buf_ring(ringIoPtr, &reg, flags);

  if (ret) {
    LOG(ERROR) << folly::to<std::string>(
        "unable to register provided buffer ring ",
        -ret,
        ": ",
        folly::errnoStr(-ret));
    LOG(ERROR) << folly::to<std::string>(
        "buffer ring buffer count: ",
        ringBufferCount_,
        ", size per buf: ",
        sizePerBuffer_,
        ", bgid: ",
        gid_);
    if (ret == -ENOMEM) {
      throw OutOfMemory("unable to register provided buffer ring");
    }
    throw LibUringCallError("unable to register provided buffer ring");
  }
}

IoUringDynamicProvidedBufferRing::BufferArea::BufferArea(
    uint32_t bufferCount, uint32_t bufferSize, bool incremental)
    : states(std::make_unique<BufferState[]>(bufferCount)),
      memSize(
          folly::align_ceil(
              static_cast<size_t>(bufferCount) * bufferSize,
              kHugePageSizeBytes)),
      incremental(incremental) {
  void* mem = ::mmap(
      nullptr,
      memSize,
      PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE,
      -1,
      0);

  if (mem == MAP_FAILED) {
    PLOG(ERROR) << "unable to allocate area block of size " << memSize;
    throw std::bad_alloc();
  }

  int ret = ::madvise(mem, memSize, MADV_HUGEPAGE);
  PLOG_IF(ERROR, ret) << "cannot enable huge pages";

  buffers = static_cast<char*>(mem);

  for (uint32_t b = 0; b < bufferCount; b++) {
    states[b].area = this;
    states[b].bufId = static_cast<uint16_t>(b);
    states[b].offset = 0;
  }
}

IoUringDynamicProvidedBufferRing::BufferArea::~BufferArea() {
  ::munmap(buffers, memSize);
}

void IoUringDynamicProvidedBufferRing::incBufferState(
    BufferArea& area,
    uint16_t bid,
    bool hasMore,
    size_t bytesConsumed) noexcept {
  if (useIncremental_ && hasMore) {
    BufferState* bufferState = &area.states[bid];
    bufferState->refCount.fetch_add(1);
    bufferState->offset += bytesConsumed;
  }

  if (!hasMore) {
    ringHead_++;

    // We consumed all the buffers of the current area. active area becomes the
    // refill area
    if (bid == ringBufferCount_ - 1) {
      bufferActiveArea_ = bufferRefillArea_;
    }
  }
}

int IoUringDynamicProvidedBufferRing::getUtilPct() const noexcept {
  uint64_t totalBuffers = areaCount_ * ringBufferCount_;
  auto outstanding = areasOutstandingSum();
  auto inRing = ringFillLevel();
  uint64_t inUse = (outstanding > inRing) ? (outstanding - inRing) : 0;
  inUse = std::min(inUse, totalBuffers);

  return (100 * inUse) / totalBuffers;
}

} // namespace folly

#endif
