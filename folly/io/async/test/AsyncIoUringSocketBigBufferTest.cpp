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
#include <system_error>
#include <thread>

#include <folly/io/async/test/AsyncIoUringSocketTestHelper.h>
#include <folly/lang/Exception.h>
#include <folly/portability/GTest.h>
#include <folly/testing/TestUtil.h>

namespace folly {
namespace {

class TolerantWriteCallback : public AsyncWriter::WriteCallback {
 public:
  void writeSuccess() noexcept override {}

  void writeErr(size_t, const AsyncSocketException& error) noexcept override {
    if (writeError == 0) {
      writeError = error.getErrno();
    }
  }

  int writeError{0};
};

constexpr size_t kBigWriteSize = 1000000;
constexpr int kMaxAttempts = 3;

template <typename Attempt>
void runWithRetries(const std::string& name, Attempt&& attempt) {
  for (int i = 0; i < kMaxAttempts; ++i) {
    try {
      attempt();
      return;
    } catch (const std::system_error& error) {
      if (error.code() != std::errc::not_enough_memory) {
        throw;
      }
      if (i + 1 == kMaxAttempts) {
        test::report_resource_exhaustion(name, error.code(), error.what());
        throw;
      }
    }
    std::this_thread::sleep_for(std::chrono::seconds(1 << i));
  }
}

void throwWriteEnomem(
    const EchoTransport& echo, const TolerantWriteCallback& writer) {
  if (writer.writeError == ENOMEM || echo.writeError == ENOMEM) {
    throw_exception<std::system_error>(
        ENOMEM,
        std::generic_category(),
        writer.writeError == ENOMEM ? "client write" : "echo write");
  }
}

std::string waitForEcho(
    CollectCallback& callback,
    EventBase& base,
    size_t size,
    const EchoTransport& echo,
    const TolerantWriteCallback& writer) {
  try {
    return callback.waitFor(size).via(&base).getVia(&base);
  } catch (const AsyncSocketException& error) {
    if (error.getErrno() == ECONNRESET || error.getErrno() == EPIPE) {
      throwWriteEnomem(echo, writer);
    }
    throw;
  } catch (const FutureTimeout&) {
    throwWriteEnomem(echo, writer);
    throw;
  } catch (const BrokenPromise&) {
    throwWriteEnomem(echo, writer);
    throw;
  }
}

} // namespace

TEST_P(AsyncIoUringSocketTestAll, WriteBig) {
  MAYBE_SKIP();
  std::string big(kBigWriteSize, 'X');
  runWithRetries("WriteBig", [&] {
    TolerantWriteCallback writer;
    auto [e, s, cb] = makeConnected();
    cb->setHoldData(true);
    s->write(&writer, big.c_str(), big.size());
    auto res = waitForEcho(*cb, *base, big.size(), *e, writer);
    EXPECT_TRUE(big == res) << big.size() << " vs " << res.size();
  });
}

TEST_P(AsyncIoUringSocketTestAll, WriteBigDrop) {
  MAYBE_SKIP();
  std::string big(kBigWriteSize, 'X');
  // holdData=false should trigger overflow in provided buffers.
  runWithRetries("WriteBigDrop", [&] {
    TolerantWriteCallback writer;
    auto [e, s, cb] = makeConnected();
    cb->setHoldData(false);
    s->write(&writer, big.c_str(), big.size());
    EXPECT_EQ(big, waitForEcho(*cb, *base, big.size(), *e, writer));
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
