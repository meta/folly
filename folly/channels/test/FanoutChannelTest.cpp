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
#include <folly/channels/FanoutChannel.h>
#include <folly/channels/test/ChannelTestUtil.h>
#include <folly/channels/testing/MockNextCallback.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/executors/SerialExecutor.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

namespace folly {
namespace channels {

using namespace testing;

class FanoutChannelFixture : public Test {
 protected:
  FanoutChannelFixture() {}

  ~FanoutChannelFixture() { executor_.drain(); }

  template <typename T>
  using Callback = StrictMock<MockNextCallback<T>>;

  template <typename T>
  std::pair<ChannelCallbackHandle, Callback<T>*> processValues(
      Receiver<T> receiver) {
    auto callback = std::make_unique<Callback<T>>();
    auto callbackPtr = callback.get();
    auto handle = consumeChannelWithCallback(
        std::move(receiver),
        &executor_,
        [cbk = std::move(callback)](
            Try<T> resultTry) mutable -> folly::coro::Task<bool> {
          (*cbk)(std::move(resultTry));
          co_return true;
        });
    return std::make_pair(std::move(handle), callbackPtr);
  }

  StrictMock<MockNextCallback<std::string>> createCallback() {
    return StrictMock<MockNextCallback<std::string>>();
  }

  folly::ManualExecutor executor_;
};

TEST_F(FanoutChannelFixture, ReceiveValue_FanoutBroadcastsValues) {
  struct LatestVersion {
    int version{-1};
    size_t numSubscribers{0};

    void update(int& newVersion, size_t newNumSubscribers) {
      version = newVersion;
      numSubscribers = newNumSubscribers;
    }
  };

  auto [inputReceiver, sender] = Channel<int>::create();
  auto fanoutChannel = createFanoutChannel(
      std::move(inputReceiver), &executor_, LatestVersion());

  EXPECT_FALSE(fanoutChannel.anySubscribers());

  auto [handle1, callback1] = processValues(fanoutChannel.subscribe(
      [](const auto&) { return toVector(100); } /* getInitialValues */));
  auto [handle2, callback2] = processValues(fanoutChannel.subscribe(
      [](const auto&) { return toVector(200); } /* getInitialValues */));

  EXPECT_TRUE(fanoutChannel.anySubscribers());
  EXPECT_CALL(*callback1, onValue(100));
  EXPECT_CALL(*callback2, onValue(200));
  executor_.drain();

  EXPECT_CALL(*callback1, onValue(1));
  EXPECT_CALL(*callback2, onValue(1));
  EXPECT_CALL(*callback1, onValue(2));
  EXPECT_CALL(*callback2, onValue(2));
  sender.write(1);
  sender.write(2);
  executor_.drain();

  EXPECT_EQ(fanoutChannel.getContext().version, 2);

  auto [handle3, callback3] = processValues(
      fanoutChannel.subscribe([](const LatestVersion& latestVersion) {
        EXPECT_EQ(latestVersion.numSubscribers, 2);
        return toVector(latestVersion.version);
      } /* getInitialValues */));

  EXPECT_CALL(*callback3, onValue(2));
  executor_.drain();

  sender.write(3);
  EXPECT_CALL(*callback1, onValue(3));
  EXPECT_CALL(*callback2, onValue(3));
  EXPECT_CALL(*callback3, onValue(3));

  std::move(sender).close();
  EXPECT_CALL(*callback1, onClosed());
  EXPECT_CALL(*callback2, onClosed());
  EXPECT_CALL(*callback3, onClosed());
  executor_.drain();

  EXPECT_FALSE(fanoutChannel.anySubscribers());
}

TEST_F(FanoutChannelFixture, InputClosed_AllOutputReceiversClose) {
  auto [inputReceiver, sender] = Channel<int>::create();
  auto fanoutChannel =
      createFanoutChannel(std::move(inputReceiver), &executor_);

  auto [handle1, callback1] = processValues(fanoutChannel.subscribe());
  auto [handle2, callback2] = processValues(fanoutChannel.subscribe());

  EXPECT_CALL(*callback1, onValue(1));
  EXPECT_CALL(*callback2, onValue(1));
  EXPECT_CALL(*callback1, onClosed());
  EXPECT_CALL(*callback2, onClosed());

  executor_.drain();

  EXPECT_TRUE(fanoutChannel.anySubscribers());

  sender.write(1);
  executor_.drain();

  std::move(sender).close();
  executor_.drain();

  EXPECT_FALSE(fanoutChannel.anySubscribers());
}

TEST_F(FanoutChannelFixture, InputThrows_AllOutputReceiversGetException) {
  auto [inputReceiver, sender] = Channel<int>::create();
  auto fanoutChannel =
      createFanoutChannel(std::move(inputReceiver), &executor_);

  auto [handle1, callback1] = processValues(fanoutChannel.subscribe());
  auto [handle2, callback2] = processValues(fanoutChannel.subscribe());

  EXPECT_CALL(*callback1, onValue(1));
  EXPECT_CALL(*callback2, onValue(1));
  EXPECT_CALL(*callback1, onRuntimeError("std::runtime_error: Error"));
  EXPECT_CALL(*callback2, onRuntimeError("std::runtime_error: Error"));

  executor_.drain();

  EXPECT_TRUE(fanoutChannel.anySubscribers());

  sender.write(1);
  executor_.drain();

  std::move(sender).close(std::runtime_error("Error"));
  executor_.drain();

  EXPECT_FALSE(fanoutChannel.anySubscribers());
}

TEST_F(FanoutChannelFixture, ReceiversCancelled) {
  auto [inputReceiver, sender] = Channel<int>::create();
  auto fanoutChannel =
      createFanoutChannel(std::move(inputReceiver), &executor_);

  auto [handle1, callback1] = processValues(fanoutChannel.subscribe());
  auto [handle2, callback2] = processValues(fanoutChannel.subscribe());

  EXPECT_CALL(*callback1, onValue(1));
  EXPECT_CALL(*callback2, onValue(1));
  EXPECT_CALL(*callback1, onCancelled());
  EXPECT_CALL(*callback2, onValue(2));
  EXPECT_CALL(*callback2, onCancelled());

  executor_.drain();

  EXPECT_TRUE(fanoutChannel.anySubscribers());

  sender.write(1);
  executor_.drain();

  EXPECT_TRUE(fanoutChannel.anySubscribers());

  handle1.reset();
  sender.write(2);
  executor_.drain();

  EXPECT_TRUE(fanoutChannel.anySubscribers());

  handle2.reset();
  sender.write(3);
  executor_.drain();

  EXPECT_FALSE(fanoutChannel.anySubscribers());

  std::move(sender).close();
  executor_.drain();

  EXPECT_FALSE(fanoutChannel.anySubscribers());
}

TEST_F(FanoutChannelFixture, SubscribersClosed) {
  auto [inputReceiver, sender] = Channel<int>::create();
  auto fanoutChannel =
      createFanoutChannel(std::move(inputReceiver), &executor_);

  auto [handle1, callback1] = processValues(fanoutChannel.subscribe());
  auto [handle2, callback2] = processValues(fanoutChannel.subscribe());
  executor_.drain();

  EXPECT_TRUE(fanoutChannel.anySubscribers());

  EXPECT_CALL(*callback1, onValue(1));
  EXPECT_CALL(*callback2, onValue(1));
  sender.write(1);
  executor_.drain();

  EXPECT_TRUE(fanoutChannel.anySubscribers());

  EXPECT_CALL(*callback1, onClosed());
  EXPECT_CALL(*callback2, onClosed());
  fanoutChannel.closeSubscribers();
  executor_.drain();

  EXPECT_FALSE(fanoutChannel.anySubscribers());

  auto [handle3, callback3] = processValues(fanoutChannel.subscribe());
  executor_.drain();

  EXPECT_TRUE(fanoutChannel.anySubscribers());

  EXPECT_CALL(*callback3, onValue(2));
  sender.write(2);
  executor_.drain();

  EXPECT_CALL(*callback3, onClosed());
  std::move(fanoutChannel).close();
  executor_.drain();
}

TEST_F(FanoutChannelFixture, VectorBool) {
  auto [inputReceiver, sender] = Channel<bool>::create();
  auto fanoutChannel =
      createFanoutChannel(std::move(inputReceiver), &executor_);

  auto [handle1, callback1] = processValues(fanoutChannel.subscribe(
      [](const auto&) { return toVector(true); } /* getInitialValues */));
  auto [handle2, callback2] = processValues(fanoutChannel.subscribe(
      [](const auto&) { return toVector(false); } /* getInitialValues */));

  EXPECT_CALL(*callback1, onValue(true));
  EXPECT_CALL(*callback2, onValue(false));

  executor_.drain();

  EXPECT_TRUE(fanoutChannel.anySubscribers());

  EXPECT_CALL(*callback1, onValue(true));
  EXPECT_CALL(*callback2, onValue(true));
  EXPECT_CALL(*callback1, onValue(false));
  EXPECT_CALL(*callback2, onValue(false));

  EXPECT_CALL(*callback1, onClosed());
  EXPECT_CALL(*callback2, onClosed());

  sender.write(true);
  sender.write(false);
  executor_.drain();

  std::move(sender).close();
  executor_.drain();

  EXPECT_FALSE(fanoutChannel.anySubscribers());
}

class FanoutChannelFixtureStress : public Test {
 protected:
  FanoutChannelFixtureStress()
      : producer_(makeProducer()),
        consumers_(toVector(makeConsumer(), makeConsumer(), makeConsumer())) {}

  static std::unique_ptr<StressTestProducer<int>> makeProducer() {
    return std::make_unique<StressTestProducer<int>>([value = 0]() mutable {
      return value++;
    });
  }

  static std::unique_ptr<StressTestConsumer<int>> makeConsumer() {
    return std::make_unique<StressTestConsumer<int>>(
        ConsumptionMode::CallbackWithHandle,
        [lastReceived = -1](int value) mutable {
          if (lastReceived == -1) {
            lastReceived = value;
          } else {
            EXPECT_EQ(value, ++lastReceived);
          }
        });
  }

  static void sleepFor(std::chrono::milliseconds duration) {
    /* sleep override */
    std::this_thread::sleep_for(duration);
  }

  static constexpr std::chrono::milliseconds kTestTimeout =
      std::chrono::milliseconds{10};

  std::unique_ptr<StressTestProducer<int>> producer_;
  std::vector<std::unique_ptr<StressTestConsumer<int>>> consumers_;
};

TEST_F(FanoutChannelFixtureStress, HandleClosed) {
  auto [receiver, sender] = Channel<int>::create();
  producer_->startProducing(std::move(sender), std::nullopt /* closeEx */);

  folly::CPUThreadPoolExecutor fanoutChannelExecutor(1);
  auto fanoutChannel = createFanoutChannel(
      std::move(receiver),
      folly::SerialExecutor::create(&fanoutChannelExecutor));

  consumers_.at(0)->startConsuming(fanoutChannel.subscribe());
  consumers_.at(1)->startConsuming(fanoutChannel.subscribe());

  sleepFor(kTestTimeout / 3);

  consumers_.at(2)->startConsuming(fanoutChannel.subscribe());

  sleepFor(kTestTimeout / 3);

  consumers_.at(0)->cancel();
  EXPECT_EQ(consumers_.at(0)->waitForClose().get(), CloseType::Cancelled);

  sleepFor(kTestTimeout / 3);

  std::move(fanoutChannel).close();
  EXPECT_EQ(consumers_.at(1)->waitForClose().get(), CloseType::NoException);
  EXPECT_EQ(consumers_.at(2)->waitForClose().get(), CloseType::NoException);
}

TEST_F(FanoutChannelFixtureStress, InputChannelClosed) {
  auto [receiver, sender] = Channel<int>::create();
  producer_->startProducing(std::move(sender), std::nullopt /* closeEx */);

  folly::CPUThreadPoolExecutor fanoutChannelExecutor(1);
  auto fanoutChannel = createFanoutChannel(
      std::move(receiver),
      folly::SerialExecutor::create(&fanoutChannelExecutor));

  consumers_.at(0)->startConsuming(fanoutChannel.subscribe());
  consumers_.at(1)->startConsuming(fanoutChannel.subscribe());

  sleepFor(kTestTimeout / 3);

  consumers_.at(2)->startConsuming(fanoutChannel.subscribe());

  sleepFor(kTestTimeout / 3);

  consumers_.at(0)->cancel();
  EXPECT_EQ(consumers_.at(0)->waitForClose().get(), CloseType::Cancelled);

  sleepFor(kTestTimeout / 3);

  producer_->stopProducing();
  EXPECT_EQ(consumers_.at(1)->waitForClose().get(), CloseType::NoException);
  EXPECT_EQ(consumers_.at(2)->waitForClose().get(), CloseType::NoException);
}
} // namespace channels
} // namespace folly
