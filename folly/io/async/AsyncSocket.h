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

#include <sys/types.h>

#include <chrono>
#include <map>
#include <memory>
#include <variant>

#include <folly/ConstructorCallbackList.h>
#include <folly/Optional.h>
#include <folly/SocketAddress.h>
#include <folly/detail/SocketFastOpen.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufIovecBuilder.h>
#include <folly/io/ShutdownSocketSet.h>
#include <folly/io/SocketOptionMap.h>
#include <folly/io/async/AsyncSocketException.h>
#include <folly/io/async/AsyncSocketTransport.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/AsyncTransport.h>
#include <folly/io/async/DelayedDestruction.h>
#include <folly/io/async/EventHandler.h>
#include <folly/io/async/IoUringConnect.h>
#include <folly/io/async/IoUringRecv.h>
#include <folly/io/async/IoUringSend.h>
#include <folly/io/async/WriteCallbackWithState.h>
#include <folly/io/async/observer/AsyncSocketObserverContainer.h>
#include <folly/net/NetOpsDispatcher.h>
#include <folly/net/TcpInfo.h>
#include <folly/net/TcpInfoDispatcher.h>
#include <folly/portability/Sockets.h>
#include <folly/small_vector.h>

namespace folly {

class AsyncDetachFdCallback {
 public:
  virtual ~AsyncDetachFdCallback() = default;
  virtual void fdDetached(
      NetworkSocket ns, std::unique_ptr<IOBuf> unread) noexcept = 0;
  virtual void fdDetachFail(const AsyncSocketException& ex) noexcept = 0;
};

/**
 * A class for performing asynchronous I/O on a socket.
 *
 * AsyncSocket allows users to asynchronously wait for data on a socket, and
 * to asynchronously send data.
 *
 * The APIs for reading and writing are intentionally asymmetric.  Waiting for
 * data to read is a persistent API: a callback is installed, and is notified
 * whenever new data is available.  It continues to be notified of new events
 * until it is uninstalled.
 *
 * AsyncSocket does not provide read timeout functionality, because it
 * typically cannot determine when the timeout should be active.  Generally, a
 * timeout should only be enabled when processing is blocked waiting on data
 * from the remote endpoint.  For server sockets, the timeout should not be
 * active if the server is currently processing one or more outstanding
 * requests for this socket.  For client sockets, the timeout should not be
 * active if there are no requests pending on the socket.  Additionally, if a
 * client has multiple pending requests, it will usually want a separate
 * timeout for each request, rather than a single read timeout.
 *
 * The write API is fairly intuitive: a user can request to send a block of
 * data, and a callback will be informed once the entire block has been
 * transferred to the kernel, or on error.  AsyncSocket does provide a send
 * timeout, since most callers want to give up if the remote end stops
 * responding and no further progress can be made sending the data.
 */

#if defined __linux__ && !defined FOLLY_SO_TTLS_TRUSTED
#define FOLLY_SO_TTLS_TRUSTED 206
#endif

#if defined __linux__ && !defined FOLLY_SO_TTLS_TRUSTED_VAL_ENCRYPTED
#define FOLLY_SO_TTLS_TRUSTED_VAL_ENCRYPTED 1
#endif

#if defined __linux__ && !defined SO_NO_TSOCKS
#define SO_NO_TSOCKS 201
#endif

#if defined(FOLLY_HAVE_SO_TIMESTAMPING) && FOLLY_HAVE_SO_TIMESTAMPING
#define SO_MAX_ATTEMPTS_ENABLE_BYTEEVENTS 10
#endif

class AsyncSocket
    : public AsyncSocketTransport,
      public IoUringSendCallback,
      public IoUringRecvCallback,
      public IoUringConnectCallback {
 public:
  using UniquePtr = std::unique_ptr<AsyncSocket, Destructor>;
  using ByteEvent = AsyncSocketObserverInterface::ByteEvent;
  using Observer = AsyncSocketObserverContainer::Observer;
  using ManagedObserver = AsyncSocketObserverContainer::ManagedObserver;

  static inline constexpr size_t kMaxAttemptsEnableByteEvents = 10;

  class EvbChangeCallback {
   public:
    virtual ~EvbChangeCallback() = default;

    // Called when the socket has been attached to a new EVB
    // and is called from within that EVB thread
    virtual void evbAttached(AsyncSocket* socket) = 0;

    // Called when the socket is detached from an EVB and
    // is called from the EVB thread being detached
    virtual void evbDetached(AsyncSocket* socket) = 0;
  };

  /**
   * This interface is implemented only for platforms supporting
   * per-socket error queues.
   */
  class ErrMessageCallback {
   public:
    virtual ~ErrMessageCallback() = default;

    /**
     * errMessage() will be invoked when kernel puts a message to
     * the error queue associated with the socket.
     *
     * @param cmsg      Reference to cmsghdr structure describing
     *                  a message read from error queue associated
     *                  with the socket.
     */
    virtual void errMessage(const cmsghdr& cmsg) noexcept = 0;

    /**
     * errMessageError() will be invoked if an error occurs reading a message
     * from the socket error stream.
     *
     * @param ex        An exception describing the error that occurred.
     */
    virtual void errMessageError(const AsyncSocketException& ex) noexcept = 0;
  };

  class ReadAncillaryDataCallback {
   public:
    virtual ~ReadAncillaryDataCallback() = default;

    /**
     * `ancillaryData()` is invoked immediately before the corresponding
     * `ReadCallback::readDataAvailable()`, as a pair.
     *
     * You must check for `msg_flags | MSG_CTRUNC`, indicating that some
     * ancillary data was discarded due to lack of space.  This is normally
     * not recoverable, so you can `close` or `failRead` the socket -- see
     * below.
     *
     * ## Allowed socket mutations ###
     *
     * This callback is allowed to `close`, `failRead` (for child classes),
     * or destruct the underlying socket.  It is **NOT** allowed to perform
     * any other mutations, such as `setReadCallback` or `attachEventBase`.
     *
     * If `ancillaryData()` closes or fails the socket, then any data
     * received in the same read as the ancillary data will NOT be delivered
     * to the `ReadCallback`.
     *
     * # Detailed contract
     *
     * This will only be invoked when a `ReadCallback` is installed -- i.e.
     * the socket is connected, neither closed nor in an error state.
     *
     * The supplied buffer will have originated from the most recent call to
     * `getAncillaryDataCtrlBuffer()`.
     *
     * Per POSIX, ancillary data are sent / received with the first byte of
     * the `sendmsg` data buffer, so we guarantee that the subsequent
     * `readDataAvailable()` (if it happens) will include that data byte.
     *
     * @param  Can be used with macros from `man cmsg` to access ancillary
     *         data. It is permissible to check `msg_flags & MSG_EOR`.
     *         There is NO CONTRACT about any other `msghdr` fields -- that
     *         is, choosing to read `msg_name*` or `msg_iov*` leads to
     *         undefined behavior.
     */
    virtual folly::Expected<folly::Unit, AsyncSocketException> ancillaryData(
        struct ::msghdr&) noexcept = 0;

    /**
     * Must return a buffer large enough to contain the incoming ancillary
     * data, see `man cmsg` and `CMSG_SPACE`.
     *
     * DANGER: This call must not mutate the socket state.  e.g., you
     * cannot call setReadCB(), setReadAncillaryDataCB() or close()
     * from inside this call.
     *
     * If the supplied buffer is too small, your `ancillaryData()` will see
     * `MSG_CTRUNC`, and the kernel will have discarded some ancillary data.
     *
     * It is possible that `getAncillaryDataCtrlBuffer()` will be called
     * without a corresponding `ancillaryData()` call.  It is the callback's
     * responsibility not to leak the buffers it returns.  Any call to
     * `ancillaryData()` will use the most recently returned buffer.
     *
     * The returned buffer must remain valid until the point where
     * `ancillaryData()` could be called with it.  That is, previously
     * returned buffers may be freed if:
     *  - the socket is closed / fails, or its `ReadCallback` is removed
     *  - the `ReadAncillaryDataCallback` is uninstalled
     *  - `ancillaryData()` completes
     */
    virtual folly::MutableByteRange getAncillaryDataCtrlBuffer() = 0;
  };

  /**
   * Sometimes `SendMsgParamsCallback` needs to send different ancillary
   * data for different writes, for example when sending FDs over Unix
   * sockets.
   *
   * This opaque type acts as the key to match `writeChain` calls with
   * `getAncillaryData()` and corresponding `wroteBytes()` calls.  It wraps
   * `IOBuf*`, and implements equality, hashing, and ostream writes for
   * debugging.
   *
   * Important usage notes:
   *   - Even though `WriteRequestTag` never dereferences the pointer, it is
   *     still INCORRECT to use it after the write is over, whether or not
   *     the `IOBuf` had been destructed, because the same pointer could now
   *     refer to new, different data that is being written (see
   *     `getReleaseIOBufCallback` for the mechanism).
   *   - Therefore, if you store a `WriteRequestTag`, you must remove it
   *     whenever a write is complete.  This can be done either in
   *     `WriteCallback::{writeErr,writeSuccess}`, or by inheriting from
   *     `AsyncSocket::releaseIOBuf`, or by adding a new method
   *     `SendMsgParamsCallback::onReleaseIOBuf`.
   *   - Not all child classes support write tagging.  Notably, we removed
   *     the `AsyncSSLSocket` implementation since it added complexity and
   *     was not used.  Breadcrumbs are in `bioWrite`, or rev hash
   *     95df2ce7c98a.
   *   - The `EmptyDummy` constructor is for tests, or marking empty tags.
   *     `SendMsgParamsCallback` methods can also be called with an empty
   *     tag if the write is not submitted via `writeChain`.
   */
  struct WriteRequestTag {
    struct EmptyDummy {};

    explicit WriteRequestTag(EmptyDummy) : buf_(nullptr) {}
    explicit WriteRequestTag(folly::IOBuf* buf) : buf_(buf) {}

    bool operator==(const WriteRequestTag& other) const {
      // Remember to also update std::hash<folly::AsyncSocket::WriteRequestTag>
      // and ostream operator<<
      return buf_ == other.buf_;
    }

    [[nodiscard]] bool empty() const noexcept { return buf_ == nullptr; }

   private:
    friend struct std::hash<WriteRequestTag>;
    friend std::ostream& operator<<(std::ostream&, const WriteRequestTag&);

    // The `IOBuf` submitted through `writeChain`
    const folly::IOBuf* buf_;
  };

  class SendMsgParamsCallback {
   public:
    virtual ~SendMsgParamsCallback() = default;

    /**
     * getFlags() will be invoked to retrieve the desired flags to be passed
     * to ::sendmsg() system call. It is responsible for converting flags set in
     * the passed folly::WriteFlags enum into a integer flag bitmask that can be
     * passed to ::sendmsg. Some flags in folly::WriteFlags do not correspond to
     * flags that can be passed to ::sendmsg and may instead be handled via
     * getAncillaryData.
     *
     * This method was intentionally declared non-virtual, so there is no way to
     * override it. Instead feel free to override getFlagsImpl(...) instead, and
     * enjoy the convenience of defaultFlags passed there.
     *
     * @param flags     Write flags requested for the given write operation
     */
    int getFlags(folly::WriteFlags flags, bool zeroCopyEnabled) noexcept {
      return getFlagsImpl(flags, getDefaultFlags(flags, zeroCopyEnabled));
    }

    /**
     * getAncillaryData() will be invoked to initialize ancillary data buffer
     * referred by "msg_control" field of msghdr structure passed to ::sendmsg()
     * system call based on the flags set in the passed folly::WriteFlags enum.
     *
     * Some flags in folly::WriteFlags are not relevant during this process;
     * the default implementation only handles timestamping flags.
     *
     * The function requires that the size of buffer passed is equal to the
     * value returned by getAncillaryDataSize() method for the same combination
     * of flags.
     *
     * @param flags     Write flags requested for the given write operation
     * @param data      Pointer to ancillary data buffer to initialize.
     * @param writeTag  Documented on `WriteRequestTag`.
     * @param byteEventsEnabled      If byte events are enabled for this socket.
     *                               When enabled, flags relevant to socket
     *                               timestamps (e.g., TIMESTAMP_TX) should be
     *                               included in ancillary (msg_control) data.
     */
    virtual void getAncillaryData(
        folly::WriteFlags flags,
        void* data,
        const WriteRequestTag& writeTag,
        const bool byteEventsEnabled = false) noexcept;

    /**
     * getAncillaryDataSize() will be invoked to retrieve the size of
     * ancillary data buffer which should be passed to ::sendmsg() system call
     * The result must not exceed `maxAncillaryDataSize`.
     *
     * @param flags     Write flags requested for the given write operation
     * @param writeTag  Documented on `WriteRequestTag`.
     * @param byteEventsEnabled      If byte events are enabled for this socket.
     *                               When enabled, flags relevant to socket
     *                               timestamps (e.g., TIMESTAMP_TX) should be
     *                               included in ancillary (msg_control) data.
     */
    virtual uint32_t getAncillaryDataSize(
        folly::WriteFlags flags,
        const WriteRequestTag& writeTag,
        const bool byteEventsEnabled = false) noexcept;

    /**
     * Called immediately after a `sendmsg` corresponding to the preceding
     * `getAncillaryData()` successfully sends at least 1 byte.
     *
     * This is required to enable "exactly once" transmission of ancillary
     * data corresponding to `writeTag`.  For example, `AsyncFdSocket` ought
     * not transmit tag-associated FDs twice.  Per POSIX, ancillary data are
     * transmitted together with the first data byte.
     */
    virtual void wroteBytes(const WriteRequestTag&) noexcept {}

    // This is not an OS limitation (see `/proc/sys/net/core/optmem_max` on
    // Linux) but is done only because today's `AsyncSocket` implementation
    // uses `alloca` to allocate the ancillary data buffer on the stack in
    // order to support a cheap default `SendMsgParamsCallback` on every
    // socket.  If the buffer management could be handed to the socket (e.g.
    // each socket contains a few bytes of buffer for the default callback),
    // then we could delete this maximum, and `getAncillaryDataSize`, in
    // favor of `folly::ByteRange getAncillaryData()`.
    static const size_t maxAncillaryDataSize{0x5000};

   private:
    /**
     * getFlagsImpl() will be invoked by getFlags(folly::WriteFlags flags)
     * method to retrieve the flags to be passed to ::sendmsg() system call.
     * SendMsgParamsCallback::getFlags() is calling this method, and returns
     * its results directly to the caller in AsyncSocket.
     * Classes inheriting from SendMsgParamsCallback are welcome to override
     * this method to force SendMsgParamsCallback to return its own set
     * of flags.
     *
     * @param flags        Write flags requested for the given write operation
     * @param defaultflags A set of message flags returned by getDefaultFlags()
     *                     method for the given "flags" mask.
     */
    virtual int getFlagsImpl(folly::WriteFlags /*flags*/, int defaultFlags) {
      return defaultFlags;
    }

    /**
     * getDefaultFlags() will be invoked by  getFlags(folly::WriteFlags flags)
     * to retrieve the default set of flags, and pass them to getFlagsImpl(...)
     *
     * @param flags     Write flags requested for the given write operation
     */
    int getDefaultFlags(folly::WriteFlags flags, bool zeroCopyEnabled) noexcept;
  };

  /**
   * Container with state and processing logic for ByteEvents.
   */
  struct ByteEventHelper {
    bool byteEventsEnabled{false};
    long rawBytesWrittenWhenByteEventsEnabled{0};
    folly::Optional<AsyncSocketException> maybeEx;

    /**
     * Process a Cmsg and return a ByteEvent if available.
     *
     * The kernel will pass two cmsg for each timestamp:
     *   1. ScmTimestamping: Software / Hardware Timestamps.
     *   2. SockExtendedErrTimestamping: Byte offset associated with timestamp.
     *
     * These messages will be passed back-to-back; processCmsg() can handle them
     * in any order (1 then 2, or 2 then 1), as long the order is consistent
     * across timestamps.
     *
     * processCmsg() gracefully ignores Cmsg unrelated to socket timestamps, but
     * will throw if it receives a sequence of Cmsg that are not compliant with
     * its expectations.
     *
     * @return If the helper has received all components required to generate a
     *         ByteEvent (e.g., ScmTimestamping and SockExtendedErrTimestamping
     *         messages), it returns a ByteEvent and clears its local state.
     *         Otherwise, returns an empty optional.
     *
     *         If the helper has previously thrown a ByteEventHelper::Exception,
     *         it will not process further Cmsg and will continuously return an
     *         empty optional.
     *
     * @throw  If the helper receives a sequence of Cmsg that violate its
     *         expectations (e.g., multiple ScmTimestamping messages in a row
     *         without corresponding SockExtendedErrTimestamping messages), it
     *         throws a ByteEventHelper::Exception. Subsequent calls will return
     *         an empty optional.
     */
    folly::Optional<ByteEvent> processCmsg(
        const cmsghdr& cmsg, const size_t rawBytesWritten);

    /**
     * Exception class thrown by processCmsg.
     *
     * ByteEventHelper does not know the socket address and thus cannot
     * construct a AsyncSocketException. Instead, ByteEventHelper throws a
     * custom Exception and AsyncSocket rewraps it as an AsyncSocketException.
     */
    class Exception : public std::runtime_error {
      using std::runtime_error::runtime_error;
    };

   private:
    // state, reinitialized each time a complete timestamp is processed
    struct TimestampState {
      bool serrReceived{false};
      uint32_t typeRaw{0};
      uint32_t byteOffsetKernel{0};

      bool scmTsReceived{false};
      folly::Optional<std::chrono::nanoseconds> maybeSoftwareTs;
      folly::Optional<std::chrono::nanoseconds> maybeHardwareTs;
    };
    folly::Optional<TimestampState> maybeTsState_;
  };

  explicit AsyncSocket();
  /**
   * Create a new unconnected AsyncSocket.
   *
   * connect() must later be called on this socket to establish a connection.
   */
  explicit AsyncSocket(EventBase* evb);

  void setShutdownSocketSet(const std::weak_ptr<ShutdownSocketSet>& wSS);

  /**
   * Create a new AsyncSocket and begin the connection process.
   *
   * @param evb             EventBase that will manage this socket.
   * @param address         The address to connect to.
   * @param connectTimeout  Optional timeout in milliseconds for the connection
   *                        attempt.
   * @param useZeroCopy     Optional zerocopy socket mode
   */
  AsyncSocket(
      EventBase* evb,
      const folly::SocketAddress& address,
      uint32_t connectTimeout = 0,
      bool useZeroCopy = false);

  /**
   * Create a new AsyncSocket and begin the connection process.
   *
   * @param evb             EventBase that will manage this socket.
   * @param ip              IP address to connect to (dotted-quad).
   * @param port            Destination port in host byte order.
   * @param connectTimeout  Optional timeout in milliseconds for the connection
   *                        attempt.
   * @param useZeroCopy     Optional zerocopy socket mode
   */
  AsyncSocket(
      EventBase* evb,
      const std::string& ip,
      uint16_t port,
      uint32_t connectTimeout = 0,
      bool useZeroCopy = false);

  /**
   * Create a AsyncSocket from an already connected socket file descriptor.
   *
   * Note that while AsyncSocket enables TCP_NODELAY for sockets it creates
   * when connecting, it does not change the socket options when given an
   * existing file descriptor.  If callers want TCP_NODELAY enabled when using
   * this version of the constructor, they need to explicitly call
   * setNoDelay(true) after the constructor returns.
   *
   * @param evb            EventBase that will manage this socket.
   * @param fd             File descriptor to take over (connected socket).
   * @param zeroCopyBufId  Zerocopy buf id to start with.
   * @param peerAddress    Optional peer address (eg: returned from accept). If
   *                       nullptr, AsyncSocket will lazily attempt to determine
   *                       it from fd via a system call.
   * @param maybeConnectionEstablishTime  Optional parameter indicating when the
   *                                      connection was established. Can be
   *                                      used by acceptors to record when a
   *                                      connection was established and make
   *                                      this information available via the
   *                                      getConnectionEstablishTime() method.
   */
  AsyncSocket(
      EventBase* evb,
      NetworkSocket fd,
      uint32_t zeroCopyBufId = 0,
      const SocketAddress* peerAddress = nullptr,
      folly::Optional<std::chrono::steady_clock::time_point>
          maybeConnectionEstablishTime = folly::none);

  /**
   * Create an AsyncSocket from a different, already connected AsyncSocket.
   *
   * Similar to AsyncSocket(evb, fd) when fd was previously owned by an
   * AsyncSocket.
   */
  explicit AsyncSocket(AsyncSocket::UniquePtr);

  /**
   * Create an AsyncSocket from a different, already connected AsyncSocket.
   *
   * Similar to AsyncSocket(evb, fd) when fd was previously owned by an
   * AsyncSocket. Caller must call destroy on old AsyncSocket unless it is
   * in a smart pointer with appropriate destructor.
   */
  explicit AsyncSocket(AsyncSocket*);

  /**
   * Helper function to create an AsyncSocket..
   *
   * This passes in the correct destructor object, since AsyncSocket's
   * destructor is protected and cannot be invoked directly.
   */
  static UniquePtr newSocket(EventBase* evb) {
    return UniquePtr{new AsyncSocket(evb)};
  }

  /**
   * Helper function to create an AsyncSocket.
   */
  static UniquePtr newSocket(
      EventBase* evb,
      const folly::SocketAddress& address,
      uint32_t connectTimeout = 0,
      bool useZeroCopy = false) {
    return UniquePtr{
        new AsyncSocket(evb, address, connectTimeout, useZeroCopy)};
  }

  /**
   * Helper function to create an AsyncSocket.
   */
  static UniquePtr newSocket(
      EventBase* evb,
      const std::string& ip,
      uint16_t port,
      uint32_t connectTimeout = 0,
      bool useZeroCopy = false) {
    return UniquePtr{
        new AsyncSocket(evb, ip, port, connectTimeout, useZeroCopy)};
  }

  /**
   * Helper function to create an AsyncSocket.
   */
  static UniquePtr newSocket(
      EventBase* evb,
      NetworkSocket fd,
      const SocketAddress* peerAddress = nullptr) {
    return UniquePtr{new AsyncSocket(evb, fd, 0, peerAddress)};
  }

  /**
   * Destroy the socket.
   *
   * AsyncSocket::destroy() must be called to destroy the socket.
   * The normal destructor is private, and should not be invoked directly.
   * This prevents callers from deleting a AsyncSocket while it is invoking a
   * callback.
   */
  void destroy() override;

  /**
   * Get the EventBase used by this socket.
   */
  EventBase* getEventBase() const override { return eventBase_; }

  /**
   * Get the network socket used by the AsyncSocket.
   */
  NetworkSocket getNetworkSocket() const override { return fd_; }

  /**
   * Extract the file descriptor from the AsyncSocket.
   *
   * This will immediately cause any installed callbacks to be invoked with an
   * error.  The AsyncSocket may no longer be used after the file descriptor
   * has been extracted.
   *
   * This method should be used with care as the resulting fd is not guaranteed
   * to perfectly reflect the state of the AsyncSocket (security state,
   * pre-received data, etc.).
   *
   * Returns the file descriptor.  The caller assumes ownership of the
   * descriptor, and it will not be closed when the AsyncSocket is destroyed.
   */
  virtual NetworkSocket detachNetworkSocket();

  /**
   * Extract the file descriptor from an established AsyncSocket.
   *
   * The read callback is removed immediately. If a native io_uring receive
   * is in flight, it is cancelled and any data consumed while cancellation is
   * in progress is returned to the callback together with pre-received data.
   * Pending writes are failed as part of detaching the file descriptor.
   *
   * On success, the callback assumes ownership of the file descriptor and the
   * AsyncSocket may no longer be used. The callback may be invoked inline if
   * there is no native receive operation to drain.
   *
   * Fails with INVALID_STATE if the socket is not established. On failure the
   * socket is left unchanged.
   */
  void asyncDetachFd(AsyncDetachFdCallback* callback);

  /**
   * Switch from native io_uring socket operations to EventHandler I/O.
   *
   * This is a no-op if native io_uring I/O is not enabled. Otherwise, on
   * success the read callback is removed immediately. The returned future
   * completes after an in-flight receive has been cancelled and contains any
   * bytes that were consumed from the socket while cancellation was in
   * progress. The socket must not be read, moved, or re-armed until the future
   * completes.
   *
   * The descriptor's blocking mode is unchanged. The caller must put it in
   * non-blocking mode after the future completes and before using EventHandler
   * I/O, as AsyncSSLSocket does when adopting the socket.
   *
   * Fails with INVALID_STATE if the socket is not established or a connect or
   * write is pending. On failure the socket is left unchanged.
   */
  SemiFuture<std::unique_ptr<IOBuf>> asyncSwitchToEventHandlerMode();

  /**
   * Initiate a connection.
   *
   * @param callback  The callback to inform when the connection attempt
   *                  completes.
   * @param address   The address to connect to.
   * @param timeout   A timeout value, in milliseconds.  If the connection
   *                  does not succeed within this period,
   *                  callback->connectError() will be invoked.
   * @param bindOptions Either a SocketAddress to bind to, or a NetworkSocket
   *                  with an address already bound to it via bind().
   *                  Ownership of a NetworkSocket is transferred from the
   *                  caller to this AsyncSocket.
   */
  virtual void connect(
      ConnectCallback* callback,
      const folly::SocketAddress& address,
      int timeout = 0,
      const SocketOptionMap& options = emptySocketOptionMap,
      const BindOptions& bindOptions = anyAddress(),
      const std::string& ifName = "") noexcept override;

  void connect(
      ConnectCallback* callback,
      const std::string& ip,
      uint16_t port,
      int timeout = 0,
      const SocketOptionMap& options = emptySocketOptionMap) noexcept;

  /**
   * If a connect request is in-flight, cancels it and closes the socket
   * immediately. Otherwise, this is a no-op.
   *
   * This does not invoke any connection related callbacks. Call this to
   * prevent any connect callback while cleaning up, etc.
   */
  void cancelConnect() override;

  /**
   * Set the send timeout.
   *
   * If write requests do not make any progress for more than the specified
   * number of milliseconds, fail all pending writes and close the socket.
   *
   * If write requests are currently pending when setSendTimeout() is called,
   * the timeout interval is immediately restarted using the new value.
   *
   * (See the comments for AsyncSocket for an explanation of why AsyncSocket
   * provides setSendTimeout() but not setRecvTimeout().)
   *
   * @param milliseconds  The timeout duration, in milliseconds.  If 0, no
   *                      timeout will be used.
   */
  void setSendTimeout(uint32_t milliseconds) override;

  /**
   * Get the send timeout.
   *
   * @return Returns the current send timeout, in milliseconds.  A return value
   *         of 0 indicates that no timeout is set.
   */
  uint32_t getSendTimeout() const override { return sendTimeout_; }

  /**
   * Set the maximum number of reads to execute from the underlying
   * socket each time the EventBase detects that new ingress data is
   * available. The default is unlimited, but callers can use this method
   * to limit the amount of data read from the socket per event loop
   * iteration.
   *
   * @param maxReads  Maximum number of reads per data-available event;
   *                  a value of zero means unlimited.
   */
  void setMaxReadsPerEvent(uint16_t maxReads) { maxReadsPerEvent_ = maxReads; }

  /**
   * Get the maximum number of reads this object will execute from
   * the underlying socket each time the EventBase detects that new
   * ingress data is available.
   *
   * @returns Maximum number of reads per data-available event; a value
   *          of zero means unlimited.
   */
  uint16_t getMaxReadsPerEvent() const { return maxReadsPerEvent_; }

  /**
   * Set a pointer to ErrMessageCallback implementation which will be
   * receiving notifications for messages posted to the error queue
   * associated with the socket.
   * ErrMessageCallback is implemented only for platforms with
   * per-socket error message queues support (recvmsg() system call must
   * )
   *
   */
  virtual void setErrMessageCB(ErrMessageCallback* callback);

  /**
   * Get a pointer to ErrMessageCallback implementation currently
   * registered with this socket.
   *
   */
  virtual ErrMessageCallback* getErrMessageCallback() const;

  /**
   * Set a pointer to ReadAncillaryDataCallback implementation which will
   * be invoked with the ancillary data when we read a buffer from the
   * associated socket.
   * ReadAncillaryDataCallback is implemented only for platforms with
   * kernel timestamp support.
   *
   */
  virtual void setReadAncillaryDataCB(ReadAncillaryDataCallback* callback);

  /**
   * Get a pointer to ReadAncillaryDataCallback implementation currently
   * registered with this socket.
   *
   */
  virtual ReadAncillaryDataCallback* getReadAncillaryDataCallback() const;

  /**
   * Set a pointer to SendMsgParamsCallback implementation which
   * will be used to form ::sendmsg() system call parameters
   *
   */
  virtual void setSendMsgParamCB(SendMsgParamsCallback* callback);

  /**
   * Get a pointer to SendMsgParamsCallback implementation currently
   * registered with this socket.
   *
   */
  virtual SendMsgParamsCallback* getSendMsgParamsCB() const;

  /**
   * Override netops::Dispatcher to be used for netops:: calls.
   *
   * Pass empty shared_ptr to reset to default.
   * Override can be used by unit tests to intercept and mock netops:: calls.
   */
  virtual void setOverrideNetOpsDispatcher(
      std::shared_ptr<netops::Dispatcher> dispatcher) {
    netops_.setOverride(std::move(dispatcher));
  }

  /**
   * Returns override netops::Dispatcher being used for netops:: calls.
   *
   * Returns empty shared_ptr if no override set.
   * Override can be used by unit tests to intercept and mock netops:: calls.
   */
  virtual std::shared_ptr<netops::Dispatcher> getOverrideNetOpsDispatcher()
      const {
    return netops_.getOverride();
  }

  /**
   * Override folly::TcpInfoDispatcher to be used for getting TcpInfo.
   *
   * Pass empty shared_ptr to reset to default.
   * Override can be used by unit tests to intercept and mock
   * TcpInfo::initFromFd calls.
   */
  virtual void setOverrideTcpInfoDispatcher(
      std::shared_ptr<folly::TcpInfoDispatcher> dispatcher) {
    tcpInfoDispatcher_.setOverride(std::move(dispatcher));
  }

  /**
   * Returns override folly::TcpInfoDispatcher being used for tcpinfo calls.
   *
   * Returns empty shared_ptr if no override set.
   * Override can be used by unit tests to intercept and mock
   * TcpInfo::initFromFd calls.
   */
  virtual std::shared_ptr<folly::TcpInfoDispatcher>
  getOverrideTcpInfoDispatcher() const {
    return tcpInfoDispatcher_.getOverride();
  }

  // Read and write methods
  void setReadCB(ReadCallback* callback) override;
  ReadCallback* getReadCallback() const override;

  /**
   * Create a memory store to use for zero copy reads.
   *
   * The memory store contains a fixed number of entries, each with a fixed
   * size.  When data is read using zero-copy the kernel will place it in one
   * of these entries, and it will be returned to the callback with
   * readZeroCopyDataAvailable().  The callback must release the IOBuf
   * reference to make the entry available again for future zero-copy reads.
   * If all entries are exhausted the read code will fall back to non-zero-copy
   * reads.
   *
   * Note: it is the caller's responsibility to ensure that they do not destroy
   * the ZeroCopyMemStore while it still has any outstanding entries in use.
   * The caller must ensure the ZeroCopyMemStore exists until all callers have
   * finished using any data returned via zero-copy reads, and released the
   * IOBuf objects containing that data.
   *
   * @param entries  The number of entries to allocate in the memory store.
   * @param size     The size of each entry, in bytes.  This should be a
   *                 multiple of the kernel page size.
   */

  static std::unique_ptr<AsyncReader::ReadCallback::ZeroCopyMemStore>
  createDefaultZeroCopyMemStore(size_t entries, size_t size);

  bool setZeroCopy(bool enable) override;
  bool getZeroCopy() const override { return zeroCopyEnabled_; }

  uint32_t getZeroCopyBufId() const { return zeroCopyBufId_; }

  // Transfers outstanding zero-copy write completion state from `other` (whose
  // fd must already be detached) to this socket during an fd handoff.
  void moveZeroCopyStateFrom(AsyncSocket& other);

  size_t getZeroCopyReenableThreshold() const {
    return zeroCopyReenableThreshold_;
  }

  void setZeroCopyEnableFunc(AsyncWriter::ZeroCopyEnableFunc func) override;

  void setZeroCopyEnableThreshold(size_t threshold) override;

  void setZeroCopyReenableThreshold(size_t threshold);

  struct ZeroCopyDrainConfig {
    std::chrono::milliseconds drainDelay{1000};
    // Upper bound on how long the socket will stay alive polling MSG_ERRQUEUE
    // for outstanding zero-copy completions. If the kernel has not returned
    // them by then the buffers are released anyway, so a socket whose
    // completions never arrive cannot pin an fd and its IOBufs forever.
    std::chrono::milliseconds maxDrainDuration{std::chrono::minutes(5)};
    std::optional<unsigned short> linger;
  };

  void setZeroCopyDrainConfig(const ZeroCopyDrainConfig& config);

  void write(
      WriteCallback* callback,
      const void* buf,
      size_t bytes,
      WriteFlags flags = WriteFlags::NONE) override;
  void writev(
      WriteCallback* callback,
      const iovec* vec,
      size_t count,
      WriteFlags flags = WriteFlags::NONE) override;
  void writeChain(
      WriteCallback* callback,
      std::unique_ptr<folly::IOBuf>&& buf,
      WriteFlags flags = WriteFlags::NONE) override;

  class WriteRequest;
  virtual void writeRequest(WriteRequest* req);
  void writeRequestReady() { handleWrite(); }

  // Methods inherited from AsyncTransport
  void close() override;
  void closeNow() override;
  void closeWithReset() override;
  void shutdownWrite() override;
  void shutdownWriteNow() override;

  bool readable() const override;
  bool writable() const override;
  bool isPending() const override;
  bool hangup() const override;
  bool good() const override;
  bool error() const override;
  void attachEventBase(EventBase* eventBase) override;
  void detachEventBase() override;
  bool isDetachable() const override;

  void getLocalAddress(folly::SocketAddress* address) const override;
  void getPeerAddress(folly::SocketAddress* address) const override;

  bool isEorTrackingEnabled() const override { return trackEor_; }

  void setEorTracking(bool track) override { trackEor_ = track; }

  bool connecting() const override { return (state_ == StateEnum::CONNECTING); }

  virtual bool isClosedByPeer() const {
    return (
        state_ == StateEnum::CLOSED &&
        (readErr_ == READ_EOF || readErr_ == READ_ERROR));
  }

  virtual bool isClosedBySelf() const {
    return (
        state_ == StateEnum::CLOSED &&
        (readErr_ != READ_EOF && readErr_ != READ_ERROR));
  }

  size_t getAppBytesWritten() const override { return appBytesWritten_; }

  size_t getRawBytesWritten() const override { return rawBytesWritten_; }

  size_t getAppBytesReceived() const override { return appBytesReceived_; }

  size_t getRawBytesReceived() const override { return getAppBytesReceived(); }

  size_t getAppBytesBuffered() const override {
    return totalAppBytesScheduledForWrite_ - appBytesWritten_;
  }
  size_t getRawBytesBuffered() const override { return getAppBytesBuffered(); }

  size_t getAllocatedBytesBuffered() const override {
    return allocatedBytesBuffered_;
  }

  // End of methods inherited from AsyncTransport

  std::chrono::nanoseconds getConnectTime() const {
    return connectEndTime_ - connectStartTime_;
  }

  std::chrono::milliseconds getConnectTimeout() const {
    return connectTimeout_;
  }

  /**
   * Returns when connect() started.
   */
  std::chrono::steady_clock::time_point getConnectStartTime() const {
    return connectStartTime_;
  }

  /**
   * Returns when connect() finished (either successfully or failed).
   */
  std::chrono::steady_clock::time_point getConnectEndTime() const {
    return connectEndTime_;
  }

  /**
   * Returns when the connection was established.
   *
   *  -  If connect() was called and succeeded, this is the same as
   *     getConnectEndTime().
   *
   *  -  If AsyncSocket was initialized with a file descriptor (e.g., by an
   *     acceptor), returns the connection establishment time passed to the
   *     constructor. If no time was passed, returns folly::none.
   */
  folly::Optional<std::chrono::steady_clock::time_point>
  getConnectionEstablishTime() const {
    return maybeConnectionEstablishTime_;
  }

  bool getTFOAttempted() const { return tfoInfo_.attempted; }

  /**
   * Returns whether or not the attempt to use TFO
   * finished successfully. This does not necessarily
   * mean TFO worked, just that trying to use TFO
   * succeeded.
   */
  bool getTFOFinished() const { return tfoInfo_.finished; }

  /**
   * Returns whether or not TFO attempt succeeded on this
   * connection.
   * For servers this is pretty straightforward API and can
   * be invoked right after the connection is accepted. This API
   * will perform one syscall.
   * This API is a bit tricky to use for clients, since clients
   * only know this for sure after the SYN-ACK is returned. So it's
   * appropriate to call this only after the first application
   * data is read from the socket when the caller knows that
   * the SYN has been ACKed by the server.
   */
  bool getTFOSucceeded() const override;

  // Methods controlling socket options

  /**
   * Force writes to be transmitted immediately.
   *
   * This controls the TCP_NODELAY socket option.  When enabled, TCP segments
   * are sent as soon as possible, even if it is not a full frame of data.
   * When disabled, the data may be buffered briefly to try and wait for a full
   * frame of data.
   *
   * By default, TCP_NODELAY is enabled for AsyncSocket objects.
   *
   * This method will fail if the socket is not currently open.
   *
   * @return Returns 0 if the TCP_NODELAY flag was successfully updated,
   *         or a non-zero errno value on error.
   */
  int setNoDelay(bool noDelay) override;

  /**
   * Set the FD_CLOEXEC flag so that the socket will be closed if the program
   * later forks and execs.
   */
  void setCloseOnExec();

  /*
   * Set the Flavor of Congestion Control to be used for this Socket
   * Please check '/lib/modules/<kernel>/kernel/net/ipv4' for tcp_*.ko
   * first to make sure the module is available for plugging in
   * Alternatively you can choose from net.ipv4.tcp_allowed_congestion_control
   */
  int setCongestionFlavor(const std::string& cname);

  /*
   * Forces ACKs to be sent immediately
   *
   * @return Returns 0 if the TCP_QUICKACK flag was successfully updated,
   *         or a non-zero errno value on error.
   */
  int setQuickAck(bool quickack);

  /**
   * Set the send bufsize
   */
  int setSendBufSize(size_t bufsize);

  /**
   * Set the recv bufsize
   */
  int setRecvBufSize(size_t bufsize);

#if defined(__linux__)
  /**
   * @brief This method is used to get the number of bytes that are currently
   *        stored in the TCP send/tx buffer
   *
   * @return the number of bytes in the send/tx buffer or folly::none if there
   *         was a problem
   */
  size_t getSendBufInUse() const;

  /**
   * @brief This method is used to get the number of bytes that are currently
   *        stored in the TCP receive/rx buffer
   *
   * @return the number of bytes in the receive/rx buffer or folly::none if
   *         there was a problem
   */
  size_t getRecvBufInUse() const;
#endif

/**
 * Sets a specific tcp personality
 * Available only on kernels 3.2 and greater
 */
#define SO_SET_NAMESPACE 41
  int setTCPProfile(int profd);

  /**
   * Generic API for reading a socket option.
   *
   * @param level     same as the "level" parameter in getsockopt().
   * @param optname   same as the "optname" parameter in getsockopt().
   * @param optval    pointer to the variable in which the option value should
   *                  be returned.
   * @param optlen    value-result argument, initially containing the size of
   *                  the buffer pointed to by optval, and modified on return
   *                  to indicate the actual size of the value returned.
   * @return          same as the return value of getsockopt().
   */
  template <typename T>
  int getSockOpt(int level, int optname, T* optval, socklen_t* optlen) {
    return netops_->getsockopt(fd_, level, optname, (void*)optval, optlen);
  }

  /**
   * Generic API for setting a socket option.
   *
   * @param level     same as the "level" parameter in getsockopt().
   * @param optname   same as the "optname" parameter in getsockopt().
   * @param optval    the option value to set.
   * @return          same as the return value of setsockopt().
   */
  template <typename T>
  int setSockOpt(int level, int optname, const T* optval) {
    return netops_->setsockopt(fd_, level, optname, optval, sizeof(T));
  }

  int setSockOpt(
      int level, int optname, const void* optval, socklen_t optsize) override {
    return netops_->setsockopt(fd_, level, optname, optval, optsize);
  }

  /**
   * Virtual method for reading a socket option returning integer
   * value, which is the most typical case. Convenient for overriding
   * and mocking.
   *
   * @param level     same as the "level" parameter in getsockopt().
   * @param optname   same as the "optname" parameter in getsockopt().
   * @param optval    same as "optval" parameter in getsockopt().
   * @param optlen    same as "optlen" parameter in getsockopt().
   * @return          same as the return value of getsockopt().
   */
  virtual int getSockOptVirtual(
      int level, int optname, void* optval, socklen_t* optlen) {
    return netops_->getsockopt(fd_, level, optname, optval, optlen);
  }

  /**
   * Virtual method for setting a socket option accepting integer
   * value, which is the most typical case. Convenient for overriding
   * and mocking.
   *
   * @param level     same as the "level" parameter in setsockopt().
   * @param optname   same as the "optname" parameter in setsockopt().
   * @param optval    same as "optval" parameter in setsockopt().
   * @param optlen    same as "optlen" parameter in setsockopt().
   * @return          same as the return value of setsockopt().
   */
  virtual int setSockOptVirtual(
      int level, int optname, void const* optval, socklen_t optlen) {
    return netops_->setsockopt(fd_, level, optname, optval, optlen);
  }

  /**
   * Set pre-received data, to be returned to read callback before any data
   * from the socket.
   */
  void setPreReceivedData(std::unique_ptr<IOBuf> data) override {
    if (preReceivedData_) {
      preReceivedData_->prependChain(std::move(data));
    } else {
      preReceivedData_ = std::move(data);
    }
  }

  std::unique_ptr<IOBuf> takePreReceivedData() override {
    return std::move(preReceivedData_);
  }

  /**
   * Enables TFO behavior on the AsyncSocket if FOLLY_ALLOW_TFO
   * is set.
   */
  void enableTFO() override {
    // No-op if folly does not allow tfo
#if defined(FOLLY_ALLOW_TFO) && FOLLY_ALLOW_TFO
    tfoInfo_.enabled = true;
#endif
  }

  /**
   * Sets TOS or traffic class. Throws an exception on error.
   */
  void setTosOrTrafficClass(int tosOrTrafficClass);

  /**
   * This flag controls whether or not IP_BIND_ADDRESS_NO_PORT is enabled for
   * AsyncSocket sockets. This is enabled by default.
   */
  void setBindAddressNoPort(bool flag) { bindAddressNoPort_ = flag; }

  void disableTransparentTls() override { noTransparentTls_ = true; }

  void disableTSocks() { noTSocks_ = true; }

  enum class StateEnum : uint8_t {
    UNINIT,
    CONNECTING,
    ESTABLISHED,
    CLOSED,
    ERROR,
    FAST_OPEN,
  };

  void setBufferCallback(BufferCallback* cb);

  // Callers should set this prior to connecting the socket for the safest
  // behavior.
  void setEvbChangedCallback(std::unique_ptr<EvbChangeCallback> cb) {
    evbChangeCb_ = std::move(cb);
  }

  /**
   * Attempt to cache the current local and peer addresses (if not already
   * cached) so that they are available from getPeerAddress() and
   * getLocalAddress() even after the socket is closed.
   */
  void cacheAddresses() override;

  /**
   * Returns true if there is any zero copy write in progress
   * Needs to be called from within the socket's EVB thread
   */
  bool isZeroCopyWriteInProgress() const noexcept;

  /**
   * Tries to process the msg error queue
   * And returns true if there are no more zero copy writes in progress
   */
  bool processZeroCopyWriteInProgress() noexcept;

  /**
   * Whether socket should be closed on write failure (true by default).
   */
  void setCloseOnFailedWrite(bool closeOnFailedWrite) {
    closeOnFailedWrite_ = closeOnFailedWrite;
  }

  /**
   * Get folly::TcpInfo from socket
   */
  folly::Expected<folly::TcpInfo, std::errc> getTcpInfo(
      const TcpInfo::LookupOptions& options);

  /**
   * writeReturn is the total number of bytes written, or WRITE_ERROR on error.
   * If no data has been written, 0 is returned.
   * exception is a more specific exception that cause a write error.
   * Not all writes have exceptions associated with them thus writeReturn
   * should be checked to determine whether the operation resulted in an error.
   */
  struct WriteResult {
    explicit WriteResult(ssize_t ret) : writeReturn(ret) {}

    WriteResult(ssize_t ret, std::unique_ptr<const AsyncSocketException> e)
        : writeReturn(ret), exception(std::move(e)) {}

    ssize_t writeReturn;
    std::unique_ptr<const AsyncSocketException> exception;
  };

  /**
   * readReturn is the number of bytes read, or READ_EOF on EOF, or
   * READ_ERROR on error, or READ_BLOCKING if the operation will
   * block.
   * exception is a more specific exception that may have caused a read error.
   * Not all read errors have exceptions associated with them thus readReturn
   * should be checked to determine whether the operation resulted in an error.
   */
  struct ReadResult {
    explicit ReadResult(ssize_t ret) : readReturn(ret) {}

    ReadResult(ssize_t ret, std::unique_ptr<const AsyncSocketException> e)
        : readReturn(ret), exception(std::move(e)) {}

    ssize_t readReturn;
    std::unique_ptr<const AsyncSocketException> exception;
  };

  /**
   * A WriteRequest object tracks information about a pending write operation.
   */
  class WriteRequest {
   public:
    WriteRequest(AsyncSocket* socket, WriteCallback* callback)
        : socket_(socket),
          callbackWithState_(WriteCallbackWithState(callback)),
          releaseIOBufCallback_(
              callback ? callback->getReleaseIOBufCallback() : nullptr) {}

    WriteRequest(AsyncSocket* socket, WriteCallbackWithState callbackWithState)
        : socket_(socket),
          callbackWithState_(callbackWithState),
          releaseIOBufCallback_(
              callbackWithState.getCallback()
                  ? callbackWithState.getCallback()->getReleaseIOBufCallback()
                  : nullptr) {}

    virtual void start() {}

    virtual void destroy() = 0;

    virtual WriteResult performWrite() = 0;

    virtual void consume() = 0;

    virtual bool isComplete() = 0;

    WriteRequest* getNext() const { return next_; }

    WriteCallback* getCallback() const {
      return callbackWithState_.getCallback();
    }

    WriteCallbackWithState& getCallbackWithState() {
      return callbackWithState_;
    }

    uint32_t getTotalBytesWritten() const { return totalBytesWritten_; }

    void append(WriteRequest* next) {
      assert(next_ == nullptr);
      next_ = next;
    }

    void fail(const char* fn, const AsyncSocketException& ex) {
      socket_->failWrite(fn, ex);
    }

    void bytesWritten(size_t count) {
      totalBytesWritten_ += uint32_t(count);
      socket_->appBytesWritten_ += count;
    }

   protected:
    // protected destructor, to ensure callers use destroy()
    virtual ~WriteRequest() {}

    AsyncSocket* socket_; ///< parent socket
    WriteRequest* next_{nullptr}; ///< pointer to next WriteRequest
    WriteCallbackWithState callbackWithState_; ///< completion callback
    ReleaseIOBufCallback* releaseIOBufCallback_; ///< release IOBuf callback
    uint32_t totalBytesWritten_{0}; ///< total bytes written
  };

 public:
  /**
   * Observer of socket events.
   */
  class LegacyLifecycleObserver : public AsyncSocketObserverInterface {
   public:
    /**
     * Observer configuration.
     *
     * Specifies events observer wants to receive. Cannot be changed post
     * initialization because the transport may turn on / off instrumentation
     * when observers are added / removed, based on the observer configuration.
     */
    struct Config {
      Config() = default;
      Config(const Config&) = default;
      Config& operator=(const Config&) = default;
      virtual ~Config() = default;

      // receive ByteEvents
      bool byteEvents{false};

      // observer is notified during prewrite stage and can add WriteFlags
      bool prewrite{false};

      /**
       * Enable all events in config.
       */
      virtual void enableAllEvents() {
        byteEvents = true;
        prewrite = true;
      }

      /**
       * Returns a config where all events are enabled.
       */
      static Config getConfigAllEventsEnabled() {
        Config config = {};
        config.enableAllEvents();
        return config;
      }
    };

    /**
     * Constructor for observer, uses default config (instrumentation disabled).
     */
    LegacyLifecycleObserver() : LegacyLifecycleObserver(Config()) {}

    /**
     * Constructor for observer.
     *
     * @param config      Config, defaults to auxilary instrumentation disabled.
     */
    explicit LegacyLifecycleObserver(const Config& observerConfig)
        : observerConfig_(observerConfig) {}

    ~LegacyLifecycleObserver() override = default;

    /**
     * Returns observer's configuration.
     *
     * @return            Observer configuration.
     */
    const Config& getConfig() { return observerConfig_; }

    /**
     * observerAttach() will be invoked when an observer is added.
     *
     * @param socket   Socket where observer was installed.
     */
    virtual void observerAttach(AsyncSocket* /* socket */) noexcept = 0;

    /**
     * observerDetached() will be invoked if the observer is uninstalled prior
     * to socket destruction.
     *
     * No further events will be invoked after observerDetach().
     *
     * @param socket   Socket where observer was uninstalled.
     */
    virtual void observerDetach(AsyncSocket* /* socket */) noexcept = 0;

    /**
     * destroy() is the terminal callback for this observer attachment.
     *
     * It is invoked when the socket is logically destroyed. This may happen
     * before the AsyncSocket destructor runs when the socket and its file
     * descriptor must remain alive to drain zero-copy completions.
     *
     * No further observer callbacks will be invoked after destroy().
     *
     * @param socket   Socket being logically destroyed.
     */
    virtual void destroy(AsyncSocket* /* socket */) noexcept = 0;

   protected:
    // observer configuration; cannot be changed post instantiation
    const Config observerConfig_;
  };

  /**
   * Adds a lifecycle observer.
   *
   * Observers can tie their lifetime to aspects of this socket's lifecycle /
   * lifetime and perform inspection at various states.
   *
   * This enables instrumentation to be added without changing / interfering
   * with how the application uses the socket.
   *
   * Observer should implement AsyncSocket::LegacyLifecycleObserver to
   * receive additional lifecycle events specific to AsyncSocket.
   *
   * @param observer     Observer to add (implements LegacyLifecycleObserver).
   */
  virtual void addLifecycleObserver(LegacyLifecycleObserver* observer);

  /**
   * Removes a lifecycle observer.
   *
   * @param observer     Observer to remove.
   * @return             Whether observer found and removed from list.
   */
  virtual bool removeLifecycleObserver(LegacyLifecycleObserver* observer);

  /**
   * Returns installed lifecycle observers.
   *
   * @return             Vector with installed observers.
   */
  [[nodiscard]] virtual std::vector<LegacyLifecycleObserver*>
  getLifecycleObservers() const;

  /**
   * Adds an observer.
   *
   * If the observer is already added, this is a no-op.
   *
   * @param observer     Observer to add.
   * @return             Whether the observer was added (fails if no list).
   */
  virtual bool addObserver(Observer* observer) {
    if (auto list = getAsyncSocketObserverContainer()) {
      list->addObserver(observer);
      return true;
    }
    return false;
  }

  /**
   * Adds an observer.
   *
   * If the observer is already added, this is a no-op.
   *
   * @param observer     Observer to add.
   * @return             Whether the observer was added (fails if no list).
   */
  bool addObserver(std::shared_ptr<Observer> observer) {
    if (auto list = getAsyncSocketObserverContainer()) {
      list->addObserver(std::move(observer));
      return true;
    }
    return false;
  }

  /**
   * Removes an observer.
   *
   * @param observer     Observer to remove.
   * @return             Whether the observer was found and removed.
   */
  virtual bool removeObserver(Observer* observer) {
    if (auto list = getAsyncSocketObserverContainer()) {
      return list->removeObserver(observer);
    }
    return false;
  }

  /**
   * Removes an observer.
   *
   * @param observer     Observer to remove.
   * @return             Whether the observer was found and removed.
   */
  virtual bool removeObserver(std::shared_ptr<Observer> observer) {
    if (auto list = getAsyncSocketObserverContainer()) {
      return list->removeObserver(std::move(observer));
    }
    return false;
  }

  /**
   * Get number of observers.
   *
   * @return             Number of observers.
   */
  [[nodiscard]] virtual size_t numObservers() {
    if (auto list = getAsyncSocketObserverContainer()) {
      return list->numObservers();
    }
    return 0;
  }

  /**
   * Returns list of attached observers that are of type T.
   *
   * @return             Attached observers of type T.
   */
  template <typename T = Observer>
  std::vector<T*> findObservers() {
    if (auto list = getAsyncSocketObserverContainer()) {
      return list->findObservers<T>();
    }
    return {};
  }

 private:
  /**
   * Returns the AsyncSocketObserverContainer or nullptr if not available.
   */
  [[nodiscard]] AsyncSocketObserverContainer*
  getAsyncSocketObserverContainer() {
    return &observerContainer_;
  }

 public:
  /**
   * Split iovec array at given byte offsets; produce a new array with result.
   */
  static void splitIovecArray(
      const size_t startOffset,
      const size_t endOffset,
      const iovec* srcVec,
      const size_t srcCount,
      iovec* dstVec,
      size_t& dstCount);

  void applyOptions(
      const SocketOptionMap& options, SocketOptionKey::ApplyPos pos);

 protected:
  enum ReadResultEnum {
    READ_EOF = 0,
    READ_ERROR = -1,
    READ_BLOCKING = -2,
    READ_NO_ERROR = -3,
    READ_ASYNC = -4,
  };

  enum WriteResultEnum {
    WRITE_ERROR = -1,
  };

  /**
   * Protected destructor.
   *
   * Users of AsyncSocket must never delete it directly.  Instead, invoke
   * destroy() instead.  (See the documentation in DelayedDestruction.h for
   * more details.)
   */
  ~AsyncSocket() override;

  friend std::ostream& operator<<(std::ostream& os, const StateEnum& state);

  enum ShutdownFlags {
    /// shutdownWrite() called, but we are still waiting on writes to drain
    SHUT_WRITE_PENDING = 0x01,
    /// writes have been completely shut down
    SHUT_WRITE = 0x02,
    /**
     * Reads have been shutdown.
     *
     * At the moment we don't distinguish between remote read shutdown
     * (received EOF from the remote end) and local read shutdown.  We can
     * only receive EOF when a read callback is set, and we immediately inform
     * it of the EOF.  Therefore there doesn't seem to be any reason to have a
     * separate state of "received EOF but the local side may still want to
     * read".
     *
     * We also don't currently provide any API for only shutting down the read
     * side of a socket.  (This is a no-op as far as TCP is concerned, anyway.)
     */
    SHUT_READ = 0x04,
  };

  class BytesWriteRequest;

  class WriteTimeout : public AsyncTimeout {
   public:
    WriteTimeout(AsyncSocket* socket, EventBase* eventBase)
        : AsyncTimeout(eventBase), socket_(socket) {}

    void timeoutExpired() noexcept override { socket_->timeoutExpired(); }

   private:
    AsyncSocket* socket_;
  };

  class IoHandler : public EventHandler {
   public:
    IoHandler(AsyncSocket* socket, EventBase* eventBase)
        : EventHandler(eventBase, NetworkSocket()), socket_(socket) {}
    IoHandler(AsyncSocket* socket, EventBase* eventBase, NetworkSocket fd)
        : EventHandler(eventBase, fd), socket_(socket) {}

    void handlerReady(uint16_t events) noexcept override {
      socket_->ioReady(events);
    }

   private:
    AsyncSocket* socket_;
  };

  void init();

  class ImmediateReadCB : public folly::EventBase::LoopCallback {
   public:
    explicit ImmediateReadCB(AsyncSocket* socket) : socket_(socket) {}
    void runLoopCallback() noexcept override {
      DestructorGuard dg(socket_);
      socket_->checkForImmediateRead();
    }

   private:
    AsyncSocket* socket_;
  };

  /**
   * Schedule checkForImmediateRead to be executed in the next loop
   * iteration.
   */
  void scheduleImmediateRead() noexcept {
    if (good()) {
      eventBase_->runInLoop(&immediateReadHandler_);
    }
  }

  /**
   * Schedule handleInitalReadWrite to run in the next iteration.
   */
  void scheduleInitialReadWrite() noexcept {
    if (good()) {
      DestructorGuard dg(this);
      eventBase_->runInLoop([this, dg] {
        if (good()) {
          handleInitialReadWrite();
        }
      });
    }
  }

  void drainErrorQueue() noexcept;

  // event notification methods
  void ioReady(uint16_t events) noexcept;
  virtual void checkForImmediateRead() noexcept;
  virtual void handleInitialReadWrite() noexcept;
  virtual void prepareReadBuffer(void** buf, size_t* buflen);
  virtual void prepareReadBuffers(IOBufIovecBuilder::IoVecVec& iovs);
  virtual size_t handleErrMessages() noexcept;
  virtual void handleRead() noexcept;
  virtual void handleWrite() noexcept;
  virtual void handleConnect() noexcept;
  void timeoutExpired() noexcept;
  bool hasPendingWrites() noexcept;

  /**
   * Handler for when the file descriptor is attached to the AsyncSocket.

   * This updates the EventHandler to start using the fd and notifies all
   * observers attached to the socket. This is necessary to let
   * observers know about an attached fd immediately (i.e., on connection
   * attempt) rather than when the connection succeeds.
   */
  virtual void handleNetworkSocketAttached();

  /**
   * Populate an iovec array from an IOBuf and attempt to write it.
   *
   * @param callback Write completion/error callback.
   * @param vec      Target iovec array; caller retains ownership.
   * @param count    Number of IOBufs to write, beginning at start of buf.
   * @param buf      Chain of iovecs.
   * @param flags    set of flags for the underlying write calls, like cork
   */
  void writeChainImpl(
      WriteCallback* callback,
      iovec* vec,
      size_t count,
      std::unique_ptr<folly::IOBuf>&& buf,
      WriteFlags flags);

  /**
   * Write as much data as possible to the socket without blocking,
   * and queue up any leftover data to send when the socket can
   * handle writes again.
   *
   * @param callback    The callback to invoke when the write is completed.
   * @param vec         Array of buffers to write; this method will make a
   *                    copy of the vector (but not the buffers themselves)
   *                    if the write has to be completed asynchronously.
   * @param count       Number of elements in vec.
   * @param buf         The IOBuf that manages the buffers referenced by
   *                    vec, or a pointer to nullptr if the buffers are not
   *                    associated with an IOBuf.  Note that ownership of
   *                    the IOBuf is transferred here; upon completion of
   *                    the write, the AsyncSocket deletes the IOBuf.
   * @param totalBytes  The total number of bytes to be written.
   * @param flags       Set of write flags.
   */
  void writeImpl(
      WriteCallback* callback,
      const iovec* vec,
      size_t count,
      std::unique_ptr<folly::IOBuf>&& buf,
      size_t totalBytes,
      WriteFlags flags = WriteFlags::NONE);

  /**
   * Attempt to write to the socket.
   *
   * @param vec             The iovec array pointing to the buffers to write.
   * @param count           The length of the iovec array.
   * @param flags           Set of write flags.
   * @param countWritten    On return, the value pointed to by this parameter
   *                          will contain the number of iovec entries that were
   *                          fully written.
   * @param partialWritten  On return, the value pointed to by this parameter
   *                          will contain the number of bytes written in the
   *                          partially written iovec entry.
   *
   * @return Returns a WriteResult. See WriteResult for more details.
   */
  virtual WriteResult performWrite(
      const iovec* vec,
      uint32_t count,
      WriteFlags flags,
      uint32_t* countWritten,
      uint32_t* partialWritten,
      WriteRequestTag writeTag);

  /**
   * Prepares a msghdr and sends the message over the socket using sendmsg
   *
   * @param vec             The iovec array pointing to the buffers to write.
   * @param count           The length of the iovec array.
   * @param flags           Set of write flags.
   */
  virtual AsyncSocket::WriteResult sendSocketMessage(
      const iovec* vec,
      size_t count,
      WriteFlags flags,
      WriteRequestTag writeTag);

  /**
   * Sends the message over the socket using sendmsg
   *
   * @param msg       Message to send
   * @param msg_flags Flags to pass to sendmsg
   */
  virtual AsyncSocket::WriteResult sendSocketMessage(
      NetworkSocket fd, struct msghdr* msg, int msg_flags);

  virtual ssize_t tfoSendMsg(
      NetworkSocket fd, struct msghdr* msg, int msg_flags);

  int socketConnect(const struct sockaddr* addr, socklen_t len);

  virtual void scheduleConnectTimeout();
  void registerForConnectEvents();

  bool updateEventRegistration();

  /**
   * Update event registration.
   *
   * @param enable Flags of events to enable. Set it to 0 if no events
   * need to be enabled in this call.
   * @param disable Flags of events
   * to disable. Set it to 0 if no events need to be disabled in this
   * call.
   *
   * @return true iff the update is successful.
   */
  bool updateEventRegistration(uint16_t enable, uint16_t disable);

  // Attempt to read into one or more `struct iovec`s.  The caller is
  // responsible for setting `msg.msg_iov` and `msg.msg_iovlen` to the
  // buffers that will receive the read, and for initializing
  // `msg.msg_name*`.  In the case that `readAncillaryCallback_` is set, the
  // caller may also want to populate `msg_control`, `msg_controllen`, and
  // `msg_flags` -- if no ancillary data are being read, it's fine to leave
  // them at their defaults of 0.
  virtual ReadResult performReadMsg(
      struct ::msghdr& msg, AsyncReader::ReadCallback::ReadMode);

  // Actually close the file descriptor and set it to -1 so we don't
  // accidentally close it again.
  void doClose();

  // error handling methods
  enum class ReadCode {
    READ_NOT_SUPPORTED = 0,
    READ_CONTINUE = 1,
    READ_DONE = 2,
  };

  void startFail();
  void finishFail();
  void finishFail(const AsyncSocketException& ex);
  void invokeAllErrors(const AsyncSocketException& ex);
  void fail(const char* fn, const AsyncSocketException& ex);
  void failConnect(const char* fn, const AsyncSocketException& ex);
  ReadCode failRead(const char* fn, const AsyncSocketException& ex);
  void failErrMessageRead(const char* fn, const AsyncSocketException& ex);
  void failWrite(
      const char* fn,
      WriteCallback* callback,
      size_t bytesWritten,
      const AsyncSocketException& ex);
  void failWrite(const char* fn, const AsyncSocketException& ex);
  void failAllWrites(const AsyncSocketException& ex);
  void failByteEvents(const AsyncSocketException& ex);
  virtual void invokeConnectErr(const AsyncSocketException& ex);
  virtual void invokeConnectSuccess();
  virtual void invokeConnectAttempt();
  void invalidState(ConnectCallback* callback);
  void invalidState(ErrMessageCallback* callback);
  void invalidState(ReadCallback* callback);
  void invalidState(WriteCallback* callback);

  std::string withAddr(folly::StringPiece s);

  void cacheLocalAddress() const;
  void cachePeerAddress() const;

  bool isZeroCopyRequest(WriteFlags flags);

  bool isZeroCopyMsg(const cmsghdr& cmsg) const;
  void processZeroCopyMsg(const cmsghdr& cmsg);

  uint32_t getNextZeroCopyBufId() { return zeroCopyBufId_++; }
  void adjustZeroCopyFlags(folly::WriteFlags& flags);
  void addZeroCopyBuf(
      std::unique_ptr<folly::IOBuf>&& buf, ReleaseIOBufCallback* cb);
  void addZeroCopyBuf(folly::IOBuf* ptr);
  void setZeroCopyBuf(
      std::unique_ptr<folly::IOBuf>&& buf, ReleaseIOBufCallback* cb);
  bool containsZeroCopyBuf(folly::IOBuf* ptr);
  void releaseZeroCopyBuf(uint32_t id);

  void drainZeroCopyQueue();
  void scheduleZeroCopyDrain();
  void forceReleaseZeroCopyBufs();
  void closeNetworkSocket();

  void releaseIOBuf(
      std::unique_ptr<folly::IOBuf> buf,
      ReleaseIOBufCallback* callback) override;
  void detachIOBuf(const folly::IOBuf& buf) override;

  ReadCode processZeroCopyRead();
  ReadCode processNormalRead();
  /**
   * Attempt to enable Observer ByteEvents for this socket.
   *
   * Once enabled, ByteEvents rename enabled for the socket's life.
   *
   * ByteEvents are delivered to Observers; when an observer is added:
   *    - If this function has already been called, byteEventsEnabled() or
   *      byteEventsUnavailable() will be called, depending on ByteEvent state.
   *    - Else if the socket is connected, this function is called immediately.
   *    - Else if the socket has not yet connected, this function will be called
   *      after the socket has connected (ByteEvents cannot be set up earlier).
   *
   * If ByteEvents are successfully enabled, byteEventsEnabled() will be called
   * on each Observer that has requested ByteEvents. If unable to enable, or if
   * ByteEvents become unavailable (e.g., due to close), byteEventsUnavailable()
   * will be called on each Observer that has requested ByteEvents.
   *
   * This function does need to be explicitly called under other circumstances.
   */
  virtual void enableByteEvents();

  /*
   * IoUringConnectCallback
   */
  void connectSuccess() override;
  void connectTimeout() override;

  /*
   * IoUringSendCallback
   */
  void sendPartial(size_t bytesWritten = 0) override;
  void sendDone(size_t bytesWritten = 0) override;
  void sendErr(int err) override;

  /*
   * IoUringRecvCallback
   */
  void recvSuccess(std::unique_ptr<IOBuf> data) override;
  void recvEOF() noexcept override;
  void recvErr(
      int err,
      std::unique_ptr<const AsyncSocketException> exception) noexcept override;
  void recvBuffersScarce(bool scarce) noexcept override;

  using ZeroCopyEnablePolicy =
      std::variant<std::monostate, AsyncWriter::ZeroCopyEnableFunc, size_t>;
  ZeroCopyEnablePolicy zeroCopyEnablePolicy_;

  // a folly::IOBuf can be used in multiple partial requests
  // there is a that maps a buffer id to a raw folly::IOBuf ptr
  // and another one that adds a ref count for a folly::IOBuf that is either
  // the original ptr or nullptr
  uint32_t zeroCopyBufId_{0};

  ZeroCopyDrainConfig zeroCopyDrainConfig_;

  // Set when the post-close zero-copy drain starts; bounds how long
  // scheduleZeroCopyDrain() may keep rescheduling itself.
  std::chrono::steady_clock::time_point zeroCopyDrainDeadline_{};

  struct IOBufInfo {
    uint32_t count_{0};
    ReleaseIOBufCallback* cb_{nullptr};
    std::unique_ptr<folly::IOBuf> buf_;
  };

  std::unordered_map<uint32_t, folly::IOBuf*> idZeroCopyBufPtrMap_;
  std::unordered_map<folly::IOBuf*, IOBufInfo> idZeroCopyBufInfoMap_;

  StateEnum state_{StateEnum::UNINIT}; ///< StateEnum describing current state
  uint8_t shutdownFlags_{0}; ///< Shutdown state (ShutdownFlags)
  uint16_t eventFlags_; ///< EventBase::HandlerFlags settings
  NetworkSocket fd_; ///< The socket file descriptor
  mutable folly::SocketAddress addr_; ///< The address we tried to connect to
  mutable folly::SocketAddress localAddr_;
  ///< The address we are connecting from
  uint32_t sendTimeout_; ///< The send timeout, in milliseconds
  uint16_t maxReadsPerEvent_; ///< Max reads per event loop iteration

  int8_t readErr_{READ_NO_ERROR}; ///< The read error encountered, if any

  EventBase* eventBase_; ///< The EventBase
  WriteTimeout writeTimeout_; ///< A timeout for connect and write
  IoHandler ioHandler_; ///< A EventHandler to monitor the fd
  ImmediateReadCB immediateReadHandler_; ///< LoopCallback for checking read

  ConnectCallback* connectCallback_; ///< ConnectCallback
  ErrMessageCallback* errMessageCallback_; ///< TimestampCallback
  ReadAncillaryDataCallback*
      readAncillaryDataCallback_; ///< AncillaryDataCallback
  SendMsgParamsCallback* ///< Callback for retrieving
      sendMsgParamCallback_; ///< ::sendmsg() parameters
  ReadCallback* readCallback_; ///< ReadCallback
  WriteRequest* writeReqHead_; ///< Chain of WriteRequests
  WriteRequest* writeReqTail_; ///< End of WriteRequest chain
  std::weak_ptr<ShutdownSocketSet> wShutdownSocketSet_;
  size_t appBytesReceived_; ///< Num of bytes received from socket
  size_t appBytesWritten_{0}; ///< Num of bytes written to socket
  size_t rawBytesWritten_{0}; ///< Num of (raw) bytes written to socket
  // The total num of bytes passed to AsyncSocket's write functions. It doesn't
  // include failed writes, but it does include buffered writes.
  size_t totalAppBytesScheduledForWrite_;
  // Num of bytes allocated in IOBufs pending write.
  size_t allocatedBytesBuffered_{0};

  // Lifecycle observers.
  //
  // Use small_vector to avoid heap allocation for up to two observers, unless
  // mobile, in which case we fallback to std::vector to prioritize code size.
  using LifecycleObserverVecImpl = conditional_t<
      !kIsMobile,
      folly::small_vector<LegacyLifecycleObserver*, 2>,
      std::vector<LegacyLifecycleObserver*>>;
  LifecycleObserverVecImpl lifecycleObservers_;

  // Pre-received data, to be returned to read callback before any data from the
  // socket.
  std::unique_ptr<IOBuf> preReceivedData_;

  // When connect() started.
  std::chrono::steady_clock::time_point connectStartTime_;

  // When connect() completed.
  std::chrono::steady_clock::time_point connectEndTime_;

  // When the connection was established.
  //
  //  -  If connect() was called and succeeded, this is the same as
  //     connectEndTime_.
  //
  //  -  If AsyncSocket was initialized with a file descriptor (e.g., by an
  //     acceptor), this is the connection establishment time passed to the
  //     constructor. If no time was passed, this is folly::none.
  folly::Optional<std::chrono::steady_clock::time_point>
      maybeConnectionEstablishTime_;

  std::chrono::milliseconds connectTimeout_{0};

  std::unique_ptr<EvbChangeCallback> evbChangeCb_{nullptr};

  BufferCallback* bufferCallback_{nullptr};

  struct TCPFastOpenInfo {
    bool attempted{false};
    bool enabled{false};
    bool finished{false};
  };

  TCPFastOpenInfo tfoInfo_;
  bool bindAddressNoPort_{true};
  bool noTransparentTls_{false};
  bool noTSocks_{false};
  // Whether to track EOR or not.
  bool trackEor_{false};
  Optional<int> tosOrTrafficClass_;

  // ByteEvent state
  std::unique_ptr<ByteEventHelper> byteEventHelper_;

  bool zeroCopyEnabled_{false};
  bool zeroCopyVal_{false};
  // zerocopy re-enable logic
  size_t zeroCopyReenableThreshold_{0};
  size_t zeroCopyReenableCounter_{0};

  // zerocopy read
  bool zerocopyReadDisabled_{false};
  int zerocopyReadErr_{0};

  bool closeOnFailedWrite_{true};

  bool useIoUring_{false};
  bool iouRecvHandleDetached_{false};
  IoUringConnectHandle::UniquePtr iouConnectHandle_;
  IoUringSendHandle::UniquePtr iouSendHandle_;
  IoUringRecvHandle::UniquePtr iouRecvHandle_;

  netops::DispatcherContainer netops_;

  folly::TcpInfoDispatcherContainer tcpInfoDispatcher_;

  // Container of observers for the socket / transport.
  //
  // This member MUST be last in the list of members (other than
  // constructorCallbackList_) to ensure it is destroyed first, before any other
  // members are destroyed. This ensures that observers can inspect any socket /
  // transport state available through public methods when destruction of the
  // transport begins.
  AsyncSocketObserverContainer observerContainer_;

  // allow other functions to register for callbacks when
  // new AsyncSocket()'s are created
  // must be LAST member defined to ensure other members are initialized
  // before access; see ConstructorCallbackList.h for details
  ConstructorCallbackList<AsyncSocket> constructorCallbackList_{this};
};

std::ostream& operator<<(
    std::ostream& os, const folly::AsyncSocket::WriteRequestTag& tag);

} // namespace folly

template <>
struct std::hash<folly::AsyncSocket::WriteRequestTag> {
  std::size_t operator()(
      const folly::AsyncSocket::WriteRequestTag& writeTag) const {
    return std::hash<const folly::IOBuf*>{}(writeTag.buf_);
  }
};
