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

#include <folly/Conv.h>
#include <folly/detail/SocketFastOpen.h>
#include <folly/io/Cursor.h>
#include <folly/io/async/AsyncIoUringSocket.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/IoUringEventBaseLocal.h>
#include <folly/io/async/IoUringProvidedBufferRing.h>
#include <folly/memory/Malloc.h>
#include <folly/portability/SysUio.h>
#include <folly/tracing/StaticTracepoint.h>

#if FOLLY_HAS_LIBURING

namespace fsp = folly::portability::sockets;

namespace folly {

namespace {
enum ShutdownFlags {
  ShutFlags_WritePending = 1,
  ShutFlags_Write = 2,
  ShutFlags_Read = 4,
};

AsyncSocket* getAsyncSocket(AsyncTransport::UniquePtr const& o) {
  auto* raw = o->getUnderlyingTransport<folly::AsyncSocket>();
  if (!raw) {
    throw std::runtime_error("need to take a AsyncSocket");
  }
  return raw;
}

int ensureSocketReturnCode(int x, char const* message) {
  if (x >= 0) {
    return x;
  }
  auto errnoCopy = errno;
  throw AsyncSocketException(
      AsyncSocketException::INTERNAL_ERROR, message, errnoCopy);
}

NetworkSocket makeConnectSocket(SocketAddress const& peerAddress) {
  int fd = ensureSocketReturnCode(
      ::socket(peerAddress.getFamily(), SOCK_STREAM, 0),
      "failed to create socket");
  ensureSocketReturnCode(fcntl(fd, F_SETFD, FD_CLOEXEC), "set cloexec");

  // copied from folly::AsyncSocket, default enable TCP_NODELAY
  // If setNoDelay() fails, we continue anyway; this isn't a fatal error.
  // setNoDelay() will log an error message if it fails.
  int nodelay = 1;
  int ret = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  if (ret != 0) {
    VLOG(1) << "setNoDelay failed " << folly::errnoStr(errno);
  }
  return NetworkSocket{fd};
}

IoUringBackend* getBackendFromEventBase(EventBase* evb) {
  auto* b = IoUringEventBaseLocal::try_get(evb);
  if (!b) {
    b = dynamic_cast<IoUringBackend*>(evb->getBackend());
  }
  if (!b) {
    throw std::runtime_error("need to take a IoUringBackend event base");
  }
  return b;
}

} // namespace

AsyncIoUringSocket::AsyncIoUringSocket(
    folly::AsyncSocket* other, Options&& options)
    : AsyncIoUringSocket(other->getEventBase(), std::move(options)) {
  setPreReceivedData(other->takePreReceivedData());
  setFd(other->detachNetworkSocket());
  state_ = State::Established;
}

AsyncIoUringSocket::AsyncIoUringSocket(
    AsyncTransport::UniquePtr other, Options&& options)
    : AsyncIoUringSocket(getAsyncSocket(other), std::move(options)) {}

AsyncIoUringSocket::AsyncIoUringSocket(EventBase* evb, Options&& options)
    : evb_(evb), options_(std::move(options)) {
  backend_ = getBackendFromEventBase(evb);

  if (!backend_->hasBufferProvider()) {
    throw std::runtime_error("require a IoUringBackend with a buffer provider");
  }
  readSqe_ = ReadSqe::UniquePtr(new ReadSqe(this));
}

AsyncIoUringSocket::AsyncIoUringSocket(
    EventBase* evb, NetworkSocket ns, Options&& options)
    : AsyncIoUringSocket(evb, std::move(options)) {
  setFd(ns);
  state_ = State::Established;
}

std::string AsyncIoUringSocket::toString(AsyncIoUringSocket::State s) {
  switch (s) {
    case State::None:
      return "None";
    case State::Connecting:
      return "Connecting";
    case State::Established:
      return "Established";
    case State::Closed:
      return "Closed";
    case State::Error:
      return "Error";
    case State::FastOpen:
      return "FastOpen";
  }
  return to<std::string>("Unknown val=", (int)s);
}

std::unique_ptr<IOBuf>
AsyncIoUringSocket::Options::defaultAllocateNoBufferPoolBuffer() {
  size_t size = goodMallocSize(16384);
  VLOG(2) << "UseProvidedBuffers slow path starting with " << size << " bytes ";
  return IOBuf::create(size);
}

AsyncIoUringSocket::ReadSqe::ReadSqe(AsyncIoUringSocket* parent)
    : IoSqeBase(IoSqeBase::Type::Read), parent_(parent) {
  supportsMultishotRecv_ = parent->options_.multishotRecv;
  // If the backend for this socket has an IoUringZeroCopyBufferPool, then zero
  // copy is enabled implicitly.
  supportsZeroCopyRx_ = parent->backend_->zcBufferPool() != nullptr;
  setEventBase(parent->evb_);
}

AsyncIoUringSocket::~AsyncIoUringSocket() {
  VLOG(3) << "~AsyncIoUringSocket() " << this;

  // this is a bit unnecessary if we are already closed, but proper state
  // tracking is coming later and will be easier to handle then
  closeNow();

  // evb_/backend_ might be null here, but then none of these will be in flight

  // cancel outstanding
  if (readSqe_->inFlight()) {
    VLOG(3) << "cancel reading " << readSqe_.get();
    readSqe_->setReadCallback(
        nullptr, false); // not detaching, actually closing
    readSqe_->detachEventBase();
    backend_->cancel(readSqe_.release());
  }

  if (closeSqe_ && closeSqe_->inFlight()) {
    LOG_EVERY_N(WARNING, 100) << " closeSqe_ still in flight";
    closeSqe_
        ->markCancelled(); // still need to actually close it and it has no data
    closeSqe_.release();
  }
  if (connectSqe_ && connectSqe_->inFlight()) {
    VLOG(3) << "cancel connect " << connectSqe_.get();
    connectSqe_->cancelTimeout();
    backend_->cancel(connectSqe_.release());
  }
  if (fastOpenSqe_ && fastOpenSqe_->inFlight()) {
    VLOG(3) << "cancel fastopen " << fastOpenSqe_.get();
    backend_->cancel(fastOpenSqe_.release());
  }

  VLOG(2) << "~AsyncIoUringSocket() " << this << " have active "
          << writeSqeActive_ << " queue=" << writeSqeQueue_.size();

  if (writeSqeActive_) {
    // if we are detaching, then the write will not have been submitted yet
    if (writeSqeActive_->inFlight()) {
      backend_->cancel(writeSqeActive_);
    } else {
      delete writeSqeActive_;
    }
  }

  while (!writeSqeQueue_.empty()) {
    WriteSqe* w = &writeSqeQueue_.front();
    CHECK(!w->inFlight());
    writeSqeQueue_.pop_front();
    delete w;
  }
}

bool AsyncIoUringSocket::supports(EventBase* eb) {
  IoUringBackend* io = dynamic_cast<IoUringBackend*>(eb->getBackend());
  if (!io) {
    io = IoUringEventBaseLocal::try_get(eb);
  }
  return io && io->hasBufferProvider();
}

bool AsyncIoUringSocket::supportsZcRx(EventBase* eb) {
  IoUringBackend* io = dynamic_cast<IoUringBackend*>(eb->getBackend());
  return io && io->zcBufferPool() != nullptr;
}

void AsyncIoUringSocket::connect(
    AsyncSocket::ConnectCallback* callback,
    const folly::SocketAddress& address,
    std::chrono::milliseconds timeout,
    SocketOptionMap const& options,
    const BindOptions& bindOptions,
    const std::string& ifName) noexcept {
  VLOG(4) << "AsyncIoUringSocket::connect() this=" << this << " to=" << address
          << " fastopen=" << enableTFO_;
  evb_->dcheckIsInEventBaseThread();
  DestructorGuard dg(this);
  connectTimeout_ = timeout;
  connectEndTime_ = connectStartTime_ = std::chrono::steady_clock::now();
  if (!connectSqe_) {
    connectSqe_ = std::make_unique<ConnectSqe>(this);
  }
  if (connectSqe_->inFlight()) {
    if (auto* fd = std::get_if<NetworkSocket>(&bindOptions)) {
      fileops::close(fd->toFd());
    }
    callback->connectErr(AsyncSocketException(
        AsyncSocketException::NOT_OPEN, "connection in flight", -1));
    return;
  }
  if (fd_ != NetworkSocket{}) {
    if (auto* fd = std::get_if<NetworkSocket>(&bindOptions)) {
      fileops::close(fd->toFd());
    }
    callback->connectErr(AsyncSocketException(
        AsyncSocketException::NOT_OPEN, "connection is connected", -1));
    return;
  }
  connectCallback_ = callback;
  peerAddress_ = address;

  if (auto* boundFd = std::get_if<NetworkSocket>(&bindOptions)) {
    struct sockaddr_storage peerAddr{};
    socklen_t peerLen = sizeof(peerAddr);
    if (::getpeername(
            boundFd->toFd(),
            reinterpret_cast<struct sockaddr*>(&peerAddr),
            &peerLen) == 0) {
      fileops::close(boundFd->toFd());
      callback->connectErr(AsyncSocketException(
          AsyncSocketException::INVALID_STATE, "boundFd is already connected"));
      return;
    }
    setFd(*boundFd);
  } else {
    setFd(makeConnectSocket(address));
  }

  {
    auto result =
        applySocketOptions(fd_, options, SocketOptionKey::ApplyPos::PRE_BIND);
    if (result != 0) {
      callback->connectErr(AsyncSocketException(
          AsyncSocketException::INTERNAL_ERROR,
          "failed to set socket option",
          result));
      return;
    }
  }

  // bind the socket to the interface
  if (!ifName.empty() &&
      setSockOpt(
          SOL_SOCKET, SO_BINDTODEVICE, ifName.c_str(), ifName.length())) {
    auto errnoCopy = errno;
    callback->connectErr(AsyncSocketException(
        AsyncSocketException::NOT_OPEN,
        "failed to bind to device: " + ifName,
        errnoCopy));
    return;
  }

  // bind the socket
  if (auto* bindAddr = std::get_if<folly::SocketAddress>(&bindOptions);
      bindAddr && *bindAddr != anyAddress()) {
    sockaddr_storage addrStorage;
    auto saddr = reinterpret_cast<sockaddr*>(&addrStorage);

    int one = 1;
#if defined(IP_BIND_ADDRESS_NO_PORT) && !FOLLY_MOBILE
    // If the any port is specified with a non-any address this is typically
    // a client socket. However, calling bind before connect without
    // IP_BIND_ADDRESS_NO_PORT forces the OS to find a unique port relying
    // on only the local tuple. This limits the range of available ephemeral
    // ports.  Using the IP_BIND_ADDRESS_NO_PORT delays assigning a port until
    // connect expanding the available port range, unless
    // setBindAddressNoPort() is called.
    if (bindAddr->getPort() == 0) {
      if (bindAddressNoPort_ &&
          setSockOpt(IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &one, sizeof(one))) {
        auto errnoCopy = errno;
        callback->connectErr(AsyncSocketException(
            AsyncSocketException::NOT_OPEN,
            "failed to setsockopt IP_BIND_ADDRESS_NO_PORT prior to bind on " +
                bindAddr->describe(),
            errnoCopy));
        return;
      }
    } else {
#else
    {
#endif
      if (setSockOpt(SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one))) {
        auto errnoCopy = errno;
        callback->connectErr(AsyncSocketException(
            AsyncSocketException::NOT_OPEN,
            "failed to setsockopt SO_REUSEADDR prior to bind on " +
                bindAddr->describe(),
            errnoCopy));
        return;
      }
    }

    bindAddr->getAddress(&addrStorage);

    if (::bind(fd_.toFd(), saddr, bindAddr->getActualSize()) != 0) {
      auto errnoCopy = errno;
      callback->connectErr(AsyncSocketException(
          AsyncSocketException::NOT_OPEN,
          "failed to bind to async io_uring socket: " + bindAddr->describe(),
          errnoCopy));
      return;
    }
  }

  {
    auto result =
        applySocketOptions(fd_, options, SocketOptionKey::ApplyPos::POST_BIND);
    if (result != 0) {
      callback->connectErr(AsyncSocketException(
          AsyncSocketException::INTERNAL_ERROR,
          "failed to set socket option",
          result));
      return;
    }
  }

  connectCallback_->preConnect(fd_);
  if (connectTimeout_.count() > 0) {
    if (!connectSqe_->scheduleTimeout(
            connectTimeout_ + std::chrono::milliseconds(3000))) {
      connectCallback_->connectErr(AsyncSocketException(
          AsyncSocketException::INTERNAL_ERROR,
          "failed to schedule connect timeout"));
      connectCallback_ = nullptr;
      connectSqe_.reset();
      return;
    }
  }
  // if TCP Fast Open is
  if (enableTFO_) {
    state_ = State::FastOpen;
    VLOG(5) << "Not submitting connect as in fast open";
    connectCallback_->connectSuccess();
    connectCallback_ = nullptr;
  } else {
    state_ = State::Connecting;
    backend_->submit(*connectSqe_);
  }
}

void AsyncIoUringSocket::cancelConnect() {
  connectCallback_ = nullptr;
  if (state_ == State::Connecting || state_ == State::FastOpen) {
    closeNow();
  }
}

void AsyncIoUringSocket::processConnectSubmit(
    struct io_uring_sqe* sqe, sockaddr_storage& storage) {
  auto len = peerAddress_.getAddress(&storage);
  io_uring_prep_connect(sqe, usedFd_, (struct sockaddr*)&storage, len);
  sqe->flags |= mbFixedFileFlags_;
}

void AsyncIoUringSocket::setStateEstablished() {
  state_ = State::Established;
  allowReads();
  processWriteQueue();
}

void AsyncIoUringSocket::appendPreReceive(
    std::unique_ptr<IOBuf> iobuf) noexcept {
  readSqe_->appendPreReceive(std::move(iobuf));
}

void AsyncIoUringSocket::allowReads() {
  if (readSqe_->readCallback() && !readSqe_->inFlight()) {
    auto cb = readSqe_->readCallback();
    setReadCB(cb);
  }
}

void AsyncIoUringSocket::previousReadDone() {
  VLOG(4) << "AsyncIoUringSocket::previousReadDone( " << this << ") cb="
          << readSqe_->readCallback() << " in flight=" << readSqe_->inFlight();
  allowReads();
}

void AsyncIoUringSocket::processConnectResult(const io_uring_cqe* cqe) {
  auto res = cqe->res;
  VLOG(5) << "AsyncIoUringSocket::processConnectResult(" << this
          << ") res=" << res;
  DestructorGuard dg(this);
  connectSqe_.reset();
  connectEndTime_ = std::chrono::steady_clock::now();
  if (res == 0) {
    setStateEstablished();
    if (connectCallback_) {
      connectCallback_->connectSuccess();
    }
  } else {
    state_ = State::Error;
    if (connectCallback_) {
      connectCallback_->connectErr(AsyncSocketException(
          AsyncSocketException::NOT_OPEN, "connect failed", -res));
    }
  }
  connectCallback_ = nullptr;
}

void AsyncIoUringSocket::processConnectTimeout() {
  VLOG(5) << "AsyncIoUringSocket::processConnectTimeout(this=" << this
          << ") connectInFlight=" << connectSqe_->inFlight()
          << " state=" << stateAsString();
  DestructorGuard dg(this);
  if (connectSqe_->inFlight()) {
    backend_->cancel(connectSqe_.release());
  } else {
    connectSqe_.reset();
  }
  connectEndTime_ = std::chrono::steady_clock::now();
  connectCallback_->connectErr(
      AsyncSocketException(AsyncSocketException::TIMED_OUT, "timeout"));
  connectCallback_ = nullptr;
}

void AsyncIoUringSocket::processFastOpenResult(
    const io_uring_cqe* cqe) noexcept {
  VLOG(4) << "processFastOpenResult() this=" << this << " res=" << cqe->res
          << " flags=" << cqe->flags;
  if (cqe->res >= 0) {
    io_uring_cqe tmp{};
    tmp.res = 0;
    processConnectResult(&tmp);
    writeSqeActive_ = fastOpenSqe_->initialWrite.release();
    writeSqeActive_->callback(cqe);
  } else {
    VLOG(4) << "TFO falling back, did not connect, res = " << cqe->res;
    DCHECK(connectSqe_);
    backend_->submit(*connectSqe_);
    writeSqeQueue_.push_back(*fastOpenSqe_->initialWrite.release());
  }
  fastOpenSqe_.reset();
}

inline bool AsyncIoUringSocket::ReadSqe::readCallbackUseIoBufs() const {
  return readCallback_ && readCallback_->isBufferMovable();
}

void AsyncIoUringSocket::readEOF() {
  shutdownFlags_ |= ShutFlags_Read;
}

void AsyncIoUringSocket::readError() {
  VLOG(4) << " AsyncIoUringSocket::readError() this=" << this;
  state_ = State::Error;
  shutdownFlags_ |= ShutFlags_Read | ShutFlags_Write;
}

void AsyncIoUringSocket::setReadCB(ReadCallback* callback) {
  bool submitNow =
      state_ != State::FastOpen && state_ != State::Connecting && !isDetaching_;
  VLOG(4) << "setReadCB state=" << stateAsString()
          << " isDetaching_=" << isDetaching_;
  readSqe_->setReadCallback(callback, submitNow);
}

void AsyncIoUringSocket::submitRead(bool now) {
  VLOG(9) << "AsyncIoUringSocket::submitRead " << now
          << " sqe=" << readSqe_.get();
  if (readSqe_->waitingForOldEventBaseRead()) {
    // don't actually submit, wait for old event base
    return;
  }
  if (now) {
    backend_->submitNow(*readSqe_);
  } else {
    backend_->submitSoon(*readSqe_);
  }
}

void AsyncIoUringSocket::ReadSqe::invalidState(ReadCallback* callback) {
  VLOG(4) << "AsyncSocket(this=" << this << "): setReadCallback(" << callback
          << ") called in invalid state ";

  AsyncSocketException ex(
      AsyncSocketException::NOT_OPEN,
      "setReadCallback() called  io_uring with socket in "
      "invalid state");
  if (callback) {
    callback->readErr(ex);
  }
}

bool AsyncIoUringSocket::error() const {
  VLOG(2) << "AsyncIoUringSocket::error(this=" << this
          << ") state=" << stateAsString();
  return state_ == State::Error;
}

bool AsyncIoUringSocket::good() const {
  VLOG(2) << "AsyncIoUringSocket::good(this=" << this
          << ") state=" << stateAsString() << " evb_=" << evb_
          << " shutdownFlags_=" << shutdownFlags_ << " backend_=" << backend_;
  if (!evb_ || !backend_) {
    return false;
  }
  if (shutdownFlags_) {
    return false;
  }
  switch (state_) {
    case State::Connecting:
    case State::Established:
    case State::FastOpen:
      return true;
    case State::None:
    case State::Closed:
    case State::Error:
      return false;
  }
  return false;
}

bool AsyncIoUringSocket::hangup() const {
  if (fd_ == NetworkSocket()) {
    // sanity check, no one should ask for hangup if we are not connected.
    assert(false);
    return false;
  }
  struct pollfd fds[1];
  fds[0].fd = fd_.toFd();
  fds[0].events = POLLRDHUP;
  fds[0].revents = 0;
  ::poll(&fds[0], 1, 0);
  return (fds[0].revents & (POLLRDHUP | POLLHUP)) != 0;
}

void AsyncIoUringSocket::ReadSqe::setReadCallback(
    ReadCallback* callback, bool submitNow) {
  VLOG(5)
      << "AsyncIoUringSocket::setReadCB() this=" << this << " cb=" << callback
      << " cbWas=" << readCallback_ << " count=" << setReadCbCount_
      << " movable=" << (callback && callback->isBufferMovable() ? "YES" : "NO")
      << " inflight=" << inFlight() << " good_=" << parent_->good()
      << " submitNow=" << submitNow;

  if (callback == readCallback_ && (!submitNow || inFlight())) {
    VLOG(9) << "cb the same, skipping";
    return;
  }
  setReadCbCount_++;
  readCallback_ = callback;
  if (!callback) {
    return;
  }
  if (!submitNow) {
    // allowable to set a read callback here
    VLOG(5) << "AsyncIoUringSocket::setReadCB() this=" << this
            << " ignoring callback for now ";
    return;
  }
  if (!parent_->good()) {
    readCallback_ = nullptr;
    invalidState(callback);
    return;
  }

  processOldEventBaseRead();

  // callback may change after these so make sure to check
  if (readCallback_ && preReceivedData_) {
    sendReadBuf(std::move(preReceivedData_), preReceivedData_);
  }

  if (readCallback_ && queuedReceivedData_) {
    sendReadBuf(std::move(queuedReceivedData_), queuedReceivedData_);
  }

  if (readCallback_ && !inFlight()) {
    parent_->submitRead();
  }
}

void AsyncIoUringSocket::ReadSqe::processOldEventBaseRead() {
  if (!oldEventBaseRead_ || !oldEventBaseRead_->isReady()) {
    return;
  }

  auto res = std::move(*oldEventBaseRead_).get();
  oldEventBaseRead_.reset();
  VLOG(4) << "using old event base data: " << res.get()
          << " len=" << (res ? res->length() : 0);
  if (res && res->length()) {
    if (queuedReceivedData_) {
      queuedReceivedData_->appendToChain(std::move(res));
    } else {
      queuedReceivedData_ = std::move(res);
    }
  }
}

bool AsyncIoUringSocket::ReadSqe::isEOF(const io_uring_cqe* cqe) noexcept {
  if (supportsZeroCopyRx_ && useZeroCopyRx_) {
    return cqe->res == 0 && cqe->flags == 0;
  }
  return cqe->res == 0;
}

void AsyncIoUringSocket::ReadSqe::callback(const io_uring_cqe* cqe) noexcept {
  auto res = cqe->res;
  auto flags = cqe->flags;

  VLOG(5) << "AsyncIoUringSocket::ReadSqe::readCallback() this=" << this
          << " parent=" << parent_ << " cb=" << readCallback_ << " res=" << res
          << " max=" << maxSize_ << " inflight=" << inFlight()
          << " has_buffer=" << !!(flags & IORING_CQE_F_BUFFER)
          << " bytes_received=" << bytesReceived_;
  DestructorGuard dg(this);
  bool hasMore = (flags & IORING_CQE_F_BUF_MORE) != 0;
  auto buffer_guard = makeGuard([&] {
    CHECK(!(flags & IORING_CQE_F_BUFFER))
        << "Buffer guard invoked but IORING_CQE_F_BUFFER is set! " << "flags=0x"
        << std::hex << flags << " res=" << std::dec << res;
  });

  if (!readCallback_) {
    if (res == -ENOBUFS || res == -ECANCELED) {
      // ignore
    } else if (res <= 0) {
      // EOF?
      if (parent_) {
        parent_->readEOF();
      }
    } else if (res > 0 && lastUsedBufferProvider_) {
      // must take the buffer
      uint16_t bufId = flags >> IORING_CQE_BUFFER_SHIFT;
      appendReadData(
          lastUsedBufferProvider_->getIoBuf(bufId, res, hasMore),
          queuedReceivedData_);
      buffer_guard.dismiss();
    }
  } else {
    if (isEOF(cqe)) {
      if (parent_) {
        parent_->readEOF();
      }
      readCallback_->readEOF();
    } else if (res == -ENOBUFS) {
      if (lastUsedBufferProvider_) {
        // urgh, resubmit and let submit logic deal with the fact
        // we have no more buffers
        lastUsedBufferProvider_->enobuf();
      }
      if (parent_) {
        parent_->submitRead();
      }
    } else if (res < 0) {
      if (res == -ENOMEM && parent_ && supportsZeroCopyRx_ && useZeroCopyRx_) {
        auto* backend = parent_->backend_;
        auto* pool = backend ? backend->zcBufferPool() : nullptr;
        const auto diagnostics = pool
            ? pool->getDiagnostics()
            : IoUringZeroCopyBufferPool::Diagnostics{};
        LOG(ERROR)
            << "ZCRX recv CQE returned ENOMEM"
            << " socket=" << parent_ << " read_sqe=" << this
            << " fd=" << parent_->fd_.toFd() << " cqe_flags=" << flags
            << " backend=" << backend << " pool=" << pool
            << " backend_napi_id=" << (backend ? backend->getNapiId() : -1)
            << " event_base_attach_count=" << parent_->eventBaseAttachCount_
            << " bytes_received=" << bytesReceived_ << " zcrx_id="
            << diagnostics.zcrxId << " num_buffers=" << diagnostics.numBuffers
            << " buffer_size=" << diagnostics.bufferSize
            << " refill_ring_entries=" << diagnostics.refillRingEntries
            << " refill_ring_head=" << diagnostics.refillRingHead
            << " refill_ring_tail=" << diagnostics.refillRingTail
            << " buffers_dispensed=" << diagnostics.buffersDispensed
            << " pending_buffers=" << diagnostics.pendingBuffers;
        FOLLY_SDT(
            folly,
            folly_async_io_uring_socket_zcrx_recv_enomem,
            parent_,
            this,
            parent_->fd_.toFd(),
            flags,
            backend,
            pool,
            backend ? backend->getNapiId() : -1,
            parent_->eventBaseAttachCount_,
            bytesReceived_);
        FOLLY_SDT(
            folly,
            folly_io_uring_zcrx_pool_on_recv_enomem,
            pool,
            diagnostics.zcrxId,
            diagnostics.numBuffers,
            diagnostics.bufferSize,
            diagnostics.refillRingEntries,
            diagnostics.refillRingHead,
            diagnostics.refillRingTail,
            diagnostics.buffersDispensed,
            diagnostics.pendingBuffers);
      }
      // assume ECANCELED is not an unrecoverable error state, but we do still
      // have to propagate to the callback as they presumably called the cancel.
      auto callback = readCallback_;
      if (parent_ && res != -ECANCELED) {
        readCallback_ = nullptr;
        parent_->readError();
      }
      AsyncSocketException::AsyncSocketExceptionType err;
      std::string error;
      switch (res) {
        case -EBADF:
          err = AsyncSocketException::NOT_OPEN;
          error = "AsyncIoUringSocket: read error: EBADF";
          break;
        default:
          err = AsyncSocketException::UNKNOWN;
          error = to<std::string>(
              "AsyncIoUringSocket: read error: ",
              folly::errnoStr(-res),
              ": (",
              res,
              ")");
          break;
      }
      callback->readErr(AsyncSocketException(err, error));
      if (parent_ && res != -ECANCELED) {
        parent_->failAllWrites();
      }
    } else {
      uint64_t const cb_was = setReadCbCount_;
      bytesReceived_ += res;
      if (supportsZeroCopyRx_ && useZeroCopyRx_) {
        const io_uring_zcrx_cqe* rcqe = (io_uring_zcrx_cqe*)(cqe + 1);
        auto pool = parent_->backend_->zcBufferPool();
        auto result = pool->getIoBuf(cqe, rcqe);
        sendZeroCopyReadBuf(std::move(result.buffer), result.isScarce);
        buffer_guard.dismiss();
      } else if (lastUsedBufferProvider_) {
        auto bufId = flags >> 16;
        sendReadBuf(
            lastUsedBufferProvider_->getIoBuf(bufId, res, hasMore),
            queuedReceivedData_);

        buffer_guard.dismiss();
      } else {
        // slow path as must have run out of buffers
        // or maybe the callback does not support whole buffers
        DCHECK(tmpBuffer_);
        tmpBuffer_->append(res);
        VLOG(2) << "UseProvidedBuffers slow path completed " << res;
        sendReadBuf(std::move(tmpBuffer_), queuedReceivedData_);
      }
      // callback may have changed now, or we may not have a parent!
      if (parent_ && setReadCbCount_ == cb_was && !inFlight()) {
        parent_->submitRead(maxSize_ == (size_t)res);
      }
    }
  }
}

void AsyncIoUringSocket::ReadSqe::callbackCancelled(
    const io_uring_cqe* cqe) noexcept {
  auto res = cqe->res;
  auto flags = cqe->flags;

  VLOG(4) << "AsyncIoUringSocket::ReadSqe::callbackCancelled() this=" << this
          << " parent=" << parent_ << " cb=" << readCallback_ << " res=" << res
          << " inflight=" << inFlight() << " flags=" << flags
          << " has_buffer=" << !!(flags & IORING_CQE_F_BUFFER)
          << " bytes_received=" << bytesReceived_;
  DestructorGuard dg(this);
  if (readCallback_) {
    callback(cqe);
  }
  if (!(flags & IORING_CQE_F_MORE)) {
    if (readCallback_ && res > 0) {
      // may have more multishot
      readCallback_->readEOF();
      // only cancel from shutdown or event base detaching
    }
    destroy();
  }
}

void AsyncIoUringSocket::ReadSqe::processSubmit(
    struct io_uring_sqe* sqe) noexcept {
  VLOG(4) << "AsyncIoUringSocket::ReadSqe::processSubmit() this=" << this
          << " parent=" << parent_ << " cb=" << readCallback_;
  lastUsedBufferProvider_ = nullptr;
  CHECK(!waitingForOldEventBaseRead());
  processOldEventBaseRead();

  // read does not use registered fd, as it can be long lived and leak socket
  // files
  int fd = parent_->fd_.toFd();

  if (!readCallback_) {
    VLOG(2) << "readProcessSubmit with no callback?";
    tmpBuffer_ = IOBuf::create(2000);
    maxSize_ = tmpBuffer_->tailroom();
    ::io_uring_prep_recv(sqe, fd, tmpBuffer_->writableTail(), maxSize_, 0);
  } else {
    if (supportsZeroCopyRx_ && useZeroCopyRx_) {
      ::io_uring_prep_rw(IORING_OP_RECV_ZC, sqe, fd, nullptr, 0, 0);
      sqe->ioprio |= IORING_RECV_MULTISHOT;
      parent_->backend_->zcBufferPool()->sqePrepZc(sqe);
    } else if (readCallbackUseIoBufs()) {
      auto* bp = parent_->backend_->bufferProvider();
      if (bp->available()) {
        lastUsedBufferProvider_ = bp;
        maxSize_ = lastUsedBufferProvider_->sizePerBuffer();

        size_t used_len;
        unsigned int ioprio_flags;
        if (supportsMultishotRecv_) {
          ioprio_flags = IORING_RECV_MULTISHOT;
          used_len = 0;
        } else {
          ioprio_flags = 0;
          used_len = maxSize_;
        }

        if (parent_->backend_->useBundles()) {
          ioprio_flags |= IORING_RECVSEND_BUNDLE;
        }

        ::io_uring_prep_recv(sqe, fd, nullptr, used_len, 0);
        sqe->buf_group = lastUsedBufferProvider_->gid();
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->ioprio |= ioprio_flags;
        VLOG(9)
            << "AsyncIoUringSocket::readProcessSubmit bufferprovider multishot";
      } else {
        // todo: it's possible the callback can hint to us how much data to use.
        // naively you could use getReadBuffer, however it turns out that many
        // callbacks that support isBufferMovable do not expect the transport to
        // switch between both types of callbacks. A new API to provide a size
        // hint might be useful in the future.
        tmpBuffer_ = parent_->options_.allocateNoBufferPoolBuffer();
        maxSize_ = tmpBuffer_->tailroom();
        ::io_uring_prep_recv(sqe, fd, tmpBuffer_->writableTail(), maxSize_, 0);
      }
    } else {
      void* buf;
      readCallback_->getReadBuffer(&buf, &maxSize_);
      maxSize_ = std::min<size_t>(maxSize_, 2048);
      tmpBuffer_ = IOBuf::create(maxSize_);
      ::io_uring_prep_recv(sqe, fd, tmpBuffer_->writableTail(), maxSize_, 0);
      VLOG(9) << "AsyncIoUringSocket::readProcessSubmit  tmp buffer using size "
              << maxSize_;
    }

    VLOG(5) << "readProcessSubmit " << this << " reg=" << fd
            << " cb=" << readCallback_ << " size=" << maxSize_;
  }
}

void AsyncIoUringSocket::ReadSqe::sendZeroCopyReadBuf(
    std::unique_ptr<IOBuf> buf, bool scarce) noexcept {
  if (readCallback_) {
    readCallback_->readBuffersScarce(scarce);
  }
  // The callback may detach itself. sendReadBuf() rechecks readCallback_ and
  // queues the buffer when no callback remains.
  sendReadBuf(std::move(buf), queuedReceivedData_);
}

void AsyncIoUringSocket::ReadSqe::sendReadBuf(
    std::unique_ptr<IOBuf> buf, std::unique_ptr<IOBuf>& overflow) noexcept {
  VLOG(5) << "AsyncIoUringSocket::ReadSqe::sendReadBuf "
          << hexlify(buf->coalesce());
  while (readCallback_) {
    if (readCallback_->isBufferMovable()) [[likely]] {
      readCallback_->readBufferAvailable(std::move(buf));
      return;
    }
    auto* rcb_was = readCallback_;
    size_t sz;
    void* b;

    do {
      readCallback_->getReadBuffer(&b, &sz);
      size_t took = std::min<size_t>(sz, buf->length());
      VLOG(1) << "... inner sz=" << sz << "  len=" << buf->length();

      if (took) [[likely]] {
        memcpy(b, buf->data(), took);

        readCallback_->readDataAvailable(took);
        if (buf->length() == took) {
          buf = buf->pop();
          if (!buf) {
            return;
          }
        } else {
          buf->trimStart(took);
        }
      } else {
        VLOG(1) << "Bad!";
        // either empty buffer or the readcallback is bad.
        // assume empty buffer for simplicity
        buf = buf->pop();
        if (!buf) {
          return;
        }
      }
    } while (readCallback_ == rcb_was);
  }
  appendReadData(std::move(buf), overflow);
}

std::unique_ptr<IOBuf> AsyncIoUringSocket::ReadSqe::takePreReceivedData() {
  return std::move(preReceivedData_);
}

void AsyncIoUringSocket::ReadSqe::appendReadData(
    std::unique_ptr<IOBuf> data, std::unique_ptr<IOBuf>& overflow) noexcept {
  if (!data) {
    return;
  }

  if (overflow) {
    overflow->appendToChain(std::move(data));
  } else {
    overflow = std::move(data);
  }
}

void AsyncIoUringSocket::setPreReceivedData(std::unique_ptr<IOBuf> data) {
  readSqe_->appendPreReceive(std::move(data));
}

AsyncIoUringSocket::WriteSqe::WriteSqe(
    AsyncIoUringSocket* parent,
    WriteCallback* callback,
    std::unique_ptr<IOBuf>&& buf,
    WriteFlags flags,
    ZeroCopyOptions options)
    : IoSqeBase(IoSqeBase::Type::Write),
      parent_(parent),
      callback_(callback),
      buf_(std::move(buf)),
      flags_(flags),
      totalLength_(0),
      zcOptions_(options) {
  IOBuf const* p = buf_.get();
  do {
    if (auto l = p->length(); l > 0) {
      iov_.emplace_back();
      iov_.back().iov_base = const_cast<uint8_t*>(p->data());
      iov_.back().iov_len = l;
      totalLength_ += l;
    }
    p = p->next();
  } while (p != buf_.get());

  msg_.msg_iov = iov_.data();
  msg_.msg_iovlen = std::min<uint32_t>(iov_.size(), kIovMax);
  msg_.msg_name = nullptr;
  msg_.msg_namelen = 0;
  msg_.msg_control = nullptr;
  msg_.msg_controllen = 0;
  msg_.msg_flags = 0;

  setEventBase(parent->evb_);
}

int AsyncIoUringSocket::WriteSqe::sendMsgFlags() const {
  int msg_flags = MSG_NOSIGNAL;
  if (isSet(flags_, WriteFlags::CORK)) {
    // MSG_MORE tells the kernel we have more data to send, so wait for us to
    // give it the rest of the data rather than immediately sending a partial
    // frame, even when TCP_NODELAY is enabled.
    msg_flags |= MSG_MORE;
  }
  if (isSet(flags_, WriteFlags::EOR)) {
    // marks that this is the last byte of a record (response)
    msg_flags |= MSG_EOR;
  }
  return msg_flags;
}

void AsyncIoUringSocket::WriteSqe::processSubmit(
    struct io_uring_sqe* sqe) noexcept {
  VLOG(5) << "write sqe submit " << this << " iovs=" << msg_.msg_iovlen
          << " length=" << totalLength_ << " ptr=" << msg_.msg_iov
          << " zc=" << zcOptions_.zeroCopy << " fd = " << parent_->usedFd_
          << " flags=" << parent_->mbFixedFileFlags_;
  if (zcOptions_.zeroCopy) {
    ::io_uring_prep_sendmsg_zc(
        sqe, parent_->usedFd_, &msg_, sendMsgFlags() | MSG_WAITALL);
    sqe->ioprio |= IORING_SEND_ZC_REPORT_USAGE;
    if (zcOptions_.fixedBuf) {
      sqe->ioprio |= IORING_RECVSEND_FIXED_BUF;
      sqe->buf_index = zcOptions_.fixedBufIndex;
    }
  } else {
    ::io_uring_prep_sendmsg(sqe, parent_->usedFd_, &msg_, sendMsgFlags());
  }
  sqe->flags |= parent_->mbFixedFileFlags_;
}

namespace {

struct DetachFdState : AsyncReader::ReadCallback {
  DetachFdState(
      AsyncIoUringSocket* s, AsyncDetachFdCallback* cb, NetworkSocket fd)
      : socket(s), callback(cb), ns(fd) {}
  AsyncIoUringSocket* socket;
  AsyncDetachFdCallback* callback;
  NetworkSocket ns;
  std::unique_ptr<IOBuf> unread;
  std::unique_ptr<IOBuf> buffer;

  void done() {
    socket->setReadCB(nullptr);
    callback->fdDetached(ns, std::move(unread));
    delete this;
  }

  // ReadCallback:
  void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
    if (!buffer) {
      buffer = IOBuf::create(2000);
    }
    *bufReturn = buffer->writableTail();
    *lenReturn = buffer->tailroom();
  }

  void readErr(const AsyncSocketException&) noexcept override { done(); }
  void readEOF() noexcept override { done(); }
  void readBufferAvailable(std::unique_ptr<IOBuf> buf) noexcept override {
    if (unread) {
      unread->appendToChain(std::move(buf));
    } else {
      unread = std::move(buf);
    }
    if (!socket->readSqeInFlight()) {
      done();
    }
  }

  void readDataAvailable(size_t len) noexcept override {
    buffer->append(len);
    readBufferAvailable(std::move(buffer));
  }
  bool isBufferMovable() noexcept override { return true; }
};

struct CancelSqe : IoSqeBase {
  explicit CancelSqe(IoSqeBase* sqe, folly::Function<void()> fn = {})
      : IoSqeBase(IoSqeBase::Type::Cancel), target_(sqe), fn_(std::move(fn)) {}
  void processSubmit(struct io_uring_sqe* sqe) noexcept override {
    ::io_uring_prep_cancel(sqe, target_, 0);
  }
  void callback(const io_uring_cqe*) noexcept override {
    if (fn_) {
      fn_();
    }
    delete this;
  }

  void callbackCancelled(const io_uring_cqe*) noexcept override {
    if (fn_) {
      fn_();
    }
    delete this;
  }

  IoSqeBase* target_;
  folly::Function<void()> fn_;
};

} // namespace

void AsyncIoUringSocket::asyncDetachFd(AsyncDetachFdCallback* callback) {
  auto state = new DetachFdState(this, callback, takeFd());

  if (writeSqeActive_) {
    backend_->cancel(writeSqeActive_);
    writeSqeActive_->callback_->writeErr(
        0, AsyncSocketException(AsyncSocketException::UNKNOWN, "fd detached"));
    writeSqeActive_ = nullptr;
  }
  while (!writeSqeQueue_.empty()) {
    auto& f = writeSqeQueue_.front();
    f.callback_->writeErr(
        0, AsyncSocketException(AsyncSocketException::UNKNOWN, "fd detached"));
    backend_->cancel(&f);
    writeSqeQueue_.pop_front();
  }

  setReadCB(state);
  if (readSqe_->inFlight()) {
    backend_->submitNow(*new CancelSqe(readSqe_.get()));
  } else {
    state->done();
  }

  // todo - care about connect? probably doesnt matter as we wont have bad
  // results (eg wrong read data), just a broken socket
}

void AsyncIoUringSocket::attachEventBase(EventBase* evb) {
  VLOG(2) << "AsyncIoUringSocket::attachEventBase(this=" << this
          << ") state=" << stateAsString() << " isDetaching_=" << isDetaching_
          << " evb=" << evb;
  if (!isDetaching_) {
    throw std::runtime_error("bad state for attachEventBase");
  }
  backend_ = getBackendFromEventBase(evb);
  evb_ = evb;
  isDetaching_ = false;
  registerFd();
  ++eventBaseAttachCount_;
  FOLLY_SDT(
      folly,
      folly_async_io_uring_socket_event_base_attach,
      this,
      fd_.toFd(),
      evb_,
      backend_,
      backend_ ? backend_->zcBufferPool() : nullptr,
      backend_ ? backend_->getNapiId() : -1,
      eventBaseAttachCount_,
      readSqe_->inFlight());
  readSqe_->attachEventBase();

  if (writeSqeActive_) {
    alive_ = std::make_shared<folly::Unit>();
    std::move(*detachedWriteResult_)
        .via(evb)
        .thenValue(
            [w = writeSqeActive_, a = std::weak_ptr<folly::Unit>(alive_), evb](
                auto&& resFlagsPairs) {
              VLOG(5) << "attached write done, " << resFlagsPairs.size();
              if (!a.lock()) {
                return;
              }

              io_uring_cqe cqe;
              for (const auto& [res, flags] : resFlagsPairs) {
                cqe.res = res;
                cqe.flags = flags;

                evb->bumpHandlingTime();
                if (w->cancelled()) {
                  w->callbackCancelled(&cqe);
                } else {
                  w->callback(&cqe);
                }
              }
            });
  }

  writeTimeout_.attachEventBase(evb);
  if (state_ == State::Established) {
    allowReads();
    processWriteQueue();
  }
}

bool AsyncIoUringSocket::isDetachable() const {
  VLOG(3) << "AsyncIoUringSocket::isAsyncDetachable(" << this
          << ") state=" << stateAsString();
  if (fastOpenSqe_ && fastOpenSqe_->inFlight()) {
    VLOG(3) << "not detachable: fastopen";
    return false;
  }
  if (connectSqe_ && connectSqe_->inFlight()) {
    VLOG(3) << "not detachable: connect";
    return false;
  }
  if (closeSqe_ && closeSqe_->inFlight()) {
    VLOG(3) << "not detachable: closing";
    return false;
  }
  if (state_ == State::FastOpen) {
    VLOG(3) << "not detachable: fastopen";
    return false;
  }
  if (state_ == State::Connecting) {
    return false;
  }
  if (writeTimeout_.isScheduled()) {
    VLOG(3) << "not detachable: write timeout";
    return false;
  }
  return true;
}

namespace {

struct DetachReadCallback : AsyncReader::ReadCallback {
  explicit DetachReadCallback() { buf_ = folly::IOBuf::create(2048); }

  void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
    *bufReturn = buf_->writableTail();
    *lenReturn = buf_->tailroom();
  }

  void readDataAvailable(size_t len) noexcept override {
    buf_->append(len);
    buf_->reserve(0 /* minHeadroom */, 2048 /* minTailroom */);
  }

  void readErr(const AsyncSocketException&) noexcept override { done(); }
  void readEOF() noexcept override { done(); }
  void done() noexcept {
    VLOG(4) << "AsyncIoUringSocket::detachReadcallback() this=" << this
            << " done";
    prom.setValue(std::move(buf_));
    delete this;
  }

  folly::Promise<std::unique_ptr<folly::IOBuf>> prom;
  std::unique_ptr<folly::IOBuf> buf_;
};

} // namespace

void AsyncIoUringSocket::detachEventBase() {
  VLOG(4) << "AsyncIoUringSocket::detachEventBase() this=" << this
          << " readSqeInFlight_=" << readSqe_->inFlight()
          << " detachable=" << isDetachable();
  if (!isDetachable()) {
    throw std::runtime_error("not detachable");
  }
  if (isDetaching_) {
    return;
  }
  FOLLY_SDT(
      folly,
      folly_async_io_uring_socket_event_base_detach,
      this,
      fd_.toFd(),
      evb_,
      backend_,
      backend_ ? backend_->zcBufferPool() : nullptr,
      backend_ ? backend_->getNapiId() : -1,
      eventBaseAttachCount_,
      readSqe_->inFlight());
  isDetaching_ = true;

  if (writeSqeActive_) {
    // it's dangerous to have one sqeBase referred to by two backends, so make a
    // copy and redirect all the callbacks to the new one.
    auto det = writeSqeActive_->detachEventBase();
    writeSqeActive_ = det.second;
    detachedWriteResult_ = std::move(det.first);
  }
  writeTimeout_.detachEventBase();

  DetachReadCallback* drc = nullptr;
  auto* oldReadCallback = readSqe_->readCallback();
  folly::Optional<folly::SemiFuture<std::unique_ptr<IOBuf>>> previous;
  if (readSqe_->inFlight()) {
    drc = new DetachReadCallback();
    readSqe_->setReadCallback(drc, false);
    previous = readSqe_->detachEventBase();
    backend_->cancel(readSqe_.release());
  }
  readSqe_ = ReadSqe::UniquePtr(new ReadSqe(this));
  readSqe_->setReadCallback(oldReadCallback, false);
  readSqe_->setEventBase(nullptr);
  if (fd_ != NetworkSocket()) {
    try {
      SocketAddress remoteAddr;
      getPeerAddress(&remoteAddr);
      readSqe_->setUseZeroCopyRx(!remoteAddr.isLoopbackAddress());
    } catch (const std::exception& e) {
      VLOG(2) << "Error getting peer address in detachEventBase: " << e.what();
    }
  }

  unregisterFd();
  if (!drc) {
    if (previous) {
      VLOG(4) << "Setting promise from previous";
      readSqe_->setOldEventBaseRead(std::move(*previous));
    } else {
      VLOG(4) << "Not setting promise";
    }
  } else {
    auto res = drc->prom.getSemiFuture();
    if (previous) {
      VLOG(4) << "Setting promise from previous and this one";
      readSqe_->setOldEventBaseRead(
          std::move(*previous).deferValue(
              [r = std::move(res)](
                  std::unique_ptr<folly::IOBuf>&& prevRes) mutable {
                return std::move(r).deferValue(
                    [p = std::move(prevRes)](
                        std::unique_ptr<folly::IOBuf>&& nextRes) mutable {
                      p->appendToChain(std::move(nextRes));
                      return std::move(p);
                    });
              }));
    } else {
      VLOG(4) << "Setting promise from this one";
      readSqe_->setOldEventBaseRead(std::move(res));
    }
  }
  evb_ = nullptr;
  backend_ = nullptr;
}

bool AsyncIoUringSocket::ReadSqe::waitingForOldEventBaseRead() const {
  return oldEventBaseRead_ && !oldEventBaseRead_->isReady();
}

folly::Optional<folly::SemiFuture<std::unique_ptr<IOBuf>>>
AsyncIoUringSocket::ReadSqe::detachEventBase() {
  alive_ = nullptr;
  parent_ = nullptr;
  setEventBase(nullptr);
  return std::move(oldEventBaseRead_);
}

void AsyncIoUringSocket::ReadSqe::attachEventBase() {
  VLOG(5) << "AsyncIoUringSocket::ReadSqe::attachEventBase(this=" << this
          << ") parent_=" << parent_ << " cb_=" << readCallback_
          << " oldread=" << !!oldEventBaseRead_ << " inflight=" << inFlight();

  if (!parent_) {
    return;
  }
  if (!oldEventBaseRead_) {
    return;
  }
  auto* evb = parent_->evb_;
  setEventBase(evb);
  alive_ = std::make_shared<folly::Unit>();
  folly::Func deferred =
      [p = parent_, a = std::weak_ptr<folly::Unit>(alive_)]() {
        if (a.lock()) {
          p->previousReadDone();
        } else {
          VLOG(5) << "unable to lock for " << p;
        }
      };
  oldEventBaseRead_ =
      std::move(*oldEventBaseRead_)
          .via(evb)
          .thenValue([d = std::move(deferred), evb](auto&& x) mutable {
            evb->add(std::move(d));
            return std::move(x);
          });
}

AsyncIoUringSocket::FastOpenSqe::FastOpenSqe(
    AsyncIoUringSocket* parent,
    SocketAddress const& addr,
    std::unique_ptr<WriteSqe> i)
    : IoSqeBase(IoSqeBase::Type::Open),
      parent_(parent),
      initialWrite(std::move(i)) {
  addrLen_ = addr.getAddress(&addrStorage);
  setEventBase(parent->evb_);
}

void AsyncIoUringSocket::FastOpenSqe::cleanupMsg() noexcept {
  initialWrite->msg_.msg_name = nullptr;
  initialWrite->msg_.msg_namelen = 0;
}

void AsyncIoUringSocket::FastOpenSqe::processSubmit(
    struct io_uring_sqe* sqe) noexcept {
  VLOG(5) << "fastopen sqe submit " << this
          << " iovs=" << initialWrite->msg_.msg_iovlen
          << " length=" << initialWrite->totalLength_
          << " ptr=" << initialWrite->msg_.msg_iov;
  initialWrite->processSubmit(sqe);
  initialWrite->msg_.msg_name = &addrStorage;
  initialWrite->msg_.msg_namelen = addrLen_;
  sqe->msg_flags |= MSG_FASTOPEN;
}

void AsyncIoUringSocket::processWriteQueue() noexcept {
  if (writeSqeQueue_.empty() && !writeSqeActive_ &&
      shutdownFlags_ & ShutFlags_WritePending) {
    shutdownWriteNow();
    return;
  }
  if (state_ != State::Established && !connecting()) {
    failAllWrites();
    return;
  }
  if (writeSqeActive_ || writeSqeQueue_.empty()) {
    return;
  }
  writeSqeActive_ = &writeSqeQueue_.front();
  writeSqeQueue_.pop_front();
  doSubmitWrite();
}

void AsyncIoUringSocket::writeDone() noexcept {
  VLOG(5) << "AsyncIoUringSocket::writeDone queue=" << writeSqeQueue_.size()
          << " active=" << writeSqeActive_;

  if (writeTimeoutTime_.count() > 0) {
    writeTimeout_.cancelTimeout();
  }
  processWriteQueue();
}

void AsyncIoUringSocket::doSubmitWrite() noexcept {
  DCHECK(writeSqeActive_);
  backend_->submitSoon(*writeSqeActive_);
  if (writeTimeoutTime_.count() > 0) {
    startSendTimeout();
  }
}

void AsyncIoUringSocket::doReSubmitWrite() noexcept {
  DCHECK(writeSqeActive_);
  backend_->submitSoon(*writeSqeActive_);
  // do not update the send timeout for partial writes
}

void AsyncIoUringSocket::failAllWrites() noexcept {
  while (!writeSqeQueue_.empty()) {
    WriteSqe* w = &writeSqeQueue_.front();
    CHECK(!w->inFlight());
    writeSqeQueue_.pop_front();
    if (w->callback_) {
      w->callback_->writeErr(
          0,
          AsyncSocketException(
              AsyncSocketException::INVALID_STATE, "socket in err state"));
    }
    delete w;
  }
}

std::pair<
    folly::SemiFuture<std::vector<std::pair<int, uint32_t>>>,
    AsyncIoUringSocket::WriteSqe*>
AsyncIoUringSocket::WriteSqe::detachEventBase() {
  auto [promise, future] =
      makePromiseContract<std::vector<std::pair<int, uint32_t>>>();
  auto newSqe =
      new WriteSqe(parent_, callback_, std::move(buf_), flags_, zcOptions_);

  // make sure to keep the state of where we are in the write
  newSqe->totalLength_ = totalLength_;
  newSqe->iov_ = iov_;
  newSqe->msg_ = msg_;
  newSqe->refs_ = refs_;

  parent_ = nullptr;
  setEventBase(nullptr);
  detachedSignal_ =
      [prom = std::move(promise),
       ret = std::vector<std::pair<int, uint32_t>>{},
       refs = refs_](int res, uint32_t flags) mutable -> bool {
    ret.emplace_back(res, flags);
    VLOG(5) << "DetachedSignal, now refs=" << refs;
    if (flags & IORING_CQE_F_NOTIF) {
      --refs;
    } else if (!(flags & IORING_CQE_F_MORE)) {
      --refs;
    }
    if (refs == 0) {
      prom.setValue(std::move(ret));
      return true;
    }
    return false;
  };
  return std::make_pair(std::move(future), newSqe);
}

void AsyncIoUringSocket::WriteSqe::callbackCancelled(
    const io_uring_cqe* cqe) noexcept {
  auto flags = cqe->flags;
  VLOG(5) << "write sqe callback cancelled " << this << " flags=" << flags
          << " refs_=" << refs_ << " more=" << !!(flags & IORING_CQE_F_MORE)
          << " notif=" << !!(flags & IORING_CQE_F_NOTIF);
  if (flags & IORING_CQE_F_MORE) {
    return;
  }
  if (--refs_ <= 0) {
    delete this;
  }
}

void AsyncIoUringSocket::WriteSqe::callback(const io_uring_cqe* cqe) noexcept {
  auto res = cqe->res;
  auto flags = cqe->flags;

  VLOG(5)
      << "write sqe callback " << this << " res=" << res << " flags=" << flags
      << " iovStart=" << iov_.size() << " iovRemaining=" << iov_.size()
      << " length=" << totalLength_ << " refs_=" << refs_
      << " more=" << !!(flags & IORING_CQE_F_MORE)
      << " notif=" << !!(flags & IORING_CQE_F_NOTIF) << " parent_=" << parent_;

  if (!parent_) {
    // parent_ was detached, queue this up and signal.
    if (detachedSignal_(res, flags)) {
      VLOG(5) << "...detachedSignal done";
      delete this;
    }
    return;
  }

  if (flags & IORING_CQE_F_MORE) {
    // still expecting another ref for this
    ++refs_;
  }

  if (flags & IORING_CQE_F_NOTIF) {
    if (--refs_ == 0) {
      delete this;
    }
    return;
  }

  DestructorGuard dg(parent_);

  if (res > 0 && (size_t)res < totalLength_) {
    // todo clean out the iobuf
    size_t toRemove = res;
    parent_->bytesWritten_ += res;
    totalLength_ -= toRemove;
    size_t popFronts = 0;
    while (toRemove) {
      if (msg_.msg_iov->iov_len > toRemove) {
        msg_.msg_iov->iov_len -= toRemove;
        msg_.msg_iov->iov_base = ((char*)msg_.msg_iov->iov_base) + toRemove;
        toRemove = 0;
      } else {
        toRemove -= msg_.msg_iov->iov_len;
        if (iov_.size() > kIovMax) {
          // popping from the front of an iov is slow, so do it in a batch
          // prefer to do this rather than add a place to stash this
          // counter in WriteSqe, since this is very unlikely to actually
          // happen.
          popFronts++;
          DCHECK(iov_.size() > popFronts);
          ++msg_.msg_iov;
        } else {
          DCHECK(msg_.msg_iovlen > 1);
          ++msg_.msg_iov;
          --msg_.msg_iovlen;
        }
      }
    }

    if (popFronts > 0) {
      DCHECK(iov_.size() > popFronts);
      auto it = iov_.begin();
      std::advance(it, popFronts);
      iov_.erase(iov_.begin(), it);
      msg_.msg_iov = iov_.data();
      msg_.msg_iovlen = std::min<uint32_t>(iov_.size(), kIovMax);
    }

    // must make inflight false even if MORE is set
    prepareForReuse();

    // partial write
    parent_->doReSubmitWrite();
  } else {
    if (callback_) {
      if (res >= 0) {
        // todo
        parent_->bytesWritten_ += res;
        callback_->writeSuccess();
      } else if (res < 0) {
        VLOG(2) << "write error! " << res;
        callback_->writeErr(
            0,
            AsyncSocketException(
                AsyncSocketException::UNKNOWN,
                "AsyncIoUringSocket: write error",
                -res));
      }
    }
    if (parent_) {
      parent_->writeSqeActive_ = nullptr;
      parent_->writeDone();
    }
    if (--refs_ == 0) {
      delete this;
    }
  }
}

void AsyncIoUringSocket::failWrite(const AsyncSocketException& ex) {
  if (!writeSqeActive_) {
    return;
  }
  DestructorGuard dg(this);
  writeSqeActive_->callback_->writeErr(0, ex);
  backend_->cancel(writeSqeActive_);
  writeSqeActive_ = nullptr;
  writeDone();
}

void AsyncIoUringSocket::write(
    WriteCallback* callback, const void* buff, size_t n, WriteFlags wf) {
  // pretty sure that buff cannot change until the write completes
  writeChain(callback, IOBuf::wrapBuffer(buff, n), wf);
}

void AsyncIoUringSocket::writev(
    WriteCallback* callback, const iovec* iov, size_t n, WriteFlags wf) {
  if (n == 0) {
    callback->writeSuccess();
    return;
  }
  auto first = IOBuf::wrapBuffer(iov[0].iov_base, iov[0].iov_len);
  for (size_t i = 1; i < n; i++) {
    first->appendToChain(IOBuf::wrapBuffer(iov[i].iov_base, iov[i].iov_len));
  }
  writeChain(callback, std::move(first), wf);
}

bool AsyncIoUringSocket::canZC(std::unique_ptr<IOBuf> const& buf) const {
  if (!options_.zeroCopyEnable) {
    return false;
  }
  return (*options_.zeroCopyEnable)(buf);
}

namespace {
struct NullWriteCallback : AsyncWriter::WriteCallback {
  void writeSuccess() noexcept override {}
  void writeErr(size_t, const AsyncSocketException&) noexcept override {}

} sNullWriteCallback;

} // namespace

void AsyncIoUringSocket::writeChain(
    WriteCallback* callback, std::unique_ptr<IOBuf>&& buf, WriteFlags flags) {
  if ((state_ == State::Closed || state_ == State::Error) && !connecting()) {
    if (callback) {
      AsyncSocketException ex(
          AsyncSocketException::INVALID_STATE,
          "trying to write with socket in invalid state");
      callback->writeErr(0, ex);
    }
    return;
  }
  auto canzc = canZC(buf);
  if (!callback) {
    callback = &sNullWriteCallback;
  }
  WriteSqe::ZeroCopyOptions zcOptions{
      .zeroCopy = canzc,
      .fixedBuf = backend_->getArenaIndex() > 0,
      .fixedBufIndex = 0,
  };
  WriteSqe* w = new WriteSqe(this, callback, std::move(buf), flags, zcOptions);

  VLOG(5) << "AsyncIoUringSocket::writeChain(" << this
          << " ) state=" << stateAsString() << " size=" << w->totalLength_
          << " cb=" << callback << " fd=" << fd_ << " usedFd_ = " << usedFd_;
  if (state_ == State::FastOpen && !fastOpenSqe_) {
    fastOpenSqe_ = std::make_unique<FastOpenSqe>(
        this, peerAddress_, std::unique_ptr<WriteSqe>(w));
    backend_->submitSoon(*fastOpenSqe_);
  } else {
    writeSqeQueue_.push_back(*w);
    VLOG(5) << "enquque " << w << " as have active. queue now "
            << writeSqeQueue_.size();
    processWriteQueue();
  }
}

namespace {

class UnregisterFdSqe : public IoSqeBase {
 public:
  UnregisterFdSqe(IoUringBackend* b, IoUringFdRegistrationRecord* f)
      : backend(b), fd(f) {}

  void processSubmit(struct io_uring_sqe* sqe) noexcept override {
    ::io_uring_prep_nop(sqe);
  }

  void callback(const io_uring_cqe*) noexcept override {
    auto start = std::chrono::steady_clock::now();
    if (!backend->unregisterFd(fd)) {
      LOG(ERROR) << "Bad fd unregister";
    }
    auto end = std::chrono::steady_clock::now();
    if (end - start > std::chrono::milliseconds(1)) {
      LOG(INFO)
          << "unregistering fd took "
          << std::chrono::duration_cast<std::chrono::microseconds>(end - start)
                 .count()
          << "us";
    }
    delete this;
  }

  void callbackCancelled(const io_uring_cqe* cqe) noexcept override {
    callback(cqe);
  }

 private:
  IoUringBackend* backend;
  IoUringFdRegistrationRecord* fd;
};

} // namespace

void AsyncIoUringSocket::unregisterFd() {
  if (fdRegistered_) {
    // we have to asynchronously run this in case something wants the fd but has
    // not been submitted yet. So first do a submit and then unregister
    // we have to use an async SQE here rather than using the EventBase in case
    // something cleans up the backend before running.
    backend_->submitNextLoop(*new UnregisterFdSqe(backend_, fdRegistered_));
  }
  fdRegistered_ = nullptr;
  usedFd_ = fd_.toFd();
  mbFixedFileFlags_ = 0;
}

NetworkSocket AsyncIoUringSocket::takeFd() {
  auto ret = std::exchange(fd_, {});
  unregisterFd();
  usedFd_ = -1;
  return ret;
}

bool AsyncIoUringSocket::setZeroCopy(bool enable) {
  if (!enable) {
    options_.zeroCopyEnable.reset();
  } else if (!options_.zeroCopyEnable) {
    options_.zeroCopyEnable = AsyncWriter::ZeroCopyEnableFunc([](auto&&) {
      return true;
    });
  }
  return true;
}

bool AsyncIoUringSocket::getZeroCopy() const {
  return options_.zeroCopyEnable.hasValue();
}

void AsyncIoUringSocket::setZeroCopyEnableFunc(
    AsyncWriter::ZeroCopyEnableFunc func) {
  options_.zeroCopyEnable = std::move(func);
}

void AsyncIoUringSocket::closeProcessSubmit(struct io_uring_sqe* sqe) {
  if (fd_.toFd() >= 0) {
    ::io_uring_prep_close(sqe, fd_.toFd());
  } else {
    // already closed -> nop
    ::io_uring_prep_nop(sqe);
  }

  // the fd can be reused from this point
  takeFd();
}

void AsyncIoUringSocket::closeWithReset() {
  // copied from AsyncSocket
  // Enable SO_LINGER, with the linger timeout set to 0.
  // This will trigger a TCP reset when we close the socket.

  struct linger optLinger = {1, 0};
  if (::setsockopt(
          fd_.toFd(), SOL_SOCKET, SO_LINGER, &optLinger, sizeof(optLinger)) !=
      0) {
    VLOG(2) << "AsyncIoUringSocket::closeWithReset(): "
            << "error setting SO_LINGER on " << fd_ << ": errno=" << errno;
  }

  // Then let closeNow() take care of the rest
  closeNow();
}

void AsyncIoUringSocket::close() {
  closeNow();
}

void AsyncIoUringSocket::closeNow() {
  DestructorGuard dg(this);
  VLOG(2) << "AsyncIoUringSocket::closeNow() this=" << this << " fd_=" << fd_
          << " reg=" << fdRegistered_ << " evb_=" << evb_;
  if (fdRegistered_) {
    // we cannot trust that close will actually end the socket, as a
    // registered socket may be held onto for a while. So always do a shutdown
    // in case.
    ::shutdown(fd_.toFd(), SHUT_RDWR);
  }

  state_ = State::Closed;
  if (!evb_) {
    // not attached after detach
    fileops::close(fd_.toFd());
    // the fd can be reused from this point
    takeFd();
    return;
  }

  if (closeSqe_) {
    // todo: we should async close_direct registered fds and then not call
    // unregister on them

    // we submit and then release for 2 reasons:
    // 1: we dont want to accidentally clear the closeSqe_ without submitting
    // 2: we dont want to resubmit, which could close a random fd
    backend_->submitSoon(*closeSqe_);
    closeSqe_.release();
  }
  if (readSqe_) {
    ReadCallback* callback = readSqe_->readCallback();

    readSqe_->setReadCallback(nullptr, false);
    if (callback) {
      callback->readEOF();
    }
  }
}

void AsyncIoUringSocket::sendTimeoutExpired() {
  VLOG(5) << "AsyncIoUringSocket::sendTimeoutExpired(this=" << this
          << ") connect=" << !!connectSqe_;
  if (connectSqe_) {
    // reused the connect sqe
    return;
  }
  failWrite(
      AsyncSocketException(AsyncSocketException::TIMED_OUT, "write timed out"));
}

void AsyncIoUringSocket::startSendTimeout() {
  if (!writeTimeout_.scheduleTimeout(writeTimeoutTime_)) {
    failWrite(AsyncSocketException(
        AsyncSocketException::INTERNAL_ERROR,
        "failed to reschedule send timeout in startSendTimeout"));
  }
}

void AsyncIoUringSocket::setSendTimeout(uint32_t ms) {
  VLOG(5) << "AsyncIoUringSocket::setSendTimeout(this=" << this
          << ") ms=" << ms;
  writeTimeoutTime_ = std::chrono::milliseconds{ms};
  if (evb_) {
    evb_->dcheckIsInEventBaseThread();
  }

  if (!writeSqeActive_) {
    return;
  }
  // If we are currently pending on write requests, immediately update
  // writeTimeout_ with the new value.
  if (writeTimeoutTime_.count() > 0) {
    startSendTimeout();
  } else {
    writeTimeout_.cancelTimeout();
  }
}

void AsyncIoUringSocket::getLocalAddress(SocketAddress* address) const {
  if (!localAddress_.isInitialized()) {
    localAddress_.setFromLocalAddress(fd_);
  }
  *address = localAddress_;
}

void AsyncIoUringSocket::getPeerAddress(SocketAddress* address) const {
  if (!peerAddress_.isInitialized()) {
    peerAddress_.setFromPeerAddress(fd_);
  }
  *address = peerAddress_;
}

void AsyncIoUringSocket::cacheAddresses() {
  try {
    SocketAddress s;
    getLocalAddress(&s);
    getPeerAddress(&s);
  } catch (const std::system_error& e) {
    VLOG(2) << "Error caching addresses: " << e.code().value() << ", "
            << e.code().message();
  }
}

size_t AsyncIoUringSocket::getRawBytesReceived() const {
  return readSqe_->bytesReceived();
}

int AsyncIoUringSocket::setNoDelay(bool noDelay) {
  if (fd_ == NetworkSocket()) {
    VLOG(4) << "AsyncSocket::setNoDelay() called on non-open socket " << this;
    return EINVAL;
  }

  int value = noDelay ? 1 : 0;
  if (setSockOpt(IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)) != 0) {
    int errnoCopy = errno;
    VLOG(2) << "failed to update TCP_NODELAY option on AsyncSocket " << this
            << " (fd=" << fd_ << "): " << errnoStr(errnoCopy);
    return errnoCopy;
  }
  return 0;
}

int AsyncIoUringSocket::setSockOpt(
    int level, int optname, const void* optval, socklen_t optsize) {
  return ::setsockopt(fd_.toFd(), level, optname, optval, optsize);
}

bool AsyncIoUringSocket::getTFOSucceeded() const {
  return detail::tfo_succeeded(fd_);
}

void AsyncIoUringSocket::registerFd() {
  auto start = std::chrono::steady_clock::now();
  fdRegistered_ = backend_->registerFd(fd_.toFd());
  auto end = std::chrono::steady_clock::now();
  if (end - start > std::chrono::milliseconds(1)) {
    LOG(INFO)
        << "registering fd took "
        << std::chrono::duration_cast<std::chrono::microseconds>(end - start)
               .count()
        << "us";
  }
  if (fdRegistered_) {
    usedFd_ = fdRegistered_->idx_;
    mbFixedFileFlags_ = IOSQE_FIXED_FILE;
  } else {
    usedFd_ = fd_.toFd();
    VLOG(1) << "unable to register fd: " << fd_.toFd();
  }
}

void AsyncIoUringSocket::setFd(NetworkSocket ns) {
  fd_ = ns;
  try {
    int flags =
        ensureSocketReturnCode(fcntl(ns.toFd(), F_GETFL, 0), "get flags");
    flags = flags & ~O_NONBLOCK;
    ensureSocketReturnCode(fcntl(ns.toFd(), F_SETFL, flags), "set flags");
    registerFd();
  } catch (std::exception const& e) {
    LOG(ERROR) << "unable to setFd " << ns.toFd() << " : " << e.what();
    fileops::close(ns.toFd());
    throw;
  }
  // Only actually enable zero copy receive if the socket is not from loopback.
  // There is no 'zero copy' for loopback anyway, and issuing recvzc requests
  // for a loopback socket will always hit the inefficient copy fallback path.
  // Better to simply issue normal multishot recv.
  if (readSqe_) {
    try {
      SocketAddress remoteAddr;
      getPeerAddress(&remoteAddr);
      readSqe_->setUseZeroCopyRx(!remoteAddr.isLoopbackAddress());
    } catch (const std::exception& e) {
      VLOG(2) << "Error getting peer address in setFd: " << e.what();
    }
  }
}

void AsyncIoUringSocket::shutdownWrite() {
  if (shutdownFlags_ & ShutFlags_Write) {
    return;
  }
  if (writeSqeActive_ || !writeSqeQueue_.empty()) {
    shutdownFlags_ |= ShutFlags_WritePending;
  } else {
    shutdownWriteNow();
  }
}

void AsyncIoUringSocket::shutdownWriteNow() {
  if (shutdownFlags_ & ShutFlags_Write) {
    return;
  }
  int ret = ::shutdown(fd_.toFd(), SHUT_WR);
  if (!ret) {
    shutdownFlags_ |= ShutFlags_Write;
    shutdownFlags_ = shutdownFlags_ & ~ShutFlags_WritePending;
  }
}

} // namespace folly

#endif
