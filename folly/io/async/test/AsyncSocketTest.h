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

#include <memory>

#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/EventBaseBackendBase.h>
#include <folly/io/async/test/CallbackStateEnum.h>
#include <folly/io/async/test/ConnCallback.h>
#include <folly/io/async/testing/BlockingSocket.h>
#include <folly/net/NetOps.h>
#include <folly/net/NetworkSocket.h>
#include <folly/portability/Sockets.h>

namespace folly::test {

class WriteCallback
    : public folly::AsyncTransport::WriteCallback,
      public folly::AsyncWriter::ReleaseIOBufCallback {
 public:
  explicit WriteCallback(bool enableReleaseIOBufCallback = false)
      : state(STATE_WAITING),
        bytesWritten(0),
        numIoBufCount(0),
        numIoBufBytes(0),
        exception(folly::AsyncSocketException::UNKNOWN, "none"),
        releaseIOBufCallback(enableReleaseIOBufCallback ? this : nullptr) {}

  void writeSuccess() noexcept override {
    state = STATE_SUCCEEDED;
    if (successCallback) {
      successCallback();
    }
  }

  void writeErr(
      size_t nBytesWritten,
      const folly::AsyncSocketException& ex) noexcept override {
    LOG(ERROR) << ex.what();
    state = STATE_FAILED;
    this->bytesWritten = nBytesWritten;
    exception = ex;
    if (errorCallback) {
      errorCallback();
    }
  }

  void writeStarting() noexcept override { writeStartingInvocations++; }

  folly::AsyncWriter::ReleaseIOBufCallback*
  getReleaseIOBufCallback() noexcept override {
    return releaseIOBufCallback;
  }

  void releaseIOBuf(std::unique_ptr<folly::IOBuf> ioBuf) noexcept override {
    numIoBufCount += ioBuf->countChainElements();
    numIoBufBytes += ioBuf->computeChainDataLength();
    releaseIOBufCallbackCalled = true;
  }

  StateEnum state;
  std::atomic<size_t> bytesWritten;
  std::atomic<size_t> numIoBufCount;
  std::atomic<size_t> numIoBufBytes;
  folly::AsyncSocketException exception;
  VoidCallback successCallback;
  VoidCallback errorCallback;
  ReleaseIOBufCallback* releaseIOBufCallback;
  size_t writeStartingInvocations{0};
  bool releaseIOBufCallbackCalled{false};
};

class ReadCallback : public folly::AsyncTransport::ReadCallback {
 public:
  explicit ReadCallback(size_t _maxBufferSz = 4096)
      : state(STATE_WAITING),
        exception(folly::AsyncSocketException::UNKNOWN, "none"),
        buffers(),
        maxBufferSz(_maxBufferSz) {}

  ~ReadCallback() override {
    for (auto& buffer : buffers) {
      buffer.free();
    }
    currentBuffer.free();
  }

  void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
    if (!currentBuffer.buffer) {
      currentBuffer.allocate(maxBufferSz);
    }
    *bufReturn = currentBuffer.buffer;
    *lenReturn = currentBuffer.length;
  }

  void readDataAvailable(size_t len) noexcept override {
    currentBuffer.length = len;
    buffers.push_back(std::move(currentBuffer));
    currentBuffer.reset();
    if (dataAvailableCallback) {
      dataAvailableCallback();
    }
  }

  size_t maxBufferSize() const override { return maxBufferSz; }
  bool isBufferMovable() noexcept override { return true; }
  void readBufferAvailable(std::unique_ptr<IOBuf> readBuf) noexcept override {
    if (currentBuffer.buffer && currentBuffer.length > 0) {
      buffers.push_back(std::move(currentBuffer));
    }
    currentBuffer.reset();

    auto coalesced = readBuf->cloneCoalesced();
    currentBuffer.buffer = (char*)coalesced->writableData();
    currentBuffer.length = coalesced->length();
    currentBuffer.ioBuf = std::move(coalesced);
    buffers.push_back(std::move(currentBuffer));
    currentBuffer.reset();
    if (dataAvailableCallback) {
      dataAvailableCallback();
    }
  }

  void readEOF() noexcept override { state = STATE_SUCCEEDED; }

  void readErr(const folly::AsyncSocketException& ex) noexcept override {
    state = STATE_FAILED;
    exception = ex;
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

  void coalesce() {
    if (buffers.empty()) {
      return;
    }

    std::unique_ptr<IOBuf> coalesced;

    for (auto& buf : buffers) {
      if (!buf.ioBuf) {
        return;
      }

      if (!coalesced) {
        coalesced = std::move(buf.ioBuf);
      } else {
        coalesced->appendToChain(std::move(buf.ioBuf));
      }
    }
    buffers.clear();
    coalesced->coalesce();
    buffers.emplace_back(std::move(coalesced));
  }

  class Buffer {
   public:
    Buffer() : buffer(nullptr), length(0) {}
    Buffer(char* buf, size_t len) : buffer(buf), length(len) {}
    explicit Buffer(std::unique_ptr<IOBuf> buf)
        : buffer((char*)buf->writableData()),
          length(buf->length()),
          ioBuf(std::move(buf)) {}

    void reset() {
      buffer = nullptr;
      length = 0;
      ioBuf.reset();
    }
    void allocate(size_t len) {
      assert(buffer == nullptr);
      this->buffer = static_cast<char*>(malloc(len));
      this->length = len;
    }
    void free() {
      if (!ioBuf) {
        ::free(buffer);
      }
      reset();
    }

    char* buffer;
    size_t length;
    std::unique_ptr<IOBuf> ioBuf;
  };

  StateEnum state;
  folly::AsyncSocketException exception;
  std::vector<Buffer> buffers;
  Buffer currentBuffer;
  VoidCallback dataAvailableCallback;
  const size_t maxBufferSz;
};

class TestEventBaseBackend : public folly::EventBaseBackendBase {
 public:
  explicit TestEventBaseBackend() : evb_(EventBase::getDefaultBackend()) {}

  event_base* getEventBase() override { return evb_->getEventBase(); }
  int eb_event_base_loop(int flags) override {
    return evb_->eb_event_base_loop(flags);
  }
  int eb_event_base_loopbreak() override {
    return evb_->eb_event_base_loopbreak();
  }

  int eb_event_add(Event& event, const struct timeval* timeout) override {
    return evb_->eb_event_add(event, timeout);
  }
  int eb_event_del(Event& event) override { return evb_->eb_event_del(event); }

  bool eb_event_active(Event& event, int res) override {
    return evb_->eb_event_active(event, res);
  }

  void queueRecvZc(
      int fd,
      void* buf,
      unsigned long nbytes,
      EventBaseBackendBase::RecvZcCallback&& callback) override {
    queued = true;
    bytes = netops::recv(NetworkSocket::fromFd(fd), buf, nbytes, MSG_DONTWAIT);
    recvZcCb = std::move(callback);
  }

  bool queued{false};
  ssize_t bytes{0};
  EventBaseBackendBase::RecvZcCallback recvZcCb;

 private:
  std::unique_ptr<folly::EventBaseBackendBase> evb_;
};

class ReadvCallback : public folly::AsyncTransport::ReadCallback {
 public:
  ReadvCallback(size_t bufferSize, size_t len)
      : state_(STATE_WAITING),
        exception_(folly::AsyncSocketException::UNKNOWN, "none"),
        queue_(folly::IOBufIovecBuilder::Options().setBlockSize(bufferSize)),
        len_(len) {
    setReadMode(folly::AsyncTransport::ReadCallback::ReadMode::ReadVec);
  }

  ~ReadvCallback() override = default;

  void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
    std::ignore = bufReturn;
    std::ignore = lenReturn;

    CHECK(false); // this should not be called
  }

  void getReadBuffers(folly::IOBufIovecBuilder::IoVecVec& iovs) override {
    queue_.allocateBuffers(iovs, len_);
  }

  void readDataAvailable(size_t len) noexcept override {
    auto tmp = queue_.extractIOBufChain(len);
    if (!buf_) {
      buf_ = std::move(tmp);
    } else {
      buf_->prependChain(std::move(tmp));
    }
  }

  bool isBufferMovable() noexcept override { return true; }
  void readBufferAvailable(std::unique_ptr<IOBuf> readBuf) noexcept override {
    if (!buf_) {
      buf_ = std::move(readBuf);
    } else {
      buf_->appendToChain(std::move(readBuf));
    }
  }

  void reset() { buf_.reset(); }

  void readEOF() noexcept override { state_ = STATE_SUCCEEDED; }

  void readErr(const folly::AsyncSocketException& ex) noexcept override {
    state_ = STATE_FAILED;
    exception_ = ex;
  }

  void verifyData(const std::string& data) const {
    CHECK(buf_);
    auto r = buf_->coalesce();
    std::string tmp;
    tmp.assign(reinterpret_cast<const char*>(r.begin()), r.end() - r.begin());
    CHECK_EQ(data, tmp);
  }

  std::unique_ptr<folly::IOBuf> buf_;

 private:
  StateEnum state_;
  folly::AsyncSocketException exception_;
  folly::IOBufIovecBuilder queue_;
  const size_t len_;
};

class BufferCallback : public folly::AsyncTransport::BufferCallback {
 public:
  BufferCallback(folly::AsyncSocket* socket, size_t expectedBytes)
      : socket_(socket),
        expectedBytes_(expectedBytes),
        buffered_(false),
        bufferCleared_(false) {}

  void onEgressBuffered() override {
    size_t bytesWritten = socket_->getAppBytesWritten();
    size_t bytesBuffered = socket_->getAppBytesBuffered();
    CHECK_GT(bytesBuffered, 0);
    CHECK_EQ(expectedBytes_, bytesWritten + bytesBuffered);
    buffered_ = true;
  }

  void onEgressBufferCleared() override {
    size_t bytesWritten = socket_->getAppBytesWritten();
    size_t bytesBuffered = socket_->getAppBytesBuffered();
    CHECK_EQ(0, bytesBuffered);
    CHECK_EQ(expectedBytes_, bytesWritten);
    bufferCleared_ = true;
  }

  bool hasBuffered() const { return buffered_; }

  bool hasBufferCleared() const { return bufferCleared_; }

 private:
  folly::AsyncSocket* socket_{nullptr};
  size_t expectedBytes_{0};
  bool buffered_{false};
  bool bufferCleared_{false};
};

class ZeroCopyReadCallback : public folly::AsyncTransport::ReadCallback {
 public:
  explicit ZeroCopyReadCallback(
      folly::AsyncTransport::ReadCallback::ZeroCopyMemStore* memStore,
      size_t _maxBufferSz = 4096)
      : memStore_(memStore),
        state(STATE_WAITING),
        exception(folly::AsyncSocketException::UNKNOWN, "none"),
        maxBufferSz(_maxBufferSz) {}

  ~ZeroCopyReadCallback() override { currentBuffer.free(); }

  // zerocopy
  folly::AsyncTransport::ReadCallback::ZeroCopyMemStore*
  readZeroCopyEnabled() noexcept override {
    return memStore_;
  }

  void getZeroCopyFallbackBuffer(
      void** bufReturn, size_t* lenReturn) noexcept override {
    if (!currentZeroCopyBuffer.buffer) {
      currentZeroCopyBuffer.allocate(maxBufferSz);
    }
    *bufReturn = currentZeroCopyBuffer.buffer;
    *lenReturn = currentZeroCopyBuffer.length;
  }

  void readZeroCopyDataAvailable(
      std::unique_ptr<folly::IOBuf>&& zeroCopyData,
      size_t additionalBytes) noexcept override {
    auto ioBuf = std::move(zeroCopyData);
    if (additionalBytes) {
      auto tmp = folly::IOBuf::takeOwnership(
          currentZeroCopyBuffer.buffer,
          currentZeroCopyBuffer.length,
          0,
          additionalBytes);
      currentZeroCopyBuffer.reset();
      if (ioBuf) {
        ioBuf->prependChain(std::move(tmp));
      } else {
        ioBuf = std::move(tmp);
      }
    }

    if (!data_) {
      data_ = std::move(ioBuf);
    } else {
      data_->prependChain(std::move(ioBuf));
    }
  }

  void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
    if (!currentBuffer.buffer) {
      currentBuffer.allocate(maxBufferSz);
    }
    *bufReturn = currentBuffer.buffer;
    *lenReturn = currentBuffer.length;
  }

  void readDataAvailable(size_t len) noexcept override {
    auto ioBuf = folly::IOBuf::takeOwnership(
        currentBuffer.buffer, currentBuffer.length, 0, len);
    currentBuffer.reset();

    if (!data_) {
      data_ = std::move(ioBuf);
    } else {
      data_->prependChain(std::move(ioBuf));
    }
  }

  bool isBufferMovable() noexcept override { return true; }
  void readBufferAvailable(std::unique_ptr<IOBuf> readBuf) noexcept override {
    if (!data_) {
      data_ = std::move(readBuf);
    } else {
      data_->prependChain(std::move(readBuf));
    }
  }

  void readEOF() noexcept override { state = STATE_SUCCEEDED; }

  void readErr(const folly::AsyncSocketException& ex) noexcept override {
    state = STATE_FAILED;
    exception = ex;
  }

  void verifyData(const std::string& expected) const {
    verifyData((const unsigned char*)expected.data(), expected.size());
  }

  void verifyData(const unsigned char* expected, size_t expectedLen) const {
    CHECK(!!data_);
    auto len = data_->computeChainDataLength();
    CHECK_EQ(len, expectedLen);

    auto* buf = data_.get();
    auto* current = buf;
    size_t offset = 0;

    do {
      size_t cmpLen = std::min(current->length(), expectedLen - offset);
      CHECK_EQ(cmpLen, current->length());
      CHECK_EQ(memcmp(current->data(), expected + offset, cmpLen), 0);
      offset += cmpLen;

      current = current->next();
    } while (current != buf);

    std::ignore = expected;
    CHECK_EQ(offset, expectedLen);
  }

  class Buffer {
   public:
    Buffer() = default;
    Buffer(char* buf, size_t len) : buffer(buf), length(len) {}
    ~Buffer() {
      if (buffer) {
        ::free(buffer);
      }
    }

    void reset() {
      buffer = nullptr;
      length = 0;
    }
    void allocate(size_t len) {
      CHECK(buffer == nullptr);
      buffer = static_cast<char*>(malloc(len));
      length = len;
    }
    void free() {
      ::free(buffer);
      reset();
    }

    char* buffer{nullptr};
    size_t length{0};
  };
  folly::AsyncTransport::ReadCallback::ZeroCopyMemStore* memStore_;
  StateEnum state;
  folly::AsyncSocketException exception;
  Buffer currentBuffer, currentZeroCopyBuffer;
  VoidCallback dataAvailableCallback;
  const size_t maxBufferSz;
  std::unique_ptr<folly::IOBuf> data_;
};

class ReadVerifier {};

class TestSendMsgParamsCallback
    : public folly::AsyncSocket::SendMsgParamsCallback {
 public:
  TestSendMsgParamsCallback(int flags, uint32_t dataSize, void* data)
      : flags_(flags),
        writeFlags_(folly::WriteFlags::NONE),
        dataSize_(dataSize),
        data_(data),
        queriedFlags_(false),
        queriedData_(false) {}

  void reset(int flags) {
    flags_ = flags;
    writeFlags_ = folly::WriteFlags::NONE;
    queriedFlags_ = false;
    queriedData_ = false;
  }

  int getFlagsImpl(
      folly::WriteFlags flags, int /*defaultFlags*/) noexcept override {
    queriedFlags_ = true;
    if (writeFlags_ == folly::WriteFlags::NONE) {
      writeFlags_ = flags;
    } else {
      assert(flags == writeFlags_);
    }
    return flags_;
  }

  void getAncillaryData(
      folly::WriteFlags flags,
      void* data,
      const folly::AsyncSocket::WriteRequestTag& tag,
      const bool /* byteEventsEnabled */) noexcept override {
    CHECK_EQ(tag, expectedTag_);
    queriedData_ = true;
    if (writeFlags_ == folly::WriteFlags::NONE) {
      writeFlags_ = flags;
    } else {
      assert(flags == writeFlags_);
    }
    assert(data != nullptr);
    memcpy(data, data_, dataSize_);
  }

  uint32_t getAncillaryDataSize(
      folly::WriteFlags flags,
      const folly::AsyncSocket::WriteRequestTag& tag,
      const bool /* byteEventsEnabled */) noexcept override {
    CHECK_EQ(tag, expectedTag_);
    if (writeFlags_ == folly::WriteFlags::NONE) {
      writeFlags_ = flags;
    } else {
      assert(flags == writeFlags_);
    }
    return dataSize_;
  }

  void wroteBytes(
      const folly::AsyncSocket::WriteRequestTag& tag) noexcept override {
    CHECK_EQ(tag, expectedTag_);
    tagLastWritten_ = tag;
  }

  int flags_;
  folly::WriteFlags writeFlags_;
  uint32_t dataSize_;
  void* data_;
  bool queriedFlags_;
  bool queriedData_;
  folly::AsyncSocket::WriteRequestTag expectedTag_{
      folly::AsyncSocket::WriteRequestTag::EmptyDummy()};
  std::optional<folly::AsyncSocket::WriteRequestTag> tagLastWritten_;
};

class TestServer {
 public:
  // Create a TestServer.
  // This immediately starts listening on an ephemeral port.
  explicit TestServer(bool enableTFO = false, int bufSize = -1) : fd_() {
    namespace fsp = folly::portability::sockets;
    fd_ = folly::netops::socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd_ == folly::NetworkSocket()) {
      throw folly::AsyncSocketException(
          folly::AsyncSocketException::INTERNAL_ERROR,
          "failed to create test server socket",
          errno);
    }
    if (folly::netops::set_socket_non_blocking(fd_) != 0) {
      throw folly::AsyncSocketException(
          folly::AsyncSocketException::INTERNAL_ERROR,
          "failed to put test server socket in "
          "non-blocking mode",
          errno);
    }
    if (enableTFO) {
#if FOLLY_ALLOW_TFO
      auto ret = folly::detail::tfo_enable(fd_, 100);
      PCHECK(ret == 0);
#endif
    }

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(nullptr, "0", &hints, &res)) {
      throw folly::AsyncSocketException(
          folly::AsyncSocketException::INTERNAL_ERROR,
          "Attempted to bind address to socket with "
          "bad getaddrinfo",
          errno);
    }

    SCOPE_EXIT {
      freeaddrinfo(res);
    };

    if (bufSize > 0) {
      folly::netops::setsockopt(
          fd_, SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize));
      folly::netops::setsockopt(
          fd_, SOL_SOCKET, SO_RCVBUF, &bufSize, sizeof(bufSize));
    }

    if (folly::netops::bind(fd_, res->ai_addr, res->ai_addrlen)) {
      throw folly::AsyncSocketException(
          folly::AsyncSocketException::INTERNAL_ERROR,
          "failed to bind to async server socket for port 10",
          errno);
    }

    if (folly::netops::listen(fd_, 10) != 0) {
      throw folly::AsyncSocketException(
          folly::AsyncSocketException::INTERNAL_ERROR,
          "failed to listen on test server socket",
          errno);
    }

    address_.setFromLocalAddress(fd_);
    // The local address will contain 0.0.0.0.
    // Change it to 127.0.0.1, so it can be used to connect to the server
    address_.setFromIpPort("127.0.0.1", address_.getPort());
  }

  ~TestServer() {
    if (fd_ != folly::NetworkSocket()) {
      folly::netops::close(fd_);
    }
  }

  // Get the address for connecting to the server
  const folly::SocketAddress& getAddress() const { return address_; }

  folly::NetworkSocket acceptFD(int timeout = 50) {
    folly::netops::PollDescriptor pfd;
    pfd.fd = fd_;
    pfd.events = POLLIN;
    int ret = folly::netops::poll(&pfd, 1, timeout);
    if (ret == 0) {
      throw folly::AsyncSocketException(
          folly::AsyncSocketException::INTERNAL_ERROR,
          "test server accept() timed out");
    } else if (ret < 0) {
      throw folly::AsyncSocketException(
          folly::AsyncSocketException::INTERNAL_ERROR,
          "test server accept() poll failed",
          errno);
    }

    auto acceptedFd = folly::netops::accept(fd_, nullptr, nullptr);
    if (acceptedFd == folly::NetworkSocket()) {
      throw folly::AsyncSocketException(
          folly::AsyncSocketException::INTERNAL_ERROR,
          "test server accept() failed",
          errno);
    }

    return acceptedFd;
  }

  std::shared_ptr<BlockingSocket> accept(int timeout = 50) {
    auto fd = acceptFD(timeout);
    return std::make_shared<BlockingSocket>(fd);
  }

  std::shared_ptr<folly::AsyncSocket> acceptAsync(
      folly::EventBase* evb, int timeout = 50) {
    auto fd = acceptFD(timeout);
    return folly::AsyncSocket::newSocket(evb, fd);
  }

  /**
   * Accept a connection, read data from it, and verify that it matches the
   * data in the specified buffer.
   */
  void verifyConnection(const char* buf, size_t len) {
    // accept a connection
    std::shared_ptr<BlockingSocket> acceptedSocket = accept();
    // read the data and compare it to the specified buffer
    std::unique_ptr<uint8_t[]> readbuf(new uint8_t[len]);
    acceptedSocket->readAll(readbuf.get(), len);
    CHECK_EQ(memcmp(buf, readbuf.get(), len), 0);
    // make sure we get EOF next
    uint32_t bytesRead = acceptedSocket->read(readbuf.get(), len);
    CHECK_EQ(bytesRead, 0);
  }

 private:
  folly::NetworkSocket fd_;
  folly::SocketAddress address_;
};

} // namespace folly::test
