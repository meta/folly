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

#include <folly/ScopeGuard.h>
#include <folly/Synchronized.h>
#include <folly/channels/ConsumeChannel.h>
#include <folly/channels/MaxConcurrentRateLimiter.h>
#include <folly/channels/Transform.h>
#include <folly/channels/test/ChannelTestUtil.h>
#include <folly/channels/testing/MockNextCallback.h>
#include <folly/coro/AsyncGenerator.h>
#include <folly/coro/DetachOnCancel.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/executors/SerialExecutor.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

namespace folly {
namespace channels {

using namespace testing;
using namespace std::string_literals;

class TransformFixture : public Test {
 protected:
  TransformFixture() {}

  ~TransformFixture() { executor_.drain(); }

  ChannelCallbackHandle processValues(Receiver<std::string> receiver) {
    return consumeChannelWithCallback(
        std::move(receiver),
        &executor_,
        [=, this](Try<std::string> resultTry) -> folly::coro::Task<bool> {
          onNext_(std::move(resultTry));
          co_return true;
        });
  }

  folly::ManualExecutor executor_;
  StrictMock<MockNextCallback<std::string>> onNext_;
};

class SimpleTransformFixture : public TransformFixture {};

TEST_F(SimpleTransformFixture, ReceiveValue_ReturnTransformedValue) {
  auto [untransformedReceiver, sender] = Channel<int>::create();
  auto transformedReceiver = transform(
      std::move(untransformedReceiver),
      &executor_,
      [](Try<int> result) -> folly::coro::AsyncGenerator<std::string&&> {
        if (result.value() % 2 == 0) {
          co_yield folly::to<std::string>(result.value());
        } else {
          co_return;
        }
      });

  EXPECT_CALL(onNext_, onValue("2"));
  EXPECT_CALL(onNext_, onValue("4"));
  EXPECT_CALL(onNext_, onClosed());

  sender.write(1);
  sender.write(2);
  executor_.drain();

  auto callbackHandle = processValues(std::move(transformedReceiver));

  sender.write(3);
  sender.write(4);
  executor_.drain();

  std::move(sender).close();
  executor_.drain();
}

TEST_F(SimpleTransformFixture, ReceiveValue_Close) {
  bool close = false;
  auto [untransformedReceiver, sender] = Channel<int>::create();
  auto transformedReceiver = transform(
      std::move(untransformedReceiver),
      &executor_,
      [&](Try<int> result) -> folly::coro::AsyncGenerator<std::string&&> {
        if (close) {
          throw OnClosedException();
        }
        co_yield folly::to<std::string>(result.value());
      });

  EXPECT_CALL(onNext_, onValue("1"));
  EXPECT_CALL(onNext_, onValue("2"));
  EXPECT_CALL(onNext_, onClosed());

  auto callbackHandle = processValues(std::move(transformedReceiver));

  sender.write(1);
  sender.write(2);
  executor_.drain();

  close = true;
  sender.write(3);
  sender.write(4);
  executor_.drain();
}

TEST_F(SimpleTransformFixture, ReceiveValue_Throw_InCoroutine) {
  bool throwException = false;
  auto [untransformedReceiver, sender] = Channel<int>::create();
  auto transformedReceiver = transform(
      std::move(untransformedReceiver),
      &executor_,
      [&](Try<int> result) -> folly::coro::AsyncGenerator<std::string&&> {
        if (throwException) {
          throw std::runtime_error("Error");
        }
        co_yield folly::to<std::string>(result.value());
      });

  EXPECT_CALL(onNext_, onValue("1"));
  EXPECT_CALL(onNext_, onValue("2"));
  EXPECT_CALL(onNext_, onRuntimeError("std::runtime_error: Error"));

  auto callbackHandle = processValues(std::move(transformedReceiver));

  sender.write(1);
  sender.write(2);
  executor_.drain();

  throwException = true;
  sender.write(3);
  sender.write(4);
  executor_.drain();
}

TEST_F(SimpleTransformFixture, ReceiveValue_Throw_InNonCoroutine) {
  bool throwException = false;
  auto [untransformedReceiver, sender] = Channel<int>::create();
  auto transformedReceiver = transform(
      std::move(untransformedReceiver),
      &executor_,
      [&](Try<int> result) -> folly::coro::AsyncGenerator<std::string&&> {
        if (throwException) {
          throw std::runtime_error("Error");
        }
        return folly::coro::co_invoke(
            [=]() -> folly::coro::AsyncGenerator<std::string&&> {
              co_yield folly::to<std::string>(result.value());
            });
      });

  EXPECT_CALL(onNext_, onValue("1"));
  EXPECT_CALL(onNext_, onValue("2"));
  EXPECT_CALL(onNext_, onRuntimeError("std::runtime_error: Error"));

  auto callbackHandle = processValues(std::move(transformedReceiver));

  sender.write(1);
  sender.write(2);
  executor_.drain();

  throwException = true;
  sender.write(3);
  sender.write(4);
  executor_.drain();
}

TEST_F(SimpleTransformFixture, ReceiveClosed_Close) {
  auto [untransformedReceiver, sender] = Channel<int>::create();
  auto transformedReceiver = transform(
      std::move(untransformedReceiver),
      &executor_,
      [&](Try<int> result) -> folly::coro::AsyncGenerator<std::string&&> {
        if (result.hasException()) {
          EXPECT_TRUE(result.hasException<OnClosedException>());
        }
        co_yield folly::to<std::string>(result.value());
      });

  EXPECT_CALL(onNext_, onValue("1"));
  EXPECT_CALL(onNext_, onValue("2"));
  EXPECT_CALL(onNext_, onClosed());

  auto callbackHandle = processValues(std::move(transformedReceiver));

  sender.write(1);
  sender.write(2);
  executor_.drain();

  std::move(sender).close();
  executor_.drain();
}

TEST_F(SimpleTransformFixture, ReceiveClosed_Throw) {
  auto [untransformedReceiver, sender] = Channel<int>::create();
  auto transformedReceiver = transform(
      std::move(untransformedReceiver),
      &executor_,
      [&](Try<int> result) -> folly::coro::AsyncGenerator<std::string&&> {
        if (result.hasException()) {
          EXPECT_TRUE(result.hasException<OnClosedException>());
          throw std::runtime_error("Error");
        }
        co_yield folly::to<std::string>(result.value());
      });

  EXPECT_CALL(onNext_, onValue("1"));
  EXPECT_CALL(onNext_, onValue("2"));
  EXPECT_CALL(onNext_, onRuntimeError("std::runtime_error: Error"));

  auto callbackHandle = processValues(std::move(transformedReceiver));

  sender.write(1);
  sender.write(2);
  executor_.drain();

  std::move(sender).close();
  executor_.drain();
}

TEST_F(SimpleTransformFixture, ReceiveException_Close) {
  auto [untransformedReceiver, sender] = Channel<int>::create();
  auto transformedReceiver = transform(
      std::move(untransformedReceiver),
      &executor_,
      [&](Try<int> result) -> folly::coro::AsyncGenerator<std::string&&> {
        if (result.hasException()) {
          EXPECT_THROW(result.throwUnlessValue(), std::runtime_error);
          // We will swallow the exception and move on.
          throw OnClosedException();
        }
        co_yield folly::to<std::string>(result.value());
      });

  EXPECT_CALL(onNext_, onValue("1"));
  EXPECT_CALL(onNext_, onValue("2"));
  EXPECT_CALL(onNext_, onClosed());

  auto callbackHandle = processValues(std::move(transformedReceiver));

  sender.write(1);
  sender.write(2);
  executor_.drain();

  std::move(sender).close(std::runtime_error("Error"));
  executor_.drain();
}

TEST_F(SimpleTransformFixture, ReceiveException_Throw) {
  auto [untransformedReceiver, sender] = Channel<int>::create();
  auto transformedReceiver = transform(
      std::move(untransformedReceiver),
      &executor_,
      [&](Try<int> result) -> folly::coro::AsyncGenerator<std::string&&> {
        if (result.hasException()) {
          EXPECT_THROW(result.throwUnlessValue(), std::runtime_error);
        }
        co_yield folly::to<std::string>(result.value());
      });

  EXPECT_CALL(onNext_, onValue("1"));
  EXPECT_CALL(onNext_, onValue("2"));
  EXPECT_CALL(onNext_, onRuntimeError("std::runtime_error: Error"));

  auto callbackHandle = processValues(std::move(transformedReceiver));

  sender.write(1);
  sender.write(2);
  executor_.drain();

  std::move(sender).close(std::runtime_error("Error"));
  executor_.drain();
}

TEST_F(SimpleTransformFixture, Cancelled) {
  auto [untransformedReceiver, sender] = Channel<int>::create();
  auto transformedReceiver = transform(
      std::move(untransformedReceiver),
      &executor_,
      [&](Try<int> result) -> folly::coro::AsyncGenerator<std::string&&> {
        co_yield folly::to<std::string>(result.value());
      });

  EXPECT_CALL(onNext_, onValue("1"));
  EXPECT_CALL(onNext_, onValue("2"));
  EXPECT_CALL(onNext_, onCancelled());

  auto callbackHandle = processValues(std::move(transformedReceiver));

  sender.write(1);
  sender.write(2);
  executor_.drain();

  LOG(INFO) << "Cancelling...";
  callbackHandle.reset();
  executor_.drain();
  LOG(INFO) << "Finished cancelling";
}

TEST_F(SimpleTransformFixture, Chained) {
  auto [receiver, sender] = Channel<int>::create();
  for (int i = 0; i < 10; i++) {
    receiver = transform(
        std::move(receiver),
        &executor_,
        [](Try<int> result) -> folly::coro::AsyncGenerator<int> {
          co_yield result.value() + 1;
        });
  }
  auto callbackHandle = processValues(transform(
      std::move(receiver),
      &executor_,
      [](Try<int> result) -> folly::coro::AsyncGenerator<std::string&&> {
        co_yield folly::to<std::string>(result.value());
      }));
  executor_.drain();

  EXPECT_CALL(onNext_, onValue("11"));
  EXPECT_CALL(onNext_, onValue("12"));
  EXPECT_CALL(onNext_, onValue("13"));
  EXPECT_CALL(onNext_, onValue("14"));
  EXPECT_CALL(onNext_, onClosed());

  sender.write(1);
  sender.write(2);
  sender.write(3);
  sender.write(4);
  std::move(sender).close();
  executor_.drain();
}

TEST_F(SimpleTransformFixture, MultipleTransformsWithRateLimiter) {
  auto rateLimiter = MaxConcurrentRateLimiter::create(1 /* maxConcurrent */);

  auto [untransformedReceiver1, sender1] = Channel<int>::create();
  auto [controlReceiver1, controlSender1] = Channel<Unit>::create();
  int transform1Executions = 0;
  auto transformedReceiver1 = transform(
      std::move(untransformedReceiver1),
      &executor_,
      [&, &controlReceiver1_2 = controlReceiver1](
          Try<int> result) -> folly::coro::AsyncGenerator<std::string&&> {
        transform1Executions++;
        co_await controlReceiver1_2.next();
        co_yield folly::to<std::string>(result.value());
      },
      rateLimiter);

  auto [untransformedReceiver2, sender2] = Channel<int>::create();
  auto [controlReceiver2, controlSender2] = Channel<Unit>::create();
  int transform2Executions = 0;
  auto transformedReceiver2 = transform(
      std::move(untransformedReceiver2),
      &executor_,
      [&, &controlReceiver2_2 = controlReceiver2](
          Try<int> result) -> folly::coro::AsyncGenerator<std::string&&> {
        transform2Executions++;
        co_await controlReceiver2_2.next();
        co_yield folly::to<std::string>(result.value());
      },
      rateLimiter);

  auto callbackHandle1 = processValues(std::move(transformedReceiver1));
  auto callbackHandle2 = processValues(std::move(transformedReceiver2));

  EXPECT_CALL(onNext_, onValue("1"));
  EXPECT_CALL(onNext_, onValue("1000"));
  EXPECT_CALL(onNext_, onValue("2"));
  EXPECT_CALL(onNext_, onValue("2000"));
  EXPECT_CALL(onNext_, onClosed()).Times(2);

  sender1.write(1);
  sender2.write(1000);
  executor_.drain();

  EXPECT_EQ(transform1Executions, 1);
  EXPECT_EQ(transform2Executions, 0);
  controlSender1.write(unit);
  executor_.drain();

  EXPECT_EQ(transform1Executions, 1);
  EXPECT_EQ(transform2Executions, 1);
  controlSender2.write(unit);
  executor_.drain();

  sender2.write(2000);
  sender1.write(2);
  executor_.drain();

  EXPECT_EQ(transform1Executions, 1);
  EXPECT_EQ(transform2Executions, 2);
  controlSender2.write(unit);
  executor_.drain();

  EXPECT_EQ(transform1Executions, 2);
  EXPECT_EQ(transform2Executions, 2);
  controlSender1.write(unit);
  executor_.drain();

  std::move(sender1).close();
  std::move(sender2).close();
  std::move(controlSender1).close();
  std::move(controlSender2).close();
  executor_.drain();
}

class TransformFixtureStress : public Test {
 protected:
  TransformFixtureStress()
      : producer_(
            std::make_unique<StressTestProducer<int>>([value = 0]() mutable {
              return value++;
            })),
        consumer_(
            std::make_unique<StressTestConsumer<std::string>>(
                ConsumptionMode::CallbackWithHandle,
                [lastReceived = -1](std::string value) mutable {
                  EXPECT_EQ(folly::to<int>(value), ++lastReceived);
                })) {}

  static constexpr std::chrono::milliseconds kTestTimeout =
      std::chrono::milliseconds{5000};

  std::unique_ptr<StressTestProducer<int>> producer_;
  std::unique_ptr<StressTestConsumer<std::string>> consumer_;
};

TEST_F(TransformFixtureStress, Close) {
  auto [receiver, sender] = Channel<int>::create();
  producer_->startProducing(std::move(sender), std::nullopt /* closeEx */);

  folly::CPUThreadPoolExecutor transformExecutor(1);
  consumer_->startConsuming(transform(
      std::move(receiver),
      folly::SerialExecutor::create(&transformExecutor),
      [](Try<int> result) -> folly::coro::AsyncGenerator<std::string> {
        co_yield folly::to<std::string>(std::move(result.value()));
      }));

  /* sleep override */
  std::this_thread::sleep_for(kTestTimeout);
  producer_->stopProducing();
  EXPECT_EQ(consumer_->waitForClose().get(), CloseType::NoException);
}

TEST_F(TransformFixtureStress, Cancel) {
  auto [receiver, sender] = Channel<int>::create();
  producer_->startProducing(std::move(sender), std::nullopt /* closeEx */);

  folly::CPUThreadPoolExecutor transformExecutor(1);
  consumer_->startConsuming(transform(
      std::move(receiver),
      folly::SerialExecutor::create(&transformExecutor),
      [](Try<int> result) -> folly::coro::AsyncGenerator<std::string> {
        co_yield folly::to<std::string>(std::move(result.value()));
      }));

  /* sleep override */
  std::this_thread::sleep_for(kTestTimeout);
  consumer_->cancel();
  EXPECT_EQ(consumer_->waitForClose().get(), CloseType::Cancelled);
}

TEST_F(TransformFixtureStress, Close_ThenCancelImmediately) {
  auto [receiver, sender] = Channel<int>::create();
  producer_->startProducing(std::move(sender), std::nullopt /* closeEx */);

  folly::CPUThreadPoolExecutor transformExecutor(1);
  consumer_->startConsuming(transform(
      std::move(receiver),
      folly::SerialExecutor::create(&transformExecutor),
      [](Try<int> result) -> folly::coro::AsyncGenerator<std::string> {
        co_yield folly::to<std::string>(std::move(result.value()));
      }));

  /* sleep override */
  std::this_thread::sleep_for(kTestTimeout);
  producer_->stopProducing();
  consumer_->cancel();

  EXPECT_THAT(
      consumer_->waitForClose().get(),
      AnyOf(Eq(CloseType::NoException), Eq(CloseType::Cancelled)));
}

TEST_F(TransformFixtureStress, Cancel_ThenCloseImmediately) {
  auto [receiver, sender] = Channel<int>::create();
  producer_->startProducing(std::move(sender), std::nullopt /* closeEx */);

  folly::CPUThreadPoolExecutor transformExecutor(1);
  consumer_->startConsuming(transform(
      std::move(receiver),
      folly::SerialExecutor::create(&transformExecutor),
      [](Try<int> result) -> folly::coro::AsyncGenerator<std::string> {
        co_yield folly::to<std::string>(std::move(result.value()));
      }));

  /* sleep override */
  std::this_thread::sleep_for(kTestTimeout);
  consumer_->cancel();
  producer_->stopProducing();

  EXPECT_THAT(
      consumer_->waitForClose().get(),
      AnyOf(Eq(CloseType::NoException), Eq(CloseType::Cancelled)));
}

class ResumableTransformFixture : public TransformFixture {};

TEST_F(
    ResumableTransformFixture,
    InitializesAndReturnsTransformedValues_ThenClosed) {
  auto [untransformedReceiver, sender] = Channel<int>::create();
  auto transformedReceiver = resumableTransform(
      &executor_,
      toVector("abc"s, "def"s),
      [receiver = std::move(untransformedReceiver)](
          std::vector<std::string> initializeArg) mutable
          -> folly::coro::Task<
              std::pair<std::vector<std::string>, Receiver<int>>> {
        co_return std::make_pair(std::move(initializeArg), std::move(receiver));
      },
      [](Try<int> result) -> folly::coro::AsyncGenerator<std::string&&> {
        co_yield folly::to<std::string>(result.value());
      });

  EXPECT_CALL(onNext_, onValue("abc"));
  EXPECT_CALL(onNext_, onValue("def"));
  EXPECT_CALL(onNext_, onValue("1"));
  EXPECT_CALL(onNext_, onValue("2"));
  EXPECT_CALL(onNext_, onValue("3"));
  EXPECT_CALL(onNext_, onValue("4"));
  EXPECT_CALL(onNext_, onClosed());

  sender.write(1);
  sender.write(2);
  executor_.drain();

  auto callbackHandle = processValues(std::move(transformedReceiver));

  sender.write(3);
  sender.write(4);
  executor_.drain();

  std::move(sender).close();
  executor_.drain();
}

TEST_F(
    ResumableTransformFixture,
    InitializesAndReturnsTransformedValues_ThenCancelled) {
  auto [untransformedReceiver, sender] = Channel<int>::create();
  auto transformedReceiver = resumableTransform(
      &executor_,
      toVector("abc"s, "def"s),
      [receiver = std::move(untransformedReceiver)](
          std::vector<std::string> initializeArg) mutable
          -> folly::coro::Task<
              std::pair<std::vector<std::string>, Receiver<int>>> {
        co_return std::make_pair(std::move(initializeArg), std::move(receiver));
      },
      [](Try<int> result) -> folly::coro::AsyncGenerator<std::string&&> {
        co_yield folly::to<std::string>(result.value());
      });

  EXPECT_CALL(onNext_, onValue("abc"));
  EXPECT_CALL(onNext_, onValue("def"));
  EXPECT_CALL(onNext_, onValue("1"));
  EXPECT_CALL(onNext_, onValue("2"));
  EXPECT_CALL(onNext_, onValue("3"));
  EXPECT_CALL(onNext_, onValue("4"));
  EXPECT_CALL(onNext_, onCancelled());

  sender.write(1);
  sender.write(2);
  executor_.drain();

  auto callbackHandle = processValues(std::move(transformedReceiver));

  sender.write(3);
  sender.write(4);
  executor_.drain();

  callbackHandle.reset();
  executor_.drain();
}

TEST_F(
    ResumableTransformFixture,
    InitializesAndReturnsTransformedValues_ThenClosed_CancelledBeforeReinit) {
  auto [untransformedReceiver, sender] = Channel<int>::create();
  auto transformedReceiver = resumableTransform(
      &executor_,
      toVector("abc"s, "def"s),
      [receiver = std::move(untransformedReceiver)](
          std::vector<std::string> initializeArg) mutable
          -> folly::coro::Task<
              std::pair<std::vector<std::string>, Receiver<int>>> {
        co_return std::make_pair(std::move(initializeArg), std::move(receiver));
      },
      [](Try<int> result) -> folly::coro::AsyncGenerator<std::string&&> {
        co_yield folly::to<std::string>(result.value());
      });

  EXPECT_CALL(onNext_, onValue("abc"));
  EXPECT_CALL(onNext_, onValue("def"));
  EXPECT_CALL(onNext_, onValue("1"));
  EXPECT_CALL(onNext_, onValue("2"));
  EXPECT_CALL(onNext_, onCancelled());

  auto callbackHandle = processValues(std::move(transformedReceiver));

  sender.write(1);
  sender.write(2);
  executor_.drain();

  std::move(sender).close();
  callbackHandle.reset();
  executor_.drain();
}

TEST_F(
    ResumableTransformFixture,
    FirstReceiverCloses_ReinitializesWithNewReceiver_ThenClosed) {
  auto [untransformedReceiver, sender] = Channel<int>::create();
  auto transformedReceiver = resumableTransform(
      &executor_,
      toVector("abc1"s),
      [&receiver = untransformedReceiver](
          std::vector<std::string> initializeArg) mutable
          -> folly::coro::Task<
              std::pair<std::vector<std::string>, Receiver<int>>> {
        co_return std::make_pair(std::move(initializeArg), std::move(receiver));
      },
      [numReinitializations = 0](Try<int> result) mutable
          -> folly::coro::AsyncGenerator<std::string&&> {
        try {
          co_yield folly::to<std::string>(result.value());
        } catch (const OnClosedException&) {
          if (numReinitializations >= 1) {
            throw;
          }
          numReinitializations++;
          throw ReinitializeException(toVector("abc2"s));
        }
      });

  EXPECT_CALL(onNext_, onValue("abc1"));
  EXPECT_CALL(onNext_, onValue("1"));
  EXPECT_CALL(onNext_, onValue("2"));
  EXPECT_CALL(onNext_, onValue("abc2"));
  EXPECT_CALL(onNext_, onValue("3"));
  EXPECT_CALL(onNext_, onValue("4"));
  EXPECT_CALL(onNext_, onClosed());

  auto callbackHandle = processValues(std::move(transformedReceiver));

  sender.write(1);
  sender.write(2);
  executor_.drain();

  std::move(sender).close();
  std::tie(untransformedReceiver, sender) = Channel<int>::create();
  executor_.drain();

  sender.write(3);
  sender.write(4);
  executor_.drain();

  std::move(sender).close();
  executor_.drain();
}

TEST_F(
    ResumableTransformFixture,
    FirstReceiverClosesWithException_NoReinitialization_Rethrows) {
  auto [untransformedReceiver, sender] = Channel<int>::create();
  auto transformedReceiver = resumableTransform(
      &executor_,
      toVector("abc"s),
      [alreadyInitialized = false, receiver = std::move(untransformedReceiver)](
          std::vector<std::string> initializeArg) mutable
          -> folly::coro::Task<
              std::pair<std::vector<std::string>, Receiver<int>>> {
        CHECK(!alreadyInitialized);
        alreadyInitialized = true;
        co_return std::make_pair(std::move(initializeArg), std::move(receiver));
      },
      [](Try<int> result) -> folly::coro::AsyncGenerator<std::string&&> {
        co_yield folly::to<std::string>(result.value());
      });

  EXPECT_CALL(onNext_, onValue("abc"));
  EXPECT_CALL(onNext_, onValue("1"));
  EXPECT_CALL(onNext_, onValue("2"));
  EXPECT_CALL(onNext_, onRuntimeError("std::runtime_error: Error"));

  auto callbackHandle = processValues(std::move(transformedReceiver));

  sender.write(1);
  sender.write(2);
  executor_.drain();

  std::move(sender).close(std::runtime_error("Error"));
  executor_.drain();
}

TEST_F(
    ResumableTransformFixture,
    FirstReceiverClosesWithException_TransformSwallows_Reinitialization) {
  auto [untransformedReceiver, sender] = Channel<int>::create();
  auto transformedReceiver = resumableTransform(
      &executor_,
      toVector("abc1"s),
      [&receiver = untransformedReceiver](
          std::vector<std::string> initializeArg) mutable
          -> folly::coro::Task<
              std::pair<std::vector<std::string>, Receiver<int>>> {
        co_return std::make_pair(std::move(initializeArg), std::move(receiver));
      },
      [](Try<int> result) -> folly::coro::AsyncGenerator<std::string&&> {
        if (result.hasValue()) {
          co_yield folly::to<std::string>(result.value());
        } else {
          EXPECT_THROW(result.throwUnlessValue(), std::runtime_error);
          throw ReinitializeException(toVector("abc2"s));
        }
      });

  EXPECT_CALL(onNext_, onValue("abc1"));
  EXPECT_CALL(onNext_, onValue("1"));
  EXPECT_CALL(onNext_, onValue("2"));
  EXPECT_CALL(onNext_, onValue("abc2"));
  EXPECT_CALL(onNext_, onValue("3"));
  EXPECT_CALL(onNext_, onValue("4"));
  EXPECT_CALL(onNext_, onCancelled());

  auto callbackHandle = processValues(std::move(transformedReceiver));

  sender.write(1);
  sender.write(2);
  executor_.drain();

  std::move(sender).close(std::runtime_error("Error"));
  std::tie(untransformedReceiver, sender) = Channel<int>::create();
  executor_.drain();

  sender.write(3);
  sender.write(4);
  executor_.drain();

  callbackHandle.reset();
  executor_.drain();
}

TEST_F(ResumableTransformFixture, TransformThrows_NoReinitialization_Rethrows) {
  auto [untransformedReceiver, sender] = Channel<int>::create();
  bool transformThrows = false;
  auto transformedReceiver = resumableTransform(
      &executor_,
      toVector("abc"s),
      [alreadyInitialized = false, &receiver = untransformedReceiver](
          std::vector<std::string> initializeArg) mutable
          -> folly::coro::Task<
              std::pair<std::vector<std::string>, Receiver<int>>> {
        CHECK(!alreadyInitialized);
        alreadyInitialized = true;
        co_return std::make_pair(std::move(initializeArg), std::move(receiver));
      },
      [&](Try<int> result) -> folly::coro::AsyncGenerator<std::string&&> {
        if (transformThrows) {
          throw std::runtime_error("Error");
        }
        co_yield folly::to<std::string>(result.value());
      });

  EXPECT_CALL(onNext_, onValue("abc"));
  EXPECT_CALL(onNext_, onValue("1"));
  EXPECT_CALL(onNext_, onValue("2"));
  EXPECT_CALL(onNext_, onRuntimeError("std::runtime_error: Error"));

  auto callbackHandle = processValues(std::move(transformedReceiver));

  sender.write(1);
  sender.write(2);
  executor_.drain();

  transformThrows = true;
  sender.write(100);
  executor_.drain();
}

TEST_F(ResumableTransformFixture, MultipleResumableTransformsWithRateLimiter) {
  auto rateLimiter = MaxConcurrentRateLimiter::create(1 /* maxConcurrent */);

  auto [untransformedReceiver1, sender1] = Channel<int>::create();
  auto [controlReceiver1, controlSender1] = Channel<Unit>::create();
  int transform1Executions = 0;
  auto transformedReceiver1 = resumableTransform(
      &executor_,
      toVector("init1"s),
      [&,
       receiver = std::move(untransformedReceiver1),
       &controlReceiver1_2 =
           controlReceiver1](std::vector<std::string> initializeArg) mutable
          -> folly::coro::Task<
              std::pair<std::vector<std::string>, Receiver<int>>> {
        transform1Executions++;
        co_await controlReceiver1_2.next();
        co_return std::make_pair(std::move(initializeArg), std::move(receiver));
      },
      [&, &controlReceiver1_2 = controlReceiver1](
          Try<int> result) -> folly::coro::AsyncGenerator<std::string&&> {
        transform1Executions++;
        co_await controlReceiver1_2.next();
        co_yield folly::to<std::string>(result.value());
      },
      rateLimiter);

  auto [untransformedReceiver2, sender2] = Channel<int>::create();
  auto [controlReceiver2, controlSender2] = Channel<Unit>::create();
  int transform2Executions = 0;
  auto transformedReceiver2 = resumableTransform(
      &executor_,
      toVector("init2"s),
      [&,
       receiver = std::move(untransformedReceiver2),
       &controlReceiver2_2 =
           controlReceiver2](std::vector<std::string> initializeArg) mutable
          -> folly::coro::Task<
              std::pair<std::vector<std::string>, Receiver<int>>> {
        transform2Executions++;
        co_await controlReceiver2_2.next();
        co_return std::make_pair(std::move(initializeArg), std::move(receiver));
      },
      [&, &controlReceiver2_2 = controlReceiver2](
          Try<int> result) -> folly::coro::AsyncGenerator<std::string&&> {
        transform2Executions++;
        co_await controlReceiver2_2.next();
        co_yield folly::to<std::string>(result.value());
      },
      rateLimiter);

  auto callbackHandle1 = processValues(std::move(transformedReceiver1));
  auto callbackHandle2 = processValues(std::move(transformedReceiver2));

  EXPECT_CALL(onNext_, onValue("init1"));
  EXPECT_CALL(onNext_, onValue("init2"));
  EXPECT_CALL(onNext_, onValue("1"));
  EXPECT_CALL(onNext_, onValue("1000"));
  EXPECT_CALL(onNext_, onValue("2"));
  EXPECT_CALL(onNext_, onValue("2000"));
  EXPECT_CALL(onNext_, onClosed()).Times(2);
  executor_.drain();

  EXPECT_EQ(transform1Executions, 1);
  EXPECT_EQ(transform2Executions, 0);
  controlSender1.write(unit);
  executor_.drain();

  EXPECT_EQ(transform1Executions, 1);
  EXPECT_EQ(transform2Executions, 1);
  controlSender2.write(unit);
  executor_.drain();

  sender1.write(1);
  sender2.write(1000);
  executor_.drain();

  EXPECT_EQ(transform1Executions, 2);
  EXPECT_EQ(transform2Executions, 1);
  controlSender1.write(unit);
  executor_.drain();

  EXPECT_EQ(transform1Executions, 2);
  EXPECT_EQ(transform2Executions, 2);
  controlSender2.write(unit);
  executor_.drain();

  sender2.write(2000);
  sender1.write(2);
  executor_.drain();

  EXPECT_EQ(transform1Executions, 2);
  EXPECT_EQ(transform2Executions, 3);
  controlSender2.write(unit);
  executor_.drain();

  EXPECT_EQ(transform1Executions, 3);
  EXPECT_EQ(transform2Executions, 3);
  controlSender1.write(unit);
  executor_.drain();

  std::move(sender1).close();
  std::move(sender2).close();
  std::move(controlSender1).close();
  std::move(controlSender2).close();
  executor_.drain();
}

class ResumableTransformFixtureStress : public Test {
 protected:
  ResumableTransformFixtureStress()
      : consumer_(
            std::make_unique<StressTestConsumer<std::string>>(
                ConsumptionMode::CallbackWithHandle,
                [lastReceived = -1](std::string value) mutable {
                  if (value == "start") {
                    lastReceived = -1;
                  } else {
                    EXPECT_EQ(folly::to<int>(value), ++lastReceived);
                  }
                })) {}

  std::unique_ptr<StressTestProducer<int>> makeProducer() {
    return std::make_unique<StressTestProducer<int>>([value = 0]() mutable {
      return value++;
    });
  }

  void setProducer(std::unique_ptr<StressTestProducer<int>> producer) {
    (*producer_.wlock()) = std::move(producer);
    producerReady_.post();
  }

  void waitForProducer() {
    producerReady_.wait();
    LOG(INFO) << "Finished waiting!";
    producerReady_.reset();
  }

  void stopProducing() { (*producer_.wlock())->stopProducing(); }

  static constexpr std::chrono::milliseconds kTestTimeout =
      std::chrono::milliseconds{10};

  folly::Synchronized<std::unique_ptr<StressTestProducer<int>>> producer_;
  folly::Baton<> producerReady_;
  std::unique_ptr<StressTestConsumer<std::string>> consumer_;
};

TEST_F(ResumableTransformFixtureStress, Close) {
  folly::CPUThreadPoolExecutor transformExecutor(1);
  std::atomic<bool> close = false;
  consumer_->startConsuming(resumableTransform(
      folly::SerialExecutor::create(&transformExecutor),
      toVector("start"s),
      [&](std::vector<std::string> initializeArg)
          -> folly::coro::Task<
              std::pair<std::vector<std::string>, Receiver<int>>> {
        auto [receiver, sender] = Channel<int>::create();
        auto newProducer = makeProducer();
        newProducer->startProducing(
            std::move(sender), std::nullopt /* closeEx */);
        setProducer(std::move(newProducer));
        co_return std::make_pair(std::move(initializeArg), std::move(receiver));
      },
      [&](Try<int> result) -> folly::coro::AsyncGenerator<std::string> {
        try {
          co_yield folly::to<std::string>(std::move(result.value()));
        } catch (const OnClosedException&) {
          if (close) {
            throw;
          } else {
            throw ReinitializeException(toVector("start"s));
          }
        }
      }));

  waitForProducer();
  /* sleep override */
  std::this_thread::sleep_for(kTestTimeout / 2);
  stopProducing();

  waitForProducer();
  /* sleep override */
  std::this_thread::sleep_for(kTestTimeout / 2);
  close = true;
  stopProducing();

  EXPECT_EQ(consumer_->waitForClose().get(), CloseType::NoException);
}

TEST_F(ResumableTransformFixtureStress, CancelDuringReinitialization) {
  folly::CPUThreadPoolExecutor transformExecutor(1);
  auto initializationStarted = folly::SharedPromise<Unit>();
  auto initializationWait = folly::SharedPromise<Unit>();
  auto resumableTransformDestroyed = folly::SharedPromise<Unit>();
  auto guard = folly::makeGuard([&]() {
    resumableTransformDestroyed.setValue();
  });
  consumer_->startConsuming(resumableTransform(
      folly::SerialExecutor::create(&transformExecutor),
      toVector("start"s),
      [&, g = std::move(guard)](std::vector<std::string> initializeArg)
          -> folly::coro::Task<
              std::pair<std::vector<std::string>, Receiver<int>>> {
        initializationStarted.setValue(unit);
        co_await folly::coro::detachOnCancel(
            initializationWait.getSemiFuture());
        initializationWait = folly::SharedPromise<Unit>();
        auto [receiver, sender] = Channel<int>::create();
        auto newProducer = makeProducer();
        newProducer->startProducing(
            std::move(sender), std::nullopt /* closeEx */);
        setProducer(std::move(newProducer));
        co_return std::make_pair(std::move(initializeArg), std::move(receiver));
      },
      [](Try<int> result) -> folly::coro::AsyncGenerator<std::string> {
        try {
          co_yield folly::to<std::string>(std::move(result.value()));
        } catch (const OnClosedException&) {
          throw ReinitializeException(toVector("start"s));
        }
      }));

  initializationStarted.getSemiFuture().get();
  initializationStarted = folly::SharedPromise<Unit>();
  initializationWait.setValue(unit);
  waitForProducer();
  /* sleep override */
  std::this_thread::sleep_for(kTestTimeout / 2);
  stopProducing();

  initializationStarted.getSemiFuture().get();
  initializationStarted = folly::SharedPromise<Unit>();
  consumer_->cancel();
  initializationWait.setValue(unit);

  EXPECT_EQ(consumer_->waitForClose().get(), CloseType::Cancelled);
  resumableTransformDestroyed.getSemiFuture().get();
}
} // namespace channels
} // namespace folly
