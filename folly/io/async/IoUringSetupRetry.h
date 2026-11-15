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
#include <cerrno>
#include <chrono>
#include <thread>
#include <type_traits>
#include <utility>

namespace folly::detail {

namespace io_uring_setup_retry_detail {

class Backoff {
 public:
  bool wait() {
    if (std::chrono::steady_clock::now() + backoff_ > deadline_) {
      return false;
    }
    /* sleep override */ std::this_thread::sleep_for(backoff_);
    backoff_ = std::min(backoff_ * 2, kMaxBackoff);
    return true;
  }

 private:
  static constexpr auto kMaxBackoff = std::chrono::milliseconds(1000);
  const std::chrono::steady_clock::time_point deadline_ =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  std::chrono::milliseconds backoff_{10};
};

} // namespace io_uring_setup_retry_detail

template <typename TrySetup>
int retryTransientIoUringSetup(TrySetup&& trySetup) {
  io_uring_setup_retry_detail::Backoff backoff;
  int result;
  do {
    result = trySetup();
  } while (result == -ENOMEM && backoff.wait());
  return result;
}

template <typename Exception, typename TrySetup, typename ShouldRetry>
std::invoke_result_t<TrySetup&> retryTransientIoUringSetup(
    TrySetup&& trySetup, ShouldRetry&& shouldRetry) {
  io_uring_setup_retry_detail::Backoff backoff;
  while (true) {
    try {
      return trySetup();
    } catch (const Exception& error) {
      if (!shouldRetry(error) || !backoff.wait()) {
        throw;
      }
    }
  }
}

template <typename OutOfMemory, typename TrySetup>
std::invoke_result_t<TrySetup&> retryTransientIoUringSetup(
    TrySetup&& trySetup) {
  return retryTransientIoUringSetup<OutOfMemory>(
      std::forward<TrySetup>(trySetup),
      [](const OutOfMemory&) { return true; });
}

} // namespace folly::detail
