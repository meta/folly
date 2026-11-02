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

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif

#include <folly/io/IOBuf.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include <folly/Conv.h>
#include <folly/Likely.h>
#include <folly/ScopeGuard.h>
#include <folly/hash/SpookyHashV2.h>
#include <folly/io/Cursor.h>
#include <folly/lang/Align.h>
#include <folly/lang/CheckedMath.h>
#include <folly/lang/Exception.h>
#include <folly/lang/Hint.h>
#include <folly/memory/Malloc.h>
#include <folly/memory/SanitizeAddress.h>

/*
 * Callbacks that will be invoked when IOBuf allocates or frees memory.
 * Note that io_buf_alloc_cb() will also be invoked when IOBuf takes ownership
 * of a malloc-allocated buffer, even if it was allocated earlier by another
 * part of the code.
 *
 * By default these are unimplemented, but programs can define these functions
 * to perform their own custom logic on memory allocation.  This is intended
 * primarily to help programs track memory usage and possibly take action
 * when thresholds are hit.  Callers should generally avoid performing any
 * expensive work in these callbacks, since they may be called from arbitrary
 * locations in the code that use IOBuf, possibly while holding locks.
 */

#if FOLLY_HAVE_WEAK_SYMBOLS
FOLLY_ATTR_WEAK void io_buf_alloc_cb(void* /*ptr*/, size_t /*size*/) noexcept;
FOLLY_ATTR_WEAK void io_buf_free_cb(void* /*ptr*/, size_t /*size*/) noexcept;
#else
static void (*io_buf_alloc_cb)(void* /*ptr*/, size_t /*size*/) noexcept =
    nullptr;
static void (*io_buf_free_cb)(void* /*ptr*/, size_t /*size*/) noexcept =
    nullptr;
#endif

using std::unique_ptr;

namespace {

// When create() is called for buffers less than kDefaultCombinedBufSize,
// we allocate a single combined memory segment for the IOBuf and the data
// together.  See the comments for createCombined()/createSeparate() for more
// details.
//
// (The size of 1k is largely just a guess here.  We could could probably do
// benchmarks of real applications to see if adjusting this number makes a
// difference.  Callers that know their exact use case can also explicitly
// call createCombined() or createSeparate().)
constexpr size_t kDefaultCombinedBufSize = 1024;
constexpr size_t kMaxIOBufSize = std::numeric_limits<size_t>::max() >> 1;

// Helper function for IOBuf::takeOwnership()
// The user's free function is not allowed to throw.
// (We are already in the middle of throwing an exception, so
// we cannot let this exception go unhandled.)
void takeOwnershipError(
    bool freeOnError,
    void* buf,
    folly::IOBuf::FreeFunction freeFn,
    void* userData) noexcept {
  if (!freeOnError) {
    return;
  }
  if (!freeFn) {
    free(buf);
    return;
  }
  freeFn(buf, userData);
}

} // namespace

namespace folly {

// use free for size >= 4GB
// since we can store only 32 bits in the size var
struct IOBuf::HeapPrefix {
  HeapPrefix(uint8_t rc, uint32_t sz, bool hmr)
      : refcount(rc), hasMemoryResource(hmr), size(sz) {}
  ~HeapPrefix() {
    // Reset magic to 0 on destruction.  This is solely for debugging purposes
    // to help catch bugs where someone tries to use HeapStorage after it has
    // been deleted.
    magic = 0;
  }

  constexpr static uint16_t kHeapMagic = 0xa5a5;

  uint16_t magic = kHeapMagic;
  std::atomic<uint8_t> refcount; // 1 for IOBuf and 1 for SharedInfo (+ data).
  bool hasMemoryResource;
  uint32_t size;
};

struct IOBuf::HeapStorage {
  HeapPrefix prefix;
  // The IOBuf is last in the HeapStorage object.
  // This way operator new will work even if allocating a subclass of IOBuf
  // that requires more space.
  folly::IOBuf buf;

  // Only IOBuf is in use.
  constexpr static uint8_t kInitialRefcount = 1;

 private:
  // This function exists only to have a scope to do the static_asserts().
  static void checkInvariants() {
    // Make sure jemalloc allocates from the 64-byte class.
    static_assert(sizeof(HeapPrefix) == 8);
    static_assert(sizeof(IOBuf) <= 56);
    static_assert(sizeof(HeapStorage) <= 64);
  }
};

struct alignas(folly::max_align_v) IOBuf::HeapFullStorage {
  HeapStorage hs;
  SharedInfo shared;

  // Both IOBuf and SharedInfo (possibly with attached data) are in use.
  constexpr static uint8_t kInitialRefcount = 2;

 private:
  static void checkInvariants() {
    static_assert(offsetof(HeapFullStorage, hs) == 0);
  }
};

IOBuf::SharedInfo::SharedInfo(FreeFunction fn, void* arg, StorageType st)
    : freeFn(fn), userData(arg), storageType(st) {}

void IOBuf::SharedInfo::invokeAndDeleteEachObserver(
    SharedInfoObserverEntryBase* observerListHead, ObserverCb cb) noexcept {
  if (observerListHead && cb) {
    // break the chain
    observerListHead->prev->next = nullptr;
    auto entry = observerListHead;
    while (entry) {
      auto tmp = entry->next;
      cb(*entry);
      delete entry;
      entry = tmp;
    }
  }
}

// storageType is stored in info, but info may be already deleted in the
// kExtBuffer case, so we need to pass it separately.
void IOBuf::SharedInfo::releaseStorage(
    IOBuf* parent, StorageType storageType, SharedInfo* info) noexcept {
  if (storageType != StorageType::kExtBuffer) {
    DCHECK_EQ((int)storageType, (int)info->storageType);
  }

  switch (storageType) {
    case StorageType::kInvalid:
      compiler_may_unsafely_assume_unreachable();
    case StorageType::kAllocated:
      delete info;
      break;
    case StorageType::kHeapFullStorage: {
      auto storageAddr =
          reinterpret_cast<uint8_t*>(info) - offsetof(HeapFullStorage, shared);
      auto storage = reinterpret_cast<HeapFullStorage*>(storageAddr);
      info->~SharedInfo();
      if (&storage->hs.buf == parent) {
        // We were called through the same IOBuf that owns the storage, so there
        // cannot be a concurrent refcount decrement, and we can avoid an
        // expensive atomic RMW operation.
        DCHECK_EQ(
            storage->hs.prefix.refcount.load(std::memory_order_relaxed),
            HeapFullStorage::kInitialRefcount);
        storage->hs.prefix.refcount.store(1, std::memory_order_relaxed);
      } else {
        IOBuf::decrementStorageRefcount(&storage->hs);
      }
      break;
    }
    case StorageType::kExtBuffer:
      break; // Storage was already freed.
  }
}

void* IOBuf::operator new(size_t size) {
  DCHECK_GE(size, sizeof(IOBuf));
  auto [storage, mallocSize] =
      allocateStorage<HeapStorage>(nullptr, size - sizeof(IOBuf));
  return &storage->buf;
}

void* IOBuf::operator new(size_t /* size */, void* ptr) {
  return ptr;
}

void IOBuf::operator delete(void* ptr) {
  auto storageAddr = static_cast<uint8_t*>(ptr) - offsetof(HeapStorage, buf);
  auto storage = reinterpret_cast<HeapStorage*>(storageAddr);
  decrementStorageRefcount(storage);
}

void IOBuf::operator delete(void* /* ptr */, void* /* placement */) {
  // Provide matching operator for `IOBuf::new` to avoid MSVC compilation
  // warning (C4291) about memory leak when exception is thrown in the
  // constructor.
}

template <class StorageType>
/* static */ std::pair<StorageType*, size_t> IOBuf::allocateStorage(
    std::pmr::memory_resource* mr, size_t additionalBuffer) {
  size_t mallocSize;
  StorageType* storage;
  uint32_t storedSize;
  bool hasMemoryResource;

  if (mr == nullptr) {
    mallocSize = sizeof(StorageType);
    if (additionalBuffer > 0) {
      mallocSize = goodMallocSize(mallocSize + additionalBuffer);
    }
    storedSize = static_cast<uint32_t>(mallocSize);
    if (storedSize != mallocSize) {
      // If we cannot store the size in 32 bits, fall back to non-sized free.
      storedSize = 0;
    }
    storage = static_cast<StorageType*>(checkedMalloc(mallocSize));
    hasMemoryResource = false;
  } else {
#if FOLLY_HAS_MEMORY_RESOURCE
    // We only need to store the memory_resource pointer when non-null, so we
    // conditionally add a prefix of size max_align_v to the storage to fit the
    // pointer.
    constexpr size_t kPtrSize = sizeof(std::pmr::memory_resource*);
    static_assert(alignof(StorageType) <= max_align_v);
    static_assert(kPtrSize <= max_align_v);
    // We don't know the memory_resource implementation, so we cannot assume
    // that goodMallocSize() is good.
    mallocSize = max_align_v + sizeof(StorageType) + additionalBuffer;
    storedSize = static_cast<uint32_t>(mallocSize);
    if (storedSize != mallocSize) {
      // We only have 32 bits to store the size, and with the PMR interface we
      // cannot fall back to non-sized free.
      throw_exception<std::bad_alloc>();
    }
    auto ptr = static_cast<uint8_t*>(mr->allocate(mallocSize));
    memcpy(ptr + max_align_v - kPtrSize, &mr, kPtrSize);
    storage = reinterpret_cast<StorageType*>(ptr + max_align_v);
    hasMemoryResource = true;
#else
    compiler_may_unsafely_assume_unreachable();
#endif /* FOLLY_HAS_MEMORY_RESOURCE */
  }

  new (storage)
      HeapPrefix(StorageType::kInitialRefcount, storedSize, hasMemoryResource);

  if (io_buf_alloc_cb) {
    io_buf_alloc_cb(storage, mallocSize);
  }

  return {storage, mallocSize};
}

/* static */ std::pmr::memory_resource* IOBuf::getMemoryResource(
    const HeapStorage* storage) {
  if (!storage->prefix.hasMemoryResource) {
    return nullptr;
  }
  std::pmr::memory_resource* mr;
  constexpr size_t kPtrSize = sizeof(std::pmr::memory_resource*);
  memcpy(&mr, reinterpret_cast<const uint8_t*>(storage) - kPtrSize, kPtrSize);
  return mr;
}

/* static */ void IOBuf::freeStorage(HeapStorage* storage) {
  size_t size = storage->prefix.size;
  auto mr = getMemoryResource(storage);

  storage->prefix.HeapPrefix::~HeapPrefix();

  if (mr != nullptr) {
#if FOLLY_HAS_MEMORY_RESOURCE
    auto p = reinterpret_cast<uint8_t*>(storage) - max_align_v;
    mr->deallocate(p, size);
#else
    compiler_may_unsafely_assume_unreachable();
#endif /* FOLLY_HAS_MEMORY_RESOURCE */
  } else if (FOLLY_LIKELY(size)) {
    if (io_buf_free_cb) {
      io_buf_free_cb(storage, size);
    }
    sizedFree(storage, size);
  } else {
    free(storage);
  }
}

void IOBuf::decrementStorageRefcount(HeapStorage* storage) noexcept {
  CHECK_EQ(storage->prefix.magic, HeapPrefix::kHeapMagic);

  auto rc = storage->prefix.refcount.load(std::memory_order_acquire);
  DCHECK_LE(rc, HeapFullStorage::kInitialRefcount);
  if (rc > 1 &&
      storage->prefix.refcount.fetch_sub(1, std::memory_order_acq_rel) > 1) {
    return; // Storage still in use.
  }

  // The storage space is now unused, we can free it.
  freeStorage(storage);
}

IOBuf::IOBuf(CreateOp, std::size_t capacity)
    : length_(0), data_(nullptr), next_(this), prev_(this) {
  SharedInfo* info;
  allocExtBuffer(capacity, &buf_, &info, &capacity_);
  sharedInfo_ = info;
  data_ = buf_;
}

IOBuf::IOBuf(
    CopyBufferOp /* op */,
    const void* buf,
    std::size_t size,
    std::size_t headroom,
    std::size_t minTailroom)
    : length_(0), data_(nullptr), next_(this), prev_(this) {
  std::size_t capacity = 0;
  if (!checked_add(&capacity, size, headroom, minTailroom) ||
      capacity > kMaxIOBufSize) {
    throw_exception<std::bad_alloc>();
  }

  SharedInfo* info;
  allocExtBuffer(capacity, &buf_, &info, &capacity_);
  sharedInfo_ = info;
  data_ = buf_;

  advance(headroom);
  if (size > 0) {
    assert(buf != nullptr);
    memcpy(writableData(), buf, size);
    append(size);
  }
}

IOBuf::IOBuf(
    CopyBufferOp op,
    ByteRange br,
    std::size_t headroom,
    std::size_t minTailroom)
    : IOBuf(op, br.data(), br.size(), headroom, minTailroom) {}

unique_ptr<IOBuf> IOBuf::create(std::size_t capacity) {
  if (capacity > kMaxIOBufSize) {
    throw_exception<std::bad_alloc>();
  }

  // For smaller-sized buffers, allocate the IOBuf, SharedInfo, and the buffer
  // all with a single allocation.
  //
  // We don't do this for larger buffers since it can be wasteful if the user
  // needs to reallocate the buffer but keeps using the same IOBuf object.
  // In this case we can't free the data space until the IOBuf is also
  // destroyed.  Callers can explicitly call createCombined() or
  // createSeparate() if they know their use case better, and know if they are
  // likely to reallocate the buffer later.
  if (capacity <= kDefaultCombinedBufSize) {
    return createCombined(capacity);
  }

  // if we have nallocx, we want to allocate the capacity and the overhead in
  // a single allocation only if we do not cross into the next allocation class
  // for some buffer sizes, this can use about 25% extra memory
  if (canNallocx()) {
    auto mallocSize = goodMallocSize(capacity);
    // round capacity to a multiple of 8
    size_t minSize = ((capacity + 7) & ~7) + sizeof(SharedInfo);
    // if we do not have space for the overhead, allocate the mem separately
    if (mallocSize < minSize) {
      auto* buf = checkedMalloc(mallocSize);
      return takeOwnership(SIZED_FREE, buf, mallocSize, 0, 0);
    }
  }

  return createSeparate(capacity);
}

unique_ptr<IOBuf> IOBuf::createCombined(std::size_t capacity) {
  if (capacity > kMaxIOBufSize) {
    throw_exception<std::bad_alloc>();
  }

  // To save a memory allocation, allocate space for the IOBuf object, the
  // SharedInfo struct, and the data itself all with a single call to malloc().
  auto [storage, mallocSize] =
      allocateStorage<HeapFullStorage>(nullptr, capacity);
  // No free function, data lifetime is tied to SharedInfo, whole storage will
  // be deallocated when both IOBuf and SharedInfo are gone.
  new (&storage->shared) SharedInfo(
      [](void*, void*) {}, nullptr, SharedInfo::StorageType::kHeapFullStorage);

  auto bufAddr = reinterpret_cast<uint8_t*>(storage) + sizeof(HeapFullStorage);
  uint8_t* storageEnd = reinterpret_cast<uint8_t*>(storage) + mallocSize;
  auto actualCapacity = size_t(storageEnd - bufAddr);
  unique_ptr<IOBuf> ret(new (&storage->hs.buf) IOBuf(
      InternalConstructor(),
      &storage->shared,
      bufAddr,
      actualCapacity,
      bufAddr,
      0));
  return ret;
}

unique_ptr<IOBuf> IOBuf::createSeparate(std::size_t capacity) {
  return std::make_unique<IOBuf>(CREATE, capacity);
}

unique_ptr<IOBuf> IOBuf::createChain(
    size_t totalCapacity, std::size_t maxBufCapacity) {
  unique_ptr<IOBuf> out =
      create(std::min(totalCapacity, size_t(maxBufCapacity)));
  size_t allocatedCapacity = out->capacity();

  while (allocatedCapacity < totalCapacity) {
    unique_ptr<IOBuf> newBuf = create(
        std::min(totalCapacity - allocatedCapacity, size_t(maxBufCapacity)));
    allocatedCapacity += newBuf->capacity();
    out->appendToChain(std::move(newBuf));
  }

  return out;
}

size_t IOBuf::goodSize(size_t minCapacity, CombinedOption combined) {
  if (combined == CombinedOption::DEFAULT) {
    combined = minCapacity <= kDefaultCombinedBufSize
        ? CombinedOption::COMBINED
        : CombinedOption::SEPARATE;
  }
  size_t overhead;
  if (combined == CombinedOption::COMBINED) {
    overhead = sizeof(HeapFullStorage);
  } else {
    // Pad minCapacity to a multiple of 8
    minCapacity = (minCapacity + 7) & ~7;
    overhead = sizeof(SharedInfo);
  }
  size_t goodSize = folly::goodMallocSize(minCapacity + overhead);
  return goodSize - overhead;
}

IOBuf::IOBuf(
    TakeOwnershipOp,
    void* buf,
    std::size_t capacity,
    std::size_t offset,
    std::size_t length,
    FreeFunction freeFn,
    void* userData,
    bool freeOnError)
    : length_(length),
      data_(static_cast<uint8_t*>(buf) + offset),
      capacity_(capacity),
      buf_(static_cast<uint8_t*>(buf)),
      next_(this),
      prev_(this) {
  // do not allow only user data without a freeFn
  // since we use that for folly::sizedFree
  DCHECK(!userData || (userData && freeFn));

  auto rollback = makeGuard([&] { //
    takeOwnershipError(freeOnError, buf, freeFn, userData);
  });
  sharedInfo_ =
      new SharedInfo(freeFn, userData, SharedInfo::StorageType::kAllocated);
  rollback.dismiss();
}

IOBuf::IOBuf(
    TakeOwnershipOp,
    SizedFree,
    void* buf,
    std::size_t capacity,
    std::size_t offset,
    std::size_t length,
    bool freeOnError)
    : length_(length),
      data_(static_cast<uint8_t*>(buf) + offset),
      capacity_(capacity),
      buf_(static_cast<uint8_t*>(buf)),
      next_(this),
      prev_(this) {
  auto rollback = makeGuard([&] { //
    takeOwnershipError(freeOnError, buf, nullptr, nullptr);
  });
  sharedInfo_ = new SharedInfo(
      nullptr,
      reinterpret_cast<void*>(capacity),
      SharedInfo::StorageType::kAllocated);
  rollback.dismiss();

  if (io_buf_alloc_cb && capacity) {
    io_buf_alloc_cb(buf, capacity);
  }
}

unique_ptr<IOBuf> IOBuf::takeOwnershipImpl(
    void* buf,
    std::size_t capacity,
    std::size_t offset,
    std::size_t length,
    FreeFunction freeFn,
    void* userData,
    bool freeOnError,
    TakeOwnershipOption option,
    std::pmr::memory_resource* mr) {
  // do not allow only user data without a freeFn
  // since we use that for folly::sizedFree
  DCHECK(
      !userData || (userData && freeFn) ||
      (userData && !freeFn && (option == TakeOwnershipOption::STORE_SIZE)));

  HeapFullStorage* storage = nullptr;
  auto rollback = makeGuard([&] {
    if (storage) {
      freeStorage(reinterpret_cast<HeapStorage*>(storage));
    }
    takeOwnershipError(freeOnError, buf, freeFn, userData);
  });

  if (capacity > kMaxIOBufSize) {
    throw_exception<std::bad_alloc>();
  }

  size_t mallocSize;
  std::tie(storage, mallocSize) = allocateStorage<HeapFullStorage>(mr);
  new (&storage->shared)
      SharedInfo(freeFn, userData, SharedInfo::StorageType::kHeapFullStorage);

  auto result = unique_ptr<IOBuf>(new (&storage->hs.buf) IOBuf(
      InternalConstructor(),
      &storage->shared,
      static_cast<uint8_t*>(buf),
      capacity,
      static_cast<uint8_t*>(buf) + offset,
      length));

  rollback.dismiss();

  if (io_buf_alloc_cb && userData && !freeFn &&
      (option == TakeOwnershipOption::STORE_SIZE)) {
    // Even though we did not allocate the buffer, call io_buf_alloc_cb() since
    // we will call io_buf_free_cb() on destruction, and we want these calls to
    // be 1:1.
    io_buf_alloc_cb(buf, capacity);
  }

  return result;
}

IOBuf::IOBuf(WrapBufferOp, const void* buf, std::size_t capacity) noexcept
    : IOBuf(
          InternalConstructor(),
          nullptr,
          // We cast away the const-ness of the buffer here.
          // This is okay since IOBuf users must use unshare() to create a copy
          // of this buffer before writing to the buffer.
          static_cast<uint8_t*>(const_cast<void*>(buf)),
          capacity,
          static_cast<uint8_t*>(const_cast<void*>(buf)),
          capacity) {}

IOBuf::IOBuf(WrapBufferOp op, ByteRange br) noexcept
    : IOBuf(op, br.data(), br.size()) {}

unique_ptr<IOBuf> IOBuf::wrapBuffer(const void* buf, std::size_t capacity) {
  return std::make_unique<IOBuf>(WRAP_BUFFER, buf, capacity);
}

IOBuf IOBuf::wrapBufferAsValue(const void* buf, std::size_t capacity) noexcept {
  return IOBuf(WrapBufferOp::WRAP_BUFFER, buf, capacity);
}

IOBuf::IOBuf() noexcept = default;

IOBuf::IOBuf(IOBuf&& other) noexcept
    : length_(other.length_),
      data_(other.data_),
      capacity_(other.capacity_),
      buf_(other.buf_),
      sharedInfo_(other.sharedInfo_) {
  // Reset other so it is a clean state to be destroyed.
  other.data_ = nullptr;
  other.buf_ = nullptr;
  other.length_ = 0;
  other.capacity_ = 0;
  other.sharedInfo_ = nullptr;

  // If other was part of the chain, assume ownership of the rest of its chain.
  // (It's only valid to perform move assignment on the head of a chain.)
  if (other.next_ != &other) {
    next_ = other.next_;
    next_->prev_ = this;
    other.next_ = &other;

    prev_ = other.prev_;
    prev_->next_ = this;
    other.prev_ = &other;
  }

  // Sanity check to make sure that other is in a valid state to be destroyed.
  DCHECK_EQ(other.prev_, &other);
  DCHECK_EQ(other.next_, &other);
}

IOBuf::IOBuf(const IOBuf& other) {
  *this = other.cloneAsValue();
}

IOBuf::IOBuf(
    InternalConstructor,
    SharedInfo* sharedInfo,
    uint8_t* buf,
    std::size_t capacity,
    uint8_t* data,
    std::size_t length) noexcept
    : length_(length),
      data_(data),
      capacity_(capacity),
      buf_(buf),
      next_(this),
      prev_(this),
      sharedInfo_(sharedInfo) {
  assert(data >= buf);
  assert(intptr_t(data) + length <= intptr_t(buf) + capacity);

  if (folly::asan_region_is_poisoned(buf, capacity)) {
    // If we know it's a poisoned region, access it to trigger ASAN reporting.
    memset(buf, 0, capacity);
  }
}

IOBuf::~IOBuf() {
  unlinkAll();
  decrementRefcount();
}

IOBuf& IOBuf::operator=(IOBuf&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  unlinkAll();
  decrementRefcount();
  setBuffer(
      other.sharedInfo_,
      other.buf_,
      other.capacity_,
      other.data_,
      other.length_);
  // Reset other so it is a clean state to be destroyed.
  other.data_ = nullptr;
  other.buf_ = nullptr;
  other.length_ = 0;
  other.capacity_ = 0;
  other.sharedInfo_ = nullptr;

  // If other was part of the chain, assume ownership of the rest of its chain.
  // (It's only valid to perform move assignment on the head of a chain.)
  if (other.next_ != &other) {
    next_ = other.next_;
    next_->prev_ = this;
    other.next_ = &other;

    prev_ = other.prev_;
    prev_->next_ = this;
    other.prev_ = &other;
  }

  // Sanity check to make sure that other is in a valid state to be destroyed.
  DCHECK_EQ(other.prev_, &other);
  DCHECK_EQ(other.next_, &other);

  return *this;
}

IOBuf& IOBuf::operator=(const IOBuf& other) {
  if (this != &other) {
    *this = IOBuf(other);
  }
  return *this;
}

bool IOBuf::empty() const noexcept {
  const IOBuf* current = this;
  do {
    if (current->length() != 0) {
      return false;
    }
    current = current->next_;
  } while (current != this);
  return true;
}

size_t IOBuf::countChainElements() const noexcept {
  size_t numElements = 1;
  for (IOBuf* current = next_; current != this; current = current->next_) {
    ++numElements;
  }
  return numElements;
}

std::size_t IOBuf::computeChainDataLength() const noexcept {
  std::size_t fullLength = length_;
  for (IOBuf* current = next_; current != this; current = current->next_) {
    fullLength += current->length_;
  }
  return fullLength;
}

std::size_t IOBuf::computeChainCapacity() const noexcept {
  std::size_t fullCapacity = capacity_;
  for (IOBuf* current = next_; current != this; current = current->next_) {
    fullCapacity += current->capacity_;
  }
  return fullCapacity;
}

void IOBuf::appendToChain(unique_ptr<IOBuf>&& iobuf) {
  // Take ownership of the specified IOBuf
  IOBuf* other = iobuf.release();

  // Remember the pointer to the tail of the other chain
  IOBuf* otherTail = other->prev_;

  // Hook up prev_->next_ to point at the start of the other chain,
  // and other->prev_ to point at prev_
  prev_->next_ = other;
  other->prev_ = prev_;

  // Hook up otherTail->next_ to point at us,
  // and prev_ to point back at otherTail,
  otherTail->next_ = this;
  prev_ = otherTail;
}

unique_ptr<IOBuf> IOBuf::cloneImpl(std::pmr::memory_resource* mr) const {
  auto tmp = cloneOneImpl(mr);

  for (IOBuf* current = next_; current != this; current = current->next_) {
    tmp->appendToChain(current->cloneOneImpl(mr));
  }

  return tmp;
}

unique_ptr<IOBuf> IOBuf::cloneOneImpl(std::pmr::memory_resource* mr) const {
  incrementRefcount();

  auto [storage, mallocSize] = allocateStorage<HeapStorage>(mr);
  return unique_ptr<IOBuf>{new (&storage->buf) IOBuf(
      InternalConstructor(), sharedInfo_, buf_, capacity_, data_, length_)};
}

unique_ptr<IOBuf> IOBuf::cloneCoalesced() const {
  return std::make_unique<IOBuf>(cloneCoalescedAsValue());
}

unique_ptr<IOBuf> IOBuf::cloneCoalescedWithHeadroomTailroom(
    std::size_t newHeadroom, std::size_t newTailroom) const {
  return std::make_unique<IOBuf>(
      cloneCoalescedAsValueWithHeadroomTailroom(newHeadroom, newTailroom));
}

IOBuf IOBuf::cloneAsValue() const {
  auto tmp = cloneOneAsValue();

  for (IOBuf* current = next_; current != this; current = current->next_) {
    tmp.appendToChain(current->cloneOne());
  }

  return tmp;
}

IOBuf IOBuf::cloneOneAsValue() const {
  incrementRefcount();
  return IOBuf(
      InternalConstructor(), sharedInfo_, buf_, capacity_, data_, length_);
}

IOBuf IOBuf::cloneCoalescedAsValue() const {
  const std::size_t newHeadroom = headroom();
  const std::size_t newTailroom = prev()->tailroom();
  return cloneCoalescedAsValueWithHeadroomTailroom(newHeadroom, newTailroom);
}

IOBuf IOBuf::cloneCoalescedAsValueWithHeadroomTailroom(
    std::size_t newHeadroom, std::size_t newTailroom) const {
  if (isChained() || newHeadroom != headroom()) {
    // Fall through to slow code path.
  } else if (newTailroom == tailroom()) {
    return cloneOneAsValue();
  } else if (newTailroom < tailroom()) {
    const std::size_t newCapacity =
        goodExtBufferSize(length_ + newHeadroom + newTailroom) -
        sizeof(SharedInfo);
    if (tailroom() <= newCapacity - newHeadroom - length_) {
      return cloneOneAsValue();
    }
  }

  // Coalesce into newBuf
  const std::size_t newLength = computeChainDataLength();
  const std::size_t newCapacity = newLength + newHeadroom + newTailroom;
  IOBuf newBuf{CREATE, newCapacity};
  newBuf.advance(newHeadroom);

  auto current = this;
  do {
    if (current->length() > 0) {
      DCHECK_NOTNULL(current->data());
      DCHECK_LE(current->length(), newBuf.tailroom());
      memcpy(newBuf.writableTail(), current->data(), current->length());
      newBuf.append(current->length());
    }
    current = current->next();
  } while (current != this);

  DCHECK_EQ(newLength, newBuf.length());
  DCHECK_EQ(newHeadroom, newBuf.headroom());
  DCHECK_LE(newTailroom, newBuf.tailroom());

  return newBuf;
}

std::unique_ptr<IOBuf> IOBuf::maybeSplitTail() {
  if (isSharedOne()) {
    return nullptr;
  }

  const size_t tailSize = tailroom();

  // If this is already a split tail, clone the (clone of the) original buffer
  // instead of this wrapper, to avoid deep recursion in the free function.
  static constexpr auto freeFn = [](void*, void* p) {
    reinterpret_cast<IOBuf*>(p)->~IOBuf();
  };
  auto origBuf =
      getFreeFn() == freeFn ? reinterpret_cast<IOBuf*>(getUserData()) : this;
  DCHECK_EQ(
      reinterpret_cast<const void*>(origBuf->bufferEnd()),
      reinterpret_cast<const void*>(bufferEnd()));

  struct SplitTailStorage : HeapFullStorage {
    IOBuf parent;
  };

  auto [storage, mallocSize] = allocateStorage<SplitTailStorage>();
  void* userData = new (&storage->parent) IOBuf(origBuf->cloneOneAsValue());
  new (&storage->shared)
      SharedInfo(freeFn, userData, SharedInfo::StorageType::kHeapFullStorage);

  // Release ownership of the tail.
  trimWritableTail(tailSize);

  auto result = std::unique_ptr<IOBuf>(new (&storage->hs.buf) IOBuf(
      InternalConstructor(),
      &storage->shared,
      writableTail(),
      tailSize,
      writableTail(),
      0));

  return result;
}

void IOBuf::unshareOneSlow() {
  // Allocate a new buffer for the data
  uint8_t* buf;
  SharedInfo* sharedInfo;
  std::size_t actualCapacity;
  allocExtBuffer(capacity_, &buf, &sharedInfo, &actualCapacity);

  // Copy the data
  // Maintain the same amount of headroom.  Since we maintained the same
  // minimum capacity we also maintain at least the same amount of tailroom.
  std::size_t headlen = headroom();
  if (length_ > 0) {
    assert(data_ != nullptr);
    memcpy(buf + headlen, data_, length_);
  }

  // Release our reference on the old buffer
  decrementRefcount();
  sharedInfo_ = sharedInfo;

  // Update the buffer pointers to point to the new buffer
  data_ = buf + headlen;
  buf_ = buf;
}

void IOBuf::unshareChained() {
  // unshareChained() should only be called if we are part of a chain of
  // multiple IOBufs.  The caller should have already verified this.
  assert(isChained());

  IOBuf* current = this;
  while (true) {
    if (current->isSharedOne()) {
      // we have to unshare
      break;
    }

    current = current->next_;
    if (current == this) {
      // None of the IOBufs in the chain are shared,
      // so return without doing anything
      return;
    }
  }

  // We have to unshare.  Let coalesceSlow() do the work.
  coalesceSlow();
}

void IOBuf::markExternallyShared() {
  IOBuf* current = this;
  do {
    current->markExternallySharedOne();
    current = current->next_;
  } while (current != this);
}

void IOBuf::makeManagedChained() {
  assert(isChained());

  IOBuf* current = this;
  while (true) {
    current->makeManagedOne();
    current = current->next_;
    if (current == this) {
      break;
    }
  }
}

void IOBuf::coalesceSlow() {
  // coalesceSlow() should only be called if we are part of a chain of multiple
  // IOBufs.  The caller should have already verified this.
  DCHECK(isChained());

  // Compute the length of the entire chain
  std::size_t newLength = 0;
  IOBuf* end = this;
  do {
    newLength += end->length_;
    end = end->next_;
  } while (end != this);

  coalesceAndReallocate(newLength, end);
  // We should be only element left in the chain now
  DCHECK(!isChained());
}

void IOBuf::coalesceSlow(size_t maxLength) {
  // coalesceSlow() should only be called if we are part of a chain of multiple
  // IOBufs.  The caller should have already verified this.
  DCHECK(isChained());
  DCHECK_LT(length_, maxLength);

  // Compute the length of the entire chain
  std::size_t newLength = 0;
  IOBuf* end = this;
  while (true) {
    newLength += end->length_;
    end = end->next_;
    if (newLength >= maxLength) {
      break;
    }
    if (end == this) {
      throw_exception<std::overflow_error>(
          "attempted to coalesce more data than "
          "available");
    }
  }

  coalesceAndReallocate(newLength, end);
  // We should have the requested length now
  DCHECK_GE(length_, maxLength);
}

void IOBuf::coalesceAndReallocate(
    size_t newHeadroom, size_t newLength, IOBuf* end, size_t newTailroom) {
  std::size_t newCapacity = 0;
  if (!checked_add(&newCapacity, newLength, newHeadroom, newTailroom) ||
      newCapacity > kMaxIOBufSize) {
    throw_exception<std::bad_alloc>();
  }

  // Allocate space for the coalesced buffer.
  // We always convert to an external buffer, even if we happened to be an
  // internal buffer before.
  uint8_t* newBuf;
  SharedInfo* newInfo;
  std::size_t actualCapacity;
  allocExtBuffer(newCapacity, &newBuf, &newInfo, &actualCapacity);

  // Copy the data into the new buffer
  uint8_t* newData = newBuf + newHeadroom;
  uint8_t* p = newData;
  IOBuf* current = this;
  size_t remaining = newLength;
  do {
    if (current->length_ > 0) {
      assert(current->length_ <= remaining);
      assert(current->data_ != nullptr);
      remaining -= current->length_;
      memcpy(p, current->data_, current->length_);
      p += current->length_;
    }
    current = current->next_;
  } while (current != end);
  assert(remaining == 0);
  (void)remaining;

  // Point at the new buffer
  decrementRefcount();

  sharedInfo_ = newInfo;
  capacity_ = actualCapacity;
  buf_ = newBuf;
  data_ = newData;
  length_ = newLength;

  // Separate from the rest of our chain.
  // Since we don't store the unique_ptr returned by separateChain(),
  // this will immediately delete the returned subchain.
  if (isChained()) {
    (void)separateChain(next_, current->prev_);
  }
}

void IOBuf::decrementRefcount() noexcept {
  // Externally owned buffers don't have a SharedInfo object and aren't managed
  // by the reference count
  if (!sharedInfo_) {
    return;
  }

  // Avoid doing atomic decrement if the refcount is 1.
  // This is safe, because it means that we're the last reference and destroying
  // the object. Anything trying to copy it is already undefined behavior.
  if (sharedInfo_->refcount.load(std::memory_order_acquire) > 1) {
    // Decrement the refcount
    uint32_t newcnt =
        sharedInfo_->refcount.fetch_sub(1, std::memory_order_acq_rel);
    // Note that fetch_sub() returns the value before we decremented.
    // If it is 1, we were the only remaining user; if it is greater there are
    // still other users.
    if (newcnt > 1) {
      return;
    }
  }

  // Save the storage type since freeExtBuffer can free the SharedInfo.
  auto infoStorageType = sharedInfo_->storageType;
  freeExtBuffer(); // We were the last user. Free the buffer
  SharedInfo::releaseStorage(this, infoStorageType, sharedInfo_);
}

void IOBuf::reserveSlow(std::size_t minHeadroom, std::size_t minTailroom) {
  size_t newCapacity = length_;
  if (!checked_add(&newCapacity, newCapacity, minHeadroom) ||
      !checked_add(&newCapacity, newCapacity, minTailroom) ||
      newCapacity > kMaxIOBufSize) {
    // overflow
    throw_exception<std::bad_alloc>();
  }

  // reserveSlow() is dangerous if anyone else is sharing the buffer, as we may
  // reallocate and free the original buffer.  It should only ever be called if
  // we are the only user of the buffer.
  DCHECK(!isSharedOne());

  // We'll need to reallocate the buffer.
  // There are a few options.
  // - If we have enough total room, move the data around in the buffer
  //   and adjust the data_ pointer.
  // - If we're using an internal buffer, we'll switch to an external
  //   buffer with enough headroom and tailroom.
  // - If we have enough headroom (headroom() >= minHeadroom) but not too much
  //   (so we don't waste memory), we can try one of two things, depending on
  //   whether we use jemalloc or not:
  //   - If using jemalloc, we can try to expand in place, avoiding a memcpy()
  //   - If not using jemalloc and we don't have too much to copy,
  //     we'll use realloc() (note that realloc might have to copy
  //     headroom + data + tailroom, see smartRealloc in folly/memory/Malloc.h)
  // - Otherwise, bite the bullet and reallocate.
  if (headroom() + tailroom() >= minHeadroom + minTailroom) {
    uint8_t* newData = writableBuffer() + minHeadroom;
    memmove(newData, data_, length_);
    data_ = newData;
    return;
  }

  size_t newAllocatedCapacity = 0;
  uint8_t* newBuffer = nullptr;
  std::size_t newHeadroom = 0;
  std::size_t oldHeadroom = headroom();

  // If we have a buffer allocated with malloc and we just need more tailroom,
  // try to use realloc()/xallocx() to grow the buffer in place.
  SharedInfo* info = sharedInfo_;
  auto infoStorageType =
      info ? info->storageType : SharedInfo::StorageType::kInvalid;
  if (info && (info->freeFn == nullptr) && length_ != 0 &&
      oldHeadroom >= minHeadroom) {
    size_t headSlack = oldHeadroom - minHeadroom;
    newAllocatedCapacity = goodExtBufferSize(newCapacity + headSlack);
    if (usingJEMalloc()) {
      // We assume that tailroom is more useful and more important than
      // headroom (not least because realloc / xallocx allow us to grow the
      // buffer at the tail, but not at the head)  So, if we have more headroom
      // than we need, we consider that "wasted".  We arbitrarily define "too
      // much" headroom to be 25% of the capacity.
      if (headSlack * 4 <= newCapacity) {
        size_t allocatedCapacity = capacity() + sizeof(SharedInfo);
        void* p = buf_;
        if (allocatedCapacity >= jemallocMinInPlaceExpandable) {
          if (xallocx(p, newAllocatedCapacity, 0, 0) == newAllocatedCapacity) {
            if (io_buf_free_cb) {
              io_buf_free_cb(p, reinterpret_cast<size_t>(info->userData));
            }
            newBuffer = static_cast<uint8_t*>(p);
            newHeadroom = oldHeadroom;
            // update the userData
            info->userData = reinterpret_cast<void*>(newAllocatedCapacity);
            if (io_buf_alloc_cb) {
              io_buf_alloc_cb(newBuffer, newAllocatedCapacity);
            }
          }
          // if xallocx failed, do nothing, fall back to malloc/memcpy/free
        }
      }
    } else { // Not using jemalloc
      size_t copySlack = capacity() - length_;
      if (copySlack * 2 <= length_) {
        void* p = realloc(buf_, newAllocatedCapacity);
        if (FOLLY_UNLIKELY(p == nullptr)) {
          throw_exception<std::bad_alloc>();
        }
        newBuffer = static_cast<uint8_t*>(p);
        newHeadroom = oldHeadroom;
      }
    }
  }

  // None of the previous reallocation strategies worked (or we're using
  // an internal buffer).  malloc/copy/free.
  if (newBuffer == nullptr) {
    newAllocatedCapacity = goodExtBufferSize(newCapacity);
    newBuffer = static_cast<uint8_t*>(checkedMalloc(newAllocatedCapacity));
    if (length_ > 0) {
      assert(data_ != nullptr);
      memcpy(newBuffer + minHeadroom, data_, length_);
    }
    if (sharedInfo_) {
      freeExtBuffer();
    }
    newHeadroom = minHeadroom;
  }

  std::size_t cap;
  initExtBuffer(newBuffer, newAllocatedCapacity, &info, &cap);

  if (infoStorageType != SharedInfo::StorageType::kInvalid) {
    SharedInfo::releaseStorage(this, infoStorageType, sharedInfo_);
  }
  sharedInfo_ = info;

  capacity_ = cap;
  buf_ = newBuffer;
  data_ = newBuffer + newHeadroom;
  // length_ is unchanged
}

// The user's free function should never throw. Otherwise we might throw from
// the IOBuf destructor. Other code paths like coalesce() also assume that
// decrementRefcount() cannot throw.
void IOBuf::freeExtBuffer() noexcept {
  DCHECK(sharedInfo_);

  // Store all the needed information in case the SharedInfo's storage is in the
  // buffer and so we need to destroy it.
  auto observerListHead = std::exchange(sharedInfo_->observerListHead, nullptr);
  auto freeFn = sharedInfo_->freeFn;
  auto userData = sharedInfo_->userData;

  if (sharedInfo_->storageType == SharedInfo::StorageType::kExtBuffer) {
    sharedInfo_->~SharedInfo();
  }

  if (freeFn) {
    freeFn(buf_, userData);
  } else {
    size_t size = reinterpret_cast<size_t>(userData);
    if (size) {
      if (io_buf_free_cb) {
        io_buf_free_cb(buf_, size);
      }
      folly::sizedFree(buf_, size);
    } else {
      free(buf_);
    }
  }
  SharedInfo::invokeAndDeleteEachObserver(observerListHead, [](auto& entry) {
    entry.afterFreeExtBuffer();
  });

  if (kIsMobile) {
    buf_ = nullptr;
  }
}

void IOBuf::allocExtBuffer(
    std::size_t minCapacity,
    uint8_t** bufReturn,
    SharedInfo** infoReturn,
    std::size_t* capacityReturn) {
  if (minCapacity > kMaxIOBufSize) {
    throw_exception<std::bad_alloc>();
  }

  size_t mallocSize = goodExtBufferSize(minCapacity);
  auto buf = static_cast<uint8_t*>(checkedMalloc(mallocSize));
  initExtBuffer(buf, mallocSize, infoReturn, capacityReturn);

  // the userData and the freeFn are nullptr here
  // just store the mallocSize in userData
  (*infoReturn)->userData = reinterpret_cast<void*>(mallocSize);
  if (io_buf_alloc_cb) {
    io_buf_alloc_cb(buf, mallocSize);
  }

  *bufReturn = buf;
}

size_t IOBuf::goodExtBufferSize(std::size_t minCapacity) {
  if (minCapacity > kMaxIOBufSize) {
    throw_exception<std::bad_alloc>();
  }

  // Determine how much space we should allocate.  We'll store the SharedInfo
  // for the external buffer just after the buffer itself.  (We store it just
  // after the buffer rather than just before so that the code can still just
  // use free(buf_) to free the buffer.)
  size_t minSize = static_cast<size_t>(minCapacity) + sizeof(SharedInfo);
  // Add room for padding so that the SharedInfo will be aligned on an 8-byte
  // boundary.
  minSize = (minSize + 7) & static_cast<size_t>(~7);

  // Use goodMallocSize() to bump up the capacity to a decent size to request
  // from malloc, so we can use all of the space that malloc will probably give
  // us anyway.
  return goodMallocSize(minSize);
}

void IOBuf::initExtBuffer(
    uint8_t* buf,
    size_t mallocSize,
    SharedInfo** infoReturn,
    std::size_t* capacityReturn) {
  // Find the SharedInfo storage at the end of the buffer
  // and construct the SharedInfo.
  uint8_t* infoStart = (buf + mallocSize) - sizeof(SharedInfo);
  auto sharedInfo = new (infoStart)
      SharedInfo(nullptr, nullptr, SharedInfo::StorageType::kExtBuffer);

  *capacityReturn = std::size_t(infoStart - buf);
  *infoReturn = sharedInfo;
}

fbstring IOBuf::moveToFbString() {
  // we need to save SharedInfo's storage and observerListHead since sharedInfo_
  // may not be valid after fbstring str
  SharedInfo::StorageType infoStorageType = SharedInfo::StorageType::kInvalid;
  SharedInfoObserverEntryBase* observerListHead = nullptr;
  // malloc-allocated buffers are just fine, everything else needs
  // to be turned into one.
  if (!sharedInfo_ || // user owned, not ours to give up
      sharedInfo_->freeFn || // not malloc()-ed
      headroom() != 0 || // malloc()-ed block doesn't start at beginning
      tailroom() == 0 || // no room for NUL terminator
      isShared() || // shared
      isChained()) { // chained
    // We might as well get rid of all head and tailroom if we're going
    // to reallocate; we need 1 byte for NUL terminator.
    coalesceAndReallocate(0, computeChainDataLength(), this, 1);
  } else {
    if (sharedInfo_) {
      // if we do not call coalesceAndReallocate
      // we might need to call SharedInfo::releaseStorage()
      // and/or SharedInfo::invokeAndDeleteEachObserver()
      infoStorageType = sharedInfo_->storageType;
      // save the observerListHead
      // the coalesceAndReallocate path will call
      // decrementRefcount and freeExtBuffer if needed
      // so the observer lis notification is needed here
      observerListHead = std::exchange(sharedInfo_->observerListHead, nullptr);
    }
  }

  // Ensure NUL terminated
  *writableTail() = 0;
  fbstring str(
      reinterpret_cast<char*>(writableData()),
      length(),
      capacity(),
      AcquireMallocatedString());

  if (io_buf_free_cb && sharedInfo_ && sharedInfo_->userData) {
    io_buf_free_cb(
        writableData(), reinterpret_cast<size_t>(sharedInfo_->userData));
  }

  SharedInfo::invokeAndDeleteEachObserver(observerListHead, [](auto& entry) {
    entry.afterReleaseExtBuffer();
  });

  if (infoStorageType != SharedInfo::StorageType::kInvalid) {
    SharedInfo::releaseStorage(this, infoStorageType, sharedInfo_);
  }

  // Reset to a state where we can be deleted cleanly
  sharedInfo_ = nullptr;
  buf_ = nullptr;
  clear();
  return str;
}

IOBuf::Iterator IOBuf::cbegin() const {
  return Iterator(this, this);
}

IOBuf::Iterator IOBuf::cend() const {
  return Iterator(nullptr, nullptr);
}

std::unique_ptr<IOBuf> IOBuf::fromString(std::unique_ptr<std::string> ptr) {
  auto ret = takeOwnership(
      ptr->data(),
      ptr->size(),
      [](void*, void* userData) { delete static_cast<std::string*>(userData); },
      static_cast<void*>(ptr.get()));
  std::ignore = ptr.release();
  return ret;
}

folly::fbvector<struct iovec> IOBuf::getIov() const {
  folly::fbvector<struct iovec> iov;
  iov.reserve(countChainElements());
  appendToIov(&iov);
  return iov;
}

void IOBuf::appendToIov(folly::fbvector<struct iovec>* iov) const {
  IOBuf const* p = this;
  do {
    // some code can get confused by empty iovs, so skip them
    if (p->length() > 0) {
      iov->push_back({(void*)p->data(), folly::to<size_t>(p->length())});
    }
    p = p->next();
  } while (p != this);
}

unique_ptr<IOBuf> IOBuf::wrapIov(const iovec* vec, size_t count) {
  IOBuf result{};
  for (size_t i = 0; i < count; ++i) {
    size_t len = vec[i].iov_len;
    void* data = vec[i].iov_base;
    if (len > 0) {
      auto buf = wrapBuffer(data, len);
      result.appendToChain(std::move(buf));
    }
  }
  return result.isChained() ? result.pop() : create(0);
}

std::unique_ptr<IOBuf> IOBuf::takeOwnershipIov(
    const iovec* vec,
    size_t count,
    FreeFunction freeFn,
    void* userData,
    bool freeOnError) {
  unique_ptr<IOBuf> result = nullptr;
  for (size_t i = 0; i < count; ++i) {
    size_t len = vec[i].iov_len;
    void* data = vec[i].iov_base;
    if (len > 0) {
      auto buf = takeOwnership(data, len, freeFn, userData, freeOnError);
      if (!result) {
        result = std::move(buf);
      } else {
        result->appendToChain(std::move(buf));
      }
    }
  }
  if (FOLLY_UNLIKELY(result == nullptr)) {
    return create(0);
  }
  return result;
}

IOBuf::FillIovResult IOBuf::fillIov(struct iovec* iov, size_t len) const {
  IOBuf const* p = this;
  size_t i = 0;
  size_t totalBytes = 0;
  while (i < len) {
    // some code can get confused by empty iovs, so skip them
    if (p->length() > 0) {
      iov[i].iov_base = const_cast<uint8_t*>(p->data());
      iov[i].iov_len = p->length();
      totalBytes += p->length();
      i++;
    }
    p = p->next();
    if (p == this) {
      return {i, totalBytes};
    }
  }
  return {0, 0};
}

uint32_t IOBuf::approximateShareCountOne() const noexcept {
  if (FOLLY_UNLIKELY(!sharedInfo_)) {
    return 1U;
  }
  return sharedInfo_->refcount.load(std::memory_order_acquire);
}

size_t IOBufHash::operator()(const IOBuf& buf) const noexcept {
  folly::hash::SpookyHashV2 hasher;
  hasher.Init(0, 0);
  io::Cursor cursor(&buf);
  for (;;) {
    auto b = cursor.peekBytes();
    if (b.empty()) {
      break;
    }
    hasher.Update(b.data(), b.size());
    cursor.skip(b.size());
  }
  uint64_t h1;
  uint64_t h2;
  hasher.Final(&h1, &h2);
  return static_cast<std::size_t>(h1);
}

ordering IOBufCompare::impl(const IOBuf& a, const IOBuf& b) const noexcept {
  io::Cursor ca(&a);
  io::Cursor cb(&b);
  for (;;) {
    auto ba = ca.peekBytes();
    auto bb = cb.peekBytes();
    if (ba.empty() || bb.empty()) {
      return to_ordering(int(bb.empty()) - int(ba.empty()));
    }
    const size_t n = std::min(ba.size(), bb.size());
    DCHECK_GT(n, 0u);
    const ordering r = to_ordering(std::memcmp(ba.data(), bb.data(), n));
    if (r != ordering::eq) {
      return r;
    }
    // Cursor::skip() may throw if n is too large, but n is not too large here
    ca.skip(n);
    cb.skip(n);
  }
}

} // namespace folly
