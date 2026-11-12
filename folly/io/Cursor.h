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

#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>

#include <folly/Likely.h>
#include <folly/Memory.h>
#include <folly/Portability.h>
#include <folly/Range.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <folly/lang/Bits.h>
#include <folly/lang/Exception.h>

/**
 * IOBuf Cursors provide fast iteration over IOBuf chains.
 *
 * - Cursor          - Read-only access
 * - RWPrivateCursor - Read-write access, assumes private access to IOBuf chain
 * - RWUnshareCursor - Read-write access, calls unshare on write (COW)
 * - Appender        - Write access, assumes private access to IOBuf chain
 *
 * Note that RW cursors write in the preallocated part of buffers (that is,
 * between the buffer's data() and tail()), while Appenders append to the end
 * of the buffer (between the buffer's tail() and bufferEnd()).  Appenders
 * automatically adjust the buffer pointers, so you may only use one
 * Appender with a buffer chain; for this reason, Appenders assume private
 * access to the buffer (you need to call unshare() yourself if necessary).
 *
 * @file Cursor.h
 */

namespace folly {
namespace io {

class Cursor;
class ThinCursor;

// This is very useful in development, but the size perturbation is currently
// causing some previously undetected bugs in unrelated projects to manifest in
// CI-breaking ways.
// TODO(davidgoldblatt): Fix this.
#define FOLLY_IO_CURSOR_BORROW_CHECKING 0
#if FOLLY_IO_CURSOR_BORROW_CHECKING
#define FOLLY_IO_CURSOR_BORROW_DCHECK DCHECK
#else
#define FOLLY_IO_CURSOR_BORROW_DCHECK(ignored)
#endif

/**
 * Whether a clone may point at buffers that IOBuf does not own.
 *
 * Cloning shares the underlying buffers, which keeps the data alive only if
 * those buffers are managed (that is, reference counted by IOBuf).
 *
 * Requesting Managed instead yields a clone that owns its data: managed
 * buffers are still shared, and data backed by user-owned memory is copied out
 * into buffers sized to hold just the cloned range.
 */
enum class CloneOwnership {
  /// Share the buffers. The clone is valid only while unmanaged data lives.
  Shared,
  /// Copy out unmanaged data, so that the clone owns everything it points at.
  Managed,
};

template <class Derived, class BufType>
class CursorBase {
  // Make all the templated classes friends for copy constructor.
  template <class D, typename B>
  friend class CursorBase;

 protected:
  /**
   * Construct a cursor wrapping an IOBuf.
   */
  explicit CursorBase(BufType* buf) noexcept : crtBuf_(buf), buffer_(buf) {
    if (crtBuf_) {
      crtPos_ = crtBegin_ = crtBuf_->data();
      crtEnd_ = crtBuf_->tail();
    }
  }

  /**
   * Constuct a bounded cursor wrapping an IOBuf.
   *
   * @param len An upper bound on the number of bytes available to this cursor.
   */
  CursorBase(BufType* buf, size_t len) noexcept : crtBuf_(buf), buffer_(buf) {
    if (crtBuf_) {
      crtPos_ = crtBegin_ = crtBuf_->data();
      crtEnd_ = crtBuf_->tail();
      if (uintptr_t(crtPos_) + len < uintptr_t(crtEnd_)) {
        crtEnd_ = crtPos_ + len;
      }
      remainingLen_ = len - (crtEnd_ - crtPos_);
    }
  }

  /**
   * Copy constructor.
   *
   * This also allows constructing a CursorBase from other derived types.
   * For instance, this allows constructing a Cursor from an RWPrivateCursor.
   */
  template <class OtherDerived, class OtherBuf>
  explicit CursorBase(const CursorBase<OtherDerived, OtherBuf>& cursor) noexcept
      : crtBuf_(cursor.crtBuf_),
        buffer_(cursor.buffer_),
        crtBegin_(cursor.crtBegin_),
        crtEnd_(cursor.crtEnd_),
        crtPos_(cursor.crtPos_),
        absolutePos_(cursor.absolutePos_),
        remainingLen_(cursor.remainingLen_) {}

  template <class OtherDerived, class OtherBuf>
  explicit CursorBase(
      const CursorBase<OtherDerived, OtherBuf>& cursor, size_t len)
      : crtBuf_(cursor.crtBuf_),
        buffer_(cursor.buffer_),
        crtBegin_(cursor.crtBegin_),
        crtEnd_(cursor.crtEnd_),
        crtPos_(cursor.crtPos_),
        absolutePos_(cursor.absolutePos_) {
    if (cursor.isBounded() && len > cursor.remainingLen_ + cursor.length()) {
      throw_exception<std::out_of_range>("underflow");
    }
    if (uintptr_t(crtPos_) + len < uintptr_t(crtEnd_)) {
      crtEnd_ = crtPos_ + len;
    }
    remainingLen_ = len - (crtEnd_ - crtPos_);
  }

 public:
  /**
   * Reset cursor to point to a new buffer.
   *
   * @methodset Modifiers
   */
  void reset(BufType* buf) noexcept {
    crtBuf_ = buf;
    buffer_ = buf;
    absolutePos_ = 0;
    remainingLen_ = std::numeric_limits<size_t>::max();
    if (crtBuf_) {
      crtPos_ = crtBegin_ = crtBuf_->data();
      crtEnd_ = crtBuf_->tail();
    }
  }

  /**
   * Get the Cursor position relative to the IOBuf chain's currentBuffer().
   *
   * @methodset Advanced
   */
  size_t getPositionInCurrentBuffer() const noexcept {
    dcheckIntegrity();
    return crtPos_ - crtBegin_;
  }

  /**
   * Get the current Cursor position relative to the head of IOBuf chain.
   *
   * @methodset Capacity
   */
  size_t getCurrentPosition() const noexcept {
    return getPositionInCurrentBuffer() + absolutePos_;
  }

  /**
   * Get the data at the current cursor position.
   *
   * @methodset Accessors
   */
  const uint8_t* data() const noexcept {
    dcheckIntegrity();
    return crtPos_;
  }

  /**
   * Get the buffer in the IOBuf chain this Cursor currently points into.
   *
   * @methodset Advanced
   */
  const folly::IOBuf* currentBuffer() const noexcept { return crtBuf_; }

  /**
   * Return the remaining space available in the current IOBuf.
   *
   * @methodset Capacity
   *
   * May return 0 if the cursor is at the end of an IOBuf.  Use peekBytes()
   * instead if you want to avoid this.  peekBytes() will advance to the next
   * non-empty IOBuf (up to the end of the chain) if the cursor is currently
   * pointing at the end of a buffer.
   */
  size_t length() const noexcept {
    dcheckIntegrity();
    return crtEnd_ - crtPos_;
  }

  /**
   * Return the space available until the end of the entire IOBuf chain.
   *
   * @methodset Capacity
   *
   * For bounded Cursors, return the available space until the boundary.
   */
  size_t totalLength() const noexcept {
    size_t len = 0;
    const IOBuf* buf = crtBuf_->next();
    while (buf != buffer_ && len < remainingLen_) {
      len += buf->length();
      buf = buf->next();
    }
    return std::min(len, remainingLen_) + length();
  }

  /**
   * Return true if the cursor could advance the specified number of bytes
   * from its current position.
   *
   * @methodset Capacity
   *
   * This is useful for applications that want to do checked reads instead of
   * catching exceptions and is more efficient than using totalLength as it
   * walks the minimal set of buffers in the chain to determine the result.
   */
  bool canAdvance(size_t amount) const noexcept {
    if (isBounded() && amount > remainingLen_ + length()) {
      return false;
    }
    const IOBuf* nextBuf = crtBuf_;
    size_t available = length();
    do {
      if (available >= amount) {
        return true;
      }
      amount -= available;
      nextBuf = nextBuf->next();
      available = nextBuf->length();
    } while (nextBuf != buffer_);
    return false;
  }

  /**
   * Return true if the cursor is at the end of the entire IOBuf chain.
   *
   * @methodset Capacity
   */
  bool isAtEnd() const noexcept {
    dcheckIntegrity();
    // Check for the simple cases first.
    if (crtPos_ != crtEnd_) {
      return false;
    }
    if (crtBuf_ == buffer_->prev()) {
      return true;
    }
    if (isBounded() && remainingLen_ == 0) {
      return true;
    }
    // We are at the end of a buffer, but it isn't the last buffer.
    // We might still be at the end if the remaining buffers in the chain are
    // empty.
    const IOBuf* buf = crtBuf_->next();
    while (buf != buffer_) {
      if (buf->length() > 0) {
        return false;
      }
      buf = buf->next();
    }
    return true;
  }

  /**
   * Advances the cursor to the end of the entire IOBuf chain.
   *
   * @methodset Modifiers
   */
  void advanceToEnd() {
    // Simple case, we're already in the last IOBuf.
    if (crtBuf_ == buffer_->prev()) {
      crtPos_ = crtEnd_;
      return;
    }

    auto* nextBuf = crtBuf_->next();
    while (nextBuf != buffer_) {
      if (isBounded() && remainingLen_ == 0) {
        crtPos_ = crtEnd_;
        return;
      }
      absolutePos_ += crtEnd_ - crtBegin_;

      crtBuf_ = nextBuf;
      nextBuf = crtBuf_->next();
      crtBegin_ = crtBuf_->data();
      crtEnd_ = crtBuf_->tail();
      if (isBounded()) {
        if (uintptr_t(crtBegin_) + remainingLen_ < uintptr_t(crtEnd_)) {
          crtEnd_ = crtBegin_ + remainingLen_;
        }
        remainingLen_ -= crtEnd_ - crtBegin_;
      }
      crtPos_ = crtEnd_;
      derived().advanceDone();
    }
  }

  /// Advance the cursor
  ///
  /// @methodset Modifiers
  Derived& operator+=(size_t offset) {
    Derived* p = static_cast<Derived*>(this);
    p->skip(offset);
    return *p;
  }
  /// Get a new cursor, advanced by offset from this cursor.
  ///
  /// @methodset Modifiers
  Derived operator+(size_t offset) const {
    Derived other(*this);
    other.skip(offset);
    return other;
  }

  /// Retreat the cursor
  ///
  /// @methodset Modifiers
  Derived& operator-=(size_t offset) {
    Derived* p = static_cast<Derived*>(this);
    p->retreat(offset);
    return *p;
  }
  /// Get a new cursor, retreated by offset from this cursor.
  ///
  /// @methodset Modifiers
  Derived operator-(size_t offset) const {
    Derived other(*this);
    other.retreat(offset);
    return other;
  }

  /**
   * Compare cursors for equality/inequality.
   *
   * @methodset Comparison
   *
   * Two cursors are equal if they are pointing to the same location in the
   * same IOBuf chain.
   */
  bool operator==(const CursorBase& other) const {
    const IOBuf* crtBuf = crtBuf_;
    auto crtPos = crtPos_;
    // We can be pointing to the end of a buffer chunk, find first non-empty.
    while (crtPos == crtBuf->tail() && crtBuf != buffer_->prev()) {
      crtBuf = crtBuf->next();
      crtPos = crtBuf->data();
    }

    const IOBuf* crtBufOther = other.crtBuf_;
    auto crtPosOther = other.crtPos_;
    // We can be pointing to the end of a buffer chunk, find first non-empty.
    while (crtPosOther == crtBufOther->tail() &&
           crtBufOther != other.buffer_->prev()) {
      crtBufOther = crtBufOther->next();
      crtPosOther = crtBufOther->data();
    }
    return (crtPos == crtPosOther) && (crtBuf == crtBufOther);
  }

  /**
   * Attempt to read from the cursor.
   *
   * @methodset Consumers
   *
   * @param[out] val  Store the read value in this location.
   * @return True iff successful; If there are not enough bytes left in the
   * cursor, return false.
   * @note val might be modified even if tryRead returns false.
   */
  template <class T>
  typename std::enable_if<std::is_arithmetic<T>::value, bool>::type tryRead(
      T& val) {
    if (uintptr_t(crtPos_) + sizeof(T) <= uintptr_t(crtEnd_)) [[likely]] {
      val = loadUnaligned<T>(data());
      crtPos_ += sizeof(T);
      return true;
    }
    return pullAtMostSlow(&val, sizeof(T)) == sizeof(T);
  }

  /**
   * Attempt to read a Big-Endian integral from the cursor.
   *
   * @methodset Consumers
   *
   * @see tryRead
   */
  template <class T>
  bool tryReadBE(T& val) {
    const bool result = tryRead(val);
    val = Endian::big(val);
    return result;
  }

  /**
   * Attempt to read a Little-Endian integral from the cursor.
   *
   * @methodset Consumers
   *
   * @see tryRead
   */
  template <class T>
  bool tryReadLE(T& val) {
    const bool result = tryRead(val);
    val = Endian::little(val);
    return result;
  }

  /**
   * Read a value from the cursor.
   *
   * @methodset Consumers
   *
   * This function only works with types that are bit-copyable: it calls memcpy
   * to reinterpret bits from the IOBuf as T.
   *
   * @throws out_of_range if there aren't enough bytes left in the cursor.
   */
  template <class T>
  T read() {
    if (uintptr_t(crtPos_) + sizeof(T) <= uintptr_t(crtEnd_)) [[likely]] {
      T val = loadUnaligned<T>(data());
      crtPos_ += sizeof(T);
      return val;
    } else {
      return readSlow<T>();
    }
  }

  /**
   * Read a Big-Endian integral from the cursor.
   *
   * @methodset Consumers
   *
   * @see read
   */
  template <class T>
  T readBE() {
    return Endian::big(read<T>());
  }

  /**
   * Read a Little-Endian integral from the cursor.
   *
   * @methodset Consumers
   *
   * @see read
   */
  template <class T>
  T readLE() {
    return Endian::little(read<T>());
  }

  /**
   * Read a fixed-length string.
   *
   * @methodset Consumers
   *
   * The std::string-based APIs should probably be avoided unless you
   * ultimately want the data to live in an std::string. You're better off
   * using the pull() APIs to copy into a raw buffer otherwise.
   */
  std::string readFixedString(size_t len) {
    std::string str;
    str.reserve(len);
    if (length() >= len) [[likely]] {
      str.append(reinterpret_cast<const char*>(data()), len);
      crtPos_ += len;
    } else {
      readFixedStringSlow(&str, len);
    }
    return str;
  }

  /**
   * Read a string of bytes until the given terminator character is seen.
   *
   * @methodset Consumers
   *
   * Raises an std::length_error if maxLength bytes have been processed
   * before the terminator is seen.
   *
   * See comments in readFixedString() about when it's appropriate to use this
   * vs. using pull().
   */
  std::string readTerminatedString(
      char termChar = '\0',
      size_t maxLength = std::numeric_limits<size_t>::max());

  /**
   * @overloadbrief Read bytes until the specified predicate returns true.
   *
   * @methodset Consumers
   *
   * The predicate will be called on each byte in turn, until it returns false
   * or until the end of the IOBuf chain is reached.
   *
   * Returns the result as a string.
   */
  template <typename Predicate>
  std::string readWhile(const Predicate& predicate);

  /**
   * This is a more generic version of readWhile(). It takes an arbitrary Output
   * object, and calls Output::append() with each chunk of matching data.
   */
  template <typename Predicate, typename Output>
  void readWhile(const Predicate& predicate, Output& out);

  /**
   * Skip bytes until the specified predicate returns true.
   *
   * @methodset Consumers
   *
   * The predicate will be called on each byte in turn, until it returns false
   * or until the end of the IOBuf chain is reached.
   */
  template <typename Predicate>
  void skipWhile(const Predicate& predicate);

  /**
   * Advance the cursor by at most len bytes.
   *
   * @methodset Modifiers
   */
  size_t skipAtMost(size_t len) {
    dcheckIntegrity();
    if (uintptr_t(crtPos_) + len < uintptr_t(crtEnd_)) [[likely]] {
      crtPos_ += len;
      return len;
    }
    return skipAtMostSlow(len);
  }

  /**
   * Advance the cursor by len bytes.
   *
   * @methodset Modifiers
   *
   * @throws out_of_range if there aren't enough bytes left in the cursor.
   */
  void skip(size_t len) {
    dcheckIntegrity();
    if (uintptr_t(crtPos_) + len < uintptr_t(crtEnd_)) [[likely]] {
      crtPos_ += len;
    } else {
      skipSlow(len);
    }
  }

  /**
   * Skip bytes in the current IOBuf without advancing to the next one.
   *
   * @methodset Modifiers
   *
   * @pre length() >= len
   */
  void skipNoAdvance(size_t len) {
    DCHECK_LE(len, length());
    crtPos_ += len;
  }

  /**
   * Retreat the cursor by at most len bytes.
   *
   * @methodset Modifiers
   */
  size_t retreatAtMost(size_t len) {
    if (len <= getPositionInCurrentBuffer()) {
      crtPos_ -= len;
      return len;
    }
    return retreatAtMostSlow(len);
  }

  /**
   * Retreat the cursor by at most len bytes.
   *
   * @methodset Modifiers
   *
   * @throws out_of_range if the cursor doesn't have enough bytes to retreat.
   */
  void retreat(size_t len) {
    if (len <= getPositionInCurrentBuffer()) {
      crtPos_ -= len;
    } else {
      retreatSlow(len);
    }
  }

  /**
   * Copies at most len bytes from the cursor.
   *
   * @methodset Consumers
   *
   * The cursor will advance by the number of bytes read.
   *
   * @param[out] buf The buffer into which the bytes are copied.
   * @returns The number of bytes copied.
   */
  size_t pullAtMost(void* buf, size_t len) {
    if (len == 0) [[unlikely]] {
      return 0;
    }
    dcheckIntegrity();
    // Fast path: it all fits in one buffer.
    if (uintptr_t(crtPos_) + len <= uintptr_t(crtEnd_)) [[likely]] {
      memcpy(buf, data(), len);
      crtPos_ += len;
      return len;
    }
    return pullAtMostSlow(buf, len);
  }

  /**
   * Copies len bytes from the cursor.
   *
   * @methodset Consumers
   *
   * The cursor will advance by len.
   *
   * @param[out] buf The buffer into which the bytes are copied.
   * @throw out_of_range if there aren't enough bytes in the cursor.
   */
  void pull(void* buf, size_t len) {
    if (len == 0) [[unlikely]] {
      return;
    }
    dcheckIntegrity();
    if (uintptr_t(crtPos_) + len <= uintptr_t(crtEnd_)) [[likely]] {
      memcpy(buf, data(), len);
      crtPos_ += len;
    } else {
      pullSlow(buf, len);
    }
  }

  /**
   * Return the available data in the current IOBuf.
   *
   * @methodset Accessors
   *
   * Unlike data(), peekBytes() will advance to the next IOBuf while length()==0
   * (though it can still return an empty range if there are no bytes left to
   * read in the whole IOBuf chain).
   *
   * If you want to gather more data from the chain into a contiguous region
   * (for hopefully zero-copy access), use gather() before peekBytes().
   */
  ByteRange peekBytes() {
    // Ensure that we're pointing to valid data
    size_t available = length();
    if (!available) [[unlikely]] {
      available = peekBytesSlow();
    }
    return ByteRange{data(), available};
  }

  /**
   * Alternate version of peekBytes() that returns a std::span
   * instead of a ByteRage.
   *
   * @methodset Accessors
   */
  std::span<uint8_t const> peek() {
    auto bytes = peekBytes();
    return {bytes.data(), bytes.size()};
  }

  /**
   * Clone len bytes from this cursor into an IOBuf.
   *
   * @methodset Accessors
   *
   * @param[out] buf The IOBuf into which to place the cloned data.
   * @param ownership Whether the clone may point at data that IOBuf does not
   *                  own. @see CloneOwnership.
   * @throws out_of_range if there aren't enough bytes in this cursor.
   */
  void clone(
      std::unique_ptr<folly::IOBuf>& buf,
      size_t len,
      CloneOwnership ownership = CloneOwnership::Shared) {
    if (cloneAtMost(buf, len, ownership) != len) [[unlikely]] {
      throw_exception<std::out_of_range>("underflow");
    }
  }

  void clone(
      folly::IOBuf& buf,
      size_t len,
      CloneOwnership ownership = CloneOwnership::Shared) {
    if (cloneAtMost(buf, len, ownership) != len) [[unlikely]] {
      throw_exception<std::out_of_range>("underflow");
    }
  }

  /**
   * Clone at most len bytes from this cursor into an IOBuf.
   *
   * @methodset Accessors
   *
   * @param[out] buf The IOBuf into which to place the cloned data.
   * @param ownership Whether the clone may point at data that IOBuf does not
   *                  own. @see CloneOwnership.
   * @return The number of bytes actually cloned.
   */
  size_t cloneAtMost(
      folly::IOBuf& buf,
      size_t len,
      CloneOwnership ownership = CloneOwnership::Shared) {
    return ownership == CloneOwnership::Managed
        ? cloneManagedAtMost(buf, len)
        : cloneSharedAtMost(buf, len);
  }

  size_t cloneAtMost(
      std::unique_ptr<folly::IOBuf>& buf,
      size_t len,
      CloneOwnership ownership = CloneOwnership::Shared) {
    if (!buf) {
      buf = std::make_unique<folly::IOBuf>();
    }
    return cloneAtMost(*buf, len, ownership);
  }

  /**
   * Return the distance between two cursors.
   */
  size_t operator-(const CursorBase& other) const {
    BufType* otherBuf = other.crtBuf_;
    size_t len = 0;

    if (otherBuf != crtBuf_) {
      if (other.remainingLen_ == 0) {
        len += otherBuf->tail() - other.crtPos_;
      } else {
        len += other.crtEnd_ - other.crtPos_;
      }

      for (otherBuf = otherBuf->next();
           otherBuf != crtBuf_ && otherBuf != other.buffer_;
           otherBuf = otherBuf->next()) {
        len += otherBuf->length();
      }

      if (otherBuf == other.buffer_) {
        throw_exception<std::out_of_range>("wrap-around");
      }

      len += crtPos_ - crtBegin_;
    } else {
      if (crtPos_ < other.crtPos_) {
        throw_exception<std::out_of_range>("underflow");
      }

      len += crtPos_ - other.crtPos_;
    }

    return len;
  }

  /**
   * Return the distance from the given IOBuf to the this cursor.
   */
  size_t operator-(const BufType* buf) const {
    size_t len = 0;

    const BufType* curBuf = buf;
    while (curBuf != crtBuf_) {
      len += curBuf->length();
      curBuf = curBuf->next();
      if (curBuf == buf || curBuf == buffer_) {
        throw_exception<std::out_of_range>("wrap-around");
      }
    }

    len += crtPos_ - crtBegin_;
    return len;
  }

  /**
   * Check if this cursor has a size limit imposed on it.
   *
   * @methodset Configuration
   */
  bool isBounded() const {
    return remainingLen_ != std::numeric_limits<size_t>::max();
  }

 protected:
  void dcheckIntegrity() const {
    FOLLY_IO_CURSOR_BORROW_DCHECK(!*borrowed());
    DCHECK(crtBegin_ <= crtPos_ && crtPos_ <= crtEnd_);
    DCHECK(crtBuf_ == nullptr || crtBegin_ == crtBuf_->data());
    DCHECK(
        crtBuf_ == nullptr ||
        (std::size_t)(crtEnd_ - crtBegin_) <= crtBuf_->length());
  }

  CursorBase(CursorBase const&) = default;
  CursorBase(CursorBase&&) = default;
  CursorBase& operator=(CursorBase const&) = default;
  CursorBase& operator=(CursorBase&&) = default;

  ~CursorBase() = default;

  BufType* head() { return buffer_; }

  bool tryAdvanceBuffer() {
    BufType* nextBuf = crtBuf_->next();
    if (FOLLY_UNLIKELY(nextBuf == buffer_) || remainingLen_ == 0) {
      crtPos_ = crtEnd_;
      return false;
    }

    absolutePos_ += crtEnd_ - crtBegin_;
    crtBuf_ = nextBuf;
    crtPos_ = crtBegin_ = crtBuf_->data();
    crtEnd_ = crtBuf_->tail();
    if (isBounded()) {
      if (uintptr_t(crtPos_) + remainingLen_ < uintptr_t(crtEnd_)) {
        crtEnd_ = crtPos_ + remainingLen_;
      }
      remainingLen_ -= crtEnd_ - crtPos_;
    }
    derived().advanceDone();
    return true;
  }

  bool tryRetreatBuffer() {
    if (crtBuf_ == buffer_) [[unlikely]] {
      crtPos_ = crtBegin_;
      return false;
    }
    if (isBounded()) {
      remainingLen_ += crtEnd_ - crtBegin_;
    }
    crtBuf_ = crtBuf_->prev();
    crtBegin_ = crtBuf_->data();
    crtPos_ = crtEnd_ = crtBuf_->tail();
    absolutePos_ -= crtEnd_ - crtBegin_;
    derived().advanceDone();
    return true;
  }

  void advanceBufferIfEmpty() {
    dcheckIntegrity();
    if (crtPos_ == crtEnd_) {
      tryAdvanceBuffer();
    }
  }

  BufType* crtBuf_;
  BufType* buffer_;
  const uint8_t* crtBegin_{nullptr};
  const uint8_t* crtEnd_{nullptr};
  const uint8_t* crtPos_{nullptr};
  size_t absolutePos_{0};
  // For bounded Cursor, remainingLen_ is the remaining number of data bytes
  // in subsequent IOBufs in the chain. For unbounded Cursor, remainingLen_
  // is set to the max of size_t
  size_t remainingLen_{std::numeric_limits<size_t>::max()};

#if FOLLY_IO_CURSOR_BORROW_CHECKING
  bool borrowed_ = false;
  bool* borrowed() { return &borrowed_; }
  const bool* borrowed() const { return &borrowed_; }
#else
  bool* borrowed() { return nullptr; }
  const bool* borrowed() const { return nullptr; }
#endif

 private:
  Derived& derived() { return static_cast<Derived&>(*this); }

  Derived const& derived() const { return static_cast<const Derived&>(*this); }

  template <class T>
  FOLLY_NOINLINE T readSlow() {
    T val;
    pullSlow(&val, sizeof(T));
    return val;
  }

  FOLLY_NOINLINE void readFixedStringSlow(std::string* str, size_t len) {
    for (size_t available; (available = length()) < len;) {
      str->append(reinterpret_cast<const char*>(data()), available);
      if (!tryAdvanceBuffer()) [[unlikely]] {
        throw_exception<std::out_of_range>("string underflow");
      }
      len -= available;
    }
    str->append(reinterpret_cast<const char*>(data()), len);
    crtPos_ += len;
    advanceBufferIfEmpty();
  }

  FOLLY_NOINLINE size_t pullAtMostSlow(void* buf, size_t len) {
    uint8_t* p = reinterpret_cast<uint8_t*>(buf);
    size_t copied = 0;
    for (size_t available; (available = length()) < len;) {
      if (available > 0) {
        // Don't try to copy from 0-length buffers, since they could have
        // a null data() pointer.
        memcpy(p, data(), available);
        copied += available;
      }
      if (!tryAdvanceBuffer()) [[unlikely]] {
        return copied;
      }
      p += available;
      len -= available;
    }
    if (len > 0) {
      // Don't try to copy from 0-length buffers, since they could have
      // a null data() pointer.
      memcpy(p, data(), len);
      crtPos_ += len;
    }
    advanceBufferIfEmpty();
    return copied + len;
  }

  FOLLY_NOINLINE void pullSlow(void* buf, size_t len) {
    if (pullAtMostSlow(buf, len) != len) [[unlikely]] {
      throw_exception<std::out_of_range>("underflow");
    }
  }

  FOLLY_NOINLINE size_t skipAtMostSlow(size_t len) {
    size_t skipped = 0;
    for (size_t available; (available = length()) < len;) {
      skipped += available;
      if (!tryAdvanceBuffer()) [[unlikely]] {
        return skipped;
      }
      len -= available;
    }
    crtPos_ += len;
    advanceBufferIfEmpty();
    return skipped + len;
  }

  FOLLY_NOINLINE void skipSlow(size_t len) {
    if (skipAtMostSlow(len) != len) [[unlikely]] {
      throw_exception<std::out_of_range>("underflow");
    }
  }

  FOLLY_NOINLINE size_t retreatAtMostSlow(size_t len) {
    size_t retreated = 0;
    for (size_t available; (available = crtPos_ - crtBegin_) < len;) {
      retreated += available;
      if (!tryRetreatBuffer()) [[unlikely]] {
        return retreated;
      }
      len -= available;
    }
    crtPos_ -= len;
    return retreated + len;
  }

  FOLLY_NOINLINE void retreatSlow(size_t len) {
    if (retreatAtMostSlow(len) != len) [[unlikely]] {
      throw_exception<std::out_of_range>("underflow");
    }
  }

  void advanceDone() {}

  FOLLY_NOINLINE size_t peekBytesSlow() {
    size_t available = 0;
    while (available == 0 && tryAdvanceBuffer()) {
      available = length();
    }
    return available;
  }

  // Places one piece of a clone: the first piece becomes the destination
  // itself, later ones are chained onto it.
  //
  // shareCurrent() points the piece at the n bytes under the cursor, which
  // keeps them alive only while the buffer holding them does. copyOut() copies
  // n bytes out of the cursor instead, into a buffer sized to hold just them,
  // and returns how many it got.
  void shareCurrent(folly::IOBuf& buf, size_t n, bool first) {
    if (first) {
      crtBuf_->cloneOneInto(buf);
      buf.trimStart(crtPos_ - crtBegin_);
      buf.trimEnd(buf.length() - n);
    } else {
      auto piece = crtBuf_->cloneOne();
      piece->trimStart(crtPos_ - crtBegin_);
      piece->trimEnd(piece->length() - n);
      buf.prependChain(std::move(piece));
    }
    crtPos_ += n;
  }

  size_t copyOut(folly::IOBuf& buf, size_t n, bool first) {
    size_t copied;
    if (first) {
      buf = folly::IOBuf(folly::IOBuf::CREATE, n);
      copied = pullAtMost(buf.writableTail(), n);
      buf.append(copied);
    } else {
      auto piece = folly::IOBuf::create(n);
      copied = pullAtMost(piece->writableTail(), n);
      piece->append(copied);
      buf.prependChain(std::move(piece));
    }
    return copied;
  }

  size_t cloneSharedAtMost(folly::IOBuf& buf, size_t len) {
    // We might be at the end of buffer.
    advanceBufferIfEmpty();

    size_t copied = 0;
    for (bool first = true;; first = false) {
      // Fast path: it all fits in one buffer.
      const size_t available = length();
      if (available >= len) [[likely]] {
        shareCurrent(buf, len, first);
        advanceBufferIfEmpty();
        return copied + len;
      }

      // A bounded cursor can stop short of the end of the buffer, in which
      // case the clone must stop there too.
      shareCurrent(buf, available, first);
      copied += available;
      if (!tryAdvanceBuffer()) [[unlikely]] {
        return copied;
      }
      len -= available;
    }
  }

  // Same as cloneSharedAtMost(), except that data the clone would not keep
  // alive is copied out instead of shared. Sizing each copy by the whole run
  // of adjacent unmanaged buffers keeps an all-unmanaged range down to one
  // allocation.
  size_t cloneManagedAtMost(folly::IOBuf& buf, size_t len) {
    // We might be at the end of buffer.
    advanceBufferIfEmpty();

    // The destination can be the very buffer being read, so build the clone
    // beside it and hand it over once there is nothing left to read.
    folly::IOBuf result;
    size_t copied = 0;
    for (bool first = true;; first = false) {
      // Step over empty buffers: only past them does a zero length mean that
      // the chain is exhausted, and that the buffer under the cursor is the
      // one whose ownership decides between sharing and copying.
      const size_t available = peekBytes().size();
      if (available == 0 && !first) {
        break; // The chain is exhausted.
      }

      size_t n;
      if (crtBuf_->isManagedOne()) {
        n = std::min(available, len);
        shareCurrent(result, n, first);
      } else {
        n = copyOut(result, unmanagedRunLength(len), first);
      }
      advanceBufferIfEmpty();

      copied += n;
      len -= n;
      if (len == 0) {
        break;
      }
    }

    buf = std::move(result);
    return copied;
  }

  // The number of bytes, at most len, held by the run of unmanaged buffers
  // starting at the current position.
  size_t unmanagedRunLength(size_t len) const {
    size_t run = std::min(length(), len);
    // remainingLen_ is the max of size_t unless the cursor is bounded, so it
    // caps the walk without a special case.
    size_t remaining = remainingLen_;
    for (const BufType* buf = crtBuf_->next(); run < len && remaining > 0;
         buf = buf->next()) {
      if (buf == buffer_ || buf->isManagedOne()) {
        break;
      }
      const size_t take =
          std::min(std::min(buf->length(), len - run), remaining);
      run += take;
      remaining -= take;
    }
    return run;
  }
};

namespace detail {
template <typename T>
ThinCursor thinCursorReadSlow(ThinCursor, T&, Cursor&);
ThinCursor thinCursorSkipSlow(ThinCursor, Cursor&, size_t);
} // namespace detail

// A register-pass facade for a Cursor. It strips out state that's only needed
// down slow paths, so that it can be passed and returned efficiently in
// registers. This gives up some convenience (users need to maintain that state
// in a fallback Cursor), to gain performance.
class ThinCursor {
 public:
  ThinCursor() = default;
  ThinCursor(ThinCursor&&) = default;
  ThinCursor(const ThinCursor&) = delete;
  ThinCursor& operator=(ThinCursor&&) = default;
  ThinCursor& operator=(const ThinCursor&) = delete;

  const uint8_t* data() const {
    dcheckIntegrity();
    return crtPos_;
  }

  size_t length() const {
    dcheckIntegrity();
    return crtEnd_ - crtPos_;
  }

  bool canAdvance(size_t amount) const { return amount <= length(); }

  bool isAtEnd() const { return length() == 0; }

  template <class T>
  FOLLY_ALWAYS_INLINE T read(Cursor& fallback) {
    if ((uintptr_t)crtEnd_ - (uintptr_t)crtPos_ >= sizeof(T)) [[likely]] {
      T result = loadUnaligned<T>(data());
      crtPos_ += sizeof(T);
      return result;
    } else {
      T result;
      *this = detail::thinCursorReadSlow(std::move(*this), result, fallback);
      return result;
    }
  }

  template <class T>
  T readBE(Cursor& fallback) {
    return Endian::big(read<T>(fallback));
  }

  template <class T>
  T readLE(Cursor& fallback) {
    return Endian::little(read<T>(fallback));
  }

  void skip(Cursor& fallback, size_t len) {
    dcheckIntegrity();
    if (uintptr_t(crtPos_) + len < uintptr_t(crtEnd_)) [[likely]] {
      crtPos_ += len;
    } else {
      *this = detail::thinCursorSkipSlow(std::move(*this), fallback, len);
    }
  }

  void skipNoAdvance(size_t len) {
    DCHECK_LE(len, length());
    crtPos_ += len;
  }

 private:
  void dcheckIntegrity() const { DCHECK(crtPos_ <= crtEnd_); }

  friend class Cursor;
  ThinCursor(const uint8_t* crtPos, const uint8_t* crtEnd) noexcept
      : crtPos_(crtPos), crtEnd_(crtEnd) {}
  // Note: these are the only fields we can have -- x86-64 calling convention
  // maxes out at returning 2 pointer-sized fields in registers, and we don't
  // want to have to use memory for returning these.
  const uint8_t* crtPos_;
  const uint8_t* crtEnd_;
};

class Cursor : public CursorBase<Cursor, const IOBuf> {
 public:
  using Buffer = IOBuf;

  explicit Cursor(const IOBuf* buf) noexcept
      : CursorBase<Cursor, const IOBuf>(buf) {}

  explicit Cursor(const IOBuf* buf, size_t len) noexcept
      : CursorBase<Cursor, const IOBuf>(buf, len) {}

  template <class OtherDerived, class OtherBuf>
  explicit Cursor(const CursorBase<OtherDerived, OtherBuf>& cursor) noexcept
      : CursorBase<Cursor, const IOBuf>(cursor) {}

  template <class OtherDerived, class OtherBuf>
  Cursor(const CursorBase<OtherDerived, OtherBuf>& cursor, size_t len)
      : CursorBase<Cursor, const IOBuf>(cursor, len) {}

  ThinCursor borrow() {
    FOLLY_IO_CURSOR_BORROW_DCHECK(!std::exchange(*borrowed(), true));
    return {crtPos_, crtEnd_};
  }

  void unborrow(ThinCursor&& cursor) {
    FOLLY_IO_CURSOR_BORROW_DCHECK(std::exchange(*borrowed(), false));
    DCHECK_EQ(cursor.crtEnd_, crtEnd_);
    crtPos_ = cursor.crtPos_;
  }
};

namespace detail {
template <typename T>
ThinCursor thinCursorReadSlow(ThinCursor borrowed, T& val, Cursor& fallback) {
  fallback.unborrow(std::move(borrowed));
  val = fallback.read<T>();
  return fallback.borrow();
}

inline ThinCursor thinCursorSkipSlow(
    ThinCursor borrowed, Cursor& fallback, size_t len) {
  fallback.unborrow(std::move(borrowed));
  fallback.skip(len);
  return fallback.borrow();
}
} // namespace detail

template <class Derived>
class Writable {
 public:
  /**
   * Write a value to the cursor.
   *
   * @methodset Writing
   *
   * May throw if there isn't enough space and the derived cursor type does not
   * support extending the IOBuf's writable range.
   */
  template <class T>
  typename std::enable_if<std::is_arithmetic<T>::value>::type write(
      T value, size_t n = sizeof(T)) {
    this->write(tag<T>, value, n);
  }
  template <class T>
  typename std::enable_if<std::is_arithmetic<T>::value>::type write(
      tag_t<T>, T value, size_t n = sizeof(T)) {
    assert(n <= sizeof(T));
    const uint8_t* u8 = reinterpret_cast<const uint8_t*>(&value);
    Derived* d = static_cast<Derived*>(this);
    d->push(u8, n);
  }

  /**
   * Write a value to the cursor in Big-Endian.
   *
   * @methodset Writing
   *
   * May throw if there isn't enough space and the derived cursor type does not
   * support extending the IOBuf's writable range.
   */
  template <class T>
  void writeBE(T value) {
    this->writeBE<T>(tag<T>, value);
  }
  template <class T>
  void writeBE(tag_t<T>, T value) {
    Derived* d = static_cast<Derived*>(this);
    d->write(tag<T>, Endian::big(value));
  }

  /**
   * Write a value to the cursor in Little-Endian.
   *
   * @methodset Writing
   *
   * May throw if there isn't enough space and the derived cursor type does not
   * support extending the IOBuf's writable range.
   */
  template <class T>
  void writeLE(T value) {
    this->writeLE(tag<T>, value);
  }
  template <class T>
  void writeLE(tag_t<T>, T value) {
    Derived* d = static_cast<Derived*>(this);
    d->write(tag<T>, Endian::little(value));
  }

  /**
   * Write bytes to the cursor.
   *
   * @methodset Writing
   *
   * @throw out_of_range if there isn't enough space in the cursor.
   */
  void push(const uint8_t* buf, size_t len) {
    Derived* d = static_cast<Derived*>(this);
    if (d->pushAtMost(buf, len) != len) {
      throw_exception<std::out_of_range>("overflow");
    }
  }

  void push(ByteRange buf) {
    if (this->pushAtMost(buf) != buf.size()) {
      throw_exception<std::out_of_range>("overflow");
    }
  }

  /**
   * Write bytes to the cursor; stop writing if the end of the cursor is
   * reached.
   *
   * @methodset Writing
   */
  size_t pushAtMost(ByteRange buf) {
    Derived* d = static_cast<Derived*>(this);
    return d->pushAtMost(buf.data(), buf.size());
  }

  void push(Cursor cursor, size_t len) {
    if (this->pushAtMost(cursor, len) != len) {
      throw_exception<std::out_of_range>("overflow");
    }
  }

  size_t pushAtMost(Cursor cursor, size_t len) {
    size_t written = 0;
    for (;;) {
      auto currentBuffer = cursor.peekBytes();
      const uint8_t* crtData = currentBuffer.data();
      size_t available = currentBuffer.size();
      if (available == 0) {
        // end of buffer chain
        return written;
      }
      // all data is in current buffer
      if (available >= len) {
        this->push(crtData, len);
        cursor.skip(len);
        return written + len;
      }

      // write the whole current IOBuf
      this->push(crtData, available);
      cursor.skip(available);
      written += available;
      len -= available;
    }
  }
};

enum class CursorAccess { PRIVATE, UNSHARE };

template <CursorAccess access>
class RWCursor
    : public CursorBase<RWCursor<access>, IOBuf>,
      public Writable<RWCursor<access>> {
  friend class CursorBase<RWCursor<access>, IOBuf>;

 public:
  explicit RWCursor(IOBuf* buf) noexcept
      : CursorBase<RWCursor<access>, IOBuf>(buf), maybeShared_(true) {}

  explicit RWCursor(IOBufQueue& queue) noexcept
      : RWCursor((queue.flushCache(), queue.head_.get())) {}

  // Efficient way to advance to position cursor to the end of the queue,
  // using cached length instead of a walk via advanceToEnd().
  struct AtEnd {};
  /**
   * Create the cursor initially pointing to the end of queue.
   */
  RWCursor(IOBufQueue& queue, AtEnd) noexcept : RWCursor(queue) {
    if (!queue.options().cacheChainLength) {
      this->advanceToEnd();
    } else {
      this->crtBuf_ = this->buffer_->prev();
      this->crtBegin_ = this->crtBuf_->data();
      this->crtEnd_ = this->crtBuf_->tail();
      this->crtPos_ = this->crtEnd_;
      this->absolutePos_ =
          queue.chainLength() - (this->crtPos_ - this->crtBegin_);
      DCHECK_EQ(this->getCurrentPosition(), queue.chainLength());
    }
  }

  template <class OtherDerived, class OtherBuf>
  explicit RWCursor(const CursorBase<OtherDerived, OtherBuf>& cursor) noexcept
      : CursorBase<RWCursor<access>, IOBuf>(cursor), maybeShared_(true) {
    CHECK(!cursor.isBounded())
        << "Creating RWCursor from bounded Cursor is not allowed";
  }
  /**
   * Gather at least n bytes contiguously into the current buffer,
   * by coalescing subsequent buffers from the chain as necessary.
   *
   * @methodset Modifiers
   *
   * @throw overflow_error if there aren't enough bytes to gather
   */
  void gather(size_t n) {
    // Forbid attempts to gather beyond the end of this IOBuf chain.
    // Otherwise we could try to coalesce the head of the chain and end up
    // accidentally freeing it, invalidating the pointer owned by external
    // code.
    //
    // If crtBuf_ == head() then IOBuf::gather() will perform all necessary
    // checking.  We only have to perform an explicit check here when calling
    // gather() on a non-head element.
    if (this->crtBuf_ != this->head() && this->totalLength() < n) {
      throw_exception<std::overflow_error>(
          "cannot gather() past the end of the chain");
    }
    size_t offset = this->crtPos_ - this->crtBegin_;
    this->crtBuf_->gather(offset + n);
    this->crtBegin_ = this->crtBuf_->data();
    this->crtEnd_ = this->crtBuf_->tail();
    this->crtPos_ = this->crtBegin_ + offset;
  }

  /**
   * Gather at most n bytes contiguously into the current buffer,
   * by coalescing subsequent buffers from the chain as necessary.
   *
   * @methodset Modifiers
   */
  void gatherAtMost(size_t n) {
    this->dcheckIntegrity();
    size_t size = std::min(n, this->totalLength());
    size_t offset = this->crtPos_ - this->crtBegin_;
    this->crtBuf_->gather(offset + size);
    this->crtBegin_ = this->crtBuf_->data();
    this->crtEnd_ = this->crtBuf_->tail();
    this->crtPos_ = this->crtBegin_ + offset;
  }

  using Writable<RWCursor<access>>::pushAtMost;
  size_t pushAtMost(const uint8_t* buf, size_t len) {
    // We have to explicitly check for an input length of 0.
    // We support buf being nullptr in this case, but we need to avoid calling
    // memcpy() with a null source pointer, since that is undefined behavior
    // even if the length is 0.
    if (len == 0) {
      return 0;
    }

    size_t copied = 0;
    for (;;) {
      // Fast path: the current buffer is big enough.
      size_t available = this->length();
      if (available >= len) [[likely]] {
        if (access == CursorAccess::UNSHARE) {
          maybeUnshare();
        }
        memcpy(writableData(), buf, len);
        this->crtPos_ += len;
        return copied + len;
      }

      if (access == CursorAccess::UNSHARE) {
        maybeUnshare();
      }
      memcpy(writableData(), buf, available);
      copied += available;
      if (!this->tryAdvanceBuffer()) [[unlikely]] {
        return copied;
      }
      buf += available;
      len -= available;
    }
  }

  /**
   * Insert data at the cursor position.
   *
   * @methodset Writing
   *
   * Data in the IOBuf after the cursor will not be overwritten, though it might
   * be moved.
   *
   * After this operator, the cursor will point to the data just after the
   * inserted data.
   */
  void insert(std::unique_ptr<folly::IOBuf> buf) {
    this->dcheckIntegrity();
    this->absolutePos_ += buf->computeChainDataLength();
    if (this->crtPos_ == this->crtBegin_ && this->crtBuf_ != this->buffer_) {
      // Can just prepend
      this->crtBuf_->prependChain(std::move(buf));
    } else {
      IOBuf* nextBuf;
      std::unique_ptr<folly::IOBuf> remaining;
      if (this->crtPos_ != this->crtEnd_) {
        // Need to split current IOBuf in two.
        remaining = this->crtBuf_->cloneOne();
        remaining->trimStart(this->crtPos_ - this->crtBegin_);
        nextBuf = remaining.get();
        buf->prependChain(std::move(remaining));
      } else {
        // Can just append
        nextBuf = this->crtBuf_->next();
      }
      this->crtBuf_->trimEnd(this->length());
      this->absolutePos_ += this->crtPos_ - this->crtBegin_;
      this->crtBuf_->insertAfterThisOne(std::move(buf));

      if (nextBuf == this->buffer_) {
        // We've just appended to the end of the buffer, so advance to the end.
        this->crtBuf_ = this->buffer_->prev();
        this->crtBegin_ = this->crtBuf_->data();
        this->crtPos_ = this->crtEnd_ = this->crtBuf_->tail();
        // This has already been accounted for, so remove it.
        this->absolutePos_ -= this->crtEnd_ - this->crtBegin_;
      } else {
        // Jump past the new links
        this->crtBuf_ = nextBuf;
        this->crtPos_ = this->crtBegin_ = this->crtBuf_->data();
        this->crtEnd_ = this->crtBuf_->tail();
      }
    }
  }

  /**
   * Get a raw pointer to the writable section controlled by this cursor.
   *
   * @methodset Accessors
   */
  uint8_t* writableData() {
    this->dcheckIntegrity();
    return this->crtBuf_->writableData() + (this->crtPos_ - this->crtBegin_);
  }

 private:
  void maybeUnshare() {
    if (maybeShared_) [[unlikely]] {
      size_t offset = this->crtPos_ - this->crtBegin_;
      this->crtBuf_->unshareOne();
      this->crtBegin_ = this->crtBuf_->data();
      this->crtEnd_ = this->crtBuf_->tail();
      this->crtPos_ = this->crtBegin_ + offset;
      maybeShared_ = false;
    }
  }

  void advanceDone() { maybeShared_ = true; }

  bool maybeShared_;
};

using RWPrivateCursor = RWCursor<CursorAccess::PRIVATE>;
using RWUnshareCursor = RWCursor<CursorAccess::UNSHARE>;

/**
 * Append to the end of a buffer chain, growing the chain (by allocating new
 * buffers) in increments of at least growth bytes every time.  Won't grow
 * (and push() and ensure() will throw) if growth == 0.
 *
 * TODO(tudorb): add a flavor of Appender that reallocates one IOBuf instead
 * of chaining.
 */
class Appender : public Writable<Appender> {
 public:
  Appender(IOBuf* buf, std::size_t growth) noexcept
      : buffer_(buf), crtBuf_(buf->prev()), growth_(growth) {}

  /**
   * Get the writable tail of the IOBuf this cursor points to.
   *
   * @methodset Accessors
   */
  uint8_t* writableData() noexcept { return crtBuf_->writableTail(); }

  /**
   * Get the amount of writable tailroom of the IOBuf this cursor points to.
   *
   * @methodset Capacity
   */
  size_t length() const noexcept { return crtBuf_->tailroom(); }

  /**
   * Mark n bytes (must be <= length()) as appended, as per the
   * IOBuf::append() method.
   *
   * @methodset Appending
   */
  void append(size_t n) { crtBuf_->append(n); }

  /**
   * Ensure at least n contiguous bytes available to write.
   * Postcondition: length() >= n.
   *
   * @methodset Appending
   */
  void ensure(std::size_t n) {
    if (length() >= n) [[likely]] {
      return;
    }

    // Waste the rest of the current buffer and allocate a new one.
    // Don't make it too small, either.
    if (growth_ == 0) {
      throw_exception<std::out_of_range>("can't grow buffer chain");
    }

    n = std::max(n, growth_);
    buffer_->prependChain(IOBuf::create(n));
    crtBuf_ = buffer_->prev();
  }

  using Writable<Appender>::pushAtMost;
  size_t pushAtMost(const uint8_t* buf, size_t len) {
    // We have to explicitly check for an input length of 0.
    // We support buf being nullptr in this case, but we need to avoid calling
    // memcpy() with a null source pointer, since that is undefined behavior
    // even if the length is 0.
    if (len == 0) {
      return 0;
    }

    // If the length of this buffer is 0 try growing it.
    // Otherwise on the first iteration of the following loop memcpy is called
    // with a null source pointer.
    size_t available = length();
    if (available == 0) [[unlikely]] {
      if (!tryGrowChain(len)) {
        return 0;
      }
      available = length();
    }

    size_t copied = 0;
    for (;;) {
      // Fast path: it all fits in one buffer.
      if (available >= len) [[likely]] {
        memcpy(writableData(), buf, len);
        append(len);
        return copied + len;
      }

      memcpy(writableData(), buf, available);
      append(available);
      copied += available;
      buf += available;
      len -= available;
      if (!tryGrowChain(len)) [[unlikely]] {
        return copied;
      }
      available = length();
    }
  }

  /**
   * Append to the end of this buffer, using a printf() style
   * format specifier.
   *
   * @methodset Appending
   *
   * Note that {fmt} (fmt::format) provides nicer and more type-safe
   * mechanisms for formatting strings, which should generally be preferred
   * over printf-style formatting.  Appender objects accept the formatted
   * result directly.  For example:
   *
   *   Appender app(&iobuf);
   *   app(fmt::format("{} {}", "hello", "world"));
   *
   * However, printf-style strings are still needed when dealing with existing
   * third-party code in some cases.
   *
   * This will always add a nul-terminating character after the end
   * of the output.  However, the buffer data length will only be updated to
   * include the data itself.  The nul terminator will be the first byte in the
   * buffer tailroom.
   *
   * This method may throw exceptions on error.
   */
  void printf(FOLLY_PRINTF_FORMAT const char* fmt, ...)
      FOLLY_PRINTF_FORMAT_ATTR(2, 3);

  /// @methodset Appending
  void vprintf(const char* fmt, va_list ap);

  /**
   * Append a StringPiece to the buffer.
   *
   * @methodset Appending
   *
   * This allows Appender objects to be used directly with Formatter.
   */
  void operator()(StringPiece sp) { push(ByteRange(sp)); }

 private:
  bool tryGrowChain(std::size_t minSize) {
    assert(crtBuf_->next() == buffer_);
    if (growth_ == 0) {
      return false;
    }

    buffer_->prependChain(IOBuf::create(std::max(minSize, growth_)));
    crtBuf_ = buffer_->prev();
    return true;
  }

  IOBuf* buffer_;
  IOBuf* crtBuf_;
  std::size_t growth_;
};

class QueueAppender : public Writable<QueueAppender> {
 public:
  /**
   * Create an Appender that writes to a IOBufQueue.  When we allocate space in
   * the queue, each new buffer will be sized between minGrowth and maxGrowth,
   * with an exponential schedule (unless you call ensure() with a bigger value
   * yourself).
   */
  QueueAppender(
      IOBufQueue* queue, std::size_t minGrowth, std::size_t maxGrowth) noexcept
      : queueCache_(queue) {
    resetGrowth(minGrowth, maxGrowth);
  }

  /**
   * Convenience constructor to use constant buffer growth.
   */
  QueueAppender(IOBufQueue* queue, std::size_t growth) noexcept
      : QueueAppender(queue, growth, growth) {}

  /**
   * Resets this, as if constructed anew.
   */
  void reset(
      IOBufQueue* queue,
      std::size_t minGrowth,
      std::size_t maxGrowth) noexcept {
    queueCache_.reset(queue);
    resetGrowth(minGrowth, maxGrowth);
  }

  void reset(IOBufQueue* queue, std::size_t growth) noexcept {
    reset(queue, growth, growth);
  }

  /**
   * Get a pointer to the writable tail.
   *
   * @methodset Accessors
   */
  uint8_t* writableData() noexcept { return queueCache_.writableData(); }

  /**
   * Get the size of the writable tail.
   *
   * @methodset Capacity
   */
  size_t length() const noexcept { return queueCache_.length(); }

  /**
   * Append n bytes.
   *
   * @methodset Appending
   */
  void append(size_t n) { queueCache_.append(n); }

  /**
   * Ensure that there are at least n contiguous bytes available for writing.
   *
   * @methodset Modifiers
   *
   * Can go above maxGrowth.
   *
   * May throw if there isn't enough room.
   */
  void ensure(size_t n) {
    if (length() < n) {
      ensureSlowNoinline(n);
    }
  }

  /**
   * Ensures up to n contiguous bytes available, without surpassing maxGrowth_.
   *
   * @methodset Modifiers
   *
   * Cannot go above maxGrowth.
   *
   * May throw if there isn't enough room.
   */
  void ensureWithinMaxGrowth(size_t n) { ensure(std::min(n, maxGrowth_)); }

  /**
   * Write an object to the cursor.
   *
   * @param n The number of bytes of value to write; defaults to sizeof(T)
   */
  template <class T>
  typename std::enable_if<std::is_arithmetic<T>::value>::type write(
      T value, size_t n = sizeof(T)) {
    this->write(tag<T>, value, n);
  }
  template <class T>
  typename std::enable_if<std::is_arithmetic<T>::value>::type write(
      tag_t<T>, T value, size_t n = sizeof(T)) {
    // We can't fail.
    assert(n <= sizeof(T));
    if (length() < sizeof(T)) [[unlikely]] {
      ensureSlowNoinline(sizeof(T));
    }
    uint8_t* dest = queueCache_.writableData();
    storeUnaligned<T>(dest, value);
    queueCache_.setWritableData(dest + n);
  }

  using Writable<QueueAppender>::pushAtMost;
  size_t pushAtMost(const uint8_t* buf, size_t len) {
    // Fill the current buffer
    const size_t copyLength = std::min(len, length());
    if (copyLength != 0) {
      memcpy(writableData(), buf, copyLength);
      queueCache_.appendUnsafe(copyLength);
      buf += copyLength;
    }
    size_t remaining = len - copyLength;
    // Allocate more buffers as necessary
    while (remaining != 0) {
      ensureSlow(growth_);
      auto avail = std::min(length(), remaining);
      memcpy(writableData(), buf, avail);
      queueCache_.appendUnsafe(avail);
      buf += avail;
      remaining -= avail;
    }
    return len;
  }

  /**
   * Inserts data at the current cursor position.
   *
   * @methodset Writing
   */
  void insert(std::unique_ptr<folly::IOBuf> buf, bool pack = true) {
    if (buf) {
      queueCache_.queue()->append(
          std::move(buf), pack, /* allowTailReuse */ true);
    }
  }

  void insert(const folly::IOBuf& buf, bool pack = true) {
    queueCache_.queue()->append(buf, pack, /* allowTailReuse */ true);
  }

  /**
   * Get a RWCursor for this IOBufQueue.
   *
   * @methodset Accessors
   */
  template <CursorAccess access>
  explicit operator RWCursor<access>() {
    return RWCursor<access>(*queueCache_.queue());
  }

  /**
   * Get a RWCursor for the last n bytes of this IOBufQueue.
   *
   * @methodset Accessors
   */
  template <CursorAccess access>
  RWCursor<access> tail(size_t n) {
    RWCursor<access> result(
        *queueCache_.queue(), typename RWCursor<access>::AtEnd{});
    result -= n;
    return result;
  }

  /**
   * Remove n bytes from the end of this IOBufQueue.
   *
   * @methodset Modifiers
   */
  void trimEnd(size_t n) { queueCache_.queue()->trimEnd(n); }

 private:
  folly::IOBufQueue::WritableRangeCache queueCache_{nullptr};
  size_t growth_{0};
  size_t maxGrowth_{0};

  void resetGrowth(std::size_t minGrowth, std::size_t maxGrowth) noexcept {
    CHECK_LE(growth_, maxGrowth_);
    growth_ = minGrowth;
    maxGrowth_ = maxGrowth;
  }

  FOLLY_NOINLINE void ensureSlowNoinline(size_t n) { ensureSlow(n); }

  void ensureSlow(size_t n) {
    // Reinstall our cache in case something else invalidated it.
    queueCache_.fillCache();
    // Recheck with updated cache, to avoid unnecessary allocation.
    if (length() >= n) [[unlikely]] {
      return;
    }
    auto minBufSize = std::exchange(growth_, std::min(growth_ * 2, maxGrowth_));
    queueCache_.queue()->append(IOBuf::create(std::max(n, minBufSize)));
    DCHECK_GE(length(), n);
  }
};

} // namespace io
} // namespace folly

#include <folly/io/Cursor-inl.h>
