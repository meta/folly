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

#include <folly/io/IOBuf.h>

#include <cstddef>
#include <limits>
#include <random>
#include <unordered_map>

#include <folly/Range.h>
#include <folly/io/TypedIOBuf.h>
#include <folly/memory/Malloc.h>
#include <folly/portability/GTest.h>

using folly::ByteRange;
using folly::fbstring;
using folly::IOBuf;
using folly::ordering;
using folly::StringPiece;
using folly::TypedIOBuf;
using std::unique_ptr;

void append(std::unique_ptr<IOBuf>& buf, StringPiece str) {
  EXPECT_LE(str.size(), buf->tailroom());
  memcpy(buf->writableData(), str.data(), str.size());
  buf->append(str.size());
}

void prepend(std::unique_ptr<IOBuf>& buf, StringPiece str) {
  EXPECT_LE(str.size(), buf->headroom());
  memcpy(buf->writableData() - str.size(), str.data(), str.size());
  buf->prepend(str.size());
}

TEST(IOBuf, Simple) {
  unique_ptr<IOBuf> buf(IOBuf::create(100));
  const auto cap = buf->capacity();
  EXPECT_LE(100, cap);
  EXPECT_EQ(0, buf->headroom());
  EXPECT_EQ(0, buf->length());
  EXPECT_EQ(cap, buf->tailroom());

  append(buf, "world");
  buf->advance(10);
  EXPECT_EQ(10, buf->headroom());
  EXPECT_EQ(5, buf->length());
  EXPECT_EQ(cap - 15, buf->tailroom());

  prepend(buf, "hello ");
  EXPECT_EQ(4, buf->headroom());
  EXPECT_EQ(11, buf->length());
  EXPECT_EQ(cap - 15, buf->tailroom());

  const char* p = reinterpret_cast<const char*>(buf->data());
  EXPECT_EQ("hello world", std::string(p, buf->length()));

  buf->clear();
  EXPECT_EQ(0, buf->headroom());
  EXPECT_EQ(0, buf->length());
  EXPECT_EQ(cap, buf->tailroom());
}

void testAllocSize(uint32_t requestedCapacity) {
  unique_ptr<IOBuf> iobuf(IOBuf::create(requestedCapacity));
  EXPECT_GE(iobuf->capacity(), requestedCapacity);
}

TEST(IOBuf, AllocSizes) {
  // Cover small evil values exhaustively.
  for (uint32_t i = 0; i < 1234; i++) {
    testAllocSize(i);
  }

  // Try with large allocation sizes that will require an external buffer.
  testAllocSize(9000);
  testAllocSize(1048575);
  testAllocSize(1048576);
  testAllocSize(1048577);
}

void deleteArrayBuffer(void* buf, void* arg) {
  uint32_t* deleteCount = static_cast<uint32_t*>(arg);
  ++(*deleteCount);
  uint8_t* bufPtr = static_cast<uint8_t*>(buf);
  delete[] bufPtr;
}

TEST(IOBuf, TakeOwnership) {
  uint32_t size1 = 99;
  uint8_t* buf1 = static_cast<uint8_t*>(malloc(size1));
  unique_ptr<IOBuf> iobuf1(IOBuf::takeOwnership(buf1, size1));
  EXPECT_EQ(buf1, iobuf1->data());
  EXPECT_EQ(size1, iobuf1->length());
  EXPECT_EQ(buf1, iobuf1->buffer());
  EXPECT_EQ(size1, iobuf1->capacity());

  uint32_t deleteCount = 0;
  uint32_t size2 = 4321;
  uint8_t* buf2 = new uint8_t[size2];
  unique_ptr<IOBuf> iobuf2(
      IOBuf::takeOwnership(buf2, size2, deleteArrayBuffer, &deleteCount));
  EXPECT_EQ(buf2, iobuf2->data());
  EXPECT_EQ(size2, iobuf2->length());
  EXPECT_EQ(buf2, iobuf2->buffer());
  EXPECT_EQ(size2, iobuf2->capacity());
  EXPECT_EQ(0, deleteCount);
  iobuf2.reset();
  EXPECT_EQ(1, deleteCount);

  deleteCount = 0;
  uint32_t size3 = 3456;
  uint8_t* buf3 = new uint8_t[size3];
  uint32_t length3 = 48;
  unique_ptr<IOBuf> iobuf3(
      IOBuf::takeOwnership(
          buf3, size3, length3, deleteArrayBuffer, &deleteCount));
  EXPECT_EQ(buf3, iobuf3->data());
  EXPECT_EQ(length3, iobuf3->length());
  EXPECT_EQ(buf3, iobuf3->buffer());
  EXPECT_EQ(size3, iobuf3->capacity());
  EXPECT_EQ(0, deleteCount);
  iobuf3.reset();
  EXPECT_EQ(1, deleteCount);

  deleteCount = 0;
  {
    uint32_t size4 = 1234;
    uint8_t* buf4 = new uint8_t[size4];
    uint32_t length4 = 48;
    IOBuf iobuf4(
        IOBuf::TAKE_OWNERSHIP,
        buf4,
        size4,
        length4,
        deleteArrayBuffer,
        &deleteCount);
    EXPECT_EQ(buf4, iobuf4.data());
    EXPECT_EQ(length4, iobuf4.length());
    EXPECT_EQ(buf4, iobuf4.buffer());
    EXPECT_EQ(size4, iobuf4.capacity());

    IOBuf iobuf5 = std::move(iobuf4);
    EXPECT_EQ(buf4, iobuf5.data());
    EXPECT_EQ(length4, iobuf5.length());
    EXPECT_EQ(buf4, iobuf5.buffer());
    EXPECT_EQ(size4, iobuf5.capacity());
    EXPECT_EQ(0, deleteCount);
  }
  EXPECT_EQ(1, deleteCount);
  {
    uint32_t size = 2;
    uint8_t* buf = static_cast<uint8_t*>(malloc(size));
    buf[0] = 'A';
    unique_ptr<IOBuf> iobuf(IOBuf::takeOwnership(buf, size, 1));
    fbstring str = iobuf->moveToFbString();
    EXPECT_EQ(str, "A");
  }

  deleteCount = 0;
  uint32_t size6 = 100;
  uint8_t* buf6 = new uint8_t[size6];
  uint32_t offset6 = 48;
  uint32_t length6 = 48;
  unique_ptr<IOBuf> iobuf6(
      IOBuf::takeOwnership(
          buf6, size6, offset6, length6, deleteArrayBuffer, &deleteCount));
  EXPECT_EQ(buf6 + offset6, iobuf6->data());
  EXPECT_EQ(length6, iobuf6->length());
  EXPECT_EQ(buf6, iobuf6->buffer());
  EXPECT_EQ(size6, iobuf6->capacity());
  EXPECT_EQ(0, deleteCount);
  iobuf6.reset();
  EXPECT_EQ(1, deleteCount);
}

TEST(IOBuf, TakeOwnershipFreeOnErrorBugfix) {
  static void* freedBuf = nullptr;
  static void* freedUserData = nullptr;

  folly::IOBuf::FreeFunction freeFn =
      [](void* calledBuf, void* calledUserData) {
        freedBuf = calledBuf;
        freedUserData = calledUserData;
      };

  int userData = 0;
  char buf[1024];

  EXPECT_THROW(
      IOBuf::takeOwnership(
          buf, std::numeric_limits<size_t>::max(), freeFn, &userData, true),
      std::bad_alloc);

  EXPECT_EQ(freedBuf, buf);
  EXPECT_EQ(freedUserData, &userData);
}

TEST(IOBuf, GetUserData) {
  {
    const uint32_t size = 1234;
    uint8_t data[size] = {};
    unique_ptr<IOBuf> buf1(IOBuf::wrapBuffer(data, size));
    EXPECT_EQ(buf1->getUserData(), nullptr);
  }

  {
    size_t val = 0;
    uint32_t size = 4321;
    uint8_t* data = static_cast<uint8_t*>(malloc(size));
    unique_ptr<IOBuf> buf2(
        IOBuf::takeOwnership(
            data,
            size,
            [](void* buf, void* userData) {
              EXPECT_EQ(*static_cast<size_t*>(userData), 400);
              free(buf);
            },
            &val));
    EXPECT_EQ(buf2->getUserData(), &val);
    val = 200;
    EXPECT_EQ(*static_cast<size_t*>(buf2->getUserData()), 200);
    val = 400;
  }
}

TEST(IOBuf, GetFreeFn) {
  const uint32_t size = 4576;
  uint8_t* data = static_cast<uint8_t*>(malloc(size));
  std::memset(data, 0, size);
  folly::IOBuf::FreeFunction someFreeFn = [](void* buf, void* userData) {
    EXPECT_EQ(buf, userData);
    free(userData);
  };

  unique_ptr<IOBuf> someBuf(IOBuf::wrapBuffer(data, size));
  unique_ptr<IOBuf> someOtherBuf(
      IOBuf::takeOwnership(data, size, someFreeFn, data));

  EXPECT_EQ(someBuf->getFreeFn(), nullptr);
  EXPECT_EQ(someOtherBuf->getFreeFn(), someFreeFn);
}

TEST(IOBuf, WrapBuffer) {
  const uint32_t size1 = 1234;
  uint8_t buf1[size1] = {};
  unique_ptr<IOBuf> iobuf1(IOBuf::wrapBuffer(buf1, size1));
  EXPECT_EQ(buf1, iobuf1->data());
  EXPECT_EQ(size1, iobuf1->length());
  EXPECT_EQ(buf1, iobuf1->buffer());
  EXPECT_EQ(size1, iobuf1->capacity());

  uint32_t size2 = 0x1234;
  unique_ptr<uint8_t[]> buf2(new uint8_t[size2]);
  unique_ptr<IOBuf> iobuf2(IOBuf::wrapBuffer(buf2.get(), size2));
  EXPECT_EQ(buf2.get(), iobuf2->data());
  EXPECT_EQ(size2, iobuf2->length());
  EXPECT_EQ(buf2.get(), iobuf2->buffer());
  EXPECT_EQ(size2, iobuf2->capacity());

  uint32_t size3 = 4321;
  unique_ptr<uint8_t[]> buf3(new uint8_t[size3]);
  IOBuf iobuf3(IOBuf::WRAP_BUFFER, buf3.get(), size3);
  EXPECT_EQ(buf3.get(), iobuf3.data());
  EXPECT_EQ(size3, iobuf3.length());
  EXPECT_EQ(buf3.get(), iobuf3.buffer());
  EXPECT_EQ(size3, iobuf3.capacity());

  const uint32_t size4 = 2345;
  unique_ptr<uint8_t[]> buf4(new uint8_t[size4]);
  IOBuf iobuf4 = IOBuf::wrapBufferAsValue(buf4.get(), size4);
  EXPECT_EQ(buf4.get(), iobuf4.data());
  EXPECT_EQ(size4, iobuf4.length());
  EXPECT_EQ(buf4.get(), iobuf4.buffer());
  EXPECT_EQ(size4, iobuf4.capacity());

  if (folly::kIsSanitizeAddress) {
    const uint32_t size5 = 100;
    uint8_t buf5[size5];
    EXPECT_DEATH(IOBuf::wrapBuffer(buf5, size5 + 1), "stack-buffer-overflow");

    const uint32_t size6 = 100;
    std::vector<uint8_t> buf6(size6);
    EXPECT_DEATH(
        IOBuf::wrapBuffer(buf6.data(), size6 + 1), "heap-buffer-overflow");
  }
}

TEST(IOBuf, CreateCombined) {
  // Create a combined IOBuf, then destroy it.
  // The data buffer and IOBuf both become unused as part of the destruction
  {
    auto buf = IOBuf::createCombined(256);
    EXPECT_FALSE(buf->isShared());
  }

  // Create a combined IOBuf, clone from it, and then destroy the original
  // IOBuf.  The data buffer cannot be deleted until the clone is also
  // destroyed.
  {
    auto bufA = IOBuf::createCombined(256);
    EXPECT_FALSE(bufA->isShared());
    auto bufB = bufA->clone();
    EXPECT_TRUE(bufA->isShared());
    EXPECT_TRUE(bufB->isShared());
    bufA.reset();
    EXPECT_FALSE(bufB->isShared());
  }

  // Create a combined IOBuf, then call reserve() to get a larger buffer.
  // The IOBuf no longer points to the combined data buffer, but the
  // overall memory segment cannot be deleted until the IOBuf is also
  // destroyed.
  {
    auto buf = IOBuf::createCombined(256);
    buf->reserve(0, buf->capacity() + 100);
  }

  // Create a combined IOBuf, clone from it, then call unshare() on the original
  // buffer.  This creates a situation where bufB is pointing at the combined
  // buffer associated with bufA, but bufA is now using a different buffer.
  auto testSwap = [](bool resetAFirst) {
    auto bufA = IOBuf::createCombined(256);
    EXPECT_FALSE(bufA->isShared());
    auto bufB = bufA->clone();
    EXPECT_TRUE(bufA->isShared());
    EXPECT_TRUE(bufB->isShared());
    bufA->unshare();
    EXPECT_FALSE(bufA->isShared());
    EXPECT_FALSE(bufB->isShared());

    if (resetAFirst) {
      bufA.reset();
      bufB.reset();
    } else {
      bufB.reset();
      bufA.reset();
    }
  };
  testSwap(true);
  testSwap(false);
}

void fillBuf(uint8_t* buf, uint32_t length, std::mt19937& gen) {
  for (uint32_t n = 0; n < length; ++n) {
    buf[n] = static_cast<uint8_t>(gen() & 0xff);
  }
}

void fillBuf(IOBuf* buf, std::mt19937& gen) {
  buf->unshare();
  fillBuf(buf->writableData(), buf->length(), gen);
}

void checkBuf(const uint8_t* buf, uint32_t length, std::mt19937& gen) {
  // Rather than using EXPECT_EQ() to check each character,
  // count the number of differences and the first character that differs.
  // This way on error we'll report just that information, rather than tons of
  // failed checks for each byte in the buffer.
  uint32_t numDifferences = 0;
  uint32_t firstDiffIndex = 0;
  uint8_t firstDiffExpected = 0;
  for (uint32_t n = 0; n < length; ++n) {
    uint8_t expected = static_cast<uint8_t>(gen() & 0xff);
    if (buf[n] == expected) {
      continue;
    }

    if (numDifferences == 0) {
      firstDiffIndex = n;
      firstDiffExpected = expected;
    }
    ++numDifferences;
  }

  EXPECT_EQ(0, numDifferences);
  if (numDifferences > 0) {
    // Cast to int so it will be printed numerically
    // rather than as a char if the check fails
    EXPECT_EQ(
        static_cast<int>(buf[firstDiffIndex]),
        static_cast<int>(firstDiffExpected));
  }
}

void checkBuf(IOBuf* buf, std::mt19937& gen) {
  checkBuf(buf->data(), buf->length(), gen);
}

void checkBuf(ByteRange buf, std::mt19937& gen) {
  checkBuf(buf.data(), buf.size(), gen);
}

void checkChain(IOBuf* buf, std::mt19937& gen) {
  IOBuf* current = buf;
  do {
    checkBuf(current->data(), current->length(), gen);
    current = current->next();
  } while (current != buf);
}

TEST(IOBuf, Chaining) {
  uint32_t fillSeed = 0x12345678;
  std::mt19937 gen(fillSeed);

  // An IOBuf with external storage
  uint32_t headroom = 123;
  unique_ptr<IOBuf> iob1(IOBuf::create(2048));
  iob1->advance(headroom);
  iob1->append(1500);
  fillBuf(iob1.get(), gen);

  // An IOBuf with internal storage
  unique_ptr<IOBuf> iob2(IOBuf::create(20));
  iob2->append(20);
  fillBuf(iob2.get(), gen);

  // An IOBuf around a buffer it doesn't own
  uint8_t localbuf[1234];
  fillBuf(localbuf, 1234, gen);
  unique_ptr<IOBuf> iob3(IOBuf::wrapBuffer(localbuf, sizeof(localbuf)));

  // An IOBuf taking ownership of a user-supplied buffer
  uint32_t heapBufSize = 900;
  uint8_t* heapBuf = static_cast<uint8_t*>(malloc(heapBufSize));
  fillBuf(heapBuf, heapBufSize, gen);
  unique_ptr<IOBuf> iob4(IOBuf::takeOwnership(heapBuf, heapBufSize));

  // An IOBuf taking ownership of a user-supplied buffer with
  // a custom free function
  uint32_t arrayBufSize = 321;
  uint8_t* arrayBuf = new uint8_t[arrayBufSize];
  fillBuf(arrayBuf, arrayBufSize, gen);
  uint32_t arrayBufFreeCount = 0;
  unique_ptr<IOBuf> iob5(
      IOBuf::takeOwnership(
          arrayBuf, arrayBufSize, deleteArrayBuffer, &arrayBufFreeCount));

  EXPECT_FALSE(iob1->isChained());
  EXPECT_FALSE(iob2->isChained());
  EXPECT_FALSE(iob3->isChained());
  EXPECT_FALSE(iob4->isChained());
  EXPECT_FALSE(iob5->isChained());

  EXPECT_FALSE(iob1->isSharedOne());
  EXPECT_FALSE(iob2->isSharedOne());
  EXPECT_TRUE(iob3->isSharedOne()); // since we own the buffer
  EXPECT_FALSE(iob4->isSharedOne());
  EXPECT_FALSE(iob5->isSharedOne());

  // Chain the buffers all together
  // Since we are going to relinquish ownership of iob2-5 to the chain,
  // store raw pointers to them so we can reference them later.
  IOBuf* iob2ptr = iob2.get();
  IOBuf* iob3ptr = iob3.get();
  IOBuf* iob4ptr = iob4.get();
  IOBuf* iob5ptr = iob5.get();

  iob1->prependChain(std::move(iob2));
  iob1->prependChain(std::move(iob4));
  iob2ptr->insertAfterThisOne(std::move(iob3));
  iob1->prependChain(std::move(iob5));

  EXPECT_EQ(iob2ptr, iob1->next());
  EXPECT_EQ(iob3ptr, iob2ptr->next());
  EXPECT_EQ(iob4ptr, iob3ptr->next());
  EXPECT_EQ(iob5ptr, iob4ptr->next());
  EXPECT_EQ(iob1.get(), iob5ptr->next());

  EXPECT_EQ(iob5ptr, iob1->prev());
  EXPECT_EQ(iob1.get(), iob2ptr->prev());
  EXPECT_EQ(iob2ptr, iob3ptr->prev());
  EXPECT_EQ(iob3ptr, iob4ptr->prev());
  EXPECT_EQ(iob4ptr, iob5ptr->prev());

  EXPECT_TRUE(iob1->isChained());
  EXPECT_TRUE(iob2ptr->isChained());
  EXPECT_TRUE(iob3ptr->isChained());
  EXPECT_TRUE(iob4ptr->isChained());
  EXPECT_TRUE(iob5ptr->isChained());

  std::size_t fullLength =
      (iob1->length() + iob2ptr->length() + iob3ptr->length() +
       iob4ptr->length() + iob5ptr->length());
  EXPECT_EQ(5, iob1->countChainElements());
  EXPECT_EQ(fullLength, iob1->computeChainDataLength());

  // Since iob3 is shared, the entire buffer should report itself as shared
  EXPECT_TRUE(iob1->isShared());
  // Unshare just iob3
  iob3ptr->unshareOne();
  EXPECT_FALSE(iob3ptr->isSharedOne());
  // Now everything in the chain should be unshared.
  // Check on all members of the chain just for good measure
  EXPECT_FALSE(iob1->isShared());
  EXPECT_FALSE(iob2ptr->isShared());
  EXPECT_FALSE(iob3ptr->isShared());
  EXPECT_FALSE(iob4ptr->isShared());
  EXPECT_FALSE(iob5ptr->isShared());

  // Check iteration
  gen.seed(fillSeed);
  size_t count = 0;
  for (auto buf : *iob1) {
    checkBuf(buf, gen);
    ++count;
  }
  EXPECT_EQ(5, count);

  // Clone one of the IOBufs in the chain
  unique_ptr<IOBuf> iob4clone = iob4ptr->cloneOne();
  gen.seed(fillSeed);
  checkBuf(iob1.get(), gen);
  checkBuf(iob2ptr, gen);
  checkBuf(iob3ptr, gen);
  checkBuf(iob4clone.get(), gen);
  checkBuf(iob5ptr, gen);

  EXPECT_TRUE(iob1->isShared());
  EXPECT_TRUE(iob2ptr->isShared());
  EXPECT_TRUE(iob3ptr->isShared());
  EXPECT_TRUE(iob4ptr->isShared());
  EXPECT_TRUE(iob5ptr->isShared());

  EXPECT_FALSE(iob1->isSharedOne());
  EXPECT_FALSE(iob2ptr->isSharedOne());
  EXPECT_FALSE(iob3ptr->isSharedOne());
  EXPECT_TRUE(iob4ptr->isSharedOne());
  EXPECT_FALSE(iob5ptr->isSharedOne());

  // Unshare that clone
  EXPECT_TRUE(iob4clone->isSharedOne());
  iob4clone->unshare();
  EXPECT_FALSE(iob4clone->isSharedOne());
  EXPECT_FALSE(iob4ptr->isSharedOne());
  EXPECT_FALSE(iob1->isShared());
  iob4clone.reset();

  // Create a clone of a different IOBuf
  EXPECT_FALSE(iob1->isShared());
  EXPECT_FALSE(iob3ptr->isSharedOne());

  unique_ptr<IOBuf> iob3clone = iob3ptr->cloneOne();
  gen.seed(fillSeed);
  checkBuf(iob1.get(), gen);
  checkBuf(iob2ptr, gen);
  checkBuf(iob3clone.get(), gen);
  checkBuf(iob4ptr, gen);
  checkBuf(iob5ptr, gen);

  EXPECT_TRUE(iob1->isShared());
  EXPECT_TRUE(iob3ptr->isSharedOne());
  EXPECT_FALSE(iob1->isSharedOne());

  // Delete the clone and make sure the original is unshared
  iob3clone.reset();
  EXPECT_FALSE(iob1->isShared());
  EXPECT_FALSE(iob3ptr->isSharedOne());

  // Clone the entire chain
  unique_ptr<IOBuf> chainClone = iob1->clone();
  // Verify that the data is correct.
  EXPECT_EQ(fullLength, chainClone->computeChainDataLength());
  gen.seed(fillSeed);
  checkChain(chainClone.get(), gen);

  // Check that the buffers report sharing correctly
  EXPECT_TRUE(chainClone->isShared());
  EXPECT_TRUE(iob1->isShared());

  EXPECT_TRUE(iob1->isSharedOne());
  // Also verify the share count is consistent:
  EXPECT_LT(1, iob1->approximateShareCountOne());
  EXPECT_TRUE(iob2ptr->isSharedOne());
  EXPECT_TRUE(iob3ptr->isSharedOne());
  EXPECT_TRUE(iob4ptr->isSharedOne());
  EXPECT_TRUE(iob5ptr->isSharedOne());

  // Unshare the cloned chain
  chainClone->unshare();
  EXPECT_FALSE(chainClone->isShared());
  EXPECT_FALSE(iob1->isShared());
  // Also verify the share count is consistent:
  EXPECT_EQ(1, iob1->approximateShareCountOne());

  // Make sure the unshared result still has the same data
  EXPECT_EQ(fullLength, chainClone->computeChainDataLength());
  gen.seed(fillSeed);
  checkChain(chainClone.get(), gen);

  // Destroy this chain
  chainClone.reset();

  // Clone a new chain
  EXPECT_FALSE(iob1->isShared());
  chainClone = iob1->clone();
  EXPECT_TRUE(iob1->isShared());
  EXPECT_TRUE(chainClone->isShared());

  // Delete the original chain
  iob1.reset();
  EXPECT_FALSE(chainClone->isShared());

  // Coalesce the chain
  //
  // Coalescing this chain will create a new buffer and release the last
  // refcount on the original buffers we created.  Also make sure
  // that arrayBufFreeCount increases to one to indicate that arrayBuf was
  // freed.
  EXPECT_EQ(5, chainClone->countChainElements());
  EXPECT_EQ(0, arrayBufFreeCount);

  // Buffer lengths: 1500 20 1234 900 321
  // Attempting to gather more data than available should fail
  EXPECT_THROW(chainClone->gather(4000), std::overflow_error);
  // Coalesce the first 3 buffers
  chainClone->gather(1521);
  EXPECT_EQ(3, chainClone->countChainElements());
  EXPECT_EQ(0, arrayBufFreeCount);

  // Make sure the data is still the same after coalescing
  EXPECT_EQ(fullLength, chainClone->computeChainDataLength());
  gen.seed(fillSeed);
  checkChain(chainClone.get(), gen);

  // cloneCoalesced
  {
    auto chainCloneCoalesced = chainClone->cloneCoalesced();
    EXPECT_EQ(1, chainCloneCoalesced->countChainElements());
    EXPECT_EQ(fullLength, chainCloneCoalesced->computeChainDataLength());
    gen.seed(fillSeed);
    checkChain(chainCloneCoalesced.get(), gen);
  }

  // Coalesce the entire chain
  chainClone->coalesce();
  EXPECT_EQ(1, chainClone->countChainElements());
  EXPECT_EQ(1, arrayBufFreeCount);

  // Make sure the data is still the same after coalescing
  EXPECT_EQ(fullLength, chainClone->computeChainDataLength());
  gen.seed(fillSeed);
  checkChain(chainClone.get(), gen);

  // Make a new chain to test the unlink and pop operations
  iob1 = IOBuf::create(1);
  iob1->append(1);
  IOBuf* iob1ptr = iob1.get();
  iob2 = IOBuf::create(3);
  iob2->append(3);
  iob2ptr = iob2.get();
  iob3 = IOBuf::create(5);
  iob3->append(5);
  iob3ptr = iob3.get();
  iob4 = IOBuf::create(7);
  iob4->append(7);
  iob4ptr = iob4.get();
  iob1->insertAfterThisOne(std::move(iob2));
  iob1->prev()->insertAfterThisOne(std::move(iob3));
  iob1->prev()->insertAfterThisOne(std::move(iob4));
  EXPECT_EQ(4, iob1->countChainElements());
  EXPECT_EQ(16, iob1->computeChainDataLength());

  // Unlink from the middle of the chain
  iob3 = iob3ptr->unlink();
  EXPECT_TRUE(iob3.get() == iob3ptr);
  EXPECT_EQ(3, iob1->countChainElements());
  EXPECT_EQ(11, iob1->computeChainDataLength());

  // Unlink from the end of the chain
  iob4 = iob1->prev()->unlink();
  EXPECT_TRUE(iob4.get() == iob4ptr);
  EXPECT_EQ(2, iob1->countChainElements());
  EXPECT_TRUE(iob1->next() == iob2ptr);
  EXPECT_EQ(4, iob1->computeChainDataLength());

  // Pop from the front of the chain
  iob2 = iob1->pop();
  EXPECT_TRUE(iob1.get() == iob1ptr);
  EXPECT_EQ(1, iob1->countChainElements());
  EXPECT_EQ(1, iob1->computeChainDataLength());
  EXPECT_TRUE(iob2.get() == iob2ptr);
  EXPECT_EQ(1, iob2->countChainElements());
  EXPECT_EQ(3, iob2->computeChainDataLength());
}

void testFreeFn(void* buffer, void* ptr) {
  uint32_t* freeCount = static_cast<uint32_t*>(ptr);
  ;
  delete[] static_cast<uint8_t*>(buffer);
  if (freeCount) {
    ++(*freeCount);
  }
}

TEST(IOBuf, Reserve) {
  uint32_t fillSeed = 0x23456789;
  std::mt19937 gen(fillSeed);

  // Reserve does nothing if empty and doesn't have to grow the buffer
  {
    gen.seed(fillSeed);
    unique_ptr<IOBuf> iob(IOBuf::create(2000));
    EXPECT_EQ(0, iob->headroom());
    const void* p1 = iob->buffer();
    iob->reserve(5, 15);
    EXPECT_LE(5, iob->headroom());
    EXPECT_EQ(p1, iob->buffer());
  }

  // Reserve doesn't reallocate if we have enough total room
  {
    gen.seed(fillSeed);
    unique_ptr<IOBuf> iob(IOBuf::create(2000));
    iob->append(100);
    fillBuf(iob.get(), gen);
    EXPECT_EQ(0, iob->headroom());
    EXPECT_EQ(100, iob->length());
    const void* p1 = iob->buffer();
    const uint8_t* d1 = iob->data();
    iob->reserve(100, 1800);
    EXPECT_LE(100, iob->headroom());
    EXPECT_EQ(p1, iob->buffer());
    EXPECT_EQ(d1 + 100, iob->data());
    gen.seed(fillSeed);
    checkBuf(iob.get(), gen);
  }

  // Reserve reallocates if we don't have enough total room.
  // NOTE that, with jemalloc, we know that this won't reallocate in place
  // as the size is less than jemallocMinInPlaceExpanadable
  {
    gen.seed(fillSeed);
    unique_ptr<IOBuf> iob(IOBuf::create(2000));
    iob->append(100);
    fillBuf(iob.get(), gen);
    EXPECT_EQ(0, iob->headroom());
    EXPECT_EQ(100, iob->length());
    const void* p1 = iob->buffer();
    iob->reserve(100, 2512); // allocation sizes are multiples of 256
    EXPECT_LE(100, iob->headroom());
    if (folly::usingJEMalloc()) {
      EXPECT_NE(p1, iob->buffer());
    }
    gen.seed(fillSeed);
    checkBuf(iob.get(), gen);
  }

  // Test reserve from internal buffer, this used to segfault
  {
    unique_ptr<IOBuf> iob(IOBuf::create(0));
    iob->reserve(0, 2000);
    EXPECT_EQ(0, iob->headroom());
    EXPECT_LE(2000, iob->tailroom());
  }

  // Test reserving from a user-allocated buffer.
  {
    uint8_t* buf = static_cast<uint8_t*>(malloc(100));
    auto iob = IOBuf::takeOwnership(buf, 100);
    iob->reserve(0, 2000);
    EXPECT_EQ(0, iob->headroom());
    EXPECT_LE(2000, iob->tailroom());
  }

  // Test reserving from a user-allocated with a custom free function.
  {
    uint32_t freeCount{0};
    uint8_t* buf = new uint8_t[100];
    auto iob = IOBuf::takeOwnership(buf, 100, testFreeFn, &freeCount);
    iob->reserve(0, 2000);
    EXPECT_EQ(0, iob->headroom());
    EXPECT_LE(2000, iob->tailroom());
    EXPECT_EQ(1, freeCount);
  }
}

TEST(IOBuf, copyBuffer) {
  std::string s("hello");
  auto buf = IOBuf::copyBuffer(s.data(), s.size(), 1, 2);
  EXPECT_EQ(1, buf->headroom());
  EXPECT_EQ(
      s,
      std::string(reinterpret_cast<const char*>(buf->data()), buf->length()));
  EXPECT_LE(2, buf->tailroom());

  buf = IOBuf::copyBuffer(s, 5, 7);
  EXPECT_EQ(5, buf->headroom());
  EXPECT_EQ(
      s,
      std::string(reinterpret_cast<const char*>(buf->data()), buf->length()));
  EXPECT_LE(7, buf->tailroom());

  std::string empty;
  buf = IOBuf::copyBuffer(empty, 3, 6);
  EXPECT_EQ(3, buf->headroom());
  EXPECT_EQ(0, buf->length());
  EXPECT_LE(6, buf->tailroom());

  // A stack-allocated version
  IOBuf stackBuf(IOBuf::COPY_BUFFER, s, 1, 2);
  EXPECT_EQ(1, stackBuf.headroom());
  EXPECT_EQ(
      s,
      std::string(
          reinterpret_cast<const char*>(stackBuf.data()), stackBuf.length()));
  EXPECT_LE(2, stackBuf.tailroom());
}

TEST(IOBuf, maybeCopyBuffer) {
  std::string s("this is a test");
  auto buf = IOBuf::maybeCopyBuffer(s, 1, 2);
  EXPECT_EQ(1, buf->headroom());
  EXPECT_EQ(
      s,
      std::string(reinterpret_cast<const char*>(buf->data()), buf->length()));
  EXPECT_LE(2, buf->tailroom());

  std::string empty;
  buf = IOBuf::maybeCopyBuffer("", 5, 7);
  EXPECT_EQ(nullptr, buf.get());

  buf = IOBuf::maybeCopyBuffer("");
  EXPECT_EQ(nullptr, buf.get());
}

TEST(IOBuf, copyEmptyBuffer) {
  auto buf = IOBuf::copyBuffer(nullptr, 0);
  EXPECT_EQ(buf->length(), 0);
}

namespace {

int customDeleterCount = 0;
int destructorCount = 0;
struct OwnershipTestClass {
  explicit OwnershipTestClass(int v = 0) : val(v) {}
  ~OwnershipTestClass() { ++destructorCount; }
  int val;
};

using CustomDeleter = std::function<void(OwnershipTestClass*)>;

void customDelete(OwnershipTestClass* p) {
  ++customDeleterCount;
  delete p;
}

void customDeleteArray(OwnershipTestClass* p) {
  ++customDeleterCount;
  delete[] p;
}

} // namespace

TEST(IOBuf, takeOwnershipUniquePtr) {
  destructorCount = 0;
  {
    std::unique_ptr<OwnershipTestClass> p(new OwnershipTestClass());
  }
  EXPECT_EQ(1, destructorCount);

  destructorCount = 0;
  {
    std::unique_ptr<OwnershipTestClass[]> p(new OwnershipTestClass[2]);
  }
  EXPECT_EQ(2, destructorCount);

  destructorCount = 0;
  {
    std::unique_ptr<OwnershipTestClass> p(new OwnershipTestClass());
    std::unique_ptr<IOBuf> buf(IOBuf::takeOwnership(std::move(p)));
    EXPECT_EQ(sizeof(OwnershipTestClass), buf->length());
    EXPECT_EQ(0, destructorCount);
  }
  EXPECT_EQ(1, destructorCount);

  destructorCount = 0;
  {
    std::unique_ptr<OwnershipTestClass[]> p(new OwnershipTestClass[2]);
    std::unique_ptr<IOBuf> buf(IOBuf::takeOwnership(std::move(p), 2));
    EXPECT_EQ(2 * sizeof(OwnershipTestClass), buf->length());
    EXPECT_EQ(0, destructorCount);
  }
  EXPECT_EQ(2, destructorCount);

  customDeleterCount = 0;
  destructorCount = 0;
  {
    std::unique_ptr<OwnershipTestClass, CustomDeleter> p(
        new OwnershipTestClass(), customDelete);
    std::unique_ptr<IOBuf> buf(IOBuf::takeOwnership(std::move(p)));
    EXPECT_EQ(sizeof(OwnershipTestClass), buf->length());
    EXPECT_EQ(0, destructorCount);
  }
  EXPECT_EQ(1, destructorCount);
  EXPECT_EQ(1, customDeleterCount);

  customDeleterCount = 0;
  destructorCount = 0;
  {
    std::unique_ptr<OwnershipTestClass[], CustomDeleter> p(
        new OwnershipTestClass[2], CustomDeleter(customDeleteArray));
    std::unique_ptr<IOBuf> buf(IOBuf::takeOwnership(std::move(p), 2));
    EXPECT_EQ(2 * sizeof(OwnershipTestClass), buf->length());
    EXPECT_EQ(0, destructorCount);
  }
  EXPECT_EQ(2, destructorCount);
  EXPECT_EQ(1, customDeleterCount);
}

TEST(IOBuf, Alignment) {
  size_t alignment = alignof(std::max_align_t);

  std::vector<size_t> sizes;
  sizes.reserve(259);
  for (size_t i = 0; i <= 256; i++) {
    sizes.push_back(i);
  }
  sizes.push_back(1024);
  sizes.push_back(1 << 10);

  for (size_t size : sizes) {
    auto buf = IOBuf::create(size);
    uintptr_t p = reinterpret_cast<uintptr_t>(buf->data());
    EXPECT_EQ(0, p & (alignment - 1)) << "size=" << size;
  }
}

TEST(TypedIOBuf, Simple) {
  auto buf = IOBuf::create(0);
  TypedIOBuf<std::size_t> typed(buf.get());
  const std::size_t n = 10000;
  typed.reserve(0, n);
  EXPECT_LE(n, typed.capacity());
  for (std::size_t i = 0; i < n; i++) {
    *typed.writableTail() = i;
    typed.append(1);
  }
  EXPECT_EQ(n, typed.length());
  for (std::size_t i = 0; i < n; i++) {
    EXPECT_EQ(i, typed.data()[i]);
  }
}
enum BufType {
  CREATE,
  TAKE_OWNERSHIP_MALLOC,
  TAKE_OWNERSHIP_CUSTOM,
  USER_OWNED,
};

// chain element size, number of elements in chain, shared
class MoveToFbStringTest
    : public ::testing::TestWithParam<std::tuple<int, int, bool, BufType>> {
 protected:
  void SetUp() override {
    elementSize_ = std::get<0>(GetParam());
    elementCount_ = std::get<1>(GetParam());
    shared_ = std::get<2>(GetParam());
    type_ = std::get<3>(GetParam());

    buf_ = makeBuf();
    for (int i = 0; i < elementCount_ - 1; ++i) {
      buf_->prependChain(makeBuf());
    }
    EXPECT_EQ(elementCount_, buf_->countChainElements());
    EXPECT_EQ(elementCount_ * elementSize_, buf_->computeChainDataLength());
    if (shared_) {
      buf2_ = buf_->clone();
      EXPECT_EQ(elementCount_, buf2_->countChainElements());
      EXPECT_EQ(elementCount_ * elementSize_, buf2_->computeChainDataLength());
    }
  }

  std::unique_ptr<IOBuf> makeBuf() {
    unique_ptr<IOBuf> buf;
    switch (type_) {
      case CREATE:
        buf = IOBuf::create(elementSize_);
        buf->append(elementSize_);
        break;
      case TAKE_OWNERSHIP_MALLOC: {
        void* data = malloc(elementSize_);
        if (!data) {
          throw std::bad_alloc();
        }
        buf = IOBuf::takeOwnership(data, elementSize_);
        break;
      }
      case TAKE_OWNERSHIP_CUSTOM: {
        uint8_t* data = new uint8_t[elementSize_];
        buf = IOBuf::takeOwnership(data, elementSize_, testFreeFn);
        break;
      }
      case USER_OWNED: {
        unique_ptr<uint8_t[]> data(new uint8_t[elementSize_]);
        buf = IOBuf::wrapBuffer(data.get(), elementSize_);
        ownedBuffers_.emplace_back(std::move(data));
        break;
      }
      default:
        throw std::invalid_argument("unexpected buffer type parameter");
    }
    memset(buf->writableData(), 'x', elementSize_);
    return buf;
  }

  void check(std::unique_ptr<IOBuf>& buf) {
    fbstring str = buf->moveToFbString();
    EXPECT_EQ(elementCount_ * elementSize_, str.size());
    EXPECT_EQ(elementCount_ * elementSize_, strspn(str.c_str(), "x"));
    EXPECT_EQ(0, buf->length());
    EXPECT_EQ(1, buf->countChainElements());
    EXPECT_EQ(0, buf->computeChainDataLength());
    EXPECT_FALSE(buf->isChained());
  }

  int elementSize_;
  int elementCount_;
  bool shared_;
  BufType type_;
  std::unique_ptr<IOBuf> buf_;
  std::unique_ptr<IOBuf> buf2_;
  std::vector<std::unique_ptr<uint8_t[]>> ownedBuffers_;
};

TEST_P(MoveToFbStringTest, Simple) {
  check(buf_);
  if (shared_) {
    check(buf2_);
  }
}

INSTANTIATE_TEST_SUITE_P(
    MoveToFbString,
    MoveToFbStringTest,
    ::testing::Combine(
        ::testing::Values(0, 1, 24, 256, 1 << 10, 1 << 20), // element size
        ::testing::Values(1, 2, 10), // element count
        ::testing::Bool(), // shared
        ::testing::Values(
            CREATE, TAKE_OWNERSHIP_MALLOC, TAKE_OWNERSHIP_CUSTOM, USER_OWNED)));

TEST(IOBuf, getIov) {
  uint32_t fillSeed = 0xdeadbeef;
  std::mt19937 gen(fillSeed);

  size_t len = 4096;
  size_t count = 32;
  auto buf = IOBuf::create(len + 1);
  buf->append(rand() % len + 1);
  fillBuf(buf.get(), gen);

  for (size_t i = 0; i < count - 1; i++) {
    auto buf2 = IOBuf::create(len + 1);
    buf2->append(rand() % len + 1);
    fillBuf(buf2.get(), gen);
    buf->prependChain(std::move(buf2));
  }
  EXPECT_EQ(count, buf->countChainElements());

  auto iov = buf->getIov();
  EXPECT_EQ(count, iov.size());

  IOBuf const* p = buf.get();
  for (size_t i = 0; i < count; i++, p = p->next()) {
    EXPECT_EQ(p->data(), iov[i].iov_base);
    EXPECT_EQ(p->length(), iov[i].iov_len);
  }

  // an empty buf should be skipped in the iov.
  buf->next()->clear();
  iov = buf->getIov();
  EXPECT_EQ(count - 1, iov.size());
  EXPECT_EQ(buf->next()->next()->data(), iov[1].iov_base);

  // same for the first one being empty
  buf->clear();
  iov = buf->getIov();
  EXPECT_EQ(count - 2, iov.size());
  EXPECT_EQ(buf->next()->next()->data(), iov[0].iov_base);

  // and the last one
  buf->prev()->clear();
  iov = buf->getIov();
  EXPECT_EQ(count - 3, iov.size());

  // test appending to an existing iovec array
  iov.clear();
  const char localBuf[] = "hello";
  iov.push_back({(void*)localBuf, sizeof(localBuf)});
  iov.push_back({(void*)localBuf, sizeof(localBuf)});
  buf->appendToIov(&iov);
  EXPECT_EQ(count - 1, iov.size());
  EXPECT_EQ(localBuf, iov[0].iov_base);
  EXPECT_EQ(localBuf, iov[1].iov_base);
  // The first two IOBufs were cleared, so the next iov entry
  // should be the third IOBuf in the chain.
  EXPECT_EQ(buf->next()->next()->data(), iov[2].iov_base);
}

TEST(IOBuf, wrapIov) {
  // Test wrapping IOVs
  constexpr folly::StringPiece hello = "hello";
  constexpr folly::StringPiece world = "world!";
  folly::fbvector<struct iovec> iov;
  iov.push_back({nullptr, 0});
  iov.push_back({(void*)hello.data(), hello.size()});
  iov.push_back({(void*)world.data(), world.size()});
  auto wrapped = IOBuf::wrapIov(iov.data(), iov.size());
  EXPECT_EQ(iov.size() - 1, wrapped->countChainElements());
  IOBuf const* w = wrapped.get();
  // skip the first iovec, which is empty/null, as it is ignored by
  // IOBuf::wrapIov
  for (size_t i = 0; i < wrapped->countChainElements(); ++i, w = w->next()) {
    EXPECT_EQ(w->data(), iov[i + 1].iov_base);
    EXPECT_EQ(w->length(), iov[i + 1].iov_len);
  }
}

TEST(IOBuf, takeOwnershipIov) {
  // Test taking IOVs ownership
  folly::fbvector<folly::StringPiece> words{"hello", "world!"};
  folly::fbvector<struct iovec> iov;
  iov.push_back({nullptr, 0});
  for (size_t i = 0; i < words.size(); i++) {
    iov.push_back({(void*)strdup(words[i].data()), words[i].size() + 1});
  }
  auto buf = IOBuf::takeOwnershipIov(iov.data(), iov.size());
  EXPECT_EQ(iov.size() - 1, buf->countChainElements());

  IOBuf const* b = buf.get();
  // skip the first iovec, which is empty/null, as it is ignored by
  // IOBuf::takeIovOwnership
  for (size_t i = 0; i < buf->countChainElements(); ++i, b = b->next()) {
    EXPECT_EQ(words[i], static_cast<const char*>(iov[i + 1].iov_base));
  }
}

TEST(IOBuf, wrapZeroLenIov) {
  folly::fbvector<struct iovec> iov;
  iov.push_back({nullptr, 0});
  iov.push_back({nullptr, 0});
  auto wrapped = IOBuf::wrapIov(iov.data(), iov.size());
  EXPECT_NE(nullptr, wrapped);
  EXPECT_EQ(wrapped->countChainElements(), 1);
  EXPECT_EQ(wrapped->length(), 0);

  wrapped = IOBuf::wrapIov(nullptr, 0);
  EXPECT_NE(nullptr, wrapped);
  EXPECT_EQ(wrapped->countChainElements(), 1);
  EXPECT_EQ(wrapped->length(), 0);
}

TEST(IOBuf, move) {
  // Default allocate an IOBuf on the stack
  IOBuf outerBuf;
  char data[] = "foobar";
  uint32_t length = sizeof(data);
  uint32_t actualCapacity{0};
  const void* ptr{nullptr};

  {
    // Create a small IOBuf on the stack.
    // Note that IOBufs created on the stack always use an external buffer.
    IOBuf b1(IOBuf::CREATE, 10);
    actualCapacity = b1.capacity();
    EXPECT_GE(actualCapacity, 10);
    EXPECT_EQ(0, b1.length());
    EXPECT_FALSE(b1.isShared());
    ptr = b1.data();
    ASSERT_TRUE(ptr != nullptr);
    memcpy(b1.writableTail(), data, length);
    b1.append(length);
    EXPECT_EQ(length, b1.length());

    // Use the move constructor
    IOBuf b2(std::move(b1));
    EXPECT_EQ(ptr, b2.data());
    EXPECT_EQ(length, b2.length());
    EXPECT_EQ(actualCapacity, b2.capacity());
    EXPECT_FALSE(b2.isShared());

    // Use the move assignment operator
    outerBuf = std::move(b2);
    // Close scope, destroying b1 and b2
    // (which are both be invalid now anyway after moving out of them)
  }

  EXPECT_EQ(ptr, outerBuf.data());
  EXPECT_EQ(length, outerBuf.length());
  EXPECT_EQ(actualCapacity, outerBuf.capacity());
  EXPECT_FALSE(outerBuf.isShared());
}

namespace {
std::unique_ptr<IOBuf> fromStr(StringPiece sp) {
  return IOBuf::copyBuffer(ByteRange(sp));
}

std::unique_ptr<IOBuf> seq(std::initializer_list<StringPiece> sps) {
  auto ret = IOBuf::create(0);
  for (auto sp : sps) {
    ret->prependChain(IOBuf::copyBuffer(ByteRange(sp)));
  }
  return ret;
}
} // namespace

TEST(IOBuf, HashAndEqual) {
  folly::IOBufEqualTo eq;
  folly::IOBufHash hash;

  EXPECT_TRUE(eq(nullptr, nullptr));
  EXPECT_EQ(0, hash(nullptr));

  auto empty = IOBuf::create(0);

  EXPECT_TRUE(eq(*empty, *empty));
  EXPECT_TRUE(eq(empty, empty));
  EXPECT_TRUE(eq(empty.get(), empty.get()));

  EXPECT_FALSE(eq(nullptr, empty));
  EXPECT_FALSE(eq(empty, nullptr));
  EXPECT_FALSE(eq(empty.get(), nullptr));

  EXPECT_EQ(hash(*empty), hash(empty));
  EXPECT_EQ(hash(*empty), hash(empty.get()));
  EXPECT_NE(0, hash(empty));
  EXPECT_NE(0, hash(empty.get()));

  auto a = fromStr("hello");

  EXPECT_TRUE(eq(*a, *a));
  EXPECT_TRUE(eq(a, a));
  EXPECT_TRUE(eq(a.get(), a.get()));

  EXPECT_FALSE(eq(nullptr, a));
  EXPECT_FALSE(eq(a, nullptr));
  EXPECT_FALSE(eq(a.get(), nullptr));

  EXPECT_EQ(hash(*a), hash(a));
  EXPECT_EQ(hash(*a), hash(a.get()));
  EXPECT_NE(0, hash(a));
  EXPECT_NE(0, hash(a.get()));

  auto b = fromStr("hello");

  EXPECT_TRUE(eq(*a, *b));
  EXPECT_TRUE(eq(a, b));
  EXPECT_TRUE(eq(a.get(), b.get()));

  EXPECT_EQ(hash(a), hash(b));
  EXPECT_EQ(hash(a.get()), hash(b.get()));

  auto c = fromStr("hellow");

  EXPECT_FALSE(eq(a, c));
  EXPECT_FALSE(eq(a.get(), c.get()));
  EXPECT_NE(hash(a), hash(c));
  EXPECT_NE(hash(a.get()), hash(c.get()));

  auto d = fromStr("world");

  EXPECT_FALSE(eq(a, d));
  EXPECT_FALSE(eq(a.get(), d.get()));
  EXPECT_NE(hash(a), hash(d));
  EXPECT_NE(hash(a.get()), hash(d.get()));

  auto e = fromStr("helloworld");
  auto f = fromStr("hello");
  f->prependChain(fromStr("wo"));
  f->prependChain(fromStr("rld"));

  EXPECT_TRUE(eq(e, f));
  EXPECT_TRUE(eq(e.get(), f.get()));
  EXPECT_EQ(hash(e), hash(f));
  EXPECT_EQ(hash(e.get()), hash(f.get()));
}

TEST(IOBuf, IOBufCompare) {
  folly::IOBufCompare op;
  auto n = std::unique_ptr<IOBuf>{};
  auto e = IOBuf::create(0);
  auto hello1 = seq({"hello"});
  auto hello2 = seq({"hel", "lo"});
  auto hello3 = seq({"he", "ll", "o"});
  auto hellow = seq({"hellow"});
  auto hellox = seq({"hellox"});

  EXPECT_EQ(ordering::eq, op(n, n));
  EXPECT_EQ(ordering::lt, op(n, e));
  EXPECT_EQ(ordering::gt, op(e, n));
  EXPECT_EQ(ordering::lt, op(e, hello1));
  EXPECT_EQ(ordering::gt, op(hello1, e));
  EXPECT_EQ(ordering::eq, op(hello1, hello1));
  EXPECT_EQ(ordering::eq, op(hello1, hello2));
  EXPECT_EQ(ordering::eq, op(hello1, hello3));
  EXPECT_EQ(ordering::lt, op(hello1, hellow));
  EXPECT_EQ(ordering::gt, op(hellow, hello1));
  EXPECT_EQ(ordering::lt, op(hellow, hellox));
  EXPECT_EQ(ordering::gt, op(hellox, hellow));
}

// reserveSlow() had a bug when reallocating the buffer in place. It would
// preserve old headroom if it's not too much (heuristically) but wouldn't
// adjust the requested amount of memory to account for that; the end result
// would be that reserve() would return with less tailroom than requested.
TEST(IOBuf, ReserveWithHeadroom) {
  // This is assuming jemalloc, where we know that 4096 and 8192 bytes are
  // valid (and consecutive) allocation sizes. We're hoping that our
  // 4096-byte buffer can be expanded in place to 8192 (in practice, this
  // usually happens).
  const char data[] = "Lorem ipsum dolor sit amet, consectetur adipiscing elit";
  constexpr size_t reservedSize = 24; // sizeof(SharedInfo)
  // chosen carefully so that the buffer is exactly 4096 bytes
  IOBuf buf(IOBuf::CREATE, 4096 - reservedSize);
  buf.advance(10);
  memcpy(buf.writableData(), data, sizeof(data));
  buf.append(sizeof(data));
  EXPECT_EQ(sizeof(data), buf.length());

  // Grow the buffer (hopefully in place); this would incorrectly reserve
  // the 10 bytes of headroom, giving us 10 bytes less than requested.
  size_t tailroom = 8192 - reservedSize - sizeof(data);
  buf.reserve(0, tailroom);
  EXPECT_LE(tailroom, buf.tailroom());
  EXPECT_EQ(sizeof(data), buf.length());
  EXPECT_EQ(0, memcmp(data, buf.data(), sizeof(data)));
}

TEST(IOBuf, CopyConstructorAndAssignmentOperator) {
  auto buf = IOBuf::create(4096);
  append(buf, "hello world");
  auto buf2 = IOBuf::create(4096);
  append(buf2, " goodbye");
  buf->prependChain(std::move(buf2));
  EXPECT_FALSE(buf->isShared());

  {
    auto copy = *buf;
    EXPECT_TRUE(buf->isShared());
    EXPECT_TRUE(copy.isShared());
    EXPECT_EQ((void*)buf->data(), (void*)copy.data());
    EXPECT_NE(buf->next(), copy.next()); // actually different buffers

    auto copy2 = *buf;
    copy2.coalesce();
    EXPECT_TRUE(buf->isShared());
    EXPECT_TRUE(copy.isShared());
    EXPECT_FALSE(copy2.isShared());

    auto p = reinterpret_cast<const char*>(copy2.data());
    EXPECT_EQ("hello world goodbye", std::string(p, copy2.length()));
  }

  EXPECT_FALSE(buf->isShared());

  {
    folly::IOBuf newBuf(folly::IOBuf::CREATE, 4096);
    EXPECT_FALSE(newBuf.isShared());

    auto newBufCopy = newBuf;
    EXPECT_TRUE(newBuf.isShared());
    EXPECT_TRUE(newBufCopy.isShared());

    newBufCopy = *buf;
    EXPECT_TRUE(buf->isShared());
    EXPECT_FALSE(newBuf.isShared());
    EXPECT_TRUE(newBufCopy.isShared());
  }

  EXPECT_FALSE(buf->isShared());
}

TEST(IOBuf, CloneAsValue) {
  auto buf = IOBuf::create(4096);
  append(buf, "hello world");
  {
    auto buf2 = IOBuf::create(4096);
    append(buf2, " goodbye");
    buf->prependChain(std::move(buf2));
    EXPECT_FALSE(buf->isShared());
  }

  {
    auto copy = buf->cloneOneAsValue();
    EXPECT_TRUE(buf->isShared());
    EXPECT_TRUE(copy.isShared());
    EXPECT_EQ((void*)buf->data(), (void*)copy.data());
    EXPECT_TRUE(buf->isChained());
    EXPECT_FALSE(copy.isChained());

    auto copy2 = buf->cloneAsValue();
    EXPECT_TRUE(buf->isShared());
    EXPECT_TRUE(copy.isShared());
    EXPECT_TRUE(copy2.isShared());
    EXPECT_TRUE(buf->isChained());
    EXPECT_TRUE(copy2.isChained());
    EXPECT_EQ(buf->toString(), copy2.toString());

    copy.unshareOne();
    EXPECT_TRUE(buf->isShared());
    EXPECT_FALSE(copy.isShared());
    EXPECT_NE((void*)buf->data(), (void*)copy.data());
    EXPECT_TRUE(copy2.isShared());

    auto p = reinterpret_cast<const char*>(copy.data());
    EXPECT_EQ("hello world", std::string(p, copy.length()));

    copy2.coalesce();
    EXPECT_FALSE(buf->isShared());
    EXPECT_FALSE(copy.isShared());
    EXPECT_FALSE(copy2.isShared());
    EXPECT_FALSE(copy2.isChained());

    auto p2 = reinterpret_cast<const char*>(copy2.data());
    EXPECT_EQ("hello world goodbye", std::string(p2, copy2.length()));
  }

  {
    // Test clones from value IOBufs.
    auto copy = buf->cloneAsValue();
    auto copy2 = copy.cloneAsValue();
    EXPECT_TRUE(copy2.isShared());
    EXPECT_TRUE(copy2.isChained());
    EXPECT_EQ(buf->toString(), copy2.toString());

    auto copy3 = copy.clone();
    EXPECT_TRUE(copy3->isShared());
    EXPECT_TRUE(copy3->isChained());
    EXPECT_EQ(buf->toString(), copy3->toString());
  }

  EXPECT_FALSE(buf->isShared());
}

TEST(IOBuf, MoveAssignmentReadsSourceAfterDestinationCleanup) {
  auto source = IOBuf::copyBuffer("source");
  auto destination = IOBuf::takeOwnership(
      new uint8_t[1],
      1,
      [](void* buffer, void* userData) {
        static_cast<IOBuf*>(userData)->trimStart(1);
        delete[] static_cast<uint8_t*>(buffer);
      },
      source.get());

  *destination = std::move(*source);

  EXPECT_EQ("ource", destination->toString());
  EXPECT_EQ(0, source->length());
  EXPECT_EQ(nullptr, source->data());
}

TEST(IOBuf, CloneOneInto) {
  auto src = IOBuf::create(100);
  src->advance(10);
  append(src, "hello world");

  // empty destination shares the head buffer, preserving layout
  IOBuf dest;
  src->cloneOneInto(dest);
  EXPECT_EQ(src->data(), dest.data());
  EXPECT_EQ(src->length(), dest.length());
  EXPECT_EQ(src->headroom(), dest.headroom());
  EXPECT_EQ(src->capacity(), dest.capacity());
  EXPECT_FALSE(dest.isChained());
  EXPECT_TRUE(src->isShared());
  EXPECT_TRUE(dest.isShared());
  EXPECT_EQ("hello world", dest.toString());

  // matches cloneOneAsValue field by field
  auto expected = src->cloneOneAsValue();
  EXPECT_EQ(expected.data(), dest.data());
  EXPECT_EQ(expected.length(), dest.length());
  EXPECT_EQ(expected.headroom(), dest.headroom());
  EXPECT_EQ(expected.capacity(), dest.capacity());

  // destination with its own buffer and chain is fully released
  auto other = IOBuf::create(100);
  append(other, "goodbye");
  auto otherTail = IOBuf::create(100);
  append(otherTail, " tail");
  other->prependChain(std::move(otherTail));
  ASSERT_TRUE(other->isChained());
  src->cloneOneInto(*other);
  EXPECT_FALSE(other->isChained());
  EXPECT_EQ(src->data(), other->data());
  EXPECT_EQ(src->length(), other->length());
  EXPECT_EQ("hello world", other->toString());

  // only the head of a chained source is cloned
  auto head = IOBuf::create(100);
  append(head, "head");
  auto tail = IOBuf::create(100);
  append(tail, "tail");
  head->prependChain(std::move(tail));
  ASSERT_TRUE(head->isChained());
  IOBuf chained;
  head->cloneOneInto(chained);
  EXPECT_FALSE(chained.isChained());
  EXPECT_EQ(head->data(), chained.data());
  EXPECT_EQ("head", chained.toString());

  // self clone truncates the chain but keeps the head buffer alive
  head->cloneOneInto(*head);
  EXPECT_FALSE(head->isChained());
  EXPECT_EQ("head", head->toString());
  EXPECT_EQ(chained.data(), head->data());
  EXPECT_TRUE(head->isShared());

  // aliased destination sharing the source buffer survives the release
  auto alias = head->cloneOneAsValue();
  ASSERT_EQ(head->data(), alias.data());
  head->cloneOneInto(alias);
  EXPECT_EQ(head->data(), alias.data());
  EXPECT_EQ(head->toString(), alias.toString());

  // cloning from an empty source empties the destination
  IOBuf empty;
  auto victim = IOBuf::create(100);
  append(victim, "victim");
  empty.cloneOneInto(*victim);
  EXPECT_EQ(0, victim->length());
  EXPECT_FALSE(victim->isChained());
  EXPECT_EQ("", victim->toString());
}

namespace {
// Use with string literals only
std::unique_ptr<IOBuf> wrap(const char* str) {
  return IOBuf::wrapBuffer(str, strlen(str));
}

std::unique_ptr<IOBuf> copy(const char* str) {
  // At least 1KiB of tailroom, to ensure an external buffer
  return IOBuf::copyBuffer(str, strlen(str), 0, 1024);
}

char* writableStr(folly::IOBuf& buf) {
  return reinterpret_cast<char*>(buf.writableData());
}

} // namespace

TEST(IOBuf, ExternallyShared) {
  struct Item {
    Item(const char* src, size_t len) : size(len) {
      CHECK_LE(len, sizeof(buffer));
      memcpy(buffer, src, len);
    }
    uint32_t refcount{0};
    uint8_t size;
    char buffer[256];
  };

  auto hello = "hello";
  struct Item it(hello, strlen(hello));

  {
    auto freeFn = [](void* /* unused */, void* userData) {
      auto it2 = static_cast<struct Item*>(userData);
      it2->refcount--;
    };
    it.refcount++;
    auto buf1 = IOBuf::takeOwnership(it.buffer, it.size, freeFn, &it);
    EXPECT_TRUE(buf1->isManagedOne());
    EXPECT_FALSE(buf1->isSharedOne());

    buf1->markExternallyShared();
    EXPECT_TRUE(buf1->isSharedOne());

    {
      auto buf2 = buf1->clone();
      EXPECT_TRUE(buf2->isManagedOne());
      EXPECT_TRUE(buf2->isSharedOne());
      EXPECT_EQ(buf1->data(), buf2->data());
      EXPECT_EQ(it.refcount, 1);
    }
    EXPECT_EQ(it.refcount, 1);
  }
  EXPECT_EQ(it.refcount, 0);
}

TEST(IOBuf, Managed) {
  auto hello = "hello";
  auto buf1UP = wrap(hello);
  auto buf1 = buf1UP.get();
  EXPECT_FALSE(buf1->isManagedOne());
  auto buf2UP = copy("world");
  auto buf2 = buf2UP.get();
  EXPECT_TRUE(buf2->isManagedOne());
  auto buf3UP = wrap(hello);
  auto buf3 = buf3UP.get();
  auto buf4UP = buf2->clone();
  auto buf4 = buf4UP.get();

  // buf1 and buf3 share the same memory (but are unmanaged)
  EXPECT_FALSE(buf1->isManagedOne());
  EXPECT_FALSE(buf3->isManagedOne());
  EXPECT_TRUE(buf1->isSharedOne());
  EXPECT_TRUE(buf3->isSharedOne());
  EXPECT_EQ(buf1->data(), buf3->data());

  // buf2 and buf4 share the same memory (but are managed)
  EXPECT_TRUE(buf2->isManagedOne());
  EXPECT_TRUE(buf4->isManagedOne());
  EXPECT_TRUE(buf2->isSharedOne());
  EXPECT_TRUE(buf4->isSharedOne());
  EXPECT_EQ(buf2->data(), buf4->data());

  buf1->prependChain(std::move(buf2UP));
  buf1->prependChain(std::move(buf3UP));
  buf1->prependChain(std::move(buf4UP));

  EXPECT_EQ("helloworldhelloworld", buf1->to<std::string>());
  EXPECT_FALSE(buf1->isManaged());

  buf1->makeManaged();
  EXPECT_TRUE(buf1->isManaged());

  // buf1 and buf3 are now unshared (because they were unmanaged)
  EXPECT_TRUE(buf1->isManagedOne());
  EXPECT_TRUE(buf3->isManagedOne());
  EXPECT_FALSE(buf1->isSharedOne());
  EXPECT_FALSE(buf3->isSharedOne());
  EXPECT_NE(buf1->data(), buf3->data());

  // buf2 and buf4 are still shared
  EXPECT_TRUE(buf2->isManagedOne());
  EXPECT_TRUE(buf4->isManagedOne());
  EXPECT_TRUE(buf2->isSharedOne());
  EXPECT_TRUE(buf4->isSharedOne());
  EXPECT_EQ(buf2->data(), buf4->data());

  // And verify that the truth is what we expect: modify a byte in buf1 and
  // buf2, see that the change from buf1 is *not* reflected in buf3, but the
  // change from buf2 is reflected in buf4.
  writableStr(*buf1)[0] = 'j';
  writableStr(*buf2)[0] = 'x';
  EXPECT_EQ("jelloxorldhelloxorld", buf1->to<std::string>());
}

TEST(IOBuf, CoalesceEmptyBuffers) {
  auto b1 = IOBuf::takeOwnership(nullptr, 0);
  auto b2 = fromStr("hello");
  auto b3 = IOBuf::takeOwnership(nullptr, 0);

  b2->insertAfterThisOne(std::move(b3));
  b1->insertAfterThisOne(std::move(b2));

  auto br = b1->coalesce();

  EXPECT_TRUE(ByteRange(StringPiece("hello")) == br);
}

TEST(IOBuf, CloneCoalescedChain) {
  auto b = IOBuf::createChain(1000, 100);
  b->advance(10);
  const uint32_t fillSeed = 0x12345678;
  std::mt19937 gen(fillSeed);
  {
    auto c = b.get();
    std::size_t length = c->tailroom();
    do {
      length = std::min(length, c->tailroom());
      c->append(length--);
      fillBuf(c, gen);
      c = c->next();
    } while (c != b.get());
  }
  auto c = b->cloneCoalescedAsValue();
  EXPECT_FALSE(c.isChained()); // Not chained
  EXPECT_FALSE(c.isSharedOne()); // Not shared
  EXPECT_EQ(b->headroom(), c.headroom()); // Preserves headroom
  EXPECT_LE(b->prev()->tailroom(), c.tailroom()); // Preserves minimum tailroom
  EXPECT_EQ(b->computeChainDataLength(), c.length()); // Same length
  gen.seed(fillSeed);
  checkBuf(&c, gen); // Same contents

  auto newHeadroom = b->headroom() + 10;
  auto newTailroom = b->tailroom();
  c = b->cloneCoalescedAsValueWithHeadroomTailroom(newHeadroom, newTailroom);
  EXPECT_FALSE(c.isChained()); // Not chained
  EXPECT_GE(c.headroom(), newHeadroom);
  EXPECT_GE(c.tailroom(), newTailroom);
  gen.seed(fillSeed);
  checkBuf(&c, gen); // Same contents

  newHeadroom = b->headroom();
  newTailroom = b->tailroom() + 10;
  c = b->cloneCoalescedAsValueWithHeadroomTailroom(newHeadroom, newTailroom);
  EXPECT_FALSE(c.isChained()); // Not chained
  EXPECT_GE(c.headroom(), newHeadroom);
  EXPECT_GE(c.tailroom(), newTailroom);
  gen.seed(fillSeed);
  checkBuf(&c, gen); // Same contents
}

TEST(IOBuf, CloneCoalescedSingle) {
  auto b = IOBuf::create(1000);
  b->advance(10);
  b->append(900);
  const uint32_t fillSeed = 0x12345678;
  std::mt19937 gen(fillSeed);
  fillBuf(b.get(), gen);

  auto c = b->cloneCoalesced();
  EXPECT_FALSE(c->isChained()); // Not chained
  EXPECT_TRUE(c->isSharedOne()); // Shared
  EXPECT_EQ(b->buffer(), c->buffer());
  EXPECT_EQ(b->capacity(), c->capacity());
  EXPECT_EQ(b->data(), c->data());
  EXPECT_EQ(b->length(), c->length());

  auto newHeadroom = b->headroom() + 10;
  auto newTailroom = b->tailroom();
  c = b->cloneCoalescedWithHeadroomTailroom(newHeadroom, newTailroom);
  EXPECT_FALSE(c->isChained()); // Not chained
  EXPECT_EQ(
      ByteRange(c->data(), c->length()), ByteRange(b->data(), b->length()));
  EXPECT_GE(c->headroom(), newHeadroom);
  EXPECT_GE(c->tailroom(), newTailroom);

  newHeadroom = b->headroom();
  newTailroom = b->tailroom() + 10;
  c = b->cloneCoalescedWithHeadroomTailroom(newHeadroom, newTailroom);
  EXPECT_FALSE(c->isChained()); // Not chained
  EXPECT_EQ(
      ByteRange(c->data(), c->length()), ByteRange(b->data(), b->length()));
  EXPECT_GE(c->headroom(), newHeadroom);
  EXPECT_GE(c->tailroom(), newTailroom);
}

TEST(IOBuf, fillIov) {
  auto buf = IOBuf::create(4096);
  append(buf, "hello");
  auto buf2 = IOBuf::create(4096);
  append(buf2, "goodbye");
  auto buf3 = IOBuf::create(4096);
  append(buf3, "hello again");

  buf2->insertAfterThisOne(std::move(buf3));
  buf->insertAfterThisOne(std::move(buf2));

  constexpr size_t iovCount = 3;
  struct iovec vec[iovCount];
  auto res = buf->fillIov(vec, iovCount);

  EXPECT_EQ(iovCount, res.numIovecs);
  EXPECT_EQ(23, res.totalLength);
  EXPECT_EQ(
      "hello",
      std::string(
          reinterpret_cast<const char*>(vec[0].iov_base), vec[0].iov_len));
  EXPECT_EQ(
      "goodbye",
      std::string(
          reinterpret_cast<const char*>(vec[1].iov_base), vec[1].iov_len));
  EXPECT_EQ(
      "hello again",
      std::string(
          reinterpret_cast<const char*>(vec[2].iov_base), vec[2].iov_len));
}

TEST(IOBuf, fillIov2) {
  auto buf = IOBuf::create(4096);
  append(buf, "hello");
  auto buf2 = IOBuf::create(4096);
  append(buf2, "goodbye");
  auto buf3 = IOBuf::create(4096);
  append(buf2, "hello again");

  buf2->insertAfterThisOne(std::move(buf3));
  buf->insertAfterThisOne(std::move(buf2));

  constexpr size_t iovCount = 2;
  struct iovec vec[iovCount];
  auto res = buf->fillIov(vec, iovCount);

  EXPECT_EQ(0, res.numIovecs);
  EXPECT_EQ(0, res.totalLength);
}

TEST(IOBuf, FreeFn) {
  class IOBufFreeObserver {
   public:
    using Func = std::function<void()>;
    explicit IOBufFreeObserver(Func&& freeFunc, Func&& releaseFunc)
        : freeFunc_(std::move(freeFunc)),
          releaseFunc_(std::move(releaseFunc)) {}
    void afterFreeExtBuffer() const noexcept { freeFunc_(); }
    void afterReleaseExtBuffer() const noexcept { releaseFunc_(); }

   private:
    Func freeFunc_;
    Func releaseFunc_;
  };

  int freeVal = 0;
  int releaseVal = 0;
  IOBufFreeObserver observer(
      [&freeVal]() { freeVal += 1; }, [&releaseVal]() { releaseVal += 1; });

  // no observers
  {
    unique_ptr<IOBuf> iobuf(IOBuf::create(64));
  }

  // one observer
  {
    unique_ptr<IOBuf> iobuf(IOBuf::create(64));

    EXPECT_TRUE(iobuf->appendSharedInfoObserver(observer));
  }
  EXPECT_EQ(freeVal, 1);
  EXPECT_EQ(releaseVal, 0);

  freeVal = 0;
  releaseVal = 0;
  // reserve
  {
    unique_ptr<IOBuf> iobuf(IOBuf::create(64));
    EXPECT_TRUE(iobuf->appendSharedInfoObserver(observer));
    iobuf->reserve(0, iobuf->capacity() + 1024);
    EXPECT_EQ(freeVal, 1);
    EXPECT_EQ(releaseVal, 0);
  }

  freeVal = 0;
  releaseVal = 0;
  // one observer - call moveToFbString
  {
    unique_ptr<IOBuf> iobuf(IOBuf::create(64 * 1024));

    EXPECT_TRUE(iobuf->appendSharedInfoObserver(observer));
    auto str = iobuf->moveToFbString();
  }
  EXPECT_EQ(freeVal, 0);
  EXPECT_EQ(releaseVal, 1);

  freeVal = 0;
  releaseVal = 0;
  // multiple observers
  {
    unique_ptr<IOBuf> iobuf(IOBuf::create(64));

    for (size_t i = 0; i < 3; i++) {
      EXPECT_TRUE(iobuf->appendSharedInfoObserver(observer));
    }
  }
  EXPECT_EQ(freeVal, 3);
  EXPECT_EQ(releaseVal, 0);
}

// Compute the chained capacity of a single non-chained IOBuf of capacity zero
TEST(IOBuf, computeChainCapacityOfZeroSizeIOBuf) {
  size_t size = 0;
  uint8_t* data = nullptr;

  // Create buffer of capacity 0
  unique_ptr<IOBuf> buf(IOBuf::wrapBuffer(data, size));

  EXPECT_EQ(buf->computeChainCapacity(), 0);
}

// Compute the chained capacity of a single non-chained IOBuf of capacity
// non-zero
TEST(IOBuf, computeChainCapacityOfNonZeroSizeIOBuf) {
  const size_t size = 20;
  uint8_t data[size] = {};

  // Create buffer of capacity 20
  unique_ptr<IOBuf> buf(IOBuf::wrapBuffer(data, size));

  EXPECT_EQ(buf->computeChainCapacity(), 20);
}

// Compute the chained capacity of a chained IOBuf with chains having a variety
// of zero and non-zero capacities.
TEST(IOBuf, computeChainCapacityOfMixedCapacityChainedIOBuf) {
  // Total capacity is 100
  uint8_t data1[20] = {};
  uint8_t data2[0] = {};
  uint8_t data3[60] = {};
  uint8_t data4[15] = {};
  uint8_t data5[0] = {};
  uint8_t data6[5] = {};

  // Create IOBuf at head of chain
  unique_ptr<IOBuf> buf(IOBuf::wrapBuffer(data1, sizeof(data1)));

  // Create IOBuf chains
  auto temp = buf.get();
  temp->insertAfterThisOne(IOBuf::wrapBuffer(data2, sizeof(data2)));
  temp = temp->next();
  temp->insertAfterThisOne(IOBuf::wrapBuffer(data3, sizeof(data3)));
  temp = temp->next();
  temp->insertAfterThisOne(IOBuf::wrapBuffer(data4, sizeof(data4)));
  temp = temp->next();
  temp->insertAfterThisOne(IOBuf::wrapBuffer(data5, sizeof(data5)));
  temp = temp->next();
  temp->insertAfterThisOne(IOBuf::wrapBuffer(data6, sizeof(data6)));

  EXPECT_EQ(buf->computeChainCapacity(), 100);
}

TEST(IOBuf, AppendTo) {
  using folly::range;

  IOBuf buf;
  EXPECT_EQ(buf.to<std::string>(), "");
  EXPECT_EQ(buf.toString(), "");

  auto temp = &buf;
  temp->insertAfterThisOne(IOBuf::copyBuffer("Hello"));
  temp = temp->next();
  temp->insertAfterThisOne(IOBuf::copyBuffer(" and"));
  temp = temp->next();
  temp->insertAfterThisOne(IOBuf::copyBuffer(" goodbye."));

  auto testAppendTo = [&](auto c) {
    const StringPiece kExpected = "Hello and goodbye.";
    EXPECT_EQ(StringPiece(range(buf.to<decltype(c)>())), kExpected);

    const StringPiece kPreviousData = "I was there before. ";
    c.insert(c.end(), kPreviousData.begin(), kPreviousData.end());
    buf.appendTo(c);
    EXPECT_EQ(StringPiece(range(c)), kPreviousData.str() + kExpected.str());
  };

  testAppendTo(std::string{});
  testAppendTo(fbstring{});
  testAppendTo(std::vector<char>{});
  testAppendTo(std::vector<unsigned char>{});
}

TEST(IOBuf, AppendToBinaryData) {
  using folly::range;

  const unsigned char kData[] = {0x00, 0x7f, 0x80, 0xff, 'x'};
  auto buf = IOBuf::wrapBuffer(kData, 2);
  buf->appendToChain(IOBuf::wrapBuffer(kData + 2, sizeof(kData) - 2));

  const StringPiece kExpected{ByteRange{kData, sizeof(kData)}};
  EXPECT_EQ(StringPiece(range(buf->to<std::string>())), kExpected);
  EXPECT_EQ(StringPiece(range(buf->to<fbstring>())), kExpected);
  EXPECT_EQ(StringPiece(range(buf->to<std::vector<char>>())), kExpected);
  EXPECT_EQ(
      StringPiece(range(buf->to<std::vector<unsigned char>>())), kExpected);
}

TEST(IOBuf, FromStringView) {
  auto literalHelloBuffer = IOBuf::copyBuffer("Hello");
  std::string hello("Hello");
  auto fromStringHelloBuffer = IOBuf::copyBuffer(hello);
  std::string_view hello2("Hello");
  auto fromStringViewHelloBuffer = IOBuf::copyBuffer(hello2);

  std::string fromLiteral;
  literalHelloBuffer->appendTo(fromLiteral);
  std::string fromString;
  fromStringHelloBuffer->appendTo(fromString);
  std::string fromStringView;
  fromStringViewHelloBuffer->appendTo(fromStringView);

  EXPECT_EQ(fromStringView, fromString);
  EXPECT_EQ(fromLiteral, fromString);
}

TEST(IOBuf, bufferTooLarge) {
  EXPECT_THROW(
      IOBuf::copyBuffer(StringPiece("Hello"), (size_t)0xFFFF'FFFF'FFFF'FFFE),
      std::length_error);
}

TEST(IOBuf, copyConstructBufferTooLarge) {
  auto buf = StringPiece("Hello");
  EXPECT_THROW(
      IOBuf(
          IOBuf::COPY_BUFFER,
          buf.data(),
          buf.size(),
          57,
          (size_t)0xFFFF'FFFF'FFFF'FFFE),
      std::bad_alloc);
}

TEST(IOBuf, CoalesceCapacityOverflow) {
  constexpr auto kMax = std::numeric_limits<size_t>::max();
  for (const bool overflowHeadroom : {false, true}) {
    auto buf = seq({"abc", "de"});
    const size_t headroom = overflowHeadroom ? kMax : 0;
    const size_t tailroom = overflowHeadroom ? 1 : kMax;
    ASSERT_TRUE(buf->isChained());

    EXPECT_THROW(
        buf->coalesceWithHeadroomTailroom(headroom, tailroom), std::bad_alloc);

    EXPECT_TRUE(buf->isChained());
    EXPECT_EQ("abcde", buf->to<std::string>());
  }
}

TEST(IOBuf, CoalesceHeadroomTailroom) {
  auto buf = seq({"abc", "de"});
  ASSERT_TRUE(buf->isChained());

  buf->coalesceWithHeadroomTailroom(4, 8);

  EXPECT_FALSE(buf->isChained());
  EXPECT_EQ("abcde", buf->to<std::string>());
  EXPECT_EQ(4, buf->headroom());
  EXPECT_GE(buf->tailroom(), 8);
}

TEST(IOBuf, MaybeSplitTail) {
  auto buf = IOBuf::create(256);
  append(buf, "Hello ");

  size_t tailroom1 = buf->tailroom();
  auto buf1 = buf->maybeSplitTail();
  EXPECT_EQ(buf->tailroom(), 0);
  EXPECT_EQ(buf1->tailroom(), tailroom1);
  EXPECT_EQ(
      reinterpret_cast<const void*>(buf->bufferEnd()),
      reinterpret_cast<const void*>(buf1->buffer()));
  EXPECT_TRUE(buf->isSharedOne());
  EXPECT_FALSE(buf1->isSharedOne());
  append(buf1, "world");

  size_t tailroom2 = buf1->tailroom();
  auto buf2 = buf1->maybeSplitTail();
  EXPECT_EQ(buf1->tailroom(), 0);
  EXPECT_EQ(buf2->tailroom(), tailroom2);
  EXPECT_EQ(
      reinterpret_cast<const void*>(buf1->bufferEnd()),
      reinterpret_cast<const void*>(buf2->buffer()));
  // This maybeSplitTail() doesn't make buf1 shared because buf is the original
  // owner, so buf2 refers to it as well.
  EXPECT_FALSE(buf1->isSharedOne());
  EXPECT_FALSE(buf2->isSharedOne());
  append(buf2, "!");

  buf = {};
  EXPECT_EQ(buf1->to<std::string>(), "world");
  buf1 = {};
  EXPECT_EQ(buf2->to<std::string>(), "!");

  // The original buffer is gone, but we can continue splitting its tail.
  auto buf3 = buf2->maybeSplitTail();
  append(buf3, "!!!1");
}

TEST(IOBuf, FromString) {
  EXPECT_EQ(folly::IOBuf::fromString("")->toString(), "");
  EXPECT_EQ(folly::IOBuf::fromString("test")->toString(), "test");
  auto longStr = std::string(1000, '0');
  EXPECT_EQ(folly::IOBuf::fromString(longStr)->toString(), longStr);
}

#if FOLLY_HAS_MEMORY_RESOURCE

TEST(IOBuf, WithMemoryResource) {
  struct FakeMemoryResource : std::pmr::memory_resource {
    void* do_allocate(
        std::size_t bytes, std::size_t /*  alignment */) override {
      auto p = malloc(bytes);
      CHECK(active.try_emplace(p, bytes).second);
      return p;
    }

    void do_deallocate(
        void* p, std::size_t bytes, std::size_t /* alignment */) override {
      auto it = active.find(p);
      CHECK(it != active.end()) << active.size();
      CHECK_EQ(it->second, bytes);
      active.erase(it);
      free(p);
    }

    bool do_is_equal(
        const memory_resource& /* other */) const noexcept override {
      LOG(FATAL) << "Not implemented";
    }

    std::unordered_map<void*, size_t> active;
  };

  FakeMemoryResource mr;
  const auto makeBuf = [&](size_t size) {
    uint8_t* data = static_cast<uint8_t*>(malloc(size));
    return IOBuf::takeOwnership(&mr, data, size, 0, 0);
  };
  auto buf = makeBuf(10);
  EXPECT_EQ(mr.active.size(), 1);
  buf->appendToChain(makeBuf(20));
  EXPECT_EQ(mr.active.size(), 2);

  {
    // Cloning without passing a memory_resource uses the system allocator.
    auto clone = buf->clone();
    EXPECT_EQ(mr.active.size(), 2);
    clone.reset();
    EXPECT_EQ(mr.active.size(), 2);
  }

  {
    auto clone = buf->cloneOne(&mr);
    EXPECT_EQ(mr.active.size(), 3);
  }
  EXPECT_EQ(mr.active.size(), 2);

  {
    auto clone = buf->clone(&mr);
    EXPECT_EQ(mr.active.size(), 4);
  }
  EXPECT_EQ(mr.active.size(), 2);

  buf.reset();
  EXPECT_EQ(mr.active.size(), 0);
}

#endif /* FOLLY_HAS_MEMORY_RESOURCE */
