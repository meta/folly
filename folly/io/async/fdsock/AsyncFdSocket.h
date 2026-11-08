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

#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/fdsock/SocketFds.h>
#include <folly/portability/GTestProd.h>

#ifdef __APPLE__
#include <AvailabilityMacros.h>
#if MAC_OS_X_VERSION_MIN_REQUIRED < 110000
#ifdef __DARWIN_ALIGN32
#undef __DARWIN_ALIGN32
#define __DARWIN_ALIGN32(p) ((__darwin_size_t)((__darwin_size_t)(p) + __DARWIN_ALIGNBYTES32) &~ __DARWIN_ALIGNBYTES32)
#endif
#endif
#endif

namespace folly {

/**
 * Intended for use with Unix sockets. Unlike regular `AsyncSocket`:
 *  - Can send FDs via `writeChainWithFds` using socket ancillary data (see
 *    `man cmsg`).
 *  - Whenever handling regular reads, concurrently attempts to receive FDs
 *    included in incoming ancillary data.  Groups of received FDs are
 *    enqueued to be retrieved via `popNextReceivedFds`.
 *  - The "read ancillary data" and "sendmsg params" callbacks are built-in
 *    are NOT customizable.
 *
 * Implementation limitation: Unlike regular `AsyncSocket`, this currently
 * does not automatically send socket timestamping events.  This could
 * easily be added, but seems of limited utility for Unix sockets.
 */
class AsyncFdSocket : public AsyncSocket {
 public:
  using UniquePtr = std::unique_ptr<AsyncSocket, Destructor>;

  /**
   * Create a new unconnected AsyncSocket.
   *
   * connect() must later be called on this socket to establish a connection.
   */
  explicit AsyncFdSocket(EventBase* evb);

  /**
   * Create a new AsyncSocket and begin the connection process.
   *
   * Unlike `AsyncSocket`, lacks `useZeroCopy` since Unix sockets do not
   * support zero-copy.
   */
  AsyncFdSocket(
      EventBase* evb,
      const folly::SocketAddress& address,
      uint32_t connectTimeout = 0);

  /**
   * Create a AsyncSocket from an already connected socket file descriptor.
   *
   * Unlike `AsyncSocket`, lacks `zeroCopyBufId` since Unix sockets do not
   * support zero-copy.
   */
  AsyncFdSocket(
      EventBase* evb,
      NetworkSocket fd,
      const folly::SocketAddress* peerAddress = nullptr);

  static UniquePtr newSocket(EventBase* evb) {
    return UniquePtr{new AsyncFdSocket(evb)};
  }

  static UniquePtr newSocket(
      EventBase* evb,
      const folly::SocketAddress& address,
      uint32_t connectTimeout = 0) {
    return UniquePtr{new AsyncFdSocket(evb, address, connectTimeout)};
  }

  static UniquePtr newSocket(
      EventBase* evb,
      NetworkSocket fd,
      const folly::SocketAddress* peerAddress = nullptr) {
    return UniquePtr{new AsyncFdSocket(evb, fd, peerAddress)};
  }

  /**
   * EXPERIMENTAL / TEMPORARY: These move-like constructors should not be
   * used to go from one AsyncFdSocket to another because this will not
   * correctly preserve read & write state.  Full move is not implemented
   * since its trickier, and was not yet needed -- see `swapFdReadStateWith`.
   */
  struct DoesNotMoveFdSocketState {};

 protected:
  FOLLY_GTEST_FRIEND_TEST(AsyncFdSocketSequenceRoundtripTest, WithDataSize);
  // Protected since it's easy to accidentally pass an `AsyncFdSocket` here,
  // a scenario that's extremely easy to use incorrectly.
  AsyncFdSocket(DoesNotMoveFdSocketState, AsyncSocket*);

 public:
  AsyncFdSocket(DoesNotMoveFdSocketState, AsyncSocket::UniquePtr);

  /**
   * `AsyncSocket::writeChain` analog that passes FDs as ancillary data over
   * the socket (see `man cmsg`).
   *
   * Before writing the FDs, call `injectSocketSeqNumIntoFdsToSend()` on the
   * supplied `SocketFds` to commit them to be sent to this socket, in this
   * order.  That is -- for a given socket, the order of calling "inject" on
   * FDs must exactly match the socket write order.
   *
   * Invariants:
   *  - Max FDs per IOBuf: `SCM_MAX_FD` from include/net/scm.h, 253 for
   *    effectively all of Linux history.
   *  - FDs are received no earlier than the first data byte of the IOBuf,
   *    and no later than the last data byte. More specifically:
   *     - The currently implemented behavior is that FDs arrive precisely
   *       with the first data byte.  This was efficient and good enough for
   *       Thrift Rocket transport, where each message knows whether it
   *       expects FDs, and FDs are sent with the last data fragment of each
   *       message.
   *     - In other conceivable designs, it could be useful to pass FDs
   *       with the last data byte instead, which could be implemented as an
   *       optional write flag. In Thrift Rocket, this would minimize the
   *       buffering of FDs by the receiver, at the cost of more syscalls.
   */
  virtual void writeChainWithFds(
      WriteCallback*,
      std::unique_ptr<folly::IOBuf>,
      SocketFds,
      WriteFlags flags = WriteFlags::NONE);

  /**
   * This socket will look for file descriptors in the ancillary data (`man
   * cmsg`) of each incoming read.
   *   - FDs are kept in an internal queue, to be retrieved via this call.
   *   - FDs are marked with FD_CLOEXEC upon receipt, although on non-Linux
   *     platforms there is a short race window before this happens.
   *   - Empty lists of FDs (0 FDs with this message) are not stored in the
   *     queue.
   *   - Returns an empty `SocketFds` if the queue is empty.
   *
   * IMPORTANT: The returned `SocketFds` will have a populated
   * `getFdSocketSeqNum` matching the sending `AsyncFdSocket`.  The code
   * that consumes these FDs should check that the corresponding "data
   * message" includes the same sequence number, as a safeguard against code
   * bugs that cause messages to be paired with the wrong FDs.
   */
  SocketFds popNextReceivedFds();

  /**
   * Must be called on each `fds` before `writeChainWithFds`.  Embeds the
   * socket-internal sequence number in fds, and increments the counter.
   *
   * Return: Non-negative, but may be `SocketFds::kNoSeqNum` on DFATAL errors.
   */
  SocketFds::SeqNum injectSocketSeqNumIntoFdsToSend(SocketFds* fds);

  void setSendMsgParamCB(SendMsgParamsCallback*) override {
    LOG(DFATAL) << "AsyncFdSocket::setSendMsgParamCB is forbidden";
  }
  void setReadAncillaryDataCB(ReadAncillaryDataCallback*) override {
    LOG(DFATAL) << "AsyncFdSocket::setReadAncillaryDataCB is forbidden";
  }

// This class has no ancillary data callbacks on Windows, they wouldn't compile
#if !defined(_WIN32)
  /**
   * EXPERIMENTAL / TEMPORARY: This just does what is required for
   * `moveToPlaintext` to support StopTLS.  That use-case could later be
   * covered by full move-construct or move-assign support, but both would
   * be more complex to support.
   *
   * Swaps "read FDs" state (receive queue & sequence numbers) with `other`.
   * DFATALs if `other` had any "write FDs" state.
   */
  void swapFdReadStateWith(AsyncFdSocket* other);

 protected:
  void releaseIOBuf(
      std::unique_ptr<folly::IOBuf>, ReleaseIOBufCallback*) override;

 private:
  class FdSendMsgParamsCallback : public SendMsgParamsCallback {
   public:
    void getAncillaryData(
        folly::WriteFlags,
        void* data,
        const WriteRequestTag&,
        const bool byteEventsEnabled) noexcept override;

    uint32_t getAncillaryDataSize(
        folly::WriteFlags,
        const WriteRequestTag&,
        const bool byteEventsEnabled) noexcept override;

    void wroteBytes(const WriteRequestTag&) noexcept override;

   protected:
    friend class AsyncFdSocket;

    // Returns true on success, false if the tag was already registered, or
    // has a null IOBuf* (both are usage error).  The FDs are discarded on
    // error.
    bool registerFdsForWriteTag(WriteRequestTag, SocketFds::ToSend&&);

    // Called from `releaseIOBuf()` above, once we're sure that an IOBuf
    // will not be used for a write any more.  This is a good time to
    // discard the FDs associated with this write.
    //
    // CAREFUL: This may be invoked for IOBufs that never had a
    // corresponding `getAncillaryData*` call.
    void destroyFdsForWriteTag(WriteRequestTag) noexcept;

   private:
    using WriteTagToFds =
        std::unordered_map<WriteRequestTag, SocketFds::ToSend>;

    std::pair<size_t, WriteTagToFds::iterator> getCmsgSizeAndFds(
        const WriteRequestTag& writeTag) noexcept;

    WriteTagToFds writeTagToFds_;
  };

  class FdReadAncillaryDataCallback : public ReadAncillaryDataCallback {
   public:
    explicit FdReadAncillaryDataCallback(AsyncFdSocket* socket)
        : socket_(socket) {}

    folly::Expected<folly::Unit, AsyncSocketException> ancillaryData(
        struct ::msghdr& msg) noexcept override {
      socket_->enqueueFdsFromAncillaryData(msg);
      return folly::unit;
    }

    folly::MutableByteRange getAncillaryDataCtrlBuffer() noexcept override {
      // Not checking thread because `ancillaryData()` will follow.
      return folly::MutableByteRange(ancillaryDataCtrlBuffer_);
    }

   private:
    // Max number of fds in a single `sendmsg` / `recvmsg` message
    // Defined as SCM_MAX_FD in linux/include/net/scm.h
    static constexpr size_t kMaxFdsPerSocketMsg{253};

    AsyncFdSocket* socket_;
    std::array<uint8_t, CMSG_SPACE(sizeof(int) * kMaxFdsPerSocketMsg)>
        ancillaryDataCtrlBuffer_;
  };

  friend class FdReadAncillaryDataCallback;

  void enqueueFdsFromAncillaryData(struct ::msghdr& msg) noexcept;

  void setUpCallbacks() noexcept;

  FdSendMsgParamsCallback sendMsgCob_;
  std::queue<SocketFds> fdsQueue_; // must outlive readAncillaryDataCob_
  FdReadAncillaryDataCallback readAncillaryDataCob_;

  SocketFds::SeqNum allocatedToSendFdsSeqNum_{0};
  SocketFds::SeqNum sentFdsSeqNum_{0};
  SocketFds::SeqNum receivedFdsSeqNum_{0};
#endif // !Windows
};

} // namespace folly
