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

#include <folly/CancellationToken.h>
#include <folly/Synchronized.h>
#include <folly/channels/detail/ChannelBridge.h>
#include <folly/coro/Coroutine.h>

namespace folly {
namespace channels {

namespace detail {
template <typename TValue>
ChannelBridgePtr<TValue>& senderGetBridge(Sender<TValue>& sender) {
  return sender.bridge_;
}

template <typename TValue>
bool receiverWait(
    Receiver<TValue>& receiver, detail::IChannelCallback* callback) {
  if (!receiver.buffer_.empty()) {
    return false;
  }
  return receiver.bridge_->receiverWait(callback);
}

template <typename TValue>
detail::IChannelCallback* cancelReceiverWait(Receiver<TValue>& receiver) {
  return receiver.bridge_->cancelReceiverWait();
}

template <typename TValue>
std::optional<Try<TValue>> receiverGetValue(Receiver<TValue>& receiver) {
  if (receiver.buffer_.empty()) {
    receiver.buffer_ = receiver.bridge_->receiverGetValues();
    if (receiver.buffer_.empty()) {
      return std::nullopt;
    }
  }
  auto result = std::move(receiver.buffer_.front());
  receiver.buffer_.pop();
  return result;
}

template <typename TValue>
std::pair<detail::ChannelBridgePtr<TValue>, detail::ReceiverQueue<TValue>>
receiverUnbuffer(Receiver<TValue>&& receiver) {
  return std::make_pair(
      std::move(receiver.bridge_), std::move(receiver.buffer_));
}

} // namespace detail

template <typename TValue>
class Receiver<TValue>::Waiter : public detail::IChannelCallback {
 public:
  Waiter(
      Receiver<TValue>* receiver,
      folly::CancellationToken cancelToken,
      bool closeOnCancel)
      : state_(State{.receiver = receiver}),
        cancelCallback_(
            makeCancellationCallback(std::move(cancelToken), closeOnCancel)) {}

  bool await_ready() const noexcept {
    return state_.withRLock([&](const State& state) {
      return state.cancelled || !state.receiver ||
          !state.receiver->buffer_.empty();
    });
  }

  bool await_suspend(
      folly::coro::coroutine_handle<> awaitingCoroutine) noexcept {
    return state_.withWLock([&](State& state) {
      if (state.cancelled || !state.receiver ||
          !receiverWait(*state.receiver, this)) {
        // We will not suspend at all if the receiver is either cancelled or
        // closed.
        return false;
      }
      state.awaitingCoroutine = awaitingCoroutine;
      return true;
    });
  }

  std::optional<TValue> await_resume() {
    auto result = getResult();
    if (!result.hasValue() && !result.hasException()) {
      return std::nullopt;
    }
    return std::move(result.value());
  }

  // FIXME: The default implementation of `value_or_error_or_stopped` will
  // convert the `getResult()` branch below that returns an empty `Try` into an
  // error state.  That's because the normal `folly::coro` contract does not
  // allow producing an empty `Try`.  Therefore, any future
  // `await_resume_result()` implementation should investigate the intended
  // semantics of this `getResult()` logic, and provide better handling:
  //
  //   if (!state.receiver) {
  //     return Try<TValue>();
  //   }
  //
  // Also, after D73731681 (rich_error), supporting `await_resume_result()`
  // will be also motivated by better perf for signaling cancellation.
  Try<TValue> await_resume_try() { return getResult(); }

 protected:
  struct State {
    Receiver<TValue>* receiver;
    folly::coro::coroutine_handle<> awaitingCoroutine{};
    bool cancelled{false};
  };

  std::unique_ptr<folly::CancellationCallback> makeCancellationCallback(
      folly::CancellationToken cancelToken, bool closeOnCancel) {
    if (!cancelToken.canBeCancelled()) {
      return nullptr;
    }
    return std::make_unique<folly::CancellationCallback>(
        std::move(cancelToken), [this, closeOnCancel] {
          auto receiver = state_.withWLock([&](State& state) {
            state.cancelled = true;
            return std::exchange(state.receiver, nullptr);
          });
          if (!receiver) {
            return;
          }
          if (closeOnCancel) {
            std::move(*receiver).cancel();
          } else {
            auto* callback = detail::cancelReceiverWait(*receiver);
            if (callback) {
              callback->canceled(nullptr);
            }
          }
        });
  }

  void consume(detail::ChannelBridgeBase*) override { resume(); }

  void canceled(detail::ChannelBridgeBase*) override { resume(); }

  void resume() {
    auto awaitingCoroutine = state_.withWLock([&](State& state) {
      return std::exchange(state.awaitingCoroutine, nullptr);
    });
    awaitingCoroutine.resume();
  }

  Try<TValue> getResult() {
    cancelCallback_.reset();
    return state_.withWLock([&](State& state) {
      if (state.cancelled) {
        return Try<TValue>(
            folly::make_exception_wrapper<folly::OperationCancelled>());
      }
      if (!state.receiver) {
        return Try<TValue>();
      }
      auto result =
          std::move(detail::receiverGetValue(*state.receiver).value());
      if (!result.hasValue()) {
        std::move(*state.receiver).cancel();
        state.receiver = nullptr;
      }
      return result;
    });
  }

  folly::Synchronized<State> state_;
  std::unique_ptr<folly::CancellationCallback> cancelCallback_;
};

template <typename TValue>
struct Receiver<TValue>::NextSemiAwaitable {
 public:
  explicit NextSemiAwaitable(
      Receiver<TValue>* receiver,
      bool closeOnCancel,
      std::optional<folly::CancellationToken> cancelToken = std::nullopt)
      : receiver_(receiver),
        closeOnCancel_(closeOnCancel),
        cancelToken_(std::move(cancelToken)) {}

  [[nodiscard]] Waiter operator co_await() {
    return Waiter(
        receiver_,
        cancelToken_.value_or(folly::CancellationToken()),
        closeOnCancel_);
  }

  friend NextSemiAwaitable co_withCancellation(
      folly::CancellationToken cancelToken, NextSemiAwaitable&& awaitable) {
    if (awaitable.cancelToken_.has_value()) {
      return std::move(awaitable);
    }
    return NextSemiAwaitable(
        awaitable.receiver_, awaitable.closeOnCancel_, std::move(cancelToken));
  }

 private:
  Receiver<TValue>* receiver_;
  bool closeOnCancel_;
  std::optional<folly::CancellationToken> cancelToken_;
};
} // namespace channels
} // namespace folly
