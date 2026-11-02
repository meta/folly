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

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>

#include <condition_variable>
#include <iostream>
#include <list>
#include <memory>

#include <folly/ExceptionWrapper.h>
#include <folly/SocketAddress.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/SocketOptionMap.h>
#include <folly/io/async/AsyncSSLSocket.h>
#include <folly/io/async/AsyncServerSocket.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/AsyncTransport.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/ssl/SSLErrors.h>
#include <folly/io/async/test/TestSSLServer.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <folly/portability/PThread.h>
#include <folly/portability/Sockets.h>
#include <folly/portability/String.h>
#include <folly/portability/Unistd.h>
#include <folly/testing/TestUtil.h>

namespace folly::test {

// The destructors of all callback classes assert that the state is
// STATE_SUCCEEDED, for both possitive and negative tests. The tests
// are responsible for setting the succeeded state properly before the
// destructors are called.

class SendMsgParamsCallbackBase
    : public folly::AsyncSocket::SendMsgParamsCallback {
 public:
  SendMsgParamsCallbackBase() {}

  void setSocket(const std::shared_ptr<AsyncSSLSocket>& socket) {
    socket_ = socket;
    oldCallback_ = socket_->getSendMsgParamsCB();
    socket_->setSendMsgParamCB(this);
    socket_->setEorTracking(trackEor_);
  }

  void setEorTracking(bool track) {
    CHECK(!socket_); // should only be called during setup
    trackEor_ = track;
  }

  int getFlagsImpl(
      folly::WriteFlags flags, int /*defaultFlags*/) noexcept override {
    return oldCallback_->getFlags(flags, false /*zeroCopyEnabled*/);
  }

  void getAncillaryData(
      folly::WriteFlags flags,
      void* data,
      const AsyncSocket::WriteRequestTag& writeTag,
      const bool byteEventsEnabled) noexcept override {
    oldCallback_->getAncillaryData(flags, data, writeTag, byteEventsEnabled);
  }

  uint32_t getAncillaryDataSize(
      folly::WriteFlags flags,
      const AsyncSocket::WriteRequestTag& writeTag,
      const bool byteEventsEnabled) noexcept override {
    return oldCallback_->getAncillaryDataSize(
        flags, writeTag, byteEventsEnabled);
  }

  std::shared_ptr<AsyncSSLSocket> socket_;
  bool trackEor_{false};
  folly::AsyncSocket::SendMsgParamsCallback* oldCallback_{nullptr};
};

class SendMsgFlagsCallback : public SendMsgParamsCallbackBase {
 public:
  SendMsgFlagsCallback() {}

  void resetFlags(int flags) { flags_ = flags; }

  int getFlagsImpl(
      folly::WriteFlags flags, int /*defaultFlags*/) noexcept override {
    if (flags_) {
      return flags_;
    } else {
      return oldCallback_->getFlags(flags, false /*zeroCopyEnabled*/);
    }
  }

  int flags_{0};
};

class SendMsgAncillaryDataCallback : public SendMsgParamsCallbackBase {
 public:
  SendMsgAncillaryDataCallback() {}

  /**
   * This data will be returned on calls to getAncillaryData.
   */
  void resetData(std::vector<char>&& data) { ancillaryData_.swap(data); }

  /**
   * These flags were observed on the last call to getAncillaryData.
   */
  folly::WriteFlags getObservedWriteFlags() { return observedWriteFlags_; }

  void getAncillaryData(
      folly::WriteFlags flags,
      void* data,
      const AsyncSocket::WriteRequestTag& writeTag,
      const bool byteEventsEnabled) noexcept override {
    // getAncillaryData is called through a long chain of functions after send
    // record the observed write flags so we can compare later
    observedWriteFlags_ = flags;

    if (ancillaryData_.size()) {
      std::cerr << "getAncillaryData: copying data" << std::endl;
      memcpy(data, ancillaryData_.data(), ancillaryData_.size());
    } else {
      oldCallback_->getAncillaryData(flags, data, writeTag, byteEventsEnabled);
    }
  }

  uint32_t getAncillaryDataSize(
      folly::WriteFlags flags,
      const AsyncSocket::WriteRequestTag& writeTag,
      const bool byteEventsEnabled) noexcept override {
    if (ancillaryData_.size()) {
      std::cerr << "getAncillaryDataSize: returning size" << std::endl;
      return ancillaryData_.size();
    } else {
      return oldCallback_->getAncillaryDataSize(
          flags, writeTag, byteEventsEnabled);
    }
  }

  folly::WriteFlags observedWriteFlags_{};
  std::vector<char> ancillaryData_;
};

class WriteCallbackBase : public AsyncTransport::WriteCallback {
 public:
  explicit WriteCallbackBase(SendMsgParamsCallbackBase* mcb = nullptr)
      : state(STATE_WAITING),
        bytesWritten(0),
        exception(AsyncSocketException::UNKNOWN, "none"),
        mcb_(mcb) {}

  ~WriteCallbackBase() override { EXPECT_EQ(STATE_SUCCEEDED, state); }

  SemiFuture<StateEnum> getSemiFuture() { return promise_.getSemiFuture(); }

  virtual void setSocket(const std::shared_ptr<AsyncSSLSocket>& socket) {
    socket_ = socket;
    if (mcb_) {
      mcb_->setSocket(socket);
    }
  }

  void writeSuccess() noexcept override {
    std::cerr << "writeSuccess" << std::endl;
    state = STATE_SUCCEEDED;
    if (!promise_.isFulfilled()) {
      promise_.setValue(state);
    }
  }

  void writeErr(
      size_t nBytesWritten, const AsyncSocketException& ex) noexcept override {
    std::cerr << "writeError: bytesWritten " << nBytesWritten << ", exception "
              << ex.what() << std::endl;

    state = STATE_FAILED;
    if (!promise_.isFulfilled()) {
      promise_.setValue(state);
    }
    this->bytesWritten = nBytesWritten;
    exception = ex;
    socket_->close();
  }

  std::shared_ptr<AsyncSSLSocket> socket_;
  StateEnum state;
  size_t bytesWritten;
  AsyncSocketException exception;
  SendMsgParamsCallbackBase* mcb_;
  Promise<StateEnum> promise_;
};

class ExpectWriteErrorCallback : public WriteCallbackBase {
 public:
  explicit ExpectWriteErrorCallback(SendMsgParamsCallbackBase* mcb = nullptr)
      : WriteCallbackBase(mcb) {}

  ~ExpectWriteErrorCallback() override {
    EXPECT_EQ(STATE_FAILED, state);
    EXPECT_EQ(
        exception.getType(),
        AsyncSocketException::AsyncSocketExceptionType::NETWORK_ERROR);
    // sendmsg() rejects the invalid flags, but the exact errno is
    // kernel-dependent: kernels that validate the flags first fail with
    // EINVAL (22), while kernels that act on the MSG_FASTOPEN bit set in
    // -1 first fail with EISCONN (106) on the connected socket. Either
    // proves the write failed as the test intends. (Numeric literals:
    // EISCONN is unavailable on Windows builds.)
    EXPECT_TRUE(exception.getErrno() == 22 || exception.getErrno() == 106);
    // Suppress the assert in  ~WriteCallbackBase()
    state = STATE_SUCCEEDED;
  }
};

class ExpectSSLWriteErrorCallback : public WriteCallbackBase {
 public:
  explicit ExpectSSLWriteErrorCallback(SendMsgParamsCallbackBase* mcb = nullptr)
      : WriteCallbackBase(mcb) {}

  ~ExpectSSLWriteErrorCallback() override {
    EXPECT_EQ(STATE_FAILED, state);
    EXPECT_EQ(
        exception.getType(),
        AsyncSocketException::AsyncSocketExceptionType::SSL_ERROR);
    // Suppress the assert in  ~WriteCallbackBase()
    state = STATE_SUCCEEDED;
  }
};

class ReadCallbackBase : public AsyncTransport::ReadCallback {
 public:
  explicit ReadCallbackBase(WriteCallbackBase* wcb)
      : wcb_(wcb), state(STATE_WAITING) {}

  ~ReadCallbackBase() override { EXPECT_EQ(STATE_SUCCEEDED, state); }

  void setSocket(const std::shared_ptr<AsyncSSLSocket>& socket) {
    socket_ = socket;
  }

  void setState(StateEnum s) {
    state = s;
    if (wcb_) {
      wcb_->state = s;
    }
  }

  void readErr(const AsyncSocketException& ex) noexcept override {
    std::cerr << "readError " << ex.what() << std::endl;
    state = STATE_FAILED;
    socket_->close();
  }

  void readEOF() noexcept override {
    std::cerr << "readEOF" << std::endl;
    socket_->close();
  }

  std::shared_ptr<AsyncSSLSocket> socket_;
  WriteCallbackBase* wcb_;
  StateEnum state;
};

/**
 * ReadCallback reads data from the socket and then writes it back.
 *
 * It includes any folly::WriteFlags set via setWriteFlags(...) in its write
 * back operation.
 */
class ReadCallback : public ReadCallbackBase {
 public:
  explicit ReadCallback(WriteCallbackBase* wcb, bool reflect = true)
      : ReadCallbackBase(wcb),
        buffers(),
        writeFlags(folly::WriteFlags::NONE),
        reflect(reflect) {}

  explicit ReadCallback() : ReadCallback(nullptr, false) {}

  ~ReadCallback() override {
    for (std::vector<Buffer>::iterator it = buffers.begin();
         it != buffers.end();
         ++it) {
      it->free();
    }
    currentBuffer.free();
  }

  void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
    if (!currentBuffer.buffer) {
      currentBuffer.allocate(4096);
    }
    *bufReturn = currentBuffer.buffer;
    *lenReturn = currentBuffer.length;
  }

  void readDataAvailable(size_t len) noexcept override {
    std::cerr << "readDataAvailable, len " << len << std::endl;

    currentBuffer.length = len;

    if (wcb_) {
      wcb_->setSocket(socket_);
    }

    // Write back the same data.
    if (reflect) {
      socket_->write(wcb_, currentBuffer.buffer, len, writeFlags);
    }

    buffers.push_back(currentBuffer);
    currentBuffer.reset();
    state = STATE_SUCCEEDED;
  }

  void verifyData(const char* expected, size_t expectedLen) const {
    verifyData((const unsigned char*)expected, expectedLen);
  }

  void verifyData(const unsigned char* expected, size_t expectedLen) const {
    size_t offset = 0;
    for (size_t idx = 0; idx < buffers.size(); ++idx) {
      const auto& buf = buffers[idx];
      size_t cmpLen = std::min(buf.length, expectedLen - offset);
      CHECK_EQ(memcmp(buf.buffer, expected + offset, cmpLen), 0);
      CHECK_EQ(cmpLen, buf.length);
      offset += cmpLen;
    }
    CHECK_EQ(offset, expectedLen);
  }

  void clearData() {
    for (auto& buffer : buffers) {
      buffer.free();
    }
    buffers.clear();
  }

  size_t dataRead() const {
    size_t ret = 0;
    for (const auto& buf : buffers) {
      ret += buf.length;
    }
    return ret;
  }

  /**
   * These flags will be used when writing the read data back to the socket.
   */
  void setWriteFlags(folly::WriteFlags flags) { writeFlags = flags; }

  class Buffer {
   public:
    Buffer() : buffer(nullptr), length(0) {}
    Buffer(char* buf, size_t len) : buffer(buf), length(len) {}

    void reset() {
      buffer = nullptr;
      length = 0;
    }
    void allocate(size_t len) {
      assert(buffer == nullptr);
      this->buffer = static_cast<char*>(malloc(len));
      this->length = len;
    }
    void free() {
      ::free(buffer);
      reset();
    }

    char* buffer;
    size_t length;
  };

  std::vector<Buffer> buffers;
  Buffer currentBuffer;
  folly::WriteFlags writeFlags;
  bool reflect; // whether read bytes will be written back to the transport
};

class ReadErrorCallback : public ReadCallbackBase {
 public:
  explicit ReadErrorCallback(WriteCallbackBase* wcb) : ReadCallbackBase(wcb) {}

  // Return nullptr buffer to trigger readError()
  void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
    *bufReturn = nullptr;
    *lenReturn = 0;
  }

  void readDataAvailable(size_t /* len */) noexcept override {
    // This should never to called.
    FAIL();
  }

  void readErr(const AsyncSocketException& ex) noexcept override {
    ReadCallbackBase::readErr(ex);
    std::cerr << "ReadErrorCallback::readError" << std::endl;
    setState(STATE_SUCCEEDED);
  }
};

class ReadEOFCallback : public ReadCallbackBase {
 public:
  explicit ReadEOFCallback(WriteCallbackBase* wcb) : ReadCallbackBase(wcb) {}

  // Return nullptr buffer to trigger readError()
  void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
    *bufReturn = nullptr;
    *lenReturn = 0;
  }

  void readDataAvailable(size_t /* len */) noexcept override {
    // This should never to called.
    FAIL();
  }

  void readEOF() noexcept override {
    ReadCallbackBase::readEOF();
    setState(STATE_SUCCEEDED);
  }
};

class WriteErrorCallback : public ReadCallback {
 public:
  explicit WriteErrorCallback(WriteCallbackBase* wcb) : ReadCallback(wcb) {}

  void readDataAvailable(size_t len) noexcept override {
    std::cerr << "readDataAvailable, len " << len << std::endl;

    currentBuffer.length = len;

    // close the socket before writing to trigger writeError().
    netops::close(socket_->getNetworkSocket());

    wcb_->setSocket(socket_);

    // Write back the same data.
    folly::test::msvcSuppressAbortOnInvalidParams([&] {
      socket_->write(wcb_, currentBuffer.buffer, len);
    });

    if (wcb_->state == STATE_FAILED) {
      setState(STATE_SUCCEEDED);
    } else {
      state = STATE_FAILED;
    }

    buffers.push_back(currentBuffer);
    currentBuffer.reset();
  }

  void readErr(const AsyncSocketException& ex) noexcept override {
    std::cerr << "readError " << ex.what() << std::endl;
    // do nothing since this is expected
  }
};

class EmptyReadCallback : public ReadCallback {
 public:
  explicit EmptyReadCallback() : ReadCallback(nullptr) {}

  void readErr(const AsyncSocketException& ex) noexcept override {
    std::cerr << "readError " << ex.what() << std::endl;
    state = STATE_FAILED;
    if (tcpSocket_) {
      tcpSocket_->close();
    }
  }

  void readEOF() noexcept override {
    std::cerr << "readEOF" << std::endl;
    if (tcpSocket_) {
      tcpSocket_->close();
    }
    state = STATE_SUCCEEDED;
  }

  std::shared_ptr<AsyncSocket> tcpSocket_;
};

class MockCertificateIdentityVerifier : public CertificateIdentityVerifier {
 public:
  MOCK_METHOD(
      bool,
      verifyContext,
      (bool, X509_STORE_CTX*),
      (const, noexcept, override));
  MOCK_METHOD(
      std::unique_ptr<AsyncTransportCertificate>,
      verifyLeaf,
      (const AsyncTransportCertificate&),
      (const, override));
};

class MockHandshakeCB : public AsyncSSLSocket::HandshakeCB {
 public:
  MOCK_METHOD(bool, handshakeVerImpl, (AsyncSSLSocket*, bool, X509_STORE_CTX*));
  virtual bool handshakeVer(
      AsyncSSLSocket* sock,
      bool preverifyOk,
      X509_STORE_CTX* ctx) noexcept override {
    return handshakeVerImpl(sock, preverifyOk, ctx);
  }

  MOCK_METHOD(void, handshakeSucImpl, (AsyncSSLSocket*));
  virtual void handshakeSuc(AsyncSSLSocket* sock) noexcept override {
    handshakeSucImpl(sock);
  }

  MOCK_METHOD(
      void, handshakeErrImpl, (AsyncSSLSocket*, const AsyncSocketException&));
  virtual void handshakeErr(
      AsyncSSLSocket* sock, const AsyncSocketException& ex) noexcept override {
    handshakeErrImpl(sock, ex);
  }
};

class HandshakeCallback : public AsyncSSLSocket::HandshakeCB {
 public:
  enum ExpectType { EXPECT_SUCCESS, EXPECT_ERROR };

  explicit HandshakeCallback(
      ReadCallbackBase* rcb, ExpectType expect = EXPECT_SUCCESS)
      : state(STATE_WAITING), rcb_(rcb), expect_(expect) {}

  void setSocket(const std::shared_ptr<AsyncSSLSocket>& socket) {
    socket_ = socket;
  }

  void setState(StateEnum s) {
    state = s;
    rcb_->setState(s);
  }

  // Functions inherited from AsyncSSLSocketHandshakeCallback
  void handshakeSuc(AsyncSSLSocket* sock) noexcept override {
    isResumed_ = sock->getSSLSessionReused();
    std::lock_guard g(mutex_);
    cv_.notify_all();
    EXPECT_EQ(sock, socket_.get());
    std::cerr << "HandshakeCallback::connectionAccepted" << std::endl;
    rcb_->setSocket(socket_);
    sock->setReadCB(rcb_);
    state = (expect_ == EXPECT_SUCCESS) ? STATE_SUCCEEDED : STATE_FAILED;
  }
  void handshakeErr(
      AsyncSSLSocket* /* sock */,
      const AsyncSocketException& ex) noexcept override {
    isResumed_ = false;
    std::lock_guard g(mutex_);
    cv_.notify_all();
    std::cerr << "HandshakeCallback::handshakeError " << ex.what() << std::endl;
    state = (expect_ == EXPECT_ERROR) ? STATE_SUCCEEDED : STATE_FAILED;
    if (expect_ == EXPECT_ERROR) {
      // rcb will never be invoked
      rcb_->setState(STATE_SUCCEEDED);
    }
    errorString_ = ex.what();
  }

  void waitForHandshake() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return state != STATE_WAITING; });
  }

  ~HandshakeCallback() override { EXPECT_EQ(STATE_SUCCEEDED, state); }

  void closeSocket() {
    socket_->close();
    state = STATE_SUCCEEDED;
  }

  std::shared_ptr<AsyncSSLSocket> getSocket() { return socket_; }

  bool isResumed() const { return isResumed_; }

  StateEnum state;
  std::shared_ptr<AsyncSSLSocket> socket_;
  ReadCallbackBase* rcb_;
  ExpectType expect_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::string errorString_;
  bool isResumed_{false};
};

class SSLServerAcceptCallback : public SSLServerAcceptCallbackBase {
 public:
  uint32_t timeout_;

  explicit SSLServerAcceptCallback(HandshakeCallback* hcb, uint32_t timeout = 0)
      : SSLServerAcceptCallbackBase(hcb), timeout_(timeout) {}

  ~SSLServerAcceptCallback() override {
    if (timeout_ > 0) {
      // if we set a timeout, we expect failure
      EXPECT_EQ(hcb_->state, STATE_FAILED);
      hcb_->setState(STATE_SUCCEEDED);
    }
  }

  void connAccepted(
      const std::shared_ptr<folly::AsyncSSLSocket>& s) noexcept override {
    auto sock = std::static_pointer_cast<AsyncSSLSocket>(s);
    std::cerr << "SSLServerAcceptCallback::connAccepted" << std::endl;

    hcb_->setSocket(sock);
    sock->sslAccept(hcb_, std::chrono::milliseconds(timeout_));
    EXPECT_EQ(sock->getSSLState(), AsyncSSLSocket::STATE_ACCEPTING);

    state = STATE_SUCCEEDED;
  }
};

class SSLServerAcceptCallbackDelay : public SSLServerAcceptCallback {
 public:
  explicit SSLServerAcceptCallbackDelay(HandshakeCallback* hcb)
      : SSLServerAcceptCallback(hcb) {}

  void connAccepted(
      const std::shared_ptr<folly::AsyncSSLSocket>& s) noexcept override {
    auto sock = std::static_pointer_cast<AsyncSSLSocket>(s);

    std::cerr << "SSLServerAcceptCallbackDelay::connAccepted" << std::endl;
    auto fd = sock->getNetworkSocket();

#ifndef TCP_NOPUSH
    {
      // The accepted connection should already have TCP_NODELAY set
      int value;
      socklen_t valueLength = sizeof(value);
      int rc = netops::getsockopt(
          fd, IPPROTO_TCP, TCP_NODELAY, &value, &valueLength);
      EXPECT_EQ(rc, 0);
      EXPECT_EQ(value, 1);
    }
#endif

    // Unset the TCP_NODELAY option.
    int value = 0;
    socklen_t valueLength = sizeof(value);
    int rc =
        netops::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, valueLength);
    EXPECT_EQ(rc, 0);

    rc = netops::getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, &valueLength);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(value, 0);

    SSLServerAcceptCallback::connAccepted(sock);
  }
};

class HandshakeErrorCallback : public SSLServerAcceptCallbackBase {
 public:
  explicit HandshakeErrorCallback(HandshakeCallback* hcb)
      : SSLServerAcceptCallbackBase(hcb) {}

  void connAccepted(
      const std::shared_ptr<folly::AsyncSSLSocket>& s) noexcept override {
    auto sock = std::static_pointer_cast<AsyncSSLSocket>(s);

    std::cerr << "HandshakeErrorCallback::connAccepted" << std::endl;

    // The first call to sslAccept() should succeed.
    hcb_->setSocket(sock);
    sock->sslAccept(hcb_);
    EXPECT_EQ(sock->getSSLState(), AsyncSSLSocket::STATE_ACCEPTING);

    // The second call to sslAccept() should fail.
    HandshakeCallback callback2(hcb_->rcb_);
    callback2.setSocket(sock);
    sock->sslAccept(&callback2);
    EXPECT_EQ(sock->getSSLState(), AsyncSSLSocket::STATE_ERROR);

    // Both callbacks should be in the error state.
    EXPECT_EQ(hcb_->state, STATE_FAILED);
    EXPECT_EQ(callback2.state, STATE_FAILED);

    state = STATE_SUCCEEDED;
    hcb_->setState(STATE_SUCCEEDED);
    callback2.setState(STATE_SUCCEEDED);
  }
};

class HandshakeTimeoutCallback : public SSLServerAcceptCallbackBase {
 public:
  explicit HandshakeTimeoutCallback(HandshakeCallback* hcb)
      : SSLServerAcceptCallbackBase(hcb) {}

  void connAccepted(
      const std::shared_ptr<folly::AsyncSSLSocket>& s) noexcept override {
    std::cerr << "HandshakeErrorCallback::connAccepted" << std::endl;

    auto sock = std::static_pointer_cast<AsyncSSLSocket>(s);

    hcb_->setSocket(sock);
    sock->getEventBase()->tryRunAfterDelay(
        [=, this] {
          std::cerr << "Delayed SSL accept, client will have close by now"
                    << std::endl;
          // SSL accept will fail
          EXPECT_EQ(sock->getSSLState(), AsyncSSLSocket::STATE_UNINIT);
          hcb_->socket_->sslAccept(hcb_);
          // This registers for an event
          EXPECT_EQ(sock->getSSLState(), AsyncSSLSocket::STATE_ACCEPTING);

          state = STATE_SUCCEEDED;
        },
        100);
  }
};

class ConnectTimeoutCallback : public SSLServerAcceptCallbackBase {
 public:
  ConnectTimeoutCallback() : SSLServerAcceptCallbackBase(nullptr) {
    // We don't care if we get invoked or not.
    // The client may time out and give up before connAccepted() is even
    // called.
    state = STATE_SUCCEEDED;
  }

  void connAccepted(
      const std::shared_ptr<folly::AsyncSSLSocket>& s) noexcept override {
    std::cerr << "ConnectTimeoutCallback::connAccepted" << std::endl;

    // Just wait a while before closing the socket, so the client
    // will time out waiting for the handshake to complete.
    s->getEventBase()->tryRunAfterDelay([=] { s->close(); }, 100);
  }
};

class BlockingWriteClient
    : private AsyncSSLSocket::HandshakeCB,
      private AsyncTransport::WriteCallback {
 public:
  explicit BlockingWriteClient(AsyncSSLSocket::UniquePtr socket)
      : socket_(std::move(socket)), bufLen_(2500), iovCount_(2000) {
    // Fill buf_
    buf_ = std::make_unique<uint8_t[]>(bufLen_);
    for (uint32_t n = 0; n < sizeof(buf_); ++n) {
      buf_[n] = n % 0xff;
    }

    // Initialize iov_
    iov_ = std::make_unique<struct iovec[]>(iovCount_);
    for (uint32_t n = 0; n < iovCount_; ++n) {
      iov_[n].iov_base = buf_.get() + n;
      if (n & 0x1) {
        iov_[n].iov_len = n % bufLen_;
      } else {
        iov_[n].iov_len = bufLen_ - (n % bufLen_);
      }
    }

    socket_->sslConn(this, std::chrono::milliseconds(100));
  }

  struct iovec* getIovec() const { return iov_.get(); }
  uint32_t getIovecCount() const { return iovCount_; }

 private:
  void handshakeSuc(AsyncSSLSocket*) noexcept override {
    socket_->writev(this, iov_.get(), iovCount_);
  }
  void handshakeErr(
      AsyncSSLSocket*, const AsyncSocketException& ex) noexcept override {
    ADD_FAILURE() << "client handshake error: " << ex.what();
  }
  void writeSuccess() noexcept override { socket_->close(); }
  void writeErr(
      size_t bytesWritten, const AsyncSocketException& ex) noexcept override {
    ADD_FAILURE() << "client write error after " << bytesWritten
                  << " bytes: " << ex.what();
  }

  AsyncSSLSocket::UniquePtr socket_;
  uint32_t bufLen_;
  uint32_t iovCount_;
  std::unique_ptr<uint8_t[]> buf_;
  std::unique_ptr<struct iovec[]> iov_;
};

class BlockingWriteServer
    : private AsyncSSLSocket::HandshakeCB,
      private AsyncTransport::ReadCallback {
 public:
  explicit BlockingWriteServer(AsyncSSLSocket::UniquePtr socket)
      : socket_(std::move(socket)), bufSize_(2500 * 2000), bytesRead_(0) {
    buf_ = std::make_unique<uint8_t[]>(bufSize_);
    socket_->sslAccept(this, std::chrono::milliseconds(100));
  }

  void checkBuffer(struct iovec* iov, uint32_t count) const {
    uint32_t idx = 0;
    for (uint32_t n = 0; n < count; ++n) {
      size_t bytesLeft = bytesRead_ - idx;
      int rc = memcmp(
          buf_.get() + idx,
          iov[n].iov_base,
          std::min(iov[n].iov_len, bytesLeft));
      if (rc != 0) {
        FAIL() << "buffer mismatch at iovec " << n << "/" << count
               << ": rc=" << rc;
      }
      if (iov[n].iov_len > bytesLeft) {
        FAIL() << "server did not read enough data: " << "ended at byte "
               << bytesLeft << "/" << iov[n].iov_len << " in iovec " << n << "/"
               << count;
      }

      idx += iov[n].iov_len;
    }
    if (idx != bytesRead_) {
      ADD_FAILURE() << "server read extra data: " << bytesRead_
                    << " bytes read; expected " << idx;
    }
  }

 private:
  void handshakeSuc(AsyncSSLSocket*) noexcept override {
    // Wait 10ms before reading, so the client's writes will initially block.
    socket_->getEventBase()->tryRunAfterDelay(
        [this] { socket_->setReadCB(this); }, 10);
  }
  void handshakeErr(
      AsyncSSLSocket*, const AsyncSocketException& ex) noexcept override {
    ADD_FAILURE() << "server handshake error: " << ex.what();
  }
  void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
    *bufReturn = buf_.get() + bytesRead_;
    *lenReturn = bufSize_ - bytesRead_;
  }
  void readDataAvailable(size_t len) noexcept override {
    bytesRead_ += len;
    socket_->setReadCB(nullptr);
    socket_->getEventBase()->tryRunAfterDelay(
        [this] { socket_->setReadCB(this); }, 2);
  }
  void readEOF() noexcept override { socket_->close(); }
  void readErr(const AsyncSocketException& ex) noexcept override {
    ADD_FAILURE() << "server read error: " << ex.what();
  }

  AsyncSSLSocket::UniquePtr socket_;
  uint32_t bufSize_;
  uint32_t bytesRead_;
  std::unique_ptr<uint8_t[]> buf_;
};

class AlpnClient
    : private AsyncSSLSocket::HandshakeCB,
      private AsyncTransport::WriteCallback {
 public:
  explicit AlpnClient(AsyncSSLSocket::UniquePtr socket)
      : nextProto(nullptr), nextProtoLength(0), socket_(std::move(socket)) {
    socket_->sslConn(this);
  }

  const unsigned char* nextProto;
  unsigned nextProtoLength;
  folly::Optional<AsyncSocketException> except;

 private:
  void handshakeSuc(AsyncSSLSocket*) noexcept override {
    socket_->getSelectedNextProtocol(&nextProto, &nextProtoLength);
  }
  void handshakeErr(
      AsyncSSLSocket*, const AsyncSocketException& ex) noexcept override {
    except = ex;
  }
  void writeSuccess() noexcept override { socket_->close(); }
  void writeErr(
      size_t bytesWritten, const AsyncSocketException& ex) noexcept override {
    ADD_FAILURE() << "client write error after " << bytesWritten
                  << " bytes: " << ex.what();
  }

  AsyncSSLSocket::UniquePtr socket_;
};

class AlpnServer
    : private AsyncSSLSocket::HandshakeCB,
      private AsyncTransport::ReadCallback {
 public:
  explicit AlpnServer(AsyncSSLSocket::UniquePtr socket)
      : nextProto(nullptr), nextProtoLength(0), socket_(std::move(socket)) {
    socket_->sslAccept(this);
    socket_->enableClientHelloParsing();
  }

  const unsigned char* nextProto;
  unsigned nextProtoLength;
  folly::Optional<AsyncSocketException> except;
  const std::vector<std::string>& getClientAlpns() const {
    return socket_->getClientAlpns();
  }

 private:
  void handshakeSuc(AsyncSSLSocket*) noexcept override {
    socket_->getSelectedNextProtocol(&nextProto, &nextProtoLength);
  }
  void handshakeErr(
      AsyncSSLSocket*, const AsyncSocketException& ex) noexcept override {
    except = ex;
  }
  void getReadBuffer(void** /* bufReturn */, size_t* lenReturn) override {
    *lenReturn = 0;
  }
  void readDataAvailable(size_t /* len */) noexcept override {}
  void readEOF() noexcept override { socket_->close(); }
  void readErr(const AsyncSocketException& ex) noexcept override {
    ADD_FAILURE() << "server read error: " << ex.what();
  }

  AsyncSSLSocket::UniquePtr socket_;
};

class RenegotiatingServer
    : public AsyncSSLSocket::HandshakeCB,
      public AsyncTransport::ReadCallback {
 public:
  explicit RenegotiatingServer(AsyncSSLSocket::UniquePtr socket)
      : socket_(std::move(socket)) {
    socket_->sslAccept(this);
  }

  ~RenegotiatingServer() override { socket_->setReadCB(nullptr); }

  void handshakeSuc(AsyncSSLSocket* /* socket */) noexcept override {
    LOG(INFO) << "Renegotiating server handshake success";
    socket_->setReadCB(this);
  }
  void handshakeErr(
      AsyncSSLSocket*, const AsyncSocketException& ex) noexcept override {
    ADD_FAILURE() << "Renegotiating server handshake error: " << ex.what();
  }
  void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
    *lenReturn = sizeof(buf);
    *bufReturn = buf;
  }
  void readDataAvailable(size_t /* len */) noexcept override {}
  void readEOF() noexcept override {}
  void readErr(const AsyncSocketException& ex) noexcept override {
    LOG(INFO) << "server got read error " << ex.what();
    auto exPtr = dynamic_cast<const SSLException*>(&ex);
    ASSERT_NE(nullptr, exPtr);
    std::string exStr(ex.what());
    SSLException sslEx(SSLError::CLIENT_RENEGOTIATION);
    ASSERT_NE(std::string::npos, exStr.find(sslEx.what()));
    renegotiationError_ = true;
  }

  AsyncSSLSocket::UniquePtr socket_;
  unsigned char buf[128];
  bool renegotiationError_{false};
};

class SNIClient
    : private AsyncSSLSocket::HandshakeCB,
      private AsyncTransport::WriteCallback {
 public:
  explicit SNIClient(AsyncSSLSocket::UniquePtr socket)
      : serverNameMatch(false), socket_(std::move(socket)) {
    socket_->sslConn(this);
  }

  std::string getApplicationProtocol() {
    return socket_->getApplicationProtocol();
  }

  bool serverNameMatch;

 private:
  void handshakeSuc(AsyncSSLSocket*) noexcept override {
    serverNameMatch = socket_->isServerNameMatch();
  }
  void handshakeErr(
      AsyncSSLSocket*, const AsyncSocketException& ex) noexcept override {
    ADD_FAILURE() << "client handshake error: " << ex.what();
  }
  void writeSuccess() noexcept override { socket_->close(); }
  void writeErr(
      size_t bytesWritten, const AsyncSocketException& ex) noexcept override {
    ADD_FAILURE() << "client write error after " << bytesWritten
                  << " bytes: " << ex.what();
  }

  AsyncSSLSocket::UniquePtr socket_;
};

class SNIServer
    : private AsyncSSLSocket::HandshakeCB,
      private AsyncTransport::ReadCallback {
 public:
  explicit SNIServer(
      AsyncSSLSocket::UniquePtr socket,
      const std::shared_ptr<folly::SSLContext>& ctx,
      const std::shared_ptr<folly::SSLContext>& sniCtx,
      const std::string& expectedServerName)
      : serverNameMatch(false),
        socket_(std::move(socket)),
        sniCtx_(sniCtx),
        expectedServerName_(expectedServerName) {
    ctx->setServerNameCallback(
        std::bind(&SNIServer::serverNameCallback, this, std::placeholders::_1));
    socket_->sslAccept(this);
  }

  std::string getApplicationProtocol() {
    return socket_->getApplicationProtocol();
  }

  bool serverNameMatch;

 private:
  void handshakeSuc(AsyncSSLSocket* /* ssl */) noexcept override {}
  void handshakeErr(
      AsyncSSLSocket*, const AsyncSocketException& ex) noexcept override {
    ADD_FAILURE() << "server handshake error: " << ex.what();
  }
  void getReadBuffer(void** /* bufReturn */, size_t* lenReturn) override {
    *lenReturn = 0;
  }
  void readDataAvailable(size_t /* len */) noexcept override {}
  void readEOF() noexcept override { socket_->close(); }
  void readErr(const AsyncSocketException& ex) noexcept override {
    ADD_FAILURE() << "server read error: " << ex.what();
  }

  folly::SSLContext::ServerNameCallbackResult serverNameCallback(SSL* ssl) {
    const char* sn = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (sniCtx_ && sn && !strcasecmp(expectedServerName_.c_str(), sn)) {
      AsyncSSLSocket* sslSocket = AsyncSSLSocket::getFromSSL(ssl);
      sslSocket->switchServerSSLContext(sniCtx_);
      serverNameMatch = true;
      return folly::SSLContext::SERVER_NAME_FOUND;
    } else {
      serverNameMatch = false;
      return folly::SSLContext::SERVER_NAME_NOT_FOUND;
    }
  }

  AsyncSSLSocket::UniquePtr socket_;
  std::shared_ptr<folly::SSLContext> sniCtx_;
  std::string expectedServerName_;
};

// Simulates an application-installed ServerNameCallback that has gone
// haywire, returning a folly::SSLContext::ServerNameCallbackResult outside
// the three documented enumerators.
class SNIServerBadCallbackResult
    : private AsyncSSLSocket::HandshakeCB,
      private AsyncTransport::ReadCallback {
 public:
  explicit SNIServerBadCallbackResult(
      AsyncSSLSocket::UniquePtr socket,
      const std::shared_ptr<folly::SSLContext>& ctx)
      : socket_(std::move(socket)) {
    ctx->setServerNameCallback([](SSL*) {
      // 3 is deliberately chosen: ServerNameCallbackResult has 3 enumerators
      // (0, 1, 2), so its legal range per [dcl.enum] (smallest bit-width
      // fitting the enumerators) is 0..3 -- 3 is not a declared enumerator,
      // but is a well-defined, in-range value to cast, unlike an arbitrary
      // out-of-range value (which UBSan's own invalid-enum-load check would
      // catch on its own, independent of this code's own handling).
      return static_cast<folly::SSLContext::ServerNameCallbackResult>(3);
    });
    socket_->sslAccept(this);
  }

 private:
  void handshakeSuc(AsyncSSLSocket* /* ssl */) noexcept override {}
  void handshakeErr(
      AsyncSSLSocket*, const AsyncSocketException& ex) noexcept override {
    ADD_FAILURE() << "server handshake error: " << ex.what();
  }
  void getReadBuffer(void** /* bufReturn */, size_t* lenReturn) override {
    *lenReturn = 0;
  }
  void readDataAvailable(size_t /* len */) noexcept override {}
  void readEOF() noexcept override { socket_->close(); }
  void readErr(const AsyncSocketException& ex) noexcept override {
    ADD_FAILURE() << "server read error: " << ex.what();
  }

  AsyncSSLSocket::UniquePtr socket_;
};

class SSLClient
    : public AsyncSocket::ConnectCallback,
      public AsyncTransport::WriteCallback,
      public AsyncTransport::ReadCallback {
 private:
  EventBase* eventBase_;
  std::shared_ptr<AsyncSSLSocket> sslSocket_;
  std::shared_ptr<folly::ssl::SSLSession> session_;
  std::shared_ptr<folly::SSLContext> ctx_;
  uint32_t requests_;
  folly::SocketAddress address_;
  uint32_t timeout_;
  char buf_[128];
  char readbuf_[128];
  uint32_t bytesRead_;
  uint32_t hit_;
  uint32_t miss_;
  uint32_t errors_;
  uint32_t writeAfterConnectErrors_;

  // These settings test that we eventually drain the
  // socket, even if the maxReadsPerEvent_ is hit during
  // a event loop iteration.
  static constexpr size_t kMaxReadsPerEvent = 2;
  // 2 event loop iterations
  static constexpr size_t kMaxReadBufferSz =
      sizeof(decltype(readbuf_)) / kMaxReadsPerEvent / 2;

 public:
  SSLClient(
      EventBase* eventBase,
      const folly::SocketAddress& address,
      uint32_t requests,
      uint32_t timeout = 0)
      : eventBase_(eventBase),
        session_(nullptr),
        requests_(requests),
        address_(address),
        timeout_(timeout),
        bytesRead_(0),
        hit_(0),
        miss_(0),
        errors_(0),
        writeAfterConnectErrors_(0) {
    ctx_.reset(new folly::SSLContext());
    ctx_->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
    memset(buf_, 'a', sizeof(buf_));
  }

  ~SSLClient() override {
    if (errors_ == 0) {
      EXPECT_EQ(bytesRead_, sizeof(buf_));
    }
  }

  uint32_t getHit() const { return hit_; }

  uint32_t getMiss() const { return miss_; }

  uint32_t getErrors() const { return errors_; }

  uint32_t getWriteAfterConnectErrors() const {
    return writeAfterConnectErrors_;
  }

  void setSSLOptions(long options) { ctx_->setOptions(options); }

  void connect(bool writeNow = false) {
    sslSocket_ = AsyncSSLSocket::newSocket(ctx_, eventBase_);
    if (session_ != nullptr) {
      sslSocket_->setSSLSession(session_);
    }
    requests_--;
    sslSocket_->connect(this, address_, timeout_);
    if (sslSocket_ && writeNow) {
      // write some junk, used in an error test
      sslSocket_->write(this, buf_, sizeof(buf_));
    }
  }

  void connectSuccess() noexcept override {
    std::cerr << "client SSL socket connected" << std::endl;
    if (sslSocket_->getSSLSessionReused()) {
      hit_++;
    } else {
      miss_++;
      session_ = sslSocket_->getSSLSession();
    }

    // write()
    sslSocket_->setMaxReadsPerEvent(kMaxReadsPerEvent);
    sslSocket_->write(this, buf_, sizeof(buf_));
    sslSocket_->setReadCB(this);
    memset(readbuf_, 'b', sizeof(readbuf_));
    bytesRead_ = 0;
  }

  void connectErr(const AsyncSocketException& ex) noexcept override {
    std::cerr << "SSLClient::connectError: " << ex.what() << std::endl;
    errors_++;
    sslSocket_.reset();
  }

  void writeSuccess() noexcept override {
    std::cerr << "client write success" << std::endl;
  }

  void writeErr(
      size_t /* bytesWritten */,
      const AsyncSocketException& ex) noexcept override {
    std::cerr << "client writeError: " << ex.what() << std::endl;
    if (!sslSocket_) {
      writeAfterConnectErrors_++;
    }
  }

  void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
    *bufReturn = readbuf_ + bytesRead_;
    *lenReturn = std::min(kMaxReadBufferSz, sizeof(readbuf_) - bytesRead_);
  }

  void readEOF() noexcept override {
    std::cerr << "client readEOF" << std::endl;
  }

  void readErr(const AsyncSocketException& ex) noexcept override {
    std::cerr << "client readError: " << ex.what() << std::endl;
  }

  void readDataAvailable(size_t len) noexcept override {
    std::cerr << "client read data: " << len << std::endl;
    bytesRead_ += len;
    if (bytesRead_ == sizeof(buf_)) {
      EXPECT_EQ(memcmp(buf_, readbuf_, bytesRead_), 0);
      sslSocket_->closeNow();
      sslSocket_.reset();
      if (requests_ != 0) {
        connect();
      }
    }
  }
};

class SSLHandshakeBase
    : public AsyncSSLSocket::HandshakeCB,
      private AsyncTransport::WriteCallback {
 public:
  explicit SSLHandshakeBase(
      AsyncSSLSocket::UniquePtr socket, bool preverifyResult, bool verifyResult)
      : handshakeVerify_(false),
        handshakeSuccess_(false),
        handshakeError_(false),
        socket_(std::move(socket)),
        preverifyResult_(preverifyResult),
        verifyResult_(verifyResult) {}

  AsyncSSLSocket::UniquePtr moveSocket() && { return std::move(socket_); }

  bool handshakeVerify_;
  bool handshakeSuccess_;
  bool handshakeError_;
  int handshakeVerifyInvocations_{};
  std::chrono::nanoseconds handshakeTime;

 protected:
  AsyncSSLSocket::UniquePtr socket_;
  bool preverifyResult_;
  bool verifyResult_;

  // HandshakeCallback
  bool handshakeVer(
      AsyncSSLSocket* /* sock */,
      bool preverifyOk,
      X509_STORE_CTX* /* ctx */) noexcept override {
    auto invocation = handshakeVerifyInvocations_++;

    if (invocation == 0) {
      handshakeVerify_ = true;
      EXPECT_EQ(preverifyResult_, preverifyOk);
    }
    return verifyResult_;
  }

  void handshakeSuc(AsyncSSLSocket*) noexcept override {
    LOG(INFO) << "Handshake success";
    handshakeSuccess_ = true;
    if (socket_) {
      handshakeTime = socket_->getHandshakeTime();
    }
  }

  void handshakeErr(
      AsyncSSLSocket*, const AsyncSocketException& ex) noexcept override {
    LOG(INFO) << "Handshake error " << ex.what();
    handshakeError_ = true;
    if (socket_) {
      handshakeTime = socket_->getHandshakeTime();
    }
  }

  // WriteCallback
  void writeSuccess() noexcept override {
    if (socket_) {
      socket_->close();
    }
  }

  void writeErr(
      size_t bytesWritten, const AsyncSocketException& ex) noexcept override {
    ADD_FAILURE() << "client write error after " << bytesWritten
                  << " bytes: " << ex.what();
  }
};

class SSLHandshakeClient : public SSLHandshakeBase {
 public:
  SSLHandshakeClient(
      AsyncSSLSocket::UniquePtr socket, bool preverifyResult, bool verifyResult)
      : SSLHandshakeBase(std::move(socket), preverifyResult, verifyResult) {
    socket_->sslConn(this, std::chrono::milliseconds::zero());
  }
};

class SSLHandshakeClientNoVerify : public SSLHandshakeBase {
 public:
  SSLHandshakeClientNoVerify(
      AsyncSSLSocket::UniquePtr socket, bool preverifyResult, bool verifyResult)
      : SSLHandshakeBase(std::move(socket), preverifyResult, verifyResult) {
    socket_->sslConn(
        this,
        std::chrono::milliseconds::zero(),
        folly::SSLContext::SSLVerifyPeerEnum::NO_VERIFY);
  }
};

class SSLHandshakeClientDoVerify : public SSLHandshakeBase {
 public:
  SSLHandshakeClientDoVerify(
      AsyncSSLSocket::UniquePtr socket, bool preverifyResult, bool verifyResult)
      : SSLHandshakeBase(std::move(socket), preverifyResult, verifyResult) {
    socket_->sslConn(
        this,
        std::chrono::milliseconds::zero(),
        folly::SSLContext::SSLVerifyPeerEnum::VERIFY);
  }
};

class SSLHandshakeServer : public SSLHandshakeBase {
 public:
  SSLHandshakeServer(
      AsyncSSLSocket::UniquePtr socket, bool preverifyResult, bool verifyResult)
      : SSLHandshakeBase(std::move(socket), preverifyResult, verifyResult) {
    socket_->sslAccept(this, std::chrono::milliseconds::zero());
  }
};

class SSLHandshakeServerParseClientHello : public SSLHandshakeBase {
 public:
  SSLHandshakeServerParseClientHello(
      AsyncSSLSocket::UniquePtr socket, bool preverifyResult, bool verifyResult)
      : SSLHandshakeBase(std::move(socket), preverifyResult, verifyResult) {
    socket_->enableClientHelloParsing();
    socket_->sslAccept(this, std::chrono::milliseconds::zero());
  }

  std::string clientCiphers_, sharedCiphers_, serverCiphers_, chosenCipher_;
  uint16_t chosenCipherId_{0};

 protected:
  void handshakeSuc(AsyncSSLSocket* sock) noexcept override {
    handshakeSuccess_ = true;
    sock->getSSLSharedCiphers(sharedCiphers_);
    sock->getSSLServerCiphers(serverCiphers_);
    sock->getSSLClientCiphers(clientCiphers_);
    chosenCipher_ = sock->getNegotiatedCipherName();
    chosenCipherId_ = sock->getNegotiatedCipherId();
  }
};

class SSLHandshakeServerNoVerify : public SSLHandshakeBase {
 public:
  SSLHandshakeServerNoVerify(
      AsyncSSLSocket::UniquePtr socket, bool preverifyResult, bool verifyResult)
      : SSLHandshakeBase(std::move(socket), preverifyResult, verifyResult) {
    socket_->sslAccept(
        this,
        std::chrono::milliseconds::zero(),
        folly::SSLContext::SSLVerifyPeerEnum::NO_VERIFY);
  }
};

class SSLHandshakeServerDoVerify : public SSLHandshakeBase {
 public:
  SSLHandshakeServerDoVerify(
      AsyncSSLSocket::UniquePtr socket, bool preverifyResult, bool verifyResult)
      : SSLHandshakeBase(std::move(socket), preverifyResult, verifyResult) {
    socket_->sslAccept(
        this,
        std::chrono::milliseconds::zero(),
        folly::SSLContext::SSLVerifyPeerEnum::VERIFY_REQ_CLIENT_CERT);
  }
};

class EventBaseAborter : public AsyncTimeout {
 public:
  EventBaseAborter(EventBase* eventBase, uint32_t timeoutMS)
      : AsyncTimeout(eventBase, AsyncTimeout::InternalEnum::INTERNAL),
        eventBase_(eventBase) {
    scheduleTimeout(timeoutMS);
  }

  void timeoutExpired() noexcept override {
    FAIL() << "test timed out";
    eventBase_->terminateLoopSoon();
  }

 private:
  EventBase* eventBase_;
};

class SSLAcceptEvbRunner : public SSLAcceptRunner {
 public:
  explicit SSLAcceptEvbRunner(EventBase* evb) : evb_(evb) {}
  ~SSLAcceptEvbRunner() override = default;

  void run(Function<int()> acceptFunc, Function<void(int)> finallyFunc)
      const override {
    evb_->runInLoop([acceptFunc = std::move(acceptFunc),
                     finallyFunc = std::move(finallyFunc)]() mutable {
      finallyFunc(acceptFunc());
    });
  }

 protected:
  EventBase* evb_;
};

class SSLAcceptErrorRunner : public SSLAcceptEvbRunner {
 public:
  explicit SSLAcceptErrorRunner(EventBase* evb) : SSLAcceptEvbRunner(evb) {}
  ~SSLAcceptErrorRunner() override = default;

  void run(Function<int()> /*acceptFunc*/, Function<void(int)> finallyFunc)
      const override {
    evb_->runInLoop([finallyFunc = std::move(finallyFunc)]() mutable {
      finallyFunc(-1);
    });
  }
};

class SSLAcceptCloseRunner : public SSLAcceptEvbRunner {
 public:
  explicit SSLAcceptCloseRunner(EventBase* evb, folly::AsyncSSLSocket* sock)
      : SSLAcceptEvbRunner(evb), socket_(sock) {}
  ~SSLAcceptCloseRunner() override = default;

  void run(Function<int()> acceptFunc, Function<void(int)> finallyFunc)
      const override {
    evb_->runInLoop(
        [acceptFunc = std::move(acceptFunc),
         finallyFunc = std::move(finallyFunc),
         sock = socket_]() mutable {
          auto ret = acceptFunc();
          sock->closeNow();
          finallyFunc(ret);
        });
  }

 private:
  folly::AsyncSSLSocket* socket_;
};

class SSLAcceptDestroyRunner : public SSLAcceptEvbRunner {
 public:
  explicit SSLAcceptDestroyRunner(EventBase* evb, SSLHandshakeBase* base)
      : SSLAcceptEvbRunner(evb), sslBase_(base) {}
  ~SSLAcceptDestroyRunner() override = default;

  void run(Function<int()> acceptFunc, Function<void(int)> finallyFunc)
      const override {
    evb_->runInLoop(
        [acceptFunc = std::move(acceptFunc),
         finallyFunc = std::move(finallyFunc),
         sslBase = sslBase_]() mutable {
          auto ret = acceptFunc();
          std::move(*sslBase).moveSocket();
          finallyFunc(ret);
        });
  }

 private:
  SSLHandshakeBase* sslBase_;
};

class SSLAcceptFiberRunner : public SSLAcceptEvbRunner {
 public:
  explicit SSLAcceptFiberRunner(EventBase* evb) : SSLAcceptEvbRunner(evb) {}
  ~SSLAcceptFiberRunner() override = default;

  void run(Function<int()> acceptFunc, Function<void(int)> finallyFunc)
      const override {
    auto& fiberManager = folly::fibers::getFiberManager(*evb_);
    fiberManager.addTaskFinally(
        std::move(acceptFunc),
        [finally = std::move(finallyFunc)](folly::Try<int>&& res) mutable {
          finally(res.value());
        });
  }
};

} // namespace folly::test
