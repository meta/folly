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

#include <folly/channels/Producer.h>
#include <folly/channels/test/ChannelTestUtil.h>
#include <folly/channels/testing/MockNextCallback.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

namespace folly {
namespace channels {

using namespace testing;

class ProducerFixture : public Test {
 protected:
  ProducerFixture() {}

  ~ProducerFixture() { executor_.drain(); }

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

TEST_F(ProducerFixture, Write_ThenCloseWithoutException) {
  class TestProducer : public Producer<int> {
   public:
    TestProducer(
        Sender<int> sender,
        folly::Executor::KeepAlive<folly::SequencedExecutor> executor)
        : Producer<int>(std::move(sender), std::move(executor)) {
      write(1);
      close();
    }
  };

  auto receiver = makeProducer<TestProducer>(&executor_);

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onClosed());

  auto callbackHandle = processValues(std::move(receiver));

  executor_.drain();
}

TEST_F(ProducerFixture, Write_ThenCloseWithException) {
  class TestProducer : public Producer<int> {
   public:
    TestProducer(
        Sender<int> sender,
        folly::Executor::KeepAlive<folly::SequencedExecutor> executor)
        : Producer<int>(std::move(sender), std::move(executor)) {
      write(1);
      close(std::runtime_error("Error"));
    }
  };

  auto receiver = makeProducer<TestProducer>(&executor_);

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onRuntimeError("std::runtime_error: Error"));

  auto callbackHandle = processValues(std::move(receiver));

  executor_.drain();
}

TEST_F(ProducerFixture, KeepAliveExists_DelaysDestruction) {
  class TestProducer : public Producer<int> {
   public:
    TestProducer(
        Sender<int> sender,
        folly::Executor::KeepAlive<folly::SequencedExecutor> executor,
        folly::SemiFuture<Unit> future,
        bool& destructed)
        : Producer<int>(std::move(sender), std::move(executor)),
          destructed_(destructed) {
      co_withExecutor(
          getExecutor(),
          folly::coro::co_invoke(
              [keepAlive = getKeepAlive(), future = std::move(future)]() mutable
                  -> folly::coro::Task<void> { co_await std::move(future); }))
          .start();
    }

    ~TestProducer() { destructed_ = true; }

    bool& destructed_;
  };

  auto promise = folly::Promise<Unit>();
  bool destructed = false;
  auto receiver = makeProducer<TestProducer>(
      &executor_, promise.getSemiFuture(), destructed);

  EXPECT_CALL(onNext_, onCancelled());

  auto callbackHandle = processValues(std::move(receiver));
  executor_.drain();

  callbackHandle.reset();
  executor_.drain();

  EXPECT_FALSE(destructed);

  promise.setValue();
  executor_.drain();

  EXPECT_TRUE(destructed);
}

TEST_F(
    ProducerFixture,
    ConsumerStopsConsumingReceiver_OnCancelledCalled_ThenDestructed) {
  class TestProducer : public Producer<int> {
   public:
    TestProducer(
        Sender<int> sender,
        folly::Executor::KeepAlive<folly::SequencedExecutor> executor,
        folly::Promise<Unit> onCancelledStarted,
        folly::SemiFuture<Unit> onCancelledCompleted,
        bool& destructed)
        : Producer<int>(std::move(sender), std::move(executor)),
          onCancelledStarted_(std::move(onCancelledStarted)),
          onCancelledCompleted_(std::move(onCancelledCompleted)),
          destructed_(destructed) {}

    folly::coro::Task<void> onClosed() override {
      onCancelledStarted_.setValue();
      co_await std::move(onCancelledCompleted_);
    }

    ~TestProducer() override { destructed_ = true; }

    folly::Promise<Unit> onCancelledStarted_;
    folly::SemiFuture<Unit> onCancelledCompleted_;
    bool& destructed_;
  };

  auto onCancelledStartedPromise = folly::Promise<Unit>();
  auto onCancelledStartedFuture = onCancelledStartedPromise.getSemiFuture();
  auto onCancelledCompletedPromise = folly::Promise<Unit>();
  auto onCancelledCompletedFuture = onCancelledCompletedPromise.getSemiFuture();
  bool destructed = false;
  auto receiver = makeProducer<TestProducer>(
      &executor_,
      std::move(onCancelledStartedPromise),
      std::move(onCancelledCompletedFuture),
      destructed);

  EXPECT_CALL(onNext_, onCancelled());

  auto callbackHandle = processValues(std::move(receiver));
  executor_.drain();

  EXPECT_FALSE(onCancelledStartedFuture.isReady());
  EXPECT_FALSE(destructed);

  callbackHandle.reset();
  executor_.drain();

  EXPECT_TRUE(onCancelledStartedFuture.isReady());
  EXPECT_FALSE(destructed);

  onCancelledCompletedPromise.setValue();
  executor_.drain();

  EXPECT_TRUE(destructed);
}
} // namespace channels
} // namespace folly
