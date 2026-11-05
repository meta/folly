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

// EventBase needs libevent and pipe(), which wasi lacks, so on wasi nothing is
// declared here.
#if !defined(__wasi__)

#include <thread>

#include <folly/futures/Future.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/HHWheelTimer.h>

namespace folly {

class EventBaseThreadTimekeeper : public Timekeeper {
 public:
  EventBaseThreadTimekeeper() = delete;
  explicit EventBaseThreadTimekeeper(folly::EventBase& eventBase)
      : eventBaseRef_(eventBase) {}
  ~EventBaseThreadTimekeeper() override = default;

  /// Implement the Timekeeper interface
  SemiFuture<Unit> after(HighResDuration) override;

 protected:
  folly::EventBase& eventBaseRef_;
};

/// The default Timekeeper implementation which uses a HHWheelTimer on an
/// EventBase in a dedicated thread. Users needn't deal with this directly, it
/// is used by default by Future methods that work with timeouts.
class ThreadWheelTimekeeper : public EventBaseThreadTimekeeper {
 public:
  /// But it doesn't *have* to be a singleton.
  ThreadWheelTimekeeper();
  ~ThreadWheelTimekeeper() override;

 protected:
  folly::EventBase eventBase_{folly::EventBase::Options().setTimerTickInterval(
      std::chrono::milliseconds(1))};
  std::thread thread_;
};

} // namespace folly

#endif // !defined(__wasi__)
