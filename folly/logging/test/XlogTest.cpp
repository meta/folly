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

#include <folly/logging/xlog.h>

#include <chrono>
#include <string_view>
#include <thread>

#include <folly/logging/LogConfigParser.h>
#include <folly/logging/LogHandler.h>
#include <folly/logging/LogMessage.h>
#include <folly/logging/LoggerDB.h>
#include <folly/logging/test/TestLogHandler.h>
#include <folly/logging/test/XlogHeader1.h>
#include <folly/logging/test/XlogHeader2.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <folly/test/TestUtils.h>

using namespace folly;
using std::make_shared;
using testing::ElementsAre;
using testing::ElementsAreArray;
using namespace std::chrono_literals;

XLOG_SET_CATEGORY_NAME("xlog_test.main_file")

namespace {
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 9
// XLOG_SET_CATEGORY_NAME() does not work in gcc < 9.0
// TODO: remove this when we drop support for gcc 8.x
constexpr const char* current_xlog_category = "folly.logging.test.XlogTest.cpp";
constexpr const char* current_xlog_category_original =
    "folly/logging/test/XlogTest.cpp";
constexpr const char* current_xlog_parent = "folly.logging.test.XlogTest";
#else
constexpr const char* current_xlog_category = "xlog_test.main_file";
constexpr const char* current_xlog_category_original = "xlog_test.main_file";
constexpr const char* current_xlog_parent = "xlog_test.main_file";
#endif

class XlogTest : public testing::Test {
 public:
  XlogTest() {
    // Note that the XLOG* macros always use the main LoggerDB singleton.
    // There is no way to get them to use a test LoggerDB during unit tests.
    //
    // In order to ensure that changes to the LoggerDB singleton do not persist
    // across test functions we reset the configuration to a fixed state before
    // each test starts.
    auto config =
        parseLogConfig(".=WARN:default; default=stream:stream=stderr");
    LoggerDB::get().resetConfig(config);
  }
};
} // namespace

TEST_F(XlogTest, xlogName) {
  EXPECT_EQ(current_xlog_category_original, XLOG_GET_CATEGORY_NAME());
  EXPECT_EQ(current_xlog_category, XLOG_GET_CATEGORY()->getName());
}

TEST_F(XlogTest, xlogIf) {
  auto handler = make_shared<TestLogHandler>();
  LoggerDB::get().getCategory(current_xlog_parent)->addHandler(handler);
  auto& messages = handler->getMessages();

  // info messages are not enabled initially.
  EXPECT_FALSE(XLOG_IS_ON(INFO));
  EXPECT_TRUE(XLOG_IS_ON(ERR));
  XLOG_IF(INFO, false, "testing 1");
  EXPECT_EQ(0, messages.size());
  messages.clear();

  XLOG_IF(INFO, true, "testing 1");
  EXPECT_EQ(0, messages.size());
  messages.clear();

  // Increase the log level, then log a message.
  LoggerDB::get().setLevel(current_xlog_category, LogLevel::DBG1);
  XLOG_IF(DBG1, false, "testing: ", 1, 2, 3);
  ASSERT_EQ(0, messages.size());
  messages.clear();

  XLOG_IF(DBG1, true, "testing: ", 1, 2, 3);
  ASSERT_EQ(1, messages.size());
  messages.clear();

  // more complex conditional expressions
  std::array<bool, 2> conds = {{false, true}};
  for (unsigned i = 0; i < conds.size(); i++) {
    for (unsigned j = 0; j < conds.size(); j++) {
      XLOG_IF(DBG1, conds[i] && conds[j], "testing conditional");
      EXPECT_EQ((conds[i] && conds[j]) ? 1 : 0, messages.size());
      messages.clear();

      XLOG_IF(DBG1, conds[i] || conds[j], "testing conditional");
      EXPECT_EQ((conds[i] || conds[j]) ? 1 : 0, messages.size());
      messages.clear();
    }
  }

  XLOG_IF(DBG1, 0x6 & 0x2, "More conditional 1");
  EXPECT_EQ(1, messages.size());
  messages.clear();

  XLOG_IF(DBG1, 0x6 | 0x2, "More conditional 2");
  EXPECT_EQ(1, messages.size());
  messages.clear();

  XLOG_IF(DBG1, 0x6 | 0x2 ? true : false, "More conditional 3");
  EXPECT_EQ(1, messages.size());
  messages.clear();

  XLOG_IF(DBG1, 0x6 | 0x2 ? true : false, "More conditional 3");
  EXPECT_EQ(1, messages.size());
  messages.clear();

  XLOG_IF(DBG1, 0x3 & 0x4 ? true : false, "More conditional 4");
  EXPECT_EQ(0, messages.size());
  messages.clear();

  XLOG_IF(DBG1, false ? true : false, "More conditional 5");
  EXPECT_EQ(0, messages.size());
  messages.clear();

  XLOGF_IF(DBG1, false, "number: {:>3d}; string: {}", 12, "foo");
  ASSERT_EQ(0, messages.size());
  messages.clear();
  XLOGF_IF(DBG1, true, "number: {:>3d}; string: {}", 12, "foo");
  ASSERT_EQ(1, messages.size());
  messages.clear();

  XLOGF_IF(DBG1, false, "plain format string");
  ASSERT_EQ(0, messages.size());
  messages.clear();
  XLOGF_IF(DBG1, true, "plain format string");
  ASSERT_EQ(1, messages.size());
  messages.clear();

  // If the log is not enabled (false condition or below current level) the
  // message expression should not be evaluated.
  bool called = false;
  const auto call = [&] {
    called = true;
    return "called";
  };

  XLOG_IF(INFO, false) << call();
  EXPECT_FALSE(called);
  XLOGF_IF(INFO, false, "{}", call());
  EXPECT_FALSE(called);

  XLOG(DBG2) << call();
  EXPECT_FALSE(called);
  XLOGF(DBG2, "{}", call());
  EXPECT_FALSE(called);

  XLOG(INFO) << call();
  EXPECT_TRUE(called);
}

TEST_F(XlogTest, xlog) {
  auto handler = make_shared<TestLogHandler>();
  LoggerDB::get().getCategory(current_xlog_parent)->addHandler(handler);
  auto& messages = handler->getMessages();

  // info messages are not enabled initially.
  EXPECT_FALSE(XLOG_IS_ON(INFO));
  EXPECT_TRUE(XLOG_IS_ON(ERR));
  XLOG(INFO, "testing 1");
  EXPECT_EQ(0, messages.size());
  messages.clear();

  // Increase the log level, then log a message.
  LoggerDB::get().setLevel(current_xlog_category, LogLevel::DBG1);

  XLOG(DBG1, "testing: ", 1, 2, 3);
  ASSERT_EQ(1, messages.size());
  EXPECT_EQ("testing: 123", messages[0].first.getMessage());
  EXPECT_TRUE(messages[0].first.getFileName().endsWith("XlogTest.cpp"))
      << "unexpected file name: " << messages[0].first.getFileName();
  EXPECT_EQ(LogLevel::DBG1, messages[0].first.getLevel());
  EXPECT_EQ(current_xlog_category, messages[0].first.getCategory()->getName());
  EXPECT_EQ(current_xlog_parent, messages[0].second->getName());
  messages.clear();

  XLOGF(WARN, "number: {:>3d}; string: {}", 12, "foo");
  ASSERT_EQ(1, messages.size());
  EXPECT_EQ("number:  12; string: foo", messages[0].first.getMessage());
  EXPECT_TRUE(messages[0].first.getFileName().endsWith("XlogTest.cpp"))
      << "unexpected file name: " << messages[0].first.getFileName();
  EXPECT_EQ(LogLevel::WARN, messages[0].first.getLevel());
  EXPECT_EQ(current_xlog_category, messages[0].first.getCategory()->getName());
  EXPECT_EQ(current_xlog_parent, messages[0].second->getName());
  messages.clear();

  XLOGF(WARN, "plain format string");
  ASSERT_EQ(1, messages.size());
  EXPECT_EQ("plain format string", messages[0].first.getMessage());
  EXPECT_TRUE(messages[0].first.getFileName().endsWith("XlogTest.cpp"))
      << "unexpected file name: " << messages[0].first.getFileName();
  EXPECT_EQ(LogLevel::WARN, messages[0].first.getLevel());
  EXPECT_EQ(current_xlog_category, messages[0].first.getCategory()->getName());
  EXPECT_EQ(current_xlog_parent, messages[0].second->getName());
  messages.clear();

  XLOG(DBG2, "this log check should not pass");
  EXPECT_EQ(0, messages.size());
  messages.clear();

  // Test stream arguments to XLOG()
  XLOG(INFO) << "stream test: " << 1 << ", two, " << 3;
  ASSERT_EQ(1, messages.size());
  EXPECT_EQ("stream test: 1, two, 3", messages[0].first.getMessage());
  EXPECT_TRUE(messages[0].first.getFileName().endsWith("XlogTest.cpp"))
      << "unexpected file name: " << messages[0].first.getFileName();
  EXPECT_EQ(LogLevel::INFO, messages[0].first.getLevel());
  EXPECT_EQ(current_xlog_category, messages[0].first.getCategory()->getName());
  EXPECT_EQ(current_xlog_parent, messages[0].second->getName());
  messages.clear();
}

TEST_F(XlogTest, perFileCategoryHandling) {
  using namespace logging_test;

  auto handler = make_shared<TestLogHandler>();
  LoggerDB::get().getCategory("folly.logging.test")->addHandler(handler);
  LoggerDB::get().setLevel("folly.logging.test", LogLevel::DBG9);
  auto& messages = handler->getMessages();

  // Use the simple helper function in XlogHeader2
  testXlogHdrFunction("factor", 99);
  ASSERT_EQ(1, messages.size());
  EXPECT_EQ("test: factor=99", messages[0].first.getMessage());
  EXPECT_TRUE(messages[0].first.getFileName().endsWith("XlogHeader2.h"))
      << "unexpected file name: " << messages[0].first.getFileName();
  EXPECT_EQ(LogLevel::DBG3, messages[0].first.getLevel());
  EXPECT_EQ(
      "folly.logging.test.XlogHeader2.h",
      messages[0].first.getCategory()->getName());
  EXPECT_EQ("folly.logging.test", messages[0].second->getName());
  messages.clear();

  // Test the loop function from XlogHeader1
  testXlogHdrLoop(3, "hello world");
  ASSERT_EQ(5, messages.size());
  EXPECT_EQ("starting: hello world", messages[0].first.getMessage());
  EXPECT_TRUE(messages[0].first.getFileName().endsWith("XlogHeader1.h"))
      << "unexpected file name: " << messages[0].first.getFileName();
  EXPECT_EQ(LogLevel::DBG1, messages[0].first.getLevel());
  EXPECT_EQ(
      "folly.logging.test.XlogHeader1.h",
      messages[0].first.getCategory()->getName());
  EXPECT_EQ("folly.logging.test", messages[0].second->getName());

  EXPECT_EQ("test: hello world", messages[1].first.getMessage());
  EXPECT_EQ("test: hello world", messages[2].first.getMessage());
  EXPECT_EQ("test: hello world", messages[3].first.getMessage());
  EXPECT_EQ("finished: hello world", messages[4].first.getMessage());
  EXPECT_EQ(LogLevel::DBG5, messages[1].first.getLevel());
  EXPECT_EQ(LogLevel::DBG5, messages[2].first.getLevel());
  EXPECT_EQ(LogLevel::DBG5, messages[3].first.getLevel());
  EXPECT_EQ(LogLevel::DBG1, messages[4].first.getLevel());
  messages.clear();

  // Reduce the log level so that the messages inside the loop
  // should not be logged.
  LoggerDB::get().setLevel("folly.logging.test", LogLevel::DBG2);
  testXlogHdrLoop(300, "hello world");
  ASSERT_EQ(2, messages.size());
  EXPECT_EQ("starting: hello world", messages[0].first.getMessage());
  EXPECT_EQ("finished: hello world", messages[1].first.getMessage());
  messages.clear();

  // Call the helpers function in XlogFile1.cpp and XlogFile2.cpp and makes
  // sure their categories are reported correctly.
  testXlogFile1Dbg1("foobar 1234");
  ASSERT_EQ(1, messages.size());
  EXPECT_EQ("file1: foobar 1234", messages[0].first.getMessage());
  EXPECT_EQ(
      "folly.logging.test.XlogFile1.cpp",
      messages[0].first.getCategory()->getName());
  messages.clear();

  testXlogFile2Dbg1("hello world");
  ASSERT_EQ(1, messages.size());
  EXPECT_EQ("file2: hello world", messages[0].first.getMessage());
  EXPECT_EQ(
      "folly.logging.test.XlogFile2.cpp",
      messages[0].first.getCategory()->getName());
  messages.clear();

  // Adjust the log level and make sure the changes take effect for the .cpp
  // file categories
  LoggerDB::get().setLevel("folly.logging.test", LogLevel::INFO);
  testXlogFile1Dbg1("log check should fail now");
  testXlogFile2Dbg1("this should fail too");
  EXPECT_EQ(0, messages.size());
  messages.clear();

  LoggerDB::get().setLevel("folly.logging.test.XlogFile1", LogLevel::DBG1);
  testXlogFile1Dbg1("this log check should pass now");
  testXlogFile2Dbg1("but this one should still fail");
  ASSERT_EQ(1, messages.size());
  EXPECT_EQ(
      "file1: this log check should pass now", messages[0].first.getMessage());
  EXPECT_EQ(
      "folly.logging.test.XlogFile1.cpp",
      messages[0].first.getCategory()->getName());
  messages.clear();
}

TEST_F(XlogTest, rateLimiting) {
  auto SEVEN = 7;
  auto handler = make_shared<TestLogHandler>();
  LoggerDB::get().getCategory(current_xlog_parent)->addHandler(handler);
  LoggerDB::get().setLevel(current_xlog_parent, LogLevel::DBG1);

  // Test XLOG_EVERY_N
  for (size_t n = 0; n < 50; ++n) {
    XLOG_EVERY_N(DBG1, 7, "msg ", n);
  }
  EXPECT_THAT(
      handler->getMessageValues(),
      ElementsAre(
          "msg 0",
          "msg 7",
          "msg 14",
          "msg 21",
          "msg 28",
          "msg 35",
          "msg 42",
          "msg 49"));
  handler->clearMessages();

  // Test XLOGF_EVERY_N
  for (size_t n = 0; n < 50; ++n) {
    XLOGF_EVERY_N(DBG1, 7, "msg {}", n);
  }
  EXPECT_THAT(
      handler->getMessageValues(),
      ElementsAre(
          "msg 0",
          "msg 7",
          "msg 14",
          "msg 21",
          "msg 28",
          "msg 35",
          "msg 42",
          "msg 49"));
  handler->clearMessages();

  for (size_t n = 0; n < 50; ++n) {
    XLOG_EVERY_N(DBG1, SEVEN + 1, "msg ", n);
  }
  EXPECT_THAT(
      handler->getMessageValues(),
      ElementsAre(
          "msg 0", "msg 8", "msg 16", "msg 24", "msg 32", "msg 40", "msg 48"));
  handler->clearMessages();

  // Test XLOG_EVERY_N_IF
  for (size_t n = 0; n < 50; ++n) {
    bool shouldLog = n % 2 == 0;
    XLOG_EVERY_N_IF(DBG1, shouldLog, 7, "msg ", n);
  }
  EXPECT_THAT(
      handler->getMessageValues(),
      ElementsAre("msg 0", "msg 14", "msg 28", "msg 42"));
  handler->clearMessages();

  // Test XLOG_EVERY_N_OR
  for (size_t n = 0; n < 10; ++n) {
    bool shouldLog = n % 2 == 0;
    XLOG_EVERY_N_OR(DBG1, shouldLog, 2, "msg ", n);
  }
  EXPECT_THAT(
      handler->getMessageValues(),
      ElementsAre(
          "msg 0",
          "msg 1",
          "msg 2",
          "msg 4",
          "msg 5",
          "msg 6",
          "msg 8",
          "msg 9"));
  handler->clearMessages();

  // Test XLOGF_EVERY_N_IF
  for (size_t n = 0; n < 50; ++n) {
    bool shouldLog = n % 2 == 0;
    XLOGF_EVERY_N_IF(DBG1, shouldLog, 7, "msg {}", n);
  }
  EXPECT_THAT(
      handler->getMessageValues(),
      ElementsAre("msg 0", "msg 14", "msg 28", "msg 42"));
  handler->clearMessages();

  for (size_t n = 0; n < 50; ++n) {
    XLOG_EVERY_N(DBG1, SEVEN + 1, "msg ", n);
  }
  EXPECT_THAT(
      handler->getMessageValues(),
      ElementsAre(
          "msg 0", "msg 8", "msg 16", "msg 24", "msg 32", "msg 40", "msg 48"));
  handler->clearMessages();

  // Test XLOGF_EVERY_N_OR
  for (size_t n = 0; n < 10; ++n) {
    bool shouldLog = n % 2 == 0;
    XLOGF_EVERY_N_OR(DBG1, shouldLog, 2, "msg {}", n);
  }
  EXPECT_THAT(
      handler->getMessageValues(),
      ElementsAre(
          "msg 0",
          "msg 1",
          "msg 2",
          "msg 4",
          "msg 5",
          "msg 6",
          "msg 8",
          "msg 9"));
  handler->clearMessages();

  // Test XLOG_EVERY_N_EXACT
  for (size_t n = 0; n < 50; ++n) {
    XLOG_EVERY_N_EXACT(DBG1, 7, "msg ", n);
  }
  EXPECT_THAT(
      handler->getMessageValues(),
      ElementsAre(
          "msg 0",
          "msg 7",
          "msg 14",
          "msg 21",
          "msg 28",
          "msg 35",
          "msg 42",
          "msg 49"));
  handler->clearMessages();

  for (size_t n = 0; n < 50; ++n) {
    XLOG_EVERY_N_EXACT(DBG1, SEVEN + 1, "msg ", n);
  }
  EXPECT_THAT(
      handler->getMessageValues(),
      ElementsAre(
          "msg 0", "msg 8", "msg 16", "msg 24", "msg 32", "msg 40", "msg 48"));
  handler->clearMessages();

  // Test XLOG_EVERY_N_THREAD
  for (size_t n = 0; n < 50; ++n) {
    XLOG_EVERY_N_THREAD(DBG1, 7, "msg ", n);
  }
  EXPECT_THAT(
      handler->getMessageValues(),
      ElementsAre(
          "msg 0",
          "msg 7",
          "msg 14",
          "msg 21",
          "msg 28",
          "msg 35",
          "msg 42",
          "msg 49"));
  handler->clearMessages();

  for (size_t n = 0; n < 50; ++n) {
    XLOG_EVERY_N_THREAD(DBG1, SEVEN + 1, "msg ", n);
  }
  EXPECT_THAT(
      handler->getMessageValues(),
      ElementsAre(
          "msg 0", "msg 8", "msg 16", "msg 24", "msg 32", "msg 40", "msg 48"));
  handler->clearMessages();

  // Test XLOG_EVERY_MS, XLOGF_EVERY_MS and XLOG_N_PER_MS
  // We test these together to minimize the number of sleep operations.
  for (size_t n = 0; n < 10; ++n) {
    // Integer arguments are treated as millisecond
    XLOG_EVERY_MS(DBG1, 100, "int arg ", n);
    // Other duration arguments also work, as long as they are
    // coarser than milliseconds
    XLOG_EVERY_MS(DBG1, 100ms, "ms arg ", n);
    XLOG_EVERY_MS(DBG1, 1s, "s arg ", n);
    auto t = 1s;
    XLOG_EVERY_MS(DBG1, t, "s arg capture ", n);

    // Use XLOGF_EVERY_MS
    XLOGF_EVERY_MS(DBG1, 100, "fmt arg {}", n);
    XLOGF_EVERY_MS(DBG1, 100ms, "fmt ms arg {}", n);

    XLOGF_EVERY_MS(DBG1, 100, "plain fmt str");
    XLOGF_EVERY_MS(DBG1, 100ms, "plain fmt str ms");

    // Use XLOG_N_PER_MS() too
    XLOG_N_PER_MS(DBG1, 2, 100, "2x int arg ", n);
    XLOG_N_PER_MS(DBG1, 1, 100ms, "1x ms arg ", n);
    XLOG_N_PER_MS(DBG1, 3, 1s, "3x s arg ", n);

    // Conditional logging
    bool shouldLog = n && (n % 2 == 0);
    XLOG_EVERY_MS_IF(DBG1, shouldLog, 100, "int arg conditional ", n);
    XLOGF_EVERY_MS_IF(DBG1, shouldLog, 100, "int fmt arg conditional {}", n);
    XLOG_EVERY_MS_OR(DBG1, shouldLog, 100, "int arg conditional or ", n);
    XLOGF_EVERY_MS_OR(DBG1, shouldLog, 100, "int fmt arg conditional or {}", n);

    // Sleep for 100ms between iterations 5 and 6
    if (n == 5) {
      /* sleep override */ std::this_thread::sleep_for(110ms);
    }
  }
  EXPECT_THAT(
      handler->getMessageValues(),
      ElementsAreArray({
          "int arg 0",
          "ms arg 0",
          "s arg 0",
          "s arg capture 0",
          "fmt arg 0",
          "fmt ms arg 0",
          "plain fmt str",
          "plain fmt str ms",
          "2x int arg 0",
          "1x ms arg 0",
          "3x s arg 0",
          "int arg conditional or 0",
          "int fmt arg conditional or 0",
          "2x int arg 1",
          "3x s arg 1",
          "3x s arg 2",
          "int arg conditional 2",
          "int fmt arg conditional 2",
          "int arg conditional or 2",
          "int fmt arg conditional or 2",
          "int arg conditional or 4",
          "int fmt arg conditional or 4",
          "int arg 6",
          "ms arg 6",
          "fmt arg 6",
          "fmt ms arg 6",
          "plain fmt str",
          "plain fmt str ms",
          "2x int arg 6",
          "1x ms arg 6",
          "int arg conditional 6",
          "int fmt arg conditional 6",
          "int arg conditional or 6",
          "int fmt arg conditional or 6",
          "2x int arg 7",
          "int arg conditional or 7",
          "int fmt arg conditional or 7",
          "int arg conditional or 8",
          "int fmt arg conditional or 8",
      }));
  handler->clearMessages();

  // Test XLOG_FIRST_N
  for (size_t n = 0; n < 10; ++n) {
    XLOG_FIRST_N(DBG1, 4, "bah ", n);
  }
  EXPECT_THAT(
      handler->getMessageValues(),
      ElementsAreArray({
          "bah 0",
          "bah 1",
          "bah 2",
          "bah 3",
      }));
  handler->clearMessages();

  // Test XLOGF_FIRST_N
  for (size_t n = 0; n < 10; ++n) {
    XLOGF_FIRST_N(DBG1, 4, "bah {}", n);
  }
  EXPECT_THAT(
      handler->getMessageValues(),
      ElementsAreArray({
          "bah 0",
          "bah 1",
          "bah 2",
          "bah 3",
      }));
  handler->clearMessages();
}

TEST_F(XlogTest, rateLimitingEndOfThread) {
  auto handler = make_shared<TestLogHandler>();
  LoggerDB::get().getCategory(current_xlog_parent)->addHandler(handler);
  LoggerDB::get().setLevel(current_xlog_parent, LogLevel::DBG1);

  auto th = std::thread([&] {
    auto enqueue = [](int num) {
      pthread_key_t key;
      pthread_key_create(&key, [](void* obj) {
        auto* i = static_cast<int*>(obj);
        XLOG_EVERY_N_THREAD(DBG1, 1, "dtor ", *i);
        delete i;
      });
      pthread_setspecific(key, new int(num));
    };

    enqueue(100);
    enqueue(101);
    for (size_t n = 0; n < 50; ++n) {
      XLOG_EVERY_N_THREAD(DBG1, 7, "msg ", n);
    }
    enqueue(102);
    enqueue(103);
  });
  th.join();
  EXPECT_THAT(
      handler->getMessageValues(),
      ElementsAreArray({
          "msg 0",
          "msg 7",
          "msg 14",
          "msg 21",
          "msg 28",
          "msg 35",
          "msg 42",
          "msg 49",
          "dtor 100",
          "dtor 101",
          "dtor 102",
          "dtor 103",
      }));
  handler->clearMessages();
}

TEST_F(XlogTest, getXlogCategoryName) {
  EXPECT_EQ("foo.cpp", getXlogCategoryNameForFile("foo.cpp"));
  EXPECT_EQ("foo.h", getXlogCategoryNameForFile("foo.h"));

  // Directory separators should be translated to "." during LogName
  // canonicalization
  EXPECT_EQ("src/test/foo.cpp", getXlogCategoryNameForFile("src/test/foo.cpp"));
  EXPECT_EQ(
      "src.test.foo.cpp",
      LogName::canonicalize(getXlogCategoryNameForFile("src/test/foo.cpp")));
  EXPECT_EQ("src/test/foo.h", getXlogCategoryNameForFile("src/test/foo.h"));
  EXPECT_EQ(
      "src.test.foo.h",
      LogName::canonicalize(getXlogCategoryNameForFile("src/test/foo.h")));

  // Buck's directory prefixes for generated source files
  // should be stripped out

  if (folly::detail::xlog_support_buck2) {
    EXPECT_EQ(
        "dirA.dirB.Foo.h",
        LogName::canonicalize(getXlogCategoryNameForFile(
            "buck-out/v2/gen/fbcode/bfcdfe5fb8e2f7e1/dirA/dirB/__some_lib_rule__/"
            "buck-headers/dirA/dirB/Foo.h")));
    EXPECT_EQ(
        "abc.xyz.def.h",
        LogName::canonicalize(getXlogCategoryNameForFile(
            "buck-out/v2/gen/fbcode/bfcdfe5fb8e2f7e1/dirA/dirB/__some_lib_rule__/"
            "buck-private-headers/abc/xyz/def.h")));
  }
}

TEST(Xlog, xlogStripFilename) {
  EXPECT_STREQ("c/d.txt", xlogStripFilename("/a/b/c/d.txt", "/a/b"));
  EXPECT_STREQ("c/d.txt", xlogStripFilename("/a/b/c/d.txt", "/a/b/"));
  EXPECT_STREQ(
      "ships/cruiser.cpp",
      xlogStripFilename(
          "/home/johndoe/src/spacesim/ships/cruiser.cpp",
          "/home/johndoe/src/spacesim"));
  EXPECT_STREQ(
      "ships/cruiser.cpp",
      xlogStripFilename("src/spacesim/ships/cruiser.cpp", "src/spacesim"));

  // Test with multiple prefixes
  EXPECT_STREQ("c/d.txt", xlogStripFilename("/a/b/c/d.txt", "/x/y:1/2:/a/b"));
  EXPECT_STREQ("c/d.txt", xlogStripFilename("/a/b/c/d.txt", "/x/y:/a/b:/1/2"));

  EXPECT_STREQ(
      "/foobar/src/test.cpp", xlogStripFilename("/foobar/src/test.cpp", "/foo"))
      << "should only strip full directory name matches";
  EXPECT_STREQ(
      "src/test.cpp",
      xlogStripFilename("/foobar/src/test.cpp", "/foo:/foobar"));

  EXPECT_STREQ(
      "/a/b/c/d.txt", xlogStripFilename("/a/b/c/d.txt", "/a/b/c/d.txt"))
      << "should not strip if the result will be empty";
  EXPECT_STREQ("c/d.txt", xlogStripFilename("/a/b/c/d.txt", ":/x/y::/a/b:"))
      << "empty prefixes in the prefix list should be ignored";

  EXPECT_STREQ("d.txt", xlogStripFilename("/a/b/c/d.txt", "/a/b/c:/a"))
      << "only the first prefix match should be honored";
  EXPECT_STREQ("b/c/d.txt", xlogStripFilename("/a/b/c/d.txt", "/a:/a/b/c"))
      << "only the first prefix match should be honored";

  // xlogStripFilename() should ideally be a purely compile-time evaluation.
  // Use a static_assert() to ensure that it can be evaluated at compile time.
  // We use EXPECT_STREQ() checks above for most of the testing since it
  // produces nicer messages on failure.
  static_assert(
      std::string_view{xlogStripFilename(
          "/my/project/src/test.cpp", "/my/project")} == "src/test.cpp",
      "incorrect xlogStripFilename() behavior");

  if (kIsWindows) {
    EXPECT_STREQ(
        "c\\d.txt", xlogStripFilename("Z:\\a\\b\\c\\d.txt", "Z:\\a\\b"));
    EXPECT_STREQ("c\\d.txt", xlogStripFilename("Z:\\a\\b\\c\\d.txt", "Z:/a/b"));

    EXPECT_STREQ("c/d.txt", xlogStripFilename("Z:/a/b/c/d.txt", "Z:\\a\\b"));
    EXPECT_STREQ("c/d.txt", xlogStripFilename("Z:/a/b/c/d.txt", "Z:/a/b"));

    EXPECT_STREQ(
        "c\\d.txt", xlogStripFilename("Z:\\a\\b\\c\\d.txt", "C:/x/y:Z:/a/b"));
    EXPECT_STREQ(
        "c\\d.txt", xlogStripFilename("Z:\\a\\b\\c\\d.txt", "Z:/x/y:Z:/a/b"));
  }
}

TEST(Xlog, XCheckPrecedence) {
  // Ensure that XCHECK_XX() and XDCHECK_XX() avoid the common macro pitfall of
  // not wrapping arguments in parentheses and causing incorrect operator
  // precedence issues.
  XCHECK_EQ(0x22 & 0x3, 2);
  XDCHECK_EQ(2, 0x22 & 0x3);
  XCHECK_NE(0x62 & 0x22, 2);
  XDCHECK_NE(0x62 & 0x22, 2);
}
