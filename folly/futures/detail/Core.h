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
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include <folly/Executor.h>
#include <folly/Function.h>
#include <folly/Optional.h>
#include <folly/ScopeGuard.h>
#include <folly/Try.h>
#include <folly/Utility.h>
#include <folly/futures/detail/Types.h>
#include <folly/io/async/Request.h>
#include <folly/lang/Assume.h>
#include <folly/lang/Exception.h>
#include <folly/lang/Switch.h>
#include <folly/synchronization/AtomicUtil.h>

namespace folly {
namespace futures {
namespace detail {

/// See `Core` for details
enum class State : uint8_t {
  Start = 1 << 0,
  OnlyResult = 1 << 1,
  OnlyCallback = 1 << 2,
  OnlyCallbackAllowInline = 1 << 3,
  Proxy = 1 << 4,
  Done = 1 << 5,
  Empty = 1 << 6,
};
constexpr State operator&(State a, State b) {
  return State(uint8_t(a) & uint8_t(b));
}
constexpr State operator|(State a, State b) {
  return State(uint8_t(a) | uint8_t(b));
}
constexpr State operator^(State a, State b) {
  return State(uint8_t(a) ^ uint8_t(b));
}
constexpr State operator~(State a) {
  return State(~uint8_t(a));
}

class DeferredExecutor;

class UniqueDeleter {
 public:
  void operator()(DeferredExecutor* ptr);
};

using DeferredWrapper = std::unique_ptr<DeferredExecutor, UniqueDeleter>;

/**
 * Wrapper type that represents either a KeepAlive or a DeferredExecutor.
 * Acts as if a type-safe tagged union of the two using knowledge that the two
 * can safely be distinguished.
 */
class KeepAliveOrDeferred {
 private:
  using KA = Executor::KeepAlive<>;
  using DW = DeferredWrapper;

 public:
  KeepAliveOrDeferred() noexcept : state_(State::Deferred), deferred_(DW{}) {}

  /* implicit */ KeepAliveOrDeferred(KA ka) noexcept
      : state_(State::KeepAlive) {
    ::new (&keepAlive_) KA{std::move(ka)};
  }

  /* implicit */ KeepAliveOrDeferred(DW deferred) noexcept
      : state_(State::Deferred) {
    ::new (&deferred_) DW{std::move(deferred)};
  }

  KeepAliveOrDeferred(KeepAliveOrDeferred&& other) noexcept;

  ~KeepAliveOrDeferred();

  KeepAliveOrDeferred& operator=(KeepAliveOrDeferred&& other) noexcept;

  DeferredExecutor* getDeferredExecutor() const noexcept;

  Executor* getKeepAliveExecutor() const noexcept;

  KA stealKeepAlive() && noexcept;

  DW stealDeferred() && noexcept;

  bool isDeferred() const noexcept;

  bool isKeepAlive() const noexcept;

  KeepAliveOrDeferred copy() const;

  explicit operator bool() const noexcept;

 private:
  friend class DeferredExecutor;

  enum class State { Deferred, KeepAlive } state_;
  union {
    DW deferred_;
    KA keepAlive_;
  };
};

inline bool KeepAliveOrDeferred::isDeferred() const noexcept {
  return state_ == State::Deferred;
}

inline bool KeepAliveOrDeferred::isKeepAlive() const noexcept {
  return state_ == State::KeepAlive;
}

/**
 * Defer work until executor is actively boosted.
 */
class DeferredExecutor final {
 public:
  // addFrom will:
  //  * run func inline if there is a stored executor and completingKA matches
  //    the stored executor
  //  * enqueue func into the stored executor if one exists
  //  * store func until an executor is set otherwise
  void addFrom(
      Executor::KeepAlive<>&& completingKA,
      Executor::KeepAlive<>::KeepAliveFunc func);

  Executor* getExecutor() const;

  void setExecutor(
      folly::Executor::KeepAlive<> executor, bool inlineUnsafe = false);

  void setNestedExecutors(std::vector<DeferredWrapper> executors);

  void detach();

  DeferredWrapper copy();

  static DeferredWrapper create();

 private:
  friend class UniqueDeleter;

  DeferredExecutor();

  void acquire();
  void release();

  enum class State { EMPTY, HAS_FUNCTION, HAS_EXECUTOR, DETACHED };

  std::atomic<State> state_{State::EMPTY};
  Executor::KeepAlive<>::KeepAliveFunc func_;
  folly::Executor::KeepAlive<> executor_;
  std::unique_ptr<std::vector<DeferredWrapper>> nestedExecutors_;
  std::atomic<ssize_t> keepAliveCount_{1};
};

class InterruptHandler {
 public:
  virtual ~InterruptHandler();

  virtual void handle(const folly::exception_wrapper& ew) = 0;

  void acquire();
  void release();

 private:
  std::atomic<ssize_t> refCount_{1};
};

template <class F>
class InterruptHandlerImpl final : public InterruptHandler {
 public:
  template <typename R>
  explicit InterruptHandlerImpl(R&& f) noexcept(
      noexcept(F(static_cast<R&&>(f))))
      : f_(static_cast<R&&>(f)) {}

  void handle(const folly::exception_wrapper& ew) override { f_(ew); }

 private:
  F f_;
};

/// The shared state object for Future and Promise.
///
/// Nomenclature:
///
/// - "result": a `Try` object which, when set, contains a `T` or exception.
/// - "move-out the result": used to mean the `Try` object and/or its contents
///   are moved-out by a move-constructor or move-assignment. After the result
///   is set, Core itself never modifies (including moving out) the result;
///   however the docs refer to both since caller-code can move-out the result
///   implicitly (see below for examples) whereas other types of modifications
///   are more explicit in the caller code.
/// - "callback": a function provided by the future which Core may invoke. The
///   thread in which the callback is invoked depends on the executor; if there
///   is no executor or an inline executor the thread-choice depends on timing.
/// - "executor": an object which may in the future invoke a provided function
///   (some executors may, as a matter of policy, simply destroy provided
///   functions without executing them).
/// - "consumer thread": the thread which currently owns the Future and which
///   may provide the callback and/or the interrupt.
/// - "producer thread": the thread which owns the Future and which may provide
///   the result and which may provide the interrupt handler.
/// - "interrupt": if provided, an object managed by (if non-empty)
///   `exception_wrapper`.
/// - "interrupt handler": if provided, a function-object passed to
///   `promise.setInterruptHandler()`. Core invokes the interrupt handler with
///   the interrupt when both are provided (and, best effort, if there is not
///   yet any result).
///
/// Core holds three sets of data, each of which is concurrency-controlled:
///
/// - The primary producer-to-consumer info-flow: this info includes the result,
///   callback, executor, and a priority for running the callback. Management of
///   and concurrency control for this info is by an FSM based on `enum class
///   State`. All state transitions are atomic; other producer-to-consumer data
///   is sometimes modified within those transitions; see below for details.
/// - The consumer-to-producer interrupt-request flow: this info includes an
///   interrupt-handler and an interrupt.
/// - Lifetime control info: this includes two reference counts, both which are
///   internally synchronized (atomic).
///
/// The FSM to manage the primary producer-to-consumer info-flow has these
///   allowed (atomic) transitions:
///
///   +----------------------------------------------------------------+
///   |                       ---> OnlyResult -----                    |
///   |                     /                       \                  |
///   |                  (setResult())             (setCallback())     |
///   |                   /                           \                |
///   |   Start --------->                              ------> Done   |
///   |     \             \                           /                |
///   |      \           (setCallback())           (setResult())       |
///   |       \             \                       /                  |
///   |        \              ---> OnlyCallback ---                    |
///   |         \           or OnlyCallbackAllowInline                 |
///   |          \                                  \                  |
///   |      (setProxy())                          (setProxy())        |
///   |            \                                  \                |
///   |             \                                   ------> Empty  |
///   |              \                                /                |
///   |               \                            (setCallback())     |
///   |                \                            /                  |
///   |                  --------> Proxy ----------                    |
///   +----------------------------------------------------------------+
///
/// States and the corresponding producer-to-consumer data status & ownership:
///
/// - Start: has neither result nor callback. While in this state, the producer
///   thread may set the result (`setResult()`) or the consumer thread may set
///   the callback (`setCallback()`).
/// - OnlyResult: producer thread has set the result and must never access it.
///   The result is logically owned by, and possibly modified or moved-out by,
///   the consumer thread. Callers of the future object can do arbitrary
///   modifications, including moving-out, via continuations or via non-const
///   and/or rvalue-qualified `future.result()`, `future.value()`, etc.
///   Future/SemiFuture proper also move-out the result in some cases, e.g.,
///   in `wait()`, `get()`, when passing through values or results from core to
///   core, as `then-value` and `then-error`, etc.
/// - OnlyCallback: consumer thread has set a callback/continuation. From this
///   point forward only the producer thread can safely access that callback
///   (see `setResult()` and `doCallback()` where the producer thread can both
///   read and modify the callback).
/// - OnlyCallbackAllowInline: as for OnlyCallback but the core is allowed to
///   run the callback inline with the setResult call, and therefore in the
///   execution context and on the executor that executed the callback on the
///   previous core, rather than adding the callback to the current Core's
///   executor. This will only happen if the executor on which the previous
///   callback is executing, and on which it is calling setResult, is the same
///   as the executor the current core would add the callback to.
/// - Proxy: producer thread has set a proxy core which the callback should be
///   proxied to.
/// - Done: callback can be safely accessed only within `doCallback()`, which
///   gets called on exactly one thread exactly once just after the transition
///   to Done. The future object will have determined whether that callback
///   has/will move-out the result, but either way the result remains logically
///   owned exclusively by the consumer thread (the code of Future/SemiFuture,
///   of the continuation, and/or of callers of `future.result()`, etc.).
/// - Empty: the core successfully proxied the callback and is now empty.
///
/// Start state:
///
/// - Start: e.g., `Core<X>::make()`.
/// - (See also `Core<X>::make(x)` which logically transitions Start =>
///   OnlyResult within the underlying constructor.)
///
/// Terminal states:
///
/// - OnlyResult: a terminal state when a callback is never attached, and also
///   sometimes when a callback is provided, e.g., sometimes when
///   `future.wait()` and/or `future.get()` are used.
/// - Done: a terminal state when `future.then()` is used, and sometimes also
///   when `future.wait()` and/or `future.get()` are used.
/// - Proxy: a terminal state if proxy core was set, but callback was never set.
/// - Empty: a terminal state when proxying a callback was successful.
///
/// Notes and caveats:
///
/// - Unfortunately, there are things that users can do to break concurrency and
///   we can't detect that. However users should be ok if they follow move
///   semantics religiously wrt threading.
/// - Futures and/or Promises can and usually will migrate between threads,
///   though this usually happens within the API code. For example, an async
///   operation will probably make a promise-future pair (see overloads of
///   `makePromiseContract()`), then move the Promise into another thread that
///   will eventually fulfill it.
/// - Things get slightly more complicated with executors and via, but the
///   principle is the same.
/// - In general, as long as the user doesn't access a future or promise object
///   from more than one thread at a time there won't be any problems.
//
/// Implementation is split between CoreBase and Core<T>. T-independent bits are
/// in CoreBase in order to minimize the instantiation cost of Core<T>.
class CoreBase {
 protected:
  using Context = std::shared_ptr<RequestContext>;
  using Callback = folly::Function<void(
      CoreBase&, Executor::KeepAlive<>&&, exception_wrapper* ew)>;

 public:
  // not copyable
  CoreBase(CoreBase const&) = delete;
  CoreBase& operator=(CoreBase const&) = delete;

  // not movable (see comment in the implementation of Future::then)
  CoreBase(CoreBase&&) noexcept = delete;
  CoreBase& operator=(CoreBase&&) = delete;

  /// May call from any thread
  bool hasCallback() const noexcept {
    constexpr auto allowed = State::OnlyCallback |
        State::OnlyCallbackAllowInline | State::Done | State::Empty;
    auto const state = state_.load(std::memory_order_acquire);
    return State() != (state & allowed);
  }

  /// May call from any thread
  ///
  /// True if state is OnlyResult or Done.
  ///
  /// Identical to `this->ready()`
  bool hasResult() const noexcept;

  /// May call from any thread
  ///
  /// True if state is OnlyResult or Done.
  ///
  /// Identical to `this->hasResult()`
  bool ready() const noexcept { return hasResult(); }

  /// Called by a destructing Future (in the consumer thread, by definition).
  /// Calls `delete this` if there are no more references to `this`
  /// (including if `detachPromise()` is called previously or concurrently).
  void detachFuture() noexcept { detachOne(); }

  /// Called by a destructing Promise (in the producer thread, by definition).
  /// Calls `delete this` if there are no more references to `this`
  /// (including if `detachFuture()` is called previously or concurrently).
  void detachPromise() noexcept {
    DCHECK(hasResult());
    detachOne();
  }

  /// Call only from consumer thread, either before attaching a callback or
  /// after the callback has already been invoked, but not concurrently with
  /// anything which might trigger invocation of the callback.
  void setExecutor(KeepAliveOrDeferred&& x) {
    DCHECK(
        state_ != State::OnlyCallback &&
        state_ != State::OnlyCallbackAllowInline);
    executor_ = std::move(x);
  }

  Executor* getExecutor() const;

  DeferredExecutor* getDeferredExecutor() const;

  DeferredWrapper stealDeferredExecutor();

  /// Call only from consumer thread
  ///
  /// Eventual effect is to pass `e` to the Promise's interrupt handler, either
  /// synchronously within this call or asynchronously within
  /// `setInterruptHandler()`, depending on which happens first (a coin-toss if
  /// the two calls are racing).
  ///
  /// Has no effect if it was called previously.
  /// Has no effect if State is OnlyResult or Done.
  void raise(exception_wrapper e);

  /// Copy the interrupt handler from another core. This should be done only
  /// when initializing a new core (interruptHandler_ must be nullptr).
  void initCopyInterruptHandlerFrom(const CoreBase& other);

  /// Call only from producer thread
  ///
  /// May invoke `fn()` (passing the interrupt) synchronously within this call
  /// (if `raise()` preceded or perhaps if `raise()` is called concurrently).
  ///
  /// Has no effect if State is OnlyResult or Done.
  ///
  /// Note: `fn()` must not touch resources that are destroyed immediately after
  ///   `setResult()` is called. Reason: it is possible for `fn()` to get called
  ///   asynchronously (in the consumer thread) after the producer thread calls
  ///   `setResult()`.
  template <typename F>
  void setInterruptHandler(F&& fn) {
    using handler_type = InterruptHandlerImpl<std::decay_t<F>>;
    if (hasResult()) {
      return;
    }
    handler_type* handler = nullptr;
    auto interrupt = interrupt_.load(std::memory_order_acquire);
    FOLLY_EXHAUSTIVE_SWITCH({
      switch (interrupt & InterruptMask) {
        case InterruptInitial: { // store the handler
          assert(!interrupt);
          handler = new handler_type(static_cast<F&&>(fn));
          auto exchanged = folly::atomic_compare_exchange_strong_explicit(
              &interrupt_,
              &interrupt,
              reinterpret_cast<uintptr_t>(handler) | InterruptHasHandler,
              std::memory_order_release,
              std::memory_order_acquire);
          if (exchanged) {
            return;
          }
          // lost the race!
          if (interrupt & InterruptHasHandler) {
            terminate_with<std::logic_error>("set-interrupt-handler race");
          }
          assert(interrupt & InterruptHasObject);
          [[fallthrough]];
        }
        case InterruptHasObject: { // invoke over the stored object
          auto exchanged = interrupt_.compare_exchange_strong(
              interrupt, InterruptTerminal, std::memory_order_relaxed);
          if (!exchanged) {
            terminate_with<std::logic_error>("set-interrupt-handler race");
          }
          auto pointer = interrupt & ~InterruptMask;
          auto object = reinterpret_cast<exception_wrapper*>(pointer);
          if (handler) {
            handler->handle(*object);
            delete handler;
          } else {
            // mimic constructing and invoking a handler: 1 copy; non-const
            // invoke
            auto fn_ = static_cast<F&&>(fn);
            fn_(std::as_const(*object));
          }
          delete object;
          return;
        }
        case InterruptHasHandler: // fail all calls after the first
          terminate_with<std::logic_error>("set-interrupt-handler duplicate");
        case InterruptTerminal: // fail all calls after the first
          terminate_with<std::logic_error>("set-interrupt-handler after done");
        default:
          folly::assume_unreachable();
      }
    }); // FOLLY_EXHAUSTIVE_SWITCH
  }

 protected:
  CoreBase(State state, unsigned char attached) noexcept
      : state_(state), attached_(attached) {}

  virtual ~CoreBase();

  // Helper class that stores a pointer to the `Core` object and calls
  // `derefCallback` and `detachOne` in the destructor.
  class CoreAndCallbackReference;

  // interrupt_ is an atomic acyclic finite state machine with guarded state
  // which takes the form of either a pointer to a copy of the object passed to
  // raise or a pointer to a copy of the handler passed to setInterruptHandler
  //
  // the object and the handler values are both at least pointer-aligned so they
  // leave the bottom 2 bits free on all supported platforms; these bits are
  // stolen for the state machine
  enum : uintptr_t {
    InterruptMask = 0x3u,
  };
  enum InterruptState : uintptr_t {
    InterruptInitial = 0x0u,
    InterruptHasHandler = 0x1u,
    InterruptHasObject = 0x2u,
    InterruptTerminal = 0x3u,
  };

  void setCallback_(
      Callback&& callback,
      std::shared_ptr<folly::RequestContext>&& context,
      futures::detail::InlineContinuation allowInline);

  void setResult_(Executor::KeepAlive<>&& completingKA);
  void setProxy_(CoreBase* proxy);
  void doCallback(
      Executor::KeepAlive<>&& completingKA,
      futures::detail::InlineContinuation allowInline);
  void proxyCallback(State priorState);

  void detachOne() noexcept;

  void derefCallback() noexcept;

  template <typename Self>
  FOLLY_ERASE static Self& walkProxyChainImpl(Self& self) noexcept {
    DCHECK(self.hasResult());
    auto core = &self;
    while (core->state_.load(std::memory_order_relaxed) == State::Proxy) {
      core = core->proxy_;
    }
    return *core;
  }
  FOLLY_ERASE CoreBase& walkProxyChain() noexcept {
    return walkProxyChainImpl(*this);
  }
  FOLLY_ERASE CoreBase const& walkProxyChain() const noexcept {
    return walkProxyChainImpl(*this);
  }

  bool destroyDerived() noexcept;

  Callback callback_;
  std::atomic<State> state_;
  std::atomic<unsigned char> attached_;
  std::atomic<unsigned char> callbackReferences_{0};
  KeepAliveOrDeferred executor_;
  Context context_;
  std::atomic<uintptr_t> interrupt_{}; // see InterruptMask, InterruptState
  CoreBase* proxy_;
};

template <typename T>
class ResultHolder {
 protected:
  ResultHolder() {}
  ~ResultHolder() {}
  // Using a separate base class allows us to control the placement of result_,
  // making sure that it's in the same cache line as the vtable pointer and the
  // callback_ (assuming it's small enough).
  union {
    Try<T> result_;
  };
};

template <typename T>
class Core final : private ResultHolder<T>, public CoreBase {
  static_assert(
      !std::is_void<T>::value,
      "void futures are not supported. Use Unit instead.");

 public:
  using Result = Try<T>;

  /// State will be Start
  static Core* make() { return new Core(); }

  /// State will be OnlyResult
  /// Result held will be move-constructed from `t`
  static Core* make(Try<T>&& t) { return new Core(std::move(t)); }

  /// State will be OnlyResult
  /// Result held will be the `T` constructed from forwarded `args`
  template <typename... Args>
  static Core<T>* make(std::in_place_t, Args&&... args) {
    return new Core<T>(std::in_place, static_cast<Args&&>(args)...);
  }

  /// Call only from consumer thread (since the consumer thread can modify the
  ///   referenced Try object; see non-const overloads of `future.result()`,
  ///   etc., and certain Future-provided callbacks which move-out the result).
  ///
  /// Unconditionally returns a reference to the result.
  ///
  /// State dependent preconditions:
  ///
  /// - Start, OnlyCallback or OnlyCallbackAllowInline: Never safe - do not
  /// call. (Access in those states
  ///   would be undefined behavior since the producer thread can, in those
  ///   states, asynchronously set the referenced Try object.)
  /// - OnlyResult: Always safe. (Though the consumer thread should not use the
  ///   returned reference after it attaches a callback unless it knows that
  ///   the callback does not move-out the referenced result.)
  /// - Done: Safe but sometimes unusable. (Always returns a valid reference,
  ///   but the referenced result may or may not have been modified, including
  ///   possibly moved-out, depending on what the callback did; some but not
  ///   all callbacks modify (possibly move-out) the result.)
  Try<T>& getTry() {
    return static_cast<decltype(*this)&>(walkProxyChain()).result_;
  }
  Try<T> const& getTry() const {
    return static_cast<decltype(*this)&>(walkProxyChain()).result_;
  }

  /// Call only from consumer thread.
  /// Call only once - else undefined behavior.
  ///
  /// See FSM graph for allowed transitions.
  ///
  /// If it transitions to Done, synchronously initiates a call to the callback,
  /// and might also synchronously execute that callback (e.g., if there is no
  /// executor or if the executor is inline).
  template <class F>
  void setCallback(
      F&& func,
      std::shared_ptr<folly::RequestContext>&& context,
      futures::detail::InlineContinuation allowInline) {
    Callback callback =
        [func_2 = static_cast<F&&>(func)](
            CoreBase& coreBase,
            Executor::KeepAlive<>&& ka,
            exception_wrapper* ew) mutable {
          func_2(std::move(ka), setCallbackGetResult(coreBase, ew));
        };

    setCallback_(std::move(callback), std::move(context), allowInline);
  }

  /// Call only from producer thread.
  /// Call only once - else undefined behavior.
  ///
  /// See FSM graph for allowed transitions.
  ///
  /// If it transitions to Done, synchronously initiates a call to the callback,
  /// and might also synchronously execute that callback (e.g., if there is no
  /// executor or if the executor is inline).
  void setResult(Try<T>&& t) {
    setResult(Executor::KeepAlive<>{}, std::move(t));
  }

  /// Call only from producer thread.
  /// Call only once - else undefined behavior.
  ///
  /// See FSM graph for allowed transitions.
  ///
  /// If it transitions to Done, synchronously initiates a call to the callback,
  /// and might also synchronously execute that callback (e.g., if there is no
  /// executor, if the executor is inline or if completingKA represents the
  /// same executor as does executor_).
  void setResult(Executor::KeepAlive<>&& completingKA, Try<T>&& t) {
    ::new (&this->result_) Result(std::move(t));
    setResult_(std::move(completingKA));
  }

  /// Call only from producer thread.
  /// Call only once - else undefined behavior.
  ///
  /// See FSM graph for allowed transitions.
  ///
  /// This can not be called concurrently with setResult().
  void setProxy(Core* proxy) {
    // NOTE: We could just expose this from the base, but that accepts any
    // CoreBase, while we want to enforce the same Core<T> in the interface.
    setProxy_(proxy);
  }

 private:
  Core() : CoreBase(State::Start, 2) {}

  explicit Core(Try<T>&& t) : CoreBase(State::OnlyResult, 1) {
    new (&this->result_) Result(std::move(t));
  }

  template <typename... Args>
  explicit Core(std::in_place_t, Args&&... args) noexcept(
      std::is_nothrow_constructible<T, Args&&...>::value)
      : CoreBase(State::OnlyResult, 1) {
    new (&this->result_) Result(std::in_place, static_cast<Args&&>(args)...);
  }

  ~Core() override {
    if (destroyDerived()) {
      this->result_.~Result();
    }
  }

  static Try<T>&& setCallbackGetResult(
      CoreBase& coreBase, exception_wrapper* ew) {
    auto& core = static_cast<Core&>(coreBase);
    if (ew != nullptr) {
      core.result_.emplaceException(std::move(*ew));
    }
    return std::move(core.result_);
  }
};

inline Executor* CoreBase::getExecutor() const {
  if (!executor_.isKeepAlive()) {
    return nullptr;
  }
  return executor_.getKeepAliveExecutor();
}

inline DeferredExecutor* CoreBase::getDeferredExecutor() const {
  if (!executor_.isDeferred()) {
    return {};
  }

  return executor_.getDeferredExecutor();
}

#if FOLLY_USE_EXTERN_FUTURE_UNIT
// limited to the instances unconditionally forced by the futures library
extern template class Core<folly::Unit>;
#endif

} // namespace detail
} // namespace futures

} // namespace folly
