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
#include <folly/channels/FanoutSender.h>
#include <folly/channels/test/ChannelTestUtil.h>
#include <folly/channels/testing/MockNextCallback.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/executors/SerialExecutor.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

namespace folly {
namespace channels {

using namespace testing;

class FanoutSenderFixture : public Test {
 protected:
  FanoutSenderFixture() {}

  using TCallback = StrictMock<MockNextCallback<int>>;

  std::pair<ChannelCallbackHandle, TCallback*> processValues(
      Receiver<int> receiver) {
    auto callback = std::make_unique<TCallback>();
    auto callbackPtr = callback.get();
    auto handle = consumeChannelWithCallback(
        std::move(receiver),
        &executor_,
        [cbk = std::move(callback)](
            Try<int> resultTry) mutable -> folly::coro::Task<bool> {
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

TEST_F(FanoutSenderFixture, WriteValue_FanoutBroadcastsValues) {
  auto fanoutSender = FanoutSender<int>();

  fanoutSender.write(-1);

  EXPECT_FALSE(fanoutSender.anySubscribers());

  auto [handle1, callback1] =
      processValues(fanoutSender.subscribe(toVector(100)));

  EXPECT_CALL(*callback1, onValue(100));
  EXPECT_CALL(*callback1, onValue(0));

  fanoutSender.write(0);
  executor_.drain();

  EXPECT_TRUE(fanoutSender.anySubscribers());

  auto [handle2, callback2] =
      processValues(fanoutSender.subscribe(toVector(200)));

  EXPECT_CALL(*callback2, onValue(200));
  EXPECT_CALL(*callback1, onValue(1));
  EXPECT_CALL(*callback2, onValue(1));
  EXPECT_CALL(*callback1, onValue(2));
  EXPECT_CALL(*callback2, onValue(2));

  fanoutSender.write(1);
  fanoutSender.write(2);
  executor_.drain();

  EXPECT_TRUE(fanoutSender.anySubscribers());

  EXPECT_CALL(*callback1, onClosed());
  EXPECT_CALL(*callback2, onClosed());

  std::move(fanoutSender).close();
  executor_.drain();
}

TEST_F(FanoutSenderFixture, InputThrows_AllOutputReceiversGetException) {
  auto fanoutSender = FanoutSender<int>();

  auto [handle1, callback1] = processValues(fanoutSender.subscribe());
  auto [handle2, callback2] = processValues(fanoutSender.subscribe());

  EXPECT_CALL(*callback1, onValue(1));
  EXPECT_CALL(*callback2, onValue(1));
  EXPECT_CALL(*callback1, onRuntimeError("std::runtime_error: Error"));
  EXPECT_CALL(*callback2, onRuntimeError("std::runtime_error: Error"));

  executor_.drain();

  EXPECT_TRUE(fanoutSender.anySubscribers());

  fanoutSender.write(1);
  executor_.drain();

  std::move(fanoutSender).close(std::runtime_error("Error"));
  executor_.drain();

  fanoutSender = FanoutSender<int>();

  std::tie(handle1, callback1) = processValues(fanoutSender.subscribe());
  executor_.drain();

  EXPECT_CALL(*callback1, onRuntimeError("std::runtime_error: Error"));

  std::move(fanoutSender).close(std::runtime_error("Error"));
  executor_.drain();
}

TEST_F(FanoutSenderFixture, ReceiversCancelled) {
  auto fanoutSender = FanoutSender<int>();

  auto [handle1, callback1] = processValues(fanoutSender.subscribe());
  auto [handle2, callback2] = processValues(fanoutSender.subscribe());

  executor_.drain();

  EXPECT_TRUE(fanoutSender.anySubscribers());

  EXPECT_CALL(*callback1, onValue(1));
  EXPECT_CALL(*callback2, onValue(1));

  fanoutSender.write(1);
  executor_.drain();

  EXPECT_TRUE(fanoutSender.anySubscribers());

  EXPECT_CALL(*callback1, onCancelled());

  handle1.reset();
  executor_.drain();

  EXPECT_CALL(*callback2, onValue(2));

  fanoutSender.write(2);
  executor_.drain();

  EXPECT_TRUE(fanoutSender.anySubscribers());

  EXPECT_CALL(*callback2, onCancelled());

  handle2.reset();
  executor_.drain();

  EXPECT_FALSE(fanoutSender.anySubscribers());

  fanoutSender.write(3);
  executor_.drain();

  std::move(fanoutSender).close();
  executor_.drain();
}

TEST_F(FanoutSenderFixture, ReceiverCancelled_DelayedCancellationCallback) {
  auto fanoutSender = FanoutSender<int>();

  auto receiver1 = fanoutSender.subscribe();
  auto [handle2, callback2] = processValues(fanoutSender.subscribe());

  auto [bridge1, _] = detail::receiverUnbuffer(std::move(receiver1));

  // This call prevents the fanout sender from receiving the cancellation
  // callback from receiver1. We will instead call that callback ourselves
  // below, to simulate the case where the cancellation has occurred but has not
  // yet been processed by the fanout sender at the time the fanout sender
  // attempts to destroy its internal processor.
  auto callback = bridge1->cancelSenderWait();

  // This call actually cancels receiver1.
  bridge1->receiverCancel();

  EXPECT_CALL(*callback2, onValue(1));

  fanoutSender.write(1);
  executor_.drain();

  // This call triggers the cancellation callback that would have occurred
  // before, if we had not cancelled it. This should trigger the destruction of
  // the FanoutSenderProcessor.
  callback->consume(bridge1.get());

  EXPECT_CALL(*callback2, onClosed());
  std::move(fanoutSender).close();
  executor_.drain();
}

TEST_F(FanoutSenderFixture, Close_DelayedCancellationCallback) {
  auto fanoutSender = FanoutSender<int>();

  auto receiver1 = fanoutSender.subscribe();
  auto receiver2 = fanoutSender.subscribe();

  auto [bridge1, _] = detail::receiverUnbuffer(std::move(receiver1));

  // This call prevents the fanout sender from receiving the cancellation
  // callback from receiver1. We will instead call that callback ourselves
  // below, to simulate the case where the cancellation has occurred but has not
  // yet been processed by the fanout sender at the time the fanout sender was
  // closed.
  auto callback = bridge1->cancelSenderWait();

  // This call actually cancels receiver1.
  bridge1->receiverCancel();

  std::move(fanoutSender).close();

  // This call triggers the cancellation callback that would have occurred
  // before, if we had not cancelled it. This should trigger the destruction of
  // the FanoutSenderProcessor.
  callback->consume(bridge1.get());
}

TEST_F(FanoutSenderFixture, NumSubscribers) {
  auto sender = FanoutSender<int>{};
  EXPECT_EQ(sender.numSubscribers(), 0);

  auto receiver1 = std::make_unique<Receiver<int>>(sender.subscribe());
  EXPECT_EQ(sender.numSubscribers(), 1);

  auto receiver2 = std::make_unique<Receiver<int>>(sender.subscribe());
  EXPECT_EQ(sender.numSubscribers(), 2);

  auto receiver3 = std::make_unique<Receiver<int>>(sender.subscribe());
  EXPECT_EQ(sender.numSubscribers(), 3);
}

TEST_F(FanoutSenderFixture, MoveAssign_TransfersSingleSubscriberFromSource) {
  auto source = FanoutSender<int>();
  auto [handle, callback] = processValues(source.subscribe());
  executor_.drain();

  auto dest = FanoutSender<int>();
  dest = std::move(source);

  EXPECT_EQ(dest.numSubscribers(), 1);

  EXPECT_CALL(*callback, onValue(42));
  dest.write(42);
  executor_.drain();

  EXPECT_CALL(*callback, onClosed());
  std::move(dest).close();
  executor_.drain();
}

TEST_F(
    FanoutSenderFixture,
    MoveAssign_ClosesExistingSubscribersAndTransfersFromSource) {
  auto dest = FanoutSender<int>();
  auto [destHandle, destCallback] = processValues(dest.subscribe());

  auto source = FanoutSender<int>();
  auto [srcHandle1, srcCallback1] = processValues(source.subscribe());
  auto [srcHandle2, srcCallback2] = processValues(source.subscribe());
  executor_.drain();

  EXPECT_CALL(*destCallback, onClosed());

  dest = std::move(source);
  executor_.drain();

  EXPECT_EQ(dest.numSubscribers(), 2);

  EXPECT_CALL(*srcCallback1, onValue(7));
  EXPECT_CALL(*srcCallback2, onValue(7));
  dest.write(7);
  executor_.drain();

  EXPECT_CALL(*srcCallback1, onClosed());
  EXPECT_CALL(*srcCallback2, onClosed());
  std::move(dest).close();
  executor_.drain();
}
} // namespace channels
} // namespace folly
