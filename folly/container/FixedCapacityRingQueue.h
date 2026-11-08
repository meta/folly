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

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

#include <glog/logging.h>
#include <folly/Likely.h>
#include <folly/lang/Bits.h>

namespace folly {

/**
 * A simple fixed-capacity ring buffer queue backed by a power-of-two sized
 * array. Not thread-safe.
 */
template <typename T>
class FixedCapacityRingQueue {
 public:
  static constexpr uint32_t kMaxCapacity = 1u << 31;

  FixedCapacityRingQueue() = default;

  explicit FixedCapacityRingQueue(uint32_t capacity) {
    if (capacity > kMaxCapacity) {
      throw std::length_error(
          "FixedCapacityRingQueue capacity exceeds maximum");
    }
    mask_ = folly::nextPowTwo(capacity) - 1;
    buf_ = std::make_unique<T[]>(mask_ + 1);
  }

  ~FixedCapacityRingQueue() = default;

  FixedCapacityRingQueue(const FixedCapacityRingQueue&) = delete;
  FixedCapacityRingQueue& operator=(const FixedCapacityRingQueue&) = delete;

  FixedCapacityRingQueue(FixedCapacityRingQueue&& other) noexcept
      : mask_(std::exchange(other.mask_, ~0u)),
        buf_(std::move(other.buf_)),
        head_(std::exchange(other.head_, 0)),
        tail_(std::exchange(other.tail_, 0)) {}

  FixedCapacityRingQueue& operator=(FixedCapacityRingQueue&& other) noexcept {
    mask_ = std::exchange(other.mask_, ~0u);
    buf_ = std::move(other.buf_);
    head_ = std::exchange(other.head_, 0);
    tail_ = std::exchange(other.tail_, 0);
    return *this;
  }

  bool push(T val) {
    if (size() >= capacity()) [[unlikely]] {
      return false;
    }
    buf_[tail_++ & mask_] = val;
    return true;
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
  uint32_t mask_{~0u};
  std::unique_ptr<T[]> buf_{nullptr};
  uint32_t head_{0};
  uint32_t tail_{0};
};

} // namespace folly
