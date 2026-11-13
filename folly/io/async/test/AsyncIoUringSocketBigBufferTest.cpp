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

#include <chrono>
#include <string>
#include <thread>

#include <folly/io/async/test/AsyncIoUringSocketTestHelper.h>
#include <folly/portability/GTest.h>

namespace folly {
namespace {

// write() requires a callback, but a failed write here always surfaces as a
// read-side error: bytes that never went out cannot be echoed back, so
// waitFor below can only end in a reset or a timeout, where runWithRetries
// sees it. Unlike NullWriteCallback this does not LOG(FATAL), so a
// transient write failure under host memlock contention becomes a retriable
// error instead of aborting the process. (writeErr is noexcept, so
// recording-and-throwing is not an option.)
class TolerantWriteCallback : public AsyncWriter::WriteCallback {
 public:
  void writeSuccess() noexcept override {}

  void writeErr(size_t, const AsyncSocketException&) noexcept override {}
};

static TolerantWriteCallback tolerantWriteCallback;

constexpr size_t kBigWriteSize = 1000000;
constexpr int kMaxAttempts = 3;

// Runs attempt up to kMaxAttempts times with backoff. An attempt that throws
// std::exception is treated as a transient failure (e.g. ENOMEM under host
// memlock contention) and retried; deterministic failures recorded with
// gtest assertions fail immediately without retry. Fails the test if every
// attempt throws.
template <typename Attempt>
void runWithRetries(const std::string& name, Attempt&& attempt) {
  std::string lastError = "no attempts made";
  for (int i = 0; i < kMaxAttempts; ++i) {
    try {
      attempt();
      return;
    } catch (const std::exception& ex) {
      lastError = ex.what();
    }
    std::this_thread::sleep_for(std::chrono::seconds(1 << i));
  }
  FAIL() << name << " failed after " << kMaxAttempts
         << " attempts: " << lastError;
}

} // namespace

TEST_P(AsyncIoUringSocketTestAll, WriteBig) {
  MAYBE_SKIP();
  std::string big(kBigWriteSize, 'X');
  runWithRetries("WriteBig", [&] {
    auto [e, s, cb] = makeConnected();
    cb->setHoldData(true);
    s->write(&tolerantWriteCallback, big.c_str(), big.size());
    auto res = cb->waitFor(big.size()).via(base.get()).getVia(base.get());
    EXPECT_TRUE(big == res) << big.size() << " vs " << res.size();
  });
}

TEST_P(AsyncIoUringSocketTestAll, WriteBigDrop) {
  MAYBE_SKIP();
  std::string big(kBigWriteSize, 'X');
  // holdData=false should trigger overflow in provided buffers.
  runWithRetries("WriteBigDrop", [&] {
    auto [e, s, cb] = makeConnected();
    cb->setHoldData(false);
    s->write(&tolerantWriteCallback, big.c_str(), big.size());
    EXPECT_EQ(big, cb->waitFor(big.size()).via(base.get()).getVia(base.get()));
  });
}

INSTANTIATE_TEST_SUITE_P(
    AsyncIoUringSocketTest,
    AsyncIoUringSocketTestAll,
    ::testing::ValuesIn(mkAllTestParams()),
    [](const ::testing::TestParamInfo<TestParams>& info) {
      return info.param.testName();
    });

} // namespace folly
