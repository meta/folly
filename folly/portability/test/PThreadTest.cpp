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

#include <folly/portability/PThread.h>

#include <folly/portability/GTest.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

TEST(PThreadTest, pthreadCreateAndJoin) {
  static std::atomic<bool> hasRun{false};
  static std::atomic<int32_t> argPassedIn{0};
  auto mainFunc = [](void* arg) -> void* {
    hasRun = true;
    argPassedIn = (int32_t)reinterpret_cast<uintptr_t>(arg);
    return nullptr;
  };

  pthread_t thread;
  EXPECT_EQ(pthread_create(&thread, nullptr, mainFunc, (void*)53), 0);
  EXPECT_EQ(pthread_join(thread, nullptr), 0);
  EXPECT_TRUE(hasRun);
  EXPECT_EQ(argPassedIn, 53);
}

TEST(PThreadTest, pthreadJoinReturnValue) {
  static std::atomic<bool> hasRun{false};
  auto mainFunc = [](void*) -> void* {
    hasRun = true;
    return (void*)5;
  };

  pthread_t thread;
  EXPECT_EQ(pthread_create(&thread, nullptr, mainFunc, nullptr), 0);
  void* exitCode = nullptr;
  EXPECT_EQ(pthread_join(thread, &exitCode), 0);
  EXPECT_EQ(exitCode, (void*)5);
  EXPECT_TRUE(hasRun);
}

TEST(PThreadTest, pthreadEqual) {
  auto self = pthread_self();
  EXPECT_NE(pthread_equal(self, self), 0);

  auto mainFunc = [](void*) -> void* { return nullptr; };
  pthread_t thread;
  EXPECT_EQ(pthread_create(&thread, nullptr, mainFunc, nullptr), 0);
  EXPECT_EQ(pthread_equal(thread, self), 0);
  void* exitCode = nullptr;
  pthread_join(thread, &exitCode);
}

TEST(PThreadTest, pthreadSelfOnPthreadThread) {
  static std::atomic<bool> hasRun{false};
  static pthread_t otherSelf;
  auto mainFunc = [](void*) -> void* {
    hasRun = true;
    otherSelf = pthread_self();
    return nullptr;
  };

  pthread_t thread;
  EXPECT_EQ(pthread_create(&thread, nullptr, mainFunc, nullptr), 0);
  EXPECT_EQ(pthread_join(thread, nullptr), 0);
  EXPECT_NE(pthread_equal(otherSelf, thread), 0);
  EXPECT_TRUE(hasRun);
}

namespace {
// A thread created by pthread_create, which on Windows calls CreateThread
// rather than the C runtime's thread functions.
void runInPthread(std::function<void()> func) {
  auto start = [](void* arg) -> void* {
    (*static_cast<std::function<void()>*>(arg))();
    return nullptr;
  };
  pthread_t thread;
  ASSERT_EQ(pthread_create(&thread, nullptr, start, &func), 0);
  ASSERT_EQ(pthread_join(thread, nullptr), 0);
}
} // namespace

TEST(PThreadKeyTest, getAfterSet) {
  pthread_key_t key;
  ASSERT_EQ(pthread_key_create(&key, nullptr), 0);
  int a = 0;
  int b = 0;
  EXPECT_EQ(pthread_getspecific(key), nullptr);
  EXPECT_EQ(pthread_setspecific(key, &a), 0);
  EXPECT_EQ(pthread_getspecific(key), &a);
  runInPthread([&] {
    EXPECT_EQ(pthread_getspecific(key), nullptr);
    EXPECT_EQ(pthread_setspecific(key, &b), 0);
    EXPECT_EQ(pthread_getspecific(key), &b);
  });
  std::thread([&] {
    EXPECT_EQ(pthread_getspecific(key), nullptr);
    EXPECT_EQ(pthread_setspecific(key, &b), 0);
    EXPECT_EQ(pthread_getspecific(key), &b);
  }).join();
  EXPECT_EQ(pthread_getspecific(key), &a);
  EXPECT_EQ(pthread_setspecific(key, nullptr), 0);
  EXPECT_EQ(pthread_getspecific(key), nullptr);
  EXPECT_EQ(pthread_key_delete(key), 0);
}

TEST(PThreadKeyTest, recreatedKeyStartsNull) {
  int value = 0;
  std::vector<pthread_key_t> keys(10);
  for (auto& key : keys) {
    ASSERT_EQ(pthread_key_create(&key, nullptr), 0);
    EXPECT_EQ(pthread_getspecific(key), nullptr);
    EXPECT_EQ(pthread_setspecific(key, &value), 0);
  }
  for (auto key : keys) {
    EXPECT_EQ(pthread_key_delete(key), 0);
  }
  for (auto& key : keys) {
    ASSERT_EQ(pthread_key_create(&key, nullptr), 0);
    EXPECT_EQ(pthread_getspecific(key), nullptr);
  }
  for (auto key : keys) {
    EXPECT_EQ(pthread_key_delete(key), 0);
  }
}

TEST(PThreadKeyTest, manyKeys) {
  constexpr int kNumKeys = 200;
  std::vector<pthread_key_t> keys(kNumKeys);
  for (auto& key : keys) {
    ASSERT_EQ(pthread_key_create(&key, nullptr), 0);
  }
  std::vector<int> values(kNumKeys);
  runInPthread([&] {
    for (int i = 0; i < kNumKeys; ++i) {
      EXPECT_EQ(pthread_setspecific(keys[i], &values[i]), 0);
    }
    for (int i = 0; i < kNumKeys; ++i) {
      EXPECT_EQ(pthread_getspecific(keys[i]), &values[i]);
    }
  });
  for (auto key : keys) {
    EXPECT_EQ(pthread_key_delete(key), 0);
  }
}

TEST(PThreadKeyTest, createFailsWithEagainWhenKeysRunOut) {
  std::vector<pthread_key_t> keys;
  int ret = 0;
  while (keys.size() <= 100000) {
    pthread_key_t key;
    ret = pthread_key_create(&key, nullptr);
    if (ret != 0) {
      break;
    }
    keys.push_back(key);
  }
  EXPECT_EQ(ret, EAGAIN);
  for (auto key : keys) {
    EXPECT_EQ(pthread_key_delete(key), 0);
  }
  pthread_key_t key;
  ASSERT_EQ(pthread_key_create(&key, nullptr), 0);
  EXPECT_EQ(pthread_key_delete(key), 0);
}

TEST(PThreadKeyTest, recreatedKeyStartsNullInOtherThreads) {
  pthread_key_t key;
  ASSERT_EQ(pthread_key_create(&key, nullptr), 0);
  int value = 0;
  std::mutex mutex;
  std::condition_variable cv;
  int step = 0;
  auto advanceTo = [&](int next) {
    std::lock_guard lock(mutex);
    step = next;
    cv.notify_all();
  };
  auto waitFor = [&](int expected) {
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return step == expected; });
  };
  std::vector<pthread_key_t> recreated(10);
  std::thread other([&] {
    EXPECT_EQ(pthread_setspecific(key, &value), 0);
    advanceTo(1);
    waitFor(2);
    for (auto newKey : recreated) {
      EXPECT_EQ(pthread_getspecific(newKey), nullptr);
    }
  });
  waitFor(1);
  EXPECT_EQ(pthread_key_delete(key), 0);
  for (auto& newKey : recreated) {
    ASSERT_EQ(pthread_key_create(&newKey, nullptr), 0);
  }
  advanceTo(2);
  other.join();
  for (auto newKey : recreated) {
    EXPECT_EQ(pthread_key_delete(newKey), 0);
  }
}

#if defined(_WIN32) && !FOLLY_HAVE_PTHREAD
// The emulation reports invalid keys; POSIX leaves this undefined.
TEST(PThreadKeyTest, emulationRejectsInvalidKeys) {
  int value = 0;
  pthread_key_t invalid = TLS_OUT_OF_INDEXES;
  EXPECT_EQ(pthread_setspecific(invalid, &value), EINVAL);
  EXPECT_EQ(pthread_getspecific(invalid), nullptr);
  EXPECT_EQ(pthread_key_delete(invalid), EINVAL);
}

TEST(PThreadKeyTest, getspecificPreservesLastError) {
  pthread_key_t key;
  ASSERT_EQ(pthread_key_create(&key, nullptr), 0);
  SetLastError(ERROR_FILE_NOT_FOUND);
  EXPECT_EQ(pthread_getspecific(key), nullptr);
  EXPECT_EQ(GetLastError(), ERROR_FILE_NOT_FOUND);
  EXPECT_EQ(pthread_key_delete(key), 0);
}
#endif
