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

#include <folly/system/ThreadId.h>

#include <algorithm>
#include <array>
#include <thread>

#include <folly/Benchmark.h>
#include <folly/portability/GTest.h>
#include <folly/portability/Unistd.h>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace folly {
namespace detail {

uint64_t getOSThreadIDSlow();

} // namespace detail
} // namespace folly

namespace {

template <class F>
void testUnique(F&& f) {
  auto thisThreadID = f();
  uint64_t otherThreadID;
  std::thread otherThread{[&] { otherThreadID = f(); }};
  otherThread.join();
  EXPECT_NE(thisThreadID, otherThreadID);
}

} // namespace

TEST(ThreadId, getUniqueThreadIdStable) {
  const auto id = folly::get_unique_thread_id();
  EXPECT_NE(0, id);
  EXPECT_EQ(id, folly::get_unique_thread_id());
}

TEST(ThreadId, getUniqueThreadIdBacking) {
  const auto check = [](bool backingFirst) {
    std::thread thread{[=] {
      const auto id = backingFirst
          ? folly::detail::get_unique_thread_id_slow()
          : folly::get_unique_thread_id();
      EXPECT_NE(0, id);
      EXPECT_EQ(id, folly::detail::get_unique_thread_id_slow());
      EXPECT_EQ(id, folly::get_unique_thread_id());
      EXPECT_EQ(id, folly::detail::get_unique_thread_id_slow());
    }};
    thread.join();
  };
  check(false);
  check(true);
}

TEST(ThreadId, getUniqueThreadIdConcurrent) {
  std::array<uint64_t, 17> ids{};
  ids[0] = folly::get_unique_thread_id();
  std::array<std::thread, 16> threads;
  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i] = std::thread{[&, i] {
      ids[i + 1] = folly::get_unique_thread_id();
      EXPECT_NE(0, ids[i + 1]);
      EXPECT_EQ(ids[i + 1], folly::get_unique_thread_id());
    }};
  }
  for (auto& thread : threads) {
    thread.join();
  }
  std::sort(ids.begin(), ids.end());
  EXPECT_EQ(ids.end(), std::adjacent_find(ids.begin(), ids.end()));
}

TEST(ThreadId, getUniqueThreadIdAfterExit) {
  std::array<uint64_t, 33> ids{};
  ids[0] = folly::get_unique_thread_id();
  for (size_t i = 1; i < ids.size(); ++i) {
    std::thread thread{[&, i] { ids[i] = folly::get_unique_thread_id(); }};
    thread.join();
    EXPECT_NE(0, ids[i]);
  }
  std::sort(ids.begin(), ids.end());
  EXPECT_EQ(ids.end(), std::adjacent_find(ids.begin(), ids.end()));
}

TEST(ThreadId, getCurrentID) {
  testUnique(folly::getCurrentThreadID);
}

TEST(ThreadId, getOSThreadID) {
  testUnique(folly::getOSThreadID);
}

TEST(ThreadId, getOSThreadIDCache) {
  auto thisThreadID = folly::getOSThreadID();
  ASSERT_EQ(thisThreadID, folly::detail::getOSThreadIDSlow());

#ifndef _WIN32
  auto pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) { // Child.
    if (folly::getOSThreadID() != folly::detail::getOSThreadIDSlow()) {
      _exit(1);
    }
    if (folly::getOSThreadID() == thisThreadID) {
      _exit(2);
    }
    _exit(0);
  } else { // Parent.
    int status;
    ASSERT_EQ(pid, waitpid(pid, &status, 0));
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(0, WEXITSTATUS(status));
  }
#endif
}

#define BENCHMARK_FUN(NAME, F)              \
  BENCHMARK(NAME, iters) {                  \
    while (iters--) {                       \
      folly::doNotOptimizeAway(folly::F()); \
    }                                       \
  }

BENCHMARK_FUN(get_unique_thread_id, get_unique_thread_id)
BENCHMARK_FUN(getCurrentThreadID, getCurrentThreadID)
BENCHMARK_FUN(getOSThreadID, getOSThreadID)
BENCHMARK_FUN(getOSThreadIDSlow, detail::getOSThreadIDSlow)

int main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  folly::gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  int ret = RUN_ALL_TESTS();
  if (ret == 0) {
    folly::runBenchmarksOnFlag();
  }
  return ret;
}

#if 0
$ buck2 run @mode/opt folly/system/test:thread_id_test -- --benchmark --bm_slice_usec 500000
============================================================================
fbcode/folly/system/test/ThreadIdTest.cpp     relative  time/iter   iters/s
============================================================================
getCurrentThreadID                                          3.12ns   320.39M
getOSThreadID                                               2.14ns   466.64M
getOSThreadIDSlow                                         275.19ns     3.63M
============================================================================
#endif
