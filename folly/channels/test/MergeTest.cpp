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

#include <folly/channels/ConsumeChannel.h>
#include <folly/channels/Merge.h>
#include <folly/channels/test/ChannelTestUtil.h>
#include <folly/channels/testing/MockNextCallback.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/executors/SerialExecutor.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

namespace folly {
namespace channels {

using namespace testing;

class MergeFixture : public Test {
 protected:
  ~MergeFixture() { executor_.drain(); }

  ChannelCallbackHandle processValues(Receiver<int> receiver) {
    return consumeChannelWithCallback(
        std::move(receiver),
        &executor_,
        [=, this](Try<int> resultTry) -> folly::coro::Task<bool> {
          onNext_(std::move(resultTry));
          co_return true;
        });
  }

  folly::ManualExecutor executor_;
  StrictMock<MockNextCallback<int>> onNext_;
};

TEST_F(MergeFixture, ReceiveValues_ReturnMergedValues) {
  auto [receiver1, sender1] = Channel<int>::create();
  auto [receiver2, sender2] = Channel<int>::create();
  auto [receiver3, sender3] = Channel<int>::create();
  auto mergedReceiver = merge(
      toVector(
          std::move(receiver1), std::move(receiver2), std::move(receiver3)),
      &executor_);

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onValue(2));
  EXPECT_CALL(onNext_, onValue(3));
  EXPECT_CALL(onNext_, onValue(4));
  EXPECT_CALL(onNext_, onValue(5));
  EXPECT_CALL(onNext_, onValue(6));
  EXPECT_CALL(onNext_, onClosed());

  sender1.write(1);
  sender2.write(2);
  sender3.write(3);
  executor_.drain();

  auto callbackHandle = processValues(std::move(mergedReceiver));

  sender1.write(4);
  sender2.write(5);
  sender3.write(6);
  executor_.drain();

  std::move(sender1).close();
  std::move(sender2).close();
  std::move(sender3).close();
  executor_.drain();
}

TEST_F(MergeFixture, OneInputClosed_WaitForAllInputsToClose_ContinuesMerging) {
  auto [receiver1, sender1] = Channel<int>::create();
  auto [receiver2, sender2] = Channel<int>::create();
  auto [receiver3, sender3] = Channel<int>::create();
  auto mergedReceiver = merge(
      toVector(
          std::move(receiver1), std::move(receiver2), std::move(receiver3)),
      &executor_);

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onValue(2));
  EXPECT_CALL(onNext_, onValue(3));
  EXPECT_CALL(onNext_, onValue(4));
  EXPECT_CALL(onNext_, onValue(5));
  EXPECT_CALL(onNext_, onClosed());

  auto callbackHandle = processValues(std::move(mergedReceiver));

  sender1.write(1);
  sender2.write(2);
  sender3.write(3);
  std::move(sender3).close();
  executor_.drain();

  sender1.write(4);
  sender2.write(5);
  executor_.drain();

  std::move(sender1).close();
  std::move(sender2).close();
  executor_.drain();
}

TEST_F(MergeFixture, OneInputClosed_DoNotWaitForAllInputsToClose_StopsMerging) {
  auto [receiver1, sender1] = Channel<int>::create();
  auto [receiver2, sender2] = Channel<int>::create();
  auto [receiver3, sender3] = Channel<int>::create();
  auto mergedReceiver = merge(
      toVector(
          std::move(receiver1), std::move(receiver2), std::move(receiver3)),
      &executor_,
      false /* waitForAllInputsToClose */);

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onValue(2));
  EXPECT_CALL(onNext_, onValue(3));
  EXPECT_CALL(onNext_, onClosed());

  auto callbackHandle = processValues(std::move(mergedReceiver));
  executor_.drain();

  sender1.write(1);
  sender2.write(2);
  sender3.write(3);
  std::move(sender3).close();
  executor_.drain();

  sender1.write(4);
  executor_.drain();

  sender2.write(5);
  executor_.drain();

  std::move(sender1).close();
  std::move(sender2).close();
  executor_.drain();
}

TEST_F(MergeFixture, OneInputThrows_OutputClosedWithException) {
  auto [receiver1, sender1] = Channel<int>::create();
  auto [receiver2, sender2] = Channel<int>::create();
  auto [receiver3, sender3] = Channel<int>::create();
  auto mergedReceiver = merge(
      toVector(
          std::move(receiver1), std::move(receiver2), std::move(receiver3)),
      &executor_);

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onValue(2));
  EXPECT_CALL(onNext_, onValue(3));
  EXPECT_CALL(onNext_, onRuntimeError("std::runtime_error: Error"));

  auto callbackHandle = processValues(std::move(mergedReceiver));
  executor_.drain();

  sender1.write(1);
  sender2.write(2);
  sender3.write(3);
  std::move(sender3).close(std::runtime_error("Error"));
  executor_.drain();

  sender1.write(4);
  sender2.write(5);
  std::move(sender1).close();
  std::move(sender2).close();
  executor_.drain();
}

TEST_F(MergeFixture, CancelledAfterStart) {
  auto [receiver1, sender1] = Channel<int>::create();
  auto [receiver2, sender2] = Channel<int>::create();
  auto [receiver3, sender3] = Channel<int>::create();
  auto mergedReceiver = merge(
      toVector(
          std::move(receiver1), std::move(receiver2), std::move(receiver3)),
      &executor_);

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onValue(2));
  EXPECT_CALL(onNext_, onValue(3));
  EXPECT_CALL(onNext_, onCancelled());

  auto callbackHandle = processValues(std::move(mergedReceiver));

  sender1.write(1);
  sender2.write(2);
  sender3.write(3);
  executor_.drain();

  callbackHandle.reset();
  executor_.drain();

  sender1.write(4);
  sender2.write(5);
  sender3.write(6);
  executor_.drain();

  std::move(sender1).close();
  std::move(sender2).close();
  std::move(sender3).close();
  executor_.drain();
}

TEST_F(MergeFixture, CancelledBeforeStart) {
  auto [receiver1, sender1] = Channel<int>::create();
  auto [receiver2, sender2] = Channel<int>::create();
  auto [receiver3, sender3] = Channel<int>::create();
  auto mergedReceiver = merge(
      toVector(
          std::move(receiver1), std::move(receiver2), std::move(receiver3)),
      &executor_);

  EXPECT_CALL(onNext_, onValue(_)).Times(0);

  {
    auto toDestroy = std::move(mergedReceiver);
  }

  sender1.write(1);
  sender2.write(2);
  sender3.write(3);
  executor_.drain();
}

struct ProducedValue {
  int producerIndex;
  int value;
};

class MergeFixtureStress : public Test {
 protected:
  MergeFixtureStress()
      : producers_(toVector(makeProducer(0), makeProducer(1), makeProducer(2))),
        consumer_(makeConsumer()) {}

  static std::unique_ptr<StressTestProducer<ProducedValue>> makeProducer(
      int index) {
    return std::make_unique<StressTestProducer<ProducedValue>>(
        [index, value = 0]() mutable { return ProducedValue{index, value++}; });
  }

  static std::unique_ptr<StressTestConsumer<ProducedValue>> makeConsumer() {
    return std::make_unique<StressTestConsumer<ProducedValue>>(
        ConsumptionMode::CallbackWithHandle,
        [lastReceived = toVector(-1, -1, -1)](ProducedValue value) mutable {
          EXPECT_EQ(value.value, ++lastReceived[value.producerIndex]);
        });
  }

  static constexpr std::chrono::milliseconds kTestTimeout =
      std::chrono::milliseconds{5000};

  std::vector<std::unique_ptr<StressTestProducer<ProducedValue>>> producers_;
  std::unique_ptr<StressTestConsumer<ProducedValue>> consumer_;
};

TEST_F(MergeFixtureStress, Close_NoException) {
  auto receivers = std::vector<Receiver<ProducedValue>>();
  for (auto& producer : producers_) {
    auto [receiver, sender] = Channel<ProducedValue>::create();
    receivers.push_back(std::move(receiver));
    producer->startProducing(std::move(sender), std::nullopt /* closeEx */);
  }

  folly::CPUThreadPoolExecutor mergeExecutor(1);
  consumer_->startConsuming(merge(
      std::move(receivers), folly::SerialExecutor::create(&mergeExecutor)));

  for (auto& producer : producers_) {
    /* sleep override */
    std::this_thread::sleep_for(kTestTimeout / 3);
    producer->stopProducing();
  }

  EXPECT_EQ(consumer_->waitForClose().get(), CloseType::NoException);
}

TEST_F(MergeFixtureStress, Close_Exception) {
  auto receivers = std::vector<Receiver<ProducedValue>>();
  for (size_t i = 0; i < producers_.size(); i++) {
    auto [receiver, sender] = Channel<ProducedValue>::create();
    receivers.push_back(std::move(receiver));
    producers_.at(i)->startProducing(
        std::move(sender),
        i == 1 ? std::make_optional(
                     folly::make_exception_wrapper<std::runtime_error>("Error"))
               : std::nullopt /* closeEx */);
  }

  folly::CPUThreadPoolExecutor mergeExecutor(1);
  consumer_->startConsuming(merge(
      std::move(receivers), folly::SerialExecutor::create(&mergeExecutor)));

  /* sleep override */
  std::this_thread::sleep_for(kTestTimeout / 2);
  producers_.at(0)->stopProducing();

  /* sleep override */
  std::this_thread::sleep_for(kTestTimeout / 2);
  producers_.at(1)->stopProducing();

  EXPECT_EQ(consumer_->waitForClose().get(), CloseType::Exception);
}

TEST_F(MergeFixtureStress, Cancelled) {
  auto receivers = std::vector<Receiver<ProducedValue>>();
  for (size_t i = 0; i < producers_.size(); i++) {
    auto [receiver, sender] = Channel<ProducedValue>::create();
    receivers.push_back(std::move(receiver));
    producers_.at(i)->startProducing(
        std::move(sender), std::nullopt /* closeEx */);
  }

  folly::CPUThreadPoolExecutor mergeExecutor(1);
  consumer_->startConsuming(merge(
      std::move(receivers), folly::SerialExecutor::create(&mergeExecutor)));

  /* sleep override */
  std::this_thread::sleep_for(kTestTimeout);
  consumer_->cancel();

  EXPECT_EQ(consumer_->waitForClose().get(), CloseType::Cancelled);
}

TEST_F(MergeFixtureStress, Close_NoException_ThenCancelledImmediately) {
  auto receivers = std::vector<Receiver<ProducedValue>>();
  for (size_t i = 0; i < producers_.size(); i++) {
    auto [receiver, sender] = Channel<ProducedValue>::create();
    receivers.push_back(std::move(receiver));
    producers_.at(i)->startProducing(
        std::move(sender), std::nullopt /* closeEx */);
  }

  folly::CPUThreadPoolExecutor mergeExecutor(1);
  consumer_->startConsuming(merge(
      std::move(receivers), folly::SerialExecutor::create(&mergeExecutor)));

  /* sleep override */
  std::this_thread::sleep_for(kTestTimeout);
  for (auto& producer : producers_) {
    producer->stopProducing();
  }
  consumer_->cancel();

  EXPECT_THAT(
      consumer_->waitForClose().get(),
      AnyOf(Eq(CloseType::NoException), Eq(CloseType::Cancelled)));
}

TEST_F(MergeFixtureStress, Cancelled_ThenClosedImmediately_NoException) {
  auto receivers = std::vector<Receiver<ProducedValue>>();
  for (size_t i = 0; i < producers_.size(); i++) {
    auto [receiver, sender] = Channel<ProducedValue>::create();
    receivers.push_back(std::move(receiver));
    producers_.at(i)->startProducing(
        std::move(sender), std::nullopt /* closeEx */);
  }

  folly::CPUThreadPoolExecutor mergeExecutor(1);
  consumer_->startConsuming(merge(
      std::move(receivers), folly::SerialExecutor::create(&mergeExecutor)));

  /* sleep override */
  std::this_thread::sleep_for(kTestTimeout);
  consumer_->cancel();
  for (auto& producer : producers_) {
    producer->stopProducing();
  }

  EXPECT_THAT(
      consumer_->waitForClose().get(),
      AnyOf(Eq(CloseType::NoException), Eq(CloseType::Cancelled)));
}
} // namespace channels
} // namespace folly
