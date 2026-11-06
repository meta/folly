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
#include <memory>

#include <folly/Optional.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufIovecBuilder.h>
#include <folly/io/async/AsyncSocketBase.h>
#include <folly/io/async/AsyncTransportCertificate.h>
#include <folly/io/async/DelayedDestruction.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/WriteFlags.h>
#include <folly/portability/OpenSSL.h>
#include <folly/portability/SysUio.h>
#include <folly/ssl/OpenSSLPtrTypes.h>

namespace folly {

class AsyncSocketException;
class EventBase;
class SocketAddress;

class AsyncReader {
 public:
  class ReadCallback {
   public:
    enum class ReadMode : uint8_t {
      ReadBuffer = 0,
      ReadVec = 1,
      ReadZC = 2,
    };

    virtual ~ReadCallback() = default;

    ReadMode getReadMode() const noexcept { return readMode_; }

    void setReadMode(ReadMode readMode) noexcept { readMode_ = readMode; }

    /**
     * When data becomes available, getReadBuffer()/getReadBuffers() will be
     * invoked to get the buffer/buffers into which data should be read.
     *
     * These methods allows the ReadCallback to delay buffer allocation until
     * data becomes available.  This allows applications to manage large
     * numbers of idle connections, without having to maintain a separate read
     * buffer for each idle connection.
     */

    /**
     * It is possible that in some cases, getReadBuffer() may be called
     * multiple times before readDataAvailable() is invoked.  In this case, the
     * data will be written to the buffer returned from the most recent call to
     * readDataAvailable().  If the previous calls to readDataAvailable()
     * returned different buffers, the ReadCallback is responsible for ensuring
     * that they are not leaked.
     *
     * If getReadBuffer() throws an exception, returns a nullptr buffer, or
     * returns a 0 length, the ReadCallback will be uninstalled and its
     * readError() method will be invoked.
     *
     * getReadBuffer() is not allowed to change the transport state before it
     * returns.  (For example, it should never uninstall the read callback, or
     * set a different read callback.)
     *
     * @param bufReturn getReadBuffer() should update *bufReturn to contain the
     *                  address of the read buffer.  This parameter will never
     *                  be nullptr.
     * @param lenReturn getReadBuffer() should update *lenReturn to contain the
     *                  maximum number of bytes that may be written to the read
     *                  buffer.  This parameter will never be nullptr.
     */
    virtual void getReadBuffer(void** bufReturn, size_t* lenReturn) = 0;

    /**
     * It is possible that in some cases, getReadBuffers() may be called
     * multiple times before readDataAvailable() is invoked.  In this case, the
     * data will be written to the buffer returned from the most recent call to
     * readDataAvailable().  If the previous calls to readDataAvailable()
     * returned different buffers, the ReadCallback is responsible for ensuring
     * that they are not leaked.
     *
     * If getReadBuffers() throws an exception or returns a zero length array
     * the ReadCallback will be uninstalled and its readError() method will be
     * invoked.
     *
     * getReadBuffers() is not allowed to change the transport state before it
     * returns.  (For example, it should never uninstall the read callback, or
     * set a different read callback.)
     *
     * @param iovs      getReadBuffers() will copy up to num iovec entries into
     *                  iovs
     */
    virtual void getReadBuffers(IOBufIovecBuilder::IoVecVec& iovs) {
      iovs.clear();
    }

    /**
     * readDataAvailable() will be invoked when data has been successfuly read
     * into the buffer(s) returned by the last call to
     * getReadBuffer()/getReadBuffers()
     *
     * The read callback remains installed after readDataAvailable() returns.
     * It must be explicitly uninstalled to stop receiving read events.
     * getReadBuffer() will be called at least once before each call to
     * readDataAvailable().  getReadBuffer() will also be called before any
     * call to readEOF().
     *
     * @param len       The number of bytes placed in the buffer.
     */

    virtual void readDataAvailable(size_t len) noexcept = 0;

    class ZeroCopyMemStore {
     public:
      struct Entry {
        void* data{nullptr};
        size_t len{0}; // in use
        size_t capacity{0}; // capacity
        ZeroCopyMemStore* store{nullptr};

        void put() {
          DCHECK(store);
          store->put(this);
        }
      };

      struct EntryDeleter {
        void operator()(Entry* entry) { entry->put(); }
      };

      using EntryPtr = std::unique_ptr<Entry, EntryDeleter>;

      virtual ~ZeroCopyMemStore() = default;

      virtual EntryPtr get() = 0;
      virtual void put(Entry*) = 0;
    };

    /* the next 4 methods can be used if the  callback wants to support zerocopy
     * RX on Linux as described in https://lwn.net/Articles/754681/ If the
     * current kernel version does not support zerocopy RX, the callback will
     * revert to regular recv processing
     * In case we support zerocopy RX, the callback might be notified of buffer
     * chains composed of mmap memory and also memory allocated via the
     * getZeroCopyReadBuffer method
     */

    /**
     * Return a ZeroCopyMemStore to use if the callback would like to enable
     * zero-copy reads.  Return nullptr to disable zero-copy reads.
     *
     * The caller must ensure that the ZeroCopyMemStore remains valid for as
     * long as this callback is installed and reading data, and until put()
     * has been called for every outstanding Entry allocated with get().
     */
    virtual ZeroCopyMemStore* readZeroCopyEnabled() noexcept { return nullptr; }

    /**
     * Get a buffer to read data into when using zero-copy reads if some data
     * cannot be read using a zero-copy page.
     *
     * When data is available, some data may be returned in zero-copy pages,
     * followed by some amount of data in this fallback buffer.
     */
    virtual void getZeroCopyFallbackBuffer(
        void** /*bufReturn*/, size_t* /*lenReturn*/) noexcept {
      CHECK(false);
    }

    /**
     * readZeroCopyDataAvailable() will be called when data is available from a
     * zero-copy read.
     *
     * The data returned may be in two separate parts: data that was actually
     * read using zero copy pages will be in zeroCopyData.  Additionally, some
     * number of bytes may have been placed in the fallback buffer returned by
     * getZeroCopyFallbackBuffer().  additionalBytes indicates the number of
     * bytes placed in getZeroCopyFallbackBuffer().
     */
    virtual void readZeroCopyDataAvailable(
        std::unique_ptr<IOBuf>&& /*zeroCopyData*/,
        size_t /*additionalBytes*/) noexcept {
      CHECK(false);
    }

    /**
     * When data becomes available, isBufferMovable() will be invoked to figure
     * out which API will be used, readBufferAvailable() or
     * readDataAvailable(). If isBufferMovable() returns true, that means
     * ReadCallback supports the IOBuf ownership transfer and
     * readBufferAvailable() will be used.  Otherwise, not.

     * By default, isBufferMovable() always return false. If
     * readBufferAvailable() is implemented and to be invoked, You should
     * overwrite isBufferMovable() and return true in the inherited class.
     *
     * This method allows the AsyncSocket/AsyncSSLSocket do buffer allocation by
     * itself until data becomes available.  Compared with the pre/post buffer
     * allocation in getReadBuffer()/readDataAvailabe(), readBufferAvailable()
     * has two advantages.  First, this can avoid memcpy. E.g., in
     * AsyncSSLSocket, the decrypted data was copied from the openssl internal
     * buffer to the readbuf buffer.  With the buffer ownership transfer, the
     * internal buffer can be directly "moved" to ReadCallback. Second, the
     * memory allocation can be more precise.  The reason is
     * AsyncSocket/AsyncSSLSocket can allocate the memory of precise size
     * because they have more context about the available data than
     * ReadCallback.  Think about the getReadBuffer() pre-allocate 4072 bytes
     * buffer, but the available data is always 16KB (max OpenSSL record size).
     */

    virtual bool isBufferMovable() noexcept { return false; }

    /**
     * Suggested buffer size, allocated for read operations,
     * if callback is movable and supports folly::IOBuf
     */

    virtual size_t maxBufferSize() const {
      return 64 * 1024; // 64K
    }

    /**
     * readBufferAvailable() will be invoked when data has been successfully
     * read.
     *
     * Note that only either readBufferAvailable() or readDataAvailable() will
     * be invoked according to the return value of isBufferMovable(). The timing
     * and aftereffect of readBufferAvailable() are the same as
     * readDataAvailable()
     *
     * @param readBuf The unique pointer of read buffer.
     */

    virtual void readBufferAvailable(
        std::unique_ptr<IOBuf> /*readBuf*/) noexcept {}

    virtual void readBuffersScarce(bool /*scarce*/) noexcept {}

    /**
     * readEOF() will be invoked when the transport is closed.
     *
     * The read callback will be automatically uninstalled immediately before
     * readEOF() is invoked.
     */
    virtual void readEOF() noexcept = 0;

    /**
     * readError() will be invoked if an error occurs reading from the
     * transport.
     *
     * The read callback will be automatically uninstalled immediately before
     * readError() is invoked.
     *
     * @param ex        An exception describing the error that occurred.
     */
    virtual void readErr(const AsyncSocketException& ex) noexcept = 0;

   protected:
    ReadMode readMode_{ReadMode::ReadBuffer};
  };

  // Read methods that aren't part of AsyncTransport.
  virtual void setReadCB(ReadCallback* callback) = 0;
  virtual ReadCallback* getReadCallback() const = 0;
  virtual std::unique_ptr<IOBuf> takePreReceivedData() { return {}; }

 protected:
  virtual ~AsyncReader() = default;
};

class AsyncWriter {
 public:
  class ReleaseIOBufCallback {
   public:
    virtual ~ReleaseIOBufCallback() = default;

    virtual void releaseIOBuf(std::unique_ptr<folly::IOBuf>) noexcept = 0;
  };

  class WriteCallback {
   public:
    virtual ~WriteCallback() = default;

    /**
     * writeStarting() will be invoked right before bytes are written to the
     * socket.
     *
     * This enables the callback implementation to determine the raw (socket)
     * byte offset for the first byte in this write's buffer. This may be
     * different than the number of bytes written at the application layer in
     * the case of TLS and other transformations.
     *
     * Intermediary transport layers should forward this signal.
     */
    virtual void writeStarting() noexcept {}

    /**
     * writeSuccess() will be invoked when all of the data has been
     * successfully written.
     *
     * Note that this mainly signals that the buffer containing the data to
     * write is no longer needed and may be freed or re-used.  It does not
     * guarantee that the data has been fully transmitted to the remote
     * endpoint.  For example, on socket-based transports, writeSuccess() only
     * indicates that the data has been given to the kernel for eventual
     * transmission.
     */
    virtual void writeSuccess() noexcept = 0;

    /**
     * writeError() will be invoked if an error occurs writing the data.
     *
     * @param bytesWritten      The number of bytes that were successfull
     * @param ex                An exception describing the error that occurred.
     */
    virtual void writeErr(
        size_t bytesWritten, const AsyncSocketException& ex) noexcept = 0;

    virtual ReleaseIOBufCallback* getReleaseIOBufCallback() noexcept {
      return nullptr;
    }
  };

  /**
   * If you supply a non-null WriteCallback, exactly one of writeSuccess()
   * or writeErr() will be invoked when the write completes. If you supply
   * the same WriteCallback object for multiple write() calls, it will be
   * invoked exactly once per call. The only way to cancel outstanding
   * write requests is to close the socket (e.g., with closeNow() or
   * shutdownWriteNow()). When closing the socket this way, writeErr() will
   * still be invoked once for each outstanding write operation.
   */
  virtual void write(
      WriteCallback* callback,
      const void* buf,
      size_t bytes,
      WriteFlags flags = WriteFlags::NONE) = 0;

  /**
   * If you supply a non-null WriteCallback, exactly one of writeSuccess()
   * or writeErr() will be invoked when the write completes. If you supply
   * the same WriteCallback object for multiple write() calls, it will be
   * invoked exactly once per call. The only way to cancel outstanding
   * write requests is to close the socket (e.g., with closeNow() or
   * shutdownWriteNow()). When closing the socket this way, writeErr() will
   * still be invoked once for each outstanding write operation.
   */
  virtual void writev(
      WriteCallback* callback,
      const iovec* vec,
      size_t count,
      WriteFlags flags = WriteFlags::NONE) = 0;

  /**
   * If you supply a non-null WriteCallback, exactly one of writeSuccess()
   * or writeErr() will be invoked when the write completes. If you supply
   * the same WriteCallback object for multiple write() calls, it will be
   * invoked exactly once per call. The only way to cancel outstanding
   * write requests is to close the socket (e.g., with closeNow() or
   * shutdownWriteNow()). When closing the socket this way, writeErr() will
   * still be invoked once for each outstanding write operation.
   */
  virtual void writeChain(
      WriteCallback* callback,
      std::unique_ptr<IOBuf>&& buf,
      WriteFlags flags = WriteFlags::NONE) = 0;

  /** zero copy related
   * */
  virtual bool setZeroCopy(bool /*enable*/) { return false; }

  virtual bool getZeroCopy() const { return false; }

  struct RXZerocopyParams {
    bool enable{false};
    size_t mapSize{0};
  };

  [[nodiscard]] virtual bool setRXZeroCopy(RXZerocopyParams /*params*/) {
    return false;
  }

  [[nodiscard]] virtual bool getRXZeroCopy() const { return false; }

  using ZeroCopyEnableFunc =
      std::function<bool(const std::unique_ptr<folly::IOBuf>& buf)>;

  virtual void setZeroCopyEnableFunc(ZeroCopyEnableFunc /*func*/) {}

  virtual void setZeroCopyEnableThreshold(size_t /*threshold*/) {}

 protected:
  virtual ~AsyncWriter() = default;
};

/**
 * AsyncTransport defines an asynchronous API for bidirectional streaming I/O.
 *
 * This class provides an API to for asynchronously waiting for data
 * on a streaming transport, and for asynchronously sending data.
 *
 * The APIs for reading and writing are intentionally asymmetric.  Waiting for
 * data to read is a persistent API: a callback is installed, and is notified
 * whenever new data is available.  It continues to be notified of new events
 * until it is uninstalled.
 *
 * AsyncTransport does not provide read timeout functionality, because it
 * typically cannot determine when the timeout should be active.  Generally, a
 * timeout should only be enabled when processing is blocked waiting on data
 * from the remote endpoint.  For server-side applications, the timeout should
 * not be active if the server is currently processing one or more outstanding
 * requests on this transport.  For client-side applications, the timeout
 * should not be active if there are no requests pending on the transport.
 * Additionally, if a client has multiple pending requests, it will ususally
 * want a separate timeout for each request, rather than a single read timeout.
 *
 * The write API is fairly intuitive: a user can request to send a block of
 * data, and a callback will be informed once the entire block has been
 * transferred to the kernel, or on error.  AsyncTransport does provide a send
 * timeout, since most callers want to give up if the remote end stops
 * responding and no further progress can be made sending the data.
 */
class AsyncTransport
    : public DelayedDestruction,
      public AsyncSocketBase,
      public AsyncReader,
      public AsyncWriter {
 public:
  using UniquePtr = std::unique_ptr<AsyncTransport, Destructor>;

  /**
   * Close the transport.
   *
   * This gracefully closes the transport, waiting for all pending write
   * requests to complete before actually closing the underlying transport.
   *
   * If a read callback is set, readEOF() will be called immediately.  If there
   * are outstanding write requests, the close will be delayed until all
   * remaining writes have completed.  No new writes may be started after
   * close() has been called.
   */
  virtual void close() = 0;

  /**
   * Close the transport immediately.
   *
   * This closes the transport immediately, dropping any outstanding data
   * waiting to be written.
   *
   * If a read callback is set, readEOF() will be called immediately.
   * If there are outstanding write requests, these requests will be aborted
   * and writeError() will be invoked immediately on all outstanding write
   * callbacks.
   */
  virtual void closeNow() = 0;

  /**
   * Reset the transport immediately.
   *
   * This closes the transport immediately, sending a reset to the remote peer
   * if possible to indicate abnormal shutdown.
   *
   * Note that not all subclasses implement this reset functionality: some
   * subclasses may treat reset() the same as closeNow().  Subclasses that use
   * TCP transports should terminate the connection with a TCP reset.
   */
  virtual void closeWithReset() { closeNow(); }

  /**
   * Perform a half-shutdown of the write side of the transport.
   *
   * The caller should not make any more calls to write() or writev() after
   * shutdownWrite() is called.  Any future write attempts will fail
   * immediately.
   *
   * Not all transport types support half-shutdown.  If the underlying
   * transport does not support half-shutdown, it will fully shutdown both the
   * read and write sides of the transport.  (Fully shutting down the socket is
   * better than doing nothing at all, since the caller may rely on the
   * shutdownWrite() call to notify the other end of the connection that no
   * more data can be read.)
   *
   * If there is pending data still waiting to be written on the transport,
   * the actual shutdown will be delayed until the pending data has been
   * written.
   *
   * Note: There is no corresponding shutdownRead() equivalent.  Simply
   * uninstall the read callback if you wish to stop reading.  (On TCP sockets
   * at least, shutting down the read side of the socket is a no-op anyway.)
   */
  virtual void shutdownWrite() = 0;

  /**
   * Perform a half-shutdown of the write side of the transport.
   *
   * shutdownWriteNow() is identical to shutdownWrite(), except that it
   * immediately performs the shutdown, rather than waiting for pending writes
   * to complete.  Any pending write requests will be immediately failed when
   * shutdownWriteNow() is called.
   */
  virtual void shutdownWriteNow() = 0;

  /**
   * Determine if transport is open and ready to read or write.
   *
   * Note that this function returns false on EOF; you must also call error()
   * to distinguish between an EOF and an error.
   *
   * @return  true iff the transport is open and ready, false otherwise.
   */
  virtual bool good() const = 0;

  /**
   * Determine if the transport is readable or not.
   *
   * @return  true iff the transport is readable, false otherwise.
   */
  virtual bool readable() const = 0;

  /**
   * Determine if the transport is writable or not.
   *
   * @return  true iff the transport is writable, false otherwise.
   */
  virtual bool writable() const {
    // By default return good() - leave it to implementers to override.
    return good();
  }

  /**
   * Determine if the there is pending data on the transport.
   *
   * @return  true iff the if the there is pending data, false otherwise.
   */
  virtual bool isPending() const { return readable(); }

  /**
   * Determine if transport is connected to the endpoint
   *
   * @return  false iff the transport is connected, otherwise true
   */
  virtual bool connecting() const = 0;

  /**
   * Determine if an error has occurred with this transport.
   *
   * @return  true iff an error has occurred (not EOF).
   */
  virtual bool error() const = 0;

  /**
   * Attach the transport to a EventBase.
   *
   * This may only be called if the transport is not currently attached to a
   * EventBase (by an earlier call to detachEventBase()).
   *
   * This method must be invoked in the EventBase's thread.
   */
  virtual void attachEventBase(EventBase* eventBase) = 0;

  /**
   * Detach the transport from its EventBase.
   *
   * This may only be called when the transport is idle and has no reads or
   * writes pending.  Once detached, the transport may not be used again until
   * it is re-attached to a EventBase by calling attachEventBase().
   *
   * This method must be called from the current EventBase's thread.
   */
  virtual void detachEventBase() = 0;

  /**
   * Determine if the transport can be detached.
   *
   * This method must be called from the current EventBase's thread.
   */
  virtual bool isDetachable() const = 0;

  /**
   * Set the send timeout.
   *
   * If write requests do not make any progress for more than the specified
   * number of milliseconds, fail all pending writes and close the transport.
   *
   * If write requests are currently pending when setSendTimeout() is called,
   * the timeout interval is immediately restarted using the new value.
   *
   * @param milliseconds  The timeout duration, in milliseconds.  If 0, no
   *                      timeout will be used.
   */
  virtual void setSendTimeout(uint32_t milliseconds) = 0;

  /**
   * Get the send timeout.
   *
   * @return Returns the current send timeout, in milliseconds.  A return value
   *         of 0 indicates that no timeout is set.
   */
  virtual uint32_t getSendTimeout() const = 0;

  /**
   * Get the address of the local endpoint of this transport.
   *
   * This function may throw AsyncSocketException on error.
   *
   * @param address  The local address will be stored in the specified
   *                 SocketAddress.
   */
  virtual void getLocalAddress(SocketAddress* address) const = 0;

  /**
   * Get the address of the remote endpoint to which this transport is
   * connected.
   *
   * This function may throw AsyncSocketException on error.
   *
   * @return         Return the local address
   */
  SocketAddress getLocalAddress() const {
    SocketAddress addr;
    getLocalAddress(&addr);
    return addr;
  }

  void getAddress(SocketAddress* address) const override {
    getLocalAddress(address);
  }

  /**
   * Get the address of the remote endpoint to which this transport is
   * connected.
   *
   * This function may throw AsyncSocketException on error.
   *
   * @param address  The remote endpoint's address will be stored in the
   *                 specified SocketAddress.
   */
  virtual void getPeerAddress(SocketAddress* address) const = 0;

  /**
   * Get the address of the remote endpoint to which this transport is
   * connected.
   *
   * This function may throw AsyncSocketException on error.
   *
   * @return         Return the remote endpoint's address
   */
  SocketAddress getPeerAddress() const {
    SocketAddress addr;
    getPeerAddress(&addr);
    return addr;
  }

  /**
   * Get the peer certificate information if any
   */
  virtual const AsyncTransportCertificate* getPeerCertificate() const {
    return nullptr;
  }

  /**
   * Hints to transport implementations that the associated certificate is no
   * longer required by the application. The transport implementation may
   * choose to free up resources associated with the peer certificate.
   *
   * After this call, `getPeerCertificate()` may return nullptr, even if it
   * previously returned non-null
   */
  virtual void dropPeerCertificate() noexcept {}

  /**
   * Hints to transport implementations that the associated certificate is no
   * longer required by the application. The transport implementation may
   * choose to free up resources associated with the self certificate.
   *
   * After this call, `getPeerCertificate()` may return nullptr, even if it
   * previously returned non-null
   */
  virtual void dropSelfCertificate() noexcept {}

  /**
   * Get the certificate information of this transport, if any
   */
  virtual const AsyncTransportCertificate* getSelfCertificate() const {
    return nullptr;
  }

  /**
   * Return the application protocol being used by the underlying transport
   * protocol. This is useful for transports which are used to tunnel other
   * protocols.
   */
  virtual std::string getApplicationProtocol() const noexcept { return ""; }

  /**
   * Returns the name of the security protocol being used.
   */
  virtual std::string getSecurityProtocol() const { return ""; }

  /*
   * A transport may be able to produce exported keying material (ekm, per
   * rfc5705), that can be used to bind some arbitrary data to it. This can be
   * useful in contexts where you may want a token to only be used on the
   * transport it was created for. If the transport is incapable of producing
   * the ekm, this should return nullptr.
   */
  virtual std::unique_ptr<IOBuf> getExportedKeyingMaterial(
      folly::StringPiece /* label */,
      std::unique_ptr<IOBuf> /* context */,
      uint16_t /* length */) const {
    return nullptr;
  }

  /**
   * @return True iff end of record tracking is enabled
   */
  virtual bool isEorTrackingEnabled() const = 0;

  virtual void setEorTracking(bool track) = 0;

  virtual size_t getAppBytesWritten() const = 0;
  virtual size_t getRawBytesWritten() const = 0;
  virtual size_t getAppBytesReceived() const = 0;
  virtual size_t getRawBytesReceived() const = 0;

  /**
   * Calculates the total number of bytes that are currently buffered in the
   * transport to be written later.
   */
  virtual size_t getAppBytesBuffered() const { return 0; }
  virtual size_t getRawBytesBuffered() const { return 0; }
  virtual size_t getAllocatedBytesBuffered() const { return 0; }

  /**
   * Callback class to signal changes in the transport's internal buffers.
   */
  class BufferCallback {
   public:
    virtual ~BufferCallback() = default;

    /**
     * onEgressBuffered() will be invoked when there's a partial write and it
     * is necessary to buffer the remaining data.
     */
    virtual void onEgressBuffered() = 0;

    /**
     * onEgressBufferCleared() will be invoked when whatever was buffered is
     * written, or when it errors out.
     */
    virtual void onEgressBufferCleared() = 0;
  };

  /**
   * Callback class to signal when a transport that did not have replay
   * protection gains replay protection. This is needed for 0-RTT security
   * protocols.
   */
  class ReplaySafetyCallback {
   public:
    virtual ~ReplaySafetyCallback() = default;

    /**
     * Called when the transport becomes replay safe.
     */
    virtual void onReplaySafe() = 0;
  };

  /**
   * False if the transport does not have replay protection, but will in the
   * future.
   */
  virtual bool isReplaySafe() const { return true; }

  /**
   * Set the ReplaySafeCallback on this transport.
   *
   * This should only be called if isReplaySafe() returns false.
   */
  virtual void setReplaySafetyCallback(ReplaySafetyCallback* callback) {
    if (callback) {
      CHECK(false) << "setReplaySafetyCallback() not supported";
    }
  }

  /**
   * Return SO_INCOMING_NAPI_ID for this transport. For socket transports, this
   * is associated with the NAPI instance/receive queue. For other transports,
   * it is not defined.
   *
   * Returns -1 for error or invalid NAPI ID, or a positive integer for a valid
   * NAPI ID.
   */
  virtual int getNapiId() const { return -1; }

 public:
  /**
   * AsyncTransports may wrap other AsyncTransport. This returns the
   * transport that is wrapped. It returns nullptr if there is no wrapped
   * transport.
   */
  virtual const AsyncTransport* getWrappedTransport() const { return nullptr; }

  /**
   * In many cases when we need to set socket properties or otherwise access the
   * underlying transport from a wrapped transport. This method allows access to
   * the derived classes of the underlying transport.
   */
  template <class T>
  const T* getUnderlyingTransport() const {
    const AsyncTransport* current = this;
    while (current) {
      auto sock = dynamic_cast<const T*>(current);
      if (sock) {
        return sock;
      }
      current = current->getWrappedTransport();
    }
    return nullptr;
  }

  template <class T>
  T* getUnderlyingTransport() {
    return const_cast<T*>(
        static_cast<const AsyncTransport*>(this)->getUnderlyingTransport<T>());
  }

  virtual AsyncTransport::UniquePtr tryExchangeWrappedTransport(
      AsyncTransport::UniquePtr& /* transport */) {
    return AsyncTransport::UniquePtr{};
  }

  template <class T>
  typename T::UniquePtr tryExchangeUnderlyingTransport(
      AsyncTransport::UniquePtr& p) {
    AsyncTransport const* current = getWrappedTransport();
    AsyncTransport const* last = this;
    while (current) {
      if (dynamic_cast<T const*>(current)) {
        AsyncTransport::UniquePtr ret =
            const_cast<AsyncTransport*>(last)->tryExchangeWrappedTransport(p);
        ret->setReadCB(nullptr);
        DCHECK_NE(dynamic_cast<T*>(ret.get()), nullptr);
        return typename T::UniquePtr(static_cast<T*>(ret.release()));
      }
      last = current;
      current = current->getWrappedTransport();
    }
    return nullptr;
  }

  /**
   * Returns a const pointer to wrapping or decorating transport of type T.
   *
   * If this transport object is not wrapped or decorated by a transport of type
   * T, returns nullptr. If this transport is wrapped or decorated multiple
   * times by such a type, returns the first occurrence.
   */
  template <class T>
  const T* getWrappingTransport() const {
    const AsyncTransport* current = this;
    while (current) {
      auto wrapped = dynamic_cast<const T*>(current);
      if (wrapped) {
        return wrapped;
      }
      current = current->decoratingTransport_;
    }
    return nullptr;
  }

  /**
   * Returns a pointer to wrapping or decorating transport of type T.
   *
   * If this transport object is not wrapped or decorated by a transport of type
   * T, returns nullptr. If this transport is wrapped or decorated multiple
   * times by such a type, returns the first occurrence.
   */
  template <class T>
  T* getWrappingTransport() {
    return const_cast<T*>(
        static_cast<const AsyncTransport*>(this)->getWrappingTransport<T>());
  }

 protected:
  ~AsyncTransport() override = default;

 private:
  template <class T>
  friend class DecoratedAsyncTransportWrapper;

  // Transports can be wrapped through inheritence or through a decorator such
  // as DecoratedAsyncTransportWrapper, in which case the wrapped transport is
  // a member field of the decorating transport.
  //
  // When wrapped by a decorator, this field holds a pointer to the decorating
  // transport. When not supported, this field is nullptr.
  AsyncTransport* decoratingTransport_{nullptr};
};

using AsyncTransportWrapper = AsyncTransport;
} // namespace folly
