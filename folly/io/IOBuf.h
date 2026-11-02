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

//
// Docs: https://fburl.com/fbcref_iobuf
//

#pragma once

#include <atomic>
#include <cassert>
#include <cinttypes>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>

#include <glog/logging.h>

#include <folly/FBString.h>
#include <folly/FBVector.h>
#include <folly/Function.h>
#include <folly/Portability.h>
#include <folly/Range.h>
#include <folly/detail/Iterators.h>
#include <folly/lang/CheckedMath.h>
#include <folly/lang/Ordering.h>
#include <folly/memory/MemoryResource.h>
#include <folly/portability/SysUio.h>
#include <folly/synchronization/MicroSpinLock.h>

FOLLY_PUSH_WARNING
// Ignore shadowing warnings within this file, so includers can use -Wshadow.
FOLLY_GNU_DISABLE_WARNING("-Wshadow")
// Some compilers break on -Wdocumentation. Not all compilers recognise that
// option, so we also suppress -Wpragmas
FOLLY_GNU_DISABLE_WARNING("-Wpragmas")
// Ignore documentation warnings, to enable overloads to share documentation
// with differing parameters
FOLLY_GNU_DISABLE_WARNING("-Wdocumentation")

namespace std::pmr {
class memory_resource;
}

namespace folly {

namespace detail {
// Is T a unique_ptr<> to a standard-layout type?
template <typename T>
struct IsUniquePtrToSL : std::false_type {};
template <typename T, typename D>
struct IsUniquePtrToSL<std::unique_ptr<T, D>> : std::is_standard_layout<T> {};
} // namespace detail

/**
 * IOBuf manages heap-allocated byte buffers.
 *
 * API Details
 * -----------
 *
 *  - The buffer is not necessarily full of meaningful bytes - there may be
 *    uninitialized bytes before and after the central "valid" range of data.
 *  - Buffers are refcounted, and can be shared by multiple IOBuf objects.
 *    - If you ever write to an IOBuf, first use unshare() to get a unique copy.
 *  - IOBufs can be "chained" in a circularly linked list.
 *    - Use coalesce() to turn an IOBuf chain into a single IOBuf.
 *  - IOBufs are not synchronized. The user is responsible for synchronization.
 *    Notes:
 *    - Like a shared_ptr, the refcounting is atomic.
 *    - const IOBuf methods do not mutate any state, so can safely be called
 *      concurrently with each other, as expected.
 *  - IOBufs are typically stored on the heap, so that they can be used in
 *    chains.
 *
 *
 * Data Layout
 * -----------
 *
 * IOBuf objects contains a pointer to the buffer and information about which
 * segment of the buffer contains valid data.
 *
 *      +-------+
 *      | IOBuf |
 *      +-------+
 *       /
 *      |            |----- length() -----|
 *      v
 *      +------------+--------------------+-----------+
 *      | headroom   |        data        |  tailroom |
 *      +------------+--------------------+-----------+
 *      ^            ^                    ^           ^
 *      buffer()   data()               tail()      bufferEnd()
 *
 *      |----------------- capacity() ----------------|
 *
 *
 * Buffer Sharing
 * --------------
 *
 * Each buffer is reference counted, and multiple IOBuf objects may point
 * to the same buffer.  Each IOBuf may point to a different section of valid
 * data within the underlying buffer.  For example, if multiple protocol
 * requests are read from the network into a single buffer, a separate IOBuf
 * may be created for each request, all sharing the same underlying buffer.
 *
 * In other words, when multiple IOBufs share the same underlying buffer, the
 * data() and tail() methods on each IOBuf may point to a different segment of
 * the data.  However, the buffer() and bufferEnd() methods will point to the
 * same location for all IOBufs sharing the same underlying buffer, unless the
 * tail was resized by trimWritableTail() or maybeSplitTail().
 *
 *           +-----------+     +---------+
 *           |  IOBuf 1  |     | IOBuf 2 |
 *           +-----------+     +---------+
 *            |         | _____/        |
 *       data |    tail |/    data      | tail
 *            v         v               v
 *      +-------------------------------------+
 *      |     |         |               |     |
 *      +-------------------------------------+
 *
 * If you only read data from an IOBuf, you don't need to worry about other
 * IOBuf objects possibly sharing the same underlying buffer.  However, if you
 * ever write to the buffer you need to first ensure that no other IOBufs point
 * to the same buffer.  The unshare() method may be used to ensure that you
 * have an unshared buffer.
 *
 *
 * IOBuf Chains
 * ------------
 *
 * IOBuf objects also contain pointers to next and previous IOBuf objects.
 * This can be used to represent a single logical piece of data that is stored
 * in non-contiguous chunks in separate buffers.
 *
 *     +---------------------------------------------------------------+
 *     |                                                               |
 *     |    +-----------+        +-----------+        +-----------+    |
 *     +--> |  IOBuf 1  | -----> |  IOBuf 2  | -----> |  IOBuf 3  | ---+
 *          +-----------+        +-----------+        +-----------+
 *            |        | _________/     |           ___/        \__
 *            |        |/               |          /               \
 *            v        v                v         v                 v
 *      +-------------------------------------+   +-----------------+
 *      |     |        |                |     |   |                 |
 *      +-------------------------------------+   +-----------------+
 *
 * A single IOBuf object can only belong to one chain at a time.
 *
 * IOBuf chains are always circular.  The "prev" pointer in the head of the
 * chain points to the tail of the chain.  However, it is up to the user to
 * decide which IOBuf is the head.  Internally the IOBuf code does not care
 * which element is the head.
 *
 * The lifetime of all IOBufs in the chain are linked: when one element in the
 * chain is deleted, all other chained elements are also deleted.  Conceptually
 * it is simplest to treat this as if the head of the chain owns all other
 * IOBufs in the chain.  When you delete the head of the chain, it will delete
 * the other elements as well.  For this reason, appendToChain() and
 * insertAfterThisOne() take ownership of the new elements being added to this
 * chain.
 *
 * When the coalesce() method is used to coalesce an entire IOBuf chain into a
 * single IOBuf, all other IOBufs in the chain are eliminated and automatically
 * deleted.  The unshare() method may coalesce the chain; if it does it will
 * similarly delete all IOBufs eliminated from the chain.
 *
 * As discussed in the following section, it is up to the user to maintain a
 * lock around the entire IOBuf chain if multiple threads need to access the
 * chain.  IOBuf does not provide any internal locking.
 *
 *
 * Synchronization
 * ---------------
 *
 * When used in multithread programs, a single IOBuf object should only be
 * accessed mutably by a single thread at a time.  All const member functions of
 * IOBuf are safe to call concurrently with one another, but when a caller uses
 * a single IOBuf across multiple threads and at least one thread calls a
 * non-const member function, the caller is responsible for using an external
 * lock to synchronize access to the IOBuf.
 *
 * Two separate IOBuf objects may be accessed concurrently in separate threads
 * without locking, even if they point to the same underlying buffer.  The
 * buffer reference count is always accessed atomically, and no other
 * operations should affect other IOBufs that point to the same data segment.
 * The caller is responsible for using unshare() to ensure that the data buffer
 * is not shared by other IOBufs before writing to it, and this ensures that
 * the data itself is not modified in one thread while also being accessed from
 * another thread.
 *
 * For IOBuf chains, no two IOBufs in the same chain should be accessed
 * simultaneously in separate threads, except where all simultaneous accesses
 * are to const member functions.  The caller must maintain a lock around the
 * entire chain if the chain, or individual IOBufs in the chain, may be accessed
 * by multiple threads with at least one of the threads needing to mutate.
 *
 *
 * IOBuf Object Allocation
 * -----------------------
 *
 * IOBuf objects themselves exist separately from the data buffer they point
 * to.  Therefore one must also consider how to allocate and manage the IOBuf
 * objects. Typically, IOBufs are allocated on the heap.
 *
 *      +--------------+
 *      |  unique_ptr  |
 *      +--------------+
 *        |
 *        v
 *      +---------+
 *      |  IOBuf  |
 *      +---------+
 *        |
 *        v
 *      +----------+
 *      |  buffer  |
 *      +----------+
 *
 *
 * It is more common to allocate IOBuf objects on the heap, using the create(),
 * takeOwnership(), or wrapBuffer() factory functions.  The clone()/cloneOne()
 * functions also return new heap-allocated IOBufs.  The createCombined()
 * function allocates the IOBuf object and data storage space together, in a
 * single memory allocation.  This can improve performance, particularly if you
 * know that the data buffer and the IOBuf itself will have similar lifetimes.
 *
 * That said, it is also possible to allocate IOBufs on the stack or inline
 * inside another object as well.  This is useful for cases where the IOBuf is
 * short-lived, or when the overhead of allocating the IOBuf on the heap is
 * undesirable.
 *
 * However, note that stack-allocated IOBufs may only be used as the head of a
 * chain (or standalone as the only IOBuf in a chain).  All non-head members of
 * an IOBuf chain must be heap allocated.  (All functions to add nodes to a
 * chain require a std::unique_ptr<IOBuf>, which enforces this requirement.)
 *
 * Copying IOBufs is only meaningful for the head of a chain. The entire chain
 * is cloned; the IOBufs will become shared, and the old and new IOBufs will
 * refer to the same underlying memory.
 *
 *
 * IOBuf Sharing
 * -------------
 *
 * The IOBuf class manages sharing of the underlying buffer that it points to,
 * maintaining a reference count if multiple IOBufs are pointing at the same
 * buffer.
 *
 * However, it is the callers responsibility to manage sharing and ownership of
 * IOBuf objects themselves.  The IOBuf structure does not provide room for an
 * intrusive refcount on the IOBuf object itself, only the underlying data
 * buffer is reference counted.  If users want to share the same IOBuf object
 * between multiple parts of the code, they are responsible for managing this
 * sharing on their own.  (For example, by using a shared_ptr.  Alternatively,
 * users always have the option of using clone() to create a second IOBuf that
 * points to the same underlying buffer.)
 *
 *
 * Inspiration
 * -----------
 *
 * IOBuf objects are intended to be used primarily for networking code, and are
 * modelled somewhat after FreeBSD's mbuf data structure, and Linux's sk_buff
 * structure.
 *
 * IOBuf objects facilitate zero-copy network programming, by allowing multiple
 * IOBuf objects to point to the same underlying buffer of data, using a
 * reference count to track when the buffer is no longer needed and can be
 * freed.
 *
 *
 * @refcode folly/docs/examples/folly/io/IOBuf.cpp
 */
class IOBuf {
 public:
  class Iterator;

  enum CreateOp { CREATE };
  enum WrapBufferOp { WRAP_BUFFER };
  enum TakeOwnershipOp { TAKE_OWNERSHIP };
  enum CopyBufferOp { COPY_BUFFER };
  enum SizedFree { SIZED_FREE };

  enum class CombinedOption { DEFAULT, COMBINED, SEPARATE };

  using value_type = ByteRange;
  using iterator = Iterator;
  using const_iterator = Iterator;

  using FreeFunction = void (*)(void* buf, void* userData);

  /**
   * Create an IOBuf with the requested capacity.
   *
   * @param capacity  The size of buffer to allocate
   *
   * @post  data() points to the start of the buffer
   * @post  length() == 0
   * @post  capacity() >= capacity (@see goodSize for details on why IOBuf
   *        sometimes allocates a larger buffer than requested)
   *
   * @throws std::bad_alloc on malloc failure
   */
  IOBuf(CreateOp, std::size_t capacity);

  /**
   * @copydoc IOBuf(CreateOp, std::size_t)
   * @returns  A unique_ptr to a newly-constructed IOBuf
   * @methodset Makers
   */
  static std::unique_ptr<IOBuf> create(std::size_t capacity);

  /**
   * Create an IOBuf, allocated alongside its buffer.
   *
   * This method uses a single memory allocation to allocate space for both the
   * IOBuf object and the data storage space. This saves one memory allocation.
   *
   * This can be wasteful if the IOBuf and the buffer have different lifetimes.
   * The memory will not be reclaimed until both objects are destroyed. This can
   * happen, for example, if the buffer is grown using reserve().
   *
   * @copydetails IOBuf(CreateOp, std::size_t)
   * @methodset Makers
   */
  static std::unique_ptr<IOBuf> createCombined(std::size_t capacity);

  /**
   * Create an IOBuf, allocated separately from its buffer.
   *
   * IOBuf::create() doesn't necessarily perform separate allocations if the
   * buffer is small. This function forces the IOBuf and its buffer to be
   * allocated separately. This can save space if you know that the buffer will
   * be reallocated.
   *
   * @copydetails IOBuf(CreateOp, std::size_t)
   * @methodset Makers
   */
  static std::unique_ptr<IOBuf> createSeparate(std::size_t capacity);

  /**
   * Create a new IOBuf chain.
   *
   * @param totalCapacity  The total buffer size of all IOBufs in the chain
   * @param maxBufCapacity  The maximum buffer size of each IOBuf in the chain
   *
   * @post  computeChainCapacity() >= totalCapacity
   *
   * Note: Some malloc implementations will internally round up an allocation
   * size to a convenient amount (e.g. jemalloc(31) will actually give you a
   * slab of size 32). Your buffer size could actually be rounded up to
   * `goodMallocSize(maxBufCapacity)`.
   *
   * @methodset Makers
   */
  static std::unique_ptr<IOBuf> createChain(
      size_t totalCapacity, std::size_t maxBufCapacity);

  /**
   * Get a good malloc size.
   *
   * Some malloc implementations will internally round up an allocation size to
   * a convenient amount. For example, jemalloc(31) will actually return a
   * buffer of size 32. Instead of wasting such tailroom, use it.
   *
   * @param minCapacity  The malloc size to round up
   * @param combined  Here be dragons. T154812262. The default value of DEFAULT
   *                  is (a) hard to explain, and (b) probably not what you
   *                  want. Refer to the code to see why.
   *
   * @returns  A value at least as large as minCapacity. The overage, if any,
   *           depends on the allocator.
   *
   * Note that IOBufs do this up-sizing for you: they will round up to the full
   * allocation size and make that capacity available to you without your using
   * this function. This just lets you introspect into that process, so you can
   * for example figure out whether a given IOBuf can be usefully compacted.
   *
   * @methodset Memory
   */
  static size_t goodSize(
      size_t minCapacity, CombinedOption combined = CombinedOption::DEFAULT);

  /**
   * Create an IOBuf by taking ownership of an existing buffer.
   *
   * The IOBuf will assume ownership of the buffer, and free it by calling the
   * specified FreeFunction when the last IOBuf pointing to this buffer is
   * destroyed.
   *    - The FreeFunction will be called like freeFn(buf, userData)
   *    - freeFn must not throw an exception
   *    - If no freeFn is specified, then the buffer will be freed using free().
   *      Note that this is UB if the buffer was allocated using `new`.
   *
   * @param buf  The pointer to the buffer
   * @param capacity  The size of the buffer
   * @param offset  The position within the buffer at which the data begins; for
   *                overloads without this parameter, it defaults to 0
   * @param length  The amount of data already in buf; for overloads without
   *                this parameter, it defaults to capacity
   * @param freeFn  The function to call when buf is to be freed
   * @param userData  An additional arbitrary void* argument to supply to freeFn
   * @param freeOnError  Whether the buffer should be freed if this function
   *                     throws an exception
   * @param SizedFree  For overloads specified by this enum type, use
   *                   io_buf_free_cn(buf, capacity) as the freeFn
   *
   * @post  data() points to buf+offset (in overloads without offset, offset
   *        defaults to 0)
   * @post  length() == length (in overloads without length, length defaults to
   *        capacity)
   *
   * @throws std::bad_alloc on error
   *
   * @note  If length is unspecified, it defaults to capacity, as opposed to
   *        empty.
   * @note  freeOnError is not properly handled in all cases. T154815366
   */
  IOBuf(
      TakeOwnershipOp op,
      void* buf,
      std::size_t capacity,
      FreeFunction freeFn = nullptr,
      void* userData = nullptr,
      bool freeOnError = true)
      : IOBuf(op, buf, capacity, 0, capacity, freeFn, userData, freeOnError) {}

  /**
   * @copydoc IOBuf(TakeOwnershipOp, void*, std::size_t, FreeFunction, void*,
   *          bool)
   * @returns  A unique_ptr to a newly-constructed IOBuf
   * @methodset Makers
   */
  static std::unique_ptr<IOBuf> takeOwnership(
      void* buf,
      std::size_t capacity,
      FreeFunction freeFn = nullptr,
      void* userData = nullptr,
      bool freeOnError = true) {
    return takeOwnershipImpl(
        buf,
        capacity,
        0,
        capacity,
        freeFn,
        userData,
        freeOnError,
        TakeOwnershipOption::DEFAULT);
  }

  /**
   * @copydoc IOBuf(TakeOwnershipOp, void*, std::size_t, FreeFunction, void*,
   *          bool)
   * @returns  A unique_ptr to a newly-constructed IOBuf
   * @methodset Makers
   */
  static std::unique_ptr<IOBuf> takeOwnership(
      void* buf,
      std::size_t capacity,
      std::size_t length,
      FreeFunction freeFn = nullptr,
      void* userData = nullptr,
      bool freeOnError = true) {
    return takeOwnershipImpl(
        buf,
        capacity,
        0,
        length,
        freeFn,
        userData,
        freeOnError,
        TakeOwnershipOption::DEFAULT);
  }

  /// @copydoc IOBuf(TakeOwnershipOp, void*, std::size_t, FreeFunction, void*,
  /// bool)
  IOBuf(
      TakeOwnershipOp op,
      void* buf,
      std::size_t capacity,
      std::size_t length,
      FreeFunction freeFn = nullptr,
      void* userData = nullptr,
      bool freeOnError = true)
      : IOBuf(op, buf, capacity, 0, length, freeFn, userData, freeOnError) {}

  /**
   * @copydoc IOBuf(TakeOwnershipOp, void*, std::size_t, FreeFunction, void*,
   *          bool)
   * @returns  A unique_ptr to a newly-constructed IOBuf
   * @methodset Makers
   */
  static std::unique_ptr<IOBuf> takeOwnership(
      void* buf,
      std::size_t capacity,
      std::size_t offset,
      std::size_t length,
      FreeFunction freeFn = nullptr,
      void* userData = nullptr,
      bool freeOnError = true) {
    return takeOwnershipImpl(
        buf,
        capacity,
        offset,
        length,
        freeFn,
        userData,
        freeOnError,
        TakeOwnershipOption::DEFAULT);
  }

  /**
   * @copydoc IOBuf(TakeOwnershipOp, void*, std::size_t, FreeFunction, void*,
   *          bool)
   * @returns  A unique_ptr to a newly-constructed IOBuf
   * @methodset Makers
   */
  static std::unique_ptr<IOBuf> takeOwnership(
      SizedFree,
      void* buf,
      std::size_t capacity,
      std::size_t offset,
      std::size_t length,
      bool freeOnError = true) {
    return takeOwnershipImpl(
        buf,
        capacity,
        offset,
        length,
        nullptr,
        reinterpret_cast<void*>(capacity),
        freeOnError,
        TakeOwnershipOption::STORE_SIZE);
  }

  /// @copydoc IOBuf(TakeOwnershipOp, void*, std::size_t, FreeFunction, void*,
  /// bool)
  IOBuf(
      TakeOwnershipOp,
      void* buf,
      std::size_t capacity,
      std::size_t offset,
      std::size_t length,
      FreeFunction freeFn = nullptr,
      void* userData = nullptr,
      bool freeOnError = true);

  /// @copydoc IOBuf(TakeOwnershipOp, void*, std::size_t, FreeFunction, void*,
  /// bool)
  IOBuf(
      TakeOwnershipOp,
      SizedFree,
      void* buf,
      std::size_t capacity,
      std::size_t offset,
      std::size_t length,
      bool freeOnError = true);

  /**
   * Create an IOBuf with a reinterpreted buffer.
   *
   * Create a new IOBuf pointing to an existing data buffer made up of
   * count objects of a given standard-layout type.
   *
   * This is dangerous -- it is essentially equivalent to doing
   * reinterpret_cast<unsigned char*> on your data -- but it's often useful
   * for serialization / deserialization.
   *
   * The new IOBuf will assume ownership of the buffer, and free it
   * appropriately (by calling the UniquePtr's custom deleter, or by calling
   * delete or delete[] appropriately if there is no custom deleter)
   * when the buffer is destroyed.  The custom deleter, if any, must never
   * throw exceptions.
   *
   * The IOBuf data pointer will initially point to the start of the buffer,
   * and the length will be the full capacity of the buffer (count *
   * sizeof(T)).
   *
   * On error, std::bad_alloc will be thrown, and the buffer will be freed
   * before throwing the error.
   *
   * @param buf  The unique_ptr to the buffer
   * @param count  The number of elements in the buffer
   *
   * @returns  A unique_ptr to a newly-constructed IOBuf
   * @methodset Makers
   *
   * TODO T154818309
   */
  template <class UniquePtr>
  static typename std::enable_if<
      detail::IsUniquePtrToSL<UniquePtr>::value,
      std::unique_ptr<IOBuf>>::type
  takeOwnership(UniquePtr&& buf, size_t count = 1);

  /**
   * Create an IOBuf pointing to a buffer, without taking ownership.
   *
   * This should only be used when the caller knows the lifetime of the IOBuf
   * object ahead of time and can ensure that all IOBuf objects that will point
   * to this buffer will be destroyed before the buffer itself is destroyed.
   * The buffer will not be freed automatically when the last IOBuf
   * referencing it is destroyed.  It is the caller's responsibility to free
   * the buffer after the last IOBuf has been destroyed.
   *
   * An IOBuf created using wrapBuffer() will always be reported as shared.
   * unshare() may be used to create a writable copy of the buffer.
   *
   * @param buf  The pointer to the buffer
   * @param capacity  The size of the buffer
   * @param br  Can pass a ByteRange in lieu of {buf, capacity}
   *
   * @post  data() points to buf
   * @post  length() == capacity
   */
  IOBuf(WrapBufferOp op, ByteRange br) noexcept;

  /// @copydoc IOBuf(WrapBufferOp, ByteRange)
  IOBuf(WrapBufferOp op, const void* buf, std::size_t capacity) noexcept;

  /**
   * @copydoc IOBuf(WrapBufferOp, ByteRange)
   * @throws std::bad_alloc on error (the allocation of th IOBuf may throw)
   * @returns  A unique_ptr to a newly-constructed IOBuf
   * @methodset Makers
   */
  static std::unique_ptr<IOBuf> wrapBuffer(
      const void* buf, std::size_t capacity);

  /// @copydoc wrapBuffer(const void*, std::size_t)
  static std::unique_ptr<IOBuf> wrapBuffer(ByteRange br) {
    return wrapBuffer(br.data(), br.size());
  }

  /**
   * @copydoc IOBuf(WrapBufferOp, ByteRange)
   *
   * This static function behaves exactly like the WrapBufferOp constructor.
   * It exists for syntactic parity with the unique_ptr-returning variants.
   *
   * @returns  A stack-allocated IOBuf
   * @methodset Makers
   */
  static IOBuf wrapBufferAsValue(
      const void* buf, std::size_t capacity) noexcept;

  /// @copydoc wrapBufferAsValue(const void*, std::size_t)
  static IOBuf wrapBufferAsValue(ByteRange br) noexcept {
    return wrapBufferAsValue(br.data(), br.size());
  }

  /**
   * Create an IOBuf and copy data into the buffer.
   *
   * The IOBuf will have a newly-allocated buffer. That buffer shall be
   * populated with data from the argument buffer.
   *
   * @param buf  The buffer from which to copy data
   * @param size  The size of the buffer from which to copy data
   * @param br  Can pass a ByteRange in lieu of {buf, size}
   * @param headroom  The amount of headroom to add to the destination buffer
   * @param minTailroom  The amount of tailroom to add to the destination buffer
   *
   * @post  data() points to a new buffer whose content is the same as buf
   * @post  length() == size
   * @post  headroom() == headroom
   * @post  tailroom() >= minTailroom
   *
   * @throws std::bad_alloc on error
   */
  IOBuf(
      CopyBufferOp op,
      ByteRange br,
      std::size_t headroom = 0,
      std::size_t minTailroom = 0);

  /// @copydoc IOBuf(CopyBufferOp, ByteRange, std::size_t, std::size_t)
  IOBuf(
      CopyBufferOp op,
      const void* buf,
      std::size_t size,
      std::size_t headroom = 0,
      std::size_t minTailroom = 0);

  /**
   * @copydoc IOBuf(CopyBufferOp, ByteRange, std::size_t, std::size_t)
   * @returns  A unique_ptr to a newly-constructed IOBuf
   * @methodset Makers
   */
  static std::unique_ptr<IOBuf> copyBuffer(
      ByteRange br, std::size_t headroom = 0, std::size_t minTailroom = 0) {
    return copyBuffer(br.data(), br.size(), headroom, minTailroom);
  }

  /// @copydoc copyBuffer(ByteRange, std::size_t, std::size_t)
  static std::unique_ptr<IOBuf> copyBuffer(
      const void* buf,
      std::size_t size,
      std::size_t headroom = 0,
      std::size_t minTailroom = 0);

  /**
   * @copydoc IOBuf(CopyBufferOp, ByteRange, std::size_t, std::size_t)
   *
   * Beware when attempting to invoke this function with a constant string
   * literal and a headroom argument: you will likely end up invoking
   * copyBuffer(void* buf, size_t size).
   */
  static std::unique_ptr<IOBuf> copyBuffer(
      StringPiece buf, std::size_t headroom = 0, std::size_t minTailroom = 0);
  IOBuf(
      CopyBufferOp op,
      StringPiece buf,
      std::size_t headroom = 0,
      std::size_t minTailroom = 0)
      : IOBuf(op, buf.data(), buf.size(), headroom, minTailroom) {}

  /**
   * @copydoc IOBuf(CopyBufferOp, ByteRange, std::size_t, std::size_t)
   *
   * This "maybe" version of copyBuffer returns null if the input is empty.
   *
   * @methodset Makers
   */
  static std::unique_ptr<IOBuf> maybeCopyBuffer(
      StringPiece buf, std::size_t headroom = 0, std::size_t minTailroom = 0);

  /**
   * Free an IOBuf.
   *
   * Note: as with all IOBuf destruction, this will also destroy all other
   * IOBufs in the same chain.
   *
   * @param data  The IOBuf to be destroyed
   * @post data will be nullptr
   *
   * @methodset Memory
   */
  static void destroy(std::unique_ptr<IOBuf>&& data) {
    auto destroyer = std::move(data);
  }

  /**
   * Destroy this IOBuf.
   *
   * Deleting an IOBuf will automatically destroy all IOBufs in the chain.
   * (All subsequent IOBufs in the chain are considered to be owned by the head
   * of the chain.  Users should only explicitly delete the head of a chain.)
   *
   * When each individual IOBuf is destroyed, it will release its reference
   * count on the underlying buffer.  If it was the last user of the buffer,
   * the buffer will be freed.
   */
  ~IOBuf();

  /**
   * Check whether the chain is empty.
   *
   * This method is semantically equivalent to
   *   i->computeChainDataLength()==0
   * but may run faster because it can short-circuit as soon as it
   * encounters a buffer with length()!=0
   *
   * @methodset Chaining
   */
  bool empty() const noexcept;

  /**
   * Get the pointer to the start of the data.
   *
   * @methodset Access
   */
  const uint8_t* data() const noexcept { return data_; }

  /**
   * Get a writable pointer to the start of the data.
   *
   * The caller is responsible for calling unshare() first to ensure that it is
   * actually safe to write to the buffer.
   *
   * @methodset Access
   */
  uint8_t* writableData() noexcept { return data_; }

  /**
   * Get the pointer to the end of the data.
   *
   * @methodset Access
   */
  const uint8_t* tail() const noexcept { return data_ + length_; }

  /**
   * Get a writable pointer to the end of the data.
   *
   * The caller is responsible for calling unshare() first to ensure that it is
   * actually safe to write to the buffer.
   *
   * @methodset Access
   */
  uint8_t* writableTail() noexcept { return data_ + length_; }

  /**
   * Get the size of the data for this individual IOBuf in the chain.
   *
   * Use computeChainDataLength() for the sum of data length for the full chain.
   *
   * @methodset Buffer Capacity
   */
  std::size_t length() const noexcept { return length_; }

  /**
   * Get the amount of head room.
   *
   * @returns  The number of bytes in the buffer before the start of the data
   *
   * @methodset Buffer Capacity
   */
  std::size_t headroom() const noexcept {
    return std::size_t(data_ - buffer());
  }

  /**
   * Get the amount of tail room.
   *
   * @returns  The number of bytes in the buffer after the end of the data
   *
   * @methodset Buffer Capacity
   */
  std::size_t tailroom() const noexcept {
    return std::size_t(bufferEnd() - tail());
  }

  /**
   * Get the pointer to the start of the buffer.
   *
   * Note that this is the pointer to the very beginning of the usable buffer,
   * not the start of valid data within the buffer.  Use the data() method to
   * get a pointer to the start of the data within the buffer.
   *
   * @methodset Access
   */
  const uint8_t* buffer() const noexcept { return buf_; }

  /**
   * Get a writable pointer to the start of the buffer.
   *
   * The caller is responsible for calling unshare() first to ensure that it is
   * actually safe to write to the buffer.
   *
   * @methodset Access
   */
  uint8_t* writableBuffer() noexcept { return buf_; }

  /**
   * Get the pointer to the end of the buffer.
   *
   * Note that this is the pointer to the very end of the usable buffer,
   * not the end of valid data within the buffer.  Use the tail() method to
   * get a pointer to the end of the data within the buffer.
   *
   * @methodset Access
   */
  const uint8_t* bufferEnd() const noexcept { return buf_ + capacity_; }

  /**
   * Get the total size of the buffer.
   *
   * This returns the total usable length of the buffer.  Use the length()
   * method to get the length of the actual valid data in this IOBuf.
   *
   * @methodset Buffer Capacity
   */
  std::size_t capacity() const noexcept { return capacity_; }

  /**
   * Get a pointer to the next IOBuf in this chain.
   *
   * @methodset Chaining
   */
  IOBuf* next() noexcept { return next_; }
  /// @copydoc next()
  const IOBuf* next() const noexcept { return next_; }

  /**
   * Get a pointer to the previous IOBuf in this chain.
   *
   * @methodset Chaining
   */
  IOBuf* prev() noexcept { return prev_; }
  /// @copydoc prev
  const IOBuf* prev() const noexcept { return prev_; }

  /**
   * Shift the data forwards in the buffer.
   *
   * This shifts the data pointer forwards in the buffer to increase the
   * headroom.  This is commonly used to increase the headroom in a newly
   * allocated buffer.
   *
   * The caller is responsible for ensuring that there is sufficient
   * tailroom in the buffer before calling advance().
   *
   * If there is a non-zero data length, advance() will use memmove() to shift
   * the data forwards in the buffer.  In this case, the caller is responsible
   * for making sure the buffer is unshared, so it will not affect other IOBufs
   * that may be sharing the same underlying buffer.
   *
   * @param amount  The amount by which to shift all data forward
   * @post  length() is unchanged
   *
   * @methodset Shifting
   */
  void advance(std::size_t amount) noexcept {
    // In debug builds, assert if there is a problem.
    assert(amount <= tailroom());

    if (length_ > 0) {
      memmove(data_ + amount, data_, length_);
    }
    data_ += amount;
  }

  /**
   * Shift the data backwards in the buffer.
   *
   * This shifts the data pointer backwards in the buffer, decreasing the
   * headroom.
   *
   * The caller is responsible for ensuring that there is sufficient headroom
   * in the buffer before calling retreat().
   *
   * If there is a non-zero data length, retreat() will use memmove() to shift
   * the data backwards in the buffer.  In this case, the caller is responsible
   * for making sure the buffer is unshared, so it will not affect other IOBufs
   * that may be sharing the same underlying buffer.
   *
   * @param amount  The amount by which to shift all data backward
   * @post  length() is unchanged
   *
   * @methodset Shifting
   */
  void retreat(std::size_t amount) noexcept {
    // In debug builds, assert if there is a problem.
    assert(amount <= headroom());

    if (length_ > 0) {
      memmove(data_ - amount, data_, length_);
    }
    data_ -= amount;
  }

  /**
   * Adjust the data pointer to include more valid data at the beginning.
   *
   * This moves the data pointer backwards to include more of the available
   * buffer.  The caller is responsible for ensuring that there is sufficient
   * headroom for the new data.  The caller is also responsible for populating
   * this section with valid data.
   *
   * This does not modify any actual data in the buffer.
   *
   * @param amount  The amount by which to shift the data() pointer backward
   * @post  length() is increased by amount
   *
   * @methodset Shifting
   */
  void prepend(std::size_t amount) noexcept {
    DCHECK_LE(amount, headroom());
    data_ -= amount;
    length_ += amount;
  }

  /**
   * Adjust the tail pointer to include more valid data at the end.
   *
   * This moves the tail pointer forwards to include more of the available
   * buffer.  The caller is responsible for ensuring that there is sufficient
   * tailroom for the new data.  The caller is also responsible for populating
   * this section with valid data.
   *
   * This does not modify any actual data in the buffer.
   *
   * @param amount  The amount by which to shift the tail() pointer forward
   * @post  length() is increased by amount
   *
   * @methodset Shifting
   */
  void append(std::size_t amount) noexcept {
    DCHECK_LE(amount, tailroom());
    length_ += amount;
  }

  /**
   * Adjust the data pointer to include less valid data.
   *
   * This moves the data pointer forwards so that the first amount bytes are no
   * longer considered valid data.  The caller is responsible for ensuring that
   * amount is less than or equal to the actual data length.
   *
   * This does not modify any actual data in the buffer.
   *
   * @param amount  The amount by which to shift the data() pointer forward
   * @post  length() is decreased by amount
   *
   * @methodset Shifting
   */
  void trimStart(std::size_t amount) noexcept {
    DCHECK_LE(amount, length_);
    data_ += amount;
    length_ -= amount;
  }

  /**
   * Adjust the tail pointer backwards to include less valid data.
   *
   * This moves the tail pointer backwards so that the last amount bytes are no
   * longer considered valid data.  The caller is responsible for ensuring that
   * amount is less than or equal to the actual data length.
   *
   * This does not modify any actual data in the buffer.
   *
   * @param amount  The amount by which to shift the tail() pointer backward
   * @post  length() is decreased by amount
   *
   * @methodset Shifting
   */
  void trimEnd(std::size_t amount) noexcept {
    DCHECK_LE(amount, length_);
    length_ -= amount;
  }

  /**
   * Adjust the buffer end pointer to reduce the buffer capacity.
   *
   * This can be used to pass the ownership of the writable tail to another
   * IOBuf.
   *
   * @param amount The amount by which to shift the bufferEnd() pointer backward
   * @post  capacity() is decreased by amount
   *
   * @methodset Shifting
   */
  void trimWritableTail(std::size_t amount) noexcept {
    DCHECK_LE(amount, tailroom());
    capacity_ -= amount;
  }

  /**
   * Clear the buffer.
   *
   * @post  data() == buffer()
   * @post  length() == 0
   */
  void clear() noexcept {
    data_ = writableBuffer();
    length_ = 0;
  }

  /**
   * Ensure that the buffer has enough free space.
   *
   * Ensure that this buffer has at least minHeadroom headroom bytes and at
   * least minTailroom tailroom bytes.  The buffer must be writable
   * (you must call unshare() before this, if necessary).
   *
   * This might involve a reallocation of the underlying buffer.
   *
   * @param minHeadroom  The requested amount of headroom
   * @param minTailroom  The requested amount of tailroom
   *
   * @post  headroom() >= minHeadroom
   * @post  tailroom() >= minTailroom
   * @post  The contents between data() and tail() are preserved
   *
   * @methodset Buffer Capacity
   */
  void reserve(std::size_t minHeadroom, std::size_t minTailroom) {
    // Maybe we don't need to do anything.
    if (headroom() >= minHeadroom && tailroom() >= minTailroom) {
      return;
    }
    // If the buffer is empty but we have enough total room (head + tail),
    // move the data_ pointer around.
    if (length() == 0 && headroom() + tailroom() >= minHeadroom + minTailroom) {
      data_ = writableBuffer() + minHeadroom;
      return;
    }
    // Bah, we have to do actual work.
    reserveSlow(minHeadroom, minTailroom);
  }

  /**
   * Is this IOBuf part of a chain.
   *
   * Technically, all IOBufs are part of a chain, possibly of length 1. This
   * function checks if the chain is non-trivial, i.e. the chain has more than
   * just one IOBuf in it.
   *
   * @returns  true iff the the IOBuf's chain has more than 1 IOBuf in it
   *
   * @methodset Chaining
   */
  bool isChained() const noexcept {
    assert((next_ == this) == (prev_ == this));
    return next_ != this;
  }

  /**
   * Get the number of IOBufs in this chain.
   *
   * Beware that this method has to walk the entire chain.
   * Use isChained() if you just want to check if this IOBuf is part of a chain
   * or not.
   *
   * @methodset Chaining
   */
  size_t countChainElements() const noexcept;

  /**
   * Get the length of all the data in this IOBuf chain.
   *
   * Beware that this method has to walk the entire chain.
   *
   * @methodset Chaining
   */
  std::size_t computeChainDataLength() const noexcept;

  /**
   * Get the capacity all IOBufs in the chain.
   *
   * Beware that this method has to walk the entire chain.
   *
   * @methodset Chaining
   */
  std::size_t computeChainCapacity() const noexcept;

  /**
   * Append another IOBuf chain to the end of this chain.
   *
   * For example, if there are two IOBuf chains (A, B, C) and (D, E, F),
   * and A->appendToChain(D) is called, the (D, E, F) chain will be subsumed
   * and become part of the chain starting at A, which will now look like
   * (A, B, C, D, E, F).
   *
   * @methodset Chaining
   */
  void appendToChain(std::unique_ptr<IOBuf>&& iobuf);

  /**
   * Insert an IOBuf chain immediately after this chain element.
   *
   * For example, if there are two IOBuf chains (A, B, C) and (D, E, F),
   * and B->insertAfterThisOne(D) is called, the (D, E, F) chain will be
   * subsumed and become part of the chain starting at A, which will now look
   * like (A, B, D, E, F, C)
   *
   * Note if X is an IOBuf chain with just a single element, X->appendToChain()
   * and X->insertAfterThisOne() behave identically.
   *
   * @methodset Chaining
   */
  void insertAfterThisOne(std::unique_ptr<IOBuf>&& iobuf) {
    // Just use appendToChain() on the next element in our chain
    next_->appendToChain(std::move(iobuf));
  }

  /**
   * Deprecated name for appendToChain()
   *
   * IOBuf chains are circular, so appending to the end of the chain is
   * logically equivalent to prepending to the current head (but keeping the
   * chain head pointing to the same element).  That was the reason this method
   * was originally called prependChain().  However, almost every time this
   * method is called the intent is to append to the end of a chain, so the
   * `prependChain()` name is very confusing to most callers.
   *
   * @methodset Chaining
   */
  void prependChain(std::unique_ptr<IOBuf>&& iobuf) {
    appendToChain(std::move(iobuf));
  }

  /**
   * Deprecated name for insertAfterThisOne()
   *
   * Beware: appendToChain() and appendChain() are two different methods,
   * and you probably want appendToChain() instead of this one.
   *
   * @methodset Chaining
   */
  void appendChain(std::unique_ptr<IOBuf>&& iobuf) {
    insertAfterThisOne(std::move(iobuf));
  }

  /**
   * Remove this IOBuf from its current chain.
   *
   * Ownership of all elements an IOBuf chain is normally maintained by
   * the head of the chain. unlink() transfers ownership of this IOBuf from the
   * chain and gives it to the caller.  A new unique_ptr to the IOBuf is
   * returned to the caller.  The caller must store the returned unique_ptr (or
   * call release() on it) to take ownership, otherwise the IOBuf will be
   * immediately destroyed.
   *
   * Since unlink() transfers ownership of the IOBuf to the caller, be careful
   * not to call unlink() on the head of a chain if you already maintain
   * ownership on the head of the chain via other means.  The pop() method
   * is a better choice for that situation.
   *
   * @methodset Chaining
   */
  std::unique_ptr<IOBuf> unlink() {
    next_->prev_ = prev_;
    prev_->next_ = next_;
    prev_ = this;
    next_ = this;
    return std::unique_ptr<IOBuf>(this);
  }

  /**
   * Remove the rest of the chain from this IOBuf.
   *
   * Ownership of all elements an IOBuf chain is normally maintained by
   * the head of the chain. pop() transfers ownership of the rest of the chain
   * to the caller.
   *
   * Since pop() transfers ownership of the rest to the caller, be careful
   * not to call pop() except on the head of a chain.
   *
   * @returns  A new unique_ptr pointing to the rest of the chain; nullptr if
   *           this IOBuf was the only chain element
   * @methodset Chaining
   */
  std::unique_ptr<IOBuf> pop() {
    IOBuf* next = next_;
    next_->prev_ = prev_;
    prev_->next_ = next_;
    prev_ = this;
    next_ = this;
    return std::unique_ptr<IOBuf>((next == this) ? nullptr : next);
  }

  /**
   * Remove a subchain from this chain.
   *
   * Remove the subchain starting at head and ending at tail from this chain.
   * This is inclusive of tail.
   *
   * If you have a chain (A, B, C, D, E, F), and you call A->separateChain(B,
   * D), then you will be returned the chain (B, C, D) and the current IOBuf
   * chain will change to (A, E, F).
   *
   * Returns a unique_ptr pointing to the new head.  (In other words, ownership
   * of the head of the subchain is transferred to the caller.)  If the caller
   * ignores the return value and lets the unique_ptr be destroyed, the subchain
   * will be immediately destroyed.
   *
   * head may equal tail. In this case, the subchain of length 1 is removed.
   *
   * @pre  head and tail are part of the current IOBuf chain
   * @pre  head and tail are not equal to the current IOBuf
   *
   * @param head  The first IOBuf chain element to remove
   * @param tail  The last IOBuf chain element to remove (inclusive)
   *
   * @methodset Chaining
   */
  std::unique_ptr<IOBuf> separateChain(IOBuf* head, IOBuf* tail) {
    assert(head != this);
    assert(tail != this);

    head->prev_->next_ = tail->next_;
    tail->next_->prev_ = head->prev_;

    head->prev_ = tail;
    tail->next_ = head;

    return std::unique_ptr<IOBuf>(head);
  }

  /**
   * Check if any chain buffers are shared.
   *
   * Return true if at least one of the IOBufs in this chain are shared,
   * or false if all of the IOBufs point to unique buffers.
   *
   * Use isSharedOne() to only check this IOBuf rather than the entire chain.
   *
   * If isShared() returns false, then you are probably the sole owner of the
   * IOBuf chain and can write to it without needing to call unshare(). This is
   * not a guarantee, since it is possible for another thread to concurrently
   * acquire shared ownership.
   *
   * @methodset Buffer Management
   */
  bool isShared() const noexcept {
    const IOBuf* current = this;
    while (true) {
      if (current->isSharedOne()) {
        return true;
      }
      current = current->next_;
      if (current == this) {
        return false;
      }
    }
  }

  /**
   * Get userData.
   *
   * userData is the optional constructor argument which will be passed to the
   * FreeFunction.
   *
   * @returns  A non-owning pointer to userData if set, else nullptr
   *
   * @methodset Buffer Management
   */
  void* getUserData() const noexcept {
    return sharedInfo_ ? sharedInfo_->userData : nullptr;
  }

  /**
   * Get the FreeFunction.
   *
   * freeFn is the optional constructor argument which shall be called when the
   * buffer is to be destroyed.
   *
   * @returns  A non-owning pointer to freeFn if set, else nullptr
   *
   * @methodset Buffer Management
   */
  FreeFunction getFreeFn() const noexcept {
    return sharedInfo_ ? sharedInfo_->freeFn : nullptr;
  }

  /**
   * Add an Observer to the refcount block (SharedInfo).
   *
   * @param observer  The observer to add to SharedInfo
   * @returns  true iff the observer was added (if there is no SharedInfo,
   *           there's nothing to observe)
   *
   * @methodset Misc
   */
  template <typename Observer>
  bool appendSharedInfoObserver(Observer&& observer) {
    SharedInfo* info = sharedInfo_;
    if (!info) {
      return false;
    }

    auto* entry =
        new SharedInfoObserverEntry<Observer>(std::forward<Observer>(observer));
    std::lock_guard guard(info->observerListLock);
    if (!info->observerListHead) {
      info->observerListHead = entry;
    } else {
      // prepend
      entry->next = info->observerListHead;
      entry->prev = info->observerListHead->prev;
      info->observerListHead->prev->next = entry;
      info->observerListHead->prev = entry;
    }

    return true;
  }

  /**
   * Check if all IOBufs in this chain use the standard refcounting mechanism.
   *
   * If so, then the lifetime of the underlying memory can be extended by
   * clone().
   *
   * @returns  true iff all IOBufs in this chain are isManagedOne()
   *
   * @methodset Buffer Management
   */
  bool isManaged() const noexcept {
    const IOBuf* current = this;
    while (true) {
      if (!current->isManagedOne()) {
        return false;
      }
      current = current->next_;
      if (current == this) {
        return true;
      }
    }
  }

  /**
   * Check if this IOBuf uses the standard refcounting mechanism.
   *
   * If so, then the lifetime of the underlying memory can be extended by
   * cloneOne().
   *
   * @returns  true iff the current IOBuf was allocated normally (without the
   *           user specifying special memory semantics, such as with a
   *           user-owned buffer)
   *
   * @methodset Buffer Management
   */
  bool isManagedOne() const noexcept { return sharedInfo_ != nullptr; }

  /**
   * Inconsistently get the reference count.
   *
   * For most of the use-cases where it seems like a good idea to call this
   * function, what you really want is isSharedOne().
   *
   * If this IOBuf is managed by the usual refcounting mechanism (ie
   * isManagedOne() returns true) then this returns the reference count as it
   * was when recently observed by this thread.
   *
   * If this IOBuf is *not* managed by the usual refcounting mechanism then the
   * result of this function is not defined.
   *
   * This only checks the current IOBuf, and not other IOBufs in the chain.
   *
   * @methodset Buffer Management
   */
  uint32_t approximateShareCountOne() const noexcept;

  /**
   * Check if the buffer is shared.
   *
   * IOBuf buffers can be shared (using refcounting). Check if any other IOBufs
   * are pointing to this same buffer.
   *
   * If this IOBuf points at a buffer owned by another (non-IOBuf) part of the
   * code (i.e., if the IOBuf was created using wrapBuffer(), or was cloned
   * from such an IOBuf), it is always considered shared.
   *
   * This only checks the current IOBuf, and not other IOBufs in the chain.
   *
   * @methodset Buffer Management
   */
  bool isSharedOne() const noexcept {
    // If this is a user-owned buffer, it is always considered shared
    if (FOLLY_UNLIKELY(!sharedInfo_)) {
      return true;
    }

    if (FOLLY_UNLIKELY(sharedInfo_->externallyShared)) {
      return true;
    }

    return sharedInfo_->refcount.load(std::memory_order_acquire) > 1;
  }

  /**
   * Ensure that this IOBuf chain has unique, unshared buffers.
   *
   * Multiple IOBufs can point to the same buffer. This means that an IOBuf's
   * buffer is not necessarily writeable, since another IOBuf might be using the
   * same underlying data. If you want to write to an IOBuf's buffer, it is your
   * responsibility to make sure that you aren't trampling the data used by
   * another IOBuf. This can be accomplished by calling unshare().
   *
   * unshare() ensures that the underlying buffer of each IOBuf in the chain is
   * not shared with another IOBuf.
   *
   * @note If the current chain has any shared buffers, then unshare() might
   *       coalesce the chain during unsharing.
   * @note Buffers owned by other (non-IOBuf) users are automatically considered
   *       to be shared.
   *
   * @post  The buffers in this IOBuf chain are all writeable, since they are
   *        uniquely owned by the current IOBuf.
   *
   * @throws std::bad_alloc on error.  On error the IOBuf chain will be
   * unmodified.
   *
   * Currently unshare may also throw std::overflow_error if it tries to
   * coalesce.  (TODO: In the future it would be nice if unshare() were smart
   * enough not to coalesce the entire buffer if the data is too large.
   * However, in practice this seems unlikely to become an issue.)
   *
   * @methodset Buffer Management
   */
  void unshare() {
    if (isChained()) {
      unshareChained();
    } else {
      unshareOne();
    }
  }

  /**
   * Ensure that this IOBuf has a unique, unshared buffer.
   *
   * unshareOne() operates on a single IOBuf object.  This IOBuf will have a
   * unique buffer after unshareOne() returns, but other IOBufs in the chain
   * may still be shared after unshareOne() returns.
   *
   * @throws std::bad_alloc on error.  On error the IOBuf will be unmodified.
   *
   * @methodset Buffer Management
   */
  void unshareOne() {
    if (isSharedOne()) {
      unshareOneSlow();
    }
  }

  /**
   * Mark the underlying buffers in this chain as shared.
   *
   * Assume that the underlying buffers are also owned by an external memory
   * management mechanism. This will make isShared() always returns true.
   *
   * This function is not thread-safe, and only safe to call immediately after
   * creating an IOBuf, before it has been shared with other threads.
   *
   * @methodset Buffer Management
   */
  void markExternallyShared();

  /**
   * Mark the underlying buffer as shared.
   *
   * Assume that the underlying buffer is also owned by an external memory
   * management mechanism. This will make isSharedOne() always returns true.
   *
   * This function is not thread-safe, and only safe to call immediately after
   * creating an IOBuf, before it has been shared with other threads.
   *
   * @methodset Buffer Management
   */
  void markExternallySharedOne() {
    if (sharedInfo_) {
      sharedInfo_->externallyShared = true;
    }
  }

  /**
   * Ensure that the buffers are owned by the IOBuf chain.
   *
   * It is possible for an IOBuf to be constructed with a user-owned buffer. In
   * such circumstances, the user is responsible for ensuring that the buffer
   * outlives the IOBuf. makeManaged() lets the user subsequently reallocate the
   * buffer to be owned by the IOBuf directly.
   *
   * If the buffers are already owned by IOBuf, then this function doesn't need
   * to do anything.
   *
   * @methodset Buffer Management
   */
  void makeManaged() {
    if (isChained()) {
      makeManagedChained();
    } else {
      makeManagedOne();
    }
  }

  /**
   * Ensure that the buffer is owned by the IOBuf.
   *
   * It is possible for an IOBuf to be constructed with a user-owned buffer. In
   * such circumstances, the user is responsible for ensuring that the buffer
   * outlives the IOBuf. makeManaged() lets the user subsequently reallocate the
   * buffer to be owned by the IOBuf directly.
   *
   * If the buffer is already owned by IOBuf, then this function doesn't need to
   * do anything.
   *
   * @methodset Buffer Management
   */
  void makeManagedOne() {
    if (!isManagedOne()) {
      // We can call the internal function directly; unmanaged implies shared.
      unshareOneSlow();
    }
  }

  /**
   * Coalesce this IOBuf chain into a single buffer.
   *
   * This method moves all of the data in this IOBuf chain into a single
   * contiguous buffer, if it is not already in one buffer.  After coalesce()
   * returns, this IOBuf will be a chain of length one.  Other IOBufs in the
   * chain will be automatically deleted.
   *
   * After coalescing, the IOBuf will have at least as much headroom as the
   * first IOBuf in the chain, and at least as much tailroom as the last IOBuf
   * in the chain.
   *
   * @post  isChained() == false
   *
   * @throws std::bad_alloc on error.  On error the IOBuf chain will be
   * unmodified.
   *
   * @returns  A ByteRange that points to the now-contiguous buffer data()
   *
   * @methodset Chaining
   */
  ByteRange coalesce() {
    if (isChained()) {
      const std::size_t newHeadroom = headroom();
      const std::size_t newTailroom = prev()->tailroom();
      coalesceAndReallocate(
          newHeadroom, computeChainDataLength(), this, newTailroom);
    }
    return ByteRange(data_, length_);
  }

  /**
   * @copydoc coalesce()
   *
   * @param newHeadroom  How much headroom the new coalesced chain should have,
   *                     instead of mimicking the original headroom
   * @param newTailroom  How much tailroom the new coalesced chain should have,
   *                     instead of mimicking the original tailroom
   */
  ByteRange coalesceWithHeadroomTailroom(
      std::size_t newHeadroom, std::size_t newTailroom) {
    if (isChained()) {
      coalesceAndReallocate(
          newHeadroom, computeChainDataLength(), this, newTailroom);
    }
    return ByteRange(data_, length_);
  }

  /**
   * Ensure that this chain has at least contiguousLength bytes available as a
   * contiguous memory range.
   *
   * This method coalesces whole buffers in the chain into this buffer as
   * necessary until this buffer's length() is at least contiguousLength.
   *
   * After coalescing, the IOBuf will have at least as much headroom as the
   * first IOBuf in the chain, and at least as much tailroom as the last IOBuf
   * that was coalesced.
   *
   * @throws std::bad_alloc or std::overflow_error on error.  On error the IOBuf
   * chain will be unmodified.
   * @throws std::overflow_error if contiguousLength is longer than the total
   * chain length.
   *
   * @post  length() >= contiguousLength
   *
   * @methodset Chaining
   */
  void gather(std::size_t contiguousLength) {
    if (!isChained() || length_ >= contiguousLength) {
      return;
    }
    coalesceSlow(contiguousLength);
  }

  /**
   * Copy an IOBuf chain.
   *
   * This is a shallow buffer copy; the source buffers will be shared.
   *
   * The new IOBuf chain will normally point to the same underlying data
   * buffers as the original chain.  (The one exception to this is if some of
   * the IOBufs in this chain contain small internal data buffers which cannot
   * be shared.)
   *
   * @methodset Makers
   */
  std::unique_ptr<IOBuf> clone() const { return cloneImpl(nullptr); }

  /**
   * @copydoc clone()
   *
   * Similar to clone(), but returns by value rather than heap-allocating.
   */
  IOBuf cloneAsValue() const;

  /**
   * Copy an individual IOBuf.
   *
   * Only clone the buffer of the current IOBuf; ignore chained IOBufs.
   *
   * @methodset Makers
   */
  std::unique_ptr<IOBuf> cloneOne() const { return cloneOneImpl(nullptr); }

  /**
   * @copydoc cloneOne()
   *
   * Similar to cloneOne(), but returns by value rather than heap-allocating.
   */
  IOBuf cloneOneAsValue() const;

  /**
   * Copy an IOBuf chain into a single buffer.
   *
   * Semantically similar to .clone().coalesce(), but without the intermediate
   * allocations.
   *
   * The new IOBuf will have at least as much headroom as the first IOBuf in the
   * chain, and at least as much tailroom as the last IOBuf in the chain.
   *
   * @return  An IOBuf for which isChained() == false, and whose data is the
   *          same as coalesce()
   *
   * @throws std::bad_alloc on error.
   *
   * @methodset Makers
   */
  std::unique_ptr<IOBuf> cloneCoalesced() const;

  /**
   * @copydoc cloneCoalesced()
   *
   * @param newHeadroom  How much headroom the new coalesced chain should have,
   *                     instead of mimicking the original headroom
   * @param newTailroom  How much tailroom the new coalesced chain should have,
   *                     instead of mimicking the original tailroom
   */
  std::unique_ptr<IOBuf> cloneCoalescedWithHeadroomTailroom(
      std::size_t newHeadroom, std::size_t newTailroom) const;

  /**
   * @copydoc cloneCoalesced()
   *
   * Similar to cloneCoalesced(), but returns by value rather than
   * heap-allocating.
   */
  IOBuf cloneCoalescedAsValue() const;

  /**
   * @copydoc cloneCoalescedWithHeadroomTailroom(std::size_t, std::size_t) const
   *
   * Similar to cloneCoalescedWithHeadroomTailroom(), but returns by value
   * rather than heap-allocating.
   */
  IOBuf cloneCoalescedAsValueWithHeadroomTailroom(
      std::size_t newHeadroom, std::size_t newTailroom) const;

  /**
   * @copydoc clone()
   *
   * Similar to clone(), but returns by argument. The argument will become the
   * clone's head. Other nodes in the chain (if any) will be allocated on the
   * heap as usual.
   *
   * @param[out] other  An IOBuf to assign the clone to
   */
  void cloneInto(IOBuf& other) const { other = cloneAsValue(); }

  /**
   * @copydoc cloneOne()
   *
   * Similar to cloneOne(), but returns by argument. The argument will become
   * the clone.
   *
   * @param[out] other  An IOBuf to assign the clone to
   */
  void cloneOneInto(IOBuf& other) const {
    incrementRefcount();
    other.assignBuffer(sharedInfo_, buf_, data_, capacity_, length_);
  }

  /**
   * Returns a new IOBuf whose buffer is this buffer's tail. The latter is
   * trimmed to 0 to relinquish ownership of it. The returned IOBuf is unshared,
   * and it holds a shared reference to the IOBuf that originally owned the
   * buffer, extending its lifetime.
   *
   * This method is best-effort and allowed to fail if the operation is not
   * possible (for example, if the buffer is shared) or inefficient. In these
   * cases, nullptr is returned and this IOBuf is unchanged.
   */
  std::unique_ptr<IOBuf> maybeSplitTail();

  /**
   * Append the chain data into the provided container.
   *
   * This is meant to be used with containers such as std::string or
   * std::vector<char>, but any container which supports reserve(), insert(),
   * and has char or unsigned char value type is supported.
   *
   * @methodset Conversions
   */
  template <class Container>
  void appendTo(Container& container) const;

  /**
   * Returns a container containing the chain data.
   *
   * @copydetails appendTo(Container&) const
   *
   * @tparam Container  The type of container to return.
   *
   * @returns  A Container whose data equals the coalesced data of this chain
   */
  template <class Container>
  Container to() const;

  /**
   * Convenience version of to<std::string>() that works when called
   * on a dependent name in a template function without having to use
   * the "template" keyword.
   */
  std::string toString() const { return to<std::string>(); }

  /**
   * Create an IOBuf from a std::string. Avoids copying the contents of the
   * string, at the cost of an extra allocation.
   */
  static std::unique_ptr<IOBuf> fromString(std::unique_ptr<std::string>);
  static std::unique_ptr<IOBuf> fromString(std::string s) {
    return fromString(std::make_unique<std::string>(std::move(s)));
  }

  /**
   * Get an iovector suitable for e.g. writev()
   *
   *   auto iov = buf->getIov();
   *   auto xfer = writev(fd, iov.data(), iov.size());
   *
   * Naturally, the returned iovector is invalid if you modify the buffer
   * chain.
   *
   * @methodset IOV
   */
  folly::fbvector<struct iovec> getIov() const;

  /**
   * Update an existing iovec array with the IOBuf data.
   *
   * New iovecs will be appended to the existing vector; anything already
   * present in the vector will be left unchanged.
   *
   * Naturally, the returned iovec data will be invalid if you modify the
   * buffer chain.
   *
   * @param[out] iov  The iovector to append to
   *
   * @methodset IOV
   */
  void appendToIov(folly::fbvector<struct iovec>* iov) const;

  struct FillIovResult {
    // How many iovecs were filled (or 0 on error).
    size_t numIovecs;
    // The total length of filled iovecs (or 0 on error).
    size_t totalLength;
  };

  /**
   * Fill an iovec array with the IOBuf data.
   *
   * Returns a struct with two fields: the number of iovec filled, and total
   * size of the iovecs filled. If there are more buffer than iovec, returns 0
   * in both fields.
   * This version is suitable to use with stack iovec arrays.
   *
   * Naturally, the filled iovec data will be invalid if you modify the
   * buffer chain.
   *
   * @param[out] iov  The iovector to append to
   * @param len  The size of the iov array
   *
   * @methodset IOV
   */
  FillIovResult fillIov(struct iovec* iov, size_t len) const;

  /**
   * Convert an iovec array into an IOBuf.
   *
   * A helper that wraps a number of iovecs into an IOBuf chain.  If count == 0,
   * then a zero length buf is returned.  This function never returns nullptr.
   *
   * @param vec  The iovec array to convert to an IOBuf chain
   * @param count  The size of the iovec array
   *
   * @methodset IOV
   */
  static std::unique_ptr<IOBuf> wrapIov(const iovec* vec, size_t count);

  /**
   * Take ownership of an iovec, turning it into an IOBuf.
   *
   * A helper that takes ownerships a number of iovecs into an IOBuf chain.  If
   * count == 0, then a zero length buf is returned.  This function never
   * returns nullptr.
   *
   * @param vec  The iovec array to convert to an IOBuf chain
   * @param count  The size of the iovec array
   * @param freeFn  The function to call when buf is to be freed
   * @param userData  An additional arbitrary void* argument to supply to freeFn
   * @param freeOnError  Whether the buffer should be freed if this function
   *                     throws an exception
   *
   * @methodset IOV
   */
  static std::unique_ptr<IOBuf> takeOwnershipIov(
      const iovec* vec,
      size_t count,
      FreeFunction freeFn = nullptr,
      void* userData = nullptr,
      bool freeOnError = true);

#if FOLLY_HAS_MEMORY_RESOURCE

  /**
   * PMR support.
   *
   * These methods allow constructing IOBuf chains whose nodes are allocated
   * using the provided memory resource. Aside from the use of
   * std::pmr::memory_resource for allocating/deallocating the nodes, their
   * semantics are equivalent to their non-PMR counterparts. Currently only a
   * subset of IOBuf construction methods is implemented, enough to support the
   * typical lifetime of an IOBuf chain: buffer can be externally allocated and
   * wrapped with takeOwnership(), and cloned. More methods can be supported as
   * needed.
   *
   * The thread-safety requirements of the provided memory_resource depend on
   * the lifetime of the IOBufs. The allocate() method is only called in the
   * context of the IOBuf method that accepts it; deallocate() is called when
   * the IOBuf storage is eventually destroyed. Thus, in the common case where
   * the chain is constructed in a single thread (which owns the
   * memory_resource) and then handed off, it is sufficient that the
   * memory_resource supports concurrent deallocate() calls, as different nodes
   * in the chain may be destroyed by different threads.
   *
   * All the methods allow mr to be nullptr, which is equivalent to calling
   * the non-PMR counterparts.
   */

  static std::unique_ptr<IOBuf> takeOwnership(
      std::pmr::memory_resource* mr,
      void* buf,
      std::size_t capacity,
      std::size_t offset,
      std::size_t length,
      FreeFunction freeFn = nullptr,
      void* userData = nullptr,
      bool freeOnError = true) {
    return takeOwnershipImpl(
        buf,
        capacity,
        offset,
        length,
        freeFn,
        userData,
        freeOnError,
        TakeOwnershipOption::DEFAULT,
        mr);
  }
  std::unique_ptr<IOBuf> clone(std::pmr::memory_resource* mr) const {
    return cloneImpl(mr);
  }
  std::unique_ptr<IOBuf> cloneOne(std::pmr::memory_resource* mr) const {
    return cloneOneImpl(mr);
  }

#endif /* FOLLY_HAS_MEMORY_RESOURCE */

  /**
   * Overridden operator new and delete.
   *
   * These perform specialized memory management to help support
   * createCombined(), which allocates IOBuf objects together with the buffer
   * data.
   *
   * @methodset Memory
   */
  void* operator new(size_t size);

  /**
   * Overridden operator new.
   * @methodset Memory
   */
  void* operator new(size_t size, void* ptr);

  /**
   * Overridden operator delete.
   * @methodset Memory
   */
  void operator delete(void* ptr);

  /**
   * Overridden operator delete.
   * @methodset Memory
   */
  void operator delete(void* ptr, void* placement);

  /**
   * Destructively convert to an fbstring.
   *
   * Destructively convert this IOBuf to a fbstring efficiently.
   * We rely on fbstring's AcquireMallocatedString constructor to
   * transfer memory.
   *
   * @methodset Conversions
   */
  fbstring moveToFbString();

  /**
   * Iterate over the IOBufs in this chain.
   *
   * The iterators dereference to a ByteRange.
   *
   * @methodset Iterators
   */
  Iterator cbegin() const;

  /// @copydoc cbegin()
  Iterator cend() const;

  /// @copydoc cbegin()
  Iterator begin() const;

  /// @copydoc cbegin()
  Iterator end() const;

  /**
   * Create a new null buffer.
   *
   * This can be used to allocate an empty IOBuf on the stack.  It will have no
   * space allocated for it.  This is generally useful only to later use move
   * assignment to fill out the IOBuf.
   */
  IOBuf() noexcept;

  /**
   * Move constructor.
   *
   * In general, you should only ever move the head of an IOBuf chain.
   * Internal nodes in an IOBuf chain are owned by the head of the chain, and
   * should not be moved from.  (Technically, nothing prevents you from moving
   * a non-head node, but the moved-to node will replace the moved-from node in
   * the chain.  This has implications for ownership, since non-head nodes are
   * owned by the chain head.  You are then responsible for relinquishing
   * ownership of the moved-to node, and manually deleting the moved-from
   * node.)
   */
  IOBuf(IOBuf&& other) noexcept;

  /**
   * Move assignment operator.
   *
   * With the assignment operator, the destination should be the head of an
   * IOBuf chain or a solitary IOBuf not part of a chain.  If the destination is
   * part of a chain, all other IOBufs in the chain will be deleted.
   */
  IOBuf& operator=(IOBuf&& other) noexcept;

  /**
   * Copy constructor.
   *
   * @see cloneAsValue()
   */
  IOBuf(const IOBuf& other);

  /**
   * Copy assignment operator.
   *
   * @copydetails operator=(IOBuf&&)
   * @see cloneAsValue()
   */
  IOBuf& operator=(const IOBuf& other);

 private:
  enum class TakeOwnershipOption { DEFAULT, STORE_SIZE };

  static std::unique_ptr<IOBuf> takeOwnershipImpl(
      void* buf,
      std::size_t capacity,
      std::size_t offset,
      std::size_t length,
      FreeFunction freeFn,
      void* userData,
      bool freeOnError,
      TakeOwnershipOption option,
      std::pmr::memory_resource* mr = nullptr);
  std::unique_ptr<IOBuf> cloneImpl(std::pmr::memory_resource* mr) const;
  std::unique_ptr<IOBuf> cloneOneImpl(std::pmr::memory_resource* mr) const;

  struct SharedInfoObserverEntryBase {
    SharedInfoObserverEntryBase* prev{this};
    SharedInfoObserverEntryBase* next{this};

    virtual ~SharedInfoObserverEntryBase() = default;

    virtual void afterFreeExtBuffer() const noexcept = 0;
    virtual void afterReleaseExtBuffer() const noexcept = 0;
  };

  template <typename Observer>
  struct SharedInfoObserverEntry : SharedInfoObserverEntryBase {
    std::decay_t<Observer> observer;

    explicit SharedInfoObserverEntry(Observer&& obs) noexcept(
        noexcept(Observer(std::forward<Observer>(obs))))
        : observer(std::forward<Observer>(obs)) {}

    void afterFreeExtBuffer() const noexcept final {
      observer.afterFreeExtBuffer();
    }

    void afterReleaseExtBuffer() const noexcept final {
      observer.afterReleaseExtBuffer();
    }
  };

  struct SharedInfo {
    enum class StorageType : uint8_t {
      kInvalid, // Sentinel value.
      kAllocated,
      kHeapFullStorage,
      kExtBuffer,
    };

    SharedInfo(FreeFunction fn, void* arg, StorageType st);

    static void releaseStorage(
        IOBuf* parent, StorageType storageType, SharedInfo* info) noexcept;

    using ObserverCb = folly::FunctionRef<void(SharedInfoObserverEntryBase&)>;
    static void invokeAndDeleteEachObserver(
        SharedInfoObserverEntryBase* observerListHead, ObserverCb cb) noexcept;

    // A pointer to a function to call to free the buffer when the refcount
    // hits 0.  If this is null, free() will be used instead.
    FreeFunction freeFn{nullptr};
    void* userData{nullptr};
    SharedInfoObserverEntryBase* observerListHead{nullptr};
    std::atomic<uint32_t> refcount{1};
    bool externallyShared{false};
    StorageType storageType = StorageType::kInvalid;
    MicroSpinLock observerListLock{0};
  };

  // Helper structs for use by operator new and delete
  struct HeapPrefix;
  struct HeapStorage;
  struct HeapFullStorage;

  // Force inlining to allow optimizing away some branches in the common cases.
  template <class StorageType>
  FOLLY_ALWAYS_INLINE static std::pair<StorageType*, size_t> allocateStorage(
      std::pmr::memory_resource* mr = nullptr, size_t additionalBuffer = 0);
  static std::pmr::memory_resource* getMemoryResource(
      const HeapStorage* storage);
  static void freeStorage(HeapStorage* storage);

  /**
   * Create a new IOBuf pointing to an external buffer.
   *
   * The caller is responsible for holding a reference count for this new
   * IOBuf.  The IOBuf constructor does not automatically increment the
   * reference count.
   */
  struct InternalConstructor {}; // avoid conflicts
  IOBuf(
      InternalConstructor,
      SharedInfo* sharedInfo,
      uint8_t* buf,
      std::size_t capacity,
      uint8_t* data,
      std::size_t length) noexcept;

  FOLLY_ALWAYS_INLINE void incrementRefcount() const noexcept {
    if (sharedInfo_) {
      sharedInfo_->refcount.fetch_add(1, std::memory_order_relaxed);
    }
  }

  FOLLY_ALWAYS_INLINE void unlinkAll() noexcept {
    while (next_ != this) {
      (void)next_->unlink();
    }
  }

  FOLLY_ALWAYS_INLINE void setBuffer(
      SharedInfo* const& sharedInfo,
      uint8_t* const& buf,
      const std::size_t& capacity,
      uint8_t* const& data,
      const std::size_t& length) noexcept {
    data_ = data;
    buf_ = buf;
    length_ = length;
    capacity_ = capacity;
    sharedInfo_ = sharedInfo;
  }

  // By-value arguments preserve the source fields across destination cleanup.
  FOLLY_ALWAYS_INLINE void assignBuffer(
      SharedInfo* sharedInfo,
      uint8_t* buf,
      uint8_t* data,
      std::size_t capacity,
      std::size_t length) noexcept {
    unlinkAll();
    decrementRefcount();
    setBuffer(sharedInfo, buf, capacity, data, length);
  }

  void unshareOneSlow();
  void unshareChained();
  void makeManagedChained();
  void coalesceSlow();
  void coalesceSlow(size_t maxLength);
  // newLength must be the entire length of the buffers between this and
  // end (no truncation)
  void coalesceAndReallocate(
      size_t newHeadroom, size_t newLength, IOBuf* end, size_t newTailroom);
  void coalesceAndReallocate(size_t newLength, IOBuf* end) {
    coalesceAndReallocate(headroom(), newLength, end, end->prev_->tailroom());
  }
  void decrementRefcount() noexcept;
  void reserveSlow(std::size_t minHeadroom, std::size_t minTailroom);
  void freeExtBuffer() noexcept;

  static size_t goodExtBufferSize(std::size_t minCapacity);
  static void initExtBuffer(
      uint8_t* buf,
      size_t mallocSize,
      SharedInfo** infoReturn,
      std::size_t* capacityReturn);
  static void allocExtBuffer(
      std::size_t minCapacity,
      uint8_t** bufReturn,
      SharedInfo** infoReturn,
      std::size_t* capacityReturn);
  static void decrementStorageRefcount(HeapStorage* storage) noexcept;

  /*
   * Member variables
   */

  /*
   * A pointer to the start of the data referenced by this IOBuf, and the
   * length of the data.
   *
   * This may refer to any subsection of the actual buffer capacity.
   */
  std::size_t length_{0};
  uint8_t* data_{nullptr};

  std::size_t capacity_{0};
  uint8_t* buf_{nullptr};

  /*
   * Links to the next and the previous IOBuf in this chain.
   *
   * The chain is circularly linked (the last element in the chain points back
   * at the head), and next_ and prev_ can never be null.  If this IOBuf is the
   * only element in the chain, next_ and prev_ will both point to this.
   */
  IOBuf* next_{this};
  IOBuf* prev_{this};

  SharedInfo* sharedInfo_{nullptr};

  struct DeleterBase {
    virtual ~DeleterBase() {}
    virtual void dispose(void* p) noexcept = 0;
  };

  template <class UniquePtr>
  struct UniquePtrDeleter : public DeleterBase {
    using Pointer = typename UniquePtr::pointer;
    using Deleter = typename UniquePtr::deleter_type;

    explicit UniquePtrDeleter(Deleter deleter) : deleter_(std::move(deleter)) {}
    void dispose(void* p) noexcept override {
      deleter_(static_cast<Pointer>(p));
      delete this;
    }

   private:
    Deleter deleter_;
  };

  static void freeUniquePtrBuffer(void* ptr, void* userData) noexcept {
    static_cast<DeleterBase*>(userData)->dispose(ptr);
  }
};

/**
 * Hasher for IOBuf objects. Hashes the entire chain using SpookyHashV2.
 */
struct IOBufHash {
  size_t operator()(const IOBuf& buf) const noexcept;
  size_t operator()(const std::unique_ptr<IOBuf>& buf) const noexcept {
    return operator()(buf.get());
  }
  size_t operator()(const IOBuf* buf) const noexcept {
    return buf ? (*this)(*buf) : 0;
  }
};

/**
 * Ordering for IOBuf objects. Compares data in the entire chain.
 */
struct IOBufCompare {
  ordering operator()(const IOBuf& a, const IOBuf& b) const {
    return &a == &b ? ordering::eq : impl(a, b);
  }
  ordering operator()(
      const std::unique_ptr<IOBuf>& a, const std::unique_ptr<IOBuf>& b) const {
    return operator()(a.get(), b.get());
  }
  ordering operator()(const IOBuf* a, const IOBuf* b) const {
    // clang-format off
    return
        !a && !b ? ordering::eq :
        !a && b ? ordering::lt :
        a && !b ? ordering::gt :
        operator()(*a, *b);
    // clang-format on
  }

 private:
  ordering impl(IOBuf const& a, IOBuf const& b) const noexcept;
};

/**
 * Equality predicate for IOBuf objects. Compares data in the entire chain.
 */
struct IOBufEqualTo : compare_equal_to<IOBufCompare> {};

/**
 * Inequality predicate for IOBuf objects. Compares data in the entire chain.
 */
struct IOBufNotEqualTo : compare_not_equal_to<IOBufCompare> {};

/**
 * Less predicate for IOBuf objects. Compares data in the entire chain.
 */
struct IOBufLess : compare_less<IOBufCompare> {};

/**
 * At-most predicate for IOBuf objects. Compares data in the entire chain.
 */
struct IOBufLessEqual : compare_less_equal<IOBufCompare> {};

/**
 * Greater predicate for IOBuf objects. Compares data in the entire chain.
 */
struct IOBufGreater : compare_greater<IOBufCompare> {};

/**
 * At-least predicate for IOBuf objects. Compares data in the entire chain.
 */
struct IOBufGreaterEqual : compare_greater_equal<IOBufCompare> {};

template <class UniquePtr>
typename std::enable_if<
    detail::IsUniquePtrToSL<UniquePtr>::value,
    std::unique_ptr<IOBuf>>::type
IOBuf::takeOwnership(UniquePtr&& buf, size_t count) {
  size_t size = count * sizeof(typename UniquePtr::element_type);
  auto deleter = new UniquePtrDeleter<UniquePtr>(buf.get_deleter());
  return takeOwnership(
      buf.release(), size, &IOBuf::freeUniquePtrBuffer, deleter);
}

inline std::unique_ptr<IOBuf> IOBuf::copyBuffer(
    const void* data,
    std::size_t size,
    std::size_t headroom,
    std::size_t minTailroom) {
  std::size_t capacity;
  if (!folly::checked_add(&capacity, size, headroom, minTailroom)) {
    throw_exception(std::length_error(""));
  }
  std::unique_ptr<IOBuf> buf = create(capacity);
  buf->advance(headroom);
  if (size != 0) {
    memcpy(buf->writableData(), data, size);
  }
  buf->append(size);
  return buf;
}

inline std::unique_ptr<IOBuf> IOBuf::copyBuffer(
    StringPiece buf, std::size_t headroom, std::size_t minTailroom) {
  return copyBuffer(buf.data(), buf.size(), headroom, minTailroom);
}

inline std::unique_ptr<IOBuf> IOBuf::maybeCopyBuffer(
    StringPiece buf, std::size_t headroom, std::size_t minTailroom) {
  if (buf.empty()) {
    return nullptr;
  }
  return copyBuffer(buf.data(), buf.size(), headroom, minTailroom);
}

class IOBuf::Iterator
    : public detail::IteratorFacade<
          IOBuf::Iterator,
          ByteRange const,
          std::forward_iterator_tag> {
 public:
  // Note that IOBufs are stored as a circular list without a guard node,
  // so pos == end is ambiguous (it may mean "begin" or "end").  To solve
  // the ambiguity (at the cost of one extra comparison in the "increment"
  // code path), we define end iterators as having pos_ == end_ == nullptr
  // and we only allow forward iteration.
  explicit Iterator(const IOBuf* pos, const IOBuf* end) : pos_(pos), end_(end) {
    // Sadly, we must return by const reference, not by value.
    if (pos_) {
      setVal();
    }
  }

  Iterator() {}

  Iterator(Iterator const& rhs) : Iterator(rhs.pos_, rhs.end_) {}

  Iterator& operator=(Iterator const& rhs) {
    pos_ = rhs.pos_;
    end_ = rhs.end_;
    if (pos_) {
      setVal();
    }
    return *this;
  }

  const ByteRange& dereference() const { return val_; }

  bool equal(const Iterator& other) const {
    // We must compare end_ in addition to pos_, because forward traversal
    // requires that if two iterators are equal (a == b) and dereferenceable,
    // then ++a == ++b.
    return pos_ == other.pos_ && end_ == other.end_;
  }

  void increment() {
    pos_ = pos_->next();
    adjustForEnd();
  }

 private:
  void setVal() { val_ = ByteRange(pos_->data(), pos_->tail()); }

  void adjustForEnd() {
    if (pos_ == end_) {
      pos_ = end_ = nullptr;
      val_ = ByteRange();
    } else {
      setVal();
    }
  }

  const IOBuf* pos_{nullptr};
  const IOBuf* end_{nullptr};
  ByteRange val_;
};

inline IOBuf::Iterator IOBuf::begin() const {
  return cbegin();
}
inline IOBuf::Iterator IOBuf::end() const {
  return cend();
}

template <class Container>
void IOBuf::appendTo(Container& container) const {
  static_assert(
      (std::is_same<typename Container::value_type, char>::value ||
       std::is_same<typename Container::value_type, unsigned char>::value),
      "Unsupported value type");
  container.reserve(container.size() + computeChainDataLength());
  using Ptr = const typename Container::value_type*;
  for (auto data : *this) {
    container.insert(
        container.end(),
        reinterpret_cast<Ptr>(data.begin()),
        reinterpret_cast<Ptr>(data.end()));
  }
}

template <class Container>
Container IOBuf::to() const {
  Container result;
  appendTo(result);
  return result;
}

using IOBufFactory = Function<std::unique_ptr<IOBuf>(size_t capacity)>;

} // namespace folly

FOLLY_POP_WARNING
