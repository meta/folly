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

#include <optional>

#include <folly/Chrono.h>
#include <folly/futures/Future.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/HHWheelTimer.h>

namespace folly {

// Callback object for HHWheelTimer
template <class TBase>
struct WTCallback : public TBase::Callback {
  struct PrivateConstructorTag {};

 public:
  explicit WTCallback(PrivateConstructorTag) {}

  // Only allow creation by this factory, to ensure heap allocation.
  static std::pair<std::shared_ptr<WTCallback>, SemiFuture<Unit>> create(
      EventBase* base) {
    // optimization opportunity: memory pool
    auto cob = std::make_shared<WTCallback>(PrivateConstructorTag{});
    auto& state = cob->state_.unsafeGetUnlocked().emplace(State{base, {}});
    // Capture shared_ptr of cob in lambda so that Core inside Promise will
    // hold a ref count to it. The ref count will be released when Core goes
    // away which happens when both Promise and Future go away
    state.promise.setInterruptHandler([cob](exception_wrapper ew) mutable {
      interruptHandler(std::move(cob), std::move(ew));
    });
    return {std::move(cob), state.promise.getSemiFuture()};
  }

 protected:
  struct State {
    EventBase* base;
    Promise<Unit> promise;
  };

  // First thread that can fulfill the promise unsets the state, breaking the
  // circular reference WTCallback -> promise -> core -> WTCallback.
  folly::Synchronized<std::optional<State>> state_;

  void timeoutExpired() noexcept override {
    if (auto state = state_.exchange({})) {
      state->promise.setValue();
    }
  }

  void callbackCanceled() noexcept override {
    if (auto state = state_.exchange({})) {
      state->promise.setException(FutureNoTimekeeper{});
    }
  }

  static void interruptHandler(
      std::shared_ptr<WTCallback> self, exception_wrapper ew) {
    // Hold the lock while scheduling the callback, so that callbackCanceled()
    // blocks the timekeeper destructor keeping the base pointer valid.
    auto wState = self->state_.wlock();
    if (!*wState) {
      return;
    }

    auto state = std::exchange(*wState, {});
    auto* base_2 = state->base;
    base_2->runInEventBaseThreadAlwaysEnqueue(
        [self, state_2 = std::move(state), ew_2 = std::move(ew)]() mutable {
          self->cancelTimeout();
          state_2->promise.setException(std::move(ew_2));
        });
  }
};

} // namespace folly

#endif // !defined(__wasi__)
