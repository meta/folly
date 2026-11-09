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

#include <cstddef>
#include <memory>
#include <vector>

#include <folly/SocketAddress.h>
#include <folly/futures/Future.h>
#include <folly/io/async/AsyncTransport.h>
#include <folly/io/async/CmsgUtil.h>
#include <folly/io/async/DelayedDestruction.h>
#include <folly/io/async/IoUringBase.h>
#include <folly/net/NetworkSocket.h>

namespace folly {

class EventBase;
class IoUringBackend;

class IoUringRecvCallback {
 public:
  virtual ~IoUringRecvCallback() = default;

  virtual void recvSuccess(std::unique_ptr<IOBuf> data) = 0;

  virtual void recvSuccessWithCmsgs(
      std::unique_ptr<IOBuf> data,
      // NOLINTNEXTLINE(performance-unnecessary-value-param)
      detail::IoUringRecvmsgControl /*control*/) {
    recvSuccess(std::move(data));
  }

  virtual void recvEOF() noexcept = 0;
  virtual void recvErr(
      int err,
      std::unique_ptr<const AsyncSocketException> exception) noexcept = 0;
  virtual void recvBuffersScarce(bool scarce) noexcept = 0;
};

class IoUringRecvHandle : public DelayedDestruction {
 public:
  struct CmsgRead {
    std::unique_ptr<IOBuf> data;
    detail::IoUringRecvmsgControl control;
  };

  struct PendingReadData {
    std::unique_ptr<IOBuf> data;
    std::vector<CmsgRead> cmsgReads;
  };

  using UniquePtr = std::unique_ptr<IoUringRecvHandle, Destructor>;
  using PendingRead = SemiFuture<std::unique_ptr<IOBuf>>;

  static UniquePtr create(
      EventBase* evb,
      NetworkSocket fd,
      const SocketAddress& addr,
      IoUringRecvCallback* callback,
      bool useRecvmsg = false,
      size_t recvmsgControlLength = 0);

  static UniquePtr clone(
      EventBase* evb,
      NetworkSocket fd,
      const SocketAddress& addr,
      IoUringRecvCallback* callback,
      UniquePtr old,
      bool useRecvmsg = false,
      size_t recvmsgControlLength = 0);

  bool update(uint16_t eventFlags);
  void submit();
  void drainCompletedReads();

  void detachEventBase();
  PendingRead takeDetachedData();
  void cancel();

 private:
  using PendingReadDataFuture = SemiFuture<PendingReadData>;
  using PendingReadDataFutureMaybe = Optional<PendingReadDataFuture>;

  explicit IoUringRecvHandle(
      IoUringBackend* backend,
      NetworkSocket fd,
      const SocketAddress& addr,
      IoUringRecvCallback* callback,
      bool useRecvmsg,
      size_t recvmsgControlLength);

  explicit IoUringRecvHandle(
      EventBase* evb,
      IoUringBackend* backend,
      NetworkSocket fd,
      const SocketAddress& addr,
      IoUringRecvCallback* callback,
      UniquePtr other,
      bool useRecvmsg,
      size_t recvmsgControlLength);

  void setPendingRead(EventBase* evb, PendingReadDataFuture&& future);
  void processPendingRead();
  void appendQueuedData(std::unique_ptr<IOBuf> data);
  void appendQueuedCmsgData(
      std::unique_ptr<IOBuf> data, detail::IoUringRecvmsgControl control);
  void drainCompletedReadsAndRearm();
  void ensureRecvArmed();

  class RecvRequest;

  void onRecvComplete(std::unique_ptr<IOBuf> data);
  void onRecvCompleteWithCmsgs(
      std::unique_ptr<IOBuf> data, detail::IoUringRecvmsgControl control);
  void onEnobufs();
  void onRecvEOF();
  void onRecvErr(int err);
  void onBuffersScarce(bool scarce) noexcept;

  IoUringBackend* backend_;
  IoUringRecvCallback* recvCallback_;
  std::unique_ptr<RecvRequest, DelayedDestruction::Destructor> request_;

  std::unique_ptr<IOBuf> queuedReceivedData_;
  std::vector<CmsgRead> queuedCmsgReads_;
  size_t queuedCmsgReadOffset_{0};
  bool readEnabled_{false};
  PendingReadDataFutureMaybe pendingRead_{folly::none};
};

} // namespace folly
