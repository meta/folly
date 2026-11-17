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

#include <folly/channels/Channel.h>
#include <folly/channels/test/ChannelTestUtil.h>
#include <folly/channels/testing/MockNextCallback.h>
#include <folly/coro/BlockingWait.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/portability/GTest.h>

namespace folly {
namespace channels {

using namespace testing;

class ChannelFixture
    : public Test,
      public WithParamInterface<ConsumptionMode>,
      public ChannelConsumerBase<int> {
 protected:
  ChannelFixture() : ChannelConsumerBase(GetParam()) {}

  ~ChannelFixture() override {
    cancellationSource_.requestCancellation();
    if (!continueConsuming_.isFulfilled()) {
      continueConsuming_.setValue(false);
    }
    executor_.drain();
  }

  folly::Executor::KeepAlive<folly::SequencedExecutor> getExecutor() override {
    return &executor_;
  }

  void onNext(Try<int> result) override { onNext_(std::move(result)); }

  folly::ManualExecutor executor_;
  StrictMock<MockNextCallback<int>> onNext_;
};

TEST_P(ChannelFixture, SingleWriteBeforeNext_ThenCancelled) {
  auto [receiver, sender] = Channel<int>::create();
  sender.write(1);

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onCancelled());

  startConsuming(std::move(receiver));
  executor_.drain();

  EXPECT_FALSE(sender.isReceiverCancelled());

  cancellationSource_.requestCancellation();
  executor_.drain();

  EXPECT_TRUE(sender.isReceiverCancelled());
}

TEST_P(ChannelFixture, SingleWriteAfterNext_ThenCancelled) {
  auto [receiver, sender] = Channel<int>::create();

  startConsuming(std::move(receiver));
  executor_.drain();

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onCancelled());

  sender.write(1);
  executor_.drain();

  EXPECT_FALSE(sender.isReceiverCancelled());

  cancellationSource_.requestCancellation();
  executor_.drain();

  EXPECT_TRUE(sender.isReceiverCancelled());
}

TEST_P(ChannelFixture, MultipleWrites_ThenStopConsumingByReturningFalse) {
  auto [receiver, sender] = Channel<int>::create();

  startConsuming(std::move(receiver));
  executor_.drain();

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onValue(2));
  EXPECT_CALL(onNext_, onValue(3));

  sender.write(1);
  sender.write(2);
  executor_.drain();

  sender.write(3);
  sender.write(4);
  continueConsuming_ = folly::SharedPromise<bool>();
  continueConsuming_.setValue(false);
  executor_.drain();
}

TEST_P(
    ChannelFixture,
    MultipleWrites_ThenStopConsumingByThrowingOperationCancelled) {
  auto [receiver, sender] = Channel<int>::create();

  startConsuming(std::move(receiver));
  executor_.drain();

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onValue(2));
  EXPECT_CALL(onNext_, onValue(3));

  sender.write(1);
  sender.write(2);
  executor_.drain();

  sender.write(3);
  sender.write(4);
  continueConsuming_ = folly::SharedPromise<bool>();
  continueConsuming_.setException(folly::OperationCancelled());
  executor_.drain();
}

TEST_P(ChannelFixture, Close_NoException_BeforeSubscribe) {
  auto [receiver, sender] = Channel<int>::create();

  std::move(sender).close();

  EXPECT_CALL(onNext_, onClosed());

  startConsuming(std::move(receiver));
  executor_.drain();
}

TEST_P(ChannelFixture, Close_NoException_AfterSubscribeAndWrite) {
  auto [receiver, sender] = Channel<int>::create();

  sender.write(1);
  std::move(sender).close();

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onClosed());

  startConsuming(std::move(receiver));
  executor_.drain();
}

TEST_P(ChannelFixture, Close_DueToDestruction_BeforeSubscribe) {
  auto [receiver, sender] = Channel<int>::create();

  {
    auto toDestroy = std::move(sender);
  }

  EXPECT_CALL(onNext_, onClosed());

  startConsuming(std::move(receiver));
  executor_.drain();
}

TEST_P(ChannelFixture, Close_DueToDestruction_AfterSubscribeAndWrite) {
  auto [receiver, sender] = Channel<int>::create();

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onClosed());

  startConsuming(std::move(receiver));

  sender.write(1);
  {
    auto toDestroy = std::move(sender);
  }

  executor_.drain();
}

TEST_P(ChannelFixture, Close_Exception_BeforeSubscribe) {
  auto [receiver, sender] = Channel<int>::create();

  std::move(sender).close(std::runtime_error("Error"));

  EXPECT_CALL(onNext_, onRuntimeError("std::runtime_error: Error"));

  startConsuming(std::move(receiver));
  executor_.drain();
}

TEST_P(ChannelFixture, Close_Exception_AfterSubscribeAndWrite) {
  auto [receiver, sender] = Channel<int>::create();

  sender.write(1);
  std::move(sender).close(std::runtime_error("Error"));

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onRuntimeError("std::runtime_error: Error"));

  startConsuming(std::move(receiver));
  executor_.drain();
}

TEST_P(ChannelFixture, CancellationRespected) {
  auto [receiver, sender] = Channel<int>::create();

  EXPECT_CALL(onNext_, onValue(1));

  continueConsuming_ = folly::SharedPromise<bool>();
  sender.write(1);
  startConsuming(std::move(receiver));
  executor_.drain();

  EXPECT_FALSE(sender.isReceiverCancelled());

  cancellationSource_.requestCancellation();
  executor_.drain();

  EXPECT_TRUE(sender.isReceiverCancelled());
}

TEST(Channel, CancelNextWithoutClose) {
  folly::ManualExecutor executor;
  folly::CancellationSource cancelSource;
  auto [receiver, sender] = Channel<int>::create();

  sender.write(1);
  EXPECT_EQ(folly::coro::blockingWait(receiver.next()).value(), 1);

  auto nextTask =
      co_withExecutor(
          &executor,
          folly::coro::co_withCancellation(
              cancelSource.getToken(),
              folly::coro::co_invoke(
                  [&receiver_2 =
                       receiver]() -> folly::coro::Task<std::optional<int>> {
                    co_return co_await receiver_2.next(
                        false /* closeOnCancel */);
                  })))
          .start();
  executor.drain();

  cancelSource.requestCancellation();
  sender.write(2);
  executor.drain();

  EXPECT_THROW(
      folly::coro::blockingWait(std::move(nextTask)),
      folly::OperationCancelled);

  EXPECT_EQ(folly::coro::blockingWait(receiver.next()).value(), 2);
}

INSTANTIATE_TEST_SUITE_P(
    Channel_Coro_WithTry,
    ChannelFixture,
    testing::Values(ConsumptionMode::CoroWithTry));

INSTANTIATE_TEST_SUITE_P(
    Channel_Coro_WithoutTry,
    ChannelFixture,
    testing::Values(ConsumptionMode::CoroWithoutTry));

INSTANTIATE_TEST_SUITE_P(
    Channel_Callback_WithHandle,
    ChannelFixture,
    testing::Values(ConsumptionMode::CallbackWithHandle));

class ChannelFixtureStress
    : public Test,
      public WithParamInterface<ConsumptionMode> {
 protected:
  ChannelFixtureStress()
      : producer_(
            std::make_unique<StressTestProducer<int>>([value = 0]() mutable {
              return value++;
            })),
        consumer_(
            std::make_unique<StressTestConsumer<int>>(
                GetParam(), [lastReceived = -1](int value) mutable {
                  EXPECT_EQ(value, ++lastReceived);
                })) {}

  static constexpr std::chrono::milliseconds kTestTimeout =
      std::chrono::milliseconds{5000};

  std::unique_ptr<StressTestProducer<int>> producer_;
  std::unique_ptr<StressTestConsumer<int>> consumer_;
};

TEST_P(ChannelFixtureStress, Close_NoException) {
  auto [receiver, sender] = Channel<int>::create();

  producer_->startProducing(std::move(sender), std::nullopt /* closeEx */);
  consumer_->startConsuming(std::move(receiver));

  /* sleep override */
  std::this_thread::sleep_for(kTestTimeout);
  producer_->stopProducing();
  EXPECT_EQ(consumer_->waitForClose().get(), CloseType::NoException);
}

TEST_P(ChannelFixtureStress, Close_Exception) {
  auto [receiver, sender] = Channel<int>::create();

  producer_->startProducing(std::move(sender), std::runtime_error("Error"));
  consumer_->startConsuming(std::move(receiver));

  /* sleep override */
  std::this_thread::sleep_for(kTestTimeout);
  producer_->stopProducing();
  EXPECT_EQ(consumer_->waitForClose().get(), CloseType::Exception);
}

TEST_P(ChannelFixtureStress, Cancelled) {
  auto [receiver, sender] = Channel<int>::create();

  producer_->startProducing(std::move(sender), std::nullopt /* closeEx */);
  consumer_->startConsuming(std::move(receiver));

  /* sleep override */
  std::this_thread::sleep_for(kTestTimeout);

  consumer_->cancel();
  EXPECT_EQ(consumer_->waitForClose().get(), CloseType::Cancelled);
  producer_->stopProducing();
}

TEST_P(ChannelFixtureStress, Close_NoException_ThenCancelledImmediately) {
  auto [receiver, sender] = Channel<int>::create();

  producer_->startProducing(std::move(sender), std::nullopt /* closeEx */);
  consumer_->startConsuming(std::move(receiver));

  /* sleep override */
  std::this_thread::sleep_for(kTestTimeout);
  producer_->stopProducing();
  consumer_->cancel();
  EXPECT_THAT(
      consumer_->waitForClose().get(),
      AnyOf(Eq(CloseType::NoException), Eq(CloseType::Cancelled)));
}

TEST_P(ChannelFixtureStress, Cancelled_ThenClosedImmediately_NoException) {
  auto [receiver, sender] = Channel<int>::create();

  producer_->startProducing(std::move(sender), std::nullopt /* closeEx */);
  consumer_->startConsuming(std::move(receiver));

  /* sleep override */
  std::this_thread::sleep_for(kTestTimeout);
  consumer_->cancel();
  producer_->stopProducing();
  EXPECT_THAT(
      consumer_->waitForClose().get(),
      AnyOf(Eq(CloseType::NoException), Eq(CloseType::Cancelled)));
}

INSTANTIATE_TEST_SUITE_P(
    Channel_Coro_WithTry,
    ChannelFixtureStress,
    testing::Values(ConsumptionMode::CoroWithTry));

INSTANTIATE_TEST_SUITE_P(
    Channel_Coro_WithoutTry,
    ChannelFixtureStress,
    testing::Values(ConsumptionMode::CoroWithoutTry));

INSTANTIATE_TEST_SUITE_P(
    Channel_Callback_WithHandle,
    ChannelFixtureStress,
    testing::Values(ConsumptionMode::CallbackWithHandle));
} // namespace channels
} // namespace folly
