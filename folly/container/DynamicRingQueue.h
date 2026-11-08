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

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

#include <glog/logging.h>
#include <folly/Likely.h>
#include <folly/lang/Bits.h>

template <typename T>
class DynamicRingQueueTestHelper;

namespace folly {

/**
 * A ring buffer queue backed by a power-of-two sized array that grows
 * on overflow. Not thread-safe.
 */
template <typename T>
class DynamicRingQueue {
 public:
  static constexpr uint32_t kMaxCapacity = 1u << 31;

  DynamicRingQueue() = default;

  explicit DynamicRingQueue(uint32_t capacity) {
    if (capacity > kMaxCapacity) {
      throw std::length_error("DynamicRingQueue capacity exceeds maximum");
    }
    mask_ = folly::nextPowTwo(capacity) - 1;
    buf_ = std::make_unique<T[]>(mask_ + 1);
  }

  ~DynamicRingQueue() = default;

  DynamicRingQueue(const DynamicRingQueue&) = delete;
  DynamicRingQueue& operator=(const DynamicRingQueue&) = delete;

  DynamicRingQueue(DynamicRingQueue&& other) noexcept
      : mask_(std::exchange(other.mask_, ~0u)),
        buf_(std::move(other.buf_)),
        head_(std::exchange(other.head_, 0)),
        tail_(std::exchange(other.tail_, 0)) {}

  DynamicRingQueue& operator=(DynamicRingQueue&& other) noexcept {
    mask_ = std::exchange(other.mask_, ~0u);
    buf_ = std::move(other.buf_);
    head_ = std::exchange(other.head_, 0);
    tail_ = std::exchange(other.tail_, 0);
    return *this;
  }

  void push(T val) {
    if (size() >= capacity()) [[unlikely]] {
      grow();
    }
    buf_[tail_++ & mask_] = val;
  }

  T pop() {
    DCHECK(!empty());
    return buf_[head_++ & mask_];
  }

  uint32_t size() const { return tail_ - head_; }
  uint32_t capacity() const { return mask_ + 1; }
  uint32_t max_size() const noexcept { return kMaxCapacity; }
  bool empty() const { return head_ == tail_; }

 private:
  void grow() {
    uint32_t oldCap = capacity();
    if (oldCap >= kMaxCapacity) {
      throw std::length_error("DynamicRingQueue capacity exceeds maximum");
    }
    uint32_t newCap = oldCap == 0 ? 1 : oldCap * 2;
    auto newBuf = std::make_unique<T[]>(newCap);

    uint32_t n = size();
    uint32_t head = head_ & mask_;
    uint32_t headLen = std::min(n, oldCap - head);
    auto* src = buf_.get();
    auto* dst = newBuf.get();
    std::copy(src + head, src + head + headLen, dst);
    std::copy(src, src + (n - headLen), dst + headLen);

    buf_ = std::move(newBuf);
    mask_ = newCap - 1;
    head_ = 0;
    tail_ = n;
  }

  template <typename U>
  friend class ::DynamicRingQueueTestHelper;

  uint32_t mask_{~0u};
  std::unique_ptr<T[]> buf_{nullptr};
  uint32_t head_{0};
  uint32_t tail_{0};
};

} // namespace folly
