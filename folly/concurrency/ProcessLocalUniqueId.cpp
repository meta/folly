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

#include <folly/concurrency/ProcessLocalUniqueId.h>
#include <folly/synchronization/RelaxedAtomic.h>

#include <atomic>

namespace folly {

uint64_t processLocalUniqueId() {
  constinit static relaxed_atomic<uint64_t> nextEpoch{0};
  // Id format is <epoch: 48 bits> <counter: 16 bits>.
  // Ephemeral threads, if any, can waste a whole epoch, so we keep epochs
  // relatively small, but large enough to amortize the atomic epoch increment.
  constexpr int kCounterBits = 16;
  constexpr uint64_t kCounterMask = (uint64_t(1) << kCounterBits) - 1;
  thread_local uint64_t next{0};

  // If first call in thread, or counter wrapped around, start new epoch.
  if ((next & kCounterMask) == 0) [[unlikely]] {
    next = nextEpoch++ << kCounterBits;
    // Skip 0 as per contract.
    if (next == 0) [[unlikely]] {
      ++next;
    }
  }

  return next++;
}

} // namespace folly
