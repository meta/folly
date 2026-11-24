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

#include <folly/ScopeGuard.h>
#include <folly/Try.h>
#include <folly/coro/Coroutine.h>
#include <folly/coro/WithAsyncStack.h>
#include <folly/coro/detail/Malloc.h>
#include <folly/lang/Assume.h>
#include <folly/tracing/AsyncStack.h>

#include <cassert>
#include <utility>

#if FOLLY_HAS_COROUTINES

namespace folly {
namespace coro {
namespace detail {

/// InlineTask<T> is a coroutine-return type where the coroutine is launched
/// inline in the current execution context when it is co_awaited and the
/// task's continuation is launched inline in the execution context that the
/// task completed on.
///
/// This task type is primarily intended as a building block for certain
/// coroutine operators. It is not intended for general use in application
/// code or in library interfaces exposed to library code as it can easily be
/// abused to accidentally run logic on the wrong execution context.
///
/// For this reason, the InlineTask<T> type has been placed inside the
/// folly::coro::detail namespace to discourage general usage.
template <typename T>
class InlineTask;

class InlineTaskPromiseBase {
  struct FinalAwaiter {
    bool await_ready() noexcept { return false; }

    template <typename Promise>
    coroutine_handle<> await_suspend(coroutine_handle<Promise> h) noexcept {
      InlineTaskPromiseBase& promise = h.promise();
      return promise.continuation_;
    }

    void await_resume() noexcept {}
  };

 protected:
  InlineTaskPromiseBase() noexcept = default;

  InlineTaskPromiseBase(const InlineTaskPromiseBase&) = delete;
  InlineTaskPromiseBase(InlineTaskPromiseBase&&) = delete;
  InlineTaskPromiseBase& operator=(const InlineTaskPromiseBase&) = delete;
  InlineTaskPromiseBase& operator=(InlineTaskPromiseBase&&) = delete;

 public:
  FOLLY_CORO_DEFINE_FRAME_ALLOCATOR(
      ::folly::coro::detail::CoroFrameAsyncAllocator)

  suspend_always initial_suspend() noexcept { return {}; }

  auto final_suspend() noexcept { return FinalAwaiter{}; }

  void set_continuation(coroutine_handle<> continuation) noexcept {
    assert(!continuation_);
    continuation_ = continuation;
  }

 private:
  coroutine_handle<> continuation_;
};

template <typename T>
class InlineTaskPromise : public InlineTaskPromiseBase {
 public:
  static_assert(
      std::is_move_constructible<T>::value,
      "InlineTask<T> only supports types that are move-constructible.");
  static_assert(
      !std::is_rvalue_reference<T>::value, "InlineTask<T&&> is not supported");

  InlineTaskPromise() noexcept = default;

  ~InlineTaskPromise() = default;

  InlineTask<T> get_return_object() noexcept;

  template <typename Value = T>
    requires std::is_convertible_v<Value&&, T>
  void return_value(Value&& value) noexcept(
      std::is_nothrow_constructible<T, Value&&>::value) {
    result_.emplace(static_cast<Value&&>(value));
  }

  void unhandled_exception() noexcept {
    result_.emplaceException(folly::exception_wrapper{current_exception()});
  }

  T result() { return std::move(result_).value(); }

 private:
  // folly::Try<T> doesn't support storing reference types so we store a
  // std::reference_wrapper instead.
  using StorageType = std::conditional_t<
      std::is_lvalue_reference<T>::value,
      std::reference_wrapper<std::remove_reference_t<T>>,
      T>;

  folly::Try<StorageType> result_;
};

template <>
class InlineTaskPromise<void> : public InlineTaskPromiseBase {
 public:
  InlineTaskPromise() noexcept = default;

  InlineTask<void> get_return_object() noexcept;

  void return_void() noexcept {}

  void unhandled_exception() noexcept {
    result_.emplaceException(folly::exception_wrapper{current_exception()});
  }

  void result() { return result_.value(); }

 private:
  folly::Try<void> result_;
};

template <typename T>
class InlineTask {
 public:
  using promise_type = detail::InlineTaskPromise<T>;

 private:
  using handle_t = coroutine_handle<promise_type>;

 public:
  InlineTask(InlineTask&& other) noexcept
      : coro_(std::exchange(other.coro_, {})) {}

  ~InlineTask() {
    if (coro_) {
      coro_.destroy();
    }
  }

  class Awaiter {
   public:
    ~Awaiter() {
      if (coro_) {
        coro_.destroy();
      }
    }

    bool await_ready() noexcept { return false; }

    handle_t await_suspend(coroutine_handle<> awaitingCoroutine) noexcept {
      assert(coro_ && !coro_.done());
      coro_.promise().set_continuation(awaitingCoroutine);
      return coro_;
    }

    T await_resume() {
      auto destroyOnExit = folly::makeGuard([this] {
        std::exchange(coro_, {}).destroy();
      });
      return coro_.promise().result();
    }

   private:
    friend class InlineTask<T>;
    explicit Awaiter(handle_t coro) noexcept : coro_(coro) {}
    handle_t coro_;
  };

  Awaiter operator co_await() && {
    assert(coro_ && !coro_.done());
    return Awaiter{std::exchange(coro_, {})};
  }

 private:
  friend class InlineTaskPromise<T>;
  explicit InlineTask(handle_t coro) noexcept : coro_(coro) {}
  handle_t coro_;
};

template <typename T>
inline InlineTask<T> InlineTaskPromise<T>::get_return_object() noexcept {
  return InlineTask<T>{
      coroutine_handle<InlineTaskPromise<T>>::from_promise(*this)};
}

inline InlineTask<void> InlineTaskPromise<void>::get_return_object() noexcept {
  return InlineTask<void>{
      coroutine_handle<InlineTaskPromise<void>>::from_promise(*this)};
}

/// InlineTaskDetached is a coroutine-return type where the coroutine is
/// launched in the current execution context when it is created and the
/// task's continuation is launched inline in the execution context that the
/// task completed on.
///
/// This task type is primarily intended as a building block for certain
/// coroutine operators. It is not intended for general use in application
/// code or in library interfaces exposed to library code as it can easily be
/// abused to accidentally run logic on the wrong execution context.
///
/// For this reason, the InlineTaskDetached type has been placed inside the
/// folly::coro::detail namespace to discourage general usage.
struct InlineTaskDetached {
  class promise_type {
    struct FinalAwaiter {
      bool await_ready() noexcept { return false; }
      void await_suspend(coroutine_handle<promise_type> h) noexcept {
        folly::deactivateAsyncStackFrame(h.promise().getAsyncFrame());
        h.destroy();
      }
      [[noreturn]] void await_resume() noexcept { folly::assume_unreachable(); }
    };

   public:
    FOLLY_CORO_DEFINE_FRAME_ALLOCATOR(
        ::folly::coro::detail::CoroFrameAsyncAllocator)

    promise_type() noexcept {
      asyncFrame_.setParentFrameUnsafe(folly::getDetachedRootAsyncStackFrame());
    }

    InlineTaskDetached get_return_object() noexcept {
      return InlineTaskDetached{
          coroutine_handle<promise_type>::from_promise(*this)};
    }

    suspend_always initial_suspend() noexcept { return {}; }

    FinalAwaiter final_suspend() noexcept { return {}; }

    void return_void() noexcept {}

    [[noreturn]] void unhandled_exception() noexcept { std::terminate(); }

    template <typename Awaitable>
    decltype(auto) await_transform(Awaitable&& awaitable) {
      return folly::coro::co_withAsyncStack(
          static_cast<Awaitable&&>(awaitable));
    }

    folly::AsyncStackFrame& getAsyncFrame() noexcept { return asyncFrame_; }

   private:
    folly::AsyncStackFrame asyncFrame_;
  };

  InlineTaskDetached(InlineTaskDetached&& other) noexcept
      : coro_(std::exchange(other.coro_, {})) {}

  ~InlineTaskDetached() {
    if (coro_) {
      coro_.destroy();
    }
  }

  FOLLY_NOINLINE void start() noexcept {
    start(FOLLY_ASYNC_STACK_RETURN_ADDRESS());
  }

  void start(void* returnAddress) noexcept {
    coro_.promise().getAsyncFrame().setReturnAddress(returnAddress);
    folly::resumeCoroutineWithNewAsyncStackRoot(std::exchange(coro_, {}));
  }

 private:
  explicit InlineTaskDetached(coroutine_handle<promise_type> h) noexcept
      : coro_(h) {}

  coroutine_handle<promise_type> coro_;
};

} // namespace detail
} // namespace coro
} // namespace folly

#endif // FOLLY_HAS_COROUTINES
