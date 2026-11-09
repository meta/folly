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

#include <chrono>
#include <map>
#include <memory>

#include <folly/Optional.h>
#include <folly/SocketAddress.h>
#include <folly/container/IntrusiveList.h>
#include <folly/futures/Future.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufIovecBuilder.h>
#include <folly/io/SocketOptionMap.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/AsyncSocketException.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/AsyncTransport.h>
#include <folly/io/async/DelayedDestruction.h>
#include <folly/io/async/EventHandler.h>
#include <folly/io/async/IoUringBase.h>
#include <folly/io/async/Liburing.h>
#include <folly/net/NetOpsDispatcher.h>
#include <folly/portability/Sockets.h>
#include <folly/small_vector.h>

#if FOLLY_HAS_LIBURING

namespace folly {
class IoUringBackend;
class IoUringBufferProvider;

class AsyncIoUringSocket : public AsyncSocketTransport {
 public:
  using Cert = folly::AsyncTransportCertificate;
  struct Options {
    Options()
        : allocateNoBufferPoolBuffer(defaultAllocateNoBufferPoolBuffer),
          multishotRecv(true) {}

    static std::unique_ptr<IOBuf> defaultAllocateNoBufferPoolBuffer();
    folly::Function<std::unique_ptr<IOBuf>()> allocateNoBufferPoolBuffer;
    folly::Optional<AsyncWriter::ZeroCopyEnableFunc> zeroCopyEnable;
    bool multishotRecv;
  };

  using UniquePtr = std::unique_ptr<AsyncIoUringSocket, Destructor>;
  explicit AsyncIoUringSocket(
      AsyncTransport::UniquePtr other, Options&& options = Options{});
  explicit AsyncIoUringSocket(AsyncSocket* sock, Options&& options = Options{});
  explicit AsyncIoUringSocket(EventBase* evb, Options&& options = Options{});
  explicit AsyncIoUringSocket(
      EventBase* evb, NetworkSocket ns, Options&& options = Options{});

  static bool supports(EventBase* backend);
  static bool supportsZcRx(EventBase* backend);

  void connect(
      AsyncSocket::ConnectCallback* callback,
      const folly::SocketAddress& address,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
      SocketOptionMap const& options = emptySocketOptionMap,
      const BindOptions& bindOptions = anyAddress(),
      const std::string& ifName = std::string()) noexcept;

  void connect(
      ConnectCallback* callback,
      const folly::SocketAddress& address,
      int timeout,
      SocketOptionMap const& options,
      const BindOptions& bindOptions,
      const std::string& ifName) noexcept override {
    connect(
        callback,
        address,
        std::chrono::milliseconds(timeout),
        options,
        bindOptions,
        ifName);
  }

  std::chrono::nanoseconds getConnectTime() const {
    return connectEndTime_ - connectStartTime_;
  }

  void cancelConnect() override;

  /*
   * This flag controls whether or not IP_BIND_ADDRESS_NO_PORT is enabled for
   * AsyncSocket sockets. This is enabled by default.
   */
  void setBindAddressNoPort(bool flag) { bindAddressNoPort_ = flag; }

  // AsyncSocketBase
  EventBase* getEventBase() const override { return evb_; }

  // AsyncReader
  void setReadCB(ReadCallback* callback) override;

  ReadCallback* getReadCallback() const override {
    return readSqe_->readCallback();
  }
  std::unique_ptr<IOBuf> takePreReceivedData() override {
    return readSqe_->takePreReceivedData();
  }

  // AsyncWriter
  void write(WriteCallback*, const void*, size_t, WriteFlags = WriteFlags::NONE)
      override;
  void writev(
      WriteCallback*, const iovec*, size_t, WriteFlags = WriteFlags::NONE)
      override;
  void writeChain(
      WriteCallback* callback,
      std::unique_ptr<IOBuf>&& buf,
      WriteFlags flags) override;
  bool canZC(std::unique_ptr<IOBuf> const& buf) const;

  // AsyncTransport
  void close() override;
  void closeNow() override;
  void closeWithReset() override;
  void shutdownWrite() override;
  void shutdownWriteNow() override;

  bool good() const override;
  bool readable() const override { return good(); }
  bool error() const override;
  bool hangup() const override;

  bool connecting() const override {
    return connectSqe_ && connectSqe_->inFlight();
  }

  void attachEventBase(EventBase*) override;
  void detachEventBase() override;
  bool isDetachable() const override;

  uint32_t getSendTimeout() const override {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(writeTimeoutTime_)
            .count());
  }

  void setSendTimeout(uint32_t ms) override;

  void getLocalAddress(SocketAddress* address) const override;

  void getPeerAddress(SocketAddress*) const override;

  void setPreReceivedData(std::unique_ptr<IOBuf> data) override;
  void cacheAddresses() override;

  /**
   * @return True iff end of record tracking is enabled
   */
  bool isEorTrackingEnabled() const override { return false; }

  void setEorTracking(bool) override {
    // don't support this.
    // as far as I can see this is only used by AsyncSSLSocket, but TLS1.3
    // supercedes this so I think we can ignore it.
    throw std::runtime_error(
        "AsyncIoUringSocket::setEorTracking not supported");
  }

  size_t getAppBytesWritten() const override { return getRawBytesWritten(); }
  size_t getRawBytesWritten() const override { return bytesWritten_; }
  size_t getAppBytesReceived() const override { return getRawBytesReceived(); }
  size_t getRawBytesReceived() const override;

  const AsyncTransport* getWrappedTransport() const override { return nullptr; }

  // AsyncSocketTransport
  int setNoDelay(bool noDelay) override;
  int setSockOpt(
      int level, int optname, const void* optval, socklen_t optsize) override;

  std::string getSecurityProtocol() const override { return securityProtocol_; }
  std::string getApplicationProtocol() const noexcept override {
    return applicationProtocol_;
  }
  NetworkSocket getNetworkSocket() const override { return fd_; }

  void setSecurityProtocol(std::string s) { securityProtocol_ = std::move(s); }
  void setApplicationProtocol(std::string s) {
    applicationProtocol_ = std::move(s);
  }

  const folly::AsyncTransportCertificate* getPeerCertificate() const override {
    return peerCert_.get();
  }

  const folly::AsyncTransportCertificate* getSelfCertificate() const override {
    return selfCert_.get();
  }

  void dropPeerCertificate() noexcept override { peerCert_.reset(); }

  void dropSelfCertificate() noexcept override { selfCert_.reset(); }

  void setPeerCertificate(const std::shared_ptr<const Cert>& peerCert) {
    peerCert_ = peerCert;
  }

  void setSelfCertificate(const std::shared_ptr<const Cert>& selfCert) {
    selfCert_ = selfCert;
  }

  void asyncDetachFd(AsyncDetachFdCallback* callback);
  bool readSqeInFlight() const { return readSqe_->inFlight(); }
  bool getTFOSucceeded() const override;
  void enableTFO() override {
    // No-op if folly does not allow tfo
#if FOLLY_ALLOW_TFO
    VLOG(5) << "AsyncIoUringSocket::enableTFO()";
    enableTFO_ = true;
#endif
  }

  void appendPreReceive(std::unique_ptr<IOBuf> iobuf) noexcept;

 protected:
  ~AsyncIoUringSocket() override;

 private:
  friend class ReadSqe;
  friend class WriteSqe;
  friend class
      AsyncIoUringSocketTest_ZeroCopyReadNotifiesScarcityBeforeBuffer_Test;
  void setFd(NetworkSocket ns);
  void registerFd();
  void unregisterFd();
  void readProcessSubmit(
      struct io_uring_sqe* sqe,
      IoUringBufferProvider* bufferProvider,
      size_t* maxSize,
      IoUringBufferProvider* usedBufferProvider) noexcept;
  void readCallback(
      int res,
      uint32_t flags,
      size_t maxSize,
      IoUringBufferProvider* bufferProvider) noexcept;
  void allowReads();
  void previousReadDone();
  void processWriteQueue() noexcept;
  void setStateEstablished();
  void writeDone() noexcept;
  void doSubmitWrite() noexcept;
  void doReSubmitWrite() noexcept;
  void failAllWrites() noexcept;
  void submitRead(bool now = false);
  void processConnectSubmit(
      struct io_uring_sqe* sqe, sockaddr_storage& storage);
  void processConnectResult(const io_uring_cqe* cqe);
  void processConnectTimeout();
  void processFastOpenResult(const io_uring_cqe* cqe) noexcept;
  void startSendTimeout();
  void sendTimeoutExpired();
  void failWrite(const AsyncSocketException& ex);
  void readEOF();
  void readError();
  NetworkSocket takeFd();
  bool setZeroCopy(bool enable) override;
  bool getZeroCopy() const override;
  void setZeroCopyEnableFunc(AsyncWriter::ZeroCopyEnableFunc func) override;

  enum class State {
    None,
    Connecting,
    Established,
    Closed,
    Error,
    FastOpen,
  };

  static std::string toString(State s);
  std::string stateAsString() const { return toString(state_); }

  struct ReadSqe : IoSqeBase, DelayedDestruction {
    using UniquePtr = std::unique_ptr<ReadSqe, Destructor>;
    explicit ReadSqe(AsyncIoUringSocket* parent);
    void processSubmit(struct io_uring_sqe* sqe) noexcept override;
    void callback(const io_uring_cqe* cqe) noexcept override;
    void callbackCancelled(const io_uring_cqe* cqe) noexcept override;

    void setReadCallback(ReadCallback* callback, bool submitNow);
    ReadCallback* readCallback() const { return readCallback_; }

    size_t bytesReceived() const { return bytesReceived_; }

    std::unique_ptr<IOBuf> takePreReceivedData();
    void appendPreReceive(std::unique_ptr<IOBuf> data) noexcept {
      appendReadData(std::move(data), preReceivedData_);
    }

    void destroy() override {
      parent_ = nullptr;
      DelayedDestruction::destroy();
    }

    bool waitingForOldEventBaseRead() const;
    void setOldEventBaseRead(folly::SemiFuture<std::unique_ptr<IOBuf>>&& f) {
      oldEventBaseRead_ = std::move(f);
    }
    void attachEventBase();
    folly::Optional<folly::SemiFuture<std::unique_ptr<IOBuf>>>
    detachEventBase();

    void setUseZeroCopyRx(bool val) { useZeroCopyRx_ = val; }

    void sendZeroCopyReadBuf(std::unique_ptr<IOBuf> buf, bool scarce) noexcept;

   private:
    ~ReadSqe() override = default;
    void appendReadData(
        std::unique_ptr<IOBuf> data, std::unique_ptr<IOBuf>& overflow) noexcept;
    void sendReadBuf(
        std::unique_ptr<IOBuf> buf, std::unique_ptr<IOBuf>& overflow) noexcept;
    bool readCallbackUseIoBufs() const;
    void invalidState(ReadCallback* callback);
    void processOldEventBaseRead();

    bool isEOF(const io_uring_cqe* cqe) noexcept;

    IoUringBufferProvider* lastUsedBufferProvider_;
    ReadCallback* readCallback_ = nullptr;
    AsyncIoUringSocket* parent_;
    size_t maxSize_;
    uint64_t setReadCbCount_{0};
    size_t bytesReceived_{0};

    std::unique_ptr<IOBuf> queuedReceivedData_;
    std::unique_ptr<IOBuf> preReceivedData_;
    std::unique_ptr<IOBuf> tmpBuffer_;
    bool supportsMultishotRecv_ =
        false; // todo: this can be per process instead of per socket
    bool supportsZeroCopyRx_ = false;
    bool useZeroCopyRx_ = false;

    folly::Optional<folly::SemiFuture<std::unique_ptr<IOBuf>>>
        oldEventBaseRead_;
    std::shared_ptr<folly::Unit> alive_;
  };

  struct CloseSqe : IoSqeBase {
    explicit CloseSqe(AsyncIoUringSocket* parent)
        : IoSqeBase(IoSqeBase::Type::Close), parent_(parent) {
      setEventBase(parent->evb_);
    }

    void processSubmit(struct io_uring_sqe* sqe) noexcept override {
      parent_->closeProcessSubmit(sqe);
    }
    void callback(const io_uring_cqe*) noexcept override { delete this; }
    void callbackCancelled(const io_uring_cqe*) noexcept override {
      delete this;
    }
    AsyncIoUringSocket* parent_;
  };

  struct WriteSqe final : IoSqeBase {
    struct ZeroCopyOptions {
      bool zeroCopy{false};
      bool fixedBuf{false};
      uint16_t fixedBufIndex{0};
    };
    explicit WriteSqe(
        AsyncIoUringSocket* parent,
        WriteCallback* callback,
        std::unique_ptr<IOBuf>&& buf,
        WriteFlags flags,
        ZeroCopyOptions options);
    ~WriteSqe() override { VLOG(5) << "~WriteSqe() " << this; }

    void processSubmit(struct io_uring_sqe* sqe) noexcept override;
    void callback(const io_uring_cqe* cqe) noexcept override;
    void callbackCancelled(const io_uring_cqe* cqe) noexcept override;
    int sendMsgFlags() const;
    std::pair<
        folly::SemiFuture<std::vector<std::pair<int, uint32_t>>>,
        WriteSqe*>
    detachEventBase();

    SafeIntrusiveListHook writeQueueHook_;
    AsyncIoUringSocket* parent_;
    WriteCallback* callback_;
    std::unique_ptr<IOBuf> buf_;
    WriteFlags flags_;
    static constexpr size_t kSmallIoVecSize = 16;
    small_vector<struct iovec, kSmallIoVecSize> iov_;
    size_t totalLength_;
    struct msghdr msg_;

    ZeroCopyOptions zcOptions_;
    int refs_ = 1;
    folly::Function<bool(int, uint32_t)> detachedSignal_;
  };
  using WriteSqeList = SafeIntrusiveList<WriteSqe, &WriteSqe::writeQueueHook_>;

  class WriteTimeout : public AsyncTimeout {
   public:
    explicit WriteTimeout(AsyncIoUringSocket* socket)
        : AsyncTimeout(socket->evb_), socket_(socket) {}

    void timeoutExpired() noexcept override { socket_->sendTimeoutExpired(); }

   private:
    AsyncIoUringSocket* socket_;
  };

  struct ConnectSqe : IoSqeBase, AsyncTimeout {
    explicit ConnectSqe(AsyncIoUringSocket* parent)
        : IoSqeBase(IoSqeBase::Type::Connect),
          AsyncTimeout(parent->evb_),
          parent_(parent) {
      setEventBase(parent->evb_);
    }

    void processSubmit(struct io_uring_sqe* sqe) noexcept override {
      parent_->processConnectSubmit(sqe, addrStorage);
    }
    void callback(const io_uring_cqe* cqe) noexcept override {
      parent_->processConnectResult(cqe);
    }
    void callbackCancelled(const io_uring_cqe*) noexcept override {
      delete this;
    }
    void timeoutExpired() noexcept override {
      if (!cancelled()) {
        parent_->processConnectTimeout();
      }
    }
    AsyncIoUringSocket* parent_;
    sockaddr_storage addrStorage;
  };

  struct FastOpenSqe : IoSqeBase {
    explicit FastOpenSqe(
        AsyncIoUringSocket* parent,
        SocketAddress const& addr,
        std::unique_ptr<AsyncIoUringSocket::WriteSqe> initialWrite);
    void processSubmit(struct io_uring_sqe* sqe) noexcept override;
    void cleanupMsg() noexcept;
    void callback(const io_uring_cqe* cqe) noexcept override {
      cleanupMsg();
      parent_->processFastOpenResult(cqe);
    }
    void callbackCancelled(const io_uring_cqe*) noexcept override {
      delete this;
    }

    AsyncIoUringSocket* parent_;
    std::unique_ptr<AsyncIoUringSocket::WriteSqe> initialWrite;
    size_t addrLen_;
    sockaddr_storage addrStorage;
  };

  EventBase* evb_ = nullptr;
  NetworkSocket fd_;
  IoUringBackend* backend_ = nullptr;
  Options options_;
  mutable SocketAddress localAddress_;
  mutable SocketAddress peerAddress_;
  IoUringFdRegistrationRecord* fdRegistered_ = nullptr;
  int usedFd_ = -1;
  unsigned int mbFixedFileFlags_ = 0;
  std::unique_ptr<CloseSqe> closeSqe_{new CloseSqe(this)};

  State state_ = State::None;

  // read
  friend struct DetachFdState;
  ReadSqe::UniquePtr readSqe_;

  // write
  std::chrono::milliseconds writeTimeoutTime_{0};
  WriteTimeout writeTimeout_{this};
  WriteSqe* writeSqeActive_ = nullptr;
  WriteSqeList writeSqeQueue_;
  size_t bytesWritten_{0};

  // connect
  std::unique_ptr<ConnectSqe> connectSqe_;
  AsyncSocket::ConnectCallback* connectCallback_;
  std::chrono::milliseconds connectTimeout_{0};
  std::chrono::steady_clock::time_point connectStartTime_;
  std::chrono::steady_clock::time_point connectEndTime_;
  bool bindAddressNoPort_{true};

  // stopTLS helpers:
  std::string securityProtocol_;
  std::string applicationProtocol_;

  std::shared_ptr<const Cert> selfCert_;
  std::shared_ptr<const Cert> peerCert_;

  // shutdown:
  int shutdownFlags_ = 0;

  // TCP fast open
  std::unique_ptr<FastOpenSqe> fastOpenSqe_;
  bool enableTFO_ = false;

  // detach event base
  bool isDetaching_ = false;
  uint32_t eventBaseAttachCount_{0};
  Optional<SemiFuture<std::vector<std::pair<int, uint32_t>>>>
      detachedWriteResult_;
  std::shared_ptr<folly::Unit> alive_;

  void closeProcessSubmit(struct io_uring_sqe* sqe);
};
} // namespace folly

#endif
