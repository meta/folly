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

#include <folly/init/Init.h>
#include <folly/io/async/IoUring.h>
#include <folly/io/async/test/AsyncBaseTestLib.h>

using folly::IoUring;

namespace folly {
namespace test {
namespace async_base_test_lib_detail {
REGISTER_TYPED_TEST_SUITE_P(
    AsyncTest,
    ZeroAsyncDataNotPollable,
    ZeroAsyncDataPollable,
    SingleAsyncDataNotPollable,
    SingleAsyncDataPollable,
    MultipleAsyncDataNotPollable,
    MultipleAsyncDataPollable,
    ManyAsyncDataNotPollable,
    ManyAsyncDataPollable,
    NonBlockingWait,
    Cancel);

REGISTER_TYPED_TEST_SUITE_P(AsyncBatchTest, BatchRead);

INSTANTIATE_TYPED_TEST_SUITE_P(AsyncTest, AsyncTest, IoUring);

class BatchIoUring : public IoUring {
 public:
  static constexpr size_t kMaxSubmit = 64;
  BatchIoUring()
      : IoUring(kBatchNumEntries, folly::AsyncBase::NOT_POLLABLE, kMaxSubmit) {}
};
INSTANTIATE_TYPED_TEST_SUITE_P(AsyncBatchTest, AsyncBatchTest, BatchIoUring);

TEST(IoUringTest, RegisteredBuffers) {
  constexpr size_t kNumEntries = 2;
  constexpr size_t kBufSize = 4096;
  auto ioUring = getAIO<IoUring>(kNumEntries, folly::AsyncBase::NOT_POLLABLE);
  SKIP_IF(!ioUring) << "IOUring not available";

  auto tempFile =
      folly::test::makeTemporaryFileWithRandomData(kDefaultFileSize);
  int fd = ::open(tempFile.path().c_str(), O_DIRECT | O_RDWR);
  if (fd == -1) {
    fd = ::open(tempFile.path().c_str(), O_RDWR);
  }
  SKIP_IF(fd == -1) << "Tempfile can't be opened: " << folly::errnoStr(errno);
  SCOPE_EXIT {
    fileops::close(fd);
  };

  folly::test::async_base_test_lib_detail::TestUtil::ManagedBuffer
      regFdWriteBuf =
          folly::test::async_base_test_lib_detail::TestUtil::allocateAligned(
              kBufSize),
      readBuf =
          folly::test::async_base_test_lib_detail::TestUtil::allocateAligned(
              kBufSize),
      regFdReadBuf =
          folly::test::async_base_test_lib_detail::TestUtil::allocateAligned(
              kBufSize);

  ::memset(regFdWriteBuf.get(), '0', kBufSize);
  ::memset(readBuf.get(), 'A', kBufSize);
  ::memset(regFdReadBuf.get(), 'Z', kBufSize);

  struct iovec iov[2];
  iov[0].iov_base = regFdWriteBuf.get();
  iov[0].iov_len = kBufSize;
  iov[1].iov_base = regFdReadBuf.get();
  iov[1].iov_len = kBufSize;

  CHECK_EQ(ioUring->register_buffers(iov, 2), 0);

  IoUring::Op regFdWriteOp, readOp, regFdReadOp;
  size_t completed = 0;

  regFdWriteOp.setNotificationCallback([&](folly::AsyncBaseOp*) {
    ++completed;
  });
  regFdWriteOp.pwrite(fd, regFdWriteBuf.get(), kBufSize, 0, 0 /*buf_index*/);

  readOp.setNotificationCallback([&](folly::AsyncBaseOp*) { ++completed; });
  readOp.pread(fd, readBuf.get(), kBufSize, 0);
  regFdReadOp.setNotificationCallback([&](folly::AsyncBaseOp*) {
    ++completed;
  });
  regFdReadOp.pread(fd, regFdReadBuf.get(), kBufSize, 0, 1 /*buf_index*/);

  // write
  ioUring->submit(&regFdWriteOp);
  ioUring->wait(1);
  CHECK_EQ(completed, 1);
  CHECK_EQ(regFdWriteOp.result(), kBufSize);

  // read - both via regular and registered buffers
  completed = 0;
  ioUring->submit(&readOp);
  ioUring->submit(&regFdReadOp);

  ioUring->wait(kNumEntries);

  CHECK_EQ(completed, kNumEntries);
  CHECK_EQ(readOp.result(), kBufSize);
  CHECK_EQ(regFdReadOp.result(), kBufSize);

  // make sure we read the same data
  CHECK_EQ(::memcmp(readBuf.get(), regFdWriteBuf.get(), kBufSize), 0);
  CHECK_EQ(::memcmp(regFdReadBuf.get(), regFdWriteBuf.get(), kBufSize), 0);
}

} // namespace async_base_test_lib_detail
} // namespace test
} // namespace folly
