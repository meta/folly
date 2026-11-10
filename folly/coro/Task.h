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

//
// Docs: https://fburl.com/fbcref_coro_task
//

#pragma once

#include <exception>
#include <functional>
#include <type_traits>

#include <glog/logging.h>

#include <folly/CancellationToken.h>
#include <folly/DefaultKeepAliveExecutor.h>
#include <folly/Executor.h>
#include <folly/GLog.h>
#include <folly/Portability.h>
#include <folly/ScopeGuard.h>
#include <folly/Traits.h>
#include <folly/Try.h>
#include <folly/coro/BasePromise.h>
#include <folly/coro/Coroutine.h>
#include <folly/coro/Invoke.h>
#include <folly/coro/Result.h>
#include <folly/coro/ScopeExit.h>
#include <folly/coro/Traits.h>
#include <folly/coro/ViaIfAsync.h>
#include <folly/coro/WithAsyncStack.h>
#include <folly/coro/WithCancellation.h>
#include <folly/coro/detail/InlineTask.h>
#include <folly/coro/detail/Malloc.h>
#include <folly/coro/detail/Traits.h>
#include <folly/futures/Future.h>
#include <folly/io/async/Request.h>
#include <folly/lang/Assume.h>
#include <folly/lang/MustUseImmediately.h>
#include <folly/lang/SafeAlias-fwd.h>
#include <folly/result/result.h>
#include <folly/result/try.h>
#include <folly/tracing/AsyncStack.h>

#if FOLLY_HAS_COROUTINES

namespace folly::coro {

template <typename T = void>
class Task;

template <typename T = void>
class TaskWithExecutor;

namespace detail {

class TaskPromiseBase;

class TaskPromisePrivate {
 private:
  friend TaskPromiseBase;
  TaskPromisePrivate() = default;
};

class TaskPromiseBase : public BasePromise<> {
  static TaskPromisePrivate privateTag() { return TaskPromisePrivate{}; }

 protected:
  class FinalAwaiter {
   public:
    bool await_ready() noexcept { return false; }

    template <typename Promise>
    coroutine_handle<> await_suspend_promise(Promise& promise) noexcept {
      // If ScopeExitTask has been attached, then we expect that the
      // ScopeExitTask will handle the lifetime of the async stack. See
      // ScopeExitTaskPromise's FinalAwaiter for more details.
      //
      // This is a bit untidy, and hopefully something we can replace with
      // a virtual wrapper over coroutine_handle that handles the pop for us.
      if (promise.scopeExitRef(privateTag())) {
        promise.scopeExitRef(privateTag())
            .promise()
            .setContext(
                promise.continuationRef(privateTag()),
                &promise.getAsyncFrame(),
                promise.executorRef(privateTag()).get_alias(),
                promise.result().hasException()
                    ? promise.result().exception()
                    : exception_wrapper{});
        return promise.scopeExitRef(privateTag());
      }

      folly::popAsyncStackFrameCallee(promise.getAsyncFrame());
      if (promise.result().hasException()) {
        auto [handle, frame] =
            promise.continuationRef(privateTag())
                .getErrorHandle(promise.result().exception());
        return handle.getHandle();
      }
      return promise.continuationRef(privateTag()).getHandle();
    }

    template <typename Promise>
    FOLLY_CORO_AWAIT_SUSPEND_NONTRIVIAL_ATTRIBUTES coroutine_handle<>
    await_suspend(coroutine_handle<Promise> coro) noexcept {
      return await_suspend_promise(coro.promise());
    }

    [[noreturn]] void await_resume() noexcept { folly::assume_unreachable(); }
  };

  TaskPromiseBase() noexcept = default;
  ~TaskPromiseBase() = default;

 public:
  FOLLY_CORO_DEFINE_FRAME_ALLOCATOR(
      ::folly::coro::detail::CoroFrameAsyncAllocator)

  suspend_always initial_suspend() noexcept { return {}; }

  FinalAwaiter final_suspend() noexcept { return {}; }

  folly::Executor::KeepAlive<> getExecutor() const noexcept {
    return executor_;
  }

  // These getters exist so that `FinalAwaiter` can interact with wrapped
  // `TaskPromise`s, and not just `TaskPromiseBase` descendants.  We use a
  // private tag to let `TaskWrapper` call them without becoming a `friend`.
  auto& scopeExitRef(TaskPromisePrivate /*unused*/) { return scopeExit_; }
  // FIXME: `result/coro.h` checks if this overload is callable to decide when
  // something is a task-promise.  A second use-case would merit a concept.
  auto& continuationRef(TaskPromisePrivate /*unused*/) { return continuation_; }
  // Unlike `getExecutor()`, does not copy an atomic.
  auto& executorRef(TaskPromisePrivate /*unused*/) { return executor_; }

 private:
  template <typename>
  friend class folly::coro::TaskWithExecutor;

  template <typename>
  friend class folly::coro::Task;

  friend coroutine_handle<ScopeExitTaskPromiseBase> tag_invoke(
      cpo_t<co_attachScopeExit> /*unused*/,
      TaskPromiseBase& p,
      coroutine_handle<ScopeExitTaskPromiseBase> scopeExit) noexcept {
    return std::exchange(p.scopeExit_, scopeExit);
  }

  // From the base: continuation_, asyncFrame_, executor_, cancelToken_,
  // hasCancelTokenOverride_
  coroutine_handle<ScopeExitTaskPromiseBase> scopeExit_;
};

// Separate from `TaskPromiseBase` so the compiler has less to specialize.
template <typename Promise, typename T>
class TaskPromiseCrtpBase
    : public TaskPromiseBase,
      public ExtendedCoroutinePromiseCrtp<Promise> {
 public:
  using StorageType = detail::lift_lvalue_reference_t<T>;

  Task<T> get_return_object() noexcept;

  void unhandled_exception() noexcept {
    result_.emplaceException(exception_wrapper{current_exception()});
  }

  Try<StorageType>& result() { return result_; }

  auto yield_value(co_error ex) {
    result_.emplaceException(std::move(ex.exception()));
    return final_suspend();
  }

  template <typename ContainerRef>
  auto yield_value(co_result<StorageType, ContainerRef> result) {
    std::move(result).assignTo(result_);
    return final_suspend();
  }

  using BasePromise<>::await_transform;

  auto await_transform(co_safe_point_t /*unused*/) noexcept {
    return do_safe_point<FinalAwaiter>(*this);
  }

  // Unlike `getErrorHandleUncheckedImpl`, checks the type of `me`.
  static std::optional<ExtendedCoroutineHandle::ErrorHandle> getErrorHandleImpl(
      Promise& me, exception_wrapper& ex) {
    return getErrorHandleUncheckedImpl(me, ex);
  }

 protected:
  TaskPromiseCrtpBase() noexcept = default;
  ~TaskPromiseCrtpBase() = default;

  Try<StorageType> result_;
};

template <typename T>
class TaskPromise final : public TaskPromiseCrtpBase<TaskPromise<T>, T> {
 public:
  static_assert(
      !std::is_rvalue_reference_v<T>,
      "Task<T&&> is not supported. "
      "Consider using Task<T> or Task<std::unique_ptr<T>> instead.");
  friend class TaskPromiseBase;

  using StorageType =
      typename TaskPromiseCrtpBase<TaskPromise<T>, T>::StorageType;

  template <typename U>
  static constexpr bool IsDeprecatedTryReturn =
      std::is_same_v<std::remove_cvref_t<U>, Try<StorageType>> ||
      (std::is_same_v<StorageType, Unit> &&
       std::is_same_v<std::remove_cvref_t<U>, Try<void>>);

  TaskPromise() noexcept = default;

  template <typename U = T>
    requires IsDeprecatedTryReturn<U>
  [[deprecated("Use `co_yield co_result(...)` instead.")]]
  void return_value(U&& value) {
    DCHECK(value.hasValue() || (value.hasException() && value.exception()));
    this->result_ = static_cast<Try<StorageType>>(static_cast<U&&>(value));
  }

  template <typename U = T>
    requires(!IsDeprecatedTryReturn<U>)
  void return_value(U&& value) {
    static_assert(
        std::is_convertible_v<U&&, StorageType>,
        "cannot convert return value to type T");
    this->result_.emplace(static_cast<U&&>(value));
  }
};

template <>
class TaskPromise<void> final
    : public TaskPromiseCrtpBase<TaskPromise<void>, void> {
 public:
  friend class TaskPromiseBase;

  using StorageType = void;

  TaskPromise() noexcept = default;

  void return_void() noexcept { this->result_.emplace(); }

  using TaskPromiseCrtpBase<TaskPromise<void>, void>::yield_value;

  template <typename ContainerRef>
  auto yield_value(co_result<Unit, ContainerRef> result) {
    std::move(result).assignTo(this->result_);
    return final_suspend();
  }
};

namespace adl {
// ADL should prefer your `friend co_withExecutor` over this dummy overload.
void co_withExecutor();
// This CPO deliberately does NOT use `tag_invoke`, but rather reuses the
// `co_withExecutor` name as the ADL implementation, just like `co_viaIfAsync`.
// The reason is that `tag_invoke()` would plumb through `Awaitable&&` instead
// of `Awaitable`, but `folly::ext::must_use_immediately_v` types require
// by-value.
struct WithExecutorFunction {
  template <typename Awaitable>
  // Pass `awaitable` by-value, since `&&` would break immediate types
  auto operator()(Executor::KeepAlive<> executor, Awaitable awaitable) const
      FOLLY_DETAIL_FORWARD_BODY(co_withExecutor(
          std::move(executor),
          // NOLINTNEXTLINE(facebook-folly-coro-temporary-by-ref)
          folly::ext::must_use_immediately_unsafe_mover(
              std::move(awaitable))()))
};
} // namespace adl

} // namespace detail

// Semi-awaitables like `Task` should use this CPO to attach executors:
//   auto taskWithExec = co_withExecutor(std::move(exec), std::move(task));
FOLLY_DEFINE_CPO(detail::adl::WithExecutorFunction, co_withExecutor)

/// Represents an allocated but not yet started coroutine that has already
/// been bound to an executor.
///
/// This task, when co_awaited, will launch the task on the bound executor
/// and will resume the awaiting coroutine on the bound executor when it
/// completes.
///
/// More information on how to use this is available at folly::coro::Task.
template <typename T>
class [[nodiscard]] TaskWithExecutor {
  using handle_t = coroutine_handle<detail::TaskPromise<T>>;
  using StorageType = typename detail::TaskPromise<T>::StorageType;

 public:
  /// @private
  ~TaskWithExecutor() {
    if (coro_) {
      coro_.destroy();
    }
  }

  TaskWithExecutor(TaskWithExecutor&& t) noexcept
      : coro_(std::exchange(t.coro_, {})) {}

  TaskWithExecutor& operator=(TaskWithExecutor t) noexcept {
    swap(t);
    return *this;
  }
  /// Returns the executor that the task is bound to
  folly::Executor* executor() const noexcept {
    return coro_.promise().executor_.get();
  }

  void swap(TaskWithExecutor& t) noexcept { std::swap(coro_, t.coro_); }

  /// Start eager execution of this task.
  ///
  /// This starts execution of the Task on the bound executor.
  /// @returns folly::SemiFuture<T> that will complete with the result.
  FOLLY_NOINLINE SemiFuture<lift_unit_t<StorageType>> start() && {
    folly::Promise<lift_unit_t<StorageType>> p;

    auto sf = p.getSemiFuture();

    std::move(*this).startImpl(
        [promise = std::move(p)](Try<StorageType>&& result) mutable {
          promise.setTry(std::move(result));
        },
        folly::CancellationToken{},
        FOLLY_ASYNC_STACK_RETURN_ADDRESS());

    return sf;
  }

  /// Start eager execution of the task and call the passed callback on
  /// completion
  ///
  /// This starts execution of the Task on the bound executor, and call the
  /// passed callback upon completion. The callback takes a Try<T> which
  /// represents either th value returned by the Task on success or an
  /// exception thrown by the Task
  /// @param tryCallback a function that takes in a Try<T>
  /// @param cancelToken a CancelationToken object
  template <typename F>
  FOLLY_NOINLINE void start(
      F&& tryCallback, folly::CancellationToken cancelToken = {}) && {
    std::move(*this).startImpl(
        static_cast<F&&>(tryCallback),
        std::move(cancelToken),
        FOLLY_ASYNC_STACK_RETURN_ADDRESS());
  }

  /// Start eager execution of this task on this thread.
  ///
  /// Assumes the current thread is already on the executor associated with the
  /// Task. Refer to TaskWithExecuter::start(F&& tryCallback,
  /// folly::CancellationToken cancelToken = {}) for more information.
  template <typename F>
  FOLLY_NOINLINE void startInlineUnsafe(
      F&& tryCallback, folly::CancellationToken cancelToken = {}) && {
    std::move(*this).startInlineImpl(
        static_cast<F&&>(tryCallback),
        std::move(cancelToken),
        FOLLY_ASYNC_STACK_RETURN_ADDRESS());
  }

  /// Start eager execution of this task on this thread.
  ///
  /// Assumes the current thread is already on the executor associated with the
  /// Task. Refer to TaskWithExecuter::start() for more information.
  FOLLY_NOINLINE SemiFuture<lift_unit_t<StorageType>> startInlineUnsafe() && {
    folly::Promise<lift_unit_t<StorageType>> p;

    auto sf = p.getSemiFuture();

    std::move(*this).startInlineImpl(
        [promise = std::move(p)](Try<StorageType>&& result) mutable {
          promise.setTry(std::move(result));
        },
        folly::CancellationToken{},
        FOLLY_ASYNC_STACK_RETURN_ADDRESS());

    return sf;
  }

 private:
  template <typename F>
  void startImpl(
      F&& tryCallback,
      folly::CancellationToken cancelToken,
      void* returnAddress) && {
    coro_.promise().setCancellationToken(std::move(cancelToken));
    startImpl(std::move(*this), static_cast<F&&>(tryCallback))
        .start(returnAddress);
  }

  template <typename F>
  void startInlineImpl(
      F&& tryCallback,
      folly::CancellationToken cancelToken,
      void* returnAddress) && {
    coro_.promise().setCancellationToken(std::move(cancelToken));
    // If the task replaces the request context and reaches a suspension point,
    // it will not have a chance to restore the previous context before we
    // return, so we need to ensure it is restored. This simulates starting the
    // coroutine in an actual executor, which would wrap the task with a guard.
    RequestContextSaverScopeGuard contextScope;
    startInlineImpl(std::move(*this), static_cast<F&&>(tryCallback))
        .start(returnAddress);
  }

  template <typename F>
  detail::InlineTaskDetached startImpl(TaskWithExecutor task, F cb) {
    try {
      cb(co_await folly::coro::co_awaitTry(std::move(task)));
    } catch (...) {
      cb(Try<StorageType>(exception_wrapper(current_exception())));
    }
  }

  template <typename F>
  detail::InlineTaskDetached startInlineImpl(TaskWithExecutor task, F cb) {
    try {
      cb(co_await InlineTryAwaitable{std::exchange(task.coro_, {})});
    } catch (...) {
      cb(Try<StorageType>(exception_wrapper(current_exception())));
    }
  }

 public:
  class Awaiter {
   public:
    explicit Awaiter(handle_t coro) noexcept : coro_(coro) {}

    Awaiter(Awaiter&& other) noexcept : coro_(std::exchange(other.coro_, {})) {}

    ~Awaiter() {
      if (coro_) {
        coro_.destroy();
      }
    }

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    FOLLY_NOINLINE void await_suspend(
        coroutine_handle<Promise> continuation) noexcept {
      DCHECK(coro_);
      auto& promise = coro_.promise();
      DCHECK(!promise.continuation_);
      DCHECK(promise.executor_);
      DCHECK(!dynamic_cast<folly::InlineExecutor*>(promise.executor_.get()))
          << "InlineExecutor is not safe and is not supported for coro::Task. "
          << "If you need to run a task inline in a unit-test, you should use "
          << "coro::blockingWait instead.";
      DCHECK(!dynamic_cast<folly::QueuedImmediateExecutor*>(
          promise.executor_.get()))
          << "QueuedImmediateExecutor is not safe and is not supported for coro::Task. "
          << "If you need to run a task inline in a unit-test, you should use "
          << "coro::blockingWait instead.";
      if constexpr (kIsDebug) {
        if (dynamic_cast<InlineLikeExecutor*>(promise.executor_.get())) {
          FB_LOG_ONCE(ERROR)
              << "InlineLikeExecutor is not safe and is not supported for coro::Task. "
              << "If you need to run a task inline in a unit-test, you should use "
              << "coro::blockingWait or write your test using the CO_TEST* macros instead."
              << "If you are using folly::getCPUExecutor, switch to getGlobalCPUExecutor "
              << "or be sure to call setCPUExecutor first.";
        }
        if (dynamic_cast<folly::DefaultKeepAliveExecutor::WeakRefExecutor*>(
                promise.executor_.get())) {
          FB_LOG_ONCE(ERROR)
              << "You are scheduling a coro::Task on a weak executor. "
              << "It is not supported, and can lead to memory leaks. "
              << "Consider using CancellationToken instead.";
        }
      }

      auto& calleeFrame = promise.getAsyncFrame();
      calleeFrame.setReturnAddress();

      if constexpr (detail::promiseHasAsyncFrame_v<Promise>) {
        auto& callerFrame = continuation.promise().getAsyncFrame();
        calleeFrame.setParentFrameUnsafe(callerFrame);
        folly::deactivateAsyncStackFrame(callerFrame);
      }

      promise.continuation_ = continuation;
      promise.executor_->add(
          [coro = coro_, ctx = RequestContext::saveContext()]() mutable {
            RequestContextScopeGuard contextScope{std::move(ctx)};
            folly::resumeCoroutineWithNewAsyncStackRoot(coro);
          });
    }

    T await_resume() {
      DCHECK(coro_);
      // Eagerly destroy the coroutine-frame once we have retrieved the result.
      SCOPE_EXIT {
        std::exchange(coro_, {}).destroy();
      };
      return std::move(coro_.promise().result()).value();
    }

    folly::Try<StorageType> await_resume_try() noexcept(
        std::is_nothrow_move_constructible_v<StorageType>) {
      SCOPE_EXIT {
        std::exchange(coro_, {}).destroy();
      };
      return std::move(coro_.promise().result());
    }

#if FOLLY_HAS_RESULT
    result<T> await_resume_result() noexcept(
        std::is_nothrow_move_constructible_v<StorageType>) {
      SCOPE_EXIT {
        std::exchange(coro_, {}).destroy();
      };
      return try_to_result(std::move(coro_.promise().result()));
    }
#endif

   private:
    handle_t coro_;
  };

  class InlineTryAwaitable {
   public:
    InlineTryAwaitable(handle_t coro) noexcept : coro_(coro) {}

    InlineTryAwaitable(InlineTryAwaitable&& other) noexcept
        : coro_(std::exchange(other.coro_, {})) {}

    ~InlineTryAwaitable() {
      if (coro_) {
        coro_.destroy();
      }
    }

    bool await_ready() noexcept { return false; }

    template <typename Promise>
    FOLLY_NOINLINE coroutine_handle<> await_suspend(
        coroutine_handle<Promise> continuation) noexcept {
      DCHECK(coro_);
      auto& promise = coro_.promise();
      DCHECK(!promise.continuation_);
      DCHECK(promise.executor_);

      promise.continuation_ = continuation;

      auto& calleeFrame = promise.getAsyncFrame();
      calleeFrame.setReturnAddress();

      // This awaitable is only ever awaited from a DetachedInlineTask
      // which is an async-stack-aware coroutine.
      //
      // Assume it has a .getAsyncFrame() and that this frame is currently
      // active.
      auto& callerFrame = continuation.promise().getAsyncFrame();
      folly::pushAsyncStackFrameCallerCallee(callerFrame, calleeFrame);
      return coro_;
    }

    folly::Try<StorageType> await_resume() {
      DCHECK(coro_);
      // Eagerly destroy the coroutine-frame once we have retrieved the result.
      SCOPE_EXIT {
        std::exchange(coro_, {}).destroy();
      };
      return std::move(coro_.promise().result());
    }

   private:
    friend InlineTryAwaitable tag_invoke(
        cpo_t<co_withAsyncStack> /*unused*/,
        InlineTryAwaitable&& awaitable) noexcept {
      return std::move(awaitable);
    }

    handle_t coro_;
  };

 public:
  Awaiter operator co_await() && noexcept {
    DCHECK(coro_);
    return Awaiter{std::exchange(coro_, {})};
  }

  std::pair<Task<T>, Executor::KeepAlive<>> unwrap() && {
    auto executor = std::move(coro_.promise().executor_);
    Task<T> task{std::exchange(coro_, {})};
    return {std::move(task), std::move(executor)};
  }

  friend ViaIfAsyncAwaitable<TaskWithExecutor> co_viaIfAsync(
      Executor::KeepAlive<> executor,
      TaskWithExecutor&& taskWithExecutor) noexcept {
    auto [task, taskExecutor] = std::move(taskWithExecutor).unwrap();
    return ViaIfAsyncAwaitable<TaskWithExecutor>(
        std::move(executor),
        co_withExecutor(std::move(taskExecutor), [](Task<T> t) -> Task<T> {
          co_yield co_result(co_await co_awaitTry(std::move(t)));
        }(std::move(task))));
  }

  friend TaskWithExecutor co_withCancellation(
      folly::CancellationToken cancelToken, TaskWithExecutor&& task) noexcept {
    DCHECK(task.coro_);
    task.coro_.promise().setCancellationToken(std::move(cancelToken));
    return std::move(task);
  }

  friend TaskWithExecutor tag_invoke(
      cpo_t<co_withAsyncStack> /*unused*/, TaskWithExecutor&& task) noexcept {
    return std::move(task);
  }

  using folly_private_task_without_executor_t = Task<T>;
  // See comment in `Task`, or use `safe_task_with_executor` instead.
  template <safe_alias>
  using folly_private_safe_alias_t = safe_alias_constant<safe_alias::unsafe>;

 private:
  friend class Task<T>;

  explicit TaskWithExecutor(handle_t coro) noexcept : coro_(coro) {}

  handle_t coro_;
};

// This macro makes it easier for `TaskWrapper.h` users to apply the correct
// attributes for the wrapped `Task`s.
#define FOLLY_CORO_TASK_ATTRS \
  [[nodiscard]] [[FOLLY_ATTR_CLANG_CORO_AWAIT_ELIDABLE]]

/// Represents an allocated, but not-started coroutine, which is not yet
/// been bound to an executor.
///
/// You can only co_await a Task from within another Task, in which case it
/// is implicitly bound to the same executor as the parent Task.
///
/// Alternatively, you can explicitly provide an executor by calling
/// `co_withExecutor(executor, task())`, which will return a not-yet-started
/// `TaskWithExecutor` that can be `co_await`ed anywhere and that will
/// automatically schedule the coroutine to start executing on the bound
/// executor when it is `co_await`ed.
///
/// Within the body of a Task's coroutine, executor binding to the parent
/// executor is maintained by implicitly transforming all 'co_await expr'
/// expressions into `co_await co_viaIfAsync(parentExecutor, expr)' to ensure
/// that the coroutine always resumes on the parent's executor.
///
/// The Task coroutine is RequestContext-aware
/// and will capture the current RequestContext at the time the coroutine
/// function is either awaited or explicitly started and will save/restore the
/// current RequestContext whenever the coroutine suspends and resumes at a
/// co_await expression.
///
/// Prefer `now_task` (`folly/coro/safe/NowTask.h`) for new coroutines.  A
/// `now_task` is immovable and must be `co_await`ed in the expression that
/// creates it, so C++ lifetime extension keeps by-reference arguments and
/// lambda captures alive until it completes.  This rules out the
/// dangling-reference bugs that `Task` allows.
///
/// Caveat: code that holds a task before awaiting it -- collect-style
/// algorithms such as `collectAll` and `collectAllRange`, `AsyncScope::add`,
/// or storing tasks in a container -- needs a movable `Task`.  Such callers can
/// wrap a `now_task` function in a `Task` coroutine that takes its arguments
/// by value, e.g. `Task<T> fTask(Arg arg) { co_return co_await f(arg); }`,
/// so that the wrapper owns everything the call refers to.
///
/// More documentation on how to use coroutines is available at
/// https://github.com/facebook/folly/blob/main/folly/coro/README.md
///
/// @refcode folly/docs/examples/folly/coro/Task.cpp
template <typename T>
class FOLLY_CORO_TASK_ATTRS Task {
 public:
  using promise_type = detail::TaskPromise<T>;
  using StorageType = typename promise_type::StorageType;

 private:
  class Awaiter;
  using handle_t = coroutine_handle<promise_type>;

  void setExecutor(folly::Executor::KeepAlive<>&& e) noexcept {
    DCHECK(coro_);
    DCHECK(e);
    coro_.promise().executor_ = std::move(e);
  }

  // `co_withExecutor` implementation detail -- this works around the fact that
  // not all compilers consider the hidden friend `co_withExecutor` to be a
  // friend of `TaskWithExecutor`, and I found no uniform way to add the
  // friendship without making it non-hidden.  Try folding back into
  // `co_withExecutor` in 2027 or so, to see if the old compiler issue is gone.
  TaskWithExecutor<T> asTaskWithExecutor() && {
    return TaskWithExecutor<T>{std::exchange(coro_, {})};
  }

 public:
  Task(const Task& t) = delete;

  /// Create a Task, invalidating the original Task in the process.
  Task(Task&& t) noexcept : coro_(std::exchange(t.coro_, {})) {}

  /// @private
  ~Task() {
    if (coro_) {
      coro_.destroy();
    }
  }

  Task& operator=(Task t) noexcept {
    swap(t);
    return *this;
  }

  void swap(Task& t) noexcept { std::swap(coro_, t.coro_); }

  /// Specify the executor that this task should execute on:
  ///   co_withExecutor(executor, std::move(task))
  //
  /// @param executor An Executor::KeepAlive object, which can be implicitly
  /// constructed from Executor*
  /// @returns a new TaskWithExecutor object, which represents the existing Task
  /// bound to an executor
  friend TaskWithExecutor<T> co_withExecutor(
      Executor::KeepAlive<> executor, Task task) noexcept {
    task.setExecutor(std::move(executor));
    DCHECK(task.coro_);
    return std::move(task).asTaskWithExecutor();
  }

  /// Converts a Task into a SemiFuture object.
  ///
  /// The SemiFuture object is implicitly of type Semifuture<Try<T>>, where the
  /// Try represents whether the execution of the converted Task succeeded and T
  /// is the original task's result type.
  /// @returns a SemiFuture object
  FOLLY_NOINLINE
  SemiFuture<folly::lift_unit_t<StorageType>> semi() && {
    return makeSemiFuture().deferExTry(
        [task = std::move(*this),
         returnAddress = FOLLY_ASYNC_STACK_RETURN_ADDRESS()](
            const Executor::KeepAlive<>& executor, Try<Unit>&&) mutable {
          folly::Promise<lift_unit_t<StorageType>> p;

          auto sf = p.getSemiFuture();

          co_withExecutor(executor, std::move(task))
              .startInlineImpl(
                  [promise = std::move(p)](Try<StorageType>&& result) mutable {
                    promise.setTry(std::move(result));
                  },
                  folly::CancellationToken{},
                  returnAddress);

          return sf;
        });
  }

  friend auto co_viaIfAsync(
      Executor::KeepAlive<> executor, Task<T>&& t) noexcept {
    DCHECK(t.coro_);
    // Child task inherits the awaiting task's executor
    t.setExecutor(std::move(executor));
    return Awaiter{std::exchange(t.coro_, {})};
  }

  friend Task co_withCancellation(
      folly::CancellationToken cancelToken, Task&& task) noexcept {
    DCHECK(task.coro_);
    task.coro_.promise().setCancellationToken(std::move(cancelToken));
    return std::move(task);
  }

  template <typename F, typename... A, typename F_, typename... A_>
  friend Task tag_invoke(
      tag_t<co_invoke_fn> /*unused*/,
      tag_t<Task, F, A...> /*unused*/,
      F_ f,
      A_... a) {
    co_yield co_result(
        co_await co_awaitTry(
            std::invoke(static_cast<F&&>(f), static_cast<A&&>(a)...)));
  }

  using PrivateAwaiterTypeForTests = Awaiter;
  // Use `safe_task` instead of `Task` to move tasks into other safe coro APIs.
  //
  // User-facing stuff from `Task.h` can trivially include unsafe aliasing, the
  // `folly::coro` docs include hundreds of words of pitfalls.  The intent here
  // is to catch people accidentally passing `Task`s into safer primitives, and
  // breaking their memory-safety guarantees.
  template <safe_alias>
  using folly_private_safe_alias_t = safe_alias_constant<safe_alias::unsafe>;

 private:
  friend class detail::TaskPromiseBase;
  friend class detail::TaskPromiseCrtpBase<detail::TaskPromise<T>, T>;
  friend class TaskWithExecutor<T>;

  class Awaiter {
   public:
    explicit Awaiter(handle_t coro) noexcept : coro_(coro) {}

    Awaiter(Awaiter&& other) noexcept : coro_(std::exchange(other.coro_, {})) {}

    Awaiter(const Awaiter&) = delete;

    ~Awaiter() {
      if (coro_) {
        coro_.destroy();
      }
    }

    bool await_ready() noexcept { return false; }

    template <typename Promise>
    FOLLY_NOINLINE auto await_suspend(
        coroutine_handle<Promise> continuation) noexcept {
      DCHECK(coro_);
      auto& promise = coro_.promise();

      promise.continuation_ = continuation;

      auto& calleeFrame = promise.getAsyncFrame();
      calleeFrame.setReturnAddress();

      if constexpr (detail::promiseHasAsyncFrame_v<Promise>) {
        auto& callerFrame = continuation.promise().getAsyncFrame();
        folly::pushAsyncStackFrameCallerCallee(callerFrame, calleeFrame);
        return coro_;
      } else {
        folly::resumeCoroutineWithNewAsyncStackRoot(coro_);
        return;
      }
    }

    T await_resume() {
      DCHECK(coro_);
      SCOPE_EXIT {
        std::exchange(coro_, {}).destroy();
      };
      return std::move(coro_.promise().result()).value();
    }

    folly::Try<StorageType> await_resume_try() noexcept(
        std::is_nothrow_move_constructible_v<StorageType>) {
      DCHECK(coro_);
      SCOPE_EXIT {
        std::exchange(coro_, {}).destroy();
      };
      return std::move(coro_.promise().result());
    }

#if FOLLY_HAS_RESULT
    result<T> await_resume_result() noexcept(
        std::is_nothrow_move_constructible_v<StorageType>) {
      DCHECK(coro_);
      SCOPE_EXIT {
        std::exchange(coro_, {}).destroy();
      };
      return try_to_result(std::move(coro_.promise().result()));
    }
#endif

   private:
    // This overload needed as Awaiter is returned from co_viaIfAsync() which is
    // then passed into co_withAsyncStack().
    friend Awaiter tag_invoke(
        cpo_t<co_withAsyncStack> /*unused*/, Awaiter&& awaiter) noexcept {
      return std::move(awaiter);
    }

    handle_t coro_;
  };

  Task(handle_t coro) noexcept : coro_(coro) {}

  handle_t coro_;
};

/// Make a task that trivially returns a value.
/// @param t value to be returned by the Task
template <class T>
Task<T> makeTask(T t) {
  co_return t;
}

/// Make a Task that trivially returns with no return value.
inline Task<void> makeTask() {
  co_return;
}
/// Same as makeTask(). See Unit
inline Task<void> makeTask(Unit /*unused*/) {
  co_return;
}

/// Make a Task that will trivially yield an Exception.
/// @param ew an exception_wrapper object
template <class T>
Task<T> makeErrorTask(exception_wrapper ew) {
  co_yield co_error(std::move(ew));
}

/// Make a Task out of a Try.
/// @tparam T the type of the value wrapped by the Try
/// @param t the Try to convert into a Task
/// @returns a Task that will yield the Try's value or exception.
template <class T>
Task<drop_unit_t<T>> makeResultTask(Try<T> t) {
  co_yield co_result(std::move(t));
}

template <typename Promise, typename T>
inline Task<T>
detail::TaskPromiseCrtpBase<Promise, T>::get_return_object() noexcept {
  // Watch out: When used with `TaskWrapper`, this relies on "practically safe"
  // UB wherein this handle is only valid because `TaskPromise` and the true
  // "wrapper promise" of the wrapper coro coincide in layout exactly.
  // Documented in `TaskPromiseWrapperBase::is_promise_type_punning_safe`.
  return Task<T>{
      coroutine_handle<Promise>::from_promise(*static_cast<Promise*>(this))};
}

} // namespace folly::coro

#endif // FOLLY_HAS_COROUTINES
