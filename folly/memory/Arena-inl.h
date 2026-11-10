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

#ifndef FOLLY_ARENA_H_
#error This file may only be included from Arena.h
#endif

// Implementation of Arena.h functions

#include <folly/lang/SafeAssert.h>

namespace folly {

template <class Alloc>
void* Arena<Alloc>::allocateSlow(size_t size) {
  char* start;

  size_t allocSize;
  if (!checked_add(
          &allocSize, std::max(size, minBlockSize()), roundUp(sizeof(Block)))) {
    throw_exception<std::bad_alloc>();
  }
  if (sizeLimit_ != kNoSizeLimit &&
      allocSize > sizeLimit_ - totalAllocatedSize_) {
    throw_exception<std::bad_alloc>();
  }

  if (size > minBlockSize()) {
    // Allocate a large block for this chunk only; don't change ptr_ and end_,
    // let them point into a normal block (or none, if they're null)
    allocSize = roundUp(sizeof(LargeBlock)) + size;
    void* mem = AllocTraits::allocate(alloc(), allocSize);
    auto blk = new (mem) LargeBlock(allocSize);
    start = align(blk->start());
    largeBlocks_.push_back(*blk);
  } else {
    // Allocate a normal sized block and carve out size bytes from it
    // Will allocate more than size bytes if convenient
    // (via ArenaAllocatorTraits::goodSize()) as we'll try to pack small
    // allocations in this block.
    allocSize = blockGoodAllocSize();
    void* mem = AllocTraits::allocate(alloc(), allocSize);
    auto blk = new (mem) Block();
    start = align(blk->start());
    blocks_.push_back(*blk);
    currentBlock_ = blocks_.last();
    ptr_ = start + size;
    end_ = static_cast<char*>(mem) + allocSize;
    assert(ptr_ <= end_);
  }

  totalAllocatedSize_ += allocSize;
  return start;
}

template <class Alloc>
void Arena<Alloc>::merge(Arena<Alloc>&& other) {
  FOLLY_SAFE_CHECK(
      blockGoodAllocSize() == other.blockGoodAllocSize(),
      "cannot merge arenas of different minBlockSize");
  const bool wasEmpty = blocks_.empty();
  blocks_.splice_after(blocks_.before_begin(), other.blocks_);
  other.blocks_.clear();
  if (wasEmpty) {
    // currentBlock_ is still before_begin(); leaving it there would let
    // canReuseExistingBlock() bump-allocate into a merged block.
    currentBlock_ = blocks_.last();
  }
  largeBlocks_.splice_after(largeBlocks_.before_begin(), other.largeBlocks_);
  other.largeBlocks_.clear();
  other.ptr_ = other.end_ = nullptr;
  totalAllocatedSize_ += other.totalAllocatedSize_;
  other.totalAllocatedSize_ = 0;
  bytesUsed_ += other.bytesUsed_;
  other.bytesUsed_ = 0;
}
} // namespace folly
