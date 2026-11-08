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

#include <folly/FileUtil.h>
#include <folly/system/Pid.h>

namespace folly {

template <typename Task, typename Consumer>
EventBaseAtomicNotificationQueue<Task, Consumer>::
    EventBaseAtomicNotificationQueue(Consumer&& consumer)
    : pid_(get_cached_pid()),
      notificationQueue_(),
      consumer_(std::move(consumer)) {
#if __has_include(<sys/eventfd.h>)
  eventfd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (eventfd_ == -1) {
    auto errno_ = errno;
    if (errno_ == ENOSYS || errno_ == EINVAL) {
      // eventfd not availalble
      LOG(ERROR)
          << "failed to create eventfd for AtomicNotificationQueue: " << errno_
          << ", falling back to pipe mode (is your kernel " << "> 2.6.30?)";
    } else {
      // some other error
      folly::throwSystemError(
          errno, "Failed to create eventfd for AtomicNotificationQueue");
    }
  }
#endif
  if (eventfd_ == -1) {
    if (fileops::pipe(pipeFds_)) {
      folly::throwSystemError(
          errno, "Failed to create pipe for AtomicNotificationQueue");
    }
    try {
      // put both ends of the pipe into non-blocking mode
      if (fcntl(pipeFds_[0], F_SETFL, O_RDONLY | O_NONBLOCK) != 0) {
        folly::throwSystemError(
            errno,
            "failed to put AtomicNotificationQueue pipe read "
            "endpoint into non-blocking mode");
      }
      if (fcntl(pipeFds_[1], F_SETFL, O_WRONLY | O_NONBLOCK) != 0) {
        folly::throwSystemError(
            errno,
            "failed to put AtomicNotificationQueue pipe write "
            "endpoint into non-blocking mode");
      }
    } catch (...) {
      fileops::close(pipeFds_[0]);
      fileops::close(pipeFds_[1]);
      throw;
    }
  }
}

template <typename Task, typename Consumer>
EventBaseAtomicNotificationQueue<Task, Consumer>::
    ~EventBaseAtomicNotificationQueue() {
  // discard pending tasks and disarm the queue
  while (drive([](Task&&) {
    return AtomicNotificationQueueTaskStatus::DISCARD;
  })) {
  }

  // We must unregister before closing the fd. Otherwise the base class
  // would unregister the fd after it's already closed, which is invalid
  // (some other thread could've opened something that reused the fd).
  unregisterHandler();

  // Don't drain fd in the child process.
  if (pid_ == get_cached_pid()) {
    // Wait till we observe all the writes before closing fds
    while (writesObserved_ <
           (successfulArmCount_ - consumerDisarmedCount_) + writesLocal_) {
      drainFd();
    }
    DCHECK(
        writesObserved_ ==
        (successfulArmCount_ - consumerDisarmedCount_) + writesLocal_);
  }
  if (eventfd_ >= 0) {
    fileops::close(eventfd_);
    eventfd_ = -1;
  }
  if (pipeFds_[0] >= 0) {
    fileops::close(pipeFds_[0]);
    pipeFds_[0] = -1;
  }
  if (pipeFds_[1] >= 0) {
    fileops::close(pipeFds_[1]);
    pipeFds_[1] = -1;
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
  evb_ = nullptr;
  cancelLoopCallback();
  unregisterHandler();
  detachEventBase();
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
void EventBaseAtomicNotificationQueue<Task, Consumer>::startConsumingImpl(
    EventBase* evb, bool internal) {
  evb_ = evb;
  initHandler(
      evb_,
      folly::NetworkSocket::fromFd(eventfd_ >= 0 ? eventfd_ : pipeFds_[0]));
  auto registerHandlerResult = internal
      ? registerInternalHandler(READ | PERSIST)
      : registerHandler(READ | PERSIST);
  if (registerHandlerResult) {
    edgeTriggeredSet_ = eventfd_ >= 0 && setEdgeTriggered();
    ++writesLocal_;
    notifyFd();
  } else {
    edgeTriggeredSet_ = false;
  }
}

template <typename Task, typename Consumer>
void EventBaseAtomicNotificationQueue<Task, Consumer>::notifyFd() {
  checkPid();

  ssize_t bytes_written = 0;
  size_t bytes_expected = 0;

  do {
    if (eventfd_ >= 0) {
      // eventfd(2) dictates that we must write a 64-bit integer
      uint64_t signal = 1;
      bytes_expected = sizeof(signal);
      bytes_written = fileops::write(eventfd_, &signal, bytes_expected);
    } else {
      uint8_t signal = 1;
      bytes_expected = sizeof(signal);
      bytes_written = fileops::write(pipeFds_[1], &signal, bytes_expected);
    }
  } while (bytes_written == -1 && errno == EINTR);

  if (bytes_written != ssize_t(bytes_expected)) {
    folly::throwSystemError(
        errno,
        "failed to signal AtomicNotificationQueue after "
        "write");
  }
}

template <typename Task, typename Consumer>
void EventBaseAtomicNotificationQueue<Task, Consumer>::drainFd() {
  checkPid();

  uint64_t message = 0;
  if (eventfd_ >= 0) {
    auto result = readNoInt(eventfd_, &message, sizeof(message));
    auto errno_ = errno;
#ifndef _WIN32
    CHECK(
        result == (int)sizeof(message) || errno_ == EAGAIN ||
        errno_ == EWOULDBLOCK)
#else
    CHECK(
        result == (int)sizeof(message) || errno_ == EAGAIN ||
        errno_ == EWOULDBLOCK || errno_ == WSAECONNRESET)
#endif
        << "result = " << result << "; errno = " << errno_;
    writesObserved_ += message;
  } else {
    ssize_t result;
    while ((result = readNoInt(pipeFds_[0], &message, sizeof(message))) != -1) {
      writesObserved_ += result;
    }
    auto errno_ = errno;
#ifndef _WIN32
    CHECK(result == -1 && (errno_ == EAGAIN || errno_ == EWOULDBLOCK))
#else
    CHECK(
        result == -1 && (errno_ == EAGAIN || errno_ == EWOULDBLOCK) ||
        errno_ == WSAECONNRESET)
#endif
        << "result = " << result << "; errno = " << errno_;
  }
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

template <typename Task, typename Consumer>
void EventBaseAtomicNotificationQueue<Task, Consumer>::activateEvent() {
  if (!EventHandler::activateEvent(0)) {
    // Fallback for EventBase backends that don't support activateEvent
    ++writesLocal_;
    notifyFd();
  }
}

template <typename Task, typename Consumer>
void EventBaseAtomicNotificationQueue<Task, Consumer>::checkPid() const {
  if (pid_ != get_cached_pid()) [[unlikely]] {
    checkPidFail();
  }
}

template <typename Task, typename Consumer>
[[noreturn]] FOLLY_NOINLINE void
EventBaseAtomicNotificationQueue<Task, Consumer>::checkPidFail() const {
  folly::terminate_with<std::runtime_error>(
      "Pid mismatch. Pid = " + folly::to<std::string>(get_cached_pid()) +
      ". Expecting " + folly::to<std::string>(pid_));
}
} // namespace folly
