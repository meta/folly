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

#include <folly/io/async/test/IoUringTestUtil.h>

#include <thread>
#include <tuple>

#include <glog/logging.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/IoUringSetupRetry.h>
#include <folly/io/async/test/AsyncSocketTest.h>

#if FOLLY_HAS_LIBURING

namespace folly::test {

IoUringBackend::Options ioUringOptions() {
  IoUringBackend::Options options;
  options.setCapacity(64).setMaxSubmit(32);
  return options;
}

IoUringBackend::Options ioUringOptionsWithProvidedBuffers() {
  auto options = ioUringOptions();
  options.setInitialProvidedBuffers(2048, 256);
  return options;
}

std::unique_ptr<EventBase> makeIoUringEventBase(
    IoUringBackend::Options (*makeOptions)()) {
  return detail::retryTransientIoUringSetup<IoUringBackend::OutOfMemory>(
      [makeOptions] {
        return std::make_unique<EventBase>(
            EventBase::Options{}.setBackendFactory(
                [makeOptions]() -> std::unique_ptr<EventBaseBackendBase> {
                  return std::make_unique<IoUringBackend>(makeOptions());
                }));
      });
}

IoUringBackend::IoUringStats ioUringStats(EventBase& evb) {
  return CHECK_NOTNULL(dynamic_cast<IoUringBackend*>(evb.getBackend()))
      ->getAndResetStats();
}

std::string makePayload(size_t len) {
  std::string payload(len, '\0');
  for (size_t i = 0; i < len; ++i) {
    payload[i] = static_cast<char>(i % 251);
  }
  return payload;
}

void RetainingReadCallback::getReadBuffer(void** buf, size_t* len) {
  std::tie(*buf, *len) = received_.preallocate(4096, 65536);
}

void RetainingReadCallback::readDataAvailable(size_t len) noexcept {
  received_.postallocate(len);
}

void RetainingReadCallback::readBufferAvailable(
    std::unique_ptr<IOBuf> buf) noexcept {
  received_.append(std::move(buf));
}

void RetainingReadCallback::readErr(const AsyncSocketException& ex) noexcept {
  ADD_FAILURE() << ex.what();
}

std::string RetainingReadCallback::data() const {
  return received_.empty() ? "" : received_.front()->to<std::string>();
}

void receiveFromPeer(
    EventBase& evb,
    AsyncTransport::ReadCallback& rcb,
    const std::string& payload) {
  TestServer server;
  auto socket = AsyncSocket::newSocket(&evb);
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);
  evb.loop();
  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);

  auto peer = server.accept();
  std::thread writer([&] {
    peer->write(
        reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    peer->close();
  });
  socket->setReadCB(&rcb);
  evb.loop();
  writer.join();
}

void sendToPeer(
    EventBase& evb,
    const std::string& payload,
    WriteFlags flags,
    AsyncWriter::ZeroCopyEnableFunc enableFunc) {
  TestServer server;
  auto socket = AsyncSocket::newSocket(&evb);
  ASSERT_TRUE(socket->setZeroCopy(true));
  if (enableFunc) {
    socket->setZeroCopyEnableFunc(std::move(enableFunc));
  }
  ConnCallback ccb;
  socket->connect(&ccb, server.getAddress(), 30);
  evb.loop();
  ASSERT_EQ(ccb.state, STATE_SUCCEEDED);

  std::thread reader([&] {
    server.verifyConnection(payload.data(), payload.size());
  });
  WriteCallback wcb(true /*enableReleaseIOBufCallback*/);
  socket->writeChain(&wcb, IOBuf::copyBuffer(payload), flags);
  evb.loop();
  socket->close();
  reader.join();
  ASSERT_EQ(wcb.state, STATE_SUCCEEDED);
}

} // namespace folly::test

#endif
