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

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <folly/ScopeGuard.h>
#include <folly/io/async/AsyncSocketException.h>
#include <folly/io/async/EventHandler.h>
#include <folly/io/async/IoUringRecv.h>
#include <folly/io/async/test/IoUringTestUtil.h>
#include <folly/net/NetOps.h>

namespace folly {

#if FOLLY_HAS_LIBURING
namespace {

IoUringBackend::Options ioUringOptionsWithTwoProvidedBuffers() {
  auto options = test::ioUringOptions();
  options.setInitialProvidedBuffers(2048, 2);
  return options;
}

class NoopRecvCallback final : public IoUringRecvCallback {
 public:
  void recvSuccess(std::unique_ptr<IOBuf> /*data*/) override {}
  void recvSuccessWithCmsgs(
      std::unique_ptr<IOBuf> /*data*/,
      detail::IoUringRecvmsgControl /*control*/) override {}
  void recvEOF() noexcept override {}
  void recvErr(
      int /*err*/,
      std::unique_ptr<const AsyncSocketException> /*exception*/) noexcept
      override {}
  void recvBuffersScarce(bool /*scarce*/) noexcept override {}
};

class RecordingRecvCallback final : public IoUringRecvCallback {
 public:
  explicit RecordingRecvCallback(EventBase* evb) : evb_(evb) {}

  void recvSuccess(std::unique_ptr<IOBuf> data) override {
    recordData(std::move(data));
  }
  void recvSuccessWithCmsgs(
      std::unique_ptr<IOBuf> data,
      detail::IoUringRecvmsgControl /*control*/) override {
    recordData(std::move(data));
  }
  void recvEOF() noexcept override { evb_->terminateLoopSoon(); }
  void recvErr(
      int err,
      std::unique_ptr<const AsyncSocketException> /*exception*/) noexcept
      override {
    recvError = err;
    evb_->terminateLoopSoon();
  }
  void recvBuffersScarce(bool /*scarce*/) noexcept override {}

  std::vector<std::unique_ptr<IOBuf>> receivedData;
  int recvError{0};

 private:
  void recordData(std::unique_ptr<IOBuf> data) {
    receivedData.push_back(std::move(data));
    evb_->terminateLoopSoon();
  }

  EventBase* evb_;
};

class IoUringRecvTest : public test::IoUringTest {};

TEST_F(IoUringRecvTest, CreateRejectsBufferlessBackend) {
  auto evb = test::makeIoUringEventBase(test::ioUringOptions);
  NoopRecvCallback callback;

  EXPECT_EQ(
      nullptr,
      IoUringRecvHandle::create(
          evb.get(), NetworkSocket{}, SocketAddress{}, &callback));
  EXPECT_EQ(
      nullptr,
      IoUringRecvHandle::create(
          evb.get(), NetworkSocket{}, SocketAddress{}, &callback, true));
}

TEST_F(IoUringRecvTest, FallbackRejectsMsgCtrunc) {
  auto evb = test::makeIoUringEventBase(ioUringOptionsWithTwoProvidedBuffers);
  NetworkSocket sockets[2];
  ASSERT_EQ(0, netops::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
  SCOPE_EXIT {
    netops::close(sockets[0]);
    netops::close(sockets[1]);
  };

  RecordingRecvCallback callback(evb.get());
  auto handle = IoUringRecvHandle::create(
      evb.get(), sockets[1], SocketAddress{}, &callback, true);
  ASSERT_NE(nullptr, handle);
  ASSERT_TRUE(handle->update(EventHandler::READ));
  handle->submit();

  constexpr std::array<std::string_view, 2> kInitialPayloads{
      "consume first provided buffer", "consume second provided buffer"};
  for (const auto payload : kInitialPayloads) {
    ASSERT_EQ(
        static_cast<ssize_t>(payload.size()),
        netops::send(sockets[0], payload.data(), payload.size(), 0));
    evb->loop();
  }
  ASSERT_EQ(kInitialPayloads.size(), callback.receivedData.size());
  const std::vector<std::string> expectedPayloads(
      kInitialPayloads.begin(), kInitialPayloads.end());
  const auto getReceivedPayloads = [&callback] {
    std::vector<std::string> payloads;
    payloads.reserve(callback.receivedData.size());
    for (const auto& data : callback.receivedData) {
      payloads.push_back(data ? data->toString() : std::string{});
    }
    return payloads;
  };
  EXPECT_EQ(expectedPayloads, getReceivedPayloads());

  // Holding both IOBufs exhausts the ring. The next receive falls back to
  // one-shot recvmsg, whose control buffer cannot fit ucred.
  int passCredentials = 1;
  ASSERT_EQ(
      0,
      netops::setsockopt(
          sockets[1],
          SOL_SOCKET,
          SO_PASSCRED,
          &passCredentials,
          sizeof(passCredentials)));
  constexpr char kSecondPayload[] = "truncated control";
  ASSERT_EQ(
      sizeof(kSecondPayload) - 1,
      netops::send(sockets[0], kSecondPayload, sizeof(kSecondPayload) - 1, 0));
  evb->loop();

  EXPECT_EQ(expectedPayloads, getReceivedPayloads());
  EXPECT_EQ(EBADMSG, callback.recvError);
  handle->cancel();
}

TEST_F(IoUringRecvTest, CloneRejectsBufferlessBackend) {
  auto sourceEvb =
      test::makeIoUringEventBase(test::ioUringOptionsWithProvidedBuffers);
  auto targetEvb = test::makeIoUringEventBase(test::ioUringOptions);
  NoopRecvCallback callback;

  auto other = IoUringRecvHandle::create(
      sourceEvb.get(), NetworkSocket{}, SocketAddress{}, &callback, true);
  ASSERT_NE(nullptr, other);
  other->detachEventBase();
  EXPECT_EQ(
      nullptr,
      IoUringRecvHandle::clone(
          targetEvb.get(),
          NetworkSocket{},
          SocketAddress{},
          &callback,
          std::move(other)));

  other = IoUringRecvHandle::create(
      sourceEvb.get(), NetworkSocket{}, SocketAddress{}, &callback, true);
  ASSERT_NE(nullptr, other);
  other->detachEventBase();
  EXPECT_EQ(
      nullptr,
      IoUringRecvHandle::clone(
          targetEvb.get(),
          NetworkSocket{},
          SocketAddress{},
          &callback,
          std::move(other),
          true));
}

} // namespace
#endif

} // namespace folly
