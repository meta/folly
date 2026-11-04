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

#include <folly/system/ThreadName.h>

#include <string>
#include <thread>
#include <vector>

#include <folly/ScopeGuard.h>
#include <folly/portability/GTest.h>
#include <folly/synchronization/Baton.h>

using namespace std;
using namespace folly;

namespace {

const bool expectedSetOtherThreadNameResult = folly::canSetOtherThreadName();
const bool expectedSetSelfThreadNameResult = folly::canSetCurrentThreadName();
constexpr StringPiece kThreadName{"rockin-thread"};

} // namespace

TEST(ThreadName, getCurrentThreadName) {
  thread th([] {
    EXPECT_EQ(expectedSetSelfThreadNameResult, setThreadName(kThreadName));
    if (expectedSetSelfThreadNameResult) {
      EXPECT_EQ(kThreadName.toString(), *getCurrentThreadName());
    }
  });
  SCOPE_EXIT {
    th.join();
  };
}

#if FOLLY_HAVE_PTHREAD
TEST(ThreadName, setThreadNameOtherPthread) {
  Baton<> handle_set;
  Baton<> let_thread_end;
  pthread_t handle;
  thread th([&] {
    handle = pthread_self();
    handle_set.post();
    let_thread_end.wait();
  });
  SCOPE_EXIT {
    th.join();
  };
  handle_set.wait();
  SCOPE_EXIT {
    let_thread_end.post();
  };
  EXPECT_EQ(
      expectedSetOtherThreadNameResult, setThreadName(handle, kThreadName));
}
#endif

TEST(ThreadName, setThreadNameOtherId) {
  Baton<> let_thread_end;
  thread th([&] { let_thread_end.wait(); });
  SCOPE_EXIT {
    th.join();
  };
  SCOPE_EXIT {
    let_thread_end.post();
  };
  EXPECT_EQ(
      expectedSetOtherThreadNameResult,
      setThreadName(th.get_id(), kThreadName));
  if (expectedSetOtherThreadNameResult) {
    EXPECT_EQ(*getThreadName(th.get_id()), kThreadName);
  }
}

namespace {

struct NamedThread {
  std::string name;
  std::thread::id id;
};
std::vector<NamedThread> namedThreads;

void recordNamedThread(StringPiece name) noexcept {
  namedThreads.push_back({name.toString(), std::this_thread::get_id()});
}

} // namespace

TEST(ThreadName, currentThreadNameCallbackRunsOnTheNamingThread) {
  namedThreads.clear();
  const auto previous = setCurrentThreadNameCallback(recordNamedThread);
  SCOPE_EXIT {
    setCurrentThreadNameCallback(previous);
  };
  std::thread::id namingThread;
  thread th([&] {
    namingThread = std::this_thread::get_id();
    setThreadName(kThreadName);
  });
  th.join();

  ASSERT_EQ(namedThreads.size(), 1);
  EXPECT_EQ(namedThreads[0].name, kThreadName.toString());
  EXPECT_EQ(namedThreads[0].id, namingThread);
}

TEST(ThreadName, currentThreadNameCallbackSkipsNamingAnotherThread) {
  namedThreads.clear();
  const auto previous = setCurrentThreadNameCallback(recordNamedThread);
  SCOPE_EXIT {
    setCurrentThreadNameCallback(previous);
  };
  Baton<> let_thread_end;
  thread th([&] { let_thread_end.wait(); });
  setThreadName(th.get_id(), kThreadName);
  let_thread_end.post();
  th.join();

  EXPECT_TRUE(namedThreads.empty());
}

TEST(ThreadName, clearedCurrentThreadNameCallbackIsNotRun) {
  namedThreads.clear();
  const auto previous = setCurrentThreadNameCallback(recordNamedThread);
  EXPECT_EQ(setCurrentThreadNameCallback(nullptr), recordNamedThread);
  SCOPE_EXIT {
    setCurrentThreadNameCallback(previous);
  };
  thread th([] { setThreadName(kThreadName); });
  th.join();

  EXPECT_TRUE(namedThreads.empty());
}
