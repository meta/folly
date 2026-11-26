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

#include <folly/Benchmark.h>
#include <folly/FileUtil.h>
#include <folly/io/async/AsyncIO.h>
#include <folly/io/async/IoUring.h>
#include <folly/io/async/test/AsyncBaseTestLib.h>
#include <folly/portability/GFlags.h>
#include <folly/testing/TestUtil.h>

namespace {

static constexpr size_t kBlockSize = 4096;
// we cannot register more than UIO_MAXIOV iovs
// we can create bigger buffers and split them
static constexpr size_t kNumBlocks = UIO_MAXIOV;

static folly::test::TemporaryFile& getTempFile(size_t num) {
  CHECK_LE(num, kNumBlocks);

  static auto sTempFile =
      folly::test::makeTemporaryFileWithRandomData(kNumBlocks * kBlockSize);

  return sTempFile;
}

template <typename OP>
struct BenchmarkData {
  BenchmarkData(size_t num, size_t& c) : numEntries(num), completed(c) {
    fd = folly::fileops::open(
        getTempFile(num).path().c_str(), O_DIRECT | O_RDONLY);
    if (fd == -1) {
      fd = folly::fileops::open(getTempFile(num).path().c_str(), O_RDONLY);
    }
    CHECK_GE(fd, 0);
    ops.reserve(numEntries);
    bufs.reserve(numEntries);
    for (size_t i = 0; i < numEntries; i++) {
      bufs.push_back(
          folly::test::async_base_test_lib_detail::TestUtil::allocateAligned(
              kBlockSize));
    }
  }

  ~BenchmarkData() { folly::fileops::close(fd); }

  void reset(bool useRegisteredBuffers) {
    ops.clear();
    for (size_t i = 0; i < numEntries; i++) {
      ops.push_back(std::make_unique<OP>());
      auto& op = *ops.back();
      op.setNotificationCallback([&](folly::AsyncBaseOp*) { ++completed; });
      if (useRegisteredBuffers) {
        op.pread(fd, bufs[i].get(), kBlockSize, i * kBlockSize, i);
      } else {
        op.pread(fd, bufs[i].get(), kBlockSize, i * kBlockSize);
      }
    }
  }

  std::vector<std::unique_ptr<folly::AsyncBase::Op>> ops;
  std::vector<folly::test::async_base_test_lib_detail::TestUtil::ManagedBuffer>
      bufs;
  size_t numEntries;
  size_t& completed;
  int fd{-1};
};

template <typename TAsync>
void runTAsyncIOTest(
    unsigned int iters,
    size_t numEntries,
    size_t batchSize,
    bool persist,
    bool useRegisteredBuffers) {
  folly::BenchmarkSuspender suspender;
  std::vector<folly::AsyncBase::Op*> ops;
  ops.reserve(batchSize);
  size_t completed = 0;
  BenchmarkData<typename TAsync::Op> bmData(numEntries, completed);
  std::unique_ptr<TAsync> aio(
      persist
          ? new TAsync(numEntries, folly::AsyncBase::NOT_POLLABLE, batchSize)
          : nullptr);
  if (aio) {
    aio->register_buffers(bmData.bufs);
  }
  suspender.dismiss();
  for (unsigned iter = 0; iter < iters; iter++) {
    if (!persist) {
      aio.reset(
          new TAsync(numEntries, folly::AsyncBase::NOT_POLLABLE, batchSize));
      if (useRegisteredBuffers) {
        aio->register_buffers(bmData.bufs);
      }
    }
    completed = 0;
    bmData.reset(useRegisteredBuffers);
    size_t num = 0;
    for (size_t i = 0; i < numEntries; i++) {
      ops.push_back(bmData.ops[i].get());
      if (++num == batchSize) {
        num = 0;
        aio->submit(folly::Range(ops.data(), ops.data() + ops.size()));
        ops.clear();
      }
    }
    if (num) {
      aio->submit(folly::Range(ops.data(), ops.data() + ops.size()));
      ops.clear();
    }
    aio->wait(numEntries);
    CHECK_EQ(completed, numEntries);
    for (size_t i = 0; i < numEntries; i++) {
      CHECK_EQ(bmData.ops[i]->result(), kBlockSize);
    }
    if (!persist) {
      aio.reset();
    }
  }
  aio.reset();
  suspender.rehire();
}

void runAsyncIOTest(
    unsigned int iters, size_t numEntries, size_t batchSize, bool persist) {
  class BatchAsyncIO : public folly::AsyncIO {
   public:
    BatchAsyncIO(size_t capacity, PollMode pollMode, size_t /*unused*/)
        : folly::AsyncIO(capacity, pollMode) {}
    void register_buffers(
        const std::vector<folly::test::async_base_test_lib_detail::TestUtil::
                              ManagedBuffer>&) {}
  };
  runTAsyncIOTest<BatchAsyncIO>(iters, numEntries, batchSize, persist, false);
}

void runIOUringTest(
    unsigned int iters,
    size_t numEntries,
    size_t batchSize,
    bool persist,
    bool useRegisteredBuffers = false) {
  class BatchIoUring : public folly::IoUring {
   public:
    BatchIoUring(size_t capacity, PollMode pollMode, size_t batchSize)
        : folly::IoUring(capacity, pollMode, batchSize) {}
    void register_buffers(
        const std::vector<
            folly::test::async_base_test_lib_detail::TestUtil::ManagedBuffer>&
            bufs) {
      std::vector<struct iovec> iovs(bufs.size());
      for (size_t i = 0; i < bufs.size(); i++) {
        iovs[i].iov_base = bufs[i].get();
        iovs[i].iov_len = kBlockSize;
      }

      auto ret = folly::IoUring::register_buffers(iovs.data(), iovs.size());
      CHECK_EQ(ret, 0);
    }
  };

  runTAsyncIOTest<BatchIoUring>(
      iters, numEntries, batchSize, persist, useRegisteredBuffers);
}

} // namespace
BENCHMARK_DRAW_LINE();
BENCHMARK_NAMED_PARAM(
    runAsyncIOTest, async_io_no_batching_no_per, 1024, 1, false)
BENCHMARK_RELATIVE_NAMED_PARAM(
    runAsyncIOTest, async_io_batching_64_no_per, 1024, 64, false)
BENCHMARK_RELATIVE_NAMED_PARAM(
    runAsyncIOTest, async_io_batching_256_no_per, 1024, 256, false)
BENCHMARK_RELATIVE_NAMED_PARAM(
    runAsyncIOTest, async_io_no_batching_per, 1024, 1, true)
BENCHMARK_RELATIVE_NAMED_PARAM(
    runAsyncIOTest, async_io_batching_64_per, 1024, 64, true)
BENCHMARK_RELATIVE_NAMED_PARAM(
    runAsyncIOTest, async_io_batching_256_per, 1024, 256, true)
BENCHMARK_DRAW_LINE();
BENCHMARK_RELATIVE_NAMED_PARAM(
    runIOUringTest, io_uring_no_batching_no_per, 1024, 1, false)
BENCHMARK_RELATIVE_NAMED_PARAM(
    runIOUringTest, io_uring_batching_64_no_per, 1024, 64, false)
BENCHMARK_RELATIVE_NAMED_PARAM(
    runIOUringTest, io_uring_batching_256_no_per, 1024, 256, false)
BENCHMARK_RELATIVE_NAMED_PARAM(
    runIOUringTest, io_uring_no_batching_per, 1024, 1, true)
BENCHMARK_RELATIVE_NAMED_PARAM(
    runIOUringTest, io_uring_batching_64_per, 1024, 64, true)
BENCHMARK_RELATIVE_NAMED_PARAM(
    runIOUringTest, io_uring_batching_256_per, 1024, 256, true)
BENCHMARK_DRAW_LINE();
BENCHMARK_RELATIVE_NAMED_PARAM(
    runIOUringTest, io_uring_no_batching_no_per_reg, 1024, 1, false, true)
BENCHMARK_RELATIVE_NAMED_PARAM(
    runIOUringTest, io_uring_batching_64_no_per_reg, 1024, 64, false, true)
BENCHMARK_RELATIVE_NAMED_PARAM(
    runIOUringTest, io_uring_batching_256_no_per_reg, 1024, 256, false, true)
BENCHMARK_RELATIVE_NAMED_PARAM(
    runIOUringTest, io_uring_no_batching_per_reg, 1024, 1, true, true)
BENCHMARK_RELATIVE_NAMED_PARAM(
    runIOUringTest, io_uring_batching_64_per_reg, 1024, 64, true, true)
BENCHMARK_RELATIVE_NAMED_PARAM(
    runIOUringTest, io_uring_batching_256_per_reg, 1024, 256, true, true)
BENCHMARK_DRAW_LINE();

int main(int argc, char** argv) {
  getTempFile(kNumBlocks);
  folly::gflags::ParseCommandLineFlags(&argc, &argv, true);
  folly::runBenchmarks();
}

/*
./io_benchmark --bm_min_iters=100
============================================================================
folly/experimental/io/test/IOBenchmark.cpp      relative  time/iter  iters/s
============================================================================
----------------------------------------------------------------------------
runAsyncIOTest(async_io_no_batching_no_per)                 45.33ms    22.06
runAsyncIOTest(async_io_batching_64_no_per)      102.48%    44.24ms    22.61
runAsyncIOTest(async_io_batching_256_no_per)      94.30%    48.08ms    20.80
runAsyncIOTest(async_io_no_batching_per)         173.66%    26.11ms    38.31
runAsyncIOTest(async_io_batching_64_per)         179.94%    25.19ms    39.69
runAsyncIOTest(async_io_batching_256_per)        171.69%    26.40ms    37.87
----------------------------------------------------------------------------
runIOUringTest(io_uring_no_batching_no_per)      180.66%    25.09ms    39.85
runIOUringTest(io_uring_batching_64_no_per)      176.16%    25.74ms    38.86
runIOUringTest(io_uring_batching_256_no_per)     178.45%    25.40ms    39.36
runIOUringTest(io_uring_no_batching_per)         177.59%    25.53ms    39.17
runIOUringTest(io_uring_batching_64_per)         178.06%    25.46ms    39.28
runIOUringTest(io_uring_batching_256_per)        178.81%    25.35ms    39.44
----------------------------------------------------------------------------
runIOUringTest(io_uring_no_batching_no_per_reg)  121.76%    37.23ms    26.86
runIOUringTest(io_uring_batching_64_no_per_reg)  119.86%    37.82ms    26.44
runIOUringTest(io_uring_batching_256_no_per_reg  127.17%    35.65ms    28.05
runIOUringTest(io_uring_no_batching_per_reg)     178.60%    25.38ms    39.39
runIOUringTest(io_uring_batching_64_per_reg)     179.33%    25.28ms    39.56
runIOUringTest(io_uring_batching_256_per_reg)    178.69%    25.37ms    39.42
----------------------------------------------------------------------------
============================================================================
*/
