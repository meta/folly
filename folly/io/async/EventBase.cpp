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

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <folly/io/async/EventBase.h>

#include <fcntl.h>

#include <memory>
#include <mutex>
#include <thread>

#include <folly/Chrono.h>
#include <folly/ExceptionString.h>
#include <folly/Memory.h>
#include <folly/String.h>
#include <folly/io/async/EventBaseAtomicNotificationQueue.h>
#include <folly/io/async/EventBaseBackendBase.h>
#include <folly/io/async/EventBaseLocal.h>
#include <folly/io/async/VirtualEventBase.h>
#include <folly/lang/Assume.h>
#include <folly/portability/Unistd.h>
#include <folly/synchronization/Baton.h>
#include <folly/synchronization/EventCount.h>
#include <folly/system/ThreadId.h>
#include <folly/system/ThreadName.h>

#if defined(__linux__) && !FOLLY_MOBILE
#define FOLLY_USE_EPOLLET

#include <sys/epoll.h>

struct event_base {
  void* evsel;
  void* evbase;
};

struct epollop {
  void* fds;
  int nfds;
  void* events;
  int nevents;
  int epfd;
};
#endif

namespace {

class EventBaseBackend : public folly::EventBaseBackendBase {
 public:
  EventBaseBackend();
  explicit EventBaseBackend(event_base* evb);
  ~EventBaseBackend() override;

  event_base* getEventBase() override { return evb_; }

  int eb_event_base_loop(int flags) override;
  int eb_event_base_loopbreak() override;
  void eb_yield_after_internal_event() override;

  int eb_event_add(Event& event, const struct timeval* timeout) override;
  int eb_event_del(EventBaseBackendBase::Event& event) override;

  bool eb_event_active(Event& event, int res) override;

  bool setEdgeTriggered(Event& event) override;

 private:
  void initYieldEvent();

  static void yieldCallback(folly::libevent_fd_t, short, void*) {}

  event_base* evb_;
  // See eb_yield_after_internal_event(). Assigned but never added.
  struct event yieldEvent_{};
};

EventBaseBackend::EventBaseBackend() {
  evb_ = event_base_new();
  initYieldEvent();
}

EventBaseBackend::EventBaseBackend(event_base* evb) : evb_(evb) {
  if (evb_ == nullptr) [[unlikely]] {
    LOG(ERROR) << "EventBase(): Pass nullptr as event base.";
    throw std::invalid_argument("EventBase(): event base cannot be nullptr");
  }
  initYieldEvent();
}

void EventBaseBackend::initYieldEvent() {
  event_set(&yieldEvent_, -1, 0, &EventBaseBackend::yieldCallback, nullptr);
  event_base_set(evb_, &yieldEvent_);
}

int EventBaseBackend::eb_event_base_loop(int flags) {
  return event_base_loop(evb_, flags);
}

int EventBaseBackend::eb_event_base_loopbreak() {
  return event_base_loopbreak(evb_);
}

void EventBaseBackend::eb_yield_after_internal_event() {
  // This is loopexit(), but faster. Activating a non-internal event is how
  // loopexit() gets event_base_loop() to return from EVLOOP_ONCE; it allocates
  // an ephemeral event to do that, we keep ours around. Since yieldEvent_ is
  // never added it does not count towards event_haveevents(), so it cannot
  // keep the loop alive either.
  event_active(&yieldEvent_, EV_TIMEOUT, 1);
}

int EventBaseBackend::eb_event_add(
    Event& event, const struct timeval* timeout) {
  return event_add(event.getEvent(), timeout);
}

int EventBaseBackend::eb_event_del(EventBaseBackendBase::Event& event) {
  return event_del(event.getEvent());
}

bool EventBaseBackend::eb_event_active(Event& event, int res) {
  event_active(event.getEvent(), res, 1);
  return true;
}

bool EventBaseBackend::setEdgeTriggered(Event& event) {
#ifdef FOLLY_USE_EPOLLET
  // Until v2 libevent doesn't expose API to set edge-triggered flag for events.
  // If epoll backend is used by libevent, we can enable it though epoll_ctl
  // directly.
  // Note that this code depends on internal event_base and epollop layout, so
  // we have to validate libevent version.
  static const bool supportedVersion =
      !strcmp(event_get_version(), "1.4.14b-stable");
  if (!supportedVersion || strcmp(event_base_get_method(evb_), "epoll")) {
    return false;
  }

  auto epfd = static_cast<epollop*>(evb_->evbase)->epfd;
  epoll_event epev = {0, {nullptr}};
  epev.data.fd = event.eb_ev_fd();
  epev.events = EPOLLET;
  if (event.eb_ev_events() & EV_READ) {
    epev.events |= EPOLLIN;
  }
  if (event.eb_ev_events() & EV_WRITE) {
    epev.events |= EPOLLOUT;
  }
  if (::epoll_ctl(epfd, EPOLL_CTL_MOD, event.eb_ev_fd(), &epev) == -1) {
    LOG(DFATAL) << "epoll_ctl failed: " << errno;
    return false;
  }
  return true;
#else
  (void)event;
  return false;
#endif
}

EventBaseBackend::~EventBaseBackend() {
  event_base_free(evb_);
}

class TestEventBaseBackend : public EventBaseBackend {
 public:
  explicit TestEventBaseBackend(int napiId) : napiId_(napiId) {}

  int getNapiId() const override { return napiId_; }

 private:
  int napiId_;
};

} // namespace

namespace folly {

class EventBase::LoopCallbacksDeadline {
 public:
  void reset(EventBase& evb) {
    if (auto timeslice = evb.loopCallbacksTimeslice_; timeslice.count() != 0) {
      deadline_ = Clock::now() + timeslice;
    } else {
      deadline_ = {};
    }
  }

  // Should be checked only after at least one callback has been processed, to
  // guarantee forward progress.
  bool expired() const {
    return deadline_ != Clock::time_point{} && Clock::now() >= deadline_;
  }

 private:
  // Use the fastest clock, here millisecond granularity is enough.
  using Clock = folly::chrono::coarse_steady_clock;
  Clock::time_point deadline_;
};

class EventBase::FuncRunner {
 public:
  explicit FuncRunner(EventBase& eventBase)
      : eventBase_(eventBase), curLoopCnt_(eventBase_.nextLoopCnt_) {}

  AtomicNotificationQueueTaskStatus operator()(Func&& func) noexcept {
    if (eventBase_.nextLoopCnt_ != curLoopCnt_) {
      // We're the first callaback of this iteration, set a new deadline.
      deadline_.reset(eventBase_);
      curLoopCnt_ = eventBase_.nextLoopCnt_;
    }

    ExecutionObserverScopeGuard guard(
        &eventBase_.getExecutionObserverList(),
        &func,
        folly::ExecutionObserver::CallbackType::NotificationQueue);
    std::exchange(func, {})();

    return deadline_.expired()
        ? AtomicNotificationQueueTaskStatus::CONSUMED_STOP
        : AtomicNotificationQueueTaskStatus::CONSUMED;
  }

 private:
  EventBase& eventBase_;
  LoopCallbacksDeadline deadline_;
  size_t curLoopCnt_;
};

class EventBase::ThreadIdCollector : public WorkerProvider {
 public:
  explicit ThreadIdCollector(EventBase& parent) : parent_(parent) {}

  IdsWithKeepAlive collectThreadIds() override {
    keepAlives_.fetch_add(1, std::memory_order_acq_rel);
    auto guard = std::make_unique<Guard>(*this);
    auto tid = parent_.loopTid_.load(std::memory_order_acquire);
    if (tid < 0) {
      return {};
    }
    return {std::move(guard), std::vector<pid_t>{tid}};
  }

  void awaitOutstandingKeepAlives() {
    wakeUp_.await([&] {
      return keepAlives_.load(std::memory_order_acquire) == 0;
    });
  }

 private:
  class Guard : public KeepAlive {
   public:
    Guard(ThreadIdCollector& parent) : parent_(parent) {}

    ~Guard() override {
      if (parent_.keepAlives_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        parent_.wakeUp_.notifyAll();
      }
    }

   private:
    ThreadIdCollector& parent_;
  };

  EventBase& parent_;
  std::atomic<size_t> keepAlives_ = 0;
  EventCount wakeUp_;
};

/*
 * EventBase methods
 */

EventBase::EventBase(std::chrono::milliseconds tickInterval)
    : EventBase(Options().setTimerTickInterval(tickInterval)) {}

EventBase::EventBase(bool enableTimeMeasurement)
    : EventBase(Options().setSkipTimeMeasurement(!enableTimeMeasurement)) {}

// takes ownership of the event_base
EventBase::EventBase(event_base* evb, bool enableTimeMeasurement)
    : EventBase(
          Options()
              .setBackendFactory([evb] {
                return std::make_unique<EventBaseBackend>(evb);
              })
              .setSkipTimeMeasurement(!enableTimeMeasurement)) {}

EventBase::EventBase(Options options)
    : intervalDuration_(options.timerTickInterval),
      enableTimeMeasurement_(!options.skipTimeMeasurement),
      loopCallbacksTimeslice_(options.loopCallbacksTimeslice),
      runOnceCallbacks_(nullptr),
      stop_(false),
      queue_(nullptr),
      maxLatency_(0),
      avgLoopTime_(std::chrono::seconds(2)),
      maxLatencyLoopTime_(avgLoopTime_),
      nextLoopCnt_(
          std::size_t(-40)) // Early wrap-around so bugs will manifest soon
      ,
      latestLoopCnt_(nextLoopCnt_),
      startWork_(),
      observer_(nullptr),
      observerSampleCount_(0),
      evb_(
          options.backendFactory
              ? options.backendFactory()
              : getDefaultBackend()),
      threadIdCollector_(std::make_unique<ThreadIdCollector>(*this)) {
  initNotificationQueue();
}

EventBase::~EventBase() {
  DCheckRequestContextRestoredGuard dcheckRctxGuard;

  // Relax strict mode to allow callbacks to run in the destructor outside of
  // the main loop. Note that any methods (including driving the loop) must be
  // called before the destructor starts, so it is safe to modify the variable.
  strictLoopThread_ = false;

  // Call all pre-destruction callbacks, before we start cleaning up our state
  // or apply any keepalives
  while (!preDestructionCallbacks_.rlock()->empty()) {
    OnDestructionCallbackList callbacks;
    preDestructionCallbacks_.swap(callbacks);
    while (!callbacks.empty()) {
      auto& callback = callbacks.front();
      callbacks.pop_front();
      RequestContextSaverScopeGuard rctxGuard;
      callback.runCallback();
    }
  }

  std::future<void> virtualEventBaseDestroyFuture;
  if (virtualEventBase_) {
    virtualEventBaseDestroyFuture = virtualEventBase_->destroy();
  }

  // Keep looping until all keep-alive handles are released. Each keep-alive
  // handle signals that some external code will still schedule some work on
  // this EventBase (so it's not safe to destroy it).
  while (loopKeepAliveCount() > 0) {
    applyLoopKeepAlive();
    loopOnce();
  }

  if (virtualEventBaseDestroyFuture.valid()) {
    virtualEventBaseDestroyFuture.get();
  }

  // Call all destruction callbacks, before we start cleaning up our state.
  while (!onDestructionCallbacks_.rlock()->empty()) {
    OnDestructionCallbackList callbacks;
    onDestructionCallbacks_.swap(callbacks);
    while (!callbacks.empty()) {
      auto& callback = callbacks.front();
      callbacks.pop_front();
      RequestContextSaverScopeGuard rctxGuard;
      callback.runCallback();
    }
  }

  clearCobTimeouts();

  DCHECK_EQ(0u, runBeforeLoopCallbacks_.size());
  DCHECK_EQ(0u, runAfterLoopCallbacks_.size());

  runLoopCallbacks();

  queue_->drain();

  // Stop consumer before deleting NotificationQueue
  queue_->stopConsuming();

  // Remove self from all registered EventBaseLocal instances.
  // Notice that we could be racing with EventBaseLocal dtor similarly
  // deregistering itself from all registered EventBase instances. Because
  // both sides need to acquire two locks, but in inverse order, we retry if
  // inner lock acquisition fails to prevent lock inversion deadlock.
  while (true) {
    auto locked = localStorageToDtor_.wlock();
    if (locked->empty()) {
      break;
    }
    auto evbl = *locked->begin();
    if (evbl->tryDeregister(*this)) {
      locked->erase(evbl);
    }
  }

  executionObserverList_.clear();

  localStorage_.clear();

  evb_.reset();

  VLOG(5) << "EventBase(): Destroyed.";
}

void EventBase::setStrictLoopThread() {
  CHECK(!isRunning());
  strictLoopThread_ = true;
}

bool EventBase::tryDeregister(detail::EventBaseLocalBase& evbl) {
  if (auto locked = localStorageToDtor_.tryWLock()) {
    locked->erase(&evbl);
    runInEventBaseThread([this, key = evbl.key_] { localStorage_.erase(key); });
    return true;
  }
  return false;
}

std::unique_ptr<EventBaseBackendBase> EventBase::getDefaultBackend() {
  return std::make_unique<EventBaseBackend>();
}

std::unique_ptr<EventBaseBackendBase> EventBase::getTestBackend(int napiId) {
  return std::make_unique<TestEventBaseBackend>(napiId);
}

size_t EventBase::getNotificationQueueSize() const {
  return queue_->size();
}

size_t EventBase::getNumLoopCallbacks() const {
  dcheckIsInEventBaseThread();
  return loopCallbacks_.size();
}

uint32_t EventBase::getMaxReadAtOnce() const {
  return queue_->getMaxReadAtOnce();
}

void EventBase::setMaxReadAtOnce(uint32_t maxAtOnce) {
  queue_->setMaxReadAtOnce(maxAtOnce);
}

bool EventBase::isInEventBaseThread() const {
  auto tid = loopTid_.load(std::memory_order_relaxed);
  return tid == static_cast<pid_t>(getOSThreadID()) ||
      (!strictLoopThread_ && tid == kNotRunningTid);
}

bool EventBase::inRunningEventBaseThread() const {
  return loopTid_.load(std::memory_order_relaxed) ==
      static_cast<pid_t>(getOSThreadID());
}

void EventBase::checkIsInEventBaseThread() const {
  auto evbTid = loopTid_.load(std::memory_order_relaxed);
  if (!strictLoopThread_ && evbTid == kNotRunningTid) {
    return;
  }

  // As opposed to name_, using getThreadName(loopThread_) will also work if the
  // thread name is set outside of EventBase (and name_ is empty).
  auto curTid = getOSThreadID();
  CHECK_EQ(evbTid, curTid)
      << "This logic must be executed in the event base thread. "
      << "Event base thread name: \""
      << folly::getThreadName(loopThread_.load(std::memory_order_acquire))
             .value_or("")
      << "\", current thread name: \""
      << folly::getCurrentThreadName().value_or("") << "\"";
}

// Set smoothing coefficient for loop load average; input is # of milliseconds
// for exp(-1) decay.
void EventBase::setLoadAvgMsec(std::chrono::milliseconds ms) {
  assert(enableTimeMeasurement_);
  std::chrono::microseconds us = std::chrono::milliseconds(ms);
  if (ms > std::chrono::milliseconds::zero()) {
    maxLatencyLoopTime_.setTimeInterval(us);
    avgLoopTime_.setTimeInterval(us);
  } else {
    LOG(ERROR) << "non-positive arg to setLoadAvgMsec()";
  }
}

void EventBase::resetLoadAvg(double value) {
  assert(enableTimeMeasurement_);
  avgLoopTime_.reset(value);
  maxLatencyLoopTime_.reset(value);
}

static std::chrono::milliseconds getTimeDelta(
    std::chrono::steady_clock::time_point* prev) {
  auto now = std::chrono::steady_clock::now();
  auto result = now - *prev;
  *prev = now;

  return std::chrono::duration_cast<std::chrono::milliseconds>(result);
}

void EventBase::waitUntilRunning() {
  while (loopTid_.load(std::memory_order_acquire) == kNotRunningTid) {
    std::this_thread::yield();
  }
}

// enters the event_base loop -- will only exit when forced to
bool EventBase::loop() {
  // Enforce blocking tracking and if we have a name override any previous one
  ExecutorBlockingGuard guard{ExecutorBlockingGuard::TrackTag{}, this, name_};
  return loopBody(0, {});
}

bool EventBase::loopIgnoreKeepAlive() {
  if (loopKeepAliveActive_) {
    // Make sure NotificationQueue is not counted as one of the readers
    // (otherwise loopBody won't return until terminateLoopSoon is called).
    queue_->stopConsuming();
    queue_->startConsumingInternal(this);
    loopKeepAliveActive_ = false;
  }
  LoopOptions options;
  options.ignoreKeepAlive = true;
  return loopBody(0, options);
}

bool EventBase::loopOnce(int flags) {
  return loopBody(flags | EVLOOP_ONCE, {});
}

bool EventBase::isSuccess(LoopStatus status) {
  switch (status) {
    case LoopStatus::kDone:
      return true;
    case LoopStatus::kError:
      return false;
    case LoopStatus::kSuspended:
      LOG(DFATAL) << "Reached suspension when not allowed";
      return false;
  }
  assume_unreachable();
}

bool EventBase::loopBody(int flags, LoopOptions options) {
  loopMainSetup();
  SCOPE_EXIT {
    DCHECK(!loopState_); // Cannot be suspended.
    loopMainCleanup();
  };
  return isSuccess(loopMain(flags, options));
}

void EventBase::loopPollSetup() {
  loopMainSetup();
}

bool EventBase::loopPoll(LoopPollOptions options) {
  DCHECK(isRunning());
  dcheckIsInEventBaseThread();
  return isSuccess(
      loopMain((options.nonblock ? EVLOOP_NONBLOCK : 0) | EVLOOP_ONCE, {}));
}

void EventBase::loopPollCleanup() {
  loopMainCleanup();
}

EventBase::LoopStatus EventBase::loopWithSuspension() {
  DCHECK_NE(evb_->getPollableFd(), -1)
      << "loopWithSuspension() is only supported for backends with pollable fd";
  loopMainSetup();
  SCOPE_EXIT {
    loopMainCleanup();
  };
  LoopOptions options;
  options.allowSuspension = true;
  return loopMain(EVLOOP_NONBLOCK, options);
}

void EventBase::loopMainSetup() {
  VLOG(5) << "EventBase(): Starting loop.";

  auto tid = getOSThreadID();
  // Lock the loop.
  auto const prevLoopTid = loopTid_.exchange(tid, std::memory_order_release);
  loopThread_.store(std::this_thread::get_id(), std::memory_order_release);

  // NOTE: This also fatals on reentrancy, which is not supported by old
  // versions of libevent.
  pid_t expected = loopState_ ? kSuspendedTid : kNotRunningTid;
  CHECK_EQ(expected, prevLoopTid)
      << "Driving an EventBase (in thread " << tid
      << ") while it is already being driven (in thread " << prevLoopTid
      << ") is forbidden.";

  if (!name_.empty()) {
    setThreadName(name_);
  }
}

EventBase::LoopStatus EventBase::loopMain(int flags, LoopOptions options) {
  DCheckRequestContextRestoredGuard dcheckRctxGuard;

  int res = 0;
  bool blocking = !(flags & EVLOOP_NONBLOCK);
  bool once = (flags & EVLOOP_ONCE);

  bool resumed = false;
  if (!loopState_) {
    loopState_.emplace(LoopState{});
    if (enableTimeMeasurement_) {
      loopState_->prev = std::chrono::steady_clock::now();
      loopState_->idleStart = loopState_->prev;
    }
  } else {
    resumed = true;
  }

  SCOPE_EXIT {
    // Consume the stop signal so that the loop can resume on the next call.
    stop_.store(false, std::memory_order_relaxed);
  };

  while (!stop_.load(std::memory_order_relaxed)) {
    // Skip the setup if we're resuming.
    if (!std::exchange(resumed, false)) {
      if (!options.ignoreKeepAlive) {
        applyLoopKeepAlive();
      }
      ++nextLoopCnt_;

      // Before-loop callbacks must by definition all run regardless of
      // timeslice, so do not enforce a deadline.
      LoopCallbackList currentCallbacks;
      swapAndRunLoopCallbackList(
          runBeforeLoopCallbacks_,
          currentCallbacks,
          /* enforceDeadline */ false);
    }

    // nobody can add loop callbacks from within this thread if
    // we don't have to handle anything to start with...
    if (blocking && loopCallbacks_.empty()) {
      res = evb_->eb_event_base_loop(EVLOOP_ONCE);
    } else {
      res = evb_->eb_event_base_loop(EVLOOP_ONCE | EVLOOP_NONBLOCK);
    }
    if (res == 2) {
      // Only backends with pollable fd support return value 2.
      DCHECK_NE(evb_->getPollableFd(), -1);
      if (options.allowSuspension && loopCallbacks_.empty()) {
        return LoopStatus::kSuspended;
      } else {
        res = 0; // Return value 2 implies success.
      }
    }

    // libevent may return 1 early if there are no registered non-internal
    // events, so even if the queue is not empty it may not be processed, thus
    // we check that explicitly.
    //
    // Note that the queue was either not consumed, or it will be re-armed by a
    // loop callback scheduled by execute(), so if there is an enqueue after the
    // empty check here the queue's event will eventually be active.
    if (res != 0 && !queue_->empty()) {
      queue_->execute();
    }

    bool ranLoopCallbacks = runLoopCallbacks();

    {
      // Like the before-loop callbacks, after-loop do not enforce deadlines.
      LoopCallbackList currentCallbacks;
      swapAndRunLoopCallbackList(
          runAfterLoopCallbacks_,
          currentCallbacks,
          /* enforceDeadline */ false);
    }

    if (enableTimeMeasurement_) {
      auto now = std::chrono::steady_clock::now();
      auto busy = std::chrono::duration_cast<std::chrono::microseconds>(
          now - startWork_);
      auto idle = std::chrono::duration_cast<std::chrono::microseconds>(
          startWork_ - loopState_->idleStart);
      auto loop_time = busy + idle;

      avgLoopTime_.addSample(loop_time, busy);
      maxLatencyLoopTime_.addSample(loop_time, busy);

      if (observer_) {
        RequestContextSaverScopeGuard rctxGuard;
        if (++observerSampleCount_ >= observer_->getSampleRate()) {
          observerSampleCount_ = 0;
          observer_->loopSample(busy.count(), idle.count());
        }
      }

      VLOG(11)
          << "EventBase " << this << " did not timeout "
          << " loop time guess: " << loop_time.count()
          << " idle time: " << idle.count() << " busy time: " << busy.count()
          << " avgLoopTime: " << avgLoopTime_.get()
          << " maxLatencyLoopTime: " << maxLatencyLoopTime_.get()
          << " maxLatency_: " << maxLatency_.count() << "us"
          << " notificationQueueSize: " << getNotificationQueueSize()
          << " nothingHandledYet(): " << nothingHandledYet();

      if (maxLatency_ > std::chrono::microseconds::zero()) {
        // see if our average loop time has exceeded our limit
        if (dampenMaxLatency_ &&
            (maxLatencyLoopTime_.get() > double(maxLatency_.count()))) {
          maxLatencyCob_();
          // back off temporarily -- don't keep spamming maxLatencyCob_
          // if we're only a bit over the limit
          maxLatencyLoopTime_.dampen(0.9);
        } else if (!dampenMaxLatency_ && busy > maxLatency_) {
          // If no damping, we compare the raw busy time
          maxLatencyCob_();
        }
      }

      // Our loop run did real work; reset the idle timer
      loopState_->idleStart = now;
    } else {
      VLOG(11) << "EventBase " << this << " did not timeout";
    }

    if (enableTimeMeasurement_) {
      VLOG(11) << "EventBase " << this
               << " loop time: " << getTimeDelta(&loopState_->prev).count();
    }

    if (once ||
        // Event loop indicated that there were are no more registered events
        // (except queue_ which is an internal event) and we didn't have any
        // loop callbacks to run, so there is nothing left to do.
        (res != 0 && !ranLoopCallbacks)) {
      break;
    }
  }

  loopState_.reset();
  if (res < 0) {
    LOG(ERROR) << "EventBase: -- error in event loop, res = " << res;
    return LoopStatus::kError;
  } else if (res == 1) {
    VLOG(5) << "EventBase: ran out of events (exiting loop)!";
  } else if (res > 1) {
    LOG(ERROR) << "EventBase: unknown event loop result = " << res;
    return LoopStatus::kError;
  }
  VLOG(5) << "EventBase(): Done with loop.";
  return LoopStatus::kDone;
}

void EventBase::loopMainCleanup() {
  threadIdCollector_->awaitOutstandingKeepAlives();
  loopThread_.store({}, std::memory_order_release);
  // Must be last, unlocks the loop.
  loopTid_.store(
      loopState_ ? kSuspendedTid : kNotRunningTid, std::memory_order_release);
}

namespace {
std::atomic<EventBase::KeepAliveTraceCallback> gKeepAliveAcquireTraceCallback{
    nullptr};
std::atomic<EventBase::KeepAliveTraceCallback> gKeepAliveReleaseTraceCallback{
    nullptr};
} // namespace

void EventBase::setKeepAliveAcquireTraceCallback(KeepAliveTraceCallback cb) {
  gKeepAliveAcquireTraceCallback.store(cb, std::memory_order_relaxed);
}

void EventBase::setKeepAliveReleaseTraceCallback(KeepAliveTraceCallback cb) {
  gKeepAliveReleaseTraceCallback.store(cb, std::memory_order_relaxed);
}

bool EventBase::keepAliveAcquire() noexcept {
  loopKeepAliveCount_.fetch_add(1, std::memory_order_relaxed);
  auto cb = gKeepAliveAcquireTraceCallback.load(std::memory_order_relaxed);
  if (cb != nullptr) [[unlikely]] {
    cb(this);
  }
  return true;
}

void EventBase::keepAliveRelease() noexcept {
  auto cb = gKeepAliveReleaseTraceCallback.load(std::memory_order_relaxed);
  if (cb != nullptr) [[unlikely]] {
    cb(this);
  }
  size_t count = loopKeepAliveCount_.load(std::memory_order_relaxed);
  do {
    DCHECK_GE(count, 1);
    // Ensure that the transition to 0 only happens in the loop, so that the
    // loop can observe it and complete.
    if (count == 1 && !inRunningEventBaseThread()) {
      queue_->putMessage([this] {
        auto oldCount =
            loopKeepAliveCount_.fetch_sub(1, std::memory_order_acq_rel);
        DCHECK_GE(oldCount, 1);
      });
      return;
    }
  } while (!loopKeepAliveCount_.compare_exchange_weak(
      count, count - 1, std::memory_order_acq_rel, std::memory_order_relaxed));
}

size_t EventBase::loopKeepAliveCount() const {
  return loopKeepAliveCount_.load(std::memory_order_acquire);
}

void EventBase::applyLoopKeepAlive() {
  auto keepAliveCount = loopKeepAliveCount();
  // Make sure default VirtualEventBase won't hold EventBase::loop() forever.
  if (auto virtualEventBase = tryGetVirtualEventBase()) {
    if (virtualEventBase->keepAliveCount() == 1) {
      --keepAliveCount;
    }
  }

  if (loopKeepAliveActive_ && keepAliveCount == 0) {
    // Restore the notification queue internal flag
    queue_->stopConsuming();
    queue_->startConsumingInternal(this);
    loopKeepAliveActive_ = false;
  } else if (!loopKeepAliveActive_ && keepAliveCount > 0) {
    // Update the notification queue event to treat it as a normal
    // (non-internal) event.  The notification queue event always remains
    // installed, and the main loop won't exit with it installed.
    queue_->stopConsuming();
    queue_->startConsuming(this);
    loopKeepAliveActive_ = true;
  }
}

void EventBase::loopForever() {
  bool ret;
  {
    // Make sure notification queue events are treated as normal events.
    loopKeepAliveCount_.fetch_add(1, std::memory_order_relaxed);
    SCOPE_EXIT {
      loopKeepAliveCount_.fetch_sub(1, std::memory_order_relaxed);
      applyLoopKeepAlive();
    };
    ret = loop();
  }

  if (!ret) {
    folly::throwSystemError("error in EventBase::loopForever()");
  }
}

void EventBase::bumpHandlingTimeSlow() {
  VLOG(11) << "EventBase " << this << " " << __PRETTY_FUNCTION__
           << " (loop) latest " << latestLoopCnt_ << " next " << nextLoopCnt_;
  latestLoopCnt_ = nextLoopCnt_;
  // set the time
  startWork_ = std::chrono::steady_clock::now();

  VLOG(11) << "EventBase " << this << " " << __PRETTY_FUNCTION__
           << " (loop) startWork_ " << startWork_.time_since_epoch().count();
}

void EventBase::yieldAfterInternalEvent() {
  // Reached from an event callback, so the loop thread. The loopCallbacks_
  // read and the backend call below both count on that.
  dcheckIsInEventBaseThread();
  // No reason to return to the loop if the callback left no work behind.
  if (!loopCallbacks_.empty()) {
    evb_->eb_yield_after_internal_event();
  }
}

void EventBase::terminateLoopSoon() {
  CHECK(!strictLoopThread_)
      << "terminateLoopSoon() not allowed in strict loop thread mode";

  VLOG(5) << "EventBase(): Received terminateLoopSoon() command.";

  auto keepAlive = getKeepAliveToken(this);

  // Set stop to true, so the event loop will know to exit.
  stop_.store(true, std::memory_order_relaxed);

  // If terminateLoopSoon() is called from another thread,
  // the EventBase thread might be stuck waiting for events.
  // In this case, it won't wake up and notice that stop_ is set until it
  // receives another event.  Send an empty frame to the notification queue
  // so that the event loop will wake up even if there are no other events.
  queue_->putMessage([] {});
}

void EventBase::runInLoop(
    LoopCallback* callback,
    bool thisIteration,
    std::shared_ptr<RequestContext> rctx) {
  dcheckIsInEventBaseThread();
  callback->cancelLoopCallback();
  callback->context_ = std::move(rctx);
  if (runOnceCallbacks_ != nullptr && thisIteration) {
    runOnceCallbacks_->push_back(*callback);
  } else {
    loopCallbacks_.push_back(*callback);
  }
}

void EventBase::runInLoop(Func cob, bool thisIteration) {
  dcheckIsInEventBaseThread();
  auto wrapper = new FunctionLoopCallback(std::move(cob));
  wrapper->context_ = RequestContext::saveContext();
  if (runOnceCallbacks_ != nullptr && thisIteration) {
    runOnceCallbacks_->push_back(*wrapper);
  } else {
    loopCallbacks_.push_back(*wrapper);
  }
}

void EventBase::runOnDestruction(OnDestructionCallback& callback) {
  callback.schedule(
      [this](auto& cb) { onDestructionCallbacks_.wlock()->push_back(cb); },
      [this](auto& cb) {
        onDestructionCallbacks_.withWLock([&](auto& list) {
          list.erase(list.iterator_to(cb));
        });
      });
}

void EventBase::runOnDestruction(Func f) {
  auto* callback = new FunctionOnDestructionCallback(std::move(f));
  runOnDestruction(*callback);
}

void EventBase::runOnDestructionStart(OnDestructionCallback& callback) {
  callback.schedule(
      [this](auto& cb) { preDestructionCallbacks_.wlock()->push_back(cb); },
      [this](auto& cb) {
        preDestructionCallbacks_.withWLock([&](auto& list) {
          list.erase(list.iterator_to(cb));
        });
      });
}

void EventBase::runOnDestructionStart(Func f) {
  auto* callback = new FunctionOnDestructionCallback(std::move(f));
  runOnDestructionStart(*callback);
}

void EventBase::runBeforeLoop(LoopCallback* callback) {
  dcheckIsInEventBaseThread();
  callback->cancelLoopCallback();
  runBeforeLoopCallbacks_.push_back(*callback);
}

void EventBase::runAfterLoop(LoopCallback* callback) {
  dcheckIsInEventBaseThread();
  callback->cancelLoopCallback();
  runAfterLoopCallbacks_.push_back(*callback);
}

void EventBase::runInEventBaseThread(Func fn) noexcept {
  // Send the message.
  // It will be received by the FunctionRunner in the EventBase's thread.

  // We try not to schedule nullptr callbacks
  if (!fn) {
    LOG(DFATAL) << "EventBase " << this
                << ": Scheduling nullptr callbacks is not allowed";
    return;
  }

  // Short-circuit if we are already in our event base
  if (inRunningEventBaseThread()) {
    runInLoop(std::move(fn));
    return;
  }

  queue_->putMessage(std::move(fn));
}

void EventBase::runInEventBaseThreadAlwaysEnqueue(Func fn) noexcept {
  // Send the message.
  // It will be received by the FunctionRunner in the EventBase's thread.

  // We try not to schedule nullptr callbacks
  if (!fn) {
    LOG(DFATAL) << "EventBase " << this
                << ": Scheduling nullptr callbacks is not allowed";
    return;
  }

  queue_->putMessage(std::move(fn));
}

void EventBase::runInEventBaseThreadAlwaysEnqueue(
    std::shared_ptr<RequestContext>&& ctx, Func fn) noexcept {
  // Send the message.
  // It will be received by the FunctionRunner in the EventBase's thread.

  // We try not to schedule nullptr callbacks
  if (!fn) {
    LOG(DFATAL) << "EventBase " << this
                << ": Scheduling nullptr callbacks is not allowed";
    return;
  }

  queue_->putMessage(std::move(ctx), std::move(fn));
}

void EventBase::runInEventBaseThreadAndWait(Func fn) noexcept {
  if (inRunningEventBaseThread()) {
    LOG(DFATAL) << "EventBase " << this << ": Waiting in the event loop is not "
                << "allowed";
    return;
  }

  Baton<> ready;
  runInEventBaseThread([&ready, fn = std::move(fn)]() mutable {
    SCOPE_EXIT {
      ready.post();
    };
    // A trick to force the stored functor to be executed and then destructed
    // before posting the baton and waking the waiting thread.
    copy(std::move(fn))();
  });
  ready.wait(folly::Baton<>::wait_options().logging_enabled(false));
}

void EventBase::runImmediatelyOrRunInEventBaseThreadAndWait(Func fn) noexcept {
  if (isInEventBaseThread()) {
    RequestContextSaverScopeGuard rctxGuard;
    fn();
  } else {
    runInEventBaseThreadAndWait(std::move(fn));
  }
}

void EventBase::runImmediatelyOrRunInEventBaseThread(Func fn) noexcept {
  if (isInEventBaseThread()) {
    RequestContextSaverScopeGuard rctxGuard;
    fn();
  } else {
    runInEventBaseThreadAlwaysEnqueue(std::move(fn));
  }
}

// Swap callbacks with currentCallbacks and execute currentCallbacks, so that if
// any of these callbacks in turn schedule more callbacks, those new callbacks
// won't be run until the next iteration around the event loop.  This prevents
// runInLoop() callbacks from being able to start file descriptor and timeout
// based events.
bool EventBase::swapAndRunLoopCallbackList(
    LoopCallbackList& callbacks,
    LoopCallbackList& currentCallbacks,
    bool enforceDeadline) {
  DCHECK(currentCallbacks.empty());
  if (callbacks.empty()) {
    return false;
  }
  currentCallbacks.swap(callbacks);

  LoopCallbacksDeadline deadline;
  if (enforceDeadline) {
    deadline.reset(*this);
  }

  RequestContextSaverScopeGuard ctxGuard;
  do {
    LoopCallback* callback = &currentCallbacks.front();
    currentCallbacks.pop_front();
    // Use RequestContextSaverScopeGuard instead of a per-callback
    // RequestContextScopeGuard to avoid switching context back and forth when
    // consecutive callbacks have the same context. This runs the pop_front() in
    // the previous callback's context, but that is non-blocking and doesn't run
    // application logic.
    ctxGuard.setContext(std::move(callback->context_));
    ExecutionObserverScopeGuard guard(
        &executionObserverList_,
        callback,
        folly::ExecutionObserver::CallbackType::Loop);
    callback->runLoopCallback();
  } while (!currentCallbacks.empty() && !deadline.expired());

  return true;
}

bool EventBase::runLoopCallbacks() {
  bumpHandlingTime();
  LoopCallbackList currentCallbacks;
  // Store a reference to the temporary callback list to runOnceCallbacks_, so
  // that runInLoop(..., thisIteration = true) can append directly to it.
  runOnceCallbacks_ = &currentCallbacks;
  SCOPE_EXIT {
    runOnceCallbacks_ = nullptr;
  };
  if (swapAndRunLoopCallbackList(loopCallbacks_, currentCallbacks)) {
    // If the deadline expired before the list was fully consumed, prepend the
    // leftover callbacks to the list to run on the next iteration.
    loopCallbacks_.splice(loopCallbacks_.begin(), currentCallbacks);
    return true;
  }
  return false;
}

void EventBase::initNotificationQueue() {
  // Infinite size queue
  queue_ = std::make_unique<EventBaseAtomicNotificationQueue<Func, FuncRunner>>(
      FuncRunner{*this});

  // Mark this as an internal event, so event_base_loop() will return if
  // there are no other events besides this one installed.
  //
  // Most callers don't care about the internal notification queue used by
  // EventBase.  The queue is always installed, so if we did count the queue as
  // an active event, loop() would never exit with no more events to process.
  // Users can use loopForever() if they do care about the notification queue.
  // (This is useful for EventBase threads that do nothing but process
  // runInEventBaseThread() notifications.)
  queue_->startConsumingInternal(this);
}

void EventBase::SmoothLoopTime::setTimeInterval(
    std::chrono::microseconds timeInterval) {
  expCoeff_ = -1.0 / static_cast<double>(timeInterval.count());
  VLOG(11) << "expCoeff_ " << expCoeff_ << " " << __PRETTY_FUNCTION__;
}

void EventBase::SmoothLoopTime::reset(double value) {
  value_ = value;
}

void EventBase::SmoothLoopTime::addSample(
    std::chrono::microseconds total, std::chrono::microseconds busy) {
  if ((buffer_time_ + total) > buffer_interval_ && buffer_cnt_ > 0) {
    // See https://en.wikipedia.org/wiki/Exponential_smoothing for
    // more info on this calculation.
    double coeff = exp(static_cast<double>(buffer_time_.count()) * expCoeff_);
    value_ = value_ * coeff +
        (1.0 - coeff) *
            (static_cast<double>(busy_buffer_.count()) / buffer_cnt_);
    buffer_time_ = std::chrono::microseconds{0};
    busy_buffer_ = std::chrono::microseconds{0};
    buffer_cnt_ = 0;
  }
  buffer_time_ += total;
  busy_buffer_ += busy;
  buffer_cnt_++;
}

bool EventBase::nothingHandledYet() const noexcept {
  return (nextLoopCnt_ != latestLoopCnt_);
}

void EventBase::attachTimeoutManager(AsyncTimeout* obj, InternalEnum internal) {
  auto* ev = obj->getEvent();
  assert(ev->eb_ev_base() == nullptr);

  ev->eb_event_base_set(this);
  if (internal == AsyncTimeout::InternalEnum::INTERNAL) {
    ev->eb_set_internal(true);
  }
}

void EventBase::detachTimeoutManager(AsyncTimeout* obj) {
  cancelTimeout(obj);
  auto* ev = obj->getEvent();
  ev->eb_set_ev_base(nullptr);
}

bool EventBase::scheduleTimeout(
    AsyncTimeout* obj, TimeoutManager::timeout_type timeout) {
  dcheckIsInEventBaseThread();
  // Set up the timeval and add the event
  struct timeval tv;
  tv.tv_sec = to_narrow(timeout.count() / 1000LL);
  tv.tv_usec = to_narrow((timeout.count() % 1000LL) * 1000LL);

  auto* ev = obj->getEvent();

  DCHECK(ev->eb_ev_base());

  if (ev->eb_event_add(&tv) < 0) {
    LOG(ERROR) << "EventBase: failed to schedule timeout: " << errnoStr(errno);
    return false;
  }

  return true;
}

void EventBase::cancelTimeout(AsyncTimeout* obj) {
  dcheckIsInEventBaseThread();
  auto* ev = obj->getEvent();
  if (ev->isEventRegistered()) {
    ev->eb_event_del();
  }
}

void EventBase::setName(const std::string& name) {
  dcheckIsInEventBaseThread();
  name_ = name;

  if (isRunning()) {
    setThreadName(loopThread_.load(std::memory_order_relaxed), name_);
  }
}

const std::string& EventBase::getName() {
  dcheckIsInEventBaseThread();
  return name_;
}

std::thread::id EventBase::getLoopThreadId() {
  return loopThread_.load(std::memory_order_relaxed);
}

void EventBase::scheduleAt(Func&& fn, TimePoint const& timeout) {
  auto duration = timeout - now();
  timer().scheduleTimeoutFn(
      std::move(fn),
      std::chrono::duration_cast<std::chrono::milliseconds>(duration));
}

event_base* EventBase::getLibeventBase() const {
  return evb_ ? (evb_->getEventBase()) : nullptr;
}

const char* EventBase::getLibeventVersion() {
  return event_get_version();
}
const char* EventBase::getLibeventMethod() {
  // event_base_method() would segv if there is no current_base so simulate it
  struct op {
    const char* name;
  };
  struct base {
    const op* evsel;
  };
  auto b = reinterpret_cast<base*>(evb_ ? evb_->getEventBase() : nullptr);
  return !b ? "" : b->evsel->name;
}

VirtualEventBase& EventBase::getVirtualEventBase() {
  folly::call_once(virtualEventBaseInitFlag_, [&] {
    virtualEventBase_ = std::make_unique<VirtualEventBase>(*this);
  });

  return *virtualEventBase_;
}

VirtualEventBase* EventBase::tryGetVirtualEventBase() {
  if (folly::test_once(virtualEventBaseInitFlag_)) {
    return virtualEventBase_.get();
  }
  return nullptr;
}

EventBase* EventBase::getEventBase() {
  return this;
}

Executor::KeepAlive<EventBase> EventBase::getEventBaseKeepAlive() {
  return getKeepAliveToken(this);
}

WorkerProvider* EventBase::getThreadIdCollector() {
  return threadIdCollector_.get();
}

EventBase::OnDestructionCallback::~OnDestructionCallback() {
  if (*scheduled_.rlock()) {
    LOG(FATAL)
        << "OnDestructionCallback must be canceled if needed prior to destruction";
  }
}

void EventBase::OnDestructionCallback::runCallback() noexcept {
  scheduled_.withWLock([&](bool& scheduled) {
    CHECK(scheduled);
    scheduled = false;

    // run can only be called by EventBase and VirtualEventBase, and it's called
    // after the callback has been popped off the list.
    eraser_ = nullptr;

    // Note that the exclusive lock on shared state is held while the callback
    // runs. This ensures concurrent callers to cancel() block until the
    // callback finishes.
    onEventBaseDestruction();
  });
}

void EventBase::OnDestructionCallback::schedule(
    Function<void(OnDestructionCallback&)> linker,
    Function<void(OnDestructionCallback&)> eraser) {
  eraser_ = std::move(eraser);
  scheduled_.withWLock([](bool& scheduled) { scheduled = true; });
  linker(*this);
}

bool EventBase::OnDestructionCallback::cancel() {
  return scheduled_.withWLock([this](bool& scheduled) {
    const bool wasScheduled = std::exchange(scheduled, false);
    if (wasScheduled) {
      auto eraser = std::move(eraser_);
      CHECK(eraser);
      eraser(*this);
    }
    return wasScheduled;
  });
}

} // namespace folly
