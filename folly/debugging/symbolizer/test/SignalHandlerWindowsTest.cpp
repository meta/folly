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

#include <folly/portability/GTest.h>

#ifdef _WIN32
#include <exception>
#include <stdexcept>

#include <folly/CPortability.h>
#include <folly/debugging/symbolizer/SignalHandler.h>
#include <folly/portability/GMock.h>
#include <folly/portability/Windows.h>

namespace folly {
namespace symbolizer {
namespace test {
namespace {

void installedCallbackFn() {
  static constexpr char kMarker[] = "WinCallbackFired\n";
  DWORD written = 0;
  ::WriteFile(
      ::GetStdHandle(STD_ERROR_HANDLE),
      kMarker,
      sizeof(kMarker) - 1,
      &written,
      nullptr);
}

FOLLY_NOINLINE void crashAvRead() {
  volatile char* p = nullptr;
  char c = *p;
  (void)c;
}

FOLLY_NOINLINE void crashAvWrite() {
  volatile char* p = nullptr;
  *p = 0;
}

FOLLY_NOINLINE void crashStackOverflow() {
  volatile char buf[1024];
  buf[0] = 0;
  crashStackOverflow();
  (void)buf[0];
}

// Throwing out of a noexcept function calls std::terminate, exercising the
// terminateHandler path (which the VEH filter never sees).
FOLLY_NOINLINE void throwFromNoexcept() noexcept {
  throw std::runtime_error("boom");
}

void setupAndCrash(void (*crashFn)()) {
  addFatalSignalCallback(&installedCallbackFn);
  installFatalSignalHandler();
  installFatalSignalCallbacks();
  crashFn();
}

} // namespace

TEST(SignalHandlerWindows, EmptySignalMaskAllowsLaterInstallation) {
  EXPECT_EXIT(
      {
        auto previousTerminate = std::get_terminate();
        installFatalSignalHandler(std::bitset<64>{});
        if (std::get_terminate() != previousTerminate) {
          ::ExitProcess(1);
        }
        installFatalSignalHandler();
        if (std::get_terminate() == previousTerminate) {
          ::ExitProcess(2);
        }
        ::ExitProcess(0);
      },
      ::testing::ExitedWithCode(0),
      "");
}

TEST(SignalHandlerWindows, AccessViolationRead) {
  EXPECT_DEATH(
      setupAndCrash(&crashAvRead),
      ::testing::AllOf(
          ::testing::HasSubstr(
              "*** Fatal exception 0xc0000005 (EXCEPTION_ACCESS_VIOLATION)"),
          ::testing::HasSubstr("read at 0x0"),
          ::testing::HasSubstr("faulting instruction at 0x"),
          ::testing::HasSubstr(
              "signal_handler_windows_test.exe module offset 0x"),
          ::testing::HasSubstr("WinCallbackFired")));
}

TEST(SignalHandlerWindows, AccessViolationWrite) {
  EXPECT_DEATH(
      setupAndCrash(&crashAvWrite),
      ::testing::AllOf(
          ::testing::HasSubstr(
              "*** Fatal exception 0xc0000005 (EXCEPTION_ACCESS_VIOLATION)"),
          ::testing::HasSubstr("write at 0x0"),
          ::testing::HasSubstr("faulting instruction at 0x"),
          ::testing::HasSubstr(
              "signal_handler_windows_test.exe module offset 0x"),
          ::testing::HasSubstr("WinCallbackFired")));
}

TEST(SignalHandlerWindows, StackOverflow) {
  EXPECT_DEATH(
      setupAndCrash(&crashStackOverflow),
      ::testing::AllOf(
          ::testing::HasSubstr(
              "*** Fatal exception 0xc00000fd (EXCEPTION_STACK_OVERFLOW)"),
          ::testing::HasSubstr("WinCallbackFired")));
}

// A throw escaping a noexcept function reaches std::terminate, not the VEH;
// terminateHandler must still print the message + symbolized trace and run the
// registered fatal callbacks.
TEST(SignalHandlerWindows, TerminateHandler) {
  EXPECT_DEATH(
      setupAndCrash(&throwFromNoexcept),
      ::testing::AllOf(
          ::testing::HasSubstr("*** std::terminate called (boom)"),
          ::testing::HasSubstr("WinCallbackFired")));
}

// MSVC C++ throws produce exception code 0xe06d7363, which the filter
// classifies as non-fatal. The throw should propagate to the catch block
// normally without setting fatalSignalReceived().
TEST(SignalHandlerWindows, CxxThrowDoesNotFire) {
  installFatalSignalHandler();
  bool caught = false;
  try {
    throw std::runtime_error("not fatal");
  } catch (const std::exception&) {
    caught = true;
  }
  EXPECT_TRUE(caught);
  EXPECT_FALSE(fatalSignalReceived());
}

TEST(SignalHandlerWindows, FatalSignalReceivedStartsFalse) {
  installFatalSignalHandler();
  EXPECT_FALSE(fatalSignalReceived());
}

} // namespace test
} // namespace symbolizer
} // namespace folly

#endif // _WIN32

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
