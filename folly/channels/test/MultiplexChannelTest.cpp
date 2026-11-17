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

#include <algorithm>
#include <string>
#include <vector>

#include <folly/channels/ConsumeChannel.h>
#include <folly/channels/MaxConcurrentRateLimiter.h>
#include <folly/channels/MultiplexChannel.h>
#include <folly/channels/test/ChannelTestUtil.h>
#include <folly/channels/testing/MockNextCallback.h>
#include <folly/coro/BlockingWait.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/executors/SerialExecutor.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

namespace folly {
namespace channels {

using namespace testing;
using namespace std::string_literals;

class MultiplexChannelFixture : public Test {
 protected:
  MultiplexChannelFixture() {}

  ~MultiplexChannelFixture() { executor_.drain(); }

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

  folly::ManualExecutor executor_;
};

static constexpr int kCloseSubscription = -1;

struct TestInputValue {
  folly::F14FastMap<std::string, int> values;
};

struct TestContext {
  std::string contextValue;
};

struct TestSubscriptionArg {
  int initialValue;
  bool firstSubscriptionForKey;
  bool throwException{false};
  folly::SemiFuture<Unit> waitForSubscription{folly::makeSemiFuture()};
};

struct TestMultiplexer {
 public:
  TestMultiplexer(
      folly::Executor::KeepAlive<folly::SequencedExecutor> executor,
      std::shared_ptr<RateLimiter> rateLimiter = nullptr)
      : executor_(std::move(executor)), rateLimiter_(std::move(rateLimiter)) {}

  folly::Executor::KeepAlive<folly::SequencedExecutor> getExecutor() {
    return executor_;
  }

  folly::coro::Task<std::vector<int>> onNewSubscription(
      std::string key,
      TestContext& subscriptionContext,
      TestSubscriptionArg subscriptionArg) {
    co_await std::move(subscriptionArg.waitForSubscription);
    if (subscriptionArg.throwException) {
      throw std::runtime_error("Error");
    }
    if (subscriptionArg.firstSubscriptionForKey) {
      subscriptionContext.contextValue = key;
    }
    EXPECT_EQ(subscriptionContext.contextValue, key);
    co_return toVector(subscriptionArg.initialValue);
  }

  folly::coro::Task<void> onInputValue(
      Try<TestInputValue> inputValue,
      MultiplexedSubscriptions<TestMultiplexer>& subscriptions) {
    for (auto& [key, value] : inputValue.value().values) {
      if (!subscriptions.hasSubscription(key)) {
        continue;
      }
      if (value == kCloseSubscription) {
        subscriptions.close(key, {} /* ex */);
      } else {
        subscriptions.write(key, value);
      }
    }
    co_return;
  }

  std::shared_ptr<RateLimiter> getRateLimiter() { return rateLimiter_; }

 private:
  folly::Executor::KeepAlive<folly::SequencedExecutor> executor_;
  std::shared_ptr<RateLimiter> rateLimiter_;
};

struct KeysRecordingMultiplexer : TestMultiplexer {
  KeysRecordingMultiplexer(
      folly::Executor::KeepAlive<folly::SequencedExecutor> executor,
      std::vector<std::string>& keys)
      : TestMultiplexer(std::move(executor)), keys_(&keys) {}

  folly::coro::Task<void> onInputValue(
      Try<TestInputValue> /* inputValue */,
      MultiplexedSubscriptions<KeysRecordingMultiplexer>& subscriptions) {
    keys_->clear();
    for (const auto& key : subscriptions.getAllSubscriptionKeys()) {
      keys_->push_back(key);
    }
    co_return;
  }

 private:
  std::vector<std::string>* keys_ = nullptr;
};

TEST_F(MultiplexChannelFixture, ReceiveValues_MultiplexesValues) {
  auto [inputReceiver, inputSender] = Channel<TestInputValue>::create();
  auto multiplexChannel = createMultiplexChannel(
      TestMultiplexer(&executor_), std::move(inputReceiver));

  EXPECT_FALSE(multiplexChannel.anySubscribers());

  auto [handle1a, callback1a] = processValues(multiplexChannel.subscribe(
      "one"s,
      TestSubscriptionArg{
          100 /* initialValue */, true /* firstSubscriptionForKey */}));
  auto [handle1b, callback1b] = processValues(multiplexChannel.subscribe(
      "one"s,
      TestSubscriptionArg{
          101 /* initialValue */, false /* firstSubscriptionForKey */}));
  auto [handle2, callback2] = processValues(multiplexChannel.subscribe(
      "two"s,
      TestSubscriptionArg{
          200 /* initialValue */, true /* firstSubscriptionForKey */}));

  EXPECT_TRUE(multiplexChannel.anySubscribers());
  EXPECT_CALL(*callback1a, onValue(100));
  EXPECT_CALL(*callback1b, onValue(101));
  EXPECT_CALL(*callback2, onValue(200));
  executor_.drain();

  EXPECT_CALL(*callback1a, onValue(110));
  EXPECT_CALL(*callback1b, onValue(110));
  inputSender.write(TestInputValue{toMap(std::make_pair("one"s, 110))});
  executor_.drain();

  EXPECT_CALL(*callback2, onValue(210));
  inputSender.write(TestInputValue{toMap(std::make_pair("two"s, 210))});
  executor_.drain();

  EXPECT_CALL(*callback1a, onValue(120));
  EXPECT_CALL(*callback1b, onValue(120));
  EXPECT_CALL(*callback2, onValue(220));
  inputSender.write(
      TestInputValue{toMap(
          std::make_pair("one"s, 120),
          std::make_pair("two"s, 220),
          std::make_pair("three", 330))});
  executor_.drain();

  std::move(inputSender).close();
  EXPECT_CALL(*callback1a, onClosed());
  EXPECT_CALL(*callback1b, onClosed());
  EXPECT_CALL(*callback2, onClosed());
  executor_.drain();

  EXPECT_FALSE(multiplexChannel.anySubscribers());
}

TEST_F(MultiplexChannelFixture, GetAllSubscriptionKeys) {
  auto [inputReceiver, inputSender] = Channel<TestInputValue>::create();
  auto keys = std::vector<std::string>();
  auto multiplexChannel = createMultiplexChannel(
      KeysRecordingMultiplexer(&executor_, keys), std::move(inputReceiver));

  auto [handle1, callback1] = processValues(multiplexChannel.subscribe(
      "one"s,
      TestSubscriptionArg{
          100 /* initialValue */, true /* firstSubscriptionForKey */}));
  auto [handle2, callback2] = processValues(multiplexChannel.subscribe(
      "two"s,
      TestSubscriptionArg{
          200 /* initialValue */, true /* firstSubscriptionForKey */}));

  EXPECT_CALL(*callback1, onValue(100));
  EXPECT_CALL(*callback2, onValue(200));
  executor_.drain();

  inputSender.write(TestInputValue{});
  executor_.drain();

  std::sort(keys.begin(), keys.end());
  EXPECT_EQ(keys, (std::vector<std::string>{"one"s, "two"s}));

  std::move(inputSender).close();
  EXPECT_CALL(*callback1, onClosed());
  EXPECT_CALL(*callback2, onClosed());
  executor_.drain();
}

TEST_F(MultiplexChannelFixture, InputThrows_AllOutputReceiversGetException) {
  auto [inputReceiver, inputSender] = Channel<TestInputValue>::create();
  auto multiplexChannel = createMultiplexChannel(
      TestMultiplexer(&executor_), std::move(inputReceiver));

  auto [handle1a, callback1a] = processValues(multiplexChannel.subscribe(
      "one"s,
      TestSubscriptionArg{
          100 /* initialValue */, true /* firstSubscriptionForKey */}));
  auto [handle1b, callback1b] = processValues(multiplexChannel.subscribe(
      "one"s,
      TestSubscriptionArg{
          101 /* initialValue */, false /* firstSubscriptionForKey */}));
  auto [handle2, callback2] = processValues(multiplexChannel.subscribe(
      "two"s,
      TestSubscriptionArg{
          200 /* initialValue */, true /* firstSubscriptionForKey */}));

  EXPECT_CALL(*callback1a, onValue(100));
  EXPECT_CALL(*callback1b, onValue(101));
  EXPECT_CALL(*callback2, onValue(200));
  executor_.drain();

  std::move(inputSender)
      .close(folly::make_exception_wrapper<std::runtime_error>("Error"));
  EXPECT_CALL(*callback1a, onRuntimeError("std::runtime_error: Error"));
  EXPECT_CALL(*callback1b, onRuntimeError("std::runtime_error: Error"));
  EXPECT_CALL(*callback2, onRuntimeError("std::runtime_error: Error"));
  executor_.drain();
}

TEST_F(MultiplexChannelFixture, ClearUnusedSubscriptions) {
  auto [inputReceiver, inputSender] = Channel<TestInputValue>::create();
  auto multiplexChannel = createMultiplexChannel(
      TestMultiplexer(&executor_), std::move(inputReceiver));

  EXPECT_FALSE(multiplexChannel.anySubscribers());

  auto [handle1a, callback1a] = processValues(multiplexChannel.subscribe(
      "one"s,
      TestSubscriptionArg{
          100 /* initialValue */, true /* firstSubscriptionForKey */}));
  auto [handle1b, callback1b] = processValues(multiplexChannel.subscribe(
      "one"s,
      TestSubscriptionArg{
          101 /* initialValue */, false /* firstSubscriptionForKey */}));
  auto [handle2, callback2] = processValues(multiplexChannel.subscribe(
      "two"s,
      TestSubscriptionArg{
          200 /* initialValue */, true /* firstSubscriptionForKey */}));

  EXPECT_TRUE(multiplexChannel.anySubscribers());

  EXPECT_CALL(*callback1a, onValue(100));
  EXPECT_CALL(*callback1b, onValue(101));
  EXPECT_CALL(*callback2, onValue(200));
  executor_.drain();

  auto clearedSubscriptions1Task =
      co_withExecutor(&executor_, multiplexChannel.clearUnusedSubscriptions())
          .start();
  executor_.drain();
  auto clearedSubscriptions1 =
      folly::coro::blockingWait(std::move(clearedSubscriptions1Task));

  EXPECT_TRUE(clearedSubscriptions1.empty());

  EXPECT_CALL(*callback1a, onCancelled());
  EXPECT_CALL(*callback2, onCancelled());
  handle1a.reset();
  handle2.reset();
  executor_.drain();

  auto clearedSubscriptions2Task =
      co_withExecutor(&executor_, multiplexChannel.clearUnusedSubscriptions())
          .start();
  executor_.drain();
  auto clearedSubscriptions2 =
      folly::coro::blockingWait(std::move(clearedSubscriptions2Task));

  EXPECT_EQ(clearedSubscriptions2.size(), 1);
  EXPECT_EQ(clearedSubscriptions2[0].first, "two"s);
  EXPECT_EQ(clearedSubscriptions2[0].second.contextValue, "two"s);

  EXPECT_CALL(*callback1b, onCancelled());
  handle1b.reset();
  executor_.drain();

  EXPECT_TRUE(multiplexChannel.anySubscribers());

  auto clearedSubscriptions3Task =
      co_withExecutor(&executor_, multiplexChannel.clearUnusedSubscriptions())
          .start();
  executor_.drain();
  auto clearedSubscriptions3 =
      folly::coro::blockingWait(std::move(clearedSubscriptions3Task));

  EXPECT_EQ(clearedSubscriptions3.size(), 1);
  EXPECT_EQ(clearedSubscriptions3[0].first, "one"s);
  EXPECT_EQ(clearedSubscriptions3[0].second.contextValue, "one"s);
  EXPECT_FALSE(multiplexChannel.anySubscribers());
}

TEST_F(MultiplexChannelFixture, OnNewSubscriptionThrows_OutputReceiverClosed) {
  auto [inputReceiver, inputSender] = Channel<TestInputValue>::create();
  auto multiplexChannel = createMultiplexChannel(
      TestMultiplexer(&executor_), std::move(inputReceiver));

  auto [handle1a, callback1a] = processValues(multiplexChannel.subscribe(
      "one"s,
      TestSubscriptionArg{
          100 /* initialValue */,
          true /* firstSubscriptionForKey */,
          false /* throwException */}));

  EXPECT_CALL(*callback1a, onValue(100));
  executor_.drain();

  auto [handle1b, callback1b] = processValues(multiplexChannel.subscribe(
      "one"s,
      TestSubscriptionArg{
          101 /* initialValue */,
          false /* firstSubscriptionForKey */,
          true /* throwException */}));

  EXPECT_CALL(*callback1b, onRuntimeError("std::runtime_error: Error"));
  executor_.drain();

  inputSender.write(TestInputValue{toMap(std::make_pair("one"s, 110))});

  EXPECT_CALL(*callback1a, onValue(110));
  executor_.drain();

  std::move(inputSender).close();
  EXPECT_CALL(*callback1a, onClosed());
  executor_.drain();
}

TEST_F(MultiplexChannelFixture, HandleDestroyed) {
  auto [inputReceiver, inputSender] = Channel<TestInputValue>::create();
  auto multiplexChannel = createMultiplexChannel(
      TestMultiplexer(&executor_), std::move(inputReceiver));

  EXPECT_FALSE(multiplexChannel.anySubscribers());

  auto [handle1a, callback1a] = processValues(multiplexChannel.subscribe(
      "one"s,
      TestSubscriptionArg{
          100 /* initialValue */,
          true /* firstSubscriptionForKey */,
          false /* throwException */}));

  EXPECT_CALL(*callback1a, onValue(100));
  executor_.drain();

  {
    auto toDestroy = std::move(multiplexChannel);
  }
  EXPECT_CALL(*callback1a, onClosed());
  executor_.drain();
}

TEST_F(MultiplexChannelFixture, Subscribe_WithRateLimiter) {
  auto rateLimiter = MaxConcurrentRateLimiter::create(1 /* maxConcurrent */);
  auto [inputReceiver, inputSender] = Channel<TestInputValue>::create();
  auto multiplexChannel = createMultiplexChannel(
      TestMultiplexer(&executor_, std::move(rateLimiter)),
      std::move(inputReceiver));

  EXPECT_FALSE(multiplexChannel.anySubscribers());

  auto promise1a = folly::Promise<Unit>();
  auto [handle1a, callback1a] = processValues(multiplexChannel.subscribe(
      "one"s,
      TestSubscriptionArg{
          100 /* initialValue */,
          true /* firstSubscriptionForKey */,
          false /* throwException */,
          promise1a.getSemiFuture() /* waitForSubscription */}));

  executor_.drain();

  auto promise1b = folly::Promise<Unit>();
  auto [handle1b, callback1b] = processValues(multiplexChannel.subscribe(
      "one"s,
      TestSubscriptionArg{
          101 /* initialValue */,
          true /* firstSubscriptionForKey */,
          false /* throwException */,
          promise1b.getSemiFuture() /* waitForSubscription */}));

  executor_.drain();

  EXPECT_CALL(*callback1a, onValue(100));
  promise1a.setValue();
  executor_.drain();

  EXPECT_CALL(*callback1b, onValue(101));
  promise1b.setValue();
  executor_.drain();

  std::move(inputSender).close();
  EXPECT_CALL(*callback1a, onClosed());
  EXPECT_CALL(*callback1b, onClosed());
  executor_.drain();
}

class MultiplexChannelFixtureStress : public Test {
 protected:
  MultiplexChannelFixtureStress()
      : producer_(makeProducer()),
        consumers_(
            toVector(makeConsumer(0), makeConsumer(1), makeConsumer(2))) {}

  static std::unique_ptr<StressTestProducer<int>> makeProducer() {
    return std::make_unique<StressTestProducer<int>>([value = 0]() mutable {
      return value++;
    });
  }

  static std::unique_ptr<StressTestConsumer<int>> makeConsumer(int remainder) {
    return std::make_unique<StressTestConsumer<int>>(
        ConsumptionMode::CallbackWithHandle,
        [remainder, lastReceived = -1](int value) mutable {
          if (lastReceived == -1) {
            lastReceived = value;
            EXPECT_EQ(lastReceived % 3, remainder);
          } else {
            lastReceived += 3;
            EXPECT_EQ(value, lastReceived);
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

struct NoContext {};
struct NoSubscriptionArg {};

struct TestMultiplexerStress {
 public:
  explicit TestMultiplexerStress(
      folly::Executor::KeepAlive<folly::SequencedExecutor> executor)
      : executor_(std::move(executor)) {}

  folly::Executor::KeepAlive<folly::SequencedExecutor> getExecutor() {
    return executor_;
  }

  std::shared_ptr<RateLimiter> getRateLimiter() {
    return nullptr; // No rate limiting
  }

  folly::coro::Task<std::vector<int>> onNewSubscription(
      int, NoContext&, NoSubscriptionArg) {
    co_return std::vector<int>(); // No initial values
  }

  folly::coro::Task<void> onInputValue(
      Try<int> inputValue,
      MultiplexedSubscriptions<TestMultiplexerStress>& subscriptions) {
    if (subscriptions.hasSubscription(inputValue.value() % 3)) {
      subscriptions.write(inputValue.value() % 3, inputValue.value());
    }
    co_return;
  }

 private:
  folly::Executor::KeepAlive<folly::SequencedExecutor> executor_;
};

TEST_F(MultiplexChannelFixtureStress, HandleClosed) {
  auto [receiver, sender] = Channel<int>::create();
  producer_->startProducing(std::move(sender), std::nullopt /* closeEx */);

  folly::CPUThreadPoolExecutor multiplexChannelExecutor(1);
  auto multiplexChannel = createMultiplexChannel(
      TestMultiplexerStress(
          folly::SerialExecutor::create(&multiplexChannelExecutor)),
      std::move(receiver));

  consumers_.at(0)->startConsuming(
      multiplexChannel.subscribe(0 /* key */, NoSubscriptionArg()));
  consumers_.at(1)->startConsuming(
      multiplexChannel.subscribe(1 /* key */, NoSubscriptionArg()));

  sleepFor(kTestTimeout / 3);

  consumers_.at(2)->startConsuming(
      multiplexChannel.subscribe(2 /* key */, NoSubscriptionArg()));

  sleepFor(kTestTimeout / 3);

  consumers_.at(0)->cancel();
  EXPECT_EQ(consumers_.at(0)->waitForClose().get(), CloseType::Cancelled);

  sleepFor(kTestTimeout / 3);

  std::move(multiplexChannel).close();
  EXPECT_EQ(consumers_.at(1)->waitForClose().get(), CloseType::NoException);
  EXPECT_EQ(consumers_.at(2)->waitForClose().get(), CloseType::NoException);
}

TEST_F(MultiplexChannelFixtureStress, InputChannelClosed) {
  auto [receiver, sender] = Channel<int>::create();
  producer_->startProducing(std::move(sender), std::nullopt /* closeEx */);

  folly::CPUThreadPoolExecutor multiplexChannelExecutor(1);
  auto multiplexChannel = createMultiplexChannel(
      TestMultiplexerStress(
          folly::SerialExecutor::create(&multiplexChannelExecutor)),
      std::move(receiver));

  consumers_.at(0)->startConsuming(
      multiplexChannel.subscribe(0 /* key */, NoSubscriptionArg()));
  consumers_.at(1)->startConsuming(
      multiplexChannel.subscribe(1 /* key */, NoSubscriptionArg()));

  sleepFor(kTestTimeout / 3);

  consumers_.at(2)->startConsuming(
      multiplexChannel.subscribe(2 /* key */, NoSubscriptionArg()));

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
