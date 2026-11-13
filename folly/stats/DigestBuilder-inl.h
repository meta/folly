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

#include <folly/concurrency/CacheLocality.h>
#include <folly/lang/Bits.h>

namespace folly {

template <typename DigestT>
DigestBuilder<DigestT>::DigestBuilder(size_t bufferSize, size_t digestSize)
    : bufferSize_(bufferSize), digestSize_(digestSize) {
  auto& cl = CacheLocality::system();
  cpuLocalBuffers_.resize(cl.numCachesByLevel[0]);
}

template <typename DigestT>
DigestT DigestBuilder<DigestT>::build() {
  std::vector<std::vector<double>> valuesVec;
  std::vector<std::unique_ptr<DigestT>> digestPtrs;
  valuesVec.reserve(cpuLocalBuffers_.size());
  digestPtrs.reserve(cpuLocalBuffers_.size());

  for (auto& cpuLocalBuffer : cpuLocalBuffers_) {
    // We want to keep the critical section in update() as small as possible, to
    // reduce the chance of preemption while holding the lock; in particular, we
    // should avoid allocations, which can involve syscalls. So, try to return
    // the cpuLocalBuffer in the same state it was found if it received any
    // values. The state may have changed by the time we re-acquire the lock,
    // but this does not affect correctness.
    std::vector<double> newBuffer;
    std::unique_ptr<DigestT> newDigest;

    auto g = std::unique_lock(cpuLocalBuffer.mutex);
    bool hasDigest =
        cpuLocalBuffer.digest != nullptr && !cpuLocalBuffer.digest->empty();
    // If at least one merge happened, bufferSize_ was reached.
    size_t capacity = hasDigest
        ? bufferSize_
        : std::min(nextPowTwo(cpuLocalBuffer.buffer.size()), bufferSize_);
    if (capacity > 0 || hasDigest) {
      g.unlock();
      newBuffer.reserve(capacity);
      newDigest = hasDigest ? std::make_unique<DigestT>(digestSize_) : nullptr;
      g.lock();
    }

    valuesVec.push_back(
        std::exchange(cpuLocalBuffer.buffer, std::move(newBuffer)));
    if (cpuLocalBuffer.digest) {
      digestPtrs.push_back(
          std::exchange(cpuLocalBuffer.digest, std::move(newDigest)));
    }
  }

  size_t count = 0;
  for (const auto& vec : valuesVec) {
    count += vec.size();
  }

  std::vector<DigestT> digests;
  digests.reserve(digestPtrs.size() + (count > 0 ? 1 : 0));
  for (auto& digestPtr : digestPtrs) {
    digests.push_back(std::move(*digestPtr));
  }
  if (count) {
    std::vector<double> values;
    values.reserve(count);
    for (const auto& vec : valuesVec) {
      values.insert(values.end(), vec.begin(), vec.end());
    }
    DigestT digest(digestSize_);
    digests.push_back(digest.merge(values));
  }
  return DigestT::merge(digests);
}

template <typename DigestT>
void DigestBuilder<DigestT>::append(double value) {
  const auto numBuffers = cpuLocalBuffers_.size();
  auto cpuLocalBuf =
      &cpuLocalBuffers_[AccessSpreader<>::cachedCurrent(numBuffers)];
  auto g = std::unique_lock(cpuLocalBuf->mutex, std::try_to_lock);
  if (!g.owns_lock()) [[unlikely]] {
    // If the mutex is already held by another thread, either build() is
    // running, or this or that thread have a stale stripe (possibly because the
    // thread migrated right after the call to cachedCurrent()). So invalidate
    // the cache and wait on the mutex.
    AccessSpreader<>::invalidateCachedCurrent();
    cpuLocalBuf =
        &cpuLocalBuffers_[AccessSpreader<>::cachedCurrent(numBuffers)];
    g = std::unique_lock(cpuLocalBuf->mutex);
  }

  // CAUTION: This looks like a good opportunity to preallocate the vector. But
  // the vector is per-quantile per-core, meaning there can be quite a lot of
  // these vectors, and it is expected that only the hottest of them will ever
  // actually get full. For those buffers which do actually get full, we already
  // take care to avoid deallocating below or to preallocate the max in build().
  // For all other vectors, we need to be conscious of memory usage. So do not
  // preallocate here.

  cpuLocalBuf->buffer.push_back(value);
  if (cpuLocalBuf->buffer.size() == bufferSize_) [[unlikely]] {
    if (!cpuLocalBuf->digest) {
      cpuLocalBuf->digest = std::make_unique<DigestT>(digestSize_);
    }
    *cpuLocalBuf->digest = cpuLocalBuf->digest->merge(cpuLocalBuf->buffer);
    cpuLocalBuf->buffer.clear();
  }
}

} // namespace folly
