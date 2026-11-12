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

#include <cstring>
#include <stdexcept>

#include <sodium.h>

#include <folly/crypto/detail/LtHashInternal.h>
#include <folly/lang/Bits.h>

namespace folly {
namespace crypto {

namespace detail {

/**
 * Implements bit twiddling operations for elements of size B bits.
 * Currently there are specializations for B = 16, B = 20, and B = 32.
 * All operations are performed on groups of elements packed into uint64_t
 * operands.
 *
 * When B == 16, each uint64_t contains 4 elements without any padding bits.
 * Both SSE2 and AVX2 have native support for adding vectors of 16-bit ints
 * so we can use those directly. When not using SSE2 or AVX2, there is some
 * minor inefficiency because the odd and even elements of each 64-bit block
 * need to be added separately, then XORed together.
 * The packed int looks like:
 *  <16 bits of data> <16 bits of data> <16 bits of data> <16 bits of data>.
 *
 * When B == 20, each uint64_t contains 3 elements with 0 padding bits at
 *   0-based positions 63, 62, 41, and 20. The packed int looks like:
 *   00 <20 bits of data> 0 <20 bits of data> 0 <20 bits of data>.
 *
 * When B == 32, each uint64_t contains 2 elements without any padding bits.
 * Both SSE2 and AVX2 have native support for adding vectors of 32-bit ints
 * so we can use those directly. When not using SSE2 or AVX2, there is some
 * minor inefficiency because the high and low elements of each 64-bit block
 * need to be added separately, then XORed together.
 * The packed int looks like:
 *  <32 bits of data> <32 bits of data>.
 */
template <std::size_t B>
struct Bits {
  static inline constexpr uint64_t kDataMask();
  static inline constexpr bool needsPadding();
};

////// Template specialization for B = 16

// static
template <>
inline constexpr uint64_t Bits<16>::kDataMask() {
  return 0xffffffffffffffffULL;
}

// static
template <>
inline constexpr bool Bits<16>::needsPadding() {
  return false;
}

////// Template specialization for B = 20

// static
template <>
inline constexpr uint64_t Bits<20>::kDataMask() {
  // In binary this mask looks like:
  // 00 <1 repeated 20 times> 0 <1 repeated 20 times> 0 <1 repeated 20 times>
  return ~0xC000020000100000ULL;
}

// static
template <>
inline constexpr bool Bits<20>::needsPadding() {
  return true;
}

////// Template specialization for B = 32

// static
template <>
inline constexpr uint64_t Bits<32>::kDataMask() {
  return 0xffffffffffffffffULL;
}

// static
template <>
inline constexpr bool Bits<32>::needsPadding() {
  return false;
}

/* static */
template <std::size_t B>
constexpr size_t getElementsPerUint64() {
  // how many elements fit into a 64-bit int? If padding is needed, assumes that
  // there is 1 padding bit between elements and any partial space is not used.
  // If padding is not needed, the computation is a trivial division.
  return detail::Bits<B>::needsPadding()
      ? ((sizeof(uint64_t) * 8) / (B + 1))
      : ((sizeof(uint64_t) * 8) / B);
}

// Compile-time computation of the checksum size for a hash with given B and N.
template <std::size_t B, std::size_t N>
constexpr size_t getChecksumSizeBytes() {
  constexpr size_t elemsPerUint64 = getElementsPerUint64<B>();
  static_assert(
      N % elemsPerUint64 == 0,
      "Invalid parameters: N %% elemsPerUint64 must be 0");
  return (N / elemsPerUint64) * sizeof(uint64_t);
}

} // namespace detail

template <std::size_t B, std::size_t N>
LtHash<B, N>::LtHash(const folly::IOBuf& initialChecksum)
    : checksum_{}, key_{folly::none} {
  static_assert(N > 999, "element count must be at least 1000");
  static_assert(
      B == 16 || B == 20 || B == 32,
      "invalid element size in bits, must be one of: [ 16, 20, 32 ]");

  // Make sure libsodium is initialized, but only do it once.
  static const int sodiumInitResult = []() { return sodium_init(); }();

  if (sodiumInitResult == -1) {
    throw std::runtime_error("sodium_init() failed");
  }

  if (initialChecksum.length() == 0) {
    checksum_ = detail::allocateCacheAlignedIOBuf(getChecksumSizeBytes());
    checksum_.append(getChecksumSizeBytes());
    reset();
  } else {
    setChecksum(initialChecksum);
  }
}

template <std::size_t B, std::size_t N>
LtHash<B, N>::LtHash(std::unique_ptr<folly::IOBuf> initialChecksum)
    : checksum_{}, key_{folly::none} {
  // Make sure libsodium is initialized, but only do it once.
  static const int sodiumInitResult = []() { return sodium_init(); }();

  if (sodiumInitResult == -1) {
    throw std::runtime_error("sodium_init() failed");
  }

  setChecksum(std::move(initialChecksum));
}

template <std::size_t B, std::size_t N>
LtHash<B, N>::LtHash(const LtHash<B, N>& that)
    : checksum_{}, key_{folly::none} {
  // Note: we don't need to initialize libsodium in the copy constructor, since
  // before a copy constructor is called, at least one object of this type must
  // be constructed without using a copy constructor, so we know that libsodium
  // must have been initialized already.
  setChecksum(that.checksum_);
  key_ = that.key_;
}

template <std::size_t B, std::size_t N>
LtHash<B, N>& LtHash<B, N>::operator=(const LtHash<B, N>& that) {
  if (checksum_.length() == that.checksum_.length()) {
    std::memcpy(
        checksum_.writableData(), that.checksum_.data(), checksum_.length());
  } else {
    // this probably means that this object was moved away from and
    // checksum_.length() is 0, so we need to allocate a new checksum_ and
    // copy the contents.
    setChecksum(that.checksum_);
  }
  key_ = that.key_;
  return *this;
}

template <std::size_t B, std::size_t N>
LtHash<B, N>::~LtHash() {
  clearKey(); // securely erase the old key if there is one
}

template <std::size_t B, std::size_t N>
void LtHash<B, N>::setKey(folly::ByteRange key) {
  if (key.size() < crypto_generichash_blake2b_KEYBYTES_MIN ||
      key.size() > crypto_generichash_blake2b_KEYBYTES_MAX) {
    throw std::runtime_error("invalid key size");
  }
  clearKey(); // securely erase the old key if there is one
  key_ = std::vector<uint8_t>{key.begin(), key.end()};
}

template <std::size_t B, std::size_t N>
void LtHash<B, N>::clearKey() {
  if (key_.has_value()) {
    sodium_memzero(key_->data(), key_->size());
    key_ = folly::none;
  }
}

template <std::size_t B, std::size_t N>
LtHash<B, N>& LtHash<B, N>::operator+=(const LtHash<B, N>& rhs) {
  if (!keysEqual(*this, rhs)) {
    throw std::runtime_error("Cannot add 2 LtHashes with different keys");
  }
  detail::MathOperation<detail::MathEngine::AUTO>::add(
      detail::Bits<B>::kDataMask(),
      B,
      {checksum_.data(), checksum_.length()},
      {rhs.checksum_.data(), rhs.checksum_.length()},
      {checksum_.writableData(), checksum_.length()});
  return *this;
}

template <std::size_t B, std::size_t N>
LtHash<B, N>& LtHash<B, N>::operator-=(const LtHash<B, N>& rhs) {
  if (!keysEqual(*this, rhs)) {
    throw std::runtime_error("Cannot subtract 2 LtHashes with different keys");
  }
  detail::MathOperation<detail::MathEngine::AUTO>::sub(
      detail::Bits<B>::kDataMask(),
      B,
      {checksum_.data(), checksum_.length()},
      {rhs.checksum_.data(), rhs.checksum_.length()},
      {checksum_.writableData(), checksum_.length()});
  return *this;
}

template <std::size_t B, std::size_t N>
bool LtHash<B, N>::operator==(const LtHash<B, N>& that) const {
  if (this == &that) { // same memory location means it's the same object
    return true;
  } else if (this->checksum_.length() != that.checksum_.length()) {
    return false;
  } else if (this->checksum_.length() == 0) {
    // both objects must have been moved away from
    return true;
  } else {
    int cmp = sodium_memcmp(
        this->checksum_.data(),
        that.checksum_.data(),
        this->checksum_.length());
    return cmp == 0;
  }
}

template <std::size_t B, std::size_t N>
bool LtHash<B, N>::checksumEquals(folly::ByteRange otherChecksum) const {
  if (otherChecksum.size() != getChecksumSizeBytes()) {
    throw std::runtime_error("Invalid checksum size");
  } else if (this->checksum_.length() != otherChecksum.size()) {
    return false;
  } else {
    int cmp = sodium_memcmp(
        this->checksum_.data(), otherChecksum.data(), this->checksum_.length());
    return cmp == 0;
  }
}

template <std::size_t B, std::size_t N>
void LtHash<B, N>::reset() {
  std::memset(checksum_.writableData(), 0, checksum_.length());
}

template <std::size_t B, std::size_t N>
void LtHash<B, N>::setChecksum(const folly::IOBuf& checksum) {
  if (checksum.computeChainDataLength() != getChecksumSizeBytes()) {
    throw std::runtime_error("Invalid checksum size");
  }
  folly::IOBuf checksumCopy =
      detail::allocateCacheAlignedIOBuf(getChecksumSizeBytes());
  for (auto range : checksum) {
    std::memcpy(checksumCopy.writableTail(), range.data(), range.size());
    checksumCopy.append(range.size());
  }
  if constexpr (detail::Bits<B>::needsPadding()) {
    bool isPaddedCorrectly =
        detail::MathOperation<detail::MathEngine::AUTO>::checkPaddingBits(
            detail::Bits<B>::kDataMask(),
            {checksumCopy.data(), checksumCopy.length()});
    if (!isPaddedCorrectly) {
      throw std::runtime_error("Invalid checksum has non-0 padding bits");
    }
  }
  checksum_ = std::move(checksumCopy);
}

template <std::size_t B, std::size_t N>
void LtHash<B, N>::setChecksum(std::unique_ptr<folly::IOBuf> checksum) {
  if (checksum == nullptr) {
    throw std::runtime_error("null checksum");
  }
  // If the checksum is not eligible for move, call the copy version
  if (checksum->isChained() || checksum->isShared() ||
      !detail::isCacheAlignedAddress(checksum->data())) {
    setChecksum(*checksum);
    return;
  }

  if (checksum->computeChainDataLength() != getChecksumSizeBytes()) {
    throw std::runtime_error("Invalid checksum size");
  }

  // If we get here, we know that the input is not null, shared, or chained,
  // is the proper size, and is aligned on a cache line boundary.
  // Just need to check the padding bits before taking ownership of the buffer.
  if constexpr (detail::Bits<B>::needsPadding()) {
    bool isPaddedCorrectly =
        detail::MathOperation<detail::MathEngine::AUTO>::checkPaddingBits(
            detail::Bits<B>::kDataMask(),
            {checksum->data(), checksum->length()});
    if (!isPaddedCorrectly) {
      throw std::runtime_error("Invalid checksum has non-0 padding bits");
    }
  }
  checksum_ = std::move(*checksum);
}

template <std::size_t B, std::size_t N>
template <typename... Args>
void LtHash<B, N>::hashObject(
    folly::MutableByteRange out,
    folly::ByteRange firstRange,
    Args&&... moreRanges) {
  CHECK_EQ(getChecksumSizeBytes(), out.size());
  Blake2xb digest;
  if (key_.has_value()) {
    digest.init(out.size(), folly::range(*key_));
  } else {
    digest.init(out.size());
  }
  updateDigest(digest, firstRange, std::forward<Args>(moreRanges)...);
  digest.finish(out);
  if constexpr (detail::Bits<B>::needsPadding()) {
    detail::MathOperation<detail::MathEngine::AUTO>::clearPaddingBits(
        detail::Bits<B>::kDataMask(), out);
  }
}

template <std::size_t B, std::size_t N>
template <typename... Args>
void LtHash<B, N>::updateDigest(
    Blake2xb& digest, folly::ByteRange firstRange, Args&&... moreRanges) {
  digest.update(firstRange);
  updateDigest(digest, std::forward<Args>(moreRanges)...);
}

template <std::size_t B, std::size_t N>
void LtHash<B, N>::updateDigest(Blake2xb& /* digest */) {}

template <std::size_t B, std::size_t N>
template <typename... Args>
LtHash<B, N>& LtHash<B, N>::addObject(
    folly::ByteRange firstRange, Args&&... moreRanges) {
  // hash obj and add to elements of checksum
  using H = std::array<unsigned char, getChecksumSizeBytes()>;
  alignas(detail::kCacheLineSize) H h;
  hashObject(
      {h.data(), h.size()}, firstRange, std::forward<Args>(moreRanges)...);
  detail::MathOperation<detail::MathEngine::AUTO>::add(
      detail::Bits<B>::kDataMask(),
      B,
      {checksum_.data(), checksum_.length()},
      {h.data(), h.size()},
      {checksum_.writableData(), checksum_.length()});
  return *this;
}

template <std::size_t B, std::size_t N>
template <typename... Args>
LtHash<B, N>& LtHash<B, N>::removeObject(
    folly::ByteRange firstRange, Args&&... moreRanges) {
  // hash obj and subtract from elements of checksum
  using H = std::array<unsigned char, getChecksumSizeBytes()>;
  alignas(detail::kCacheLineSize) H h;
  hashObject(
      {h.data(), h.size()}, firstRange, std::forward<Args>(moreRanges)...);
  detail::MathOperation<detail::MathEngine::AUTO>::sub(
      detail::Bits<B>::kDataMask(),
      B,
      {checksum_.data(), checksum_.length()},
      {h.data(), h.size()},
      {checksum_.writableData(), checksum_.length()});
  return *this;
}

/* static */
template <std::size_t B, std::size_t N>
constexpr size_t LtHash<B, N>::getChecksumSizeBytes() {
  return detail::getChecksumSizeBytes<B, N>();
}

/* static */
template <std::size_t B, std::size_t N>
constexpr size_t LtHash<B, N>::getElementSizeInBits() {
  return B;
}

/* static */
template <std::size_t B, std::size_t N>
constexpr size_t LtHash<B, N>::getElementsPerUint64() {
  return detail::getElementsPerUint64<B>();
}

/* static */
template <std::size_t B, std::size_t N>
constexpr size_t LtHash<B, N>::getElementCount() {
  return N;
}

/* static */
template <std::size_t B, std::size_t N>
constexpr bool LtHash<B, N>::hasPaddingBits() {
  return detail::Bits<B>::needsPadding();
}

template <std::size_t B, std::size_t N>
std::unique_ptr<folly::IOBuf> LtHash<B, N>::getChecksum() const {
  auto result = std::make_unique<folly::IOBuf>(
      detail::allocateCacheAlignedIOBuf(checksum_.length()));
  result->append(checksum_.length());
  std::memcpy(result->writableData(), checksum_.data(), checksum_.length());
  return result;
}

// static
template <std::size_t B, std::size_t N>
bool LtHash<B, N>::keysEqual(const LtHash<B, N>& h1, const LtHash<B, N>& h2) {
  if (h1.key_.has_value() != h2.key_.has_value()) {
    return false;
  }
  if (!h1.key_.has_value()) {
    return true; // both LtHashes have empty keys
  }
  if (h1.key_->size() != h2.key_->size()) {
    return false;
  }
  return sodium_memcmp(h1.key_->data(), h2.key_->data(), h1.key_->size()) == 0;
}

} // namespace crypto
} // namespace folly
