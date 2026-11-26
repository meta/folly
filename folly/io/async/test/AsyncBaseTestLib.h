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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include <glog/logging.h>

#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <folly/io/FsUtil.h>
#include <folly/portability/GTest.h>
#include <folly/portability/Sockets.h>
#include <folly/portability/Unistd.h>
#include <folly/test/TestUtils.h>

#include <folly/io/async/AsyncBase.h>
#include <folly/testing/TestUtil.h>

namespace folly {
namespace test {
namespace async_base_test_lib_detail {

constexpr size_t kODirectAlign = 4096; // align reads to 4096 B (for O_DIRECT)
constexpr size_t kDefaultFileSize = 6 << 20; // 6MiB

constexpr size_t kBatchNumEntries = 1024;
constexpr size_t kBatchSize = 192;
constexpr size_t kBatchBlockSize = 4096;

struct TestSpec {
  off_t start;
  size_t size;
};

struct TestUtil {
  static void waitUntilReadable(int fd);
  static folly::Range<folly::AsyncBase::Op**> readerWait(
      folly::AsyncBase* reader);
  using ManagedBuffer = std::unique_ptr<char, void (*)(void*)>;
  static ManagedBuffer allocateAligned(size_t size);
};

template <typename TAsync>
std::unique_ptr<TAsync> getAIO(size_t capacity, AsyncBase::PollMode pollMode) {
  try {
    auto ret = std::make_unique<TAsync>(capacity, pollMode);
    ret->initializeContext();

    return ret;
  } catch (const std::runtime_error&) {
  }

  return nullptr;
}

template <typename TAsync>
void testReadsSerially(
    const std::vector<TestSpec>& specs, folly::AsyncBase::PollMode pollMode) {
  auto aioReader = getAIO<TAsync>(1, pollMode);
  SKIP_IF(!aioReader) << "TAsync not available";

  typename TAsync::Op op;
  auto tempFile =
      folly::test::makeTemporaryFileWithRandomData(kDefaultFileSize);
  int fd = ::open(tempFile.path().c_str(), O_DIRECT | O_RDONLY);
  if (fd == -1) {
    fd = ::open(tempFile.path().c_str(), O_RDONLY);
  }
  SKIP_IF(fd == -1) << "Tempfile can't be opened: " << folly::errnoStr(errno);
  SCOPE_EXIT {
    fileops::close(fd);
  };

  for (size_t i = 0; i < specs.size(); i++) {
    auto buf = TestUtil::allocateAligned(specs[i].size);
    op.pread(fd, buf.get(), specs[i].size, specs[i].start);
    aioReader->submit(&op);
    EXPECT_EQ((i + 1), aioReader->totalSubmits());
    EXPECT_EQ(aioReader->pending(), 1);
    auto ops =
        test::async_base_test_lib_detail::TestUtil::readerWait(aioReader.get());
    EXPECT_EQ(1, ops.size());
    EXPECT_TRUE(ops[0] == &op);
    EXPECT_EQ(aioReader->pending(), 0);
    ssize_t res = op.result();
    EXPECT_LE(0, res) << folly::errnoStr(-res);
    EXPECT_EQ(specs[i].size, res);
    op.reset();
  }
}

template <typename TAsync>
void testReadsParallel(
    const std::vector<TestSpec>& specs,
    folly::AsyncBase::PollMode pollMode,
    bool multithreaded) {
  auto aioReader = getAIO<TAsync>(specs.size(), pollMode);
  SKIP_IF(!aioReader) << "TAsync not available";

  std::unique_ptr<typename TAsync::Op[]> ops(
      new typename TAsync::Op[specs.size()]);
  uintptr_t sizeOf = sizeof(typename TAsync::Op);
  std::vector<TestUtil::ManagedBuffer> bufs;
  bufs.reserve(specs.size());

  auto tempFile =
      folly::test::makeTemporaryFileWithRandomData(kDefaultFileSize);
  int fd = ::open(tempFile.path().c_str(), O_DIRECT | O_RDONLY);
  if (fd == -1) {
    fd = ::open(tempFile.path().c_str(), O_RDONLY);
  }
  SKIP_IF(fd == -1) << "Tempfile can't be opened: " << folly::errnoStr(errno);
  SCOPE_EXIT {
    fileops::close(fd);
  };

  std::vector<std::thread> threads;
  if (multithreaded) {
    threads.reserve(specs.size());
  }
  for (size_t i = 0; i < specs.size(); i++) {
    bufs.push_back(TestUtil::allocateAligned(specs[i].size));
  }
  auto submit = [&](size_t i) {
    ops[i].pread(fd, bufs[i].get(), specs[i].size, specs[i].start);
    aioReader->submit(&ops[i]);
  };
  for (size_t i = 0; i < specs.size(); i++) {
    if (multithreaded) {
      threads.emplace_back([&submit, i] { submit(i); });
    } else {
      submit(i);
    }
  }
  for (auto& t : threads) {
    t.join();
  }
  std::vector<bool> pending(specs.size(), true);

  size_t remaining = specs.size();
  while (remaining != 0) {
    EXPECT_EQ(remaining, aioReader->pending());
    auto completed =
        test::async_base_test_lib_detail::TestUtil::readerWait(aioReader.get());
    size_t nrRead = completed.size();
    remaining -= nrRead;

    for (size_t i = 0; i < nrRead; i++) {
      int id = (reinterpret_cast<uintptr_t>(completed[i]) -
                reinterpret_cast<uintptr_t>(ops.get())) /
          sizeOf;
      EXPECT_GE(id, 0);
      EXPECT_LT(id, specs.size());
      EXPECT_TRUE(pending[id]);
      pending[id] = false;
      ssize_t res = ops[id].result();
      EXPECT_LE(0, res) << folly::errnoStr(-res);
      EXPECT_EQ(specs[id].size, res);
    }
  }
  EXPECT_EQ(specs.size(), aioReader->totalSubmits());

  EXPECT_EQ(aioReader->pending(), 0);
  for (size_t i = 0; i < pending.size(); i++) {
    EXPECT_FALSE(pending[i]);
  }
}

template <typename TAsync>
void testReadsQueued(
    const std::vector<TestSpec>& specs, folly::AsyncBase::PollMode pollMode) {
  size_t readerCapacity = std::max(specs.size() / 2, size_t(1));
  auto aioReader = getAIO<TAsync>(readerCapacity, pollMode);
  SKIP_IF(!aioReader) << "TAsync not available";
  folly::AsyncBaseQueue aioQueue(aioReader.get());
  std::unique_ptr<typename TAsync::Op[]> ops(
      new typename TAsync::Op[specs.size()]);
  uintptr_t sizeOf = sizeof(typename TAsync::Op);
  std::vector<TestUtil::ManagedBuffer> bufs;

  auto tempFile =
      folly::test::makeTemporaryFileWithRandomData(kDefaultFileSize);
  int fd = ::open(tempFile.path().c_str(), O_DIRECT | O_RDONLY);
  if (fd == -1) {
    fd = ::open(tempFile.path().c_str(), O_RDONLY);
  }
  SKIP_IF(fd == -1) << "Tempfile can't be opened: " << folly::errnoStr(errno);
  SCOPE_EXIT {
    fileops::close(fd);
  };
  for (size_t i = 0; i < specs.size(); i++) {
    bufs.push_back(TestUtil::allocateAligned(specs[i].size));
    ops[i].pread(fd, bufs[i].get(), specs[i].size, specs[i].start);
    aioQueue.submit(&ops[i]);
  }
  std::vector<bool> pending(specs.size(), true);

  size_t remaining = specs.size();
  while (remaining != 0) {
    if (remaining >= readerCapacity) {
      EXPECT_EQ(readerCapacity, aioReader->pending());
      EXPECT_EQ(remaining - readerCapacity, aioQueue.queued());
    } else {
      EXPECT_EQ(remaining, aioReader->pending());
      EXPECT_EQ(0, aioQueue.queued());
    }
    auto completed =
        test::async_base_test_lib_detail::TestUtil::readerWait(aioReader.get());
    size_t nrRead = completed.size();
    remaining -= nrRead;

    for (size_t i = 0; i < nrRead; i++) {
      int id = (reinterpret_cast<uintptr_t>(completed[i]) -
                reinterpret_cast<uintptr_t>(ops.get())) /
          sizeOf;
      EXPECT_GE(id, 0);
      EXPECT_LT(id, specs.size());
      EXPECT_TRUE(pending[id]);
      pending[id] = false;
      ssize_t res = ops[id].result();
      EXPECT_LE(0, res) << folly::errnoStr(-res);
      EXPECT_EQ(specs[id].size, res);
    }
  }
  EXPECT_EQ(specs.size(), aioReader->totalSubmits());
  EXPECT_EQ(aioReader->pending(), 0);
  EXPECT_EQ(aioQueue.queued(), 0);
  for (size_t i = 0; i < pending.size(); i++) {
    EXPECT_FALSE(pending[i]);
  }
}

template <typename TAsync>
void testReads(
    const std::vector<TestSpec>& specs, folly::AsyncBase::PollMode pollMode) {
  testReadsSerially<TAsync>(specs, pollMode);
  testReadsParallel<TAsync>(specs, pollMode, false);
  testReadsParallel<TAsync>(specs, pollMode, true);
  testReadsQueued<TAsync>(specs, pollMode);
}

template <typename T>
class AsyncTest : public ::testing::Test {};
TYPED_TEST_SUITE_P(AsyncTest);

TYPED_TEST_P(AsyncTest, ZeroAsyncDataNotPollable) {
  test::async_base_test_lib_detail::testReads<TypeParam>(
      {{0, 0}}, folly::AsyncBase::NOT_POLLABLE);
}

TYPED_TEST_P(AsyncTest, ZeroAsyncDataPollable) {
  test::async_base_test_lib_detail::testReads<TypeParam>(
      {{0, 0}}, folly::AsyncBase::POLLABLE);
}

TYPED_TEST_P(AsyncTest, SingleAsyncDataNotPollable) {
  test::async_base_test_lib_detail::testReads<TypeParam>(
      {{0, test::async_base_test_lib_detail::kODirectAlign}},
      folly::AsyncBase::NOT_POLLABLE);
  test::async_base_test_lib_detail::testReads<TypeParam>(
      {{0, test::async_base_test_lib_detail::kODirectAlign}},
      folly::AsyncBase::NOT_POLLABLE);
}

TYPED_TEST_P(AsyncTest, SingleAsyncDataPollable) {
  test::async_base_test_lib_detail::testReads<TypeParam>(
      {{0, test::async_base_test_lib_detail::kODirectAlign}},
      folly::AsyncBase::POLLABLE);
  test::async_base_test_lib_detail::testReads<TypeParam>(
      {{0, test::async_base_test_lib_detail::kODirectAlign}},
      folly::AsyncBase::POLLABLE);
}

TYPED_TEST_P(AsyncTest, MultipleAsyncDataNotPollable) {
  test::async_base_test_lib_detail::testReads<TypeParam>(
      {{test::async_base_test_lib_detail::kODirectAlign,
        2 * test::async_base_test_lib_detail::kODirectAlign},
       {test::async_base_test_lib_detail::kODirectAlign,
        2 * test::async_base_test_lib_detail::kODirectAlign},
       {test::async_base_test_lib_detail::kODirectAlign,
        4 * test::async_base_test_lib_detail::kODirectAlign}},
      folly::AsyncBase::NOT_POLLABLE);
  test::async_base_test_lib_detail::testReads<TypeParam>(
      {{test::async_base_test_lib_detail::kODirectAlign,
        2 * test::async_base_test_lib_detail::kODirectAlign},
       {test::async_base_test_lib_detail::kODirectAlign,
        2 * test::async_base_test_lib_detail::kODirectAlign},
       {test::async_base_test_lib_detail::kODirectAlign,
        4 * test::async_base_test_lib_detail::kODirectAlign}},
      folly::AsyncBase::NOT_POLLABLE);

  test::async_base_test_lib_detail::testReads<TypeParam>(
      {{0, 5 * 1024 * 1024},
       {test::async_base_test_lib_detail::kODirectAlign, 5 * 1024 * 1024}},
      folly::AsyncBase::NOT_POLLABLE);

  test::async_base_test_lib_detail::testReads<TypeParam>(
      {
          {test::async_base_test_lib_detail::kODirectAlign, 0},
          {test::async_base_test_lib_detail::kODirectAlign,
           test::async_base_test_lib_detail::kODirectAlign},
          {test::async_base_test_lib_detail::kODirectAlign,
           2 * test::async_base_test_lib_detail::kODirectAlign},
          {test::async_base_test_lib_detail::kODirectAlign,
           20 * test::async_base_test_lib_detail::kODirectAlign},
          {test::async_base_test_lib_detail::kODirectAlign, 1024 * 1024},
      },
      folly::AsyncBase::NOT_POLLABLE);
}

TYPED_TEST_P(AsyncTest, MultipleAsyncDataPollable) {
  test::async_base_test_lib_detail::testReads<TypeParam>(
      {{test::async_base_test_lib_detail::kODirectAlign,
        2 * test::async_base_test_lib_detail::kODirectAlign},
       {test::async_base_test_lib_detail::kODirectAlign,
        2 * test::async_base_test_lib_detail::kODirectAlign},
       {test::async_base_test_lib_detail::kODirectAlign,
        4 * test::async_base_test_lib_detail::kODirectAlign}},
      folly::AsyncBase::POLLABLE);
  test::async_base_test_lib_detail::testReads<TypeParam>(
      {{test::async_base_test_lib_detail::kODirectAlign,
        2 * test::async_base_test_lib_detail::kODirectAlign},
       {test::async_base_test_lib_detail::kODirectAlign,
        2 * test::async_base_test_lib_detail::kODirectAlign},
       {test::async_base_test_lib_detail::kODirectAlign,
        4 * test::async_base_test_lib_detail::kODirectAlign}},
      folly::AsyncBase::POLLABLE);

  test::async_base_test_lib_detail::testReads<TypeParam>(
      {{0, 5 * 1024 * 1024},
       {test::async_base_test_lib_detail::kODirectAlign, 5 * 1024 * 1024}},
      folly::AsyncBase::NOT_POLLABLE);

  test::async_base_test_lib_detail::testReads<TypeParam>(
      {
          {test::async_base_test_lib_detail::kODirectAlign, 0},
          {test::async_base_test_lib_detail::kODirectAlign,
           test::async_base_test_lib_detail::kODirectAlign},
          {test::async_base_test_lib_detail::kODirectAlign,
           2 * test::async_base_test_lib_detail::kODirectAlign},
          {test::async_base_test_lib_detail::kODirectAlign,
           20 * test::async_base_test_lib_detail::kODirectAlign},
          {test::async_base_test_lib_detail::kODirectAlign, 1024 * 1024},
      },
      folly::AsyncBase::NOT_POLLABLE);
}

TYPED_TEST_P(AsyncTest, ManyAsyncDataNotPollable) {
  {
    std::vector<test::async_base_test_lib_detail::TestSpec> v;
    for (int i = 0; i < 1000; i++) {
      v.push_back(
          {off_t(test::async_base_test_lib_detail::kODirectAlign * i),
           test::async_base_test_lib_detail::kODirectAlign});
    }
    test::async_base_test_lib_detail::testReads<TypeParam>(
        v, folly::AsyncBase::NOT_POLLABLE);
  }
}

TYPED_TEST_P(AsyncTest, ManyAsyncDataPollable) {
  {
    std::vector<test::async_base_test_lib_detail::TestSpec> v;
    for (int i = 0; i < 1000; i++) {
      v.push_back(
          {off_t(test::async_base_test_lib_detail::kODirectAlign * i),
           test::async_base_test_lib_detail::kODirectAlign});
    }
    test::async_base_test_lib_detail::testReads<TypeParam>(
        v, folly::AsyncBase::POLLABLE);
  }
}

TYPED_TEST_P(AsyncTest, NonBlockingWait) {
  TypeParam aioReader(1, folly::AsyncBase::NOT_POLLABLE);
  typename TypeParam::Op op;
  auto tempFile =
      folly::test::makeTemporaryFileWithRandomData(kDefaultFileSize);
  int fd = ::open(tempFile.path().c_str(), O_DIRECT | O_RDONLY);
  if (fd == -1) {
    fd = ::open(tempFile.path().c_str(), O_RDONLY);
  }
  SKIP_IF(fd == -1) << "Tempfile can't be opened: " << folly::errnoStr(errno);
  SCOPE_EXIT {
    fileops::close(fd);
  };
  size_t size = 2 * test::async_base_test_lib_detail::kODirectAlign;
  auto buf = test::async_base_test_lib_detail::TestUtil::allocateAligned(size);
  op.pread(fd, buf.get(), size, 0);
  aioReader.submit(&op);
  EXPECT_EQ(aioReader.pending(), 1);

  folly::Range<folly::AsyncBase::Op**> completed;
  while (completed.empty()) {
    // poll without blocking until the read request completes.
    completed = aioReader.wait(0);
  }
  EXPECT_EQ(completed.size(), 1);

  EXPECT_TRUE(completed[0] == &op);
  ssize_t res = op.result();
  EXPECT_LE(0, res) << folly::errnoStr(-res);
  EXPECT_EQ(size, res);
  EXPECT_EQ(aioReader.pending(), 0);
}

TYPED_TEST_P(AsyncTest, Cancel) {
  constexpr size_t kNumOpsBatch1 = 10;
  constexpr size_t kNumOpsBatch2 = 10;

  TypeParam aioReader(
      kNumOpsBatch1 + kNumOpsBatch2, folly::AsyncBase::NOT_POLLABLE);
  auto tempFile =
      folly::test::makeTemporaryFileWithRandomData(kDefaultFileSize);
  int fd = ::open(tempFile.path().c_str(), O_DIRECT | O_RDONLY);
  if (fd == -1) {
    fd = ::open(tempFile.path().c_str(), O_RDONLY);
  }
  SKIP_IF(fd == -1) << "Tempfile can't be opened: " << folly::errnoStr(errno);
  SCOPE_EXIT {
    fileops::close(fd);
  };

  size_t completed = 0;

  std::vector<std::unique_ptr<folly::AsyncBase::Op>> ops;
  std::vector<test::async_base_test_lib_detail::TestUtil::ManagedBuffer> bufs;
  const auto schedule = [&](size_t n) {
    for (size_t i = 0; i < n; ++i) {
      const size_t size = 2 * test::async_base_test_lib_detail::kODirectAlign;
      bufs.push_back(
          test::async_base_test_lib_detail::TestUtil::allocateAligned(size));

      ops.push_back(std::make_unique<typename TypeParam::Op>());
      auto& op = *ops.back();

      op.setNotificationCallback([&](folly::AsyncBaseOp*) { ++completed; });
      op.pread(fd, bufs.back().get(), size, 0);
      aioReader.submit(&op);
    }
  };

  // Mix completed and canceled operations for this test.
  // In order to achieve that, schedule in two batches and do partial
  // wait() after the first one.

  schedule(kNumOpsBatch1);
  EXPECT_EQ(aioReader.pending(), kNumOpsBatch1);
  EXPECT_EQ(completed, 0);

  auto result = aioReader.wait(1);
  EXPECT_GE(result.size(), 1);
  EXPECT_EQ(completed, result.size());
  EXPECT_EQ(aioReader.pending(), kNumOpsBatch1 - result.size());

  schedule(kNumOpsBatch2);
  EXPECT_EQ(aioReader.pending(), ops.size() - result.size());
  EXPECT_EQ(completed, result.size());

  auto canceled = aioReader.cancel();
  EXPECT_EQ(canceled.size(), ops.size() - result.size());
  EXPECT_EQ(aioReader.pending(), 0);
  EXPECT_EQ(completed, result.size());

  size_t foundCompleted = 0;
  for (auto& op : ops) {
    if (op->state() == folly::AsyncBaseOp::State::COMPLETED) {
      ++foundCompleted;
    } else {
      EXPECT_TRUE(op->state() == folly::AsyncBaseOp::State::CANCELED) << *op;
    }
  }
  EXPECT_EQ(foundCompleted, completed);
}

// batch tests
template <typename T>
class AsyncBatchTest : public ::testing::Test {};
TYPED_TEST_SUITE_P(AsyncBatchTest);

TYPED_TEST_P(AsyncBatchTest, BatchRead) {
  TypeParam aioReader;
  auto tempFile =
      folly::test::makeTemporaryFileWithRandomData(kDefaultFileSize);
  int fd = ::open(tempFile.path().c_str(), O_DIRECT | O_RDONLY);
  if (fd == -1) {
    fd = ::open(tempFile.path().c_str(), O_RDONLY);
  }
  SKIP_IF(fd == -1) << "Tempfile can't be opened: " << folly::errnoStr(errno);
  SCOPE_EXIT {
    fileops::close(fd);
  };

  using OpPtr = folly::AsyncBaseOp*;
  std::unique_ptr<typename TypeParam::Op[]> ops(
      new typename TypeParam::Op[kBatchNumEntries]);

  std::unique_ptr<OpPtr[]> opPtrs(new OpPtr[kBatchNumEntries]);
  std::vector<folly::test::async_base_test_lib_detail::TestUtil::ManagedBuffer>
      bufs;

  bufs.reserve(kBatchNumEntries);

  size_t completed = 0;

  for (size_t i = 0; i < kBatchNumEntries; i++) {
    bufs.push_back(
        folly::test::async_base_test_lib_detail::TestUtil::allocateAligned(
            kBatchBlockSize));
    auto& op = ops[i];
    opPtrs[i] = &op;

    op.setNotificationCallback([&](folly::AsyncBaseOp*) { ++completed; });
    op.pread(fd, bufs[i].get(), kBatchBlockSize, i * kBatchBlockSize);
  }
  aioReader.submit(
      Range<AsyncBase::Op**>(opPtrs.get(), opPtrs.get() + kBatchNumEntries));
  aioReader.wait(kBatchNumEntries);
  CHECK_EQ(completed, kBatchNumEntries);
}

} // namespace async_base_test_lib_detail
} // namespace test
} // namespace folly
