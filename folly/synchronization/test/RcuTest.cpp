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

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include <glog/logging.h>

#include <folly/Benchmark.h>
#include <folly/Random.h>
#include <folly/portability/GFlags.h>
#include <folly/portability/GTest.h>
#include <folly/synchronization/Rcu.h>
#include <folly/synchronization/RelaxedAtomic.h>
#include <folly/synchronization/detail/ThreadCachedReaders.h>
#include <folly/synchronization/test/Barrier.h>

using namespace folly;
using rcu_domain = folly::rcu_domain;

DEFINE_int64(iters, 100000, "Number of iterations");
DEFINE_uint64(threads, 32, "Number of threads");

TEST(RcuTest, Basic) {
  auto foo = new int(2);
  rcu_retire(foo);
}

class des {
  bool* d_;

 public:
  des(bool* d) : d_(d) {}
  ~des() { *d_ = true; }
};

TEST(RcuTest, Guard) {
  bool del = false;
  auto foo = new des(&del);
  {
    std::scoped_lock<rcu_domain> g(rcu_default_domain());
  }
  rcu_retire(foo);
  rcu_synchronize();
  EXPECT_TRUE(del);
}

TEST(RcuTest, SlowReader) {
  std::thread t;
  {
    std::scoped_lock<rcu_domain> lock(rcu_default_domain());

    t = std::thread([&]() { rcu_synchronize(); });
    usleep(100); // Wait for synchronize to start
  }
  t.join();
}

static std::unique_lock<rcu_domain> try_retire(des* obj) {
  std::unique_lock<rcu_domain> g(rcu_default_domain());
  rcu_retire(obj);
  return g;
}

TEST(RcuTest, CopyGuard) {
  bool del = false;
  auto foo = new des(&del);
  {
    auto res = try_retire(foo);
    EXPECT_FALSE(del);
  }
  rcu_barrier();
  EXPECT_TRUE(del);
}

static void delete_or_retire_oldint(int* oldint) {
  if (folly::Random::rand32() % 2 == 0) {
    rcu_retire(oldint, [](int* obj) {
      *obj = folly::Random::rand32();
      delete obj;
    });
  } else {
    rcu_synchronize();
    *oldint = folly::Random::rand32();
    delete oldint;
  }
}

TEST(RcuTest, Stress) {
  std::vector<std::thread> readers;
  constexpr uint32_t sz = 1000;
  std::atomic<int*> ints[sz];
  for (uint32_t i = 0; i < sz; i++) {
    ints[i].store(new int(0), std::memory_order_release);
  }
  for (unsigned th = 0; th < FLAGS_threads; th++) {
    readers.emplace_back([&]() {
      for (int i = 0; i < FLAGS_iters / 100; i++) {
        std::scoped_lock<rcu_domain> lock(rcu_default_domain());
        int sum = 0;
        int* ptrs[sz];
        for (uint32_t j = 0; j < sz; j++) {
          ptrs[j] = ints[j].load(std::memory_order_acquire);
        }
        for (uint32_t j = 0; j < sz; j++) {
          sum += *ptrs[j];
        }
        EXPECT_EQ(sum, 0);
      }
    });
  }
  folly::relaxed_atomic<bool> done{false};
  std::vector<std::thread> updaters;
  for (unsigned th = 0; th < FLAGS_threads; th++) {
    updaters.emplace_back([&]() {
      while (!done) {
        auto newint = new int(0);
        auto oldint = ints[folly::Random::rand32() % sz].exchange(
            newint, std::memory_order_acq_rel);
        delete_or_retire_oldint(oldint);
      }
    });
  }
  for (auto& t : readers) {
    t.join();
  }
  done = true;

  for (auto& t : updaters) {
    t.join();
  }

  // Cleanup for asan
  rcu_synchronize();
  for (uint32_t i = 0; i < sz; i++) {
    delete ints[i].exchange(nullptr, std::memory_order_acq_rel);
  }
}

TEST(RcuTest, Synchronize) {
  std::vector<std::thread> threads;
  for (unsigned th = 0; th < FLAGS_threads; th++) {
    threads.emplace_back([&]() {
      for (int i = 0; i < 10; i++) {
        rcu_synchronize();
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
}

TEST(RcuTest, NewDomainTest) {
  rcu_domain newdomain(nullptr);
  rcu_synchronize(newdomain);
}

TEST(RcuTest, NewDomainGuardTest) {
  struct UniqueTag;
  rcu_domain newdomain(nullptr);
  bool del = false;
  auto foo = new des(&del);
  {
    std::scoped_lock<rcu_domain> g(newdomain);
  }
  rcu_retire(foo, {}, newdomain);
  rcu_synchronize(newdomain);
  EXPECT_TRUE(del);
}

TEST(RcuTest, MovableReader) {
  {
    std::unique_lock<rcu_domain> g(rcu_default_domain());
    std::unique_lock<rcu_domain> f(std::move(g));
  }
  rcu_synchronize();
  {
    std::unique_lock<rcu_domain> g(rcu_default_domain(), std::defer_lock);
    std::unique_lock<rcu_domain> f(rcu_default_domain());
    g = std::move(f);
  }
  rcu_synchronize();
}

TEST(RcuTest, SynchronizeInCall) {
  rcu_default_domain().call([]() { rcu_synchronize(); });
  rcu_synchronize();
}

TEST(RcuTest, SafeForkTest) {
  rcu_default_domain().lock();
  rcu_default_domain().unlock();
  auto pid = fork();
  if (pid > 0) {
    // Parent branch -- wait for child to exit.
    rcu_synchronize();
    int status = -1;
    auto pid2 = waitpid(pid, &status, 0);
    EXPECT_EQ(pid, pid2);
    EXPECT_EQ(status, 0);
  } else if (pid == 0) {
    rcu_synchronize();
    // Exit quickly to avoid spamming gtest output to console.
    exit(0);
  } else {
    // Skip the test if fork() fails.
    GTEST_SKIP();
  }
}

TEST(RcuTest, ThreadLocalList) {
  folly::detail::ThreadCachedLists lists;
  std::vector<std::thread> threads{FLAGS_threads};
  folly::relaxed_atomic<unsigned long> done{FLAGS_threads};
  for (auto& tr : threads) {
    tr = std::thread([&]() {
      for (int i = 0; i < FLAGS_iters; i++) {
        auto node = new folly::detail::ThreadCachedListsBase::Node;
        lists.push(node);
      }
      --done;
    });
  }
  while (done > 0) {
    folly::detail::ThreadCachedLists::ListHead list{};
    lists.collect(list);
    list.forEach([](folly::detail::ThreadCachedLists::Node* node) {
      delete node;
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  // Run cleanup pass one more time to make ASAN happy
  folly::detail::ThreadCachedLists::ListHead list{};
  lists.collect(list);
  list.forEach([](folly::detail::ThreadCachedLists::Node* node) {
    delete node;
  });
}

TEST(RcuTest, ThreadDeath) {
  bool del = false;
  std::thread t([&] {
    auto foo = new des(&del);
    rcu_retire(foo);
  });
  t.join();
  rcu_synchronize();
  EXPECT_TRUE(del);
}

TEST(RcuTest, RcuObjBase) {
  bool retired = false;
  struct base_test : rcu_obj_base<base_test> {
    bool* ret_;
    base_test(bool* ret) : ret_(ret) {}
    ~base_test() { (*ret_) = true; }
  };

  auto foo = new base_test(&retired);
  foo->retire();
  rcu_synchronize();
  EXPECT_TRUE(retired);
}

TEST(RcuTest, Tsan) {
  int data = 0;
  std::thread t1([&] {
    rcu_default_domain().lock();
    data = 1;
    rcu_default_domain().unlock();
    // Delay before exiting so the thread is still alive for TSAN detection.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  });

  std::thread t2([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // This should establish a happens-before relationship between the earlier
    // write (data = 1) and this write below (data = 2).
    rcu_default_domain().synchronize();
    data = 2;
  });

  t1.join();
  t2.join();
  EXPECT_EQ(data, 2);
}

TEST(RcuTest, TsanActiveNextEpochDuringReclaim) {
  // A reader exits one epoch and immediately enters the next, remaining
  // active there while the previous epoch is reclaimed. The reclaiming
  // scan then observes the entry store rather than the exit store, so
  // the entry store must order the exited payload before teardown.
  folly::detail::ThreadCachedReaders readers;
  int data = 0;
  std::atomic<bool> entered{false};
  std::atomic<bool> done{false};
  std::thread reader([&] {
    readers.increment(0);
    data = 1;
    readers.decrement();
    readers.increment(1);
    entered.store(true, std::memory_order_release);
    while (!done.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    readers.decrement();
  });
  while (!entered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  EXPECT_TRUE(readers.epochIsClear(0));
  data = 2;
  done.store(true, std::memory_order_release);
  reader.join();
  EXPECT_EQ(data, 2);
}

TEST(RcuTest, DeeplyNestedReaders) {
  std::vector<std::thread> readers;
  std::atomic<int*> int_ptr = std::atomic<int*>(nullptr);
  int_ptr.store(new int(0), std::memory_order_release);
  for (unsigned th = 0; th < 32; th++) {
    readers.emplace_back([&]() {
      std::vector<std::unique_lock<rcu_domain>> domain_readers;
      for (unsigned i = 0; i < 8192; i++) {
        domain_readers.emplace_back(rcu_default_domain());
        EXPECT_EQ(*(int_ptr.load(std::memory_order_acquire)), 0);
      }
    });
  }

  folly::relaxed_atomic<bool> done{false};
  auto updater = std::thread([&]() {
    while (!done) {
      auto newint = new int(0);
      auto oldint = int_ptr.exchange(newint, std::memory_order_acq_rel);
      delete_or_retire_oldint(oldint);
    }
  });
  for (auto& t : readers) {
    t.join();
  }
  done = true;
  updater.join();

  // Clean up to avoid ASAN complaining about a leak.
  rcu_synchronize();
  delete int_ptr.exchange(nullptr, std::memory_order_acq_rel);
}

TEST(RcuTest, DeadLock) {
  // The test tries to reproduce the potential deadlock caused by 2 threads.
  // T1 does rcu_domain::lock(), T2 starts rcu_domain::synchronize(), T1 tries
  // to call rcu_domain::call(). Which made deadlock happens.

  static folly::Indestructible<folly::rcu_domain> domain;

  folly::test::Barrier ready(2);
  char* to_retire = new char[50];
  std::thread t1([&] {
    std::scoped_lock<folly::rcu_domain> guard(domain);
    ready.wait();
    strcpy(to_retire, "Spend some time to guarantee deadlock situation.");
    std::cout << to_retire << std::endl;
    folly::rcu_retire(to_retire, std::default_delete<char[]>(), domain);
  });
  std::thread t2([&] {
    ready.wait();
    folly::rcu_synchronize(domain);
  });

  t1.join();
  t2.join();
  folly::rcu_synchronize(domain);
}
