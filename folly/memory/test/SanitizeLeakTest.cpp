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

#include <array>
#include <memory>
#include <thread>

#include <folly/memory/SanitizeLeak.h>
#include <folly/portability/GTest.h>

namespace folly {

TEST(SanitizeLeak, lsanIgnoreObject) {
  int* ptr = new int(5);
  EXPECT_EQ(*ptr, 5);
  lsan_ignore_object(ptr);
  EXPECT_EQ(*ptr, 5);
}

TEST(SanitizeLeak, ImplementationAlwaysWorks) {
  int* ptr = new int(5);
  EXPECT_EQ(*ptr, 5);
  detail::annotate_object_leaked_impl(ptr);
}

TEST(SanitizeLeak, Basic) {
  int* ptr = new int(5);
  EXPECT_EQ(*ptr, 5);
  annotate_object_leaked(ptr);
}

TEST(SanitizeLeak, TwoLeaks) {
  int* ptr0 = new int(0);
  int* ptr1 = new int(1);
  EXPECT_EQ(*ptr0, 0);
  EXPECT_EQ(*ptr1, 1);
  annotate_object_leaked(ptr0);
  annotate_object_leaked(ptr1);
}

TEST(SanitizeLeak, Nullptr) {
  annotate_object_leaked(nullptr);
  annotate_object_collected(nullptr);
  detail::annotate_object_leaked_impl(nullptr);
  detail::annotate_object_collected_impl(nullptr);
}

TEST(SanitizeLeak, NotLeaked) {
  int* ptr0 = new int(0);
  annotate_object_leaked(ptr0);
  {
    int* ptr1 = new int(0);
    delete ptr0;
    annotate_object_collected(ptr0);
    ptr0 = ptr1;
  }
  annotate_object_leaked(ptr0);
}

TEST(SanitizeLeak, MultiLeaked) {
  int* ptr0 = new int(0);
  for (size_t i = 0; i < 3; ++i) {
    annotate_object_leaked(ptr0);
  }
  for (size_t i = 0; i < 2; ++i) {
    annotate_object_collected(ptr0);
  }
}

TEST(SanitizeLeak, MultiNotLeaked) {
  int* ptr0 = new int(0);
  for (size_t i = 0; i < 3; ++i) {
    annotate_object_leaked(ptr0);
  }
  for (size_t i = 0; i < 3; ++i) {
    annotate_object_collected(ptr0);
  }
  delete ptr0;
}

TEST(SanitizeLeak, Concurrent) {
  std::vector<std::thread> threads;
  for (size_t t = 0; t < 8; ++t) {
    threads.emplace_back([]() {
      for (size_t i = 0; i < 1000; ++i) {
        annotate_object_leaked(new int(2 * i));
        // Call the impl directly so we can test it in TSAN mode
        detail::annotate_object_leaked_impl(new int(2 * i + 1));
        int* x = new int(i);
        annotate_object_leaked(x);
        delete x;
        annotate_object_collected(x);
        detail::annotate_object_collected_impl(x);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
}

TEST(SanitizeLeak, AnnotateCount) {
  constexpr size_t size = 27;
  constexpr auto count = annotate_object_count_leaked_uncollected;
  auto const base = count();
  std::vector<int> vec;
  vec.resize(size);
  for (auto const& item : vec) {
    annotate_object_leaked(&item);
  }
  EXPECT_EQ(base + size * size_t(kIsSanitizeAddress), count());
  for (auto const& item : vec) {
    annotate_object_leaked(&item);
  }
  EXPECT_EQ(base + 2 * size * size_t(kIsSanitizeAddress), count());
  for (auto const& item : vec) {
    annotate_object_collected(&item);
  }
  EXPECT_EQ(base + size * size_t(kIsSanitizeAddress), count());
  for (auto const& item : vec) {
    annotate_object_collected(&item);
  }
  EXPECT_EQ(base, count());
}

} // namespace folly
