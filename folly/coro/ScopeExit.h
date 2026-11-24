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
// Docs: https://fburl.com/fbcref_coro_scopeexit
//

#pragma once

#include <folly/tracing/AsyncStack.h>

#include <folly/ExceptionWrapper.h>
#include <folly/Executor.h>
#include <folly/ScopeGuard.h>
#include <folly/coro/Coroutine.h>
#include <folly/coro/Traits.h>
#include <folly/coro/ViaIfAsync.h>
#include <folly/coro/detail/Malloc.h>
#include <folly/functional/Invoke.h>
#include <folly/lang/Assume.h>
#include <folly/lang/CustomizationPoint.h>

#if FOLLY_HAS_COROUTINES

namespace folly {
namespace coro {
namespace detail {
struct AttachScopeExitFn {
  /// Dispatches to a custom implementation using tag_invoke()
  template <typename ParentPromise, typename ChildPromise>
    requires folly::is_tag_invocable_v<
        AttachScopeExitFn,
        ParentPromise&,
        coroutine_handle<ChildPromise>>
  auto operator()(
      ParentPromise& parent, coroutine_handle<ChildPromise> action) const
      noexcept(folly::is_nothrow_tag_invocable_v<
               AttachScopeExitFn,
               ParentPromise&,
               coroutine_handle<ChildPromise>>)
          -> folly::tag_invoke_result_t<
              AttachScopeExitFn,
              ParentPromise&,
              coroutine_handle<ChildPromise>> {
    return folly::tag_invoke(AttachScopeExitFn{}, parent, action);
  }
};

/// co_attachScopeExit extension point opts the parent coroutine type into
/// handling ScopeExitTasks and executing them at the end of the parent
/// coroutine's scope.
///
/// There are two important steps the parent coroutine must take:
/// 1. It must store the provided ScopeExitTask coroutine handle and return the
/// latest previously attached ScopeExitTask handle (or an empty handle if this
/// one is the first).
/// 2. On destruction of the parent coroutine, the context of the latest stored
/// ScopeExitTask coroutine must be set by calling setContext(...) on its
/// promise object, then the ScopeExitTask coroutine must be executed.
/// The continuation passed to the setContext(...) call will be resumed after
/// the executing the last (the first attached) coroutine in the ScopeExitTask
/// chain. NOTE: The user must not pop the async frame if it is passed to the
/// setContext(...) call, it will be popped by the last ScopeExitTask coroutine
/// in the chain instead.
FOLLY_DEFINE_CPO(AttachScopeExitFn, co_attachScopeExit)

template <typename... Args>
class ScopeExitTask;

class ScopeExitTaskPromiseBase {
 public:
  FOLLY_CORO_DEFINE_FRAME_ALLOCATOR_IF_SUPPORTED(
      ::folly::coro::detail::CoroFrameAsyncAllocator)

  class FinalAwaiter {
   public:
    bool await_ready() noexcept { return false; }

    template <typename Promise>
    FOLLY_CORO_AWAIT_SUSPEND_NONTRIVIAL_ATTRIBUTES coroutine_handle<>
    await_suspend(coroutine_handle<Promise> coro) noexcept {
      SCOPE_EXIT {
        coro.destroy();
      };

      ScopeExitTaskPromiseBase& promise = coro.promise();
      DCHECK(promise.continuation_);
      DCHECK(promise.parentAsyncFrame_);
      DCHECK(promise.executor_);
      if (promise.next_) {
        promise.next_.promise().setContext(
            promise.continuation_,
            promise.parentAsyncFrame_,
            promise.executor_.get_alias(),
            std::move(promise.error_));
        return promise.next_;
      }

      /// If we reached this point, then this ScopeExitTask is the final one to
      /// be executed on the parent task, and we can now pop the parent's async
      /// frame before calling the original parent's continuation.
      folly::popAsyncStackFrameCallee(*promise.parentAsyncFrame_);
      if (promise.error_) {
        auto [handle, frame] =
            promise.continuation_.getErrorHandle(promise.error_);
        return handle.getHandle();
      }
      return promise.continuation_.getHandle();
    }

    [[noreturn]] void await_resume() noexcept { folly::assume_unreachable(); }
  };

  void setContext(
      ExtendedCoroutineHandle continuation,
      folly::AsyncStackFrame* asyncFrame,
      folly::Executor::KeepAlive<> executor,
      folly::exception_wrapper error = {}) {
    continuation_ = continuation;
    parentAsyncFrame_ = asyncFrame;
    executor_ = std::move(executor);
    error_ = std::move(error);
  }

  suspend_always initial_suspend() noexcept { return {}; }

  FinalAwaiter final_suspend() noexcept { return {}; }

  template <typename Awaitable>
  auto await_transform(Awaitable&& awaitable) {
    return folly::coro::co_withAsyncStack(
        folly::coro::co_viaIfAsync(
            executor_.get_alias(), static_cast<Awaitable&&>(awaitable)));
  }

  folly::AsyncStackFrame& getAsyncFrame() noexcept {
    return *parentAsyncFrame_;
  }

  [[noreturn]] void unhandled_exception() noexcept {
    /// Since ScopeExitTasks execute after the parent coroutine has completed,
    /// we are unable to propagate exceptions back to the caller. Similar to
    /// throwing another exception while unwinding an exception, we opt to
    /// terminate here by throwing within a noexcept frame.
    rethrow_current_exception();
  }

  void return_void() noexcept {}

 protected:
  template <typename... Args>
  friend class ScopeExitTask;

  ExtendedCoroutineHandle continuation_;
  folly::AsyncStackFrame* parentAsyncFrame_;
  folly::Executor::KeepAlive<> executor_;
  folly::exception_wrapper error_;
  coroutine_handle<ScopeExitTaskPromiseBase> next_;
};

template <typename... Args>
class ScopeExitTaskPromise : public ScopeExitTaskPromiseBase {
 public:
  template <typename Action>
  explicit ScopeExitTaskPromise(Action&&, Args&... args) noexcept
      : args_(args...) {}

  ScopeExitTask<Args...> get_return_object() noexcept;

 private:
  friend class ScopeExitTask<Args...>;

  std::tuple<Args&...> args_;
};

template <typename... Args>
class [[nodiscard]] ScopeExitTask {
 public:
  using promise_type = ScopeExitTaskPromise<Args...>;

 private:
  class Awaiter;
  using handle_t = coroutine_handle<promise_type>;

 public:
  explicit ScopeExitTask(handle_t coro) noexcept : coro_(coro) {}

  ~ScopeExitTask() {
    /// Failing to await this Task is likely a bug
    DCHECK(!coro_);
  }

  ScopeExitTask(ScopeExitTask&& t) noexcept
      : coro_(std::exchange(t.coro_, {})) {}

  friend auto co_viaIfAsync(Executor::KeepAlive<>, ScopeExitTask&& t) noexcept {
    DCHECK(t.coro_);
    return Awaiter{std::exchange(t.coro_, {})};
  }

  /// We explicitly do not handle co_withCancellation, as these tasks are
  /// designed to always run at the end of their parent coroutine.

 private:
  class Awaiter {
   public:
    explicit Awaiter(handle_t coro) noexcept : coro_(coro) {}

    Awaiter(Awaiter&& other) noexcept : coro_(std::exchange(other.coro_, {})) {}

    Awaiter(const Awaiter&) = delete;

    ~Awaiter() {
      /// The coro will destroy itself in the FinalAwaiter, before continuing
      /// the next continuation
      DCHECK(!coro_);
    }

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    bool await_suspend(coroutine_handle<Promise> parent) noexcept {
      auto& promise = coro_.promise();
      auto& parentPromise = parent.promise();

      /// Calling co_attachScopeExit here inserts the ScopeExit coroutine handle
      /// as the parent's continuation, and sets the ScopeExit's continuation as
      /// the parents.
      ///
      /// Before:
      /// Parent FinalAwaiter -> Parent's continuation
      ///
      /// After one scope exit:
      /// Parent FinalAwaiter -> ScopeExit1 -> Parent's Continuation
      /// After two scope exits:
      /// Parent FinalAwaiter -> ScopeExit2 -> ScopeExit1 -> Parent's
      /// continuation
      ///
      /// This ensures that the scope exit coroutines are executed in reverse
      /// order to when they were attached in the parent.
      ///
      /// Since each ScopeExitTask runs as a continuation at the end of the
      /// parent coroutine's scope without popping the async stack to the
      /// caller, we must run within the parent's async frame. In order to
      /// guarantee correctness, the parent must defer responsibility of popping
      /// the async stack frame to the final scope exit continuation.
      promise.next_ = co_attachScopeExit(
          parentPromise,
          coroutine_handle<ScopeExitTaskPromiseBase>::from_promise(
              coro_.promise()));

      return false;
    }

    std::tuple<Args&...> await_resume() noexcept {
      /// The coro will destroy itself in the FinalAwaiter
      handle_t coro = std::exchange(coro_, {});
      return std::move(coro.promise().args_);
    }

   private:
    friend Awaiter tag_invoke(cpo_t<co_withAsyncStack>, Awaiter&& t) noexcept {
      return std::move(t);
    }

    handle_t coro_;
  };

  handle_t coro_;
};

template <typename... Args>
inline ScopeExitTask<Args...>
ScopeExitTaskPromise<Args...>::get_return_object() noexcept {
  return ScopeExitTask<Args...>{
      coroutine_handle<ScopeExitTaskPromise>::from_promise(*this)};
}

} // namespace detail

class co_scope_exit_fn {
  /// Use a static helper as we do not wish to pass the implicit `this` pointer
  /// to the promise constructor
  ///
  /// TODO: It's not mandatory to elide copy/move of args into the coroutine
  /// frame today, which makes using some types, like AsyncScope, annoying. For
  /// non-copyable, non-moveable types, you must wrap the type in a
  /// std::unique_ptr.
  ///
  /// We might be able to work around this by storing the arguments in the
  /// promise type, rather than on the coroutine frame.
  template <typename Action, typename... Args>
  static detail::ScopeExitTask<Args...> coScopeExitImpl(
      Action action, Args... args) {
    co_await std::move(action)(std::move(args)...);
  }

 public:
  template <typename Action, typename... Args>
  detail::ScopeExitTask<std::decay_t<Args>...> operator()(
      Action&& action, Args&&... args) const {
    return coScopeExitImpl(
        static_cast<Action&&>(action), static_cast<Args&&>(args)...);
  }
};

/// co_scope_exit is a utility function that allows you to associate
/// continuations which execute at the end of the coroutine, just before
/// resuming the caller.
///
/// The first argument is a Task-returning callable. The subsequent arguments
/// are optional state that can be used within the exit coroutine. The cleanup
/// action will assume ownership of the provided state by copying the state
/// inside the exit coroutine.
///
/// If you need access to the state in both the parent coroutine *and* in the
/// exit coroutine, you can receive l-values to the captured state as return
/// values. See the example below.
///
/// If you attach multiple co_scope_exit coroutines, they will be executed in
/// reverse order to the order in which they were registered.
///
/// CAUTION: The body of the co_scope_exit coroutine runs *after* the parent
/// coroutine has already been destroyed. This means that any local variables in
/// the coroutine body will no longer be accessible. Do not capture references
/// to any locals in the exit coroutine, or else you will hit undefined
/// behavior. Any state you wish to pass to the scope exit coroutine should be
/// passed as an argument to co_scope_exit.
///
/// Example:
/// folly::coro::Task<> doSomethingComplicated(std::vector<int> inputs) {
///   auto&& [scope] = co_await folly::coro::co_scope_exit(
///       [](auto scope) -> folly::coro::Task<> {
///         co_await scope.joinAsync();
///       }, std::make_unique<AsyncScope>());
///
///   // Do some complicated, potentially throwing work using the AsyncScope
///   auto ex = co_await co_current_executor;
///   asyncScope->add(co_withExecutor(ex, someTask(std::move(inputs))));
/// }
///
/// The body of the coroutine passed to co_scope_exit will be executed when the
/// parent task completes, either when the parent completes with a result, or
/// due to an unhandled exception.
inline constexpr co_scope_exit_fn co_scope_exit{};

} // namespace coro
} // namespace folly

#endif // FOLLY_HAS_COROUTINES
