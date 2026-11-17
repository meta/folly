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

#include <folly/channels/ProxyChannel.h>
#include <folly/channels/test/ChannelTestUtil.h>
#include <folly/channels/testing/MockNextCallback.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

namespace folly {
namespace channels {

using namespace testing;

class ProxyChannelFixture : public Test {
 protected:
  ~ProxyChannelFixture() { executor_.drain(); }

  ChannelCallbackHandle processValues(Receiver<int> receiver) {
    return consumeChannelWithCallback(
        std::move(receiver),
        &executor_,
        [=, this](folly::Try<int> resultTry) -> folly::coro::Task<bool> {
          onNext_(std::move(resultTry));
          co_return true;
        });
  }

  folly::ManualExecutor executor_;
  StrictMock<MockNextCallback<int>> onNext_;
};

TEST_F(ProxyChannelFixture, ReceiveValues) {
  auto [inputReceiver, inputSender] = Channel<int>::create();
  auto [outputReceiver, proxyChannel] = createProxyChannel<int>(&executor_);

  inputSender.write(1);
  inputSender.write(2);
  executor_.drain();

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onValue(2));
  EXPECT_CALL(onNext_, onValue(3));
  EXPECT_CALL(onNext_, onValue(4));
  EXPECT_CALL(onNext_, onClosed());

  auto callbackHandle = processValues(std::move(outputReceiver));
  proxyChannel.setInputReceiver(std::move(inputReceiver));
  executor_.drain();

  inputSender.write(3);
  inputSender.write(4);
  std::move(inputSender).close();
  executor_.drain();
}

TEST_F(ProxyChannelFixture, InputSenderClosedWithException) {
  auto [inputReceiver, inputSender] = Channel<int>::create();
  auto [outputReceiver, proxyChannel] = createProxyChannel<int>(&executor_);

  inputSender.write(1);
  inputSender.write(2);
  executor_.drain();

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onValue(2));
  EXPECT_CALL(onNext_, onValue(3));
  EXPECT_CALL(onNext_, onValue(4));
  EXPECT_CALL(onNext_, onRuntimeError("std::runtime_error: Error"));

  auto callbackHandle = processValues(std::move(outputReceiver));
  proxyChannel.setInputReceiver(std::move(inputReceiver));
  executor_.drain();

  inputSender.write(3);
  inputSender.write(4);
  std::move(inputSender)
      .close(folly::make_exception_wrapper<std::runtime_error>("Error"));
  executor_.drain();
}

TEST_F(ProxyChannelFixture, ReplaceInputReceiver_BeforeFirstReceiverClosed) {
  auto [inputReceiver1, inputSender1] = Channel<int>::create();
  auto [inputReceiver2, inputSender2] = Channel<int>::create();
  auto [outputReceiver, proxyChannel] = createProxyChannel<int>(&executor_);

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onValue(2));
  EXPECT_CALL(onNext_, onValue(3));
  EXPECT_CALL(onNext_, onValue(4));
  EXPECT_CALL(onNext_, onValue(5));
  EXPECT_CALL(onNext_, onValue(6));
  EXPECT_CALL(onNext_, onClosed());

  auto callbackHandle = processValues(std::move(outputReceiver));
  proxyChannel.setInputReceiver(std::move(inputReceiver1));
  executor_.drain();

  inputSender1.write(1);
  inputSender1.write(2);
  executor_.drain();

  proxyChannel.setInputReceiver(std::move(inputReceiver2));
  executor_.drain();

  inputSender2.write(3);
  inputSender2.write(4);
  executor_.drain();

  inputSender1.write(100);
  inputSender1.write(200);
  std::move(inputSender1).close();

  inputSender2.write(5);
  inputSender2.write(6);
  executor_.drain();

  std::move(inputSender2).close();
  executor_.drain();
}

TEST_F(ProxyChannelFixture, ReplaceInputReceiver_AfterFirstReceiverClosed) {
  auto [inputReceiver1, inputSender1] = Channel<int>::create();
  auto [inputReceiver2, inputSender2] = Channel<int>::create();
  auto [outputReceiver, proxyChannel] = createProxyChannel<int>(&executor_);

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onValue(2));
  EXPECT_CALL(onNext_, onClosed());

  auto callbackHandle = processValues(std::move(outputReceiver));
  proxyChannel.setInputReceiver(std::move(inputReceiver1));
  executor_.drain();

  inputSender1.write(1);
  inputSender1.write(2);
  std::move(inputSender1).close();
  executor_.drain();

  proxyChannel.setInputReceiver(std::move(inputReceiver2));
  executor_.drain();

  inputSender2.write(3);
  inputSender2.write(4);
  std::move(inputSender2).close();
  executor_.drain();
}

TEST_F(ProxyChannelFixture, ReplaceInputReceiver_AfterFirstReceiverRemoved) {
  auto [inputReceiver1, inputSender1] = Channel<int>::create();
  auto [inputReceiver2, inputSender2] = Channel<int>::create();
  auto [outputReceiver, proxyChannel] = createProxyChannel<int>(&executor_);

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onValue(2));
  EXPECT_CALL(onNext_, onValue(3));
  EXPECT_CALL(onNext_, onValue(4));
  EXPECT_CALL(onNext_, onClosed());

  auto callbackHandle = processValues(std::move(outputReceiver));
  proxyChannel.setInputReceiver(std::move(inputReceiver1));
  executor_.drain();

  inputSender1.write(1);
  inputSender1.write(2);
  executor_.drain();

  proxyChannel.removeInputReceiver();
  executor_.drain();

  inputSender1.write(100);
  inputSender1.write(200);
  std::move(inputSender1).close();

  proxyChannel.setInputReceiver(std::move(inputReceiver2));
  executor_.drain();

  inputSender2.write(3);
  inputSender2.write(4);
  executor_.drain();

  std::move(inputSender2).close();
  executor_.drain();
}

TEST_F(
    ProxyChannelFixture, ProxyChannelClosed_NoException_NoMoreValuesReceived) {
  auto [inputReceiver, inputSender] = Channel<int>::create();
  auto [outputReceiver, proxyChannel] = createProxyChannel<int>(&executor_);

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onValue(2));
  EXPECT_CALL(onNext_, onClosed());

  auto callbackHandle = processValues(std::move(outputReceiver));
  proxyChannel.setInputReceiver(std::move(inputReceiver));
  executor_.drain();

  inputSender.write(1);
  inputSender.write(2);
  executor_.drain();

  std::move(proxyChannel).close();
  executor_.drain();

  inputSender.write(3);
  inputSender.write(4);
  std::move(inputSender).close();
  executor_.drain();
}

TEST_F(
    ProxyChannelFixture,
    ProxyChannelClosed_WithException_NoMoreValuesReceived) {
  auto [inputReceiver, inputSender] = Channel<int>::create();
  auto [outputReceiver, proxyChannel] = createProxyChannel<int>(&executor_);

  EXPECT_CALL(onNext_, onValue(1));
  EXPECT_CALL(onNext_, onValue(2));
  EXPECT_CALL(onNext_, onRuntimeError("std::runtime_error: Error"));

  auto callbackHandle = processValues(std::move(outputReceiver));
  proxyChannel.setInputReceiver(std::move(inputReceiver));
  executor_.drain();

  inputSender.write(1);
  inputSender.write(2);
  executor_.drain();

  std::move(proxyChannel)
      .close(folly::make_exception_wrapper<std::runtime_error>("Error"));
  executor_.drain();

  inputSender.write(3);
  inputSender.write(4);
  std::move(inputSender).close();
  executor_.drain();
}
} // namespace channels
} // namespace folly
