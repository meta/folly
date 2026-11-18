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
#include <folly/CancellationToken.h>
#include <folly/channels/Channel.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Task.h>
#include <folly/init/Init.h>
#include <folly/io/async/Request.h>

namespace folly::channels {
namespace {

coro::Task<size_t> consumeBufferedValues(Receiver<size_t>& receiver) {
  size_t receivedCount = 0;
  while (auto value = co_await receiver.next()) {
    folly::doNotOptimizeAway(*value);
    ++receivedCount;
  }
  co_return receivedCount;
}

void bufferedChannelReads(unsigned int iterations, size_t batchSize) {
  BenchmarkSuspender setup;
  RequestContextScopeGuard requestContext;
  CancellationSource cancellationSource;
  for (unsigned int iteration = 0; iteration < iterations; ++iteration) {
    auto [receiver, sender] = Channel<size_t>::create();
    for (size_t value = 0; value < batchSize; ++value) {
      sender.write(value);
    }
    std::move(sender).close();
    setup.dismiss();
    auto receivedCount = coro::blockingWait(
        coro::co_withCancellation(
            cancellationSource.getToken(), consumeBufferedValues(receiver)));
    folly::doNotOptimizeAway(receivedCount);
    setup.rehire();
  }
}

BENCHMARK_NAMED_PARAM(bufferedChannelReads, batch1, 1);
BENCHMARK_NAMED_PARAM(bufferedChannelReads, batch32, 32);
BENCHMARK_NAMED_PARAM(bufferedChannelReads, batch1024, 1024);

} // namespace
} // namespace folly::channels

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  folly::runBenchmarks();
}
