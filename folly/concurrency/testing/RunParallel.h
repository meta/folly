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

#include <cstddef>
#include <latch>
#include <thread>
#include <vector>

namespace folly {
namespace test {

/*
 * Run a function simultaneously in a number of different threads.
 *
 * The function will be passed the index number of the thread it is running in.
 * This function makes an attempt to synchronize the start of the threads as
 * best as possible.  It waits for all threads to be allocated and started
 * before invoking the function.
 */
template <class Function>
void run_parallel(std::size_t numThreads, Function function) {
  std::vector<std::thread> threads;
  threads.reserve(numThreads);

  // Latches used to synchronize all threads to try and start them
  // as close to the same time as possible.
  std::latch ready(numThreads);
  std::latch go(1);

  auto worker = [&](std::size_t threadIndex) {
    // Signal that we are ready, then wait for the signal to start.
    // The purpose of this is to try and make sure all threads start
    // as close to the same time as possible.
    ready.count_down();
    go.wait();

    function(threadIndex);
  };

  // Start all of the threads
  for (std::size_t threadIndex = 0; threadIndex < numThreads; ++threadIndex) {
    threads.emplace_back([threadIndex, &worker]() { worker(threadIndex); });
  }

  // Wait for all threads to become ready, then signal them to go
  ready.wait();
  go.count_down();

  // Wait for all threads to finish
  for (auto& thread : threads) {
    thread.join();
  }
}

} // namespace test
} // namespace folly
