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

#include <folly/Memory.h>

namespace folly {
namespace fibers {

inline EventBaseLoopController::EventBaseLoopController() : callback_(*this) {}

inline EventBaseLoopController::~EventBaseLoopController() {
  callback_.cancelLoopCallback();
  eventBaseKeepAlive_.reset();
}

inline void EventBaseLoopController::attachEventBase(EventBase& eventBase) {
  attachEventBase(eventBase.getVirtualEventBase());
}

inline void EventBaseLoopController::attachEventBase(
    VirtualEventBase& eventBase) {
  if (eventBaseAttached_.exchange(true)) {
    LOG(ERROR) << "Attempt to reattach EventBase to LoopController";
    return;
  }

  eventBase_ = &eventBase;

  CancellationSource source;
  eventBaseShutdownToken_ = source.getToken();
  eventBase_->runOnDestruction([source = std::move(source)] {
    source.requestCancellation();
  });

  if (awaitingScheduling_) {
    schedule();
  }
}

inline void EventBaseLoopController::setFiberManager(FiberManager* fm) {
  fm_ = fm;
}

inline void EventBaseLoopController::schedule() {
  if (eventBase_ == nullptr) {
    // In this case we need to postpone scheduling.
    awaitingScheduling_ = true;
  } else {
    // Schedule it to run in current iteration.

    if (!eventBaseKeepAlive_) {
      eventBaseKeepAlive_ = getKeepAliveToken(eventBase_);
    }
    eventBase_->getEventBase().runInLoop(&callback_, true);
    awaitingScheduling_ = false;
  }
}

inline void EventBaseLoopController::runLoop() {
  if (!eventBaseKeepAlive_) {
    // runLoop can be called twice if both schedule() and scheduleThreadSafe()
    // were called.
    if (!fm_->hasTasks()) {
      return;
    }
    eventBaseKeepAlive_ = getKeepAliveToken(eventBase_);
  }
  if (loopRunner_) {
    if (fm_->hasReadyTasks()) {
      loopRunner_->run([&] { fm_->loopUntilNoReadyImpl(); });
    }
  } else {
    fm_->loopUntilNoReadyImpl();
  }
  if (!fm_->hasTasks()) {
    eventBaseKeepAlive_.reset();
  }
}

inline void EventBaseLoopController::runEagerFiber(Fiber* fiber) {
  if (!eventBaseKeepAlive_) {
    eventBaseKeepAlive_ = getKeepAliveToken(eventBase_);
  }
  if (loopRunner_) {
    loopRunner_->run([&] { fm_->runEagerFiberImpl(fiber); });
  } else {
    fm_->runEagerFiberImpl(fiber);
  }
  if (!fm_->hasTasks()) {
    eventBaseKeepAlive_.reset();
  }
}

inline void EventBaseLoopController::scheduleThreadSafe() {
  /* The only way we could end up here is if
     1) Fiber thread creates a fiber that awaits (which means we must
        have already attached, fiber thread wouldn't be running).
     2) We move the promise to another thread (this move is a memory fence)
     3) We fulfill the promise from the other thread. */
  assert(eventBaseAttached_);

  // VirtualEventBase::runInEventBaseThread captures its own keep-alive token.
  eventBase_->runInEventBaseThread([this]() {
    if (fm_->shouldRunLoopRemote()) {
      return runLoop();
    }

    if (!fm_->hasTasks()) {
      eventBaseKeepAlive_.reset();
    }
  });
}

inline HHWheelTimer* EventBaseLoopController::timer() {
  assert(eventBaseAttached_);

  if (eventBaseShutdownToken_.isCancellationRequested()) [[unlikely]] {
    return nullptr;
  }

  return &eventBase_->timer();
}
} // namespace fibers
} // namespace folly
