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

#include <concepts>
#include <fmt/format.h>
#include <folly/Executor.h>
#include <folly/IntrusiveList.h>
#include <folly/ScopeGuard.h>
#include <folly/channels/Channel.h>
#include <folly/channels/ChannelCallbackHandle.h>
#include <folly/channels/detail/Utility.h>
#include <folly/coro/Task.h>

namespace folly {
namespace channels {

namespace detail {

template <typename TValue, typename OnNextFunc>
class ChannelCallbackProcessorImpl : public ChannelCallbackProcessor {
 public:
  ChannelCallbackProcessorImpl(
      ChannelBridgePtr<TValue> receiver,
      folly::Executor::KeepAlive<folly::SequencedExecutor> executor,
      OnNextFunc onNext)
      : receiver_(std::move(receiver)),
        executor_(std::move(executor)),
        onNext_(std::move(onNext)),
        cancelSource_(folly::CancellationSource::invalid()) {}

  void start(std::optional<detail::ReceiverQueue<TValue>> buffer) {
    co_withExecutor(
        executor_,
        runCoroutineWithCancellation(
            processAllAvailableValues(std::move(buffer))))
        .start();
  }

 private:
  /**
   * Called when the handle is destroyed.
   */
  void onHandleDestroyed() override {
    executor_->add([=, this]() { processHandleDestroyed(); });
  }

  /**
   * Called when the channel we are listening to has an update.
   */
  void consume(ChannelBridgeBase*) override {
    co_withExecutor(
        executor_, runCoroutineWithCancellation(processAllAvailableValues()))
        .start();
  }

  /**
   * Called after we cancelled the input channel (which happens after the handle
   * is destroyed).
   */
  void canceled(ChannelBridgeBase*) override {
    co_withExecutor(
        executor_,
        runCoroutineWithCancellation(
            processReceiverCancelled(true /* fromHandleDestruction */)))
        .start();
  }

  /**
   * Processes all available values from the input receiver (starting from the
   * provided buffer, if present).
   *
   * If a value was received indicating that the input channel has been closed,
   * we will process cancellation for the input receiver.
   */
  folly::coro::Task<void> processAllAvailableValues(
      std::optional<ReceiverQueue<TValue>> buffer = std::nullopt) {
    bool closed = buffer.has_value()
        ? !co_await processValues(std::move(buffer.value()))
        : false;
    while (!closed) {
      if (receiver_->receiverWait(this)) {
        // There are no more values available right now, but more values may
        // come in the future. We will stop processing for now, until we
        // re-start processing when the consume() callback is fired.
        break;
      }
      auto values = receiver_->receiverGetValues();
      CHECK(!values.empty());
      closed = !co_await processValues(std::move(values));
    }
    if (closed) {
      // The input receiver was closed.
      receiver_->receiverCancel();
      co_await processReceiverCancelled(false /* fromHandleDestruction */);
    }
  }

  /**
   * Processes values from the channel. Returns false if the channel has been
   * closed, so the caller can stop processing values from it.
   */
  folly::coro::Task<bool> processValues(ReceiverQueue<TValue> values) {
    auto cancelToken = co_await folly::coro::co_current_cancellation_token;
    while (!values.empty()) {
      if (cancelToken.isCancellationRequested()) {
        co_return true;
      }
      auto result = std::move(values.front());
      values.pop();
      bool closed = !result.hasValue();
      if (!co_await callCallback(std::move(result))) {
        closed = true;
      }
      if (closed) {
        co_return false;
      }
      co_await folly::coro::co_reschedule_on_current_executor;
    }
    co_return true;
  }

  /**
   * Process cancellation of the input receiver.
   *
   * @param fromHandleDestruction: Whether the cancellation was prompted by the
   *    handle being destroyed. If true, we will call the user's callback with
   *    a folly::OperationCancelled exception. This will be false if the
   *    cancellation was prompted by the closure of the channel.
   */
  folly::coro::Task<void> processReceiverCancelled(bool fromHandleDestruction) {
    CHECK_EQ(getReceiverState(), ChannelState::CancellationTriggered);
    receiver_ = nullptr;
    if (fromHandleDestruction) {
      co_await callCallback(
          Try<TValue>(
              folly::make_exception_wrapper<folly::OperationCancelled>()));
    }
    maybeDelete();
  }

  /**
   * Processes the destruction of the handle.
   */
  void processHandleDestroyed() {
    CHECK(!handleDestroyed_);
    handleDestroyed_ = true;
    cancelSource_.requestCancellation();
    if (getReceiverState() == ChannelState::Active) {
      receiver_->receiverCancel();
    }
    maybeDelete();
  }

  /**
   * Deletes this object if we have already processed cancellation for the
   * receiver and the handle.
   */
  void maybeDelete() {
    if (getReceiverState() == ChannelState::CancellationProcessed &&
        handleDestroyed_) {
      delete this;
    }
  }

  /**
   * Calls the user's callback with the given result.
   */
  folly::coro::Task<bool> callCallback(Try<TValue> result) {
    auto retVal = co_await folly::coro::co_awaitTry(onNext_(std::move(result)));
    if (retVal.template hasException<folly::OperationCancelled>()) {
      co_return false;
    } else if (retVal.hasException()) {
      LOG(FATAL) << fmt::format(
          "Encountered exception from callback when consuming channel of "
          "type {}: {}",
          typeid(TValue).name(),
          retVal.exception().what());
    }
    co_return retVal.value();
  }

  /**
   * Runs the given coroutine while listening for cancellation triggered by the
   * handle's destruction.
   */
  folly::coro::Task<void> runCoroutineWithCancellation(
      folly::coro::Task<void> task) {
    cancelSource_ = folly::CancellationSource();
    if (handleDestroyed_) {
      // The handle was already destroyed before we even started the coroutine.
      // Request cancellation so that the user's callback knows to stop quickly.
      cancelSource_.requestCancellation();
    }
    auto token = cancelSource_.getToken();
    auto retVal = co_await folly::coro::co_awaitTry(
        folly::coro::co_withCancellation(token, std::move(task)));
    CHECK(!retVal.hasException()) << fmt::format(
        "Unexpected exception when running coroutine: {}",
        retVal.exception().what());
    if (!token.isCancellationRequested()) {
      cancelSource_ = folly::CancellationSource::invalid();
    }
  }

  ChannelState getReceiverState() {
    return detail::getReceiverState(receiver_.get());
  }

  ChannelBridgePtr<TValue> receiver_;
  folly::Executor::KeepAlive<folly::SequencedExecutor> executor_;
  OnNextFunc onNext_;
  folly::CancellationSource cancelSource_;
  bool handleDestroyed_{false};
};
} // namespace detail

template <typename TReceiver, typename OnNextFunc, typename TValue>
  requires std::constructible_from<
      folly::Function<folly::coro::Task<bool>(Try<TValue>)>,
      OnNextFunc>
ChannelCallbackHandle consumeChannelWithCallback(
    TReceiver receiver,
    folly::Executor::KeepAlive<folly::SequencedExecutor> executor,
    OnNextFunc onNext) {
  detail::ChannelCallbackProcessorImpl<TValue, OnNextFunc>* processor = nullptr;
  auto [unbufferedReceiver, buffer] =
      detail::receiverUnbuffer(std::move(receiver));
  processor = new detail::ChannelCallbackProcessorImpl<TValue, OnNextFunc>(
      std::move(unbufferedReceiver), std::move(executor), std::move(onNext));
  processor->start(std::move(buffer));
  return ChannelCallbackHandle(processor);
}
} // namespace channels
} // namespace folly
