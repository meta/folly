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

#include <folly/io/IOBufQueue.h>

#include <cstring>
#include <iostream>
#include <stdexcept>

#include <fmt/format.h>
#include <folly/Range.h>
#include <folly/io/Cursor.h>
#include <folly/portability/GTest.h>

using folly::IOBuf;
using folly::IOBufQueue;
using folly::StringPiece;
using std::pair;
using std::string;
using std::unique_ptr;

// String Comma Length macro for string literals
#define SCL(x) (x), sizeof(x) - 1

namespace {

IOBufQueue::Options clOptions;
struct Initializer {
  Initializer() { clOptions.cacheChainLength = true; }
};
Initializer initializer;

unique_ptr<IOBuf> stringToIOBuf(
    const char* s, size_t len, size_t tailroom = 0) {
  unique_ptr<IOBuf> buf = IOBuf::create(len + tailroom);
  memcpy(buf->writableTail(), s, len);
  buf->append(len);
  return buf;
}

void checkConsistency(const IOBufQueue& queue) {
  if (queue.options().cacheChainLength) {
    size_t len = queue.front() ? queue.front()->computeChainDataLength() : 0;
    EXPECT_EQ(len, queue.chainLength());
  }
}

std::string queueToString(const IOBufQueue& queue) {
  std::string out;
  queue.appendToString(out);
  return out;
}

} // namespace

TEST(IOBufQueue, Simple) {
  IOBufQueue queue(clOptions);
  EXPECT_EQ(nullptr, queue.front());
  queue.append(SCL(""));
  EXPECT_EQ(nullptr, queue.front());
  queue.append(unique_ptr<IOBuf>());
  EXPECT_EQ(nullptr, queue.front());
  string emptyString;
  queue.append(emptyString);
  EXPECT_EQ(nullptr, queue.front());
}

TEST(IOBufQueue, Append) {
  IOBufQueue queue(clOptions);
  queue.append(SCL("Hello"));
  IOBufQueue queue2(clOptions);
  queue2.append(SCL(", "));
  queue2.append(SCL("World"));
  checkConsistency(queue);
  checkConsistency(queue2);
  queue.append(queue2.move());
  checkConsistency(queue);
  checkConsistency(queue2);
  const IOBuf* chain = queue.front();
  EXPECT_NE((IOBuf*)nullptr, chain);
  EXPECT_EQ(12, chain->computeChainDataLength());
  EXPECT_EQ(nullptr, queue2.front());
}

TEST(IOBufQueue, Append2) {
  IOBufQueue queue(clOptions);
  queue.append(SCL("Hello"));
  IOBufQueue queue2(clOptions);
  queue2.append(SCL(", "));
  queue2.append(SCL("World"));
  checkConsistency(queue);
  checkConsistency(queue2);
  queue.append(queue2);
  checkConsistency(queue);
  checkConsistency(queue2);
  const IOBuf* chain = queue.front();
  EXPECT_NE((IOBuf*)nullptr, chain);
  EXPECT_EQ(12, chain->computeChainDataLength());
  EXPECT_EQ(nullptr, queue2.front());
}

TEST(IOBufQueue, AppendStringPiece) {
  std::string s("Hello, World");
  IOBufQueue queue(clOptions);
  IOBufQueue queue2(clOptions);
  queue.append(s.data(), s.length());
  queue2.append(s);
  checkConsistency(queue);
  checkConsistency(queue2);
  const IOBuf* chain = queue.front();
  const IOBuf* chain2 = queue2.front();
  EXPECT_EQ(s.length(), chain->computeChainDataLength());
  EXPECT_EQ(s.length(), chain2->computeChainDataLength());
  EXPECT_EQ(0, memcmp(chain->data(), chain2->data(), s.length()));
}

TEST(IOBufQueue, Reset) {
  IOBufQueue queue(clOptions);
  queue.preallocate(8, 8);
  queue.append(SCL("Hello "));
  queue.preallocate(128, 128);
  queue.append(SCL("World"));
  EXPECT_EQ(2, queue.front()->countChainElements());
  queue.reset();
  EXPECT_EQ(queue.front(), nullptr);
  checkConsistency(queue);
}

TEST(IOBufQueue, ClearAndTryReuseLargestBuffer) {
  IOBufQueue queue(clOptions);
  queue.preallocate(8, 8);
  queue.append(SCL("Hello "));

  // Separate allocation sizes enough that, if they're internally rounded up,
  // all the buffers have different capacities.
  queue.preallocate(128, 128);
  queue.append(SCL("World "));
  // The current tail will be kept.
  const IOBuf* kept = queue.front()->prev();

  // The new tail is larger but cannot be reused because it's shared.
  queue.preallocate(256, 256);
  queue.append(SCL("abc"));
  const auto shared = queue.front()->prev()->cloneOne();
  EXPECT_EQ(3, queue.front()->countChainElements());

  queue.clearAndTryReuseLargestBuffer();
  ASSERT_TRUE(queue.front());
  EXPECT_TRUE(queue.empty());
  // Only the largest non-shared buffer is kept.
  EXPECT_EQ(1, queue.front()->countChainElements());
  ASSERT_EQ(kept, queue.front());
  checkConsistency(queue);
}

TEST(IOBufQueue, AppendIOBufRef) {
  IOBufQueue queue(clOptions);
  queue.append(*stringToIOBuf("abc", 3), true);
  EXPECT_EQ(3, queue.chainLength());
  EXPECT_EQ(1, queue.front()->countChainElements());
  EXPECT_EQ("abc", queueToString(queue));
  // Make sure we have enough space to copy over next data.
  queue.preallocate(10, 10, 10);
  EXPECT_LE(10, queue.tailroom());
  auto numElements = queue.front()->countChainElements();
  queue.append(*stringToIOBuf("FooBar", 6), true);
  EXPECT_EQ(9, queue.chainLength());
  // Make sure that we performed copy and not append chain.
  EXPECT_EQ(numElements, queue.front()->countChainElements());
  EXPECT_EQ("abcFooBar", queueToString(queue));
}

namespace {

void appendWithPack(IOBufQueue& queue, IOBuf& chain, bool asRRef) {
  if (asRRef) {
    queue.append(std::move(chain), /* pack */ true);
  } else {
    queue.append(chain, /* pack */ true);
  }
}

} // namespace

TEST(IOBufQueue, AppendIOBufRefChain) {
  const auto test = [](bool asRRef) {
    IOBufQueue queue(clOptions);
    appendWithPack(queue, *stringToIOBuf("abc", 3), asRRef);

    queue.preallocate(10, 10, 10);
    auto numElements = queue.front()->countChainElements();
    auto chain = stringToIOBuf("Hello", 5);
    chain->appendToChain(stringToIOBuf("World", 5));
    appendWithPack(queue, *chain, asRRef);

    // Make sure that we performed a copy and not append chain.
    EXPECT_EQ(numElements, queue.front()->countChainElements());
    EXPECT_EQ("abcHelloWorld", queueToString(queue));
  };

  test(false);
  test(true);
}

TEST(IOBufQueue, AppendIOBufRefChainPartial) {
  const auto test = [](bool asRRef) {
    IOBufQueue queue(clOptions);
    appendWithPack(queue, *stringToIOBuf("abc", 3), asRRef);

    queue.preallocate(16, 16, 16);
    auto numElements = queue.front()->countChainElements();
    auto chain = stringToIOBuf("This fits in 16B", 16);
    chain->appendToChain(stringToIOBuf("Hello ", 5));
    chain->appendToChain(stringToIOBuf("World", 5));
    appendWithPack(queue, *chain, asRRef);

    // Make sure that we performed a copy of first IOBuf and chained the rest.
    EXPECT_EQ(numElements + 2, queue.front()->countChainElements());
    EXPECT_EQ("abcThis fits in 16BHelloWorld", queueToString(queue));
  };

  test(false);
  test(true);
}

TEST(IOBufQueue, Split) {
  IOBufQueue queue(clOptions);
  queue.append(stringToIOBuf(SCL("Hello")));
  queue.append(stringToIOBuf(SCL(",")));
  queue.append(stringToIOBuf(SCL(" ")));
  queue.append(stringToIOBuf(SCL("")));
  queue.append(stringToIOBuf(SCL("World")));
  checkConsistency(queue);
  EXPECT_EQ(12, queue.front()->computeChainDataLength());

  unique_ptr<IOBuf> prefix(queue.split(1));
  checkConsistency(queue);
  EXPECT_EQ(1, prefix->computeChainDataLength());
  EXPECT_EQ(11, queue.front()->computeChainDataLength());
  prefix = queue.split(2);
  checkConsistency(queue);
  EXPECT_EQ(2, prefix->computeChainDataLength());
  EXPECT_EQ(9, queue.front()->computeChainDataLength());
  prefix = queue.split(3);
  checkConsistency(queue);
  EXPECT_EQ(3, prefix->computeChainDataLength());
  EXPECT_EQ(6, queue.front()->computeChainDataLength());
  prefix = queue.split(1);
  checkConsistency(queue);
  EXPECT_EQ(1, prefix->computeChainDataLength());
  EXPECT_EQ(5, queue.front()->computeChainDataLength());
  prefix = queue.split(5);
  checkConsistency(queue);
  EXPECT_EQ(5, prefix->computeChainDataLength());
  EXPECT_EQ((IOBuf*)nullptr, queue.front());

  queue.append(stringToIOBuf(SCL("Hello,")));
  queue.append(stringToIOBuf(SCL(" World")));
  checkConsistency(queue);
  EXPECT_THROW({ prefix = queue.split(13); }, std::underflow_error);
  checkConsistency(queue);
}

TEST(IOBufQueue, SplitAtMost) {
  IOBufQueue queue(clOptions);
  queue.append(stringToIOBuf(SCL("Hello,")));
  queue.append(stringToIOBuf(SCL(" World")));
  auto buf = queue.splitAtMost(9999);
  EXPECT_EQ(buf->computeChainDataLength(), 12);
  EXPECT_TRUE(queue.empty());
}

TEST(IOBufQueue, SplitZero) {
  IOBufQueue queue(clOptions);
  queue.append(stringToIOBuf(SCL("Hello world")));
  auto buf = queue.split(0);
  EXPECT_EQ(buf->computeChainDataLength(), 0);
}

TEST(IOBufQueue, SplitOptimizeCoalescesIntoTailroom) {
  IOBufQueue queue(clOptions);
  // Create an IOBuf with 1024 bytes capacity but only 4 bytes used,
  // leaving ~1020 bytes of tailroom.
  auto big = IOBuf::create(1024);
  memcpy(big->writableTail(), "AAAA", 4);
  big->append(4);
  queue.append(std::move(big));

  // Append small buffers that should fit in the tailroom.
  queue.append(stringToIOBuf(SCL("BB")));
  queue.append(stringToIOBuf(SCL("CC")));
  EXPECT_EQ(8, queue.chainLength());

  auto result = queue.splitAtMost(8, true);
  EXPECT_EQ(8, result->computeChainDataLength());
  // The small buffers should have been coalesced into the first IOBuf's
  // tailroom, producing a single non-chained IOBuf.
  EXPECT_FALSE(result->isChained());
  EXPECT_EQ("AAAABBCC", std::string((const char*)result->data(), 8));
}

TEST(IOBufQueue, SplitOptimizeFallsBackToChainWhenNoTailroom) {
  IOBufQueue queue(clOptions);
  // Create IOBufs with exactly zero tailroom using wrapBuffer.
  const char a[] = "Hello";
  const char b[] = "World";
  queue.append(IOBuf::wrapBuffer(a, 5));
  queue.append(IOBuf::wrapBuffer(b, 5));
  EXPECT_EQ(10, queue.chainLength());

  auto result = queue.splitAtMost(10, true);
  EXPECT_EQ(10, result->computeChainDataLength());
  // Both buffers have no spare tailroom, so the result is still chained.
  EXPECT_TRUE(result->isChained());
}

TEST(IOBufQueue, SplitOptimizeDropsEmptyNodes) {
  IOBufQueue queue(clOptions);
  queue.append(stringToIOBuf(SCL("AA")));
  queue.append(stringToIOBuf(SCL("")));
  queue.append(stringToIOBuf(SCL("BB")));
  EXPECT_EQ(4, queue.chainLength());

  auto result = queue.splitAtMost(4, true);
  EXPECT_EQ(4, result->computeChainDataLength());
  // The empty node should have been dropped, so at most 2 chain links.
  size_t chainLinks = 0;
  const IOBuf* cur = result.get();
  do {
    chainLinks++;
    cur = cur->next();
  } while (cur != result.get());
  EXPECT_LE(chainLinks, 2u);
}

TEST(IOBufQueue, SplitOptimizePreservesDataCorrectness) {
  IOBufQueue queue(clOptions);
  // Build a chain of many small buffers.
  for (int i = 0; i < 100; i++) {
    auto buf = IOBuf::create(32);
    auto s = fmt::format("{:02d}", i);
    memcpy(buf->writableTail(), s.data(), 2);
    buf->append(2);
    queue.append(std::move(buf));
  }
  EXPECT_EQ(200, queue.chainLength());

  // Split with optimization and verify data is byte-identical.
  auto optimized = queue.splitAtMost(200, true);
  EXPECT_EQ(200, optimized->computeChainDataLength());

  // Verify content.
  folly::io::Cursor cursor(optimized.get());
  for (int i = 0; i < 100; i++) {
    auto expected = fmt::format("{:02d}", i);
    auto actual = cursor.readFixedString(2);
    EXPECT_EQ(expected, actual) << "Mismatch at index " << i;
  }
  EXPECT_TRUE(queue.empty());
}

TEST(IOBufQueue, SplitOptimizePartialSplit) {
  IOBufQueue queue(clOptions);
  auto buf = IOBuf::create(1024);
  memcpy(buf->writableTail(), "AAAA", 4);
  buf->append(4);
  queue.append(std::move(buf));
  queue.append(stringToIOBuf(SCL("BBBB")));
  queue.append(stringToIOBuf(SCL("CCCC")));
  EXPECT_EQ(12, queue.chainLength());

  // Split only part of the queue.
  auto result = queue.splitAtMost(8, true);
  EXPECT_EQ(8, result->computeChainDataLength());
  EXPECT_EQ(4, queue.chainLength());

  // Remaining data is correct.
  auto rest = queue.splitAtMost(4, true);
  EXPECT_EQ(4, rest->computeChainDataLength());
  folly::io::Cursor cursor(rest.get());
  EXPECT_EQ("CCCC", cursor.readFixedString(4));
}

TEST(IOBufQueue, SplitOptimizeDefaultIsFalse) {
  // Verify default template argument doesn't optimize.
  IOBufQueue queue(clOptions);
  queue.append(stringToIOBuf(SCL("AA")));
  queue.append(stringToIOBuf(SCL("")));
  queue.append(stringToIOBuf(SCL("BB")));

  auto result = queue.splitAtMost(4);
  EXPECT_EQ(4, result->computeChainDataLength());
  // Default (OptimizeOnSplit=false) preserves all nodes including empty.
  EXPECT_TRUE(result->isChained());
}

TEST(IOBufQueue, Preallocate) {
  IOBufQueue queue(clOptions);
  queue.append(string("Hello"));
  pair<void*, std::size_t> writable = queue.preallocate(2, 64, 64);
  checkConsistency(queue);
  EXPECT_NE((void*)nullptr, writable.first);
  EXPECT_LE(2, writable.second);
  EXPECT_GE(64, writable.second);
  memcpy(writable.first, SCL(", "));
  queue.postallocate(2);
  checkConsistency(queue);
  EXPECT_EQ(7, queue.front()->computeChainDataLength());
  queue.append(SCL("World"));
  checkConsistency(queue);
  EXPECT_EQ(12, queue.front()->computeChainDataLength());
  // There are not 2048 bytes available, this will alloc a new buf
  writable = queue.preallocate(2048, 4096);
  checkConsistency(queue);
  EXPECT_LE(2048, writable.second);
  // IOBuf allocates more than newAllocationSize, and we didn't cap it
  EXPECT_GE(writable.second, 4096);
  queue.postallocate(writable.second);
  // queue has no empty space, make sure we allocate at least min, even if
  // newAllocationSize < min
  writable = queue.preallocate(1024, 1, 1024);
  checkConsistency(queue);
  EXPECT_EQ(1024, writable.second);
}

TEST(IOBufQueue, Wrap) {
  IOBufQueue queue(clOptions);
  const char* buf = "hello world goodbye";
  size_t len = strlen(buf);
  queue.wrapBuffer(buf, len, 6);
  auto iob = queue.move();
  EXPECT_EQ((len - 1) / 6 + 1, iob->countChainElements());
  iob->unshare();
  iob->coalesce();
  EXPECT_EQ(
      StringPiece(buf),
      StringPiece(reinterpret_cast<const char*>(iob->data()), iob->length()));
}

TEST(IOBufQueue, Trim) {
  IOBufQueue queue(clOptions);
  unique_ptr<IOBuf> a = IOBuf::create(4);
  a->append(4);
  queue.append(std::move(a));
  checkConsistency(queue);
  a = IOBuf::create(6);
  a->append(6);
  queue.append(std::move(a));
  checkConsistency(queue);
  a = IOBuf::create(8);
  a->append(8);
  queue.append(std::move(a));
  checkConsistency(queue);
  a = IOBuf::create(10);
  a->append(10);
  queue.append(std::move(a));
  checkConsistency(queue);

  EXPECT_EQ(4, queue.front()->countChainElements());
  EXPECT_EQ(28, queue.front()->computeChainDataLength());
  EXPECT_EQ(4, queue.front()->length());

  queue.trimStart(1);
  checkConsistency(queue);
  EXPECT_EQ(4, queue.front()->countChainElements());
  EXPECT_EQ(27, queue.front()->computeChainDataLength());
  EXPECT_EQ(3, queue.front()->length());

  queue.trimStart(5);
  checkConsistency(queue);
  EXPECT_EQ(3, queue.front()->countChainElements());
  EXPECT_EQ(22, queue.front()->computeChainDataLength());
  EXPECT_EQ(4, queue.front()->length());

  queue.trimEnd(1);
  checkConsistency(queue);
  EXPECT_EQ(3, queue.front()->countChainElements());
  EXPECT_EQ(21, queue.front()->computeChainDataLength());
  EXPECT_EQ(9, queue.front()->prev()->length());

  queue.trimEnd(20);
  checkConsistency(queue);
  EXPECT_EQ(1, queue.front()->countChainElements());
  EXPECT_EQ(1, queue.front()->computeChainDataLength());
  EXPECT_EQ(1, queue.front()->prev()->length());

  queue.trimEnd(1);
  checkConsistency(queue);
  EXPECT_EQ(nullptr, queue.front());

  EXPECT_THROW(queue.trimStart(2), std::underflow_error);
  checkConsistency(queue);

  EXPECT_THROW(queue.trimEnd(30), std::underflow_error);
  checkConsistency(queue);
}

TEST(IOBufQueue, TrimStartAtMost) {
  IOBufQueue queue(clOptions);
  unique_ptr<IOBuf> a = IOBuf::create(4);
  a->append(4);
  queue.append(std::move(a));
  checkConsistency(queue);
  a = IOBuf::create(6);
  a->append(6);
  queue.append(std::move(a));
  checkConsistency(queue);
  a = IOBuf::create(8);
  a->append(8);
  queue.append(std::move(a));
  checkConsistency(queue);
  a = IOBuf::create(10);
  a->append(10);
  queue.append(std::move(a));
  checkConsistency(queue);

  EXPECT_EQ(4, queue.front()->countChainElements());
  EXPECT_EQ(28, queue.front()->computeChainDataLength());
  EXPECT_EQ(4, queue.front()->length());

  queue.trimStartAtMost(1);
  checkConsistency(queue);
  EXPECT_EQ(4, queue.front()->countChainElements());
  EXPECT_EQ(27, queue.front()->computeChainDataLength());
  EXPECT_EQ(3, queue.front()->length());

  queue.trimStartAtMost(50);
  checkConsistency(queue);
  EXPECT_EQ(nullptr, queue.front());
  EXPECT_EQ(0, queue.chainLength());
}

TEST(IOBufQueue, TrimEndAtMost) {
  IOBufQueue queue(clOptions);
  unique_ptr<IOBuf> a = IOBuf::create(4);
  a->append(4);
  queue.append(std::move(a));
  checkConsistency(queue);
  a = IOBuf::create(6);
  a->append(6);
  queue.append(std::move(a));
  checkConsistency(queue);
  a = IOBuf::create(8);
  a->append(8);
  queue.append(std::move(a));
  checkConsistency(queue);
  a = IOBuf::create(10);
  a->append(10);
  queue.append(std::move(a));
  checkConsistency(queue);

  EXPECT_EQ(4, queue.front()->countChainElements());
  EXPECT_EQ(28, queue.front()->computeChainDataLength());
  EXPECT_EQ(4, queue.front()->length());

  queue.trimEndAtMost(1);
  checkConsistency(queue);
  EXPECT_EQ(4, queue.front()->countChainElements());
  EXPECT_EQ(27, queue.front()->computeChainDataLength());
  EXPECT_EQ(4, queue.front()->length());

  queue.trimEndAtMost(50);
  checkConsistency(queue);
  EXPECT_EQ(nullptr, queue.front());
  EXPECT_EQ(0, queue.chainLength());
}

TEST(IOBufQueue, TrimPack) {
  IOBufQueue queue(clOptions);
  unique_ptr<IOBuf> a = IOBuf::create(64);
  a->append(4);
  queue.append(std::move(a), true);
  checkConsistency(queue);
  a = IOBuf::create(6);
  a->append(6);
  queue.append(std::move(a), true);
  checkConsistency(queue);
  a = IOBuf::create(8);
  a->append(8);
  queue.append(std::move(a), true);
  checkConsistency(queue);
  a = IOBuf::create(10);
  a->append(10);
  queue.append(std::move(a), true);
  checkConsistency(queue);

  EXPECT_EQ(1, queue.front()->countChainElements());
  EXPECT_EQ(28, queue.front()->computeChainDataLength());
  EXPECT_EQ(28, queue.front()->length());

  queue.trimStart(1);
  checkConsistency(queue);
  EXPECT_EQ(1, queue.front()->countChainElements());
  EXPECT_EQ(27, queue.front()->computeChainDataLength());
  EXPECT_EQ(27, queue.front()->length());

  queue.trimStart(5);
  checkConsistency(queue);
  EXPECT_EQ(1, queue.front()->countChainElements());
  EXPECT_EQ(22, queue.front()->computeChainDataLength());
  EXPECT_EQ(22, queue.front()->length());

  queue.trimEnd(1);
  checkConsistency(queue);
  EXPECT_EQ(1, queue.front()->countChainElements());
  EXPECT_EQ(21, queue.front()->computeChainDataLength());
  EXPECT_EQ(21, queue.front()->prev()->length());

  queue.trimEnd(20);
  checkConsistency(queue);
  EXPECT_EQ(1, queue.front()->countChainElements());
  EXPECT_EQ(1, queue.front()->computeChainDataLength());
  EXPECT_EQ(1, queue.front()->prev()->length());

  queue.trimEnd(1);
  checkConsistency(queue);
  EXPECT_EQ(nullptr, queue.front());

  EXPECT_THROW(queue.trimStart(2), std::underflow_error);
  checkConsistency(queue);

  EXPECT_THROW(queue.trimEnd(30), std::underflow_error);
  checkConsistency(queue);
}

TEST(IOBufQueue, Prepend) {
  folly::IOBufQueue queue;

  auto buf = folly::IOBuf::create(10);
  buf->advance(5);
  queue.append(std::move(buf));

  queue.append(SCL(" World"));
  queue.prepend(SCL("Hello"));

  EXPECT_THROW(queue.prepend(SCL("x")), std::overflow_error);

  auto out = queue.move();
  out->coalesce();
  EXPECT_EQ(
      "Hello World",
      StringPiece(reinterpret_cast<const char*>(out->data()), out->length()));
}

TEST(IOBufQueue, PopFirst) {
  IOBufQueue queue(IOBufQueue::cacheChainLength());
  const char* strings[] = {"Hello", ",", " ", "", "World"};

  const size_t numStrings = sizeof(strings) / sizeof(*strings);
  size_t chainLength = 0;
  for (size_t i = 0; i < numStrings; ++i) {
    queue.append(stringToIOBuf(strings[i], strlen(strings[i])));
    checkConsistency(queue);
    chainLength += strlen(strings[i]);
  }

  unique_ptr<IOBuf> first;
  for (size_t i = 0; i < numStrings; ++i) {
    checkConsistency(queue);
    EXPECT_EQ(chainLength, queue.front()->computeChainDataLength());
    EXPECT_EQ(chainLength, queue.chainLength());
    first = queue.pop_front();
    chainLength -= strlen(strings[i]);
    EXPECT_EQ(strlen(strings[i]), first->computeChainDataLength());
  }
  checkConsistency(queue);
  EXPECT_EQ(chainLength, queue.chainLength());

  EXPECT_EQ((IOBuf*)nullptr, queue.front());
  first = queue.pop_front();
  EXPECT_EQ((IOBuf*)nullptr, first.get());

  checkConsistency(queue);
  EXPECT_EQ((IOBuf*)nullptr, queue.front());
  EXPECT_EQ(0, queue.chainLength());
}

TEST(IOBufQueue, AppendToString) {
  IOBufQueue queue;
  queue.append("hello ", 6);
  queue.append("world", 5);
  std::string s;
  queue.appendToString(s);
  EXPECT_EQ("hello world", s);
}

TEST(IOBufQueue, Gather) {
  IOBufQueue queue;

  queue.append(stringToIOBuf(SCL("hello ")));
  queue.append(stringToIOBuf(SCL("world")));

  EXPECT_EQ(queue.front()->length(), 6);
  queue.gather(11);
  EXPECT_EQ(queue.front()->length(), 11);

  StringPiece s(
      reinterpret_cast<const char*>(queue.front()->data()),
      queue.front()->length());
  EXPECT_EQ("hello world", s);
}

namespace {
enum class AppendType { Ptr, ConstRef, RRef };
enum class EmptyHead { No, Yes };
enum class Pack { No, Yes };

struct IOBufQueueAppendTest
    : testing::TestWithParam<std::tuple<AppendType, EmptyHead, Pack>> {};
INSTANTIATE_TEST_CASE_P(
    IOBufQueueAppendTest,
    IOBufQueueAppendTest,
    testing::Combine(
        ::testing::Values(
            AppendType::Ptr, AppendType::ConstRef, AppendType::RRef),
        ::testing::Values(EmptyHead::No, EmptyHead::Yes),
        ::testing::Values(Pack::No, Pack::Yes)),
    [](const testing::TestParamInfo<std::tuple<AppendType, EmptyHead, Pack>>&
           pinfo) {
      std::string name;
      switch (std::get<AppendType>(pinfo.param)) {
        case AppendType::Ptr:
          name = "AppendPtr";
          break;
        case AppendType::ConstRef:
          name = "AppendConstRef";
          break;
        case AppendType::RRef:
          name = "AppendRRef";
          break;
      }
      switch (std::get<EmptyHead>(pinfo.param)) {
        case EmptyHead::No:
          name += "NoEmptyHead";
          break;
        case EmptyHead::Yes:
          name += "EmptyHead";
          break;
      }
      switch (std::get<Pack>(pinfo.param)) {
        case Pack::No:
          name += "NoPack";
          break;
        case Pack::Yes:
          name += "Pack";
          break;
      }
      return name;
    });
} // namespace

TEST_P(IOBufQueueAppendTest, ReuseTail) {
  const AppendType appendType = std::get<AppendType>(GetParam());
  const bool withEmptyHead = std::get<EmptyHead>(GetParam()) == EmptyHead::Yes;
  const bool pack = std::get<Pack>(GetParam()) == Pack::Yes;
  IOBufQueue queue;
  IOBufQueue::WritableRangeCache cache(&queue);

  constexpr size_t kInitialCapacity = 1024;
  queue.preallocate(kInitialCapacity, kInitialCapacity);
  size_t expectedCapacity = queue.front()->capacity();

  const auto makeUnpackable = [] {
    auto unpackable = IOBuf::create(IOBufQueue::kMaxPackCopy + 1);
    unpackable->append(IOBufQueue::kMaxPackCopy + 1);
    return unpackable;
  };

  auto unpackable = makeUnpackable();
  expectedCapacity += unpackable->capacity();

  std::unique_ptr<IOBuf> buf;
  size_t packableLength = 0;
  if (withEmptyHead) {
    // In this case, the unpackable buffer should just shift the empty head
    // buffer forward.
    buf = std::move(unpackable);
  } else {
    queue.append("hello ");
    buf = stringToIOBuf(SCL("world"));
    packableLength = buf->length();
    if (!pack) {
      expectedCapacity += buf->computeChainCapacity();
    }
    buf->insertAfterThisOne(std::move(unpackable));
  }

  const auto oldTail = reinterpret_cast<uint8_t*>(queue.writableTail());
  const auto oldTailroom = queue.tailroom();

  // Append two buffers in a row to verify that the reused tail gets pushed
  // forward without wrapping.
  for (size_t i = 0; i < 2; ++i) {
    SCOPED_TRACE(fmt::format("i={}", i));

    switch (appendType) {
      case AppendType::Ptr:
        queue.append(std::move(buf), pack, /* allowTailReuse */ true);
        break;
      case AppendType::ConstRef:
        queue.append(*buf, pack, /* allowTailReuse */ true);
        break;
      case AppendType::RRef:
        queue.append(std::move(*buf), pack, /* allowTailReuse */ true);
        break;
    }

    // We should be able to avoid allocating new memory because we still had
    // room in the old tail, even after packing the first IOBuf.
    EXPECT_EQ(queue.writableTail(), oldTail + (pack ? packableLength : 0));
    EXPECT_EQ(queue.tailroom(), oldTailroom - (pack ? packableLength : 0));
    EXPECT_EQ(queue.front()->computeChainCapacity(), expectedCapacity);
    // if pack is not set, one extra chain element is present in the case of a
    // non-empty head (the first element appended)
    EXPECT_EQ(
        queue.front()->countChainElements(),
        i + (withEmptyHead ? 2 : (pack ? 0 : 1) + 3));

    if (i == 0) {
      buf = makeUnpackable();
      expectedCapacity += buf->capacity();
    }
  }
}

TEST(IOBufQueue, PackWithSharedTail) {
  auto buf = stringToIOBuf("Hello ", 6, 10);
  IOBufQueue queue;
  queue.append(buf->clone());
  *buf->writableTail() = 'X';
  buf->append(1);
  queue.append(stringToIOBuf("world", 5), /* pack */ true);
  // buf is shared, packing should not modify it.
  EXPECT_EQ(buf->data()[6], 'X');
}
