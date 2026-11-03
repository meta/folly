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

#include <folly/system/Pid.h>

#include <atomic>

#include <glog/logging.h>

#include <folly/portability/Unistd.h>
#include <folly/system/AtFork.h>

namespace folly {

namespace {

enum class State : uint8_t { INVALID, LOCKED, VALID };

class PidState {
  std::atomic<State> value_{State::INVALID};

 public:
  FOLLY_ALWAYS_INLINE State load() noexcept {
    return value_.load(std::memory_order_acquire);
  }

  void store(State state) noexcept {
    value_.store(state, std::memory_order_release);
  }

  bool cas(State& expected, State newstate) noexcept {
    return value_.compare_exchange_strong(
        expected,
        newstate,
        std::memory_order_relaxed,
        std::memory_order_relaxed);
  }
}; // PidState

class PidCache {
  PidState state_;
  pid_t pid_{};

 public:
  FOLLY_ALWAYS_INLINE pid_t get() {
    DCHECK(!valid() || pid_ == getpid());
    return valid() ? pid_ : init();
  }

 private:
  bool valid() { return state_.load() == State::VALID; }

  [[FOLLY_ATTR_GNU_COLD]] pid_t init() {
    pid_t pid = getpid();
    auto s = state_.load();
    if (s == State::INVALID && state_.cas(s, State::LOCKED)) {
      folly::AtFork::registerHandler(
          this, [] { return true; }, [] {}, [this] { pid_ = getpid(); });
      pid_ = pid;
      state_.store(State::VALID);
    }
    return pid;
  }
}; // PidCache

constinit PidCache cache_;

} // namespace

pid_t get_cached_pid() {
  return cache_.get();
}

} // namespace folly
