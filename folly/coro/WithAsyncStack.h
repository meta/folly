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

#include <folly/coro/Coroutine.h>
#include <folly/coro/Traits.h>
#include <folly/coro/detail/Malloc.h>
#include <folly/functional/Invoke.h>
#include <folly/lang/Assume.h>
#include <folly/lang/CustomizationPoint.h>
#include <folly/tracing/AsyncStack.h>

#include <cassert>
#include <type_traits>
#include <utility>

#if FOLLY_HAS_COROUTINES

namespace folly::coro {

namespace detail {

class WithAsyncStackCoroutine {
 public:
  class promise_type {
   public:
    FOLLY_CORO_DEFINE_FRAME_ALLOCATOR_IF_SUPPORTED(
        ::folly::coro::detail::CoroFrameAsyncAllocator)

    WithAsyncStackCoroutine get_return_object() noexcept {
      return WithAsyncStackCoroutine{
          coroutine_handle<promise_type>::from_promise(*this)};
    }

    suspend_always initial_suspend() noexcept { return {}; }

    struct FinalAwaiter {
      bool await_ready() noexcept { return false; }
      void await_suspend(coroutine_handle<promise_type> h) noexcept {
        auto& promise = h.promise();
        folly::deactivateSuspendedLeaf(promise.getLeafFrame());
        folly::resumeCoroutineWithNewAsyncStackRoot(
            promise.continuation_,
            *promise.getLeafFrame().getParentFrameUnsafe());
      }

      [[noreturn]] void await_resume() noexcept { folly::assume_unreachable(); }
    };

    FinalAwaiter final_suspend() noexcept { return {}; }

    void return_void() noexcept {}

    [[noreturn]] void unhandled_exception() noexcept {
      folly::assume_unreachable();
    }

    folly::AsyncStackFrame& getLeafFrame() noexcept { return leafFrame; }

   private:
    friend WithAsyncStackCoroutine;

    coroutine_handle<> continuation_;
    folly::AsyncStackFrame leafFrame;
  };

  WithAsyncStackCoroutine() noexcept : coro_() {}

  WithAsyncStackCoroutine(WithAsyncStackCoroutine&& other) noexcept
      : coro_(std::exchange(other.coro_, {})) {}

  ~WithAsyncStackCoroutine() {
    if (coro_) {
      coro_.destroy();
    }
  }

  WithAsyncStackCoroutine& operator=(WithAsyncStackCoroutine other) noexcept {
    std::swap(coro_, other.coro_);
    return *this;
  }

  static WithAsyncStackCoroutine create() { co_return; }

  template <typename Promise>
  coroutine_handle<promise_type> getWrapperHandleFor(
      coroutine_handle<Promise> h, void* returnAddress) noexcept {
    auto& promise = coro_.promise();
    promise.continuation_ = h;
    promise.getLeafFrame().setParentFrameUnsafe(h.promise().getAsyncFrame());
    promise.getLeafFrame().setReturnAddress(returnAddress);
    return coro_;
  }

  folly::AsyncStackFrame& getLeafFrame() noexcept {
    return coro_.promise().getLeafFrame();
  }

 private:
  explicit WithAsyncStackCoroutine(coroutine_handle<promise_type> h) noexcept
      : coro_(h) {}

  coroutine_handle<promise_type> coro_;
};

template <typename Awaitable>
class WithAsyncStackAwaiter {
  using Awaiter = awaiter_type_t<Awaitable>;

  static constexpr bool kSuspendIsNoexcept =
      noexcept(FOLLY_DECLVAL(Awaiter&).await_suspend(FOLLY_DECLVAL(
          coroutine_handle<WithAsyncStackCoroutine::promise_type>)));

 public:
  explicit WithAsyncStackAwaiter(Awaitable&& awaitable)
      : awaiter_(get_awaiter(static_cast<Awaitable&&>(awaitable))),
        coroWrapper_(WithAsyncStackCoroutine::create()) {}

  auto await_ready() noexcept(noexcept(std::declval<Awaiter&>().await_ready()))
      -> decltype(std::declval<Awaiter&>().await_ready()) {
    return awaiter_.await_ready();
  }

  // needs to be no-inline as return address is being captured for async stack
  // tracing
  template <typename Promise>
  FOLLY_NOINLINE auto await_suspend(coroutine_handle<Promise> h) noexcept(
      kSuspendIsNoexcept) {
    AsyncStackFrame& callerFrame = h.promise().getAsyncFrame();
    AsyncStackRoot* stackRoot = callerFrame.getStackRoot();
    assert(stackRoot != nullptr);

    auto wrapperHandle =
        coroWrapper_.getWrapperHandleFor(h, FOLLY_ASYNC_STACK_RETURN_ADDRESS());

    folly::deactivateAsyncStackFrame(callerFrame);
    folly::activateSuspendedLeaf(coroWrapper_.getLeafFrame());

    using await_suspend_result_t =
        decltype(awaiter_.await_suspend(wrapperHandle));

    auto doSuspend = [&]() {
      if constexpr (std::is_same_v<await_suspend_result_t, bool>) {
        if (!awaiter_.await_suspend(wrapperHandle)) {
          folly::activateAsyncStackFrame(*stackRoot, callerFrame);
          folly::deactivateSuspendedLeaf(coroWrapper_.getLeafFrame());
          return false;
        }
        return true;
      } else {
        return awaiter_.await_suspend(wrapperHandle);
      }
    };

    if constexpr (kSuspendIsNoexcept) {
      return doSuspend();
    } else {
      // Restore async stack state on exception, then rethrow.
      try {
        return doSuspend();
      } catch (...) {
        folly::activateAsyncStackFrame(*stackRoot, callerFrame);
        folly::deactivateSuspendedLeaf(coroWrapper_.getLeafFrame());
        throw;
      }
    }
  }

  auto await_resume() noexcept(
      noexcept(std::declval<Awaiter&>().await_resume()))
      -> decltype(std::declval<Awaiter&>().await_resume()) {
    coroWrapper_ = WithAsyncStackCoroutine();
    return awaiter_.await_resume();
  }

  template <typename Awaiter2 = Awaiter>
  auto await_resume_try() noexcept(
      noexcept(std::declval<Awaiter2&>().await_resume_try()))
      -> decltype(std::declval<Awaiter2&>().await_resume_try()) {
    coroWrapper_ = WithAsyncStackCoroutine();
    return awaiter_.await_resume_try();
  }

#if FOLLY_HAS_RESULT
  template <typename Awaiter2 = Awaiter>
  auto await_resume_result() noexcept(
      noexcept(FOLLY_DECLVAL(Awaiter2&).await_resume_result()))
      -> decltype(FOLLY_DECLVAL(Awaiter2&).await_resume_result()) {
    coroWrapper_ = WithAsyncStackCoroutine();
    return awaiter_.await_resume_result();
  }
#endif

 private:
  awaiter_type_t<Awaitable> awaiter_;
  WithAsyncStackCoroutine coroWrapper_;
};

template <typename Awaitable>
class WithAsyncStackAwaitable {
 public:
  explicit WithAsyncStackAwaitable(Awaitable&& awaitable)
      : awaitable_(static_cast<Awaitable&&>(awaitable)) {}

  WithAsyncStackAwaiter<Awaitable&> operator co_await() & {
    return WithAsyncStackAwaiter<Awaitable&>{awaitable_};
  }

  WithAsyncStackAwaiter<Awaitable> operator co_await() && {
    return WithAsyncStackAwaiter<Awaitable>{
        static_cast<Awaitable&&>(awaitable_)};
  }

 private:
  Awaitable awaitable_;
};

struct WithAsyncStackFunction {
  // Dispatches to a custom implementation using tag_invoke()
  template <typename Awaitable>
    requires folly::is_tag_invocable_v<WithAsyncStackFunction, Awaitable>
  auto operator()(Awaitable&& awaitable) const noexcept(
      folly::is_nothrow_tag_invocable_v<WithAsyncStackFunction, Awaitable>)
      -> folly::tag_invoke_result_t<WithAsyncStackFunction, Awaitable> {
    return folly::tag_invoke(
        WithAsyncStackFunction{}, static_cast<Awaitable&&>(awaitable));
  }

  // Fallback implementation. Wraps the awaitable in the
  // WithAsyncStackAwaitable which just saves/restores the
  // awaiting coroutine's AsyncStackFrame.
  template <typename Awaitable>
    requires(
        !folly::is_tag_invocable_v<WithAsyncStackFunction, Awaitable> &&
        folly::coro::is_awaitable_v<Awaitable>)
  WithAsyncStackAwaitable<Awaitable> operator()(Awaitable&& awaitable) const
      noexcept(std::is_nothrow_move_constructible_v<Awaitable>) {
    return WithAsyncStackAwaitable<Awaitable>{
        static_cast<Awaitable&&>(awaitable)};
  }
};

} // namespace detail

template <typename Awaitable>
inline constexpr bool is_awaitable_async_stack_aware_v =
    folly::is_tag_invocable_v<detail::WithAsyncStackFunction, Awaitable>;

// Coroutines that support the AsyncStack protocol will apply the
// co_withAsyncStack() customisation-point to an awaitable inside its
// await_transform() to ensure that the current coroutine's AsyncStackFrame
// is saved and later restored when the coroutine resumes.
//
// The default implementation is used for awaitables that don't know
// about the AsyncStackFrame and just wraps the awaitable to ensure
// that the stack-frame is saved/restored if the coroutine suspends.
//
// Awaitables that know about the AsyncStackFrame protocol can customise
// this CPO by defining an overload of tag_invoke() for this CPO
// for their type.
//
// For example:
//   class MyAwaitable {
//     friend MyAwaitable&& tag_invoke(
//         cpo_t<folly::coro::co_withAsyncStack>, MyAwaitable&& awaitable) {
//       return std::move(awaitable);
//     }
//
//     ...
//   };
//
// If you customise this CPO then it is your responsibility to ensure that
// if the awaiting coroutine suspends then before the coroutine is resumed
// that its original AsyncStackFrame is activated on the current thread.
// e.g. using folly::activateAsyncStackFrame()
//
// The awaiting coroutine's AsyncStackFrame can be obtained from its
// promise, which is assumed to have a 'AsyncStackFrame& getAsyncFrame()'
// method that returns a reference to the parent coroutine's async frame.

FOLLY_DEFINE_CPO(detail::WithAsyncStackFunction, co_withAsyncStack)

} // namespace folly::coro

#endif // FOLLY_HAS_COROUTINES
