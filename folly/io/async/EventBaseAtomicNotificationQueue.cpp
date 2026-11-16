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

#include <folly/io/async/EventBaseAtomicNotificationQueue.h>

#include <algorithm>
#include <cerrno>
#include <utility>

#include <folly/FileUtil.h>
#include <folly/ScopeGuard.h>
#include <folly/net/NetOps.h>
#include <folly/net/NetworkSocket.h>
#include <folly/system/Pid.h>

namespace folly::detail {
namespace {

class NotificationQueueErrorState {
 public:
  NotificationQueueErrorState() = default;
  NotificationQueueErrorState(const NotificationQueueErrorState&) = delete;
  NotificationQueueErrorState& operator=(const NotificationQueueErrorState&) =
      delete;

  ~NotificationQueueErrorState() {
#ifdef _WIN32
    WSASetLastError(winLastError_);
#endif
    errno = errno_;
  }

 private:
  const int errno_{errno};
#ifdef _WIN32
  const int winLastError_{WSAGetLastError()};
#endif
};

} // namespace

void EventBaseAtomicNotificationQueueBase::closePipe(int (&fds)[2]) {
  for (int& fd : fds) {
    if (fd >= 0) {
      fileops::close(fd);
      fd = -1;
    }
  }
}

bool EventBaseAtomicNotificationQueueBase::createPipe(int (&fds)[2]) {
  if (fileops::pipe(fds) != 0) {
    return false;
  }
  auto rollback = makeGuard([&] {
    NotificationQueueErrorState errorState;
    closePipe(fds);
  });
  for (int fd : fds) {
    if (fd < 0 ||
        netops::set_socket_close_on_exec(NetworkSocket::fromFd(fd)) != 0) {
      return false;
    }
  }
  if (fcntl(fds[0], F_SETFL, O_RDONLY | O_NONBLOCK) != 0 ||
      fcntl(fds[1], F_SETFL, O_WRONLY | O_NONBLOCK) != 0) {
    return false;
  }
  rollback.dismiss();
  return true;
}

EventBaseAtomicNotificationQueueBase::EventBaseAtomicNotificationQueueBase()
    : pid_(get_cached_pid()) {
#if !defined(_WIN32) && __has_include(<sys/eventfd.h>)
  eventfd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (eventfd_ == -1) {
    auto errno_ = errno;
    if (errno_ == ENOSYS || errno_ == EINVAL) {
      // eventfd not availalble
      LOG(ERROR)
          << "failed to create eventfd for AtomicNotificationQueue: " << errno_
          << ", falling back to pipe mode (is your kernel " << "> 2.6.30?)";
    } else {
      folly::throwSystemError(
          errno, "failed to create eventfd for AtomicNotificationQueue");
    }
  }
#endif
  if (eventfd_ != -1) {
    return;
  }
  if (!createPipe(pipeFds_)) {
#ifdef _WIN32
    folly::throwSystemError(
        WSAGetLastError(), "could not create Windows notification pipe");
#else
    folly::throwSystemError(
        errno, "failed to create pipe for AtomicNotificationQueue");
#endif
  }
#ifdef _WIN32
  winPipeEnabled_ = true;
#endif
}

EventBaseAtomicNotificationQueueBase::~EventBaseAtomicNotificationQueueBase() {
#ifdef _WIN32
  stopConsuming();
  winRetirePipe();
#else
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
  closePipe(pipeFds_);
#endif
}

void EventBaseAtomicNotificationQueueBase::stopConsuming() {
#ifdef _WIN32
  EventBase::OnDestructionCallback::cancel();
#endif
  detachConsuming();
}

void EventBaseAtomicNotificationQueueBase::detachConsuming() {
#ifdef _WIN32
  ++winAttachmentEpoch_;
  AsyncTimeout::cancelTimeout();
  AsyncTimeout::detachEventBase();
#endif
  cancelLoopCallback();
  unregisterHandler();
  EventHandler::detachEventBase();
  evb_ = nullptr;
}

void EventBaseAtomicNotificationQueueBase::startConsumingImpl(
    EventBase* evb, bool internal) {
  evb_ = evb;
#ifdef _WIN32
  ++winAttachmentEpoch_;
  winInternal_ = internal;
  winRetryAt_ = {};
  winRetryDelay_ = 100;
  AsyncTimeout::attachEventBase(
      evb,
      internal ? AsyncTimeout::InternalEnum::INTERNAL
               : AsyncTimeout::InternalEnum::NORMAL);
  initHandler(
      evb,
      pipeFds_[0] >= 0 ? NetworkSocket::fromFd(pipeFds_[0]) : NetworkSocket());
  if (!winIsPipeBroken()) {
    if (!registerReadHandler(internal)) {
      winRetirePipe();
    }
  }
  evb->runOnDestruction(*this);
  winScheduleRecoveryTick();
  ++writesLocal_;
  notifyFd();
#else
  initHandler(
      evb_, NetworkSocket::fromFd(eventfd_ >= 0 ? eventfd_ : pipeFds_[0]));
  if (registerReadHandler(internal)) {
    edgeTriggeredSet_ = eventfd_ >= 0 && setEdgeTriggered();
    ++writesLocal_;
    notifyFd();
  } else {
    edgeTriggeredSet_ = false;
  }
#endif
}

bool EventBaseAtomicNotificationQueueBase::registerReadHandler(bool internal) {
  return internal
      ? registerInternalHandler(READ | PERSIST)
      : registerHandler(READ | PERSIST);
}

void EventBaseAtomicNotificationQueueBase::notifyFd() {
  checkPid();
#ifdef _WIN32
  std::lock_guard lock(winPipeMutex_);
  if (!winPipeEnabled_) {
    return;
  }
  const auto writer = NetworkSocket::fromFd(pipeFds_[1]);
  const char signal = 1;
  ssize_t result;
  do {
    result = netops::send(writer, &signal, sizeof(signal), 0);
  } while (result < 0 && errno == EINTR);
  if (result == 1) {
    ++winSignalDebt_;
    return;
  }
  const int error = errno;
  if (result < 0 && (error == EAGAIN || error == EWOULDBLOCK)) {
    return;
  }
  if (result < 0 && error == WSAECONNRESET) {
    winPipeEnabled_ = false;
    return;
  }
  folly::throwSystemError(error, "failed to signal Windows notification pipe");
#else
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
#endif
}

void EventBaseAtomicNotificationQueueBase::drainFd() {
  checkPid();
#ifdef _WIN32
  std::unique_lock lock(winPipeMutex_);
  if (!winPipeEnabled_) {
    lock.unlock();
    unregisterHandler();
    return;
  }
  const auto reader = NetworkSocket::fromFd(pipeFds_[0]);
  char signals[64];
  while (true) {
    const auto result = netops::recv(reader, signals, sizeof(signals), 0);
    if (result > 0) {
      CHECK_GE(winSignalDebt_, static_cast<size_t>(result));
      winSignalDebt_ -= static_cast<size_t>(result);
      writesObserved_ += result;
      continue;
    }
    const int error = errno;
    if (result < 0 && error == EINTR) {
      continue;
    }
    if (result < 0 && (error == EAGAIN || error == EWOULDBLOCK)) {
      return;
    }
    if (result == 0 || (result < 0 && error == WSAECONNRESET)) {
      winPipeEnabled_ = false;
      lock.unlock();
      unregisterHandler();
      return;
    }
    folly::throwSystemError(error, "failed to drain Windows notification pipe");
  }
#else
  uint64_t message = 0;
  if (eventfd_ >= 0) {
    auto result = readNoInt(eventfd_, &message, sizeof(message));
    auto errno_ = errno;
    CHECK(
        result == (int)sizeof(message) || errno_ == EAGAIN ||
        errno_ == EWOULDBLOCK)
        << "result = " << result << "; errno = " << errno_;
    writesObserved_ += message;
  } else {
    ssize_t result;
    while ((result = readNoInt(pipeFds_[0], &message, sizeof(message))) != -1) {
      writesObserved_ += result;
    }
    auto errno_ = errno;
    CHECK(result == -1 && (errno_ == EAGAIN || errno_ == EWOULDBLOCK))
        << "result = " << result << "; errno = " << errno_;
  }
#endif
}

void EventBaseAtomicNotificationQueueBase::activateEvent() {
  if (!EventHandler::activateEvent(0)) {
    // Fallback for EventBase backends that don't support activateEvent
    ++writesLocal_;
    notifyFd();
  }
}

void EventBaseAtomicNotificationQueueBase::onEventBaseDestruction() noexcept {
#ifdef _WIN32
  detachConsuming();
#endif
}

#ifdef _WIN32

bool EventBaseAtomicNotificationQueueBase::winIsPipeBroken() const {
  std::lock_guard lock(winPipeMutex_);
  return !winPipeEnabled_;
}

void EventBaseAtomicNotificationQueueBase::winRetirePipe() {
  std::lock_guard lock(winPipeMutex_);
  winPipeEnabled_ = false;
  closePipe(pipeFds_);
  winSignalDebt_ = 0;
}

void EventBaseAtomicNotificationQueueBase::winScheduleRecoveryTick() {
  CHECK(AsyncTimeout::scheduleTimeout(100, nullptr))
      << "could not schedule notification queue recovery watchdog";
}

void EventBaseAtomicNotificationQueueBase::winRepairPipe() {
  const auto now = std::chrono::steady_clock::now();
  if (!winIsPipeBroken() || now < winRetryAt_) {
    return;
  }
  unregisterHandler();
  winRetirePipe();
  int replacement[2]{-1, -1};
  if (createPipe(replacement)) {
    auto rollback = makeGuard([&] { closePipe(replacement); });
    {
      std::lock_guard lock(winPipeMutex_);
      std::swap(pipeFds_[0], replacement[0]);
      std::swap(pipeFds_[1], replacement[1]);
    }
    rollback.dismiss();
    changeHandlerFD(NetworkSocket::fromFd(pipeFds_[0]));
    if (registerReadHandler(winInternal_)) {
      {
        std::lock_guard lock(winPipeMutex_);
        winPipeEnabled_ = true;
      }
      winRetryDelay_ = 100;
      winRetryAt_ = {};
      return;
    }
    winRetirePipe();
  }
  winRetryAt_ = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(winRetryDelay_);
  winRetryDelay_ = std::min(winRetryDelay_ * 2, uint32_t{1000});
}

#endif

void EventBaseAtomicNotificationQueueBase::checkPid() const {
  if (pid_ != get_cached_pid()) [[unlikely]] {
    checkPidFail();
  }
}

[[noreturn]] FOLLY_NOINLINE void
EventBaseAtomicNotificationQueueBase::checkPidFail() const {
  folly::terminate_with<std::runtime_error>(
      "pid mismatch: pid = " + folly::to<std::string>(get_cached_pid()) +
      ", expecting " + folly::to<std::string>(pid_));
}

} // namespace folly::detail
