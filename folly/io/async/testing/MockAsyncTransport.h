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

#pragma once

#include <folly/Memory.h>
#include <folly/io/async/AsyncSocketException.h>
#include <folly/io/async/AsyncTransport.h>
#include <folly/portability/GMock.h>

namespace folly {
namespace test {

class MockAsyncTransport : public AsyncTransport {
 public:
  MOCK_METHOD(void, setReadCB, (ReadCallback*));
  MOCK_METHOD(ReadCallback*, getReadCallback, (), (const));
  MOCK_METHOD(ReadCallback*, getReadCB, (), (const));
  MOCK_METHOD(void, write, (WriteCallback*, const void*, size_t, WriteFlags));
  MOCK_METHOD(void, writev, (WriteCallback*, const iovec*, size_t, WriteFlags));
  MOCK_METHOD(
      void,
      writeChain,
      (WriteCallback*, std::shared_ptr<folly::IOBuf>, WriteFlags));

  void writeChain(
      WriteCallback* callback,
      std::unique_ptr<folly::IOBuf>&& iob,
      WriteFlags flags = WriteFlags::NONE) override {
    writeChain(callback, std::shared_ptr<folly::IOBuf>(iob.release()), flags);
  }

  MOCK_METHOD(void, close, ());
  MOCK_METHOD(void, closeNow, ());
  MOCK_METHOD(void, closeWithReset, ());
  MOCK_METHOD(void, shutdownWrite, ());
  MOCK_METHOD(void, shutdownWriteNow, ());
  MOCK_METHOD(bool, good, (), (const));
  MOCK_METHOD(bool, readable, (), (const));
  MOCK_METHOD(bool, connecting, (), (const));
  MOCK_METHOD(bool, error, (), (const));
  MOCK_METHOD(void, attachEventBase, (EventBase*));
  MOCK_METHOD(void, detachEventBase, ());
  MOCK_METHOD(bool, isDetachable, (), (const));
  MOCK_METHOD(EventBase*, getEventBase, (), (const));
  MOCK_METHOD(void, setSendTimeout, (uint32_t));
  MOCK_METHOD(uint32_t, getSendTimeout, (), (const));
  MOCK_METHOD(void, getLocalAddress, (folly::SocketAddress*), (const));
  MOCK_METHOD(void, getPeerAddress, (folly::SocketAddress*), (const));
  MOCK_METHOD(size_t, getAppBytesWritten, (), (const));
  MOCK_METHOD(size_t, getRawBytesWritten, (), (const));
  MOCK_METHOD(size_t, getAppBytesReceived, (), (const));
  MOCK_METHOD(size_t, getRawBytesReceived, (), (const));
  MOCK_METHOD(size_t, getAppBytesBuffered, (), (const));
  MOCK_METHOD(size_t, getRawBytesBuffered, (), (const));
  MOCK_METHOD(bool, isEorTrackingEnabled, (), (const));
  MOCK_METHOD(void, setEorTracking, (bool));
  MOCK_METHOD(AsyncTransport*, getWrappedTransport, (), (const));
  MOCK_METHOD(bool, isReplaySafe, (), (const));
  MOCK_METHOD(
      void, setReplaySafetyCallback, (AsyncTransport::ReplaySafetyCallback*));
  MOCK_METHOD(std::string, getSecurityProtocol, (), (const));
  MOCK_METHOD(
      const AsyncTransportCertificate*, getPeerCertificate, (), (const));
};

class MockReplaySafetyCallback : public AsyncTransport::ReplaySafetyCallback {
 public:
  MOCK_METHOD(void, onReplaySafe_, (), (noexcept));
  void onReplaySafe() noexcept override { onReplaySafe_(); }
};

class MockReadCallback : public AsyncTransport::ReadCallback {
 public:
  MOCK_METHOD(void, getReadBuffer, (void**, size_t*));

  MOCK_METHOD(void, readDataAvailable_, (size_t), (noexcept));
  void readDataAvailable(size_t size) noexcept override {
    readDataAvailable_(size);
  }

  MOCK_METHOD(bool, isBufferMovable_, (), (noexcept));
  bool isBufferMovable() noexcept override { return isBufferMovable_(); }

  MOCK_METHOD(void, readBufferAvailable_, (std::unique_ptr<folly::IOBuf>&));
  void readBufferAvailable(
      std::unique_ptr<folly::IOBuf> readBuf) noexcept override {
    readBufferAvailable_(readBuf);
  }

  MOCK_METHOD(void, readEOF_, (), (noexcept));
  void readEOF() noexcept override { readEOF_(); }

  MOCK_METHOD(void, readErr_, (const AsyncSocketException&), (noexcept));
  void readErr(const AsyncSocketException& ex) noexcept override {
    readErr_(ex);
  }
};

class MockWriteCallback : public AsyncTransport::WriteCallback {
 public:
  MOCK_METHOD(void, writeSuccess_, (), (noexcept));
  void writeSuccess() noexcept override { writeSuccess_(); }

  MOCK_METHOD(
      void, writeErr_, (size_t, const AsyncSocketException&), (noexcept));
  void writeErr(size_t size, const AsyncSocketException& ex) noexcept override {
    writeErr_(size, ex);
  }
};

} // namespace test
} // namespace folly
