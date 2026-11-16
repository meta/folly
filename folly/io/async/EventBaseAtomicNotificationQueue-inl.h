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

namespace folly {

template <typename Task, typename Consumer>
EventBaseAtomicNotificationQueue<Task, Consumer>::
    EventBaseAtomicNotificationQueue(Consumer&& consumer)
    : notificationQueue_(), consumer_(std::move(consumer)) {}

template <typename Task, typename Consumer>
EventBaseAtomicNotificationQueue<Task, Consumer>::
    ~EventBaseAtomicNotificationQueue() {
  // discard pending tasks and disarm the queue
  while (drive([](Task&&) {
    return AtomicNotificationQueueTaskStatus::DISCARD;
  })) {
  }
}

template <typename Task, typename Consumer>
uint32_t EventBaseAtomicNotificationQueue<Task, Consumer>::getMaxReadAtOnce()
    const {
  return notificationQueue_.getMaxReadAtOnce();
}
template <typename Task, typename Consumer>
void EventBaseAtomicNotificationQueue<Task, Consumer>::setMaxReadAtOnce(
    uint32_t maxAtOnce) {
  notificationQueue_.setMaxReadAtOnce(maxAtOnce);
}
template <typename Task, typename Consumer>
size_t EventBaseAtomicNotificationQueue<Task, Consumer>::size() const {
  return notificationQueue_.size();
}
template <typename Task, typename Consumer>
bool EventBaseAtomicNotificationQueue<Task, Consumer>::empty() const {
  return notificationQueue_.empty();
}

template <typename Task, typename Consumer>
void EventBaseAtomicNotificationQueue<Task, Consumer>::drain() {
  while (drive(consumer_)) {
  }
}

template <typename Task, typename Consumer>
template <typename... Args>
void EventBaseAtomicNotificationQueue<Task, Consumer>::putMessage(
    Args&&... args) {
  if (notificationQueue_.push(std::forward<Args>(args)...)) {
    notifyFd();
  }
}

template <typename Task, typename Consumer>
bool EventBaseAtomicNotificationQueue<Task, Consumer>::tryPutMessage(
    Task&& task, uint32_t maxSize) {
  auto result = notificationQueue_.tryPush(std::forward<Task>(task), maxSize);
  if (result ==
      AtomicNotificationQueue<Task>::TryPushResult::SUCCESS_AND_ARMED) {
    notifyFd();
  }
  return result !=
      AtomicNotificationQueue<Task>::TryPushResult::FAILED_LIMIT_REACHED;
}

template <typename Task, typename Consumer>
void EventBaseAtomicNotificationQueue<Task, Consumer>::stopConsuming() {
  detail::EventBaseAtomicNotificationQueueBase::stopConsuming();
}

template <typename Task, typename Consumer>
void EventBaseAtomicNotificationQueue<Task, Consumer>::startConsuming(
    EventBase* evb) {
  startConsumingImpl(evb, false);
}

template <typename Task, typename Consumer>
void EventBaseAtomicNotificationQueue<Task, Consumer>::startConsumingInternal(
    EventBase* evb) {
  startConsumingImpl(evb, true);
}

template <typename Task, typename Consumer>
void EventBaseAtomicNotificationQueue<Task, Consumer>::
    runLoopCallback() noexcept {
  DCHECK(!armed_);
  if (!notificationQueue_.arm()) {
    activateEvent();
  } else {
    armed_ = true;
    successfulArmCount_++;
  }
}

template <typename Task, typename Consumer>
template <typename T>
bool EventBaseAtomicNotificationQueue<Task, Consumer>::drive(T&& consumer) {
  auto wasEmpty = !notificationQueue_.drive(std::forward<T>(consumer));
  if (wasEmpty && armed_) {
    consumerDisarmedCount_++;
  }
  armed_ = false;
  return !wasEmpty;
}

template <typename Task, typename Consumer>
void EventBaseAtomicNotificationQueue<Task, Consumer>::handlerReady(
    uint16_t) noexcept {
  execute();
}

template <typename Task, typename Consumer>
void EventBaseAtomicNotificationQueue<Task, Consumer>::execute() {
  if (!edgeTriggeredSet_) {
    drainFd();
  }
  drive(consumer_);
  evb_->runInLoop(this, false, nullptr);
}

} // namespace folly
