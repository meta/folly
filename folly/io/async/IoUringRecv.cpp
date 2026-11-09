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

#include <folly/io/async/IoUringRecv.h>

#include <iterator>

#include <folly/io/async/AsyncSocketException.h>
#include <folly/io/async/CmsgUtil.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventHandler.h>
#include <folly/io/async/IoUringBackend.h>

namespace folly {

#if FOLLY_HAS_LIBURING

namespace {
/*
 * DetachedReadCallback
 */

class DetachedReadCallback : public IoUringRecvCallback {
 public:
  void recvSuccess(std::unique_ptr<IOBuf> data) override {
    if (!data_.data) {
      data_.data = std::move(data);
    } else {
      data_.data->appendToChain(std::move(data));
    }
  }

  void recvSuccessWithCmsgs(
      std::unique_ptr<IOBuf> data,
      detail::IoUringRecvmsgControl control) override {
    data_.cmsgReads.push_back(
        IoUringRecvHandle::CmsgRead{std::move(data), std::move(control)});
  }

  void recvEOF() noexcept override { done(); }
  void recvErr(
      int /*err*/,
      std::unique_ptr<const AsyncSocketException> /*exception*/) noexcept
      override {
    done();
  }
  void recvBuffersScarce(bool /*scarce*/) noexcept override {}

  void done() {
    promise.setValue(std::move(data_));
    delete this;
  }

  Promise<IoUringRecvHandle::PendingReadData> promise;

 private:
  IoUringRecvHandle::PendingReadData data_;
};
} // namespace

/*
 * RecvRequest
 */

class IoUringRecvHandle::RecvRequest
    : public IoSqeBase,
      public DelayedDestruction {
 public:
  using UniquePtr = std::unique_ptr<RecvRequest, Destructor>;

  static UniquePtr create(
      IoUringBackend* backend,
      NetworkSocket fd,
      const SocketAddress& addr,
      IoUringRecvHandle* handle,
      bool useRecvmsg,
      size_t recvmsgControlLength);

  explicit RecvRequest(
      IoUringBackend* backend,
      NetworkSocket fd,
      const SocketAddress& addr,
      IoUringRecvHandle* handle)
      : RecvRequest(backend, fd, handle) {
    const bool zcRxUsable = backend->zcBufferPool() &&
        (!addr.isLoopbackAddress() || backend->options().zcRxNoDev);
    if (zcRxUsable) {
      bufferPool_ = backend->zcBufferPool();
      bufferRing_ = nullptr;
    }
    useBundles_ = backend->useBundles();
  }

  /*
   * IoSqeBase
   */
  void processSubmit(struct io_uring_sqe* sqe) noexcept override {
    fallbackBuffer_.reset();

    if (bufferPool_) {
      ::io_uring_prep_rw(IORING_OP_RECV_ZC, sqe, fd_.toFd(), nullptr, 0, 0);
      sqe->ioprio |= IORING_RECV_MULTISHOT;
      bufferPool_->sqePrepZc(sqe);
      return;
    }

    if (bufferRing_->available()) {
      prepRecvNormal(sqe);
    } else {
      prepRecvFallback(sqe);
    }
  }

  void callback(const struct io_uring_cqe* cqe) noexcept override {
    DestructorGuard dg(this);
    auto res = cqe->res;

    if (res > 0) {
      completeRecv(cqe);
      return;
    }

    if (res == -ECANCELED) {
      // ignore
      return;
    } else if (res == -ENOBUFS) {
      bufferRing_->enobuf();
      handle_->onEnobufs();
      return;
    } else if (isEOF(cqe)) {
      handle_->onRecvEOF();
    } else {
      handle_->onRecvErr(-res);
    }
  }

  void callbackCancelled(const io_uring_cqe* cqe) noexcept override {
    DestructorGuard dg(this);

    if (cqe->res > 0) {
      completeRecv(cqe);
    }

    if (!(cqe->flags & IORING_CQE_F_MORE)) {
      handle_->onRecvEOF();
      destroy();
    }
  }

 protected:
  ~RecvRequest() override = default;

  explicit RecvRequest(
      IoUringBackend* backend, NetworkSocket fd, IoUringRecvHandle* handle)
      : IoSqeBase(IoSqeBase::Type::Read),
        fd_(fd),
        handle_(handle),
        bufferRing_(
            backend->hasBufferProvider() ? backend->bufferProvider() : nullptr),
        handleGuard_(handle) {}

  virtual void prepRecvNormal(struct io_uring_sqe* sqe) {
    ::io_uring_prep_recv_multishot(sqe, fd_.toFd(), nullptr, 0, 0);
    if (useBundles_) {
      sqe->ioprio |= IORING_RECVSEND_BUNDLE;
    }
    sqe->buf_group = bufferRing_->gid();
    sqe->flags |= IOSQE_BUFFER_SELECT;
  }

  virtual void prepRecvFallback(struct io_uring_sqe* sqe) {
    const auto size = goodMallocSize(16384);
    fallbackBuffer_ = IOBuf::create(size);
    ::io_uring_prep_recv(
        sqe,
        fd_.toFd(),
        fallbackBuffer_->writableTail(),
        fallbackBuffer_->tailroom(),
        0);
  }

  virtual void completeRecv(const struct io_uring_cqe* cqe) {
    if (bufferPool_) {
      const auto* rcqe = (struct io_uring_zcrx_cqe*)(cqe + 1);
      auto result = bufferPool_->getIoBuf(cqe, rcqe);
      handle_->onBuffersScarce(result.isScarce);
      handle_->onRecvComplete(std::move(result.buffer));
      return;
    }
    handle_->onRecvComplete(getData(cqe));
  }

  NetworkSocket fd_;
  IoUringRecvHandle* handle_;
  IoUringBufferProvider* bufferRing_{nullptr};
  std::unique_ptr<IOBuf> fallbackBuffer_;

 private:
  class RecvmsgRequest;

  std::unique_ptr<IOBuf> getData(const struct io_uring_cqe* cqe) {
    if (fallbackBuffer_) {
      fallbackBuffer_->append(cqe->res);
      return std::move(fallbackBuffer_);
    }

    return bufferRing_->getIoBuf(cqe);
  }

  bool isEOF(const struct io_uring_cqe* cqe) {
    return cqe->res == 0 && (bufferPool_ ? cqe->flags == 0 : true);
  }

  DestructorGuard handleGuard_;
  bool useBundles_{false};
  IoUringZeroCopyBufferPool* bufferPool_{nullptr};
};

class IoUringRecvHandle::RecvRequest::RecvmsgRequest final
    : public RecvRequest {
 public:
  explicit RecvmsgRequest(
      IoUringBackend* backend,
      NetworkSocket fd,
      IoUringRecvHandle* handle,
      size_t controlLength)
      : RecvRequest(backend, fd, handle),
        controlLength_(
            controlLength == 0 ? kDefaultControlLength : controlLength) {
    memset(&msgh_, 0, sizeof(msgh_));
    msgh_.msg_namelen = 0;
    msgh_.msg_controllen = controlLength_;
  }

 private:
  void prepRecvNormal(struct io_uring_sqe* sqe) override {
    ::io_uring_prep_recvmsg_multishot(sqe, fd_.toFd(), &msgh_, 0);
    sqe->buf_group = bufferRing_->gid();
    sqe->flags |= IOSQE_BUFFER_SELECT;
  }

  void prepRecvFallback(struct io_uring_sqe* sqe) override {
    const auto size = goodMallocSize(16384);
    fallbackBuffer_ = IOBuf::create(size);
    fallbackIov_.iov_base = fallbackBuffer_->writableTail();
    fallbackIov_.iov_len = fallbackBuffer_->tailroom();
    fallbackControl_.assign(
        (controlLength_ + sizeof(cmsghdr) - 1) / sizeof(cmsghdr), {});
    memset(&fallbackMsgh_, 0, sizeof(fallbackMsgh_));
    fallbackMsgh_.msg_iov = &fallbackIov_;
    fallbackMsgh_.msg_iovlen = 1;
    fallbackMsgh_.msg_control = fallbackControl_.data();
    fallbackMsgh_.msg_controllen = controlLength_;
    ::io_uring_prep_recvmsg(sqe, fd_.toFd(), &fallbackMsgh_, 0);
  }

  void completeRecv(const struct io_uring_cqe* cqe) override {
    if (fallbackBuffer_) {
      deliverFallbackRecvmsg(cqe);
    } else {
      deliverRecvmsg(cqe);
    }
  }

  void deliverFallbackRecvmsg(const struct io_uring_cqe* cqe) {
    if (cqe->res < 0) {
      fallbackBuffer_.reset();
      handle_->onRecvErr(-cqe->res);
      return;
    }

    if (static_cast<size_t>(cqe->res) > fallbackBuffer_->tailroom() ||
        (fallbackMsgh_.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
      fallbackBuffer_.reset();
      handle_->onRecvErr(EBADMSG);
      return;
    }

    detail::IoUringRecvmsgControl control;
    if (!detail::parseCmsgs(
            CMSG_FIRSTHDR(&fallbackMsgh_),
            fallbackMsgh_.msg_controllen,
            fallbackMsgh_.msg_flags,
            control)) {
      fallbackBuffer_.reset();
      handle_->onRecvErr(EBADMSG);
      return;
    }

    fallbackBuffer_->append(cqe->res);
    handle_->onRecvCompleteWithCmsgs(
        std::move(fallbackBuffer_), std::move(control));
  }

  void deliverRecvmsg(const struct io_uring_cqe* cqe) {
    if (!(cqe->flags & IORING_CQE_F_BUFFER)) {
      handle_->onRecvErr(EBADMSG);
      return;
    }

    auto buf = bufferRing_->getIoBuf(cqe);
    if (!buf) {
      handle_->onRecvErr(EBADMSG);
      return;
    }

    detail::IoUringRecvmsgControl control;
    if (!detail::parseIoUringRecvmsg(
            buf, cqe->res, msgh_.msg_namelen, msgh_.msg_controllen, control)) {
      handle_->onRecvErr(EBADMSG);
      return;
    }

    handle_->onRecvCompleteWithCmsgs(std::move(buf), std::move(control));
  }

  static constexpr size_t kDefaultControlLength = CMSG_SPACE(sizeof(uint8_t));
  struct msghdr msgh_{};
  size_t controlLength_{0};
  struct iovec fallbackIov_{};
  struct msghdr fallbackMsgh_{};
  std::vector<cmsghdr> fallbackControl_;
};

IoUringRecvHandle::RecvRequest::UniquePtr
IoUringRecvHandle::RecvRequest::create(
    IoUringBackend* backend,
    NetworkSocket fd,
    const SocketAddress& addr,
    IoUringRecvHandle* handle,
    bool useRecvmsg,
    size_t recvmsgControlLength) {
  if (useRecvmsg) {
    return UniquePtr(
        new RecvmsgRequest(backend, fd, handle, recvmsgControlLength));
  }
  return UniquePtr(new RecvRequest(backend, fd, addr, handle));
}

namespace {
IoUringBackend* checkBackend(EventBase* evb, bool useRecvmsg) {
  auto* backend = dynamic_cast<IoUringBackend*>(evb->getBackend());
  if (backend && backend->canReceiveBuffer(useRecvmsg)) {
    return backend;
  }
  return nullptr;
}
} // namespace

/*
 * IoUringRecvHandle
 */

IoUringRecvHandle::UniquePtr IoUringRecvHandle::create(
    EventBase* evb,
    NetworkSocket fd,
    const SocketAddress& addr,
    IoUringRecvCallback* callback,
    bool useRecvmsg,
    size_t recvmsgControlLength) {
  auto* backend = checkBackend(evb, useRecvmsg);
  if (!backend) {
    return nullptr;
  }

  return UniquePtr(new IoUringRecvHandle(
      backend, fd, addr, callback, useRecvmsg, recvmsgControlLength));
}

IoUringRecvHandle::UniquePtr IoUringRecvHandle::clone(
    EventBase* evb,
    NetworkSocket fd,
    const SocketAddress& addr,
    IoUringRecvCallback* callback,
    UniquePtr other,
    bool useRecvmsg,
    size_t recvmsgControlLength) {
  auto* backend = checkBackend(evb, useRecvmsg);
  if (!backend) {
    return nullptr;
  }

  return UniquePtr(new IoUringRecvHandle(
      evb,
      backend,
      fd,
      addr,
      callback,
      std::move(other),
      useRecvmsg,
      recvmsgControlLength));
}

IoUringRecvHandle::IoUringRecvHandle(
    IoUringBackend* backend,
    NetworkSocket fd,
    const SocketAddress& addr,
    IoUringRecvCallback* callback,
    bool useRecvmsg,
    size_t recvmsgControlLength)
    : backend_(backend),
      recvCallback_(callback),
      request_(
          RecvRequest::create(
              backend, fd, addr, this, useRecvmsg, recvmsgControlLength)) {}

IoUringRecvHandle::IoUringRecvHandle(
    EventBase* evb,
    IoUringBackend* backend,
    NetworkSocket fd,
    const SocketAddress& addr,
    IoUringRecvCallback* callback,
    UniquePtr other,
    bool useRecvmsg,
    size_t recvmsgControlLength)
    : backend_(backend),
      recvCallback_(callback),
      request_(
          RecvRequest::create(
              backend, fd, addr, this, useRecvmsg, recvmsgControlLength)),
      queuedReceivedData_(std::move(other->queuedReceivedData_)),
      queuedCmsgReads_(std::move(other->queuedCmsgReads_)),
      queuedCmsgReadOffset_(std::exchange(other->queuedCmsgReadOffset_, 0)) {
  if (other->pendingRead_.has_value()) {
    setPendingRead(evb, std::move(*other->pendingRead_));
  }
}

bool IoUringRecvHandle::update(uint16_t eventFlags) {
  if (!readEnabled_ && eventFlags & EventHandler::READ) {
    CHECK(!readEnabled_);
    readEnabled_ = true;
  } else if (readEnabled_ && !(eventFlags & EventHandler::READ)) {
    CHECK(readEnabled_);
    readEnabled_ = false;
  }

  return true;
}

void IoUringRecvHandle::submit() {
  CHECK(readEnabled_);
  CHECK(request_);
  ensureRecvArmed();
}

void IoUringRecvHandle::drainCompletedReads() {
  if (!readEnabled_) {
    return;
  }

  DestructorGuard dg(this);
  if (queuedReceivedData_) {
    auto data = std::exchange(queuedReceivedData_, nullptr);
    recvCallback_->recvSuccess(std::move(data));
  }

  while (readEnabled_ && recvCallback_ &&
         queuedCmsgReadOffset_ < queuedCmsgReads_.size()) {
    auto read = std::move(queuedCmsgReads_[queuedCmsgReadOffset_++]);
    recvCallback_->recvSuccessWithCmsgs(
        std::move(read.data), std::move(read.control));
  }
  queuedCmsgReads_.erase(
      queuedCmsgReads_.begin(),
      queuedCmsgReads_.begin() + queuedCmsgReadOffset_);
  queuedCmsgReadOffset_ = 0;
}

void IoUringRecvHandle::detachEventBase() {
  CHECK_NE(backend_, nullptr);

  if (request_->inFlight()) {
    auto* drc = new DetachedReadCallback();
    auto thisPendingRead = drc->promise.getSemiFuture();

    if (pendingRead_) {
      pendingRead_ =
          std::move(*pendingRead_)
              .deferValue([currRead = std::move(thisPendingRead)](
                              PendingReadData&& prevData) mutable {
                return std::move(currRead).deferValue(
                    [data = std::move(prevData)](
                        PendingReadData&& currData) mutable {
                      if (!data.data) {
                        data.data = std::move(currData.data);
                      } else if (currData.data) {
                        data.data->appendToChain(std::move(currData.data));
                      }
                      data.cmsgReads.insert(
                          data.cmsgReads.end(),
                          std::make_move_iterator(currData.cmsgReads.begin()),
                          std::make_move_iterator(currData.cmsgReads.end()));
                      return std::move(data);
                    });
              });
    } else {
      pendingRead_ = std::move(thisPendingRead);
    }

    backend_->cancel(request_.release());
    recvCallback_ = drc;
  } else {
    request_.reset();
    recvCallback_ = nullptr;
  }

  backend_ = nullptr;
}

IoUringRecvHandle::PendingRead IoUringRecvHandle::takeDetachedData() {
  CHECK_EQ(backend_, nullptr);
  CHECK(!request_)
      << "detached receive handle must not own a request: "
         "detachEventBase() releases an in-flight request for cancellation "
         "or resets an idle request";

  if (queuedCmsgReadOffset_ < queuedCmsgReads_.size()) {
    return makeSemiFuture<std::unique_ptr<IOBuf>>(AsyncSocketException(
        AsyncSocketException::INVALID_STATE,
        "switching a receive with ancillary data to EventHandler mode is "
        "unsupported"));
  }
  auto queuedData = std::move(queuedReceivedData_);
  if (!pendingRead_) {
    return makeSemiFuture(std::move(queuedData));
  }

  auto pendingRead = std::move(*pendingRead_);
  pendingRead_.reset();
  return std::move(pendingRead)
      .deferValue([queuedData = std::move(queuedData)](
                      PendingReadData pendingData) mutable {
        if (!pendingData.cmsgReads.empty()) {
          throw AsyncSocketException(
              AsyncSocketException::INVALID_STATE,
              "switching a receive with ancillary data to EventHandler mode "
              "is unsupported");
        }
        if (!queuedData) {
          return std::move(pendingData.data);
        }
        if (pendingData.data) {
          queuedData->appendToChain(std::move(pendingData.data));
        }
        return std::move(queuedData);
      });
}

void IoUringRecvHandle::cancel() {
  if (request_->inFlight()) {
    request_->setEventBase(nullptr);
    backend_->cancel(request_.release());
  } else {
    request_.reset();
  }
  recvCallback_ = nullptr;
  backend_ = nullptr;
  return;
}

void IoUringRecvHandle::setPendingRead(
    EventBase* evb, PendingReadDataFuture&& prevRead) {
  pendingRead_ = std::move(prevRead).via(evb).thenValue(
      [this, evb, dg = DestructorGuard(this)](auto&& prevData) {
        if (backend_) {
          evb->add([this, dg = DestructorGuard(this)] {
            processPendingRead();
          });
        }
        return std::move(prevData);
      });
}

void IoUringRecvHandle::processPendingRead() {
  if (!pendingRead_.has_value() || !backend_) {
    return;
  }

  DestructorGuard dg(this);
  if (pendingRead_->isReady()) {
    auto data = std::move(*pendingRead_).get();
    pendingRead_.reset();

    appendQueuedData(std::move(data.data));
    queuedCmsgReads_.insert(
        queuedCmsgReads_.end(),
        std::make_move_iterator(data.cmsgReads.begin()),
        std::make_move_iterator(data.cmsgReads.end()));
    drainCompletedReads();
    ensureRecvArmed();
  }
}

void IoUringRecvHandle::appendQueuedData(std::unique_ptr<IOBuf> data) {
  if (data) {
    if (queuedReceivedData_) {
      queuedReceivedData_->appendToChain(std::move(data));
    } else {
      queuedReceivedData_ = std::move(data);
    }
  }
}

void IoUringRecvHandle::appendQueuedCmsgData(
    std::unique_ptr<IOBuf> data, detail::IoUringRecvmsgControl control) {
  queuedCmsgReads_.push_back(CmsgRead{std::move(data), std::move(control)});
}

void IoUringRecvHandle::drainCompletedReadsAndRearm() {
  drainCompletedReads();
  ensureRecvArmed();
}

void IoUringRecvHandle::ensureRecvArmed() {
  if (!backend_) {
    return;
  }
  CHECK(request_);

  if (!readEnabled_ || pendingRead_ || request_->inFlight()) {
    return;
  }

  backend_->submitSoon(*request_);
}

void IoUringRecvHandle::onBuffersScarce(bool scarce) noexcept {
  DestructorGuard dg(this);
  if (backend_ && recvCallback_) {
    recvCallback_->recvBuffersScarce(scarce);
  }
}

void IoUringRecvHandle::onRecvComplete(std::unique_ptr<IOBuf> data) {
  DestructorGuard dg(this);
  if (backend_ == nullptr) {
    CHECK(!request_);
    if (recvCallback_) {
      // DetachedReadCallback
      recvCallback_->recvSuccess(std::move(data));
    }
    return;
  }

  appendQueuedData(std::move(data));
  drainCompletedReadsAndRearm();
}

void IoUringRecvHandle::onRecvCompleteWithCmsgs(
    std::unique_ptr<IOBuf> data, detail::IoUringRecvmsgControl control) {
  DestructorGuard dg(this);
  if (backend_ == nullptr) {
    if (recvCallback_) {
      // DetachedReadCallback preserves the cmsgs until the handle is cloned.
      recvCallback_->recvSuccessWithCmsgs(std::move(data), std::move(control));
    }
    return;
  }

  appendQueuedCmsgData(std::move(data), std::move(control));
  drainCompletedReadsAndRearm();
}

void IoUringRecvHandle::onEnobufs() {
  CHECK(!request_->inFlight());
  ensureRecvArmed();
}

void IoUringRecvHandle::onRecvEOF() {
  DestructorGuard dg(this);
  if (recvCallback_) {
    recvCallback_->recvEOF();
  }
}

void IoUringRecvHandle::onRecvErr(int err) {
  DestructorGuard dg(this);
  if (recvCallback_) {
    recvCallback_->recvErr(err, nullptr);
  }
}

#else

class IoUringRecvHandle::RecvRequest : public DelayedDestruction {};

IoUringRecvHandle::UniquePtr IoUringRecvHandle::create(
    EventBase* /*evb*/,
    NetworkSocket /*fd*/,
    const SocketAddress& /*addr*/,
    IoUringRecvCallback* /*callback*/,
    bool /*useRecvmsg*/,
    size_t /*recvmsgControlLength*/) {
  folly::terminate_with<std::runtime_error>("io_uring not supported");
}

IoUringRecvHandle::UniquePtr IoUringRecvHandle::clone(
    EventBase* /*evb*/,
    NetworkSocket /*fd*/,
    const SocketAddress& /*addr*/,
    IoUringRecvCallback* /*callback*/,
    IoUringRecvHandle::UniquePtr /*old*/,
    bool /*useRecvmsg*/,
    size_t /*recvmsgControlLength*/) {
  folly::terminate_with<std::runtime_error>("io_uring not supported");
}

IoUringRecvHandle::IoUringRecvHandle(
    IoUringBackend* /*backend*/,
    NetworkSocket /*fd*/,
    const SocketAddress& /*addr*/,
    IoUringRecvCallback* /*callback*/,
    bool /*useRecvmsg*/,
    size_t /*recvmsgControlLength*/) {
  (void)backend_;
  (void)recvCallback_;
  (void)request_;
  (void)queuedReceivedData_;
  (void)readEnabled_;
  (void)pendingRead_;
}

IoUringRecvHandle::IoUringRecvHandle(
    EventBase* /*evb*/,
    IoUringBackend* /*backend*/,
    NetworkSocket /*fd*/,
    const SocketAddress& /*addr*/,
    IoUringRecvCallback* /*callback*/,
    UniquePtr /*other*/,
    bool /*useRecvmsg*/,
    size_t /*recvmsgControlLength*/) {
  (void)backend_;
  (void)recvCallback_;
  (void)request_;
  (void)queuedReceivedData_;
  (void)readEnabled_;
  (void)pendingRead_;
}

bool IoUringRecvHandle::update(uint16_t /*eventFlags*/) {
  folly::terminate_with<std::runtime_error>("io_uring not supported");
}

void IoUringRecvHandle::submit() {
  folly::terminate_with<std::runtime_error>("io_uring not supported");
}

void IoUringRecvHandle::drainCompletedReads() {
  folly::terminate_with<std::runtime_error>("io_uring not supported");
}

void IoUringRecvHandle::detachEventBase() {
  folly::terminate_with<std::runtime_error>("io_uring not supported");
}

IoUringRecvHandle::PendingRead IoUringRecvHandle::takeDetachedData() {
  folly::terminate_with<std::runtime_error>("io_uring not supported");
}

void IoUringRecvHandle::cancel() {
  folly::terminate_with<std::runtime_error>("io_uring not supported");
}

void IoUringRecvHandle::setPendingRead(
    EventBase* /*evb*/, PendingReadDataFuture&& /*future*/) {
  folly::terminate_with<std::runtime_error>("io_uring not supported");
}

void IoUringRecvHandle::processPendingRead() {
  folly::terminate_with<std::runtime_error>("io_uring not supported");
}

void IoUringRecvHandle::appendQueuedData(std::unique_ptr<IOBuf> /*data*/) {
  folly::terminate_with<std::runtime_error>("io_uring not supported");
}

void IoUringRecvHandle::ensureRecvArmed() {
  folly::terminate_with<std::runtime_error>("io_uring not supported");
}

void IoUringRecvHandle::onBuffersScarce(bool /*scarce*/) noexcept {
  folly::terminate_with<std::runtime_error>("io_uring not supported");
}

void IoUringRecvHandle::onRecvComplete(std::unique_ptr<IOBuf> /*data*/) {
  folly::terminate_with<std::runtime_error>("io_uring not supported");
}

void IoUringRecvHandle::onRecvCompleteWithCmsgs(
    std::unique_ptr<IOBuf> /*data*/,
    detail::IoUringRecvmsgControl /*control*/) {
  folly::terminate_with<std::runtime_error>("io_uring not supported");
}

void IoUringRecvHandle::onEnobufs() {
  folly::terminate_with<std::runtime_error>("io_uring not supported");
}

void IoUringRecvHandle::onRecvEOF() {
  folly::terminate_with<std::runtime_error>("io_uring not supported");
}

void IoUringRecvHandle::onRecvErr(int /*err*/) {
  folly::terminate_with<std::runtime_error>("io_uring not supported");
}

#endif

} // namespace folly
