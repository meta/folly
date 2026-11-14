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

#include <folly/Portability.h> // FOLLY_HAS_COROUTINES
#include <folly/coro/CurrentExecutor.h>
#include <folly/coro/Error.h>
#include <folly/coro/Nothrow.h>
#include <folly/coro/ValueOrError.h>

/// Coroutine promise implementation shared between tasks and async generators.
/// Only the `await_transform` signatures here are of interest to end-users.

#if FOLLY_HAS_COROUTINES

namespace folly::coro::detail {

class TaskPromiseBase;

template <typename Reference, typename Value, bool RequiresCleanup = false>
class AsyncGeneratorPromise;

template <typename TailStorage = Unit>
class BasePromise {
 protected:
  // The destructor is protected to alert future authors -- this is a TIGHTLY
  // COUPLED DETAIL of task & async generator, not an easily reusable
  // component.  For example, it relies on correctly specified
  // `await_transform` behavior, a correct definition of the `getErrorHandle`
  // protocol, and its correct usage in `await_suspend`.
  friend class TaskPromiseBase;
  template <typename, typename, bool>
  friend class AsyncGeneratorPromise;
  ~BasePromise() = default;

  ExtendedCoroutineHandle continuation_;
  folly::AsyncStackFrame asyncFrame_;
  folly::Executor::KeepAlive<> executor_;
  folly::CancellationToken cancelToken_;
  bool hasCancelTokenOverride_ = false;
  BypassExceptionThrowing bypassThrowing_;
  // Let derived classes pack data in one word with the prior 2 members
  // This could perhaps be avoided by using bitfields...
  TailStorage tailStorage_;

  // Implementation of `await_transform(co_safe_point_t)`
  template <typename Awaiter, typename Promise>
  variant_awaitable<Awaiter, ready_awaitable<>> do_safe_point(
      Promise& promise) noexcept {
    if (cancelToken_.isCancellationRequested()) {
      return promise.yield_value(co_stopped_may_throw);
    }
    return ready_awaitable<>{};
  }

  // Async generator & task specialize this to check the type of Promise
  template <typename Promise>
  static std::optional<ExtendedCoroutineHandle::ErrorHandle>
  getErrorHandleUncheckedImpl(Promise& me, exception_wrapper& ex) {
    if (me.bypassThrowing_.shouldBypassFor(ex)) {
      auto finalAwaiter = me.yield_value(co_error(std::move(ex)));
      DCHECK(!finalAwaiter.await_ready());
      return ExtendedCoroutineHandle::ErrorHandle{
          finalAwaiter.await_suspend_promise(me),
          // finalAwaiter.await_suspend pops a frame
          me.getAsyncFrame().getParentFrameUnsafe()};
    }
    return std::nullopt;
  }

 public:
  template <
      typename Awaitable,
      std::enable_if_t<!folly::ext::must_use_immediately_v<Awaitable>, int> = 0>
  auto await_transform(Awaitable&& awaitable) {
    bypassThrowing_.maybeActivate<Awaitable>();
    return folly::coro::co_withAsyncStack(
        folly::coro::co_viaIfAsync(
            executor_.get_alias(),
            folly::coro::co_withCancellation(
                cancelToken_, static_cast<Awaitable&&>(awaitable))));
  }
  template <
      typename Awaitable,
      std::enable_if_t<folly::ext::must_use_immediately_v<Awaitable>, int> = 0>
  auto await_transform(Awaitable awaitable) {
    bypassThrowing_.maybeActivate<Awaitable>();
    return folly::coro::co_withAsyncStack(
        folly::coro::co_viaIfAsync(
            executor_.get_alias(),
            folly::coro::co_withCancellation(
                cancelToken_,
                folly::ext::must_use_immediately_unsafe_mover(
                    std::move(awaitable))())));
  }

  template <typename Awaitable>
  auto await_transform(NothrowAwaitable<Awaitable> awaitable) {
    bypassThrowing_.requestDueToNothrow<Awaitable>();
    return await_transform(
        folly::ext::must_use_immediately_unsafe_mover(awaitable.unwrap())());
  }

  template <typename Awaitable>
  auto await_transform(ValueOrError<Awaitable> awaitable) {
    bypassThrowing_.template requestDueToValueOrError<Awaitable>();
    return await_transform(std::move(awaitable).toValueOrErrorImpl());
  }

  auto await_transform(co_current_executor_t) noexcept {
    return ready_awaitable<folly::Executor*>{executor_.get()};
  }

  auto await_transform(co_current_cancellation_token_t) noexcept {
    return ready_awaitable<const folly::CancellationToken&>{cancelToken_};
  }

  // End-users can do this via `co_withCancellation`.
  void setCancellationToken(
      const folly::CancellationToken& cancelToken) noexcept {
    // Only keep the first cancellation token.  This is usually the inner-most
    // cancellation scope of the consumer's calling context.
    if (!hasCancelTokenOverride_) {
      cancelToken_ = folly::CancellationToken{cancelToken};
      hasCancelTokenOverride_ = true;
    }
  }

  void setCancellationToken(folly::CancellationToken&& cancelToken) noexcept {
    // Rvalue tokens are consumed even when an earlier override is kept.
    auto consumedToken = std::move(cancelToken);
    if (!hasCancelTokenOverride_) {
      cancelToken_ = std::move(consumedToken);
      hasCancelTokenOverride_ = true;
    }
  }

  folly::AsyncStackFrame& getAsyncFrame() noexcept { return asyncFrame_; }
};

} // namespace folly::coro::detail

#endif // FOLLY_HAS_COROUTINES
