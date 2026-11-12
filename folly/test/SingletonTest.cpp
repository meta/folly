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

#include <folly/Singleton.h>

#include <cstdlib>
#include <thread>

#include <glog/logging.h>
#include <folly/synchronization/test/Barrier.h>

#include <folly/io/async/EventBase.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <folly/test/SingletonTestStructs.h>
#include <folly/test/TestUtils.h>

FOLLY_GNU_DISABLE_WARNING("-Wdeprecated-declarations")

using namespace folly;
using namespace std::chrono_literals;

TEST(Singleton, MissingSingleton) {
  EXPECT_DEATH(
      []() { auto u = Singleton<UnregisteredWatchdog>::try_get(); }(), "");
  EXPECT_DEATH(
      []() {
        Singleton<UnregisteredWatchdog>::apply([](auto* v) { return v; });
      }(),
      "");
}

struct BasicUsageTag {};
template <typename T, typename Tag = detail::DefaultTag>
using SingletonBasicUsage = Singleton<T, Tag, BasicUsageTag>;

// Exercise some basic codepaths ensuring registration order and
// destruction order happen as expected, that instances are created
// when expected, etc etc.
TEST(Singleton, BasicUsage) {
  auto& vault = *SingletonVault::singleton<BasicUsageTag>();

  EXPECT_EQ(vault.registeredSingletonCount(), 0);
  SingletonBasicUsage<Watchdog> watchdog_singleton;
  EXPECT_EQ(vault.registeredSingletonCount(), 1);

  SingletonBasicUsage<ChildWatchdog> child_watchdog_singleton;
  EXPECT_EQ(vault.registeredSingletonCount(), 2);

  vault.registrationComplete();

  // limit a scope to release references so we can destroy them later
  {
    std::shared_ptr<Watchdog> s1 = SingletonBasicUsage<Watchdog>::try_get();
    EXPECT_NE(s1, nullptr);

    SingletonBasicUsage<Watchdog>::apply([](auto*) {});
    SUCCEED();
    auto w1 = SingletonBasicUsage<Watchdog>::apply([](auto* v) { return v; });
    EXPECT_NE(w1, nullptr);

    std::shared_ptr<Watchdog> s2 = SingletonBasicUsage<Watchdog>::try_get();
    EXPECT_NE(s2, nullptr);
    auto w2 = SingletonBasicUsage<Watchdog>::apply([](auto* v) { return v; });
    EXPECT_NE(w2, nullptr);

    EXPECT_EQ(s1, s2);
    EXPECT_EQ(w1, w2);

    EXPECT_EQ(s1.get(), SingletonBasicUsage<Watchdog>::try_get_fast().get());
    EXPECT_EQ(w1, SingletonBasicUsage<Watchdog>::try_get_fast().get());

    std::shared_ptr<ChildWatchdog> s3 =
        SingletonBasicUsage<ChildWatchdog>::try_get();
    EXPECT_NE(s3, nullptr);
    EXPECT_NE(s2, s3);

    SingletonBasicUsage<ChildWatchdog>::apply([](auto*) {});
    SUCCEED();
    auto w3 = SingletonBasicUsage<ChildWatchdog>::apply([](auto* v) {
      return v;
    });
    EXPECT_NE(w3, nullptr);
    EXPECT_NE(w2, w3);

    EXPECT_EQ(vault.registeredSingletonCount(), 2);
    EXPECT_EQ(vault.livingSingletonCount(), 2);
  }

  vault.destroyInstances();
  EXPECT_EQ(vault.registeredSingletonCount(), 2);
  EXPECT_EQ(vault.livingSingletonCount(), 0);
}

struct DirectUsageTag {};
template <typename T, typename Tag = detail::DefaultTag>
using SingletonDirectUsage = Singleton<T, Tag, DirectUsageTag>;

TEST(Singleton, DirectUsage) {
  auto& vault = *SingletonVault::singleton<DirectUsageTag>();

  EXPECT_EQ(vault.registeredSingletonCount(), 0);

  // Verify we can get to the underlying singletons via directly using
  // the singleton definition.
  SingletonDirectUsage<Watchdog> watchdog;
  struct TestTag {};
  SingletonDirectUsage<Watchdog, TestTag> named_watchdog;
  EXPECT_EQ(vault.registeredSingletonCount(), 2);
  vault.registrationComplete();

  EXPECT_NE(watchdog.try_get(), nullptr);
  EXPECT_EQ(watchdog.try_get(), SingletonDirectUsage<Watchdog>::try_get());
  EXPECT_NE(watchdog.try_get(), named_watchdog.try_get());
  EXPECT_EQ(watchdog.try_get()->livingWatchdogCount(), 2);

  watchdog.apply([](auto*) {});
  SingletonDirectUsage<Watchdog>::apply([](auto*) {});
  named_watchdog.apply([](auto*) {});
  SUCCEED();

  // Cannot use the Singleton::apply directly in the EXPECT_* macros due to
  // "error: lambda expression in an unevaluated operand"
  auto w1 = watchdog.apply([](auto* v) { return v; });
  auto w2 = SingletonDirectUsage<Watchdog>::apply([](auto* v) { return v; });
  auto w3 = named_watchdog.apply([](auto* v) { return v; });
  EXPECT_NE(w1, nullptr);
  EXPECT_EQ(w1, w2);
  EXPECT_NE(w1, w3);
  auto count = watchdog.apply([](auto* v) { return v->livingWatchdogCount(); });
  EXPECT_EQ(count, 2);

  vault.destroyInstances();
}

struct NamedUsageTag {};
template <typename T, typename Tag = detail::DefaultTag>
using SingletonNamedUsage = Singleton<T, Tag, NamedUsageTag>;

TEST(Singleton, NamedUsage) {
  auto& vault = *SingletonVault::singleton<NamedUsageTag>();

  EXPECT_EQ(vault.registeredSingletonCount(), 0);

  // Define two named Watchdog singletons and one unnamed singleton.
  struct Watchdog1 {};
  struct Watchdog2 {};
  using Watchdog3 = detail::DefaultTag;
  SingletonNamedUsage<Watchdog, Watchdog1> watchdog1_singleton;
  EXPECT_EQ(vault.registeredSingletonCount(), 1);
  SingletonNamedUsage<Watchdog, Watchdog2> watchdog2_singleton;
  EXPECT_EQ(vault.registeredSingletonCount(), 2);
  SingletonNamedUsage<Watchdog, Watchdog3> watchdog3_singleton;
  EXPECT_EQ(vault.registeredSingletonCount(), 3);

  vault.registrationComplete();
  {
    // Verify our three singletons are distinct and non-nullptr.
    auto s1 = SingletonNamedUsage<Watchdog, Watchdog1>::try_get();
    EXPECT_EQ(s1, watchdog1_singleton.try_get());
    auto s2 = SingletonNamedUsage<Watchdog, Watchdog2>::try_get();
    EXPECT_EQ(s2, watchdog2_singleton.try_get());
    EXPECT_NE(s1, s2);
    auto s3 = SingletonNamedUsage<Watchdog, Watchdog3>::try_get();
    EXPECT_EQ(s3, watchdog3_singleton.try_get());
    EXPECT_NE(s3, s1);
    EXPECT_NE(s3, s2);

    auto w1 = SingletonNamedUsage<Watchdog, Watchdog1>::apply([](auto* v) {
      return v;
    });
    auto wd1 = watchdog1_singleton.apply([](auto* v) { return v; });
    EXPECT_EQ(w1, wd1);
    auto w2 = SingletonNamedUsage<Watchdog, Watchdog2>::apply([](auto* v) {
      return v;
    });
    auto wd2 = watchdog2_singleton.apply([](auto* v) { return v; });
    EXPECT_EQ(w2, wd2);
    EXPECT_NE(w1, w2);
    auto w3 = SingletonNamedUsage<Watchdog, Watchdog3>::apply([](auto* v) {
      return v;
    });
    auto wd3 = watchdog3_singleton.apply([](auto* v) { return v; });
    EXPECT_EQ(w3, wd3);
    EXPECT_NE(w3, w1);
    EXPECT_NE(w3, w2);

    // Verify the "default" singleton is the same as the DefaultTag-tagged
    // singleton.
    auto s4 = SingletonNamedUsage<Watchdog>::try_get();
    EXPECT_EQ(s4, watchdog3_singleton.try_get());

    auto w4 = SingletonNamedUsage<Watchdog>::apply([](auto* v) { return v; });
    EXPECT_EQ(w4, wd3);
  }

  vault.destroyInstances();
}

struct NaughtyUsageTag {};
template <typename T, typename Tag = detail::DefaultTag>
using SingletonNaughtyUsage = Singleton<T, Tag, NaughtyUsageTag>;
struct NaughtyUsageTag2 {};
template <typename T, typename Tag = detail::DefaultTag>
using SingletonNaughtyUsage2 = Singleton<T, Tag, NaughtyUsageTag2>;

// Some pathological cases such as getting unregistered singletons,
// double registration, etc.
TEST(Singleton, NaughtyUsage) {
  auto& vault = *SingletonVault::singleton<NaughtyUsageTag>();

  const char* kBeforeRegistrationCompleteMsg =
      ".*Singleton .* requested before registrationComplete().*";
  SingletonNaughtyUsage2<int> s0;
  EXPECT_DEATH(s0.try_get(), kBeforeRegistrationCompleteMsg);

  vault.registrationComplete();

  const char* kUnregisteredMsg =
      ".*Creating instance for unregistered singleton.*";
  EXPECT_DEATH(Singleton<Watchdog>::try_get(), kUnregisteredMsg);
  EXPECT_DEATH(
      Singleton<Watchdog>::apply([](auto* v) { return v; }), kUnregisteredMsg);
  EXPECT_DEATH(SingletonNaughtyUsage<Watchdog>::try_get(), kUnregisteredMsg);
  EXPECT_DEATH(
      SingletonNaughtyUsage<Watchdog>::apply([](auto* v) { return v; }),
      kUnregisteredMsg);

  vault.destroyInstances();

  auto& vault2 = *SingletonVault::singleton<NaughtyUsageTag2>();

  EXPECT_DEATH(SingletonNaughtyUsage2<Watchdog>::try_get(), kUnregisteredMsg);
  EXPECT_DEATH(
      SingletonNaughtyUsage2<Watchdog>::apply([](auto* v) { return v; }),
      kUnregisteredMsg);
  SingletonNaughtyUsage2<Watchdog> watchdog_singleton;

  const char* kDoubleRegistration =
      "Double registration of singletons of the same underlying type";
  EXPECT_DEATH(
      [] { SingletonNaughtyUsage2<Watchdog> w2; }(), kDoubleRegistration);
  vault2.destroyInstances();

  // Double registration after destroy.
  EXPECT_DEATH(
      [] { SingletonNaughtyUsage2<Watchdog> w3; }(), kDoubleRegistration);
}

struct LateRegistrationTag {};
template <typename T, typename Tag = detail::DefaultTag>
using SingletonLateRegistration = Singleton<T, Tag, LateRegistrationTag>;

TEST(Singleton, LateRegistrationLogsType) {
  auto& vault = *SingletonVault::singleton<LateRegistrationTag>();
  vault.setType(SingletonVault::Type::Strict);
  vault.registrationComplete();

  testing::internal::CaptureStderr();
  SingletonLateRegistration<Watchdog> watchdog_singleton;
  const auto output = testing::internal::GetCapturedStderr();

  EXPECT_THAT(
      output,
      testing::HasSubstr(
          "Registering singleton after registrationComplete(). "
          "Singleton type is: Watchdog"));

  vault.destroyInstances();
}

struct SharedPtrUsageTag {};
template <typename T, typename Tag = detail::DefaultTag>
using SingletonSharedPtrUsage = Singleton<T, Tag, SharedPtrUsageTag>;

// TODO (anob): revisit this test
TEST(Singleton, SharedPtrUsage) {
  struct WatchdogHolder {
    ~WatchdogHolder() {
      if (watchdog) {
        LOG(ERROR) << "The following log message with stack trace is expected";
      }
    }

    std::shared_ptr<Watchdog> watchdog;
  };

  auto& vault = *SingletonVault::singleton<SharedPtrUsageTag>();

  EXPECT_EQ(vault.registeredSingletonCount(), 0);
  std::vector<std::unique_ptr<Watchdog>> watchdog_instances;
  SingletonSharedPtrUsage<Watchdog> watchdog_singleton(
      [&] {
        watchdog_instances.push_back(std::make_unique<Watchdog>());
        return watchdog_instances.back().get();
      },
      [&](Watchdog* ptr) {
        // Make sure that only second instance is destroyed. First instance is
        // expected to be leaked.
        EXPECT_EQ(watchdog_instances[1].get(), ptr);
        watchdog_instances[1].reset();
      });
  EXPECT_EQ(vault.registeredSingletonCount(), 1);

  SingletonSharedPtrUsage<ChildWatchdog> child_watchdog_singleton;
  EXPECT_EQ(vault.registeredSingletonCount(), 2);

  struct ATag {};
  SingletonSharedPtrUsage<Watchdog, ATag> named_watchdog_singleton;

  SingletonSharedPtrUsage<WatchdogHolder> watchdog_holder_singleton;

  vault.registrationComplete();

  // Initilize holder singleton first, so that it's the last one to be
  // destroyed.
  watchdog_holder_singleton.try_get();
  watchdog_holder_singleton.apply([](auto* v) { return v; });

  auto s1 = SingletonSharedPtrUsage<Watchdog>::try_get().get();
  EXPECT_NE(s1, nullptr);

  auto w1 = SingletonSharedPtrUsage<Watchdog>::apply([](auto* v) { return v; });
  EXPECT_NE(w1, nullptr);

  auto s2 = SingletonSharedPtrUsage<Watchdog>::try_get().get();
  EXPECT_NE(s2, nullptr);

  auto w2 = SingletonSharedPtrUsage<Watchdog>::apply([](auto* v) { return v; });
  EXPECT_NE(w2, nullptr);

  EXPECT_EQ(s1, s2);

  EXPECT_EQ(w1, w2);

  auto weak_s1 = SingletonSharedPtrUsage<Watchdog>::get_weak();

  auto shared_s1 = weak_s1.lock();
  EXPECT_EQ(shared_s1.get(), s1);

  auto old_serial = shared_s1->serial_number;

  {
    auto named_weak_s1 = SingletonSharedPtrUsage<Watchdog, ATag>::get_weak();
    auto locked = named_weak_s1.lock();
    EXPECT_NE(locked.get(), shared_s1.get());
  }

  // We should release externally locked shared_ptr, otherwise it will be
  // considered a leak
  watchdog_holder_singleton.try_get()->watchdog = shared_s1;
  watchdog_holder_singleton.apply([&](auto* v) {
    v->watchdog = std::move(shared_s1);
  });

  LOG(ERROR) << "The following log message regarding shared_ptr is expected";
  {
    auto start_time = std::chrono::steady_clock::now();
    vault.destroyInstances();
    auto duration = std::chrono::steady_clock::now() - start_time;
    EXPECT_TRUE(duration > std::chrono::seconds{4});
  }
  EXPECT_EQ(vault.registeredSingletonCount(), 4);
  EXPECT_EQ(vault.livingSingletonCount(), 0);

  EXPECT_TRUE(weak_s1.expired());

  auto empty_s1 = SingletonSharedPtrUsage<Watchdog>::get_weak();
  EXPECT_FALSE(empty_s1.lock());

  vault.reenableInstances();

  {
    // Singleton should be re-created only after reenableInstances() was called.
    auto new_s1 = SingletonSharedPtrUsage<Watchdog>::try_get();
    auto new_w1 = SingletonSharedPtrUsage<Watchdog>::apply([](auto* v) {
      return v;
    });
    // Track serial number rather than pointer since the memory could be
    // re-used when we create new_s1.
    EXPECT_NE(new_s1->serial_number, old_serial);
    EXPECT_NE(new_w1->serial_number, old_serial);
  }

  auto new_s1_weak = SingletonSharedPtrUsage<Watchdog>::get_weak();
  auto new_s1_shared = new_s1_weak.lock();
  std::thread t([new_s1_shared]() mutable {
    std::this_thread::sleep_for(std::chrono::seconds{2});
    new_s1_shared.reset();
  });
  new_s1_shared.reset();
  {
    auto start_time = std::chrono::steady_clock::now();
    vault.destroyInstances();
    auto duration = std::chrono::steady_clock::now() - start_time;
    EXPECT_TRUE(
        duration > std::chrono::seconds{1} &&
        duration < std::chrono::seconds{3});
  }
  EXPECT_TRUE(new_s1_weak.expired());
  t.join();
}

// Some classes to test singleton dependencies.  NeedySingleton has a
// dependency on NeededSingleton, which happens during its
// construction.
struct NeedyTag {};
template <typename T, typename Tag = detail::DefaultTag>
using SingletonNeedy = Singleton<T, Tag, NeedyTag>;

struct NeededSingleton {};
struct NeedySingleton {
  NeedySingleton() {
    auto unused1 = SingletonNeedy<NeededSingleton>::try_get();
    EXPECT_NE(unused1, nullptr);

    auto unused2 = SingletonNeedy<NeededSingleton>::apply([](auto* v) {
      return v;
    });
    EXPECT_NE(unused2, nullptr);
  }
};

// Ensure circular dependencies fail -- a singleton that needs itself, whoops.
struct SelfNeedyTag {};
template <typename T, typename Tag = detail::DefaultTag>
using SingletonSelfNeedy = Singleton<T, Tag, SelfNeedyTag>;

struct SelfNeedySingleton {
  SelfNeedySingleton() {
    auto unused1 = SingletonSelfNeedy<SelfNeedySingleton>::try_get();
    EXPECT_NE(unused1, nullptr);

    auto unused2 = SingletonSelfNeedy<SelfNeedySingleton>::apply([](auto v) {
      return v;
    });
    EXPECT_NE(unused2, nullptr);
  }
};

TEST(Singleton, SingletonDependencies) {
  SingletonNeedy<NeededSingleton> needed_singleton;
  SingletonNeedy<NeedySingleton> needy_singleton;
  auto& needy_vault = *SingletonVault::singleton<NeedyTag>();

  needy_vault.registrationComplete();

  EXPECT_EQ(needy_vault.registeredSingletonCount(), 2);
  EXPECT_EQ(needy_vault.livingSingletonCount(), 0);

  auto needy = SingletonNeedy<NeedySingleton>::try_get();
  EXPECT_EQ(needy_vault.livingSingletonCount(), 2);

  auto another_needy = SingletonNeedy<NeedySingleton>::apply([](auto* v) {
    return v;
  });
  (void)another_needy;
  EXPECT_EQ(needy_vault.livingSingletonCount(), 2);

  SingletonSelfNeedy<SelfNeedySingleton> self_needy_singleton;
  auto& self_needy_vault = *SingletonVault::singleton<SelfNeedyTag>();

  self_needy_vault.registrationComplete();
  EXPECT_DEATH(
      []() { SingletonSelfNeedy<SelfNeedySingleton>::try_get(); }(), "");
  EXPECT_DEATH(
      []() {
        SingletonSelfNeedy<SelfNeedySingleton>::apply([](auto* v) {
          return v;
        });
      }(),
      "");
}

// A test to ensure multiple threads contending on singleton creation
// properly wait for creation rather than thinking it is a circular
// dependency.
class Slowpoke : public Watchdog {
 public:
  Slowpoke() { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
};

struct ConcurrencyTag {};
template <typename T, typename Tag = detail::DefaultTag>
using SingletonConcurrency = Singleton<T, Tag, ConcurrencyTag>;

TEST(Singleton, SingletonConcurrency) {
  auto& vault = *SingletonVault::singleton<ConcurrencyTag>();
  SingletonConcurrency<Slowpoke> slowpoke_singleton;
  vault.registrationComplete();

  std::mutex gatekeeper;
  gatekeeper.lock();
  auto func = [&gatekeeper]() {
    gatekeeper.lock();
    gatekeeper.unlock();
    auto unused1 = SingletonConcurrency<Slowpoke>::try_get();
    auto unused2 = SingletonConcurrency<Slowpoke>::apply([](auto* v) {
      return v;
    });
    EXPECT_EQ(unused1.get(), unused2);
  };

  EXPECT_EQ(vault.livingSingletonCount(), 0);
  std::vector<std::thread> threads;
  for (int i = 0; i < 100; ++i) {
    threads.emplace_back(func);
  }
  // If circular dependency checks fail, the unlock would trigger a
  // crash.  Instead, it succeeds, and we have exactly one living
  // singleton.
  gatekeeper.unlock();
  for (auto& t : threads) {
    t.join();
  }
  EXPECT_EQ(vault.livingSingletonCount(), 1);
}

struct ErrorConstructor {
  static size_t constructCount_;
  ErrorConstructor() {
    if ((constructCount_++) == 0) {
      throw std::runtime_error("first time fails");
    }
  }
};
size_t ErrorConstructor::constructCount_(0);

struct CreationErrorTag {};
template <typename T, typename Tag = detail::DefaultTag>
using SingletonCreationError = Singleton<T, Tag, CreationErrorTag>;

TEST(Singleton, SingletonCreationError) {
  SingletonCreationError<ErrorConstructor> error_once_singleton;
  SingletonVault::singleton<CreationErrorTag>()->registrationComplete();

  // first time should error out
  EXPECT_THROW(error_once_singleton.try_get(), std::runtime_error);

  // second time it'll work fine
  error_once_singleton.apply([](auto* v) { return v; });
  SUCCEED();
}

struct ConcurrencyStressTag {};
template <typename T, typename Tag = detail::DefaultTag>
using SingletonConcurrencyStress = Singleton<T, Tag, ConcurrencyStressTag>;

TEST(Singleton, SingletonConcurrencyStress) {
  auto& vault = *SingletonVault::singleton<ConcurrencyStressTag>();
  SingletonConcurrencyStress<Slowpoke> slowpoke_singleton;
  vault.registrationComplete();

  std::vector<std::thread> ts;
  for (size_t i = 0; i < 100; ++i) {
    ts.emplace_back([i, &slowpoke_singleton]() {
      if ((i % 2) == 0) {
        slowpoke_singleton.try_get();
      } else {
        slowpoke_singleton.apply([](auto) {});
      }
    });
  }

  for (size_t i = 0; i < 100; ++i) {
    std::chrono::milliseconds d(20);

    std::this_thread::sleep_for(d);
    vault.destroyInstances();
    std::this_thread::sleep_for(d);
    vault.destroyInstances();
  }

  for (auto& t : ts) {
    t.join();
  }
}

namespace {
struct EagerInitSyncTag {};
} // namespace
template <typename T, typename Tag = detail::DefaultTag>
using SingletonEagerInitSync = Singleton<T, Tag, EagerInitSyncTag>;
TEST(Singleton, SingletonEagerInitSync) {
  auto& vault = *SingletonVault::singleton<EagerInitSyncTag>();
  bool didEagerInit = false;
  auto sing =
      SingletonEagerInitSync<std::string>([&] {
        didEagerInit = true;
        return new std::string("foo");
      }).shouldEagerInit();
  vault.registrationComplete();
  EXPECT_FALSE(didEagerInit);
  vault.doEagerInit();
  EXPECT_TRUE(didEagerInit);
  sing.get_weak(); // (avoid compile error complaining about unused var 'sing')
}

namespace {
struct EagerInitAsyncTag {};
} // namespace
template <typename T, typename Tag = detail::DefaultTag>
using SingletonEagerInitAsync = Singleton<T, Tag, EagerInitAsyncTag>;
TEST(Singleton, SingletonEagerInitAsync) {
  auto& vault = *SingletonVault::singleton<EagerInitAsyncTag>();
  bool didEagerInit = false;
  auto sing =
      SingletonEagerInitAsync<std::string>([&] {
        didEagerInit = true;
        return new std::string("foo");
      }).shouldEagerInit();
  folly::EventBase eb;
  folly::Baton<> done;
  vault.registrationComplete();
  EXPECT_FALSE(didEagerInit);
  vault.doEagerInitVia(eb, &done);
  eb.loop();
  done.wait();
  EXPECT_TRUE(didEagerInit);
  sing.get_weak(); // (avoid compile error complaining about unused var 'sing')
}

namespace {
class TestEagerInitParallelExecutor : public folly::Executor {
 public:
  explicit TestEagerInitParallelExecutor(const size_t threadCount) {
    eventBases_.reserve(threadCount);
    threads_.reserve(threadCount);
    for (size_t i = 0; i < threadCount; i++) {
      eventBases_.push_back(std::make_shared<folly::EventBase>());
      auto eb = eventBases_.back();
      threads_.emplace_back(std::make_shared<std::thread>([eb] {
        eb->loopForever();
      }));
    }
  }

  ~TestEagerInitParallelExecutor() override {
    for (auto eb : eventBases_) {
      eb->runInEventBaseThread([eb] { eb->terminateLoopSoon(); });
    }
    for (auto thread : threads_) {
      thread->join();
    }
  }

  void add(folly::Func func) override {
    const auto index = (counter_++) % eventBases_.size();
    eventBases_[index]->add(std::move(func));
  }

 private:
  std::vector<std::shared_ptr<folly::EventBase>> eventBases_;
  std::vector<std::shared_ptr<std::thread>> threads_;
  std::atomic<size_t> counter_{0};
};
} // namespace

namespace {
struct EagerInitParallelTag {};
} // namespace
template <typename T, typename Tag = detail::DefaultTag>
using SingletonEagerInitParallel = Singleton<T, Tag, EagerInitParallelTag>;
TEST(Singleton, SingletonEagerInitParallel) {
  const static size_t kIters = 1000;
  const static size_t kThreads = 20;

  std::atomic<size_t> initCounter{0};

  auto& vault = *SingletonVault::singleton<EagerInitParallelTag>();

  auto sing =
      SingletonEagerInitParallel<std::string>([&] {
        ++initCounter;
        return new std::string("");
      }).shouldEagerInit();

  for (size_t i = 0; i < kIters; i++) {
    SCOPE_EXIT {
      // clean up each time
      vault.destroyInstances();
      vault.reenableInstances();
    };

    initCounter.store(0);

    {
      std::vector<std::shared_ptr<std::thread>> threads;
      folly::test::Barrier barrier(kThreads);
      TestEagerInitParallelExecutor exe(kThreads);
      vault.registrationComplete();

      EXPECT_EQ(0, initCounter.load());

      for (size_t j = 0; j < kThreads; j++) {
        threads.push_back(std::make_shared<std::thread>([&] {
          barrier.wait();
          vault.doEagerInitVia(exe);
        }));
      }

      for (auto thread : threads) {
        thread->join();
      }
    }

    EXPECT_EQ(1, initCounter.load());

    sing.get_weak(); // (avoid compile error complaining about unused var)
  }
}

struct StateTestTag {};
template <typename T, typename Tag = detail::DefaultTag>
using SingletonVaultStateTest = Singleton<T, Tag, StateTestTag>;

TEST(Singleton, SingletonVaultStateTest) {
  auto& vault = *SingletonVault::singleton<StateTestTag>();
  // vault must not be disabled after construction
  EXPECT_FALSE(vault.isDisabled());

  EXPECT_EQ(vault.registeredSingletonCount(), 0);
  SingletonVaultStateTest<Watchdog> watchdog_singleton;
  EXPECT_EQ(vault.registeredSingletonCount(), 1);

  // vault must not be disabled after adding a singleton
  EXPECT_FALSE(vault.isDisabled());

  vault.registrationComplete();

  // vault must not be disabled after registration is complete
  EXPECT_FALSE(vault.isDisabled());

  vault.destroyInstances();

  // vault must be disabled after destroying instances
  EXPECT_TRUE(vault.isDisabled());

  vault.reenableInstances();

  // vault must not be disabled after reenabling instances
  EXPECT_FALSE(vault.isDisabled());
  EXPECT_EQ(vault.registeredSingletonCount(), 1);
}

struct MockTag {};
template <typename T, typename Tag = detail::DefaultTag>
using SingletonMock = Singleton<T, Tag, MockTag>;

// Verify that existing Singleton's can be overridden
// using the make_mock functionality.
TEST(Singleton, MockTest) {
  auto& vault = *SingletonVault::singleton<MockTag>();

  SingletonMock<Watchdog> watchdog_singleton;
  vault.registrationComplete();

  // Registring singletons after registrationComplete called works
  // with make_mock (but not with Singleton ctor).
  EXPECT_EQ(vault.registeredSingletonCount(), 1);
  int serial_count_first = SingletonMock<Watchdog>::try_get()->serial_number;

  // Override existing mock using make_mock.
  SingletonMock<Watchdog>::make_mock();

  EXPECT_EQ(vault.registeredSingletonCount(), 1);
  int serial_count_mock = SingletonMock<Watchdog>::try_get()->serial_number;

  // If serial_count value is the same, then singleton was not replaced.
  EXPECT_NE(serial_count_first, serial_count_mock);

  // Override existing mock using make_mock one more time
  SingletonMock<Watchdog>::make_mock();

  EXPECT_EQ(vault.registeredSingletonCount(), 1);
  int serial_count_mock2 = SingletonMock<Watchdog>::try_get()->serial_number;

  // If serial_count value is the same, then singleton was not replaced.
  EXPECT_NE(serial_count_first, serial_count_mock2);
  EXPECT_NE(serial_count_mock, serial_count_mock2);

  vault.destroyInstances();
}

TEST(Singleton, MockTestWithApply) {
  auto& vault = *SingletonVault::singleton<MockTag>();

  SingletonMock<Watchdog> watchdog_singleton;
  vault.registrationComplete();

  // Registring singletons after registrationComplete called works
  // with make_mock (but not with Singleton ctor).
  EXPECT_EQ(vault.registeredSingletonCount(), 1);
  int serial_count_first = SingletonMock<Watchdog>::apply([](auto* v) {
    return v->serial_number;
  });

  // Override existing mock using make_mock.
  SingletonMock<Watchdog>::make_mock();

  EXPECT_EQ(vault.registeredSingletonCount(), 1);
  int serial_count_mock = SingletonMock<Watchdog>::apply([](auto* v) {
    return v->serial_number;
  });

  // If serial_count value is the same, then singleton was not replaced.
  EXPECT_NE(serial_count_first, serial_count_mock);

  // Override existing mock using make_mock one more time
  SingletonMock<Watchdog>::make_mock();

  EXPECT_EQ(vault.registeredSingletonCount(), 1);
  int serial_count_mock2 = SingletonMock<Watchdog>::apply([](auto* v) {
    return v->serial_number;
  });

  // If serial_count value is the same, then singleton was not replaced.
  EXPECT_NE(serial_count_first, serial_count_mock2);
  EXPECT_NE(serial_count_mock, serial_count_mock2);

  vault.destroyInstances();
}

// Singleton using a non default constructor test/example:
struct X {
  X() : X(-1, "unset") {}
  X(int a1_, std::string a2_) : a1(a1_), a2(a2_) {
    LOG(INFO) << "X(" << a1 << "," << a2 << ")";
  }
  const int a1;
  const std::string a2;
};

folly::Singleton<X> singleton_x([]() { return new X(42, "foo"); });

TEST(Singleton, CustomCreator) {
  X x1;
  std::shared_ptr<X> x2p = singleton_x.try_get();
  EXPECT_NE(nullptr, x2p);
  EXPECT_NE(x1.a1, x2p->a1);
  EXPECT_NE(x1.a2, x2p->a2);
  EXPECT_EQ(42, x2p->a1);
  EXPECT_EQ(std::string("foo"), x2p->a2);
}

TEST(Singleton, CustomCreatorViaApply) {
  X x1;
  X* x3p = singleton_x.apply([](auto* v) { return v; });
  EXPECT_NE(nullptr, x3p);
  EXPECT_NE(x1.a1, x3p->a1);
  EXPECT_NE(x1.a2, x3p->a2);
  EXPECT_EQ(42, x3p->a1);
  EXPECT_EQ(std::string("foo"), x3p->a2);
}

struct ConcurrentCreationDestructionTag {};
template <typename T, typename Tag = detail::DefaultTag>
using SingletonConcurrentCreationDestruction =
    Singleton<T, Tag, ConcurrentCreationDestructionTag>;

folly::Baton<> slowpokeNeedySingletonBaton;

struct SlowpokeNeedySingleton {
  SlowpokeNeedySingleton() {
    slowpokeNeedySingletonBaton.post();
    /* sleep override */ std::this_thread::sleep_for(
        std::chrono::milliseconds(100));
    auto unused1 =
        SingletonConcurrentCreationDestruction<NeededSingleton>::try_get();
    EXPECT_NE(unused1, nullptr);

    auto unused2 =
        SingletonConcurrentCreationDestruction<NeededSingleton>::apply(
            [](auto* v) { return v; });
    EXPECT_NE(unused2, nullptr);
  }
};

TEST(Singleton, ConcurrentCreationDestruction) {
  auto& vault = *SingletonVault::singleton<ConcurrentCreationDestructionTag>();
  SingletonConcurrentCreationDestruction<NeededSingleton> neededSingleton;
  SingletonConcurrentCreationDestruction<SlowpokeNeedySingleton> needySingleton;
  vault.registrationComplete();

  std::thread needyThread([&] {
    needySingleton.apply([](auto* v) { return v; });
  });

  slowpokeNeedySingletonBaton.wait();

  vault.destroyInstances();

  needyThread.join();
}

struct MainThreadDestructorTag {};
template <typename T, typename Tag = detail::DefaultTag>
using SingletonMainThreadDestructor =
    Singleton<T, Tag, MainThreadDestructorTag>;

struct ThreadLoggingSingleton {
  ThreadLoggingSingleton() { initThread = std::this_thread::get_id(); }

  ~ThreadLoggingSingleton() { destroyThread = std::this_thread::get_id(); }

  static std::thread::id initThread;
  static std::thread::id destroyThread;
};
std::thread::id ThreadLoggingSingleton::initThread{};
std::thread::id ThreadLoggingSingleton::destroyThread{};

TEST(Singleton, MainThreadDestructor) {
  auto& vault = *SingletonVault::singleton<MainThreadDestructorTag>();
  SingletonMainThreadDestructor<ThreadLoggingSingleton> singleton;

  vault.registrationComplete();
  EXPECT_EQ(std::thread::id(), ThreadLoggingSingleton::initThread);

  singleton.try_get();
  EXPECT_EQ(std::this_thread::get_id(), ThreadLoggingSingleton::initThread);

  std::thread t([instance = singleton.try_get()] {
    /* sleep override */ std::this_thread::sleep_for(
        std::chrono::milliseconds{100});
  });

  EXPECT_EQ(std::thread::id(), ThreadLoggingSingleton::destroyThread);

  vault.destroyInstances();
  EXPECT_EQ(std::this_thread::get_id(), ThreadLoggingSingleton::destroyThread);

  t.join();
}

TEST(Singleton, MainThreadDestructorWithApply) {
  auto& vault = *SingletonVault::singleton<MainThreadDestructorTag>();
  SingletonMainThreadDestructor<ThreadLoggingSingleton> singleton;

  vault.registrationComplete();
  EXPECT_EQ(std::thread::id(), ThreadLoggingSingleton::initThread);

  singleton.apply([](auto* v) { return v; });
  EXPECT_EQ(std::this_thread::get_id(), ThreadLoggingSingleton::initThread);

  std::thread t([instance = singleton.apply([](auto* v) { return v; })] {
    /* sleep override */ std::this_thread::sleep_for(
        std::chrono::milliseconds{100});
  });

  EXPECT_EQ(std::thread::id(), ThreadLoggingSingleton::destroyThread);

  vault.destroyInstances();
  EXPECT_EQ(std::this_thread::get_id(), ThreadLoggingSingleton::destroyThread);

  t.join();
}

TEST(Singleton, DoubleMakeMockAfterTryGet) {
  // to keep track of calls to ctor and dtor below
  struct Counts {
    size_t ctor = 0;
    size_t dtor = 0;
  };

  // a test type which keeps track of its ctor and dtor calls
  struct VaultTag {};
  struct PrivateTag {};
  struct Object {
    explicit Object(Counts& counts) : counts_(counts) { ++counts_.ctor; }
    ~Object() { ++counts_.dtor; }
    Counts& counts_;
  };
  using SingletonObject = Singleton<Object, PrivateTag, VaultTag>;

  // register everything
  Counts counts;
  auto& vault = *SingletonVault::singleton<VaultTag>();
  auto new_object = [&] { return new Object(counts); };
  SingletonObject object_(new_object);
  vault.registrationComplete();

  // no eager inits, nada (sanity)
  EXPECT_EQ(0, counts.ctor);
  EXPECT_EQ(0, counts.dtor);

  // explicit request, ctor
  SingletonObject::try_get();
  EXPECT_EQ(1, counts.ctor);
  EXPECT_EQ(0, counts.dtor);

  // first make_mock, dtor (ctor is lazy)
  SingletonObject::make_mock(new_object);
  EXPECT_EQ(1, counts.ctor);
  EXPECT_EQ(1, counts.dtor);

  // second make_mock, nada (dtor already ran, ctor is lazy)
  SingletonObject::make_mock(new_object);
  EXPECT_EQ(1, counts.ctor);
  EXPECT_EQ(1, counts.dtor);
}

TEST(Singleton, DoubleMakeMockAfterTryGetWithApply) {
  // to keep track of calls to ctor and dtor below
  struct Counts {
    size_t ctor = 0;
    size_t dtor = 0;
  };

  // a test type which keeps track of its ctor and dtor calls
  struct VaultTag {};
  struct PrivateTag {};
  struct Object {
    explicit Object(Counts& counts) : counts_(counts) { ++counts_.ctor; }
    ~Object() { ++counts_.dtor; }
    Counts& counts_;
  };
  using SingletonObject = Singleton<Object, PrivateTag, VaultTag>;

  // register everything
  Counts counts;
  auto& vault = *SingletonVault::singleton<VaultTag>();
  auto new_object = [&] { return new Object(counts); };
  SingletonObject object_(new_object);
  vault.registrationComplete();

  // no eager inits, nada (sanity)
  EXPECT_EQ(0, counts.ctor);
  EXPECT_EQ(0, counts.dtor);

  // explicit request, ctor
  SingletonObject::apply([](auto* v) { return v; });
  EXPECT_EQ(1, counts.ctor);
  EXPECT_EQ(0, counts.dtor);

  // first make_mock, dtor (ctor is lazy)
  SingletonObject::make_mock(new_object);
  EXPECT_EQ(1, counts.ctor);
  EXPECT_EQ(1, counts.dtor);

  // second make_mock, nada (dtor already ran, ctor is lazy)
  SingletonObject::make_mock(new_object);
  EXPECT_EQ(1, counts.ctor);
  EXPECT_EQ(1, counts.dtor);
}

TEST(Singleton, LeakySingletonLSAN) {
  struct PrivateTag {};
  static folly::LeakySingleton<int, PrivateTag> gPtr;
  auto* ptr0 = &gPtr.get();
  EXPECT_EQ(*ptr0, 0);
  gPtr.make_mock([] { return new int(1); });
  auto* ptr1 = &gPtr.get();
  EXPECT_NE(ptr0, ptr1);
  EXPECT_EQ(*ptr1, 1);
}

TEST(Singleton, LeakySingletonTSAN) {
  struct PrivateTag {};
  static folly::LeakySingleton<int, PrivateTag> gPtr;
  constexpr size_t kThreads = 100;
  constexpr size_t kIterations = 100;
  folly::test::Barrier start{kThreads + 1};
  folly::test::Barrier complete{kThreads + 1};
  std::atomic<bool> valuesValid{true};

  EXPECT_EQ(gPtr.get(), 0);

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (size_t i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      for (size_t j = 0; j < kIterations; ++j) {
        start.wait();
        const auto value = gPtr.get();
        if (value != 0 && value != 1) {
          valuesValid.store(false, std::memory_order_relaxed);
        }
        complete.wait();
      }
    });
  }

  for (size_t i = 0; i < kIterations; ++i) {
    const auto mockValue = static_cast<int>(i % 2);
    start.wait();
    gPtr.make_mock([mockValue] { return new int(mockValue); });
    complete.wait();
    EXPECT_EQ(gPtr.get(), mockValue);
  }

  for (auto& t : threads) {
    t.join();
  }
  EXPECT_TRUE(valuesValid.load(std::memory_order_relaxed));
}

// TSAN will SIGSEGV if the shutdown timer activates (it spawns a new thread,
// which TSAN doesn't like).
#if !defined(FOLLY_SANITIZE_THREAD)

TEST(Singleton, ShutdownTimer) {
  struct VaultTag {};
  struct PrivateTag {};
  struct Object {
    ~Object() {
      /* sleep override */ std::this_thread::sleep_for(shutdownDuration);
    }

    std::chrono::milliseconds shutdownDuration;
  };
  using SingletonObject = Singleton<Object, PrivateTag, VaultTag>;

  auto& vault = *SingletonVault::singleton<VaultTag>();
  SingletonObject object;
  vault.registrationComplete();

  vault.setShutdownTimeout(10ms);
  SingletonObject::try_get()->shutdownDuration = 10s;
  EXPECT_DEATH(
      [&]() { vault.destroyInstancesFinal(); }(),
      "Failed to complete shutdown within 10ms.");

  vault.setShutdownTimeout(10s);
  SingletonObject::try_get()->shutdownDuration = 10ms;
  vault.destroyInstancesFinal();
}

#endif

TEST(Singleton, ShutdownTimerDisable) {
  struct VaultTag {};
  struct PrivateTag {};
  struct Object {
    ~Object() {
      /* sleep override */ std::this_thread::sleep_for(shutdownDuration);
    }

    std::chrono::milliseconds shutdownDuration;
  };
  using SingletonObject = Singleton<Object, PrivateTag, VaultTag>;

  auto& vault = *SingletonVault::singleton<VaultTag>();
  SingletonObject object;
  vault.registrationComplete();

  vault.disableShutdownTimeout();
  SingletonObject::try_get()->shutdownDuration = 100ms;
  vault.destroyInstancesFinal();
}

TEST(Singleton, ForkInChild) {
  struct VaultTag {};
  struct PrivateTag {};
  struct ForkObject {};
  using SingletonObject = Singleton<ForkObject, PrivateTag, VaultTag>;

  auto& vault = *SingletonVault::singleton<VaultTag>();
  vault.setFailOnUseAfterFork(true);
  SingletonObject object;
  vault.registrationComplete();

  // We use EXPECT_DEATH here to run code in the child process.
  EXPECT_DEATH(
      [&]() {
        object.try_get();
        vault.destroyInstances();

        LOG(FATAL) << "Finished successfully";
      }(),
      "Finished successfully");

  object.try_get();

  if (!folly::kIsDebug) {
    return;
  }

  EXPECT_DEATH(
      [&]() { object.try_get(); }(),
      "Attempting to use singleton .*ForkObject.* in child process");

  EXPECT_DEATH(
      [&]() { vault.destroyInstances(); }(),
      "Attempting to destroy singleton .*ForkObject.* in child process");
}

struct EagerInitOnReenableSingletonsTag {};
template <typename T, typename Tag = detail::DefaultTag>
using SingletonEagerInitOnReenableSingletons =
    Singleton<T, Tag, EagerInitOnReenableSingletonsTag>;

TEST(Singleton, EagerInitOnReenableSingletons) {
  struct CountingSingleton {
    explicit CountingSingleton(int& counter) { ++counter; }
  };

  int counter1{0};
  int counter2{0};
  struct Tag1 {};
  struct Tag2 {};

  auto& vault = *SingletonVault::singleton<EagerInitOnReenableSingletonsTag>();
  auto singleton1 =
      SingletonEagerInitOnReenableSingletons<CountingSingleton, Tag1>([&] {
        return new CountingSingleton(counter1);
      }).shouldEagerInitOnReenable();
  auto singleton2 =
      SingletonEagerInitOnReenableSingletons<CountingSingleton, Tag2>([&] {
        return new CountingSingleton(counter2);
      }).shouldEagerInitOnReenable();
  vault.registrationComplete();

  EXPECT_EQ(0, counter1);
  EXPECT_EQ(0, counter2);

  singleton1.try_get();

  EXPECT_EQ(1, counter1);
  EXPECT_EQ(0, counter2);

  vault.destroyInstances();
  vault.reenableInstances();

  EXPECT_EQ(2, counter1);
  EXPECT_EQ(0, counter2);

  singleton1.try_get();

  EXPECT_EQ(2, counter1);
  EXPECT_EQ(0, counter2);

  vault.destroyInstances();
  vault.reenableInstances();

  EXPECT_EQ(3, counter1);
  EXPECT_EQ(0, counter2);

  singleton2.try_get();

  EXPECT_EQ(3, counter1);
  EXPECT_EQ(1, counter2);

  vault.destroyInstances();
  vault.reenableInstances();

  EXPECT_EQ(4, counter1);
  EXPECT_EQ(2, counter2);
}

namespace {
class CancelOnDestructionSingleton {
 public:
  ~CancelOnDestructionSingleton() {
    CHECK(
        SingletonVault::singleton()
            ->getDestructionCancellationToken()
            .isCancellationRequested());
  }
};

auto cancelOnDestructionSingleton =
    folly::Singleton<CancelOnDestructionSingleton>{};
} // namespace

TEST(Singleton, CancelOnDestruction) {
  cancelOnDestructionSingleton.try_get();
}

TEST(Singleton, CreationBeforeRegistrationCompleteAborts) {
  struct Dummy {};
  struct VaultTag {};
  struct Tag {};

  folly::SingletonVault* vault = folly::SingletonVault::singleton<VaultTag>();
  std::ignore = vault;

  static folly::Singleton<Dummy, Tag, VaultTag> lateSingleton;

  EXPECT_DEATH(
      { std::ignore = lateSingleton.try_get(); },
      "singletonWarnCreateBeforeRegistrationCompleteAndAbort");
}
