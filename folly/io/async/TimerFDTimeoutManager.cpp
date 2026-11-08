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

#include <folly/io/async/TimerFDTimeoutManager.h>

namespace folly {
// TimerFDTimeoutManager
TimerFDTimeoutManager::TimerFDTimeoutManager(folly::EventBase* eventBase)
    : TimerFD(eventBase) {}

TimerFDTimeoutManager::~TimerFDTimeoutManager() {
  cancelAll();
  close();
}

void TimerFDTimeoutManager::onTimeout() noexcept {
  processExpiredTimers();
  scheduleNextTimer();
}

void TimerFDTimeoutManager::scheduleTimeout(
    Callback* callback, std::chrono::microseconds timeout) {
  cancelTimeout(callback);
  // Non-positive timeouts should fire as soon as possible. A zero timeout
  // disarms timerfd, while a negative timeout is rejected with EINVAL.
  if (timeout.count() <= 0) [[unlikely]] {
    timeout = std::chrono::microseconds(1);
  }
  auto expirationTime = getCurTime() + timeout;
  auto expirationTimeUsec =
      std::chrono::duration_cast<std::chrono::microseconds>(
          expirationTime.time_since_epoch());

  if (callbacks_.empty() || expirationTimeUsec < callbacks_.begin()->first) {
    schedule(timeout);
  }

  // now add the callback
  // handle entries that expire at the same time
  callbacks_[expirationTimeUsec].push_back(*callback);

  callback->setExpirationTime(this, expirationTimeUsec);
}

bool TimerFDTimeoutManager::cancelTimeout(Callback* callback) {
  if (!callback->is_linked()) {
    return false;
  }

  callback->unlink();
  callback->callbackCanceled();

  auto expirationTime = callback->getExpirationTime();

  auto iter = callbacks_.find(expirationTime);

  if (iter == callbacks_.end()) {
    return false;
  }

  bool removeFirst = (iter == callbacks_.begin());

  if (iter->second.empty()) {
    callbacks_.erase(iter);
  }

  // reschedule the timer if needed
  if (!processingExpired_ && removeFirst && !callbacks_.empty()) {
    auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        getCurTime().time_since_epoch());
    if (now > callbacks_.begin()->first) {
      auto timeout = now - callbacks_.begin()->first;

      schedule(timeout);
    }
  }

  if (callbacks_.empty()) {
    cancel();
  }

  return true;
}

size_t TimerFDTimeoutManager::cancelAll() {
  size_t ret = 0;
  while (!callbacks_.empty()) {
    auto iter = callbacks_.begin();
    auto callbackList = std::move(iter->second);
    callbacks_.erase(iter);

    while (!callbackList.empty()) {
      ++ret;
      auto* callback = &callbackList.front();
      callbackList.pop_front();
      callback->callbackCanceled();
    }
  }

  // and now the in progress list
  while (!inProgressList_.empty()) {
    ++ret;
    auto* callback = &inProgressList_.front();
    inProgressList_.pop_front();
    callback->callbackCanceled();
  }

  if (ret) {
    cancel();
  }
  return ret;
}

size_t TimerFDTimeoutManager::count() const {
  size_t ret = 0;
  for (const auto& c : callbacks_) {
    ret += c.second.size();
  }

  return ret;
}

void TimerFDTimeoutManager::processExpiredTimers() {
  processingExpired_ = true;
  while (true) {
    if (callbacks_.empty()) {
      break;
    }

    auto iter = callbacks_.begin();
    auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        getCurTime().time_since_epoch());
    if (now >= iter->first) {
      inProgressList_ = std::move(iter->second);
      callbacks_.erase(iter);

      CHECK(!inProgressList_.empty());

      while (!inProgressList_.empty()) {
        auto* callback = &inProgressList_.front();
        inProgressList_.pop_front();
        callback->timeoutExpired();
      }
    } else {
      break;
    }
  }
  processingExpired_ = false;
}

void TimerFDTimeoutManager::scheduleNextTimer() {
  if (callbacks_.empty()) {
    return;
  }

  auto iter = callbacks_.begin();
  auto now = std::chrono::duration_cast<std::chrono::microseconds>(
      getCurTime().time_since_epoch());

  if (iter->first > now) {
    schedule(iter->first - now);
  } else {
    // we schedule it here again to avoid the case
    // where a timer can cause starvation
    // by continuosly rescheduling itlsef
    schedule(std::chrono::microseconds(1));
  }
}

} // namespace folly
