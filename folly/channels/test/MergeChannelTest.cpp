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

#include <fmt/format.h>
#include <folly/channels/ConsumeChannel.h>
#include <folly/channels/MergeChannel.h>
#include <folly/channels/test/ChannelTestUtil.h>
#include <folly/channels/testing/MockNextCallback.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/executors/SerialExecutor.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

namespace folly {
namespace channels {

using namespace std::string_literals;
using namespace testing;

template <typename T, typename U>
bool isMatch(
    const std::string& type,
    const T& actual,
    const U& expected,
    testing::MatchResultListener* resultListener) {
  if (expected != actual) {
    *resultListener << fmt::format(
        "{} mismatch: ({} != {})", type, actual, expected);
    return false;
  }
  return true;
}

MATCHER_P(ReceiverAdded, expectedKey, "") {
  return isMatch(
             "Is receiver added event",
             std::holds_alternative<MergeChannelReceiverAdded>(arg.event),
             true,
             result_listener) &&
      isMatch("Key", arg.key, expectedKey, result_listener);
}

MATCHER_P(ReceiverRemoved, expectedKey, "") {
  return isMatch(
             "Is receiver removed event",
             std::holds_alternative<MergeChannelReceiverRemoved>(arg.event),
             true,
             result_listener) &&
      isMatch("Key", arg.key, expectedKey, result_listener);
}

MATCHER_P2(ValueReceived, expectedKey, expectedValue, "") {
  return isMatch(
             "Is value received event",
             std::holds_alternative<int>(arg.event),
             true,
             result_listener) &&
      isMatch("Key", arg.key, expectedKey, result_listener) &&
      isMatch(
             "Value", std::get<int>(arg.event), expectedValue, result_listener);
}

MATCHER_P(ReceiverClosed, expectedKey, "") {
  return isMatch(
             "Is receiver closed event",
             std::holds_alternative<MergeChannelReceiverClosed>(arg.event),
             true,
             result_listener) &&
      isMatch("Key", arg.key, expectedKey, result_listener) &&
      isMatch(
             "Exception present",
             !!std::get<MergeChannelReceiverClosed>(arg.event).exception,
             false,
             result_listener);
}

MATCHER_P(ReceiverClosedRuntimeError, expectedKey, "") {
  if (!std::holds_alternative<MergeChannelReceiverClosed>(arg.event)) {
    *result_listener << "Event is not for a closed receiver";
    return false;
  }
  return isMatch("Key", arg.key, expectedKey, result_listener) &&
      isMatch(
             "Exception present",
             !!std::get<MergeChannelReceiverClosed>(arg.event).exception,
             true,
             result_listener) &&
      isMatch(
             "Runtime error present",
             std::get<MergeChannelReceiverClosed>(arg.event)
                 .exception.template is_compatible_with<std::runtime_error>(),
             true,
             result_listener);
}

class MergeChannelFixture : public Test {
 protected:
  MergeChannelFixture() {}

  ~MergeChannelFixture() { executor_.drain(); }

  using TCallback = StrictMock<MockNextCallback<int>>;

  ChannelCallbackHandle processValues(
      Receiver<MergeChannelEvent<std::string, int>> receiver) {
    return consumeChannelWithCallback(
        std::move(receiver),
        &executor_,
        [=, this](Try<MergeChannelEvent<std::string, int>> resultTry)
            -> folly::coro::Task<bool> {
          if (resultTry.hasValue()) {
            std::visit(
                [](const auto& eventType) {
                  LOG(INFO) << "Type: " << typeid(eventType).name();
                },
                resultTry.value().event);
          }
          onNext_(std::move(resultTry));
          co_return true;
        });
  }

  folly::ManualExecutor executor_;
  StrictMock<MockNextCallback<MergeChannelEvent<std::string, int>>> onNext_;
};

TEST_F(MergeChannelFixture, ReceiveValues_ReturnMergedValues) {
  auto [receiver1, sender1] = Channel<int>::create();
  auto [receiver2, sender2] = Channel<int>::create();
  auto [receiver3, sender3] = Channel<int>::create();
  auto [mergedReceiver, mergeChannel] =
      createMergeChannel<std::string, int>(&executor_);

  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub1"s)));
  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub2"s)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub1"s, 1)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub2"s, 2)));
  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub3"s)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub1"s, 3)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub3"s, 5)));
  EXPECT_CALL(onNext_, onValue(ReceiverRemoved("sub1"s)));
  EXPECT_CALL(onNext_, onValue(ReceiverRemoved("sub2"s)));
  EXPECT_CALL(onNext_, onValue(ReceiverRemoved("sub3"s)));
  EXPECT_CALL(onNext_, onClosed());

  auto callbackHandle = processValues(std::move(mergedReceiver));

  mergeChannel.addNewReceiver("sub1", std::move(receiver1));
  mergeChannel.addNewReceiver("sub2", std::move(receiver2));
  executor_.drain();

  sender1.write(1);
  sender2.write(2);
  executor_.drain();

  mergeChannel.addNewReceiver("sub3", std::move(receiver3));
  mergeChannel.removeReceiver("sub2");

  sender1.write(3);
  sender2.write(4);
  sender3.write(5);
  executor_.drain();

  std::move(mergeChannel).close();
  executor_.drain();
}

TEST_F(
    MergeChannelFixture,
    ReceiveValues_NewInputReceiver_WithSameSubscriptionId) {
  auto [receiver1, sender1] = Channel<int>::create();
  auto [receiver2a, sender2a] = Channel<int>::create();
  auto [receiver2b, sender2b] = Channel<int>::create();
  auto [mergedReceiver, mergeChannel] =
      createMergeChannel<std::string, int>(&executor_);

  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub1"s)));
  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub2"s)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub1"s, 1)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub2"s, 2)));
  EXPECT_CALL(onNext_, onValue(ReceiverRemoved("sub2"s)));
  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub2"s))).RetiresOnSaturation();
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub1"s, 3)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub2"s, 5)));
  EXPECT_CALL(onNext_, onValue(ReceiverRemoved("sub1"s)));
  EXPECT_CALL(onNext_, onValue(ReceiverRemoved("sub2"s))).RetiresOnSaturation();
  EXPECT_CALL(onNext_, onClosed());

  auto callbackHandle = processValues(std::move(mergedReceiver));

  mergeChannel.addNewReceiver("sub1", std::move(receiver1));
  mergeChannel.addNewReceiver("sub2", std::move(receiver2a));
  executor_.drain();

  sender1.write(1);
  sender2a.write(2);
  executor_.drain();

  mergeChannel.addNewReceiver("sub2", std::move(receiver2b));

  sender1.write(3);
  sender2a.write(4);
  sender2b.write(5);
  executor_.drain();

  std::move(mergeChannel).close();
  executor_.drain();
}

TEST_F(
    MergeChannelFixture,
    ReceiveValues_NewInputReceiver_WithSameSubscriptionId_AfterClose) {
  auto [receiver1, sender1] = Channel<int>::create();
  auto [receiver2a, sender2a] = Channel<int>::create();
  auto [receiver2b, sender2b] = Channel<int>::create();
  auto [mergedReceiver, mergeChannel] =
      createMergeChannel<std::string, int>(&executor_);

  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub1"s)));
  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub2"s)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub1"s, 1)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub2"s, 2)));
  EXPECT_CALL(onNext_, onValue(ReceiverClosed("sub2"s)));
  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub2"s))).RetiresOnSaturation();
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub1"s, 3)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub2"s, 5)));
  EXPECT_CALL(onNext_, onValue(ReceiverRemoved("sub1"s)));
  EXPECT_CALL(onNext_, onValue(ReceiverRemoved("sub2"s)));
  EXPECT_CALL(onNext_, onClosed());

  auto callbackHandle = processValues(std::move(mergedReceiver));

  mergeChannel.addNewReceiver("sub1", std::move(receiver1));
  mergeChannel.addNewReceiver("sub2", std::move(receiver2a));
  executor_.drain();

  sender1.write(1);
  sender2a.write(2);
  executor_.drain();

  std::move(sender2a).close();
  executor_.drain();

  mergeChannel.addNewReceiver("sub2", std::move(receiver2b));
  executor_.drain();

  sender1.write(3);
  sender2b.write(5);
  executor_.drain();

  std::move(mergeChannel).close();
  executor_.drain();
}

TEST_F(MergeChannelFixture, ReceiveValues_RemoveReceiver) {
  auto [receiver1, sender1] = Channel<int>::create();
  auto [receiver2, sender2] = Channel<int>::create();
  auto [mergedReceiver, mergeChannel] =
      createMergeChannel<std::string, int>(&executor_);

  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub1"s)));
  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub2"s)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub1"s, 1)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub2"s, 2)));
  EXPECT_CALL(onNext_, onValue(ReceiverRemoved("sub2"s)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub1"s, 3)));
  EXPECT_CALL(onNext_, onValue(ReceiverRemoved("sub1"s)));
  EXPECT_CALL(onNext_, onClosed());

  auto callbackHandle = processValues(std::move(mergedReceiver));

  mergeChannel.addNewReceiver("sub1", std::move(receiver1));
  mergeChannel.addNewReceiver("sub2", std::move(receiver2));
  executor_.drain();

  sender1.write(1);
  sender2.write(2);
  executor_.drain();

  mergeChannel.removeReceiver("sub2");

  sender1.write(3);
  sender2.write(4);
  executor_.drain();

  std::move(mergeChannel).close();
  executor_.drain();
}

TEST_F(MergeChannelFixture, ReceiveValues_RemoveReceiver_AfterClose) {
  auto [receiver1, sender1] = Channel<int>::create();
  auto [receiver2, sender2] = Channel<int>::create();
  auto [mergedReceiver, mergeChannel] =
      createMergeChannel<std::string, int>(&executor_);

  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub1"s)));
  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub2"s)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub1"s, 1)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub2"s, 2)));
  EXPECT_CALL(onNext_, onValue(ReceiverClosed("sub2"s)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub1"s, 3)));
  EXPECT_CALL(onNext_, onValue(ReceiverRemoved("sub1"s)));
  EXPECT_CALL(onNext_, onClosed());

  auto callbackHandle = processValues(std::move(mergedReceiver));

  mergeChannel.addNewReceiver("sub1", std::move(receiver1));
  mergeChannel.addNewReceiver("sub2", std::move(receiver2));
  executor_.drain();

  sender1.write(1);
  sender2.write(2);
  executor_.drain();

  std::move(sender2).close();
  executor_.drain();

  mergeChannel.removeReceiver("sub2");
  sender1.write(3);
  executor_.drain();

  std::move(mergeChannel).close();
  executor_.drain();
}

TEST_F(MergeChannelFixture, OneInputClosed_ContinuesMerging) {
  auto [receiver1, sender1] = Channel<int>::create();
  auto [receiver2, sender2] = Channel<int>::create();
  auto [receiver3, sender3] = Channel<int>::create();
  auto [mergedReceiver, mergeChannel] =
      createMergeChannel<std::string, int>(&executor_);

  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub1"s)));
  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub2"s)));
  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub3"s)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub1"s, 1)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub2"s, 2)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub3"s, 3)));
  EXPECT_CALL(onNext_, onValue(ReceiverClosed("sub3"s)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub1"s, 4)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub2"s, 5)));
  EXPECT_CALL(onNext_, onValue(ReceiverClosed("sub1"s)));
  EXPECT_CALL(onNext_, onValue(ReceiverClosed("sub2"s)));
  EXPECT_CALL(onNext_, onClosed());

  auto callbackHandle = processValues(std::move(mergedReceiver));

  mergeChannel.addNewReceiver("sub1", std::move(receiver1));
  mergeChannel.addNewReceiver("sub2", std::move(receiver2));
  mergeChannel.addNewReceiver("sub3", std::move(receiver3));
  executor_.drain();

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

  std::move(mergeChannel).close();
  executor_.drain();
}

TEST_F(MergeChannelFixture, OneInputThrows_ContinuesMerging) {
  auto [receiver1, sender1] = Channel<int>::create();
  auto [receiver2, sender2] = Channel<int>::create();
  auto [receiver3, sender3] = Channel<int>::create();
  auto [mergedReceiver, mergeChannel] =
      createMergeChannel<std::string, int>(&executor_);

  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub1"s)));
  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub2"s)));
  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub3"s)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub1"s, 1)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub2"s, 2)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub3"s, 3)));
  EXPECT_CALL(onNext_, onValue(ReceiverClosedRuntimeError("sub3"s)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub1"s, 4)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub2"s, 5)));
  EXPECT_CALL(onNext_, onValue(ReceiverClosed("sub1"s)));
  EXPECT_CALL(onNext_, onValue(ReceiverClosed("sub2"s)));
  EXPECT_CALL(onNext_, onClosed());

  auto callbackHandle = processValues(std::move(mergedReceiver));

  mergeChannel.addNewReceiver("sub1", std::move(receiver1));
  mergeChannel.addNewReceiver("sub2", std::move(receiver2));
  mergeChannel.addNewReceiver("sub3", std::move(receiver3));
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

  std::move(mergeChannel).close();
  executor_.drain();
}

TEST_F(MergeChannelFixture, Cancelled) {
  auto [receiver1, sender1] = Channel<int>::create();
  auto [receiver2, sender2] = Channel<int>::create();
  auto [receiver3, sender3] = Channel<int>::create();
  auto [mergedReceiver, mergeChannel] =
      createMergeChannel<std::string, int>(&executor_);

  mergeChannel.addNewReceiver("sub1", std::move(receiver1));
  mergeChannel.addNewReceiver("sub2", std::move(receiver2));
  mergeChannel.addNewReceiver("sub3", std::move(receiver3));
  executor_.drain();

  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub1"s)));
  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub2"s)));
  EXPECT_CALL(onNext_, onValue(ReceiverAdded("sub3"s)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub1"s, 1)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub2"s, 2)));
  EXPECT_CALL(onNext_, onValue(ValueReceived("sub3"s, 3)));
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

struct ProducedValue {
  int producerIndex;
  int value;
};

class MergeChannelFixtureStress : public Test {
 protected:
  MergeChannelFixtureStress()
      : producers_(toVector(makeProducer(0), makeProducer(1))),
        consumer_(makeConsumer()) {}

  static std::unique_ptr<StressTestProducer<ProducedValue>> makeProducer(
      int index) {
    return std::make_unique<StressTestProducer<ProducedValue>>(
        [index, value = 0]() mutable { return ProducedValue{index, value++}; });
  }

  static std::unique_ptr<
      StressTestConsumer<MergeChannelEvent<int, ProducedValue>>>
  makeConsumer() {
    return std::make_unique<
        StressTestConsumer<MergeChannelEvent<int, ProducedValue>>>(
        ConsumptionMode::CallbackWithHandle,
        [lastReceived = toVector(-1, -1, -1)](
            MergeChannelEvent<int, ProducedValue> result) mutable {
          if (std::holds_alternative<ProducedValue>(result.event)) {
            auto value = std::get<ProducedValue>(result.event);
            EXPECT_EQ(value.value, ++lastReceived[value.producerIndex]);
          }
        });
  }

  static void sleepFor(std::chrono::milliseconds duration) {
    /* sleep override */
    std::this_thread::sleep_for(duration);
  }

  static constexpr std::chrono::milliseconds kTestTimeout =
      std::chrono::milliseconds{5000};

  std::vector<std::unique_ptr<StressTestProducer<ProducedValue>>> producers_;
  std::unique_ptr<StressTestConsumer<MergeChannelEvent<int, ProducedValue>>>
      consumer_;
};

TEST_F(MergeChannelFixtureStress, HandleClosed) {
  folly::CPUThreadPoolExecutor mergeChannelExecutor(1);
  auto [mergeReceiver, mergeChannel] = createMergeChannel<int, ProducedValue>(
      folly::SerialExecutor::create(&mergeChannelExecutor));
  consumer_->startConsuming(std::move(mergeReceiver));

  auto [receiver0, sender0] = Channel<ProducedValue>::create();
  producers_.at(0)->startProducing(std::move(sender0), std::nullopt);
  mergeChannel.addNewReceiver(0 /* subscriptionId */, std::move(receiver0));

  sleepFor(kTestTimeout / 3);

  auto [receiver1, sender1] = Channel<ProducedValue>::create();
  producers_.at(1)->startProducing(std::move(sender1), std::nullopt);
  mergeChannel.addNewReceiver(1 /* subscriptionId */, std::move(receiver1));

  sleepFor(kTestTimeout / 3);

  mergeChannel.removeReceiver(0 /* subscriptionId */);

  sleepFor(kTestTimeout / 3);

  std::move(mergeChannel).close();
  EXPECT_EQ(consumer_->waitForClose().get(), CloseType::NoException);
}

TEST_F(MergeChannelFixtureStress, Cancelled) {
  folly::CPUThreadPoolExecutor mergeChannelExecutor(1);
  auto [mergeReceiver, mergeChannel] = createMergeChannel<int, ProducedValue>(
      folly::SerialExecutor::create(&mergeChannelExecutor));
  consumer_->startConsuming(std::move(mergeReceiver));

  auto [receiver0, sender0] = Channel<ProducedValue>::create();
  producers_.at(0)->startProducing(std::move(sender0), std::nullopt);
  mergeChannel.addNewReceiver(0 /* subscriptionId */, std::move(receiver0));

  sleepFor(kTestTimeout / 3);

  auto [receiver1, sender1] = Channel<ProducedValue>::create();
  producers_.at(1)->startProducing(std::move(sender1), std::nullopt);
  mergeChannel.addNewReceiver(1 /* subscriptionId */, std::move(receiver1));

  sleepFor(kTestTimeout / 3);

  mergeChannel.removeReceiver(0 /* subscriptionId */);

  sleepFor(kTestTimeout / 3);

  consumer_->cancel();
  EXPECT_EQ(consumer_->waitForClose().get(), CloseType::Cancelled);
}

TEST_F(MergeChannelFixtureStress, Cancelled_ThenHandleClosedImmediately) {
  folly::CPUThreadPoolExecutor mergeChannelExecutor(1);
  auto [mergeReceiver, mergeChannel] = createMergeChannel<int, ProducedValue>(
      folly::SerialExecutor::create(&mergeChannelExecutor));
  consumer_->startConsuming(std::move(mergeReceiver));

  auto [receiver0, sender0] = Channel<ProducedValue>::create();
  producers_.at(0)->startProducing(std::move(sender0), std::nullopt);
  mergeChannel.addNewReceiver(0 /* subscriptionId */, std::move(receiver0));

  sleepFor(kTestTimeout / 3);

  auto [receiver1, sender1] = Channel<ProducedValue>::create();
  producers_.at(1)->startProducing(std::move(sender1), std::nullopt);
  mergeChannel.addNewReceiver(1 /* subscriptionId */, std::move(receiver1));

  sleepFor(kTestTimeout / 3);

  mergeChannel.removeReceiver(0 /* subscriptionId */);

  sleepFor(kTestTimeout / 3);

  consumer_->cancel();
  std::move(mergeChannel).close();
  EXPECT_THAT(
      consumer_->waitForClose().get(),
      AnyOf(Eq(CloseType::NoException), Eq(CloseType::Cancelled)));
}

TEST_F(MergeChannelFixtureStress, HandleClosed_ThenCancelledImmediately) {
  folly::CPUThreadPoolExecutor mergeChannelExecutor(1);
  auto [mergeReceiver, mergeChannel] = createMergeChannel<int, ProducedValue>(
      folly::SerialExecutor::create(&mergeChannelExecutor));
  consumer_->startConsuming(std::move(mergeReceiver));

  auto [receiver0, sender0] = Channel<ProducedValue>::create();
  producers_.at(0)->startProducing(std::move(sender0), std::nullopt);
  mergeChannel.addNewReceiver(0 /* subscriptionId */, std::move(receiver0));

  sleepFor(kTestTimeout / 3);

  auto [receiver1, sender1] = Channel<ProducedValue>::create();
  producers_.at(1)->startProducing(std::move(sender1), std::nullopt);
  mergeChannel.addNewReceiver(1 /* subscriptionId */, std::move(receiver1));

  sleepFor(kTestTimeout / 3);

  mergeChannel.removeReceiver(0 /* subscriptionId */);

  sleepFor(kTestTimeout / 3);

  std::move(mergeChannel).close();
  consumer_->cancel();
  EXPECT_THAT(
      consumer_->waitForClose().get(),
      AnyOf(Eq(CloseType::NoException), Eq(CloseType::Cancelled)));
}
} // namespace channels
} // namespace folly
