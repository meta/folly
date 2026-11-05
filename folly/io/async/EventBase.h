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

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <stack>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <boost/intrusive/list.hpp>
#include <glog/logging.h>

#include <folly/Executor.h>
#include <folly/Function.h>
#include <folly/Memory.h>
#include <folly/Portability.h>
#include <folly/ScopeGuard.h>
#include <folly/Synchronized.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <folly/executors/DrivableExecutor.h>
#include <folly/executors/ExecutionObserver.h>
#include <folly/executors/IOExecutor.h>
#include <folly/executors/QueueObserver.h>
#include <folly/executors/ScheduledExecutor.h>
#include <folly/executors/SequencedExecutor.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/HHWheelTimer.h>
#include <folly/io/async/Request.h>
#include <folly/io/async/TimeoutManager.h>
#include <folly/portability/Event.h>
#include <folly/synchronization/CallOnce.h>

namespace folly {
class EventBaseBackendBase;

using Cob = Func; // defined in folly/Executor.h

template <typename Task, typename Consumer>
class EventBaseAtomicNotificationQueue;
template <typename MessageT>
class NotificationQueue;

namespace detail {
class EventBaseLocalBase;

} // namespace detail
template <typename T>
class EventBaseLocal;

class EventBaseObserver {
 public:
  virtual ~EventBaseObserver() = default;

  virtual uint32_t getSampleRate() const = 0;

  virtual void loopSample(int64_t busyTime, int64_t idleTime) = 0;
};

// Helper class that sets and retrieves the EventBase associated with a given
// request via RequestContext. See Request.h for that mechanism.
class RequestEventBase : public RequestData {
 public:
  static EventBase* get() {
    auto data = dynamic_cast<RequestEventBase*>(
        RequestContext::get()->getContextData(token()));
    if (!data) {
      return nullptr;
    }
    return data->eb_;
  }

  static void set(EventBase* eb) {
    RequestContext::get()->setContextData(
        token(), std::unique_ptr<RequestEventBase>(new RequestEventBase(eb)));
  }

  bool hasCallback() override { return false; }

 private:
  FOLLY_EXPORT static RequestToken const& token() {
    static RequestToken const token(kContextDataName);
    return token;
  }

  explicit RequestEventBase(EventBase* eb) : eb_(eb) {}
  EventBase* eb_;
  static constexpr const char* kContextDataName{"EventBase"};
};

class VirtualEventBase;

/**
 * This class is a wrapper for all asynchronous I/O processing functionality
 *
 * EventBase provides a main loop that notifies EventHandler callback objects
 * when I/O is ready on a file descriptor, and notifies AsyncTimeout objects
 * when a specified timeout has expired.  More complex, higher-level callback
 * mechanisms can then be built on top of EventHandler and AsyncTimeout.
 *
 * A EventBase object can only drive an event loop for a single thread.  To
 * take advantage of multiple CPU cores, most asynchronous I/O servers have one
 * thread per CPU, and use a separate EventBase for each thread.
 *
 * In general, most EventBase methods may only be called from the thread
 * running the EventBase's loop.  There are a few exceptions to this rule, for
 * methods that are explicitly intended to allow communication with a
 * EventBase from other threads.  When it is safe to call a method from
 * another thread it is explicitly listed in the method comments.
 */
class EventBase
    : public TimeoutManager,
      public DrivableExecutor,
      public IOExecutor,
      public SequencedExecutor,
      public ScheduledExecutor,
      public GetThreadIdCollector {
 public:
  friend class ScopedEventBaseThread;

  using Func = folly::Function<void()>;

  /**
   * A callback interface to use with runInLoop()
   *
   * Derive from this class if you need to delay some code execution until the
   * next iteration of the event loop.  This allows you to schedule code to be
   * invoked from the top-level of the loop, after your immediate callers have
   * returned.
   *
   * If a LoopCallback object is destroyed while it is scheduled to be run in
   * the next loop iteration, it will automatically be cancelled.
   */
  class LoopCallback
      : public boost::intrusive::list_base_hook<
            boost::intrusive::link_mode<boost::intrusive::auto_unlink>> {
   public:
    virtual ~LoopCallback() = default;

    virtual void runLoopCallback() noexcept = 0;
    void cancelLoopCallback() {
      context_.reset();
      unlink();
    }

    bool isLoopCallbackScheduled() const { return is_linked(); }

   private:
    using List = boost::intrusive::
        list<LoopCallback, boost::intrusive::constant_time_size<false>>;

    // EventBase needs access to LoopCallbackList (and therefore to hook_)
    friend class EventBase;
    friend class VirtualEventBase;
    std::shared_ptr<RequestContext> context_;
  };

  // List of loop callbacks for code that batches them, such as flush lists.
  // Folly owns the underlying type; name it only through this alias.
  using LoopCallbackList = LoopCallback::List;

  class FunctionLoopCallback : public LoopCallback {
   public:
    explicit FunctionLoopCallback(Func&& function)
        : function_(std::move(function)) {}

    void runLoopCallback() noexcept override {
      function_();
      delete this;
    }

   private:
    Func function_;
  };

  // Like FunctionLoopCallback, but saves one allocation. Use with caution.
  //
  // The caller is responsible for maintaining the lifetime of this callback
  // until after the point at which the contained function is called.
  class StackFunctionLoopCallback : public LoopCallback {
   public:
    explicit StackFunctionLoopCallback(Func&& function)
        : function_(std::move(function)) {}
    void runLoopCallback() noexcept override { Func(std::move(function_))(); }

   private:
    Func function_;
  };

  // Base class for user callbacks to be run during EventBase destruction. As
  // with LoopCallback, users may inherit from this class and provide a concrete
  // implementation of onEventBaseDestruction(). (Alternatively, users may use
  // the convenience method EventBase::runOnDestruction(Function<void()> f) to
  // schedule a function f to be run on EventBase destruction.)
  //
  // The only thread-safety guarantees of OnDestructionCallback are as follows:
  //   - Users may call runOnDestruction() from any thread, provided the caller
  //     is the only user of the callback, i.e., the callback is not already
  //     scheduled and there are no concurrent calls to schedule or cancel the
  //     callback.
  //   - Users may safely cancel() from any thread. Multiple calls to cancel()
  //     may execute concurrently. The only caveat is that it is not safe to
  //     call cancel() within the onEventBaseDestruction() callback.
  class OnDestructionCallback
      : public boost::intrusive::list_base_hook<
            boost::intrusive::link_mode<boost::intrusive::normal_link>> {
   public:
    OnDestructionCallback() = default;
    OnDestructionCallback(OnDestructionCallback&&) = default;
    OnDestructionCallback& operator=(OnDestructionCallback&&) = default;
    virtual ~OnDestructionCallback();

    // Attempt to cancel the callback. If the callback is running or has already
    // finished running, cancellation will fail. If the callback is running when
    // cancel() is called, cancel() will block until the callback completes.
    bool cancel();

    // Callback to be invoked during ~EventBase()
    virtual void onEventBaseDestruction() noexcept = 0;

   private:
    Function<void(OnDestructionCallback&)> eraser_;
    Synchronized<bool> scheduled_{std::in_place, false};

    using List = boost::intrusive::list<OnDestructionCallback>;

    void schedule(
        Function<void(OnDestructionCallback&)> linker,
        Function<void(OnDestructionCallback&)> eraser);

    friend class EventBase;
    friend class VirtualEventBase;

   protected:
    virtual void runCallback() noexcept;
  };

  class FunctionOnDestructionCallback : public OnDestructionCallback {
   public:
    explicit FunctionOnDestructionCallback(Function<void()> f)
        : f_(std::move(f)) {}

    void onEventBaseDestruction() noexcept final { f_(); }

   protected:
    void runCallback() noexcept override {
      OnDestructionCallback::runCallback();
      delete this;
    }

   private:
    Function<void()> f_;
  };

  struct Options {
    Options() {}

    /**
     * Skip measuring event base loop durations.
     *
     * Disabling it would likely improve performance, but will disable some
     * features that rely on time-measurement, including: observer, max latency
     * and avg loop time.
     */
    bool skipTimeMeasurement{false};

    Options& setSkipTimeMeasurement(bool skip) {
      skipTimeMeasurement = skip;
      return *this;
    }

    /**
     * Factory function for creating the backend.
     */
    using BackendFactory =
        std::function<std::unique_ptr<folly::EventBaseBackendBase>()>;
    BackendFactory backendFactory{nullptr};

    Options& setBackendFactory(BackendFactory factoryFn) {
      backendFactory = std::move(factoryFn);
      return *this;
    }

    /**
     * Granularity of the wheel timer in the EventBase.
     */
    std::chrono::milliseconds timerTickInterval{
        HHWheelTimer::DEFAULT_TICK_INTERVAL};

    Options& setTimerTickInterval(std::chrono::milliseconds interval) {
      timerTickInterval = interval;
      return *this;
    }

    /**
     * If non-zero, processing of loop callback and notification queue callbacks
     * will only be allowed to run for this timeslice within each iteration
     * (each gets one timeslice per iteration). This can be used to prevent the
     * queues to starve event handling or each other.
     *
     * Does not apply to runBeforeLoop() and runAfterLoop() callbacks.
     */
    std::chrono::milliseconds loopCallbacksTimeslice{0};

    Options& setLoopCallbacksTimeslice(std::chrono::milliseconds timeslice) {
      loopCallbacksTimeslice = timeslice;
      return *this;
    }
  };

  /**
   * Create a new EventBase object.
   *
   * Same as EventBase(true), which constructs an EventBase that measures time,
   * except that this also allows the timer granularity to be specified
   */

  explicit EventBase(std::chrono::milliseconds tickInterval);

  /**
   * Create a new EventBase object.
   *
   * Same as EventBase(true), which constructs an EventBase that measures time.
   */
  EventBase() : EventBase(true) {}

  /**
   * Create a new EventBase object.
   *
   * @param enableTimeMeasurement Informs whether this event base should measure
   *                              time. Disabling it would likely improve
   *                              performance, but will disable some features
   *                              that relies on time-measurement, including:
   *                              observer, max latency and avg loop time.
   */
  explicit EventBase(bool enableTimeMeasurement);

  EventBase(const EventBase&) = delete;
  EventBase& operator=(const EventBase&) = delete;

  /**
   * Create a new EventBase object that will use the specified libevent
   * event_base object to drive the event loop.
   *
   * The EventBase will take ownership of this event_base, and will call
   * event_base_free(evb) when the EventBase is destroyed.
   *
   * @param enableTimeMeasurement Informs whether this event base should measure
   *                              time. Disabling it would likely improve
   *                              performance, but will disable some features
   *                              that relies on time-measurement, including:
   *                              observer, max latency and avg loop time.
   */
  explicit EventBase(event_base* evb, bool enableTimeMeasurement = true);

  explicit EventBase(Options options);
  ~EventBase() override;

  /**
   * Runs the event loop.
   *
   * loop() will loop waiting for I/O or timeouts and invoking EventHandler
   * and AsyncTimeout callbacks as their events become ready.  loop() will
   * only return when there are no more events remaining to process, or after
   * terminateLoopSoon() has been called.
   *
   * loop() may be called again to restart event processing after a previous
   * call to loop() or loopForever() has returned.
   *
   * Returns true if the loop completed normally (if it processed all
   * outstanding requests, or if terminateLoopSoon() was called).  If an error
   * occurs waiting for events, false will be returned.
   */
  bool loop();

  /**
   * Same as loop(), but doesn't wait for all keep-alive tokens to be released.
   */
  [[deprecated("This should only be used in legacy unit tests")]] bool
  loopIgnoreKeepAlive();

  /**
   * Wait for some events to become active, run them, then return.
   *
   * When EVLOOP_NONBLOCK is set in flags, the loop won't block if there
   * are not any events to process.
   *
   * This is useful for callers that want to run the loop manually.
   *
   * Returns the same result as loop().
   */
  bool loopOnce(int flags = 0);

  struct LoopPollOptions {
    bool nonblock = true;
    LoopPollOptions() {}
  };

  /**
   * Poll the EventBase for active events, run them, then return. Unlike
   * loopOnce, the expectation is that loopPoll will be called multiple times
   * State is therefore persisted across calls to reflect that there is ongoing
   * polling. Control will be returned to the calling thread between iterations.
   * loopPollSetup and loopPollCleanup manage the maintained state across
   * loopPoll calls.
   *
   * This is useful for callers that want to run the loop manually but under the
   * context that there is continued polling being done by some thread against
   * the EventBase.
   *
   * Returns the same result as loop().
   *
   * Must be called within a corresponding pair of loopPollSetup and
   * loopPollCleanup; may be called many times within the pair.
   *
   * @param option: whether the function can block waiting for active events
   *  true:  return immediately after all active events are processed.
   *  false: block and keep running active events, until waken up by
   * notification.
   */
  bool loopPoll(LoopPollOptions options = {});

  /**
   * Sets up state for active polling to be done against the EventBase. Call
   * before polling via subsequent loopPoll calls.
   *
   * Must be matched with a corresponding call to loopPoolCleanup.
   */
  void loopPollSetup();

  /**
   * Clears state that was setup for active polling against the EventBase. Call
   * after polling via loopPoolSetup and the subsequent loopPoll calls.
   *
   * Must be matched with a corresponding call to loopPoolSetup.
   */
  void loopPollCleanup();

  /**
   * Same semantics as loop(), but, instead of blocking, it returns in a
   * "suspended" state. The caller must continue calling loopWithSuspension()
   * until a non-suspended state is reached.
   *
   * This is only supported with backends that support pollable fd, and intended
   * to enable external waiting for ready events through the fd: when the fd is
   * ready, the loop can be resumed and make progress.
   *
   * It is not allowed to call other loop methods, or to destroy the EventBase,
   * while in a suspended state.
   */
  enum class LoopStatus { kDone, kError, kSuspended };
  LoopStatus loopWithSuspension();

  /**
   * Runs the event loop.
   *
   * loopForever() behaves like loop(), except that it keeps running even if
   * when there are no more user-supplied EventHandlers or AsyncTimeouts
   * registered.  It will only return after terminateLoopSoon() has been
   * called.
   *
   * This is useful for callers that want to wait for other threads to call
   * runInEventBaseThread(), even when there are no other scheduled events.
   *
   * loopForever() may be called again to restart event processing after a
   * previous call to loop() or loopForever() has returned.
   *
   * Throws a std::system_error if an error occurs.
   */
  void loopForever();

  /**
   * Enable strict loop thread mode. This is intended for executors that take
   * ownership of the EventBase and run it continuously until joined. Once set,
   * it is not possible to unset it.
   *
   * In this mode:
   *
   * - isInEventBaseThread() returns false if the loop is not running.
   *
   * - Calling terminateLoopSoon() is not allowed, as the executor is in control
   *   of the loop lifetime.
   */
  void setStrictLoopThread();

  /**
   * Causes the event loop to exit soon.
   *
   * This will cause an existing call to loop() or loopForever() to stop event
   * processing and return, even if there are still events remaining to be
   * processed.
   *
   * It is safe to call terminateLoopSoon() from another thread to cause loop()
   * to wake up and return in the EventBase loop thread.  terminateLoopSoon()
   * may also be called from the loop thread itself (for example, a
   * EventHandler or AsyncTimeout callback may call terminateLoopSoon() to
   * cause the loop to exit after the callback returns.)  If the loop is not
   * running, this will cause the next call to loop to terminate soon after
   * starting.  If a loop runs out of work (and so terminates on its own)
   * concurrently with a call to terminateLoopSoon(), this may cause a race
   * condition.
   *
   * Note that the caller is responsible for ensuring that cleanup of all event
   * callbacks occurs properly.  Since terminateLoopSoon() causes the loop to
   * exit even when there are pending events present, there may be remaining
   * callbacks present waiting to be invoked.  If the loop is later restarted
   * pending events will continue to be processed normally, however if the
   * EventBase is destroyed after calling terminateLoopSoon() it is the
   * caller's responsibility to ensure that cleanup happens properly even if
   * some outstanding events are never processed.
   */
  void terminateLoopSoon();

  /**
   * Adds the given callback to a queue of things run after the current pass
   * through the event loop completes.  Note that if this callback calls
   * runInLoop() the new callback won't be called until the main event loop
   * has gone through a cycle.
   *
   * This method may only be called from the EventBase's thread.  This
   * essentially allows an event handler to schedule an additional callback to
   * be invoked after it returns.
   *
   * Use runInEventBaseThread() to schedule functions from another thread.
   *
   * The thisIteration parameter makes this callback run in this loop
   * iteration, instead of the next one, even if called from a
   * runInLoop callback (normal io callbacks that call runInLoop will
   * always run in this iteration).  This was originally added to
   * support detachEventBase, as a user callback may have called
   * terminateLoopSoon(), but we want to make sure we detach.  Also,
   * detachEventBase almost always must be called from the base event
   * loop to ensure the stack is unwound, since most users of
   * EventBase are not thread safe.
   *
   * Ideally we would not need thisIteration, and instead just use
   * runInLoop with loop() (instead of terminateLoopSoon).
   *
   * If loopCallbacksTimeslice is set, thisIteration is best-effort: if the
   * timeslice expires, the callback is deferred to the next iteration.
   */
  void runInLoop(
      LoopCallback* callback,
      bool thisIteration = false,
      std::shared_ptr<RequestContext> rctx = RequestContext::saveContext());

  /**
   * Convenience function to call runInLoop() with a folly::Function.
   *
   * This creates a LoopCallback object to wrap the folly::Function, and invoke
   * the folly::Function when the loop callback fires.  This is slightly more
   * expensive than defining your own LoopCallback, but more convenient in
   * areas that aren't too performance sensitive.
   *
   * This method may only be called from the EventBase's thread.  This
   * essentially allows an event handler to schedule an additional callback to
   * be invoked after it returns.
   *
   * Use runInEventBaseThread() to schedule functions from another thread.
   */
  void runInLoop(Func c, bool thisIteration = false);

  /**
   * Adds the given callback to a queue of things run on destruction
   * of current EventBase after the keepalive checks.
   *
   * This allows users of EventBase that run in it, but don't control it, to be
   * notified before EventBase gets destructed.
   *
   * Note: will be called from the thread that invoked EventBase destructor,
   *       before the final run of loop callbacks.
   */
  void runOnDestruction(OnDestructionCallback& callback);

  /**
   * Convenience function that allows users to pass in a Function<void()> to be
   * run on EventBase destruction.
   */
  void runOnDestruction(Func f);

  /**
   * Adds the given callback to a queue of things run at the start of the
   * destruction of the current EventBase, before any loop keep-alive handles
   * are checked.
   *
   * Note: will be called from the thread that invoked EventBase destructor,
   *       before the final run of loop callbacks.
   */
  void runOnDestructionStart(OnDestructionCallback& callback);

  /**
   * Convenience function that allows users to pass in a Function<void()> to be
   * run at the start of EventBase destruction, before any loop keep-alive
   * handles are checked.
   */
  void runOnDestructionStart(Func f);

  /**
   * Adds a callback that will run immediately *before* the event loop.
   * This is very similar to runInLoop(), but will not cause the loop to break:
   * For example, this callback could be used to get loop times.
   */
  void runBeforeLoop(LoopCallback* callback);

  /**
   * Adds a callback that will run immediately *after* the event loop.
   * This can be used to delay some processing until after all the normal loop
   * callback have been processed for this iteration.
   */
  void runAfterLoop(LoopCallback* callback);

  /**
   * Run the specified function in the EventBase's thread.
   *
   * This method is thread-safe, and may be called from another thread.
   *
   * If runInEventBaseThread() is called when the EventBase loop is not
   * running, the function call will be delayed until the next time the loop is
   * started.
   *
   * If the loop is terminated (and never later restarted) before it has a
   * chance to run the requested function, the function will be run upon the
   * EventBase's destruction.
   *
   * If two calls to runInEventBaseThread() are made from the same thread, the
   * functions will always be run in the order that they were scheduled.
   * Ordering between functions scheduled from separate threads is not
   * guaranteed.
   *
   * @param fn  The function to run.  The function must not throw any
   *     exceptions.
   */
  void runInEventBaseThread(Func fn) noexcept;

  /**
   * Run the specified function in the EventBase's thread.
   *
   * This method is thread-safe, and may be called from another thread.
   *
   * If runInEventBaseThreadAlwaysEnqueue() is called when the EventBase loop is
   * not running, the function call will be delayed until the next time the loop
   * is started.
   *
   * If the loop is terminated (and never later restarted) before it has a
   * chance to run the requested function, the function will be run upon the
   * EventBase's destruction.
   *
   * If two calls to runInEventBaseThreadAlwaysEnqueue() are made from the same
   * thread, the functions will always be run in the order that they were
   * scheduled. Ordering between functions scheduled from separate threads is
   * not guaranteed. If a call is made from the EventBase thread, the function
   * will not be executed inline and will be queued to the same queue as if the
   * call would have been made from a different thread
   *
   * @param fn  The function to run.  The function must not throw any
   *     exceptions.
   */
  void runInEventBaseThreadAlwaysEnqueue(Func fn) noexcept;

  /**
   * Run the specified function in the EventBase's thread
   *
   * This version takes a folly::Function and a RequestContext which will be in
   * scope when the Function is invoked.
   */
  void runInEventBaseThreadAlwaysEnqueue(
      std::shared_ptr<RequestContext>&& ctx, Func fn) noexcept;

  /*
   * Like runInEventBaseThread, but the caller waits for the callback to be
   * executed.
   */
  void runInEventBaseThreadAndWait(Func fn) noexcept;

  /**
   * Like runInEventBaseThreadAndWait, except if the caller is already in the
   * event base thread, the functor is simply run inline.
   */
  void runImmediatelyOrRunInEventBaseThreadAndWait(Func fn) noexcept;

  /**
   * Like runInEventBaseThread, but runs function immediately instead of at the
   * end of the loop when called from the eventbase thread.
   */
  void runImmediatelyOrRunInEventBaseThread(Func fn) noexcept;

  /**
   * Set the maximum desired latency in us and provide a callback which will be
   * called when that latency is exceeded.
   * OBS: This functionality depends on time-measurement.
   */
  void setMaxLatency(
      std::chrono::microseconds maxLatency,
      Func maxLatencyCob,
      bool dampen = true) {
    assert(enableTimeMeasurement_);
    maxLatency_ = maxLatency;
    maxLatencyCob_ = std::move(maxLatencyCob);
    dampenMaxLatency_ = dampen;
  }

  /**
   * Set smoothing coefficient for loop load average; # of milliseconds
   * for exp(-1) (1/2.71828...) decay.
   */
  void setLoadAvgMsec(std::chrono::milliseconds ms);

  /**
   * reset the load average to a desired value
   */
  void resetLoadAvg(double value = 0.0);

  /**
   * Get the average loop time in microseconds (an exponentially-smoothed ave)
   */
  double getAvgLoopTime() const {
    assert(enableTimeMeasurement_);
    return avgLoopTime_.get();
  }

  /**
   * Check if the event base loop is running.
   *
   * This may only be used as a sanity check mechanism; it cannot be used to
   * make any decisions; for that, consider waitUntilRunning().
   */
  bool isRunning() const {
    return loopTid_.load(std::memory_order_relaxed) != kNotRunningTid;
  }

  /**
   * Wait until the event loop starts (after starting the event loop thread).
   */
  void waitUntilRunning();

  size_t getNotificationQueueSize() const;

  /**
   * Returns the number of loop callbacks pending execution. If this is
   * non-zero, loopOnce() is guaranteed to run the callbacks without blocking.
   */
  size_t getNumLoopCallbacks() const;

  uint32_t getMaxReadAtOnce() const;
  void setMaxReadAtOnce(uint32_t maxAtOnce);

  /**
   * Verify that current thread is the EventBase thread.
   *
   * The definition of the EventBase thread depends on the strictLoopThread
   * option.
   *
   * When the loop is running, isInEventBaseThread() returns true if and only if
   * the current thread is the thread that is running the loop.
   * Otherwise,
   *
   * - In default mode (strictLoopThread = false), isInEventBaseThread() always
   *   returns true. this is to support use cases in which driving the loop is
   *   interleaved with calling other EventBase methods in the same thread.
   *
   *   In this mode, if the loop is not running continuously it is
   *   responsibility of the caller to ensure that all methods that may run
   *   non-thread-safe logic (including, for example,
   *   runImmediatelyOrRunInEventBaseThread*()) are serialized with loop runs.
   *
   * - In strict mode (strictLoopThread = true), isInEventBaseThread() always
   *   returns false. This is to support use cases in which the loop is run by a
   *   dedicated executor, possibly not continuously, so it is safe to rely on
   *   isInEventBaseThread() from any thread with with no risk of races. In this
   *   mode, the behavior is equivalent to inRunningEventBaseThread().
   */
  bool isInEventBaseThread() const;

  /**
   * Returns true if and only if the loop is running in the current thread.
   */
  bool inRunningEventBaseThread() const;

  /**
   * Equivalent to CHECK(isInEventBaseThread()) (and assert/DCHECK for
   * dcheckIsInEventBaseThread), but it prints more information on
   * failure.
   */
  void checkIsInEventBaseThread() const;
  void dcheckIsInEventBaseThread() const {
    if (kIsDebug) {
      checkIsInEventBaseThread();
    }
  }

  HHWheelTimer& timer() {
    if (!wheelTimer_) {
      wheelTimer_ = HHWheelTimer::newTimer(this, intervalDuration_);
    }
    return *wheelTimer_.get();
  }

  EventBaseBackendBase* getBackend() { return evb_.get(); }
  // --------- interface to underlying libevent base ------------
  // Avoid using these functions if possible.  These functions are not
  // guaranteed to always be present if we ever provide alternative EventBase
  // implementations that do not use libevent internally.
  [[deprecated(
      "Exposes libevent internals, and already returns null for backends that "
      "are not libevent, such as EpollBackend. Prefer folly's own event APIs; "
      "getBackend()->getEventBase() is the stopgap if you truly need the "
      "libevent base.")]]
  event_base* getLibeventBase() const;

  static const char* getLibeventVersion();
  const char* getLibeventMethod();

  /**
   * only EventHandler/AsyncTimeout subclasses and ourselves should
   * ever call this.
   *
   * This is used to mark the beginning of a new loop cycle by the
   * first handler fired within that cycle.
   *
   */
  void bumpHandlingTime() final {
    if (enableTimeMeasurement_ && nextLoopCnt_ != latestLoopCnt_) {
      bumpHandlingTimeSlow();
    }
  }

  /**
   * Give the loop a chance to return from the backend after an internal
   * event's callback ran, so that any loop callback it scheduled runs before
   * the loop blocks again.
   *
   * Called by EventHandler and AsyncTimeout; not meant for user code.
   */
  void yieldAfterInternalEvent() final;

  class SmoothLoopTime {
   public:
    explicit SmoothLoopTime(std::chrono::microseconds timeInterval)
        : expCoeff_(-1.0 / static_cast<double>(timeInterval.count())),
          value_(0.0) {
      VLOG(11) << "expCoeff_ " << expCoeff_ << " " << __PRETTY_FUNCTION__;
    }

    void setTimeInterval(std::chrono::microseconds timeInterval);
    void reset(double value = 0.0);

    void addSample(
        std::chrono::microseconds total, std::chrono::microseconds busy);

    double get() const {
      // Add the outstanding buffered times linearly, to avoid
      // expensive exponentiation
      auto lcoeff = static_cast<double>(buffer_time_.count()) * -expCoeff_;
      return value_ * (1.0 - lcoeff) +
          lcoeff * static_cast<double>(busy_buffer_.count());
    }

    void dampen(double factor) { value_ *= factor; }

   private:
    double expCoeff_;
    double value_;
    std::chrono::microseconds buffer_time_{0};
    std::chrono::microseconds busy_buffer_{0};
    std::size_t buffer_cnt_{0};
    static constexpr std::chrono::milliseconds buffer_interval_{10};
  };

  void setObserver(const std::shared_ptr<EventBaseObserver>& observer) {
    assert(enableTimeMeasurement_);
    observer_ = observer;
  }

  const std::shared_ptr<EventBaseObserver>& getObserver() { return observer_; }

  /**
   * Setup execution observation/instrumentation for every EventHandler
   * executed in this EventBase.
   *
   * @param observer EventHandle's execution observer.
   */
  void addExecutionObserver(ExecutionObserver* observer) {
    executionObserverList_.push_back(*observer);
  }

  void removeExecutionObserver(ExecutionObserver* observer) {
    executionObserverList_.erase(executionObserverList_.iterator_to(*observer));
  }

  /**
   * Gets the execution observer list associated with this EventBase.
   */
  ExecutionObserver::List& getExecutionObserverList() {
    return executionObserverList_;
  }

  /**
   * Set the name of the thread that runs this event base.
   */
  void setName(const std::string& name);

  /**
   * Returns the name of the thread that runs this event base.
   */
  const std::string& getName();

  /**
   * Returns the ID of the thread that this event base is running in
   */
  std::thread::id getLoopThreadId();

  /**
   * Returns the timepoint at the start of the loop callbacks.
   */
  std::chrono::steady_clock::time_point getLoopCallbacksStartTime() {
    return startWork_;
  }

  /// Implements the Executor interface
  void add(Cob fn) override { runInEventBaseThread(std::move(fn)); }

  /// Implements the DrivableExecutor interface
  void drive() override {
    loopKeepAliveCount_.fetch_add(1, std::memory_order_relaxed);
    SCOPE_EXIT {
      loopKeepAliveCount_.fetch_sub(1, std::memory_order_relaxed);
    };
    loopOnce();
  }

  // Implements the ScheduledExecutor interface
  void scheduleAt(Func&& fn, TimePoint const& timeout) override;

  // TimeoutManager
  void attachTimeoutManager(
      AsyncTimeout* obj, TimeoutManager::InternalEnum internal) final;

  void detachTimeoutManager(AsyncTimeout* obj) final;

  bool scheduleTimeout(
      AsyncTimeout* obj, TimeoutManager::timeout_type timeout) final;

  void cancelTimeout(AsyncTimeout* obj) final;

  bool isInTimeoutManagerThread() final { return isInEventBaseThread(); }

  // Returns a VirtualEventBase attached to this EventBase. Can be used to
  // pass to APIs which expect VirtualEventBase. This VirtualEventBase will be
  // destroyed together with the EventBase.
  //
  // Any number of VirtualEventBases instances may be independently constructed,
  // which are backed by this EventBase. This method should be only used if you
  // don't need to manage the life time of the VirtualEventBase used.
  folly::VirtualEventBase& getVirtualEventBase();

  /// Implements the IOExecutor interface
  EventBase* getEventBase() override;
  Executor::KeepAlive<EventBase> getEventBaseKeepAlive() override;

  /// Implements the GetThreadIdCollector interface
  WorkerProvider* getThreadIdCollector() override;

  static std::unique_ptr<EventBaseBackendBase> getDefaultBackend();
  static std::unique_ptr<EventBaseBackendBase> getTestBackend(int napiId);

  // Number of outstanding loop keepalives. The loop will not exit while this is
  // nonzero. Exposed for diagnostics, e.g. a shutdown-hang backstop checking
  // whether a worker EventBase is being held alive by a leaked
  // Executor::KeepAlive.
  size_t loopKeepAliveCount() const;

  // Optional, process-global tracing hooks invoked (on the calling thread) from
  // keepAliveAcquire() and keepAliveRelease() for every EventBase when set.
  // Default null => no effect and negligible cost (a relaxed load + branch), so
  // services that do not opt in pay nothing. Intended for opt-in diagnostics --
  // e.g. capturing the call sites of outstanding loop keepalives to debug a
  // shutdown hang. Callbacks run on the keepalive hot path: they must be fast
  // and must not throw.
  //
  // Acquire fires after the count is incremented, release before it is
  // decremented -- including when the release defers the actual decrement onto
  // the loop thread -- so a callback always runs while the reference it
  // describes is still held.
  //
  // Not synchronized with in-flight keepalive operations: install once at
  // startup, before the EventBases being traced exist. Replacing or clearing a
  // callback while other threads acquire or release may still invoke the old
  // one afterwards.
  using KeepAliveTraceCallback = void (*)(EventBase*);
  static void setKeepAliveAcquireTraceCallback(KeepAliveTraceCallback cb);
  static void setKeepAliveReleaseTraceCallback(KeepAliveTraceCallback cb);

 protected:
  bool keepAliveAcquire() noexcept override;
  void keepAliveRelease() noexcept override;

 private:
  class LoopCallbacksDeadline;
  class FuncRunner;
  class ThreadIdCollector;

  static constexpr pid_t kNotRunningTid = -1;
  static constexpr pid_t kSuspendedTid = -2;

  folly::VirtualEventBase* tryGetVirtualEventBase();

  void applyLoopKeepAlive();

  /*
   * Helper function that tells us whether we have already handled
   * some event/timeout/callback in this loop iteration.
   */
  bool nothingHandledYet() const noexcept;

  // Out-of-line slow path of bumpHandlingTime(), taken by the first handler
  // dispatched in a loop cycle.
  void bumpHandlingTimeSlow();

  bool isSuccess(LoopStatus status);

  struct LoopOptions {
    bool ignoreKeepAlive = false;
    bool allowSuspension = false;
  };

  bool loopBody(int flags, LoopOptions options);

  void loopMainSetup();
  LoopStatus loopMain(int flags, LoopOptions options);
  void loopMainCleanup();

  bool swapAndRunLoopCallbackList(
      LoopCallbackList& callbacks,
      LoopCallbackList& currentCallbacks,
      bool enforceDeadline = true);

  // executes any callbacks queued by runInLoop(); returns false if none found
  bool runLoopCallbacks();

  void initNotificationQueue();

  // Tick granularity to wheelTimer_
  const std::chrono::milliseconds intervalDuration_{
      HHWheelTimer::DEFAULT_TICK_INTERVAL};
  const bool enableTimeMeasurement_;
  const std::chrono::milliseconds loopCallbacksTimeslice_;
  bool strictLoopThread_ = false;

  // Loop state that needs to survive suspension.
  struct LoopState {
    std::chrono::steady_clock::time_point prev = {};
    std::chrono::steady_clock::time_point idleStart = {};
  };
  // Only set while the loop is running or suspended.
  std::optional<LoopState> loopState_;

  // ID of the thread running the loop (kNotRunningTid/kSuspendedTid if loop is
  // not running/suspended). Acts as lock to enforce loop mutual exclusion.
  std::atomic<pid_t> loopTid_{kNotRunningTid};
  // Store the std::thread::id as well, used to get/set thread names, and for
  // getLoopThreadId().
  std::atomic<std::thread::id> loopThread_{std::thread::id{}};

  // should only be accessed through public getter
  HHWheelTimer::UniquePtr wheelTimer_;

  LoopCallbackList loopCallbacks_;
  LoopCallbackList runBeforeLoopCallbacks_;
  LoopCallbackList runAfterLoopCallbacks_;
  Synchronized<OnDestructionCallback::List> onDestructionCallbacks_;
  Synchronized<OnDestructionCallback::List> preDestructionCallbacks_;

  // This will be null most of the time, but point to currentCallbacks
  // if we are in the middle of running loop callbacks, such that
  // runInLoop(..., true) will always run in the current loop
  // iteration.
  LoopCallbackList* runOnceCallbacks_;

  // stop_ is set by terminateLoopSoon() and is used by the main loop
  // to determine if it should exit
  std::atomic<bool> stop_;

  // A notification queue for runInEventBaseThread() to use
  // to send function requests to the EventBase thread.
  std::unique_ptr<EventBaseAtomicNotificationQueue<Func, FuncRunner>> queue_;
  std::atomic<size_t> loopKeepAliveCount_{0};
  bool loopKeepAliveActive_{false};

  // limit for latency in microseconds (0 disables)
  std::chrono::microseconds maxLatency_;

  // exponentially-smoothed average loop time for latency-limiting
  SmoothLoopTime avgLoopTime_;

  // smoothed loop time used to invoke latency callbacks; differs from
  // avgLoopTime_ in that it's scaled down after triggering a callback
  // to reduce spamminess
  SmoothLoopTime maxLatencyLoopTime_;

  // If true, max latency callbacks will use a dampened SmoothLoopTime, else
  // they'll use the raw loop time.
  bool dampenMaxLatency_ = true;

  // callback called when latency limit is exceeded
  Func maxLatencyCob_;

  // Wrap-around loop counter to detect beginning of each loop
  std::size_t nextLoopCnt_;
  std::size_t latestLoopCnt_;
  std::chrono::steady_clock::time_point startWork_;

  // Observer to export counters
  std::shared_ptr<EventBaseObserver> observer_;
  uint32_t observerSampleCount_;

  // EventHandler's execution observer list (in case multiple are registered)
  ExecutionObserver::List executionObserverList_;

  // Name of the thread running this EventBase
  std::string name_;

  // see EventBaseLocal
  friend class detail::EventBaseLocalBase;
  template <typename T>
  friend class EventBaseLocal;
  using LocalStorageMap = folly::F14FastMap<std::size_t, erased_unique_ptr>;
  LocalStorageMap localStorage_;
  folly::Synchronized<folly::F14FastSet<detail::EventBaseLocalBase*>>
      localStorageToDtor_;
  bool tryDeregister(detail::EventBaseLocalBase&);

  folly::once_flag virtualEventBaseInitFlag_;
  std::unique_ptr<VirtualEventBase> virtualEventBase_;

  // pointer to underlying backend class doing the heavy lifting
  std::unique_ptr<EventBaseBackendBase> evb_;

  std::unique_ptr<ThreadIdCollector> threadIdCollector_;
};

} // namespace folly
