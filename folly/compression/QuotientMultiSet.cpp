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

#include <folly/compression/QuotientMultiSet.h>

#include <cmath>
#include <stdexcept>

#include <folly/Math.h>
#include <folly/lang/Exception.h>

#if FOLLY_QUOTIENT_MULTI_SET_SUPPORTED

namespace folly {

QuotientMultiSetBuilder::QuotientMultiSetBuilder(
    size_t keyBits, size_t expectedElements, double loadFactor)
    : keyBits_(keyBits), maxKey_(qms_detail::maxValue(keyBits_)) {
  expectedElements = std::max<size_t>(expectedElements, 1);
  uint64_t numSlots = to_integral(ceil(expectedElements / loadFactor));

  // Make sure 1:1 mapping between key space and <divisor, remainder> pairs.
  divisor_ = divCeil(maxKey_, numSlots);
  remainderBits_ = findLastSet(divisor_ - 1);

  // We only support remainders as long as 56 bits. If the set is very
  // sparse, force the maximum allowed remainder size. This will waste
  // up to 3 extra blocks (because of 8-bit quotients) but be correct.
  if (remainderBits_ > 56) {
    remainderBits_ = 56;
    divisor_ = uint64_t(1) << remainderBits_;
  }

  blockSize_ = Block::blockSize(remainderBits_);
  fraction_ = qms_detail::getInverse(divisor_);
}

QuotientMultiSetBuilder::~QuotientMultiSetBuilder() = default;

bool QuotientMultiSetBuilder::maybeAllocateBlocks(size_t limitIndex) {
  bool blockAllocated = false;
  for (; numBlocks_ <= limitIndex; numBlocks_++) {
    auto block = Block::make(remainderBits_);
    blocks_.emplace_back(std::move(block), numBlocks_);
    blockAllocated = true;
  }
  return blockAllocated;
}

bool QuotientMultiSetBuilder::insert(uint64_t key) {
  if (key > maxKey_) {
    throw_exception<std::invalid_argument>("Invalid key");
  }
  if (key < prevKey_) {
    throw_exception<std::invalid_argument>(
        "Keys need to be inserted in nondecreasing order");
  }
  const auto qr = qms_detail::getQuotientAndRemainder(key, divisor_, fraction_);
  const auto& quotient = qr.first;
  const auto& remainder = qr.second;
  const size_t blockIndex = quotient / kBlockSize;
  const size_t offsetInBlock = quotient % kBlockSize;

  bool newBlockAllocated = false;
  // Allocate block for the given key if necessary.
  newBlockAllocated |= maybeAllocateBlocks(
      std::max<uint64_t>(blockIndex, nextSlot_ / kBlockSize));
  auto block = getBlock(nextSlot_ / kBlockSize).block.get();

  // Start a new run.
  if (prevOccupiedQuotient_ != quotient) {
    closePreviousRun();

    if (blockIndex > nextSlot_ / kBlockSize) {
      nextSlot_ = (blockIndex * kBlockSize);
      newBlockAllocated |= maybeAllocateBlocks(blockIndex);
      block = getBlock(blockIndex).block.get();
    }

    // Update previous run info.
    prevRunStart_ = nextSlot_;
    prevOccupiedQuotient_ = quotient;
  }

  block->setRemainder(nextSlot_ % kBlockSize, remainderBits_, remainder);

  // Set occupied bit for the given key.
  block = getBlock(blockIndex).block.get();
  block->setOccupied(offsetInBlock);

  nextSlot_++;
  prevKey_ = key;
  numKeys_++;
  return newBlockAllocated;
}

void QuotientMultiSetBuilder::setBlockPayload(uint64_t payload) {
  DCHECK(!blocks_.empty());
  blocks_.back().block->payload = payload;
}

void QuotientMultiSetBuilder::closePreviousRun() {
  if (nextSlot_ == 0) [[unlikely]] {
    return;
  }

  // Mark runend for previous run.
  const auto runEnd = nextSlot_ - 1;
  auto block = getBlock(runEnd / kBlockSize).block.get();
  block->setRunend(runEnd % kBlockSize);
  numRuns_++;

  // Set the offset of previous block if this run is the first one in that
  // block.
  auto prevRunOccupiedBlock =
      getBlock(prevOccupiedQuotient_ / kBlockSize).block.get();
  if (isPowTwo(prevRunOccupiedBlock->occupieds)) {
    prevRunOccupiedBlock->offset = runEnd;
  }

  // Update mark all blocks before prevOccupiedQuotient_ + 1 to be ready.
  size_t limitIndex = (prevOccupiedQuotient_ + 1) / kBlockSize;
  for (size_t idx = readyBlocks_; idx < blocks_.size(); idx++) {
    if (blocks_[idx].index < limitIndex) {
      blocks_[idx].ready = true;
      readyBlocks_++;
    } else {
      break;
    }
  }
}

void QuotientMultiSetBuilder::moveReadyBlocks(IOBufQueue& buff) {
  while (!blocks_.empty()) {
    if (!blocks_.front().ready) {
      break;
    }
    buff.append(
        IOBuf::takeOwnership(blocks_.front().block.release(), blockSize_));
    blocks_.pop_front();
  }
}

void QuotientMultiSetBuilder::flush(IOBufQueue& buff) {
  moveReadyBlocks(buff);
  readyBlocks_ = 0;
}

void QuotientMultiSetBuilder::close(IOBufQueue& buff) {
  closePreviousRun();

  // Mark all blocks as ready.
  for (auto iter = blocks_.rbegin(); iter != blocks_.rend(); iter++) {
    if (iter->ready) {
      break;
    }
    iter->ready = true;
  }

  moveReadyBlocks(buff);

  // Add metadata trailer. This will also allows getRemainder() to access whole
  // 64-bits at any position without bounds-checking.
  static_assert(sizeof(Metadata) > 7, "getRemainder() is not safe");
  auto metadata = reinterpret_cast<Metadata*>(calloc(1, sizeof(Metadata)));
  metadata->numBlocks = numBlocks_;
  metadata->numKeys = numKeys_;
  metadata->divisor = divisor_;
  metadata->keyBits = keyBits_;
  metadata->remainderBits = remainderBits_;
  VLOG(2) << "Metadata: " << metadata->debugString();
  buff.append(IOBuf::takeOwnership(metadata, sizeof(Metadata)));
}

} // namespace folly

#endif // FOLLY_QUOTIENT_MULTI_SET_SUPPORTED
