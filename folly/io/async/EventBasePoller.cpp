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

#include <folly/io/async/EventBasePoller.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include <glog/logging.h>
#include <folly/FileUtil.h>
#include <folly/String.h>
#include <folly/io/async/Epoll.h>
#include <folly/io/async/IoUringSetupRetry.h>
#include <folly/io/async/Liburing.h>
#include <folly/lang/Align.h>
#include <folly/synchronization/Baton.h>

#if FOLLY_HAS_EPOLL
// @lint-ignore CLANGTIDY facebook-hte-PortabilityInclude-poll.h
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#endif

#if FOLLY_HAS_LIBURING
#include <liburing.h> // @manual
#endif

namespace folly::detail {

namespace {

template <class T>
class Queue {
 public:
  bool insert(T* t) {
    DCHECK(t->next == nullptr);

    auto oldHead = head_.load(std::memory_order_relaxed);
    bool ret;
    do {
      t->next = (ret = (oldHead == kQueueArmedTag())) ? nullptr : oldHead;
    } while (!head_.compare_exchange_weak(
        oldHead, t, std::memory_order_release, std::memory_order_relaxed));
    return ret;
  }

  T* arm() {
    T* oldHead = head_.load(std::memory_order_relaxed);
    T* newHead;
    T* ret;
    do {
      if (oldHead == nullptr || oldHead == kQueueArmedTag()) {
        newHead = kQueueArmedTag();
        ret = nullptr;
      } else {
        newHead = nullptr;
        ret = oldHead;
      }
    } while (!head_.compare_exchange_weak(
        oldHead,
        newHead,
        std::memory_order_acq_rel,
        std::memory_order_relaxed));

    return ret;
  }

 private:
  static T* kQueueArmedTag() { return reinterpret_cast<T*>(1); }

  std::atomic<T*> head_{kQueueArmedTag()};
};

#if FOLLY_HAS_EPOLL

class EventBasePollerImpl : public EventBasePoller {
 public:
  EventBasePollerImpl(Options options, bool rearmInline)
      : options_(options),
        rearmInline_(rearmInline),
        notificationEv_(
            Event::NotificationFd{}, ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
    PCHECK(notificationEv_.fd >= 0);
  }

  EventBasePollerImpl(const EventBasePollerImpl&) = delete;
  EventBasePollerImpl& operator=(const EventBasePollerImpl&) = delete;
  EventBasePollerImpl(EventBasePollerImpl&&) = delete;
  EventBasePollerImpl& operator=(EventBasePollerImpl&&) = delete;

  ~EventBasePollerImpl() override { fileops::close(notificationEv_.fd); }

  std::unique_ptr<Handle> add(int fd, void* userData) override {
    auto handle = std::make_unique<Event>(*this, fd, userData);
    handle->handoff(false);
    return handle;
  }

  void reclaim(std::unique_ptr<Handle> handle) override {
    static_cast<Event*>(handle.get())->join();
  }

  ReadyHandles wait() final;

  void shutdown() override {
    stop_ = true;
    notifyEvfd();
  }

 protected:
  struct Event final : public Handle {
    struct NotificationFd {};

    Event(EventBasePollerImpl& parent_, int fd_, void* userData)
        : Handle(userData), parent(&parent_), fd(fd_) {}
    // Special internal event to poll the notification eventfd.
    Event(NotificationFd, int fd_)
        : Handle(nullptr), parent(nullptr), fd(fd_) {}

    ~Event() override {
      CHECK(isNotificationFd() || joined_.ready())
          << "Handle must be reclaimed before destruction";
    }

    bool isNotificationFd() const { return parent == nullptr; }

    // TSAN does not recognize the happens-before relationship between rearming
    // (e.g. epoll_ctl(EPOLL_CTL_MOD)) and handling the ready event, so use a
    // fake mutex to introduce it and at the same time check it.
    FOLLY_ALWAYS_INLINE void markReady() {
#ifdef FOLLY_SANITIZE_THREAD
      CHECK(!ready_.exchange(true, std::memory_order_acq_rel));
#endif
    }
    FOLLY_ALWAYS_INLINE void markProcessed() {
#ifdef FOLLY_SANITIZE_THREAD
      CHECK(ready_.exchange(false, std::memory_order_release));
#endif
    }

    void handoff(bool done) override;

    void handleHandoff();
    void join();

    EventBasePollerImpl* parent;
    const int fd;
    bool registered = false; // Managed by addEvent()/delEvent().
    Event* next{nullptr}; // Managed by Queue.

   private:
    bool joining_ = false;
    Baton<> joined_;
#ifdef FOLLY_SANITIZE_THREAD
    std::atomic<bool> ready_{true};
#endif
  };

  virtual void addEvent(Event* event) = 0;
  virtual void delEvent(Event* event) = 0;
  // Appends the ready events to readyEvents; returns false if there are none.
  virtual bool waitForEvents(std::vector<Event*>& readyEvents) = 0;

  void handleNotification();

  const Options options_;

 private:
  void notifyEvfd();
  void returnEvent(Event* event);

  const bool rearmInline_;
  std::vector<Event*> readyEvents_;
  std::atomic<bool> stop_{false};
  Event notificationEv_;
  Queue<Event> returnQueue_;
};

void EventBasePollerImpl::Event::handoff(bool done) {
  DCHECK(!isNotificationFd());
  CHECK(!joining_);
  joining_ = done;
  if (parent->rearmInline_) {
    handleHandoff();
  } else {
    parent->returnEvent(this);
  }
}

void EventBasePollerImpl::Event::handleHandoff() {
  DCHECK(!isNotificationFd());
  if (joining_) {
    parent->delEvent(this);
    joined_.post();
    return;
  }

  markProcessed();
  parent->addEvent(this);
}

void EventBasePollerImpl::Event::join() {
  DCHECK(!isNotificationFd());
  joined_.wait();
}

void EventBasePollerImpl::notifyEvfd() {
  uint64_t val = 1;
  auto ret = writeNoInt(notificationEv_.fd, &val, sizeof(val));
  PCHECK(ret == sizeof(val)) << ret;
}

void EventBasePollerImpl::returnEvent(Event* event) {
  if (returnQueue_.insert(event)) {
    notifyEvfd();
  }
}

void EventBasePollerImpl::handleNotification() {
  while (auto* event = returnQueue_.arm()) {
    while (event) {
      auto* next = std::exchange(event->next, nullptr);
      event->handleHandoff();
      event = next;
    }
  }
  notificationEv_.markProcessed();
  addEvent(&notificationEv_);
}

EventBasePoller::ReadyHandles EventBasePollerImpl::wait() {
  ReadyHandles result;

  while (true) {
    if (stop_.load(std::memory_order_relaxed)) {
      return result;
    }

    if (!waitForEvents(readyEvents_)) {
      // Spurious wake-up or signal interruption; retry.
      continue;
    }

    for (auto* event : readyEvents_) {
      event->markReady();
      if (event->isNotificationFd()) {
        handleNotification();
      } else {
        result.push_back(event);
      }
    }
    readyEvents_.clear();

    if (!result.empty()) {
      return result;
    }
    // Only the notification fd was ready; wait again.
  }
}

class EventBasePollerEpoll final : public EventBasePollerImpl {
 public:
  explicit EventBasePollerEpoll(Options options)
      : EventBasePollerImpl(options, options.epollRearmInline),
        epFd_(::epoll_create1(EPOLL_CLOEXEC)),
        epollEvents_(options_.epollMaxEvents) {
    PCHECK(epFd_ >= 0);
    handleNotification(); // Arm notificationEv_.
  }

  ~EventBasePollerEpoll() override { fileops::close(epFd_); }

  void addEvent(Event* event) override {
    if (event->isNotificationFd() && event->registered) {
      return; // notificationEv_ is persistent.
    }

    epoll_event ev = {};
    ev.data.ptr = event;
    ev.events = EPOLLIN;

    int op = EPOLL_CTL_ADD;
    if (!event->isNotificationFd()) {
      ev.events |= EPOLLONESHOT;
      if (!event->registered) [[unlikely]] {
        event->registered = true;
      } else {
        op = EPOLL_CTL_MOD;
      }
    } else {
      // Use edge triggering, so we don't need to drain the eventfd when ready.
      ev.events |= EPOLLET;
      event->registered = true;
    }

    auto ret = ::epoll_ctl(epFd_, op, event->fd, &ev);
    PCHECK(ret == 0);
  }

  void delEvent(Event* event) override {
    CHECK(!event->isNotificationFd());
    if (!event->registered) {
      return;
    }

    auto ret = ::epoll_ctl(epFd_, EPOLL_CTL_DEL, event->fd, nullptr);
    PCHECK(ret == 0);
  }

  bool waitForEvents(std::vector<Event*>& readyEvents) override {
    const int maxEvents = static_cast<int>(epollEvents_.size());
    int ret;

    auto spinUntil = std::chrono::steady_clock::now() + options_.spinTimeout;
    do {
      ret = ::epoll_wait(epFd_, epollEvents_.data(), maxEvents, 0);
    } while (ret <= 0 && std::chrono::steady_clock::now() < spinUntil);

    if (ret <= 0) {
      if (auto sleepUs = options_.sleepBeforeBlock.count(); sleepUs > 0) {
        /* sleep override */
        std::this_thread::sleep_for(options_.sleepBeforeBlock);
      }
      ret = ::epoll_wait(epFd_, epollEvents_.data(), maxEvents, -1);
    }

    if (ret <= 0) {
      PCHECK(ret == 0 || errno == EINTR);
      return false;
    }

    for (int i = 0; i < ret; ++i) {
      readyEvents.push_back(
          CHECK_NOTNULL(reinterpret_cast<Event*>(epollEvents_[i].data.ptr)));
    }
    return true;
  }

 private:
  const int epFd_;
  std::vector<struct epoll_event> epollEvents_;
};

#if FOLLY_HAS_LIBURING

class EventBasePollerIoUring final : public EventBasePollerImpl {
 public:
  explicit EventBasePollerIoUring(Options options)
      // io_uring does not support concurrent submissions.
      : EventBasePollerImpl(options, /* rearmInline */ false) {
    struct io_uring_params params{};
    // Consecutive wait() calls may come from different threads, so
    // SINGLE_ISSUER and DEFER_TASKRUN cannot be used, and COOP_TASKRUN would
    // delay completions until the submitting thread enters the kernel.
    CHECK_LE(options_.ioUringSqEntries, std::numeric_limits<unsigned>::max());
    const auto ret = retryTransientIoUringSetup([&] {
      ::memset(&ring_, 0, sizeof(ring_));
      ::memset(&params, 0, sizeof(params));
      return ::io_uring_queue_init_params(
          static_cast<unsigned>(options_.ioUringSqEntries), &ring_, &params);
    });
    CHECK_EQ(ret, 0) << "Error creating io_uring: " << folly::errnoStr(-ret);
    handleNotification(); // Arm notificationEv_.
  }

  ~EventBasePollerIoUring() override { ::io_uring_queue_exit(&ring_); }

  void addEvent(Event* event) override {
    auto* sqe = ::io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
      submitPendingSqes();
      sqe = ::io_uring_get_sqe(&ring_);
      // Only the thread in wait() touches the ring, so the SQ is empty after a
      // submit.
      CHECK(sqe != nullptr);
    }
    ++numPendingSqes_;

    ::io_uring_sqe_set_data(sqe, event);
    if (event->isNotificationFd()) {
      ::io_uring_prep_read(
          sqe, event->fd, &eventFdBuf_, sizeof(eventFdBuf_), 0);
    } else {
      ::io_uring_prep_poll_add(sqe, event->fd, POLLIN);
    }
  }

  void delEvent(Event* /* event */) override {
    // Nothing to do, no events are persistent.
  }

  bool waitForEvents(std::vector<Event*>& readyEvents) override {
    if (numPendingSqes_ > 0) {
      submitPendingSqes();
    }

    int ret;
    struct io_uring_cqe* cqe = nullptr;

    auto spinUntil = std::chrono::steady_clock::now() + options_.spinTimeout;
    do {
      ret = ::io_uring_peek_cqe(&ring_, &cqe);
    } while (ret != 0 && std::chrono::steady_clock::now() < spinUntil);

    if (auto sleepUs = options_.sleepBeforeBlock.count();
        ret != 0 && sleepUs > 0) {
      // Simulate a sleep + peek by waiting for infinite events with a timeout.
      struct __kernel_timespec timeout;
      timeout.tv_sec = sleepUs / 1'000'000;
      timeout.tv_nsec = (sleepUs % 1'000'000) * 1'000;
      ret = ::io_uring_wait_cqes(
          &ring_,
          &cqe,
          std::numeric_limits<unsigned>::max(),
          &timeout,
          nullptr);
    }

    if (ret != 0 || cqe == nullptr) {
      // No luck, do an unbounded wait.
      ret = ::io_uring_wait_cqe(&ring_, &cqe);
    }

    if (ret != 0 || cqe == nullptr) {
      CHECK(ret == 0 || ret == -EINTR) << errnoStr(-ret);
      return false;
    }

    DCHECK(readyEvents.empty());
    unsigned head;
    io_uring_for_each_cqe(&ring_, head, cqe) {
      auto* event =
          CHECK_NOTNULL(static_cast<Event*>(io_uring_cqe_get_data(cqe)));
      if (event->isNotificationFd()) {
        CHECK_EQ(cqe->res, sizeof(eventFdBuf_)) << errnoStr(-cqe->res);
      } else {
        CHECK_GE(cqe->res, 0) << errnoStr(-cqe->res);
      }
      readyEvents.push_back(event);
    }
    ::io_uring_cq_advance(&ring_, readyEvents.size());

    return true;
  }

 private:
  void submitPendingSqes() {
    auto ret = ::io_uring_submit(&ring_);
    CHECK_EQ(ret, numPendingSqes_);
    numPendingSqes_ = 0;
  }

  struct io_uring ring_;
  size_t numPendingSqes_ = 0;
  alignas(cacheline_align_v) uint64_t eventFdBuf_;
};

#endif // FOLLY_HAS_LIBURING

#endif // FOLLY_HAS_EPOLL

} // namespace

EventBasePoller::Handle::~Handle() = default;

EventBasePoller::~EventBasePoller() = default;

/* static */ std::unique_ptr<EventBasePoller> EventBasePoller::create(
    [[maybe_unused]] Options options) {
#if FOLLY_HAS_EPOLL
  if (options.backend == Options::Backend::kEpoll) {
    if (options.epollMaxEvents == 0 ||
        options.epollMaxEvents > std::numeric_limits<int>::max()) {
      throw std::invalid_argument("epollMaxEvents must be in [1, INT_MAX]");
    }
    return std::make_unique<EventBasePollerEpoll>(options);
  }
#endif
#if FOLLY_HAS_EPOLL && FOLLY_HAS_LIBURING
  if (options.backend == Options::Backend::kIoUring) {
    return std::make_unique<EventBasePollerIoUring>(options);
  }
#endif
  throw std::invalid_argument("Unsupported EventBasePoller backend");
}

} // namespace folly::detail
