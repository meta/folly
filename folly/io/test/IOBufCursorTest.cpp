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

#include <array>
#include <numeric>
#include <vector>

#include <folly/Range.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <folly/portability/GTest.h>

using folly::ByteRange;
using folly::IOBuf;
using folly::StringPiece;
using std::unique_ptr;
using namespace folly::io;

TEST(IOBuf, RWCursor) {
  unique_ptr<IOBuf> iobuf1(IOBuf::create(20));
  iobuf1->append(20);
  unique_ptr<IOBuf> iobuf2(IOBuf::create(20));
  iobuf2->append(20);

  iobuf2.get();
  iobuf1->prependChain(std::move(iobuf2));

  EXPECT_TRUE(iobuf1->isChained());

  RWPrivateCursor wcursor(iobuf1.get());
  Cursor rcursor(iobuf1.get());
  wcursor.writeLE<uint64_t>((uint64_t)1);
  wcursor.writeLE<uint64_t>((uint64_t)1);
  wcursor.writeLE<uint64_t>((uint64_t)1);
  wcursor.write<uint8_t>((uint8_t)1);

  EXPECT_EQ(1u, rcursor.readLE<uint64_t>());
  rcursor.skip(8);
  EXPECT_EQ(1u, rcursor.readLE<uint32_t>());
  rcursor.skip(0);
  EXPECT_EQ(0u, rcursor.read<uint8_t>());
  EXPECT_EQ(0u, rcursor.read<uint8_t>());
  EXPECT_EQ(0u, rcursor.read<uint8_t>());
  EXPECT_EQ(0u, rcursor.read<uint8_t>());
  EXPECT_EQ(1u, rcursor.read<uint8_t>());
}

TEST(IOBuf, skip) {
  unique_ptr<IOBuf> iobuf1(IOBuf::create(20));
  iobuf1->append(20);
  RWPrivateCursor wcursor(iobuf1.get());
  wcursor.write<uint8_t>((uint8_t)1);
  wcursor.write<uint8_t>((uint8_t)2);
  Cursor cursor(iobuf1.get());
  cursor.skip(1);
  EXPECT_EQ(2, cursor.read<uint8_t>());
}

TEST(IOBuf, reset) {
  unique_ptr<IOBuf> iobuf1(IOBuf::create(20));
  iobuf1->append(20);
  RWPrivateCursor wcursor(iobuf1.get());
  wcursor.write<uint8_t>((uint8_t)1);
  wcursor.write<uint8_t>((uint8_t)2);
  wcursor.reset(iobuf1.get());
  EXPECT_EQ(1, wcursor.read<uint8_t>());
}

TEST(IOBuf, copyAssignConvert) {
  unique_ptr<IOBuf> iobuf1(IOBuf::create(20));
  iobuf1->append(20);
  RWPrivateCursor wcursor(iobuf1.get());
  RWPrivateCursor cursor2(wcursor);
  RWPrivateCursor cursor3(iobuf1.get());

  wcursor.write<uint8_t>((uint8_t)1);
  cursor3 = wcursor;
  wcursor.write<uint8_t>((uint8_t)2);
  Cursor cursor4(wcursor);
  RWPrivateCursor cursor5(wcursor);
  wcursor.write<uint8_t>((uint8_t)3);

  EXPECT_EQ(1, cursor2.read<uint8_t>());
  EXPECT_EQ(2, cursor3.read<uint8_t>());
  EXPECT_EQ(3, cursor4.read<uint8_t>());
  EXPECT_EQ(3, cursor5.read<uint8_t>());
}

TEST(IOBuf, arithmetic) {
  IOBuf iobuf1(IOBuf::CREATE, 20);
  iobuf1.append(20);
  RWPrivateCursor wcursor(&iobuf1);
  wcursor += 1;
  wcursor.write<uint8_t>((uint8_t)1);
  Cursor cursor(&iobuf1);
  cursor += 1;
  EXPECT_EQ(1, cursor.read<uint8_t>());

  Cursor start(&iobuf1);
  Cursor cursor2 = start + 9;
  EXPECT_EQ(7, cursor2 - cursor);
  EXPECT_NE(cursor, cursor2);
  cursor += 8;
  cursor2 = cursor2 + 1;
  EXPECT_EQ(cursor, cursor2);
}

TEST(IOBuf, endian) {
  unique_ptr<IOBuf> iobuf1(IOBuf::create(20));
  iobuf1->append(20);
  RWPrivateCursor wcursor(iobuf1.get());
  Cursor rcursor(iobuf1.get());
  uint16_t v = 1;
  int16_t vu = -1;
  wcursor.writeBE<uint16_t>(v);
  wcursor.writeBE<int16_t>(vu);
  // Try a couple combinations to ensure they were generated correctly
  wcursor.writeBE<int16_t>(vu);
  wcursor.writeLE<int16_t>(vu);
  wcursor.writeLE<int16_t>(vu);
  wcursor.writeLE<uint16_t>(v);
  EXPECT_EQ(v, rcursor.readBE<uint16_t>());
}

TEST(IOBuf, Cursor) {
  unique_ptr<IOBuf> iobuf1(IOBuf::create(1));
  iobuf1->append(1);
  RWPrivateCursor c(iobuf1.get());
  c.write<uint8_t>((uint8_t)40); // OK
  try {
    c.write<uint8_t>((uint8_t)10); // Bad write, checked should except.
    ADD_FAILURE();
  } catch (...) {
  }
}

TEST(IOBuf, UnshareCursor) {
  uint8_t buf = 0;
  unique_ptr<IOBuf> iobuf1(IOBuf::wrapBuffer(&buf, 1));
  unique_ptr<IOBuf> iobuf2(IOBuf::wrapBuffer(&buf, 1));
  RWUnshareCursor c1(iobuf1.get());
  RWUnshareCursor c2(iobuf2.get());

  c1.write<uint8_t>((uint8_t)10); // This should duplicate the two buffers.
  uint8_t t = c2.read<uint8_t>();
  EXPECT_EQ(0, t);

  iobuf1 = IOBuf::wrapBuffer(&buf, 1);
  iobuf2 = IOBuf::wrapBuffer(&buf, 1);
  RWPrivateCursor c3(iobuf1.get());
  RWPrivateCursor c4(iobuf2.get());

  c3.write<uint8_t>(
      (uint8_t)10); // This should _not_ duplicate the two buffers.
  t = c4.read<uint8_t>();
  EXPECT_EQ(10, t);
}

namespace {
void append(std::unique_ptr<IOBuf>& buf, folly::StringPiece data) {
  EXPECT_LE(data.size(), buf->tailroom());
  memcpy(buf->writableData(), data.data(), data.size());
  buf->append(data.size());
}

void append(Appender& appender, StringPiece data) {
  appender.push(ByteRange(data));
}

std::string toString(const IOBuf& buf) {
  std::string str;
  Cursor cursor(&buf);
  ByteRange b;
  while (!(b = cursor.peekBytes()).empty()) {
    str.append(reinterpret_cast<const char*>(b.data()), b.size());
    cursor.skip(b.size());
  }
  return str;
}

} // namespace

TEST(IOBuf, PullAndPeek) {
  std::unique_ptr<IOBuf> iobuf1(IOBuf::create(10));
  append(iobuf1, "he");
  std::unique_ptr<IOBuf> iobuf2(IOBuf::create(10));
  append(iobuf2, "llo ");
  std::unique_ptr<IOBuf> iobuf3(IOBuf::create(10));
  append(iobuf3, "world");
  iobuf1->prependChain(std::move(iobuf2));
  iobuf1->prependChain(std::move(iobuf3));
  EXPECT_EQ(3, iobuf1->countChainElements());
  EXPECT_EQ(11, iobuf1->computeChainDataLength());

  char buf[20];
  memset(buf, 0, sizeof(buf));
  Cursor(iobuf1.get()).pull(buf, 11);
  EXPECT_EQ("hello world", std::string(buf));

  memset(buf, 0, sizeof(buf));
  EXPECT_EQ(11, Cursor(iobuf1.get()).pullAtMost(buf, 20));
  EXPECT_EQ("hello world", std::string(buf));

  EXPECT_THROW({ Cursor(iobuf1.get()).pull(buf, 20); }, std::out_of_range);

  {
    RWPrivateCursor cursor(iobuf1.get());
    auto b = cursor.peekBytes();
    EXPECT_EQ(b.data(), cursor.peek().data());
    EXPECT_EQ(b.size(), cursor.peek().size());
    EXPECT_EQ("he", StringPiece(b));
    cursor.skip(b.size());
    b = cursor.peekBytes();
    EXPECT_EQ(b.data(), cursor.peek().data());
    EXPECT_EQ(b.size(), cursor.peek().size());
    EXPECT_EQ("llo ", StringPiece(b));
    cursor.skip(b.size());
    b = cursor.peekBytes();
    EXPECT_EQ(b.data(), cursor.peek().data());
    EXPECT_EQ(b.size(), cursor.peek().size());
    EXPECT_EQ("world", StringPiece(b));
    cursor.skip(b.size());
    EXPECT_EQ(3, iobuf1->countChainElements());
    EXPECT_EQ(11, iobuf1->computeChainDataLength());
  }

  {
    RWPrivateCursor cursor(iobuf1.get());
    cursor.gather(11);
    auto b = cursor.peekBytes();
    EXPECT_EQ(b.data(), cursor.peek().data());
    EXPECT_EQ(b.size(), cursor.peek().size());
    EXPECT_EQ("hello world", StringPiece(b));
    EXPECT_EQ(1, iobuf1->countChainElements());
    EXPECT_EQ(11, iobuf1->computeChainDataLength());
  }
}

TEST(IOBuf, pullFromChainContainingEmptyBufs) {
  std::unique_ptr<IOBuf> iobuf1(IOBuf::create(10));
  append(iobuf1, "he");
  std::unique_ptr<IOBuf> iobuf2(IOBuf::create(10));
  append(iobuf2, "llo ");
  std::unique_ptr<IOBuf> iobuf3(IOBuf::create(10));
  append(iobuf3, "world");
  iobuf1->appendToChain(std::move(iobuf2));
  // Add a default-constructed IOBuf which has 0 length and a null data pointer
  // This doesn't affect the logical contents of the chain, it's here to
  // make sure the pull() logic doesn't try to call memcpy() with a null
  // pointer argument.
  iobuf1->appendToChain(std::make_unique<folly::IOBuf>());
  iobuf1->appendToChain(std::move(iobuf3));

  std::string output;
  output.resize(iobuf1->computeChainDataLength());
  auto cursor = Cursor(iobuf1.get());
  cursor.pull(&output[0], iobuf1->computeChainDataLength());
  EXPECT_EQ("hello world", output);
}

TEST(IOBuf, pushCursorData) {
  unique_ptr<IOBuf> iobuf1(IOBuf::create(20));
  iobuf1->append(15);
  iobuf1->trimStart(5);
  unique_ptr<IOBuf> iobuf2(IOBuf::create(10));
  unique_ptr<IOBuf> iobuf3(IOBuf::create(10));
  iobuf3->append(10);

  iobuf1->prependChain(std::move(iobuf2));
  iobuf1->prependChain(std::move(iobuf3));
  EXPECT_TRUE(iobuf1->isChained());

  // write 20 bytes to the buffer chain
  RWPrivateCursor wcursor(iobuf1.get());
  EXPECT_FALSE(wcursor.isAtEnd());
  wcursor.writeBE<uint64_t>(uint64_t(1));
  wcursor.writeBE<uint64_t>(uint64_t(10));
  wcursor.writeBE<uint32_t>(uint32_t(20));
  EXPECT_TRUE(wcursor.isAtEnd());

  // create a read buffer for the buffer chain
  Cursor rcursor(iobuf1.get());
  EXPECT_EQ(1, rcursor.readBE<uint64_t>());
  EXPECT_EQ(10, rcursor.readBE<uint64_t>());
  EXPECT_EQ(20, rcursor.readBE<uint32_t>());
  EXPECT_EQ(0, rcursor.totalLength());
  rcursor.reset(iobuf1.get());
  EXPECT_EQ(20, rcursor.totalLength());

  // create another write buffer
  unique_ptr<IOBuf> iobuf4(IOBuf::create(30));
  iobuf4->append(30);
  RWPrivateCursor wcursor2(iobuf4.get());
  // write buffer chain data into it, now wcursor2 should only
  // have 10 bytes writable space
  wcursor2.push(rcursor, 20);
  EXPECT_EQ(wcursor2.totalLength(), 10);
  // write again with not enough space in rcursor
  EXPECT_THROW(wcursor2.push(rcursor, 20), std::out_of_range);

  // create a read cursor to check iobuf3 data back
  Cursor rcursor2(iobuf4.get());
  EXPECT_EQ(1, rcursor2.readBE<uint64_t>());
  EXPECT_EQ(10, rcursor2.readBE<uint64_t>());
  EXPECT_EQ(20, rcursor2.readBE<uint32_t>());
}

TEST(IOBuf, Gather) {
  std::unique_ptr<IOBuf> iobuf1(IOBuf::create(10));
  append(iobuf1, "he");
  std::unique_ptr<IOBuf> iobuf2(IOBuf::create(10));
  append(iobuf2, "llo ");
  std::unique_ptr<IOBuf> iobuf3(IOBuf::create(10));
  append(iobuf3, "world");
  iobuf1->prependChain(std::move(iobuf2));
  iobuf1->prependChain(std::move(iobuf3));
  EXPECT_EQ(3, iobuf1->countChainElements());
  EXPECT_EQ(11, iobuf1->computeChainDataLength());

  // Attempting to gather() more data than available in the chain should fail.
  // Try from the very beginning of the chain.
  RWPrivateCursor cursor(iobuf1.get());
  EXPECT_THROW(cursor.gather(15), std::overflow_error);
  // Now try from the middle of the chain
  cursor += 3;
  EXPECT_THROW(cursor.gather(10), std::overflow_error);

  // Calling gatherAtMost() should succeed, however, and just gather
  // as much as it can
  cursor.gatherAtMost(10);
  EXPECT_EQ(8, cursor.length());
  EXPECT_EQ(8, cursor.totalLength());
  EXPECT_FALSE(cursor.isAtEnd());
  EXPECT_EQ(
      "lo world",
      folly::StringPiece(
          reinterpret_cast<const char*>(cursor.data()), cursor.length()));
  EXPECT_EQ(2, iobuf1->countChainElements());
  EXPECT_EQ(11, iobuf1->computeChainDataLength());

  // Now try gather again on the chain head
  cursor = RWPrivateCursor(iobuf1.get());
  cursor.gather(5);
  // Since gather() doesn't split buffers, everything should be collapsed into
  // a single buffer now.
  EXPECT_EQ(1, iobuf1->countChainElements());
  EXPECT_EQ(11, iobuf1->computeChainDataLength());
  EXPECT_EQ(11, cursor.length());
  EXPECT_EQ(11, cursor.totalLength());
}

TEST(IOBuf, cloneAndInsert) {
  std::unique_ptr<IOBuf> iobuf1(IOBuf::create(10));
  append(iobuf1, "he");
  std::unique_ptr<IOBuf> iobuf2(IOBuf::create(10));
  append(iobuf2, "llo ");
  std::unique_ptr<IOBuf> iobuf3(IOBuf::create(10));
  append(iobuf3, "world");
  iobuf1->prependChain(std::move(iobuf2));
  iobuf1->prependChain(std::move(iobuf3));
  EXPECT_EQ(3, iobuf1->countChainElements());
  EXPECT_EQ(11, iobuf1->computeChainDataLength());

  std::unique_ptr<IOBuf> cloned;

  Cursor(iobuf1.get()).clone(cloned, 3);
  EXPECT_EQ(2, cloned->countChainElements());
  EXPECT_EQ(3, cloned->computeChainDataLength());

  EXPECT_EQ(11, Cursor(iobuf1.get()).cloneAtMost(cloned, 20));
  EXPECT_EQ(3, cloned->countChainElements());
  EXPECT_EQ(11, cloned->computeChainDataLength());

  EXPECT_THROW({ Cursor(iobuf1.get()).clone(cloned, 20); }, std::out_of_range);

  {
    // Check that inserting in the middle of an iobuf splits
    RWPrivateCursor cursor(iobuf1.get());
    Cursor(iobuf1.get()).clone(cloned, 3);
    EXPECT_EQ(2, cloned->countChainElements());
    EXPECT_EQ(3, cloned->computeChainDataLength());

    cursor.skip(1);

    cursor.insert(std::move(cloned));
    cursor.insert(folly::IOBuf::create(0));
    EXPECT_EQ(4, cursor.getCurrentPosition());
    EXPECT_EQ(7, iobuf1->countChainElements());
    EXPECT_EQ(14, iobuf1->computeChainDataLength());
    // Check that nextBuf got set correctly to the buffer with 1 byte left
    EXPECT_EQ(1, cursor.peekBytes().size());
    cursor.read<uint8_t>();
  }

  {
    // Check that inserting at the end doesn't create empty buf
    RWPrivateCursor cursor(iobuf1.get());
    Cursor(iobuf1.get()).clone(cloned, 1);
    EXPECT_EQ(1, cloned->countChainElements());
    EXPECT_EQ(1, cloned->computeChainDataLength());

    cursor.skip(1);

    cursor.insert(std::move(cloned));
    EXPECT_EQ(2, cursor.getCurrentPosition());
    EXPECT_EQ(8, iobuf1->countChainElements());
    EXPECT_EQ(15, iobuf1->computeChainDataLength());
    // Check that nextBuf got set correctly
    cursor.read<uint8_t>();
  }
  {
    // Check that inserting at the beginning of a chunk (except first one)
    // doesn't create empty buf
    RWPrivateCursor cursor(iobuf1.get());
    Cursor(iobuf1.get()).clone(cloned, 1);
    EXPECT_EQ(1, cloned->countChainElements());
    EXPECT_EQ(1, cloned->computeChainDataLength());

    cursor.skip(1);

    cursor.insert(std::move(cloned));
    EXPECT_EQ(2, cursor.getCurrentPosition());
    EXPECT_EQ(14, cursor.totalLength());
    EXPECT_EQ(9, iobuf1->countChainElements());
    EXPECT_EQ(16, iobuf1->computeChainDataLength());
    // Check that nextBuf got set correctly
    cursor.read<uint8_t>();
  }
  {
    // Check that inserting at the beginning of a chain DOES keep an empty
    // buffer.
    RWPrivateCursor cursor(iobuf1.get());
    Cursor(iobuf1.get()).clone(cloned, 1);
    EXPECT_EQ(1, cloned->countChainElements());
    EXPECT_EQ(1, cloned->computeChainDataLength());

    cursor.insert(std::move(cloned));
    EXPECT_EQ(1, cursor.getCurrentPosition());
    EXPECT_EQ(16, cursor.totalLength());
    EXPECT_EQ(11, iobuf1->countChainElements());
    EXPECT_EQ(17, iobuf1->computeChainDataLength());
    // Check that nextBuf got set correctly
    cursor.read<uint8_t>();
  }
  {
    // Check that inserting at the end of the buffer keeps it at the end.
    RWPrivateCursor cursor(iobuf1.get());
    Cursor(iobuf1.get()).clone(cloned, 1);
    EXPECT_EQ(1, cloned->countChainElements());
    EXPECT_EQ(1, cloned->computeChainDataLength());

    cursor.advanceToEnd();
    EXPECT_EQ(17, cursor.getCurrentPosition());
    cursor.insert(std::move(cloned));
    EXPECT_EQ(18, cursor.getCurrentPosition());
    EXPECT_EQ(0, cursor.totalLength());
    EXPECT_EQ(12, iobuf1->countChainElements());
    EXPECT_EQ(18, iobuf1->computeChainDataLength());
    EXPECT_TRUE(cursor.isAtEnd());
  }
}

TEST(IOBuf, cloneWithEmptyBufAtStart) {
  folly::IOBufEqualTo eq;
  auto empty = IOBuf::create(0);
  auto hel = IOBuf::create(3);
  append(hel, "hel");
  auto lo = IOBuf::create(2);
  append(lo, "lo");

  auto iobuf = empty->clone();
  iobuf->prependChain(hel->clone());
  iobuf->prependChain(lo->clone());
  iobuf->prependChain(empty->clone());
  iobuf->prependChain(hel->clone());
  iobuf->prependChain(lo->clone());
  iobuf->prependChain(empty->clone());
  iobuf->prependChain(lo->clone());
  iobuf->prependChain(hel->clone());
  iobuf->prependChain(lo->clone());
  iobuf->prependChain(lo->clone());

  Cursor cursor(iobuf.get());
  std::unique_ptr<IOBuf> cloned;
  char data[3];
  cursor.pull(&data, 3);
  cursor.clone(cloned, 2);
  EXPECT_EQ(1, cloned->countChainElements());
  EXPECT_EQ(2, cloned->length());
  EXPECT_TRUE(eq(lo, cloned));

  cursor.pull(&data, 3);
  EXPECT_EQ("hel", std::string(data, sizeof(data)));

  cursor.skip(2);
  cursor.clone(cloned, 2);
  EXPECT_TRUE(eq(lo, cloned));

  std::string hello = cursor.readFixedString(5);
  cursor.clone(cloned, 2);
  EXPECT_TRUE(eq(lo, cloned));
}

TEST(IOBuf, cloneStopsAtCursorBound) {
  auto iobuf = IOBuf::create(16);
  append(iobuf, "hello world");

  // The clone must hold exactly the bytes the bounded cursor could read, and
  // no more: anything past the bound is not this cursor's to hand out.
  Cursor cursor(iobuf.get(), 5);
  IOBuf cloned;
  EXPECT_EQ(5, cursor.cloneAtMost(cloned, 8));
  EXPECT_EQ(5, cloned.computeChainDataLength());
  EXPECT_EQ("hello", toString(cloned));
}

TEST(IOBuf, cloneStopsAtCursorBoundInChain) {
  auto chain = IOBuf::create(8);
  append(chain, "hello");
  auto tail = IOBuf::create(8);
  append(tail, " world");
  chain->appendToChain(std::move(tail));

  // The bound falls inside the second buffer of the chain.
  Cursor cursor(chain.get(), 8);
  IOBuf cloned;
  EXPECT_EQ(8, cursor.cloneAtMost(cloned, 11));
  EXPECT_EQ(8, cloned.computeChainDataLength());
  EXPECT_EQ("hello wo", toString(cloned));
}

TEST(IOBuf, cloneManagedSharesManagedBuffers) {
  auto iobuf = IOBuf::create(8);
  append(iobuf, "hello");
  auto tail = IOBuf::create(8);
  append(tail, " world");
  iobuf->appendToChain(std::move(tail));

  IOBuf cloned;
  Cursor(iobuf.get()).clone(cloned, 11, CloneOwnership::Managed);

  // Managed data is shared, exactly as an ordinary clone would share it.
  EXPECT_EQ(2, cloned.countChainElements());
  EXPECT_TRUE(cloned.isShared());
  EXPECT_EQ(iobuf->data(), cloned.data());
  EXPECT_EQ("hello world", toString(cloned));
}

TEST(IOBuf, cloneManagedCopiesUnmanagedTightly) {
  constexpr size_t kCapacity = 4096;
  auto data = std::make_unique<char[]>(kCapacity);
  memcpy(data.get(), "hello world", 11);
  auto wrapped = IOBuf::wrapBuffer(data.get(), kCapacity);

  IOBuf cloned;
  Cursor cursor(wrapped.get());
  cursor.skip(6);
  cursor.clone(cloned, 5, CloneOwnership::Managed);

  // The clone owns its data, and is sized by the cloned range rather than by
  // the buffer that range came from.
  EXPECT_TRUE(cloned.isManaged());
  EXPECT_EQ(1, cloned.countChainElements());
  EXPECT_LT(cloned.capacity(), kCapacity);

  wrapped.reset();
  data.reset();
  EXPECT_EQ("world", toString(cloned));
}

TEST(IOBuf, cloneManagedMergesUnmanagedRuns) {
  std::string he = "he", llo = "llo ", wor = "wor", ld = "ld";
  auto chain = IOBuf::wrapBuffer(he.data(), he.size());
  chain->appendToChain(IOBuf::wrapBuffer(llo.data(), llo.size()));
  chain->appendToChain(IOBuf::copyBuffer(wor));
  chain->appendToChain(IOBuf::wrapBuffer(ld.data(), ld.size()));

  IOBuf cloned;
  Cursor(chain.get()).clone(cloned, 11, CloneOwnership::Managed);

  // The two adjacent unmanaged buffers are copied into one, so the chain has
  // three elements rather than four, and the managed buffer between them is
  // still shared rather than copied.
  EXPECT_EQ(3, cloned.countChainElements());
  EXPECT_EQ(6, cloned.length());
  EXPECT_TRUE(cloned.isManaged());
  EXPECT_EQ(chain->next()->next()->data(), cloned.next()->data());
  EXPECT_EQ("hello world", toString(cloned));
}

TEST(IOBuf, cloneManagedSpansEmptyBuffers) {
  // A chain can hold empty buffers; cloning has to look past one rather than
  // treat it as the end of the data.
  auto chain = IOBuf::copyBuffer(StringPiece("hello"));
  chain->appendToChain(IOBuf::create(0));
  chain->appendToChain(IOBuf::copyBuffer(StringPiece(" world")));

  IOBuf cloned;
  EXPECT_EQ(
      11, Cursor(chain.get()).cloneAtMost(cloned, 11, CloneOwnership::Managed));
  EXPECT_EQ("hello world", toString(cloned));
}

TEST(IOBuf, cloneManagedSpansEmptyUnmanagedBuffer) {
  // An empty buffer between a run to copy and a buffer to share: the decision
  // has to be made on the buffer holding the next byte, not on the empty one.
  std::string hello = "hello";
  auto chain = IOBuf::wrapBuffer(hello.data(), hello.size());
  chain->appendToChain(IOBuf::wrapBuffer(hello.data(), 0));
  chain->appendToChain(IOBuf::copyBuffer(StringPiece(" world")));

  IOBuf cloned;
  EXPECT_EQ(
      11, Cursor(chain.get()).cloneAtMost(cloned, 11, CloneOwnership::Managed));
  EXPECT_EQ("hello world", toString(cloned));
}

TEST(IOBuf, cloneManagedStopsAtCursorBound) {
  std::string data(30, 'x');
  auto chain = IOBuf::wrapBuffer(data.data(), 10);
  chain->appendToChain(IOBuf::wrapBuffer(data.data() + 10, 10));
  chain->appendToChain(IOBuf::wrapBuffer(data.data() + 20, 10));

  // The bound falls inside the second buffer, so the copy has to stop there
  // rather than run to the end of the chain.
  Cursor cursor(chain.get(), 15);
  IOBuf cloned;
  EXPECT_EQ(15, cursor.cloneAtMost(cloned, 30, CloneOwnership::Managed));
  EXPECT_EQ(15, cloned.computeChainDataLength());
  EXPECT_TRUE(cloned.isManaged());
}

TEST(IOBuf, cloneManagedIntoSourceBuffer) {
  // The destination may be the very buffer the cursor is reading from.
  std::string storage = "hello world";
  auto buf = IOBuf::wrapBuffer(storage.data(), storage.size());

  Cursor cursor(buf.get());
  EXPECT_EQ(11, cursor.cloneAtMost(*buf, 11, CloneOwnership::Managed));
  EXPECT_TRUE(buf->isManaged());
  std::fill(storage.begin(), storage.end(), '\xff');
  EXPECT_EQ("hello world", toString(*buf));
}

TEST(IOBuf, cloneManagedIntoSourceChainHead) {
  std::string storage = "hello world";
  auto chain = IOBuf::wrapBuffer(storage.data(), 5);
  chain->appendToChain(IOBuf::wrapBuffer(storage.data() + 5, 6));

  // Same, with the destination the head of the chain being read.
  Cursor cursor(chain.get());
  EXPECT_EQ(11, cursor.cloneAtMost(*chain, 11, CloneOwnership::Managed));
  EXPECT_TRUE(chain->isManaged());
  std::fill(storage.begin(), storage.end(), '\xff');
  EXPECT_EQ("hello world", toString(*chain));
}

TEST(IOBuf, cloneManagedTruncated) {
  std::string data = "hello";
  auto wrapped = IOBuf::wrapBuffer(data.data(), data.size());

  IOBuf cloned;
  EXPECT_EQ(
      5,
      Cursor(wrapped.get()).cloneAtMost(cloned, 20, CloneOwnership::Managed));
  EXPECT_TRUE(cloned.isManaged());
  EXPECT_EQ("hello", toString(cloned));

  EXPECT_THROW(
      Cursor(wrapped.get()).clone(cloned, 20, CloneOwnership::Managed),
      std::out_of_range);

  std::unique_ptr<IOBuf> owned;
  Cursor(wrapped.get()).clone(owned, 5, CloneOwnership::Managed);
  EXPECT_TRUE(owned->isManaged());
  EXPECT_EQ("hello", toString(*owned));
}

TEST(IOBuf, Appender) {
  std::unique_ptr<IOBuf> head(IOBuf::create(10));
  append(head, "hello");

  Appender app(head.get(), 10);
  auto cap = head->capacity();
  auto len1 = app.length();
  EXPECT_EQ(cap - 5, len1);
  app.ensure(len1); // won't grow
  EXPECT_EQ(len1, app.length());
  app.ensure(len1 + 1); // will grow
  EXPECT_LE(len1 + 1, app.length());

  append(app, " world");
  EXPECT_EQ("hello world", toString(*head));
}

TEST(IOBuf, Printf) {
  IOBuf head(IOBuf::CREATE, 24);
  Appender app(&head, 32);

  app.printf("%s", "test");
  EXPECT_EQ(head.length(), 4);
  EXPECT_EQ(0, memcmp(head.data(), "test\0", 5));

  app.printf(
      "%d%s %s%s %#x",
      32,
      "this string is",
      "longer than our original allocation size,",
      "and will therefore require a new allocation",
      0x12345678);
  // The tailroom should start with a nul byte now.
  EXPECT_GE(head.prev()->tailroom(), 1u);
  EXPECT_EQ(0, *head.prev()->tail());

  EXPECT_EQ(
      "test32this string is longer than our original "
      "allocation size,and will therefore require a "
      "new allocation 0x12345678",
      head.to<std::string>());
}

TEST(IOBuf, Format) {
  IOBuf head(IOBuf::CREATE, 24);
  Appender app(&head, 32);

  // Test compatibility with the legacy format API.
  app(folly::StringPiece("test"));
  EXPECT_EQ(head.length(), 4);
  EXPECT_EQ(0, memcmp(head.data(), "test", 4));
}

TEST(IOBuf, QueueAppender) {
  folly::IOBufQueue queue;

  // Allocate 100 bytes at once, but don't grow past 1024
  QueueAppender app(&queue, 100);
  size_t n = 1024 / sizeof(uint32_t);
  for (uint32_t i = 0; i < n; ++i) {
    app.writeBE<uint32_t>(i);
  }

  // There must be a goodMallocSize between 100 and 1024...
  EXPECT_LT(1u, queue.front()->countChainElements());
  const IOBuf* buf = queue.front();
  do {
    EXPECT_LE(100u, buf->capacity());
    buf = buf->next();
  } while (buf != queue.front());

  Cursor cursor(queue.front());
  for (uint32_t i = 0; i < n; ++i) {
    EXPECT_EQ(i, cursor.readBE<uint32_t>());
  }

  EXPECT_THROW({ cursor.readBE<uint32_t>(); }, std::out_of_range);
}

TEST(IOBuf, QueueAppenderPushAtMostFillBuffer) {
  folly::IOBufQueue queue;
  // There should be a goodMallocSize between 125 and 1000
  QueueAppender appender{&queue, 125};
  std::vector<uint8_t> data;
  data.resize(1000);
  std::iota(data.begin(), data.end(), uint8_t(0));
  // Add 100 byte
  appender.pushAtMost(data.data(), 100);
  // Add 900 bytes
  appender.pushAtMost(data.data() + 100, data.size() - 100);
  const auto buf = queue.front();
  // Should fill the current buffer before adding another
  EXPECT_LE(2, buf->countChainElements());
  EXPECT_EQ(0, buf->tailroom());
  EXPECT_LE(125, buf->length());
  EXPECT_EQ(1000, buf->computeChainDataLength());
  const StringPiece sp{(const char*)data.data(), data.size()};
  EXPECT_EQ(sp, toString(*buf));
}

TEST(IOBuf, QueueAppenderInsertOwn) {
  auto buf = IOBuf::create(10);
  folly::IOBufQueue queue;
  QueueAppender appender{&queue, 128};
  appender.insert(std::move(buf));

  std::vector<uint8_t> data;
  data.resize(256);
  std::iota(data.begin(), data.end(), 0);
  appender.pushAtMost(folly::range(data));
  // Buffer is owned, so we should write to it
  EXPECT_LE(2, queue.front()->countChainElements());
  EXPECT_EQ(0, queue.front()->tailroom());
  const StringPiece sp{(const char*)data.data(), data.size()};
  EXPECT_EQ(sp, toString(*queue.front()));
}

TEST(IOBuf, QueueAppenderInsertClone) {
  IOBuf buf{IOBuf::CREATE, 100};
  folly::IOBufQueue queue;
  QueueAppender appender{&queue, 100};
  // Buffer is shared, so we create a new buffer to write to
  appender.insert(buf);
  uint8_t x = 42;
  appender.pushAtMost(&x, 1);
  EXPECT_EQ(2, queue.front()->countChainElements());
  EXPECT_EQ(0, queue.front()->length());
  EXPECT_LT(0, queue.front()->tailroom());
  EXPECT_EQ(1, queue.front()->next()->length());
  EXPECT_EQ(x, queue.front()->next()->data()[0]);
}

TEST(IOBuf, QueueAppenderReuseTail) {
  folly::IOBufQueue queue;
  QueueAppender appender{&queue, 100};
  constexpr StringPiece prologue = "hello";
  appender.pushAtMost(
      reinterpret_cast<const uint8_t*>(prologue.data()), prologue.size());
  size_t expectedCapacity = queue.front()->capacity();

  auto unpackable = IOBuf::create(folly::IOBufQueue::kMaxPackCopy + 1);
  unpackable->append(folly::IOBufQueue::kMaxPackCopy + 1);
  expectedCapacity += unpackable->capacity();
  appender.insert(std::move(unpackable));

  constexpr StringPiece epilogue = " world";
  appender.pushAtMost(
      reinterpret_cast<const uint8_t*>(epilogue.data()), epilogue.size());

  EXPECT_EQ(queue.front()->computeChainCapacity(), expectedCapacity);
}

TEST(IOBuf, QueueAppenderRWCursor) {
  folly::IOBufQueue queue;

  QueueAppender app(&queue, 100);
  const size_t n = 1024 / sizeof(uint32_t);
  for (size_t m = 0; m < n; m++) {
    for (uint32_t i = 0; i < n; ++i) {
      app.writeBE<uint32_t>(uint32_t(0));
    }

    RWPrivateCursor rw(app);

    // Advance cursor to skip data that's already overwritten
    rw += m * n * sizeof(uint32_t);

    // Overwrite the data
    for (uint32_t i = 0; i < n; ++i) {
      rw.writeBE<uint32_t>(uint32_t(i + m * n));
    }
  }

  Cursor cursor(queue.front());
  for (uint32_t i = 0; i < n * n; ++i) {
    EXPECT_EQ(i, cursor.readBE<uint32_t>());
  }
}

TEST(IOBuf, QueueAppenderTrimEnd) {
  folly::IOBufQueue queue;
  QueueAppender app{&queue, 100};
  const size_t n = 1024 / sizeof(uint32_t);
  for (size_t i = 0; i < n; i++) {
    app.writeBE<uint32_t>(uint32_t(0));
  }

  app.trimEnd(4);
  EXPECT_EQ(1020, queue.front()->computeChainDataLength());

  app.trimEnd(120);
  EXPECT_EQ(900, queue.front()->computeChainDataLength());

  app.trimEnd(900);
  EXPECT_EQ(nullptr, queue.front());

  EXPECT_THROW(app.trimEnd(100), std::underflow_error);
}

TEST(IOBuf, QueueAppenderEnsureAtMost) {
  folly::IOBufQueue queue;
  constexpr size_t kInitialCapacity = 128;
  constexpr size_t kMaxCapacity = 800;

  QueueAppender app{&queue, kInitialCapacity, kMaxCapacity};

  app.ensure(kInitialCapacity); // won't grow
  EXPECT_EQ(kInitialCapacity, app.length());
  app.ensure(kInitialCapacity + 1); // will grow
  EXPECT_LE(kInitialCapacity + 1, app.length());
  app.ensureWithinMaxGrowth(kMaxCapacity * 2); // will grow up to kMaxCapacity
  EXPECT_EQ(kMaxCapacity, app.length());
  app.ensure(kMaxCapacity + 1); // will grow beyond kMaxCapacity
  EXPECT_LE(kMaxCapacity + 1, app.length());
}

TEST(IOBuf, CursorOperators) {
  // Test operators on a single-item chain
  {
    std::unique_ptr<IOBuf> chain1(IOBuf::create(20));
    chain1->append(10);

    Cursor curs1(chain1.get());
    EXPECT_EQ(0, curs1 - chain1.get());
    EXPECT_FALSE(curs1.isAtEnd());
    curs1.skip(3);
    EXPECT_EQ(3, curs1 - chain1.get());
    EXPECT_FALSE(curs1.isAtEnd());
    curs1.skip(7);
    EXPECT_EQ(10, curs1 - chain1.get());
    EXPECT_TRUE(curs1.isAtEnd());

    Cursor curs2(chain1.get());
    EXPECT_EQ(0, curs2 - chain1.get());
    EXPECT_EQ(10, curs1 - curs2);
    EXPECT_THROW(curs2 - curs1, std::out_of_range);
  }

  // Test cross-chain operations
  {
    std::unique_ptr<IOBuf> chain1(IOBuf::create(20));
    chain1->append(10);
    std::unique_ptr<IOBuf> chain2 = chain1->clone();

    Cursor curs1(chain1.get());
    Cursor curs2(chain2.get());
    EXPECT_THROW(curs1 - curs2, std::out_of_range);
    EXPECT_THROW(curs1 - chain2.get(), std::out_of_range);
  }

  // Test operations on multi-item chains
  {
    std::unique_ptr<IOBuf> chain(IOBuf::create(20));
    chain->append(10);
    chain->appendToChain(chain->clone());
    EXPECT_EQ(20, chain->computeChainDataLength());

    Cursor curs1(chain.get());
    curs1.skip(5);
    Cursor curs2(chain.get());
    curs2.skip(3);
    EXPECT_EQ(2, curs1 - curs2);
    EXPECT_EQ(5, curs1 - chain.get());
    EXPECT_THROW(curs2 - curs1, std::out_of_range);

    curs1.skip(7);
    EXPECT_EQ(9, curs1 - curs2);
    EXPECT_EQ(12, curs1 - chain.get());
    EXPECT_THROW(curs2 - curs1, std::out_of_range);

    curs2.skip(7);
    EXPECT_EQ(2, curs1 - curs2);
    EXPECT_THROW(curs2 - curs1, std::out_of_range);
  }

  // Test isAtEnd() with empty buffers at the end of a chain
  {
    auto iobuf1 = IOBuf::create(20);
    iobuf1->append(15);
    iobuf1->trimStart(5);

    Cursor c(iobuf1.get());
    EXPECT_FALSE(c.isAtEnd());
    c.skip(10);
    EXPECT_TRUE(c.isAtEnd());

    iobuf1->prependChain(IOBuf::create(10));
    iobuf1->prependChain(IOBuf::create(10));
    EXPECT_TRUE(c.isAtEnd());
    iobuf1->prev()->append(5);
    EXPECT_FALSE(c.isAtEnd());
    c.skip(5);
    EXPECT_TRUE(c.isAtEnd());
  }

  // Test canAdvance with a chain of items
  {
    auto chain = IOBuf::create(10);
    chain->append(10);
    chain->appendToChain(chain->clone());
    EXPECT_EQ(2, chain->countChainElements());
    EXPECT_EQ(20, chain->computeChainDataLength());

    Cursor c(chain.get());
    for (size_t i = 0; i <= 20; ++i) {
      EXPECT_TRUE(c.canAdvance(i));
    }
    EXPECT_FALSE(c.canAdvance(21));
    c.skip(10);
    EXPECT_TRUE(c.canAdvance(10));
    EXPECT_FALSE(c.canAdvance(11));
  }
}

TEST(IOBuf, StringOperations) {
  // Test a single buffer with two null-terminated strings and an extra uint8_t
  // at the end
  {
    std::unique_ptr<IOBuf> chain(IOBuf::create(16));
    Appender app(chain.get(), 0);
    app.push(reinterpret_cast<const uint8_t*>("hello\0world\0\x01"), 13);

    Cursor curs(chain.get());
    EXPECT_STREQ("hello", curs.readTerminatedString().c_str());
    EXPECT_STREQ("world", curs.readTerminatedString().c_str());
    EXPECT_EQ(1, curs.read<uint8_t>());
  }

  // Test multiple buffers where the first is empty and the string starts in
  // the second buffer.
  {
    std::unique_ptr<IOBuf> chain(IOBuf::create(8));
    chain->prependChain(IOBuf::create(12));
    Appender app(chain.get(), 0);
    app.push(reinterpret_cast<const uint8_t*>("hello world\0"), 12);

    Cursor curs(chain.get());
    EXPECT_STREQ("hello world", curs.readTerminatedString().c_str());
  }

  // Test multiple buffers with a single null-terminated string spanning them
  {
    std::unique_ptr<IOBuf> chain(IOBuf::create(8));
    chain->prependChain(IOBuf::create(8));
    chain->append(8);
    chain->next()->append(4);
    RWPrivateCursor rwc(chain.get());
    rwc.push(reinterpret_cast<const uint8_t*>("hello world\0"), 12);

    Cursor curs(chain.get());
    EXPECT_STREQ("hello world", curs.readTerminatedString().c_str());
  }

  // Test a reading a null-terminated string that's longer than the maximum
  // allowable length
  {
    std::unique_ptr<IOBuf> chain(IOBuf::create(16));
    Appender app(chain.get(), 0);
    app.push(reinterpret_cast<const uint8_t*>("hello world\0"), 12);

    Cursor curs(chain.get());
    EXPECT_THROW(curs.readTerminatedString('\0', 5), std::length_error);
  }

  // Test reading a null-terminated string from a chain with an empty buffer at
  // the front
  {
    std::unique_ptr<IOBuf> buf(IOBuf::create(8));
    Appender app(buf.get(), 0);
    app.push(reinterpret_cast<const uint8_t*>("hello\0"), 6);
    std::unique_ptr<IOBuf> chain(IOBuf::create(8));
    chain->prependChain(std::move(buf));

    Cursor curs(chain.get());
    EXPECT_STREQ("hello", curs.readTerminatedString().c_str());
  }

  // Test reading a null-terminated string from a chain that doesn't contain the
  // terminator
  {
    std::unique_ptr<IOBuf> buf(IOBuf::create(8));
    Appender app(buf.get(), 0);
    app.push(reinterpret_cast<const uint8_t*>("hello"), 5);
    std::unique_ptr<IOBuf> chain(IOBuf::create(8));
    chain->prependChain(std::move(buf));

    Cursor curs(chain.get());
    EXPECT_THROW(curs.readTerminatedString(), std::out_of_range);
  }

  // Test reading a null-terminated string past the maximum length
  {
    std::unique_ptr<IOBuf> buf(IOBuf::create(8));
    Appender app(buf.get(), 0);
    app.push(reinterpret_cast<const uint8_t*>("hello\0"), 6);
    std::unique_ptr<IOBuf> chain(IOBuf::create(8));
    chain->prependChain(std::move(buf));

    Cursor curs(chain.get());
    EXPECT_THROW(curs.readTerminatedString('\0', 3), std::length_error);
  }

  // Test reading a two fixed-length strings from a single buffer with an extra
  // uint8_t at the end
  {
    std::unique_ptr<IOBuf> chain(IOBuf::create(16));
    Appender app(chain.get(), 0);
    app.push(reinterpret_cast<const uint8_t*>("helloworld\x01"), 11);

    Cursor curs(chain.get());
    EXPECT_STREQ("hello", curs.readFixedString(5).c_str());
    EXPECT_STREQ("world", curs.readFixedString(5).c_str());
    EXPECT_EQ(1, curs.read<uint8_t>());
  }

  // Test multiple buffers where the first is empty and a fixed-length string
  // starts in the second buffer.
  {
    std::unique_ptr<IOBuf> chain(IOBuf::create(8));
    chain->prependChain(IOBuf::create(16));
    Appender app(chain.get(), 0);
    app.push(reinterpret_cast<const uint8_t*>("hello world"), 11);

    Cursor curs(chain.get());
    EXPECT_STREQ("hello world", curs.readFixedString(11).c_str());
  }

  // Test multiple buffers with a single fixed-length string spanning them
  {
    std::unique_ptr<IOBuf> chain(IOBuf::create(8));
    chain->prependChain(IOBuf::create(8));
    chain->append(7);
    chain->next()->append(4);
    RWPrivateCursor rwc(chain.get());
    rwc.push(reinterpret_cast<const uint8_t*>("hello world"), 11);

    Cursor curs(chain.get());
    EXPECT_STREQ("hello world", curs.readFixedString(11).c_str());
  }

  // Test reading a fixed-length string from a chain with an empty buffer at
  // the front
  {
    std::unique_ptr<IOBuf> buf(IOBuf::create(8));
    Appender app(buf.get(), 0);
    app.push(reinterpret_cast<const uint8_t*>("hello"), 5);
    std::unique_ptr<IOBuf> chain(IOBuf::create(8));
    chain->prependChain(std::move(buf));

    Cursor curs(chain.get());
    EXPECT_STREQ("hello", curs.readFixedString(5).c_str());
  }
}

TEST(IOBuf, ReadWhileTrue) {
  auto isAlpha = [](uint8_t ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
  };
  auto isDigit = [](uint8_t ch) { return (ch >= '0' && ch <= '9'); };

  // Test reading alternating alphabetic and numeric strings
  {
    std::unique_ptr<IOBuf> chain(IOBuf::create(32));
    Appender app(chain.get(), 0);
    app.push(StringPiece("hello123world456"));

    Cursor curs(chain.get());
    EXPECT_STREQ("hello", curs.readWhile(isAlpha).c_str());
    EXPECT_STREQ("123", curs.readWhile(isDigit).c_str());
    EXPECT_STREQ("world", curs.readWhile(isAlpha).c_str());
    EXPECT_STREQ("456", curs.readWhile(isDigit).c_str());
    EXPECT_TRUE(curs.isAtEnd());
  }

  // The same, but also use skipWhile()
  {
    std::unique_ptr<IOBuf> chain(IOBuf::create(16));
    Appender app(chain.get(), 0);
    app.push(StringPiece("hello123world456"));

    Cursor curs(chain.get());
    EXPECT_STREQ("hello", curs.readWhile(isAlpha).c_str());
    curs.skipWhile(isDigit);
    curs.skipWhile(isAlpha);
    EXPECT_STREQ("456", curs.readWhile(isDigit).c_str());
    EXPECT_TRUE(curs.isAtEnd());
  }

  // Test readWhile() using data split across multiple buffers,
  // including some empty buffers in the middle of the chain.
  {
    std::unique_ptr<IOBuf> chain;

    // First element in the chain has "he"
    auto buf = IOBuf::create(40);
    Appender app(buf.get(), 0);
    app.push(StringPiece("he"));
    chain = std::move(buf);

    // The second element has "ll", after 10 bytes of headroom
    buf = IOBuf::create(40);
    buf->advance(10);
    app = Appender{buf.get(), 0};
    app.push(StringPiece("ll"));
    chain->prependChain(std::move(buf));

    // The third element is empty
    buf = IOBuf::create(40);
    buf->advance(15);
    chain->prependChain(std::move(buf));

    // The fourth element has "o12"
    buf = IOBuf::create(40);
    buf->advance(37);
    app = Appender{buf.get(), 0};
    app.push(StringPiece("o12"));
    chain->prependChain(std::move(buf));

    // The fifth element has "3"
    buf = IOBuf::create(40);
    app = Appender{buf.get(), 0};
    app.push(StringPiece("3"));
    chain->prependChain(std::move(buf));

    // The sixth element is empty
    buf = IOBuf::create(40);
    chain->prependChain(std::move(buf));

    // The seventh element has "world456"
    buf = IOBuf::create(40);
    app = Appender{buf.get(), 0};
    app.push(StringPiece("world456"));
    chain->prependChain(std::move(buf));

    // The eighth element is empty
    buf = IOBuf::create(40);
    chain->prependChain(std::move(buf));

    Cursor curs(chain.get());
    EXPECT_STREQ("hello", curs.readWhile(isAlpha).c_str());
    EXPECT_STREQ("123", curs.readWhile(isDigit).c_str());
    EXPECT_STREQ("world", curs.readWhile(isAlpha).c_str());
    EXPECT_STREQ("456", curs.readWhile(isDigit).c_str());
    EXPECT_TRUE(curs.isAtEnd());
  }
}

TEST(IOBuf, TestAdvanceToEndSingle) {
  std::unique_ptr<IOBuf> chain(IOBuf::create(10));
  chain->append(10);

  Cursor curs(chain.get());
  curs.advanceToEnd();
  EXPECT_TRUE(curs.isAtEnd());
  EXPECT_EQ(curs - chain.get(), 10);
}

TEST(IOBuf, TestAdvanceToEndMulti) {
  std::unique_ptr<IOBuf> chain(IOBuf::create(10));
  chain->append(10);

  std::unique_ptr<IOBuf> buf(IOBuf::create(5));
  buf->append(5);
  chain->prependChain(std::move(buf));

  buf = IOBuf::create(20);
  buf->append(20);
  chain->prependChain(std::move(buf));

  Cursor curs(chain.get());
  curs.advanceToEnd();
  EXPECT_TRUE(curs.isAtEnd());
  EXPECT_EQ(curs - chain.get(), 35);

  curs.reset(chain.get());
  curs.skip(12);
  curs.advanceToEnd();
  EXPECT_TRUE(curs.isAtEnd());
}

TEST(IOBuf, TestRetreatSingle) {
  std::unique_ptr<IOBuf> chain(IOBuf::create(20));
  chain->append(20);

  Cursor curs(chain.get());
  EXPECT_EQ(curs.retreatAtMost(0), 0);
  EXPECT_EQ(curs.totalLength(), 20);
  EXPECT_EQ(curs.retreatAtMost(5), 0);
  EXPECT_EQ(curs.totalLength(), 20);
  EXPECT_EQ(curs.retreatAtMost(25), 0);
  EXPECT_EQ(curs.totalLength(), 20);

  curs.retreat(0);
  EXPECT_THROW(curs.retreat(5), std::out_of_range);
  curs.reset(chain.get());
  EXPECT_THROW(curs.retreat(25), std::out_of_range);
  curs.reset(chain.get());

  curs.advanceToEnd();
  curs.retreat(5);
  EXPECT_EQ(curs.totalLength(), 5);
  curs.retreat(10);
  EXPECT_EQ(curs.totalLength(), 15);
  EXPECT_THROW(curs.retreat(10), std::out_of_range);

  curs.reset(chain.get());
  curs.advanceToEnd();
  EXPECT_EQ(curs.retreatAtMost(5), 5);
  EXPECT_EQ(curs.totalLength(), 5);
  EXPECT_EQ(curs.retreatAtMost(10), 10);
  EXPECT_EQ(curs.totalLength(), 15);
  EXPECT_EQ(curs.retreatAtMost(10), 5);
  EXPECT_EQ(curs.totalLength(), 20);
}

TEST(IOBuf, TestRetreatMulti) {
  std::unique_ptr<IOBuf> chain(IOBuf::create(10));
  chain->append(10);

  std::unique_ptr<IOBuf> buf(IOBuf::create(5));
  buf->append(5);
  chain->prependChain(std::move(buf));

  buf = IOBuf::create(20);
  buf->append(20);
  chain->prependChain(std::move(buf));

  Cursor curs(chain.get());
  EXPECT_EQ(curs.retreatAtMost(10), 0);
  EXPECT_THROW(curs.retreat(10), std::out_of_range);
  curs.reset(chain.get());

  curs.advanceToEnd();
  curs.retreat(20);
  EXPECT_EQ(curs.totalLength(), 20);
  EXPECT_EQ(curs.length(), 20);
  curs.retreat(1);
  EXPECT_EQ(curs.totalLength(), 21);
  EXPECT_EQ(curs.length(), 1);
  EXPECT_EQ(curs.retreatAtMost(50), 14);
  EXPECT_EQ(curs.totalLength(), 35);

  curs.advanceToEnd();
  curs.retreat(30);
  EXPECT_EQ(curs.totalLength(), 30);
}

TEST(IOBuf, TestRetreatOperators) {
  std::unique_ptr<IOBuf> chain(IOBuf::create(20));
  chain->append(20);

  Cursor curs(chain.get());
  curs.advanceToEnd();
  curs -= 5;
  EXPECT_EQ(curs.totalLength(), 5);

  curs.advanceToEnd();
  auto retreated = curs - 5;
  EXPECT_EQ(retreated.totalLength(), 5);
  EXPECT_EQ(curs.totalLength(), 0);
}

TEST(IOBuf, tryRead) {
  unique_ptr<IOBuf> iobuf1(IOBuf::create(6));
  iobuf1->append(6);
  unique_ptr<IOBuf> iobuf2(IOBuf::create(24));
  iobuf2->append(24);

  iobuf1->prependChain(std::move(iobuf2));

  EXPECT_TRUE(iobuf1->isChained());

  RWPrivateCursor wcursor(iobuf1.get());
  Cursor rcursor(iobuf1.get());
  wcursor.writeLE<uint32_t>((uint32_t)1);
  wcursor.writeLE<uint64_t>((uint64_t)1);
  wcursor.writeLE<uint64_t>((uint64_t)1);
  wcursor.writeLE<uint64_t>((uint64_t)1);
  wcursor.writeLE<uint16_t>((uint16_t)1);
  EXPECT_EQ(0, wcursor.totalLength());

  EXPECT_EQ(1u, rcursor.readLE<uint32_t>());

  EXPECT_EQ(1u, rcursor.readLE<uint32_t>());
  EXPECT_EQ(0u, rcursor.readLE<uint32_t>());

  EXPECT_EQ(1u, rcursor.readLE<uint32_t>());
  rcursor.skip(4);

  uint32_t val;
  EXPECT_TRUE(rcursor.tryRead(val));
  EXPECT_EQ(1, val);
  EXPECT_TRUE(rcursor.tryRead(val));

  EXPECT_EQ(0, val);
  EXPECT_FALSE(rcursor.tryRead(val));
}

TEST(IOBuf, tryReadLE) {
  IOBuf buf{IOBuf::CREATE, 4};
  buf.append(4);

  RWPrivateCursor wcursor(&buf);
  Cursor rcursor(&buf);

  const uint32_t expected = 0x01020304;
  wcursor.writeLE<uint32_t>(expected);
  uint32_t actual;
  EXPECT_TRUE(rcursor.tryReadLE(actual));
  EXPECT_EQ(expected, actual);
}

TEST(IOBuf, tryReadBE) {
  IOBuf buf{IOBuf::CREATE, 4};
  buf.append(4);

  RWPrivateCursor wcursor(&buf);
  Cursor rcursor(&buf);

  const uint32_t expected = 0x01020304;
  wcursor.writeBE<uint32_t>(expected);
  uint32_t actual;
  EXPECT_TRUE(rcursor.tryReadBE(actual));
  EXPECT_EQ(expected, actual);
}

TEST(IOBuf, tryReadConsumesAllInputOnFailure) {
  IOBuf buf{IOBuf::CREATE, 2};
  buf.append(2);

  Cursor rcursor(&buf);
  uint32_t val;
  EXPECT_FALSE(rcursor.tryRead(val));
  EXPECT_EQ(0, rcursor.totalLength());
}

TEST(IOBuf, readConsumesAllInputOnFailure) {
  IOBuf buf{IOBuf::CREATE, 2};
  buf.append(2);

  Cursor rcursor(&buf);
  EXPECT_THROW(rcursor.read<uint32_t>(), std::out_of_range);
  EXPECT_EQ(0, rcursor.totalLength());
}

TEST(IOBuf, pushEmptyByteRange) {
  // Test pushing an empty ByteRange.  This mainly tests that we do not
  // trigger UBSAN warnings by calling memcpy() with an null source pointer,
  // which is undefined behavior even if the length is 0.
  IOBuf buf{IOBuf::CREATE, 2};
  ByteRange emptyBytes;

  // Test calling Cursor::push()
  RWPrivateCursor wcursor(&buf);
  wcursor.push(emptyBytes);
  EXPECT_EQ(0, buf.computeChainDataLength());

  // Test calling Appender::push()
  Appender app(&buf, 16);
  app.push(emptyBytes);
  EXPECT_EQ(0, buf.computeChainDataLength());
}

TEST(IOBuf, positionTracking) {
  unique_ptr<IOBuf> iobuf1(IOBuf::create(6));
  iobuf1->append(6);
  unique_ptr<IOBuf> iobuf2(IOBuf::create(24));
  iobuf2->append(24);
  iobuf1->prependChain(std::move(iobuf2));

  Cursor cursor(iobuf1.get());

  EXPECT_EQ(0, cursor.getCurrentPosition());
  EXPECT_EQ(6, cursor.length());

  cursor.skip(3);
  EXPECT_EQ(3, cursor.getCurrentPosition());
  EXPECT_EQ(3, cursor.length());

  // Test that we properly handle advancing to the next chunk.
  cursor.skip(4);
  EXPECT_EQ(7, cursor.getCurrentPosition());
  EXPECT_EQ(23, cursor.length());

  // Test that we properly handle doing to the previous chunk.
  cursor.retreat(2);
  EXPECT_EQ(5, cursor.getCurrentPosition());
  EXPECT_EQ(1, cursor.length());

  // Test that we properly handle advanceToEnd
  cursor.advanceToEnd();
  EXPECT_EQ(30, cursor.getCurrentPosition());
  EXPECT_EQ(0, cursor.totalLength());

  // Reset to 0.
  cursor.reset(iobuf1.get());
  EXPECT_EQ(0, cursor.getCurrentPosition());
  EXPECT_EQ(30, cursor.totalLength());
}

TEST(IOBuf, BoundedCursorSanity) {
  // initialize bounded Cursor with length 0
  std::unique_ptr<IOBuf> chain1(IOBuf::create(20));
  chain1->append(10);
  Cursor subC(chain1.get(), 0);
  EXPECT_EQ(0, subC.totalLength());
  EXPECT_EQ(0, subC.length());
  EXPECT_EQ(0, subC - chain1.get());
  EXPECT_TRUE(subC.isAtEnd());
  EXPECT_FALSE(subC.canAdvance(1));
  EXPECT_THROW(subC.skip(1), std::out_of_range);

  // multi-item chain
  chain1->appendToChain(chain1->clone());
  EXPECT_EQ(2, chain1->countChainElements());
  EXPECT_EQ(20, chain1->computeChainDataLength());

  Cursor subCurs1(chain1.get(), 0);
  EXPECT_EQ(0, subCurs1.totalLength());
  EXPECT_EQ(0, subCurs1.length());
  EXPECT_TRUE(subCurs1.isAtEnd());
  EXPECT_FALSE(subCurs1.canAdvance(1));
  EXPECT_THROW(subCurs1.skip(1), std::out_of_range);

  // initialize bounded Cursor with length greater than total IOBuf bytes
  Cursor subCurs2(chain1.get(), 25);
  EXPECT_EQ(20, subCurs2.totalLength());
  EXPECT_FALSE(subCurs2.isAtEnd());
  EXPECT_TRUE(subCurs2.canAdvance(1));
  subCurs2.skip(10);
  EXPECT_EQ(10, subCurs2.totalLength());
  EXPECT_EQ(10, subCurs2 - chain1.get());
  EXPECT_FALSE(subCurs2.isAtEnd());

  subCurs2.skip(3);
  EXPECT_EQ(7, subCurs2.totalLength());
  EXPECT_EQ(13, subCurs2 - chain1.get());
  EXPECT_EQ(7, subCurs2.skipAtMost(10));
  EXPECT_TRUE(subCurs2.isAtEnd());

  // initialize bounded Cursor with another bounded Cursor
  Cursor subCurs3(chain1.get(), 16);
  EXPECT_EQ(16, subCurs3.totalLength());
  EXPECT_FALSE(subCurs3.isAtEnd());
  subCurs3.skip(4);
  EXPECT_EQ(12, subCurs3.totalLength());
  EXPECT_FALSE(subCurs3.isAtEnd());
  Cursor subCurs4(subCurs3, 9);
  EXPECT_EQ(9, subCurs4.totalLength());
  EXPECT_FALSE(subCurs4.isAtEnd());
  EXPECT_EQ(0, subCurs3 - subCurs4);
  subCurs4.skip(5);
  EXPECT_EQ(4, subCurs4.totalLength());
  EXPECT_FALSE(subCurs4.isAtEnd());
  EXPECT_EQ(5, subCurs4 - subCurs3);
  subCurs4.skip(4);
  EXPECT_TRUE(subCurs4.isAtEnd());
  EXPECT_EQ(0, subCurs4.totalLength());

  // Initialize a bounded Cursor with length greater than the bytes left in the
  // bounded Cursor in the ctor argument
  EXPECT_THROW(Cursor(subCurs3, 20), std::out_of_range);
  // Initialize a bounded Cursor with equal length as the bounded Cursor
  // in the ctor arguments
  Cursor subCurs5(subCurs3, 12);
  EXPECT_EQ(12, subCurs5.totalLength());
  EXPECT_FALSE(subCurs5.isAtEnd());

  // Initialize a Cursor with bounded Cursor
  Cursor subCurs6(chain1.get(), 17);
  EXPECT_EQ(17, subCurs6.totalLength());
  Cursor curs(subCurs6);
  EXPECT_TRUE(curs.isBounded());
  EXPECT_EQ(17, curs.totalLength());
  EXPECT_EQ(0, curs - chain1.get());
  EXPECT_FALSE(curs.isAtEnd());
  curs.advanceToEnd();
  EXPECT_EQ(0, curs.totalLength());
  EXPECT_TRUE(curs.isAtEnd());
  EXPECT_EQ(17, curs - chain1.get());

  subCurs6.skip(7);
  EXPECT_EQ(10, subCurs6.totalLength());
  Cursor curs1(subCurs6);
  EXPECT_TRUE(curs1.isBounded());
  EXPECT_EQ(10, curs1.totalLength());
  EXPECT_EQ(7, curs1 - chain1.get());
  EXPECT_FALSE(curs1.isAtEnd());

  // test reset
  curs.reset(chain1.get());
  EXPECT_FALSE(curs.isBounded());
  EXPECT_EQ(20, curs.totalLength());
  EXPECT_EQ(0, curs - chain1.get());
  EXPECT_FALSE(curs.isAtEnd());
}

TEST(IOBuf, BoundedCursorOperators) {
  // Test operators on a single-item chain
  {
    std::unique_ptr<IOBuf> chain1(IOBuf::create(20));
    chain1->append(10);

    Cursor subCurs1(chain1.get(), 8);
    EXPECT_EQ(8, subCurs1.totalLength());
    EXPECT_EQ(0, subCurs1 - chain1.get());
    EXPECT_FALSE(subCurs1.isAtEnd());
    subCurs1.skip(3);
    EXPECT_EQ(5, subCurs1.totalLength());
    EXPECT_EQ(3, subCurs1 - chain1.get());
    EXPECT_FALSE(subCurs1.isAtEnd());
    subCurs1.skip(5);
    EXPECT_EQ(0, subCurs1.totalLength());
    EXPECT_EQ(8, subCurs1 - chain1.get());
    EXPECT_TRUE(subCurs1.isAtEnd());

    Cursor curs1(chain1.get());
    Cursor subCurs2(curs1, 6);
    EXPECT_EQ(6, subCurs2.totalLength());
    EXPECT_EQ(0, subCurs2 - chain1.get());
    EXPECT_EQ(8, subCurs1 - subCurs2);
    EXPECT_THROW(subCurs2 - subCurs1, std::out_of_range);
    EXPECT_EQ(subCurs2.retreatAtMost(10), 0);
    EXPECT_THROW(subCurs2.retreat(10), std::out_of_range);
    subCurs2.advanceToEnd();
    EXPECT_EQ(2, subCurs1 - subCurs2);
    subCurs1.retreat(1);
    EXPECT_EQ(1, subCurs1.totalLength());
    EXPECT_EQ(subCurs1.retreatAtMost(10), 7);
    EXPECT_EQ(0, subCurs1 - chain1.get());
    EXPECT_EQ(8, subCurs1.totalLength());
    EXPECT_EQ(6, subCurs2 - subCurs1);
    subCurs1.advanceToEnd();
    EXPECT_EQ(2, subCurs1 - subCurs2);
  }

  // Test cross-chain operations
  {
    std::unique_ptr<IOBuf> chain1(IOBuf::create(20));
    chain1->append(10);
    std::unique_ptr<IOBuf> chain2 = chain1->clone();

    Cursor subCurs1(chain1.get(), 9);
    Cursor subCurs2(chain2.get(), 9);
    EXPECT_THROW(subCurs1 - subCurs2, std::out_of_range);
    EXPECT_THROW(subCurs1 - chain2.get(), std::out_of_range);
  }

  // Test operations on multi-item chains
  {
    std::unique_ptr<IOBuf> iobuf1(IOBuf::create(20));
    iobuf1->append(10);
    std::unique_ptr<IOBuf> iobuf2(IOBuf::create(20));
    iobuf2->append(10);
    std::unique_ptr<IOBuf> iobuf3(IOBuf::create(20));
    iobuf3->append(10);
    iobuf1->prependChain(std::move(iobuf2));
    iobuf1->prependChain(std::move(iobuf3));
    EXPECT_EQ(3, iobuf1->countChainElements());
    EXPECT_EQ(30, iobuf1->computeChainDataLength());

    Cursor curs1(iobuf1.get());
    Cursor subCurs1(curs1, 28);
    subCurs1.skip(5);
    EXPECT_EQ(23, subCurs1.totalLength());
    EXPECT_EQ(5, subCurs1 - iobuf1.get());
    EXPECT_FALSE(subCurs1.isAtEnd());
    Cursor subCurs2(iobuf1.get(), 15);
    subCurs2.skip(3);
    EXPECT_EQ(12, subCurs2.totalLength());
    EXPECT_EQ(2, subCurs1 - subCurs2);
    EXPECT_THROW(subCurs2 - subCurs1, std::out_of_range);

    subCurs1.skip(7);
    EXPECT_EQ(9, subCurs1 - subCurs2);
    EXPECT_EQ(12, subCurs1 - iobuf1.get());
    EXPECT_FALSE(subCurs1.isAtEnd());
    EXPECT_EQ(subCurs1.length(), 8);
    EXPECT_THROW(subCurs2 - subCurs1, std::out_of_range);

    subCurs2.skip(7);
    EXPECT_EQ(5, subCurs2.totalLength());
    EXPECT_EQ(10, subCurs2 - iobuf1.get());
    EXPECT_FALSE(subCurs2.isAtEnd());
    EXPECT_EQ(2, subCurs1 - subCurs2);
    EXPECT_THROW(subCurs2 - subCurs1, std::out_of_range);

    subCurs1.advanceToEnd();
    EXPECT_EQ(28, subCurs1 - iobuf1.get());
    EXPECT_EQ(18, subCurs1 - subCurs2);
    EXPECT_TRUE(subCurs1.isAtEnd());

    subCurs1.retreat(8);
    EXPECT_FALSE(subCurs1.isAtEnd());
    EXPECT_EQ(subCurs1.totalLength(), 8);
    EXPECT_EQ(subCurs1.length(), 8);
    EXPECT_EQ(20, subCurs1 - iobuf1.get());
    EXPECT_EQ(10, subCurs1 - subCurs2);
    subCurs1.retreat(1);
    EXPECT_EQ(subCurs1.totalLength(), 9);
    EXPECT_EQ(subCurs1.length(), 1);
    EXPECT_EQ(19, subCurs1 - iobuf1.get());
    EXPECT_EQ(subCurs1.retreatAtMost(20), 19);
    EXPECT_EQ(subCurs1.totalLength(), 28);
    EXPECT_EQ(0, subCurs1 - iobuf1.get());
    EXPECT_EQ(10, subCurs2 - subCurs1);
    EXPECT_THROW(subCurs1.retreat(1), std::out_of_range);
  }

  // Test canAdvance with a chain of items
  {
    auto chain = IOBuf::create(10);
    chain->append(10);
    chain->appendToChain(chain->clone());
    EXPECT_EQ(2, chain->countChainElements());
    EXPECT_EQ(20, chain->computeChainDataLength());

    Cursor c(chain.get());
    Cursor subC(c, 14);
    for (size_t i = 0; i <= 14; ++i) {
      EXPECT_TRUE(subC.canAdvance(i));
    }
    EXPECT_FALSE(subC.canAdvance(15));
    subC.skip(10);
    EXPECT_TRUE(subC.canAdvance(4));
    EXPECT_FALSE(subC.canAdvance(5));
  }
}

TEST(IOBuf, BoundedCursorPullAndPeek) {
  std::unique_ptr<IOBuf> iobuf1(IOBuf::create(10));
  append(iobuf1, "he");
  std::unique_ptr<IOBuf> iobuf2(IOBuf::create(10));
  append(iobuf2, "llo ");
  std::unique_ptr<IOBuf> iobuf3(IOBuf::create(10));
  append(iobuf3, "world abc");
  iobuf1->prependChain(std::move(iobuf2));
  iobuf1->prependChain(std::move(iobuf3));
  EXPECT_EQ(3, iobuf1->countChainElements());
  EXPECT_EQ(15, iobuf1->computeChainDataLength());

  std::array<char, 20> buf;
  buf.fill(0);
  Cursor subCurs1(iobuf1.get(), 13);
  subCurs1.pull(buf.data(), 13);
  EXPECT_EQ("hello world a", std::string(buf.data()));
  EXPECT_TRUE(subCurs1.isAtEnd());

  buf.fill(0);
  Cursor subCurs2(iobuf1.get(), 11);
  EXPECT_EQ(11, subCurs2.pullAtMost(buf.data(), 20));
  EXPECT_EQ("hello world", std::string(buf.data()));
  EXPECT_TRUE(subCurs2.isAtEnd());

  buf.fill(0);
  Cursor subCurs3(iobuf1.get(), 2);
  EXPECT_EQ(2, subCurs3.totalLength());
  EXPECT_EQ(0, subCurs3 - iobuf1.get());
  EXPECT_EQ(2, subCurs3.pullAtMost(buf.data(), 20));
  EXPECT_EQ("he", std::string(buf.data()));
  EXPECT_TRUE(subCurs3.isAtEnd());
  EXPECT_EQ(0, subCurs3.totalLength());
  // the following test makes sure that tryAdvanceBuffer inside pullAtMostSlow
  // won't go over the entire IOBuf chain, but terminate at the right boundary
  EXPECT_EQ(2, subCurs3 - iobuf1.get());

  EXPECT_THROW(
      { Cursor(iobuf1.get(), 11).pull(buf.data(), 20); }, std::out_of_range);

  {
    Cursor subCursor(iobuf1.get(), 13);
    auto b = subCursor.peekBytes();
    EXPECT_EQ(b.data(), subCursor.peek().data());
    EXPECT_EQ(b.size(), subCursor.peek().size());
    EXPECT_EQ("he", StringPiece(b));
    subCursor.skip(b.size());
    b = subCursor.peekBytes();
    EXPECT_EQ(b.data(), subCursor.peek().data());
    EXPECT_EQ(b.size(), subCursor.peek().size());
    EXPECT_EQ("llo ", StringPiece(b));
    subCursor.skip(b.size());
    b = subCursor.peekBytes();
    EXPECT_EQ(b.data(), subCursor.peek().data());
    EXPECT_EQ(b.size(), subCursor.peek().size());
    EXPECT_EQ("world a", StringPiece(b));
    subCursor.skip(b.size());
    EXPECT_EQ(3, iobuf1->countChainElements());
    EXPECT_EQ(15, iobuf1->computeChainDataLength());
  }
}

TEST(IOBuf, ThinCursorAdvances) {
  // [0, 1)
  unique_ptr<IOBuf> iobuf1(IOBuf::create(1));
  iobuf1->append(1);

  // [1, 3)
  unique_ptr<IOBuf> iobuf2(IOBuf::create(2));
  iobuf2->append(2);

  // [3, 6)
  unique_ptr<IOBuf> iobuf3(IOBuf::create(3));
  iobuf3->append(3);

  // [7, 11)
  unique_ptr<IOBuf> iobuf4(IOBuf::create(4));
  iobuf4->append(4);

  // [11, 16)
  unique_ptr<IOBuf> iobuf5(IOBuf::create(5));
  iobuf5->append(5);

  // [16, 20)
  unique_ptr<IOBuf> iobuf6(IOBuf::create(4));
  iobuf6->append(4);

  iobuf1->prependChain(std::move(iobuf2));
  iobuf1->prependChain(std::move(iobuf3));
  iobuf1->prependChain(std::move(iobuf4));
  iobuf1->prependChain(std::move(iobuf5));
  iobuf1->prependChain(std::move(iobuf6));

  RWPrivateCursor wcursor(iobuf1.get());
  // Store into [0, 2)
  wcursor.writeLE<uint16_t>((uint16_t)0x1122);
  // Store into [2, 6)
  wcursor.writeBE<uint32_t>((uint32_t)0x33445566);
  // Store into [6, 8)
  wcursor.write<uint16_t>((uint16_t)0x7788);
  // Store into [8, 12)
  wcursor.write<uint32_t>((uint32_t)0x99AABBCC);
  // Store into [12, 16)
  wcursor.write<uint32_t>((uint32_t)0xDDEEFF00);

  Cursor rcursor(iobuf1.get());
  ThinCursor thinCursor = rcursor.borrow();

  EXPECT_EQ(iobuf1->data(), thinCursor.data());
  EXPECT_EQ(1, thinCursor.length());
  EXPECT_TRUE(thinCursor.canAdvance(1));
  EXPECT_FALSE(thinCursor.canAdvance(2));
  EXPECT_FALSE(thinCursor.isAtEnd());

  EXPECT_EQ(0x1122, thinCursor.readLE<uint16_t>(rcursor));
  EXPECT_EQ(1, thinCursor.length());

  EXPECT_EQ(0x33445566, thinCursor.readBE<uint32_t>(rcursor));

  thinCursor.skipNoAdvance(2);
  EXPECT_EQ(0x99AABBCC, thinCursor.read<uint32_t>(rcursor));

  // Note: advances IOBufs.
  thinCursor.skip(rcursor, 7);
  EXPECT_TRUE(thinCursor.isAtEnd());
}

TEST(IOBuf, ThinCursorBorrowing) {
  unique_ptr<IOBuf> iobuf(IOBuf::create(4));
  iobuf->append(4);

  RWPrivateCursor wcursor(iobuf.get());
  wcursor.writeBE<uint32_t>((uint32_t)0x11223344);

  Cursor rcursor(iobuf.get());
  EXPECT_EQ(0x11, rcursor.read<uint8_t>());
  ThinCursor thinCursor = rcursor.borrow();
  EXPECT_EQ(0x22, thinCursor.read<uint8_t>(rcursor));
  rcursor.unborrow(std::move(thinCursor));
  EXPECT_EQ(0x33, rcursor.read<uint8_t>());
  thinCursor = rcursor.borrow();
  EXPECT_EQ(0x44, thinCursor.read<uint8_t>(rcursor));
  rcursor.unborrow(std::move(thinCursor));
}
