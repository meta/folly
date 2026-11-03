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

#include <folly/debugging/symbolizer/test/SignalHandlerTest.h>

#include <folly/debugging/symbolizer/SignalHandler.h>

#include <folly/CPortability.h>
#include <folly/FileUtil.h>
#include <folly/Portability.h>
#include <folly/Range.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Task.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/portability/GTest.h>

#include <glog/logging.h>

#include <array>
#include <memory>
#include <string>

namespace folly {
namespace symbolizer {
namespace test {

namespace {

std::string expectedSyncFrames(const char* symbolized) {
  return kIsSanitizeThread
      ? "(.*\\[0x[0-9a-f]+\\]\n)+"
        "\\(safe mode, symbolizer not available\\)\n"
        "(.*\n)*"
      : symbolized;
}

std::string expectedAsyncFrames(const char* symbolized) {
  // Raw async frames bypass buffering and precede the async metadata.
  return kIsSanitizeThread
      ? "\\(safe mode, symbolizer not available\\)\n"
      : symbolized;
}

void print(StringPiece sp) {
  writeFull(STDERR_FILENO, sp.data(), sp.size());
}

void callback1() {
  print("Callback1\n");
}

void callback2() {
  if (fatalSignalReceived()) {
    print("Callback2\n");
  }
}

[[noreturn]] FOLLY_NOINLINE void funcC() {
  LOG(FATAL) << "Die";
}

FOLLY_NOINLINE folly::coro::Task<void> co_funcB() {
  funcC();
  co_return;
}

FOLLY_NOINLINE folly::coro::Task<void> co_funcA() {
  co_await co_funcB();
}

} // namespace

TEST(SignalHandler, Simple) {
  addFatalSignalCallback(callback1);
  addFatalSignalCallback(callback2);
  installFatalSignalHandler();
  installFatalSignalCallbacks();

  EXPECT_FALSE(fatalSignalReceived());

  EXPECT_DEATH(
      failHard(),
      "^\\*\\*\\* Aborted at [0-9]+ \\(Unix time, try 'date -d @[0-9]+'\\) "
      "\\*\\*\\*\n"
      "\\*\\*\\* Signal 11 \\(SIGSEGV\\) \\(0x2a\\) received by PID [0-9]+ "
      "\\(pthread TID 0x[0-9a-f]+\\) \\(linux TID [0-9]+\\) "
      "\\(code: address not mapped to object\\), "
      "stack trace: \\*\\*\\*\n" +
          expectedSyncFrames(
              ".*\n"
              ".*    @ [0-9a-f]+.* folly::symbolizer::test::SignalHandler_Simple_Test"
              "::TestBody\\(\\).*\n"
              ".*\n"
              ".*    @ [0-9a-f]+.* main.*\n"
              ".*\n") +
          "Callback1\n"
          "Callback2\n"
          ".*");
}

TEST(SignalHandler, GenericSICode) {
  addFatalSignalCallback(callback1);
  addFatalSignalCallback(callback2);
  installFatalSignalHandler();
  installFatalSignalCallbacks();

  EXPECT_FALSE(fatalSignalReceived());

  EXPECT_DEATH(
      raise(SIGSEGV),
      "^\\*\\*\\* Aborted at [0-9]+ \\(Unix time, try 'date -d @[0-9]+'\\) "
      "\\*\\*\\*\n"
      "\\*\\*\\* Signal 11 \\(SIGSEGV\\) \\(0x[a-f0-9]+\\) received by PID [0-9]+ "
      "\\(pthread TID 0x[0-9a-f]+\\) \\(linux TID [0-9]+\\) \\(maybe from PID [0-9]+, UID [0-9]+\\) "
      "\\(code: sent by tkill or tgkill\\), "
      "stack trace: \\*\\*\\*\n" +
          expectedSyncFrames(
              ".*\n"
              ".*    @ [0-9a-f]+.* folly::symbolizer::test::SignalHandler_GenericSICode_Test"
              "::TestBody\\(\\).*\n"
              ".*\n"
              ".*    @ [0-9a-f]+.* main.*\n"
              ".*\n") +
          "Callback1\n"
          "Callback2\n"
          ".*");
}

TEST(SignalHandler, AsyncStackTraceSimple) {
  addFatalSignalCallback(callback1);
  addFatalSignalCallback(callback2);
  installFatalSignalHandler();
  installFatalSignalCallbacks();

  EXPECT_EXIT(
      folly::coro::blockingWait(co_funcA()),
      ::testing::KilledBySignal(SIGABRT),
      "\\*\\*\\* Aborted at [0-9]+ \\(Unix time, try 'date -d @[0-9]+'\\) "
      "\\*\\*\\*\n"
      "\\*\\*\\* Signal 6 \\(SIGABRT\\) \\(0x[0-9a-f]+\\) received by PID [0-9]+ "
      "\\(pthread TID 0x[0-9a-f]+\\) \\(linux TID [0-9]+\\) .*, "
      "stack trace: \\*\\*\\*\n" +
          expectedSyncFrames(
              ".*\n"
              ".*    @ [0-9a-f]+.* folly::symbolizer::test::SignalHandler"
              "_AsyncStackTraceSimple_Test::TestBody\\(\\).*\n"
              ".*\n"
              ".*    @ [0-9a-f]+.* main.*\n"
              ".*\n") +
          "\\*\\*\\* Check failure async stack trace: \\*\\*\\*\n"
          "\\*\\*\\* First async stack root.* \\*\\*\\*\n"
          "\\*\\*\\* First async stack frame pointer.* \\*\\*\\*\n" +
          expectedAsyncFrames(
              ".*\n"
              ".*    @ [0-9a-f]+.* folly::symbolizer::test::\\(anonymous namespace\\)"
              "::co_funcA.*\n"
              ".*\n") +
          "Callback1\n"
          "Callback2\n"
          ".*");
}

TEST(SignalHandler, AsyncStackTraceSimple2) {
  addFatalSignalCallback(callback1);
  addFatalSignalCallback(callback2);
  installFatalSignalHandler();
  installFatalSignalCallbacks();
  EXPECT_EXIT(
      [] {
        auto ex = std::make_unique<IOThreadPoolExecutor>(/* nThreads */ 1);
        auto fut = co_withExecutor(ex.get(), co_funcA()).start();
        fut.wait();
      }(),
      ::testing::KilledBySignal(SIGABRT),
      "\\*\\*\\* Aborted at [0-9]+ \\(Unix time, try 'date -d @[0-9]+'\\) "
      "\\*\\*\\*\n"
      "\\*\\*\\* Signal 6 \\(SIGABRT\\) \\(0x[0-9a-f]+\\) received by PID [0-9]+ "
      "\\(pthread TID 0x[0-9a-f]+\\) \\(linux TID [0-9]+\\) .*, "
      "stack trace: \\*\\*\\*\n"
      "(.*WARNING.*\n)?" +
          expectedSyncFrames(
              ".*\n"
              ".*    @ [0-9a-f]+.* folly::symbolizer::test::\\(anonymous namespace\\)"
              "::funcC\\(\\)\n"
              ".*\n") +
          "\\*\\*\\* Check failure async stack trace: \\*\\*\\*\n"
          "\\*\\*\\* First async stack root.* \\*\\*\\*\n"
          "\\*\\*\\* First async stack frame pointer.* \\*\\*\\*\n" +
          expectedAsyncFrames(
              ".*\n"
              ".*    @ [0-9a-f]+.* folly::symbolizer::test::\\(anonymous namespace\\)"
              "::co_funcA.*\n"
              ".*\n") +
          "Callback1\n"
          "Callback2\n"
          ".*");
}

} // namespace test
} // namespace symbolizer
} // namespace folly

// Can't use initFacebookLight since that would install its own signal handlers
// Can't use initFacebookNoSignals since we cannot depend on common
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  if (folly::kIsSanitizeThread) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    alignas(16) static std::array<char, 65536> stack{};
    stack_t ss{};
    ss.ss_sp = stack.data();
    ss.ss_size = stack.size();
    CHECK_EQ(0, sigaltstack(&ss, nullptr));
  }
  return RUN_ALL_TESTS();
}
