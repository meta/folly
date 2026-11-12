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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <folly/Memory.h>
#include <folly/ScopeGuard.h>
#include <folly/Singleton.h>
#include <folly/container/Enumerate.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/Request.h>
#include <folly/io/async/test/RequestContextHelper.h>
#include <folly/lang/New.h>
#include <folly/portability/GTest.h>
#include <folly/synchronization/CallOnce.h>
#include <folly/synchronization/RelaxedAtomic.h>
#include <folly/system/ThreadName.h>

#include <fmt/format.h>
#include <folly/synchronization/test/Barrier.h>

using namespace folly;

namespace folly {
RequestContext* getCachedRequestContext();
}

static_assert(
    noexcept(std::declval<const RequestContextHandle&>().capture()) ==
    new_is_infallible_v);
static_assert(!noexcept(RequestContext::setContext(nullptr)));

RequestToken testtoken("test");

class RequestContextTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Make sure each test starts out using the default context, and not some
    // other context left over by a previous test.
    RequestContext::setContext(nullptr);

    // Make sure no data is set for the "test" key when we start.  There could
    // be left over data in the default context from a previous test.  If we
    // don't clear it out future calls to setContextData() won't actually work,
    // and will reset the data to null instead of properly setting the new
    // desired data.
    //
    // (All of the tests generally want the behavior of overwriteContextData()
    // rather than setContextData(), but that method is private.)
    //
    // We ideally want to clear out data for any keys that may be set, not just
    // the "test" key, but there also isn't a RequestContext API to do this.
    clearData();
  }

  void TearDown() override {
    folly::SingletonVault::singleton()->destroyInstances();
    folly::SingletonVault::singleton()->reenableInstances();
  }

  RequestContext& getContext() {
    auto* ctx = RequestContext::get();
    EXPECT_TRUE(ctx != nullptr);
    return *ctx;
  }

  void setData(int data = 0, std::string key = "test") {
    getContext().setContextData(key, std::make_unique<TestData>(data));
  }

  bool hasData(std::string key = "test") {
    return getContext().hasContextData(key);
  }

  const TestData& getData(std::string key = "test") {
    auto* ptr = dynamic_cast<TestData*>(getContext().getContextData(key));
    EXPECT_TRUE(ptr != nullptr);
    return *ptr;
  }

  void clearData(std::string key = "test") {
    getContext().clearContextData(key);
  }

  std::vector<intptr_t> getRootIdsFromAllThreads() {
    auto rootids = RequestContext::getRootIdsFromAllThreads();
    std::vector<intptr_t> result;
    std::transform(
        rootids.begin(), rootids.end(), std::back_inserter(result), [](auto e) {
          return e.id;
        });
    return result;
  }
};

TEST_F(RequestContextTest, SimpleTest) {
  EventBase base;

  // There should always be a default context with get()
  EXPECT_TRUE(RequestContext::get() != nullptr);
  // but fallback context should have rootid set to 0
  EXPECT_EQ(RequestContext::get()->getRootId(), 0);

  // but not with saveContext()
  EXPECT_EQ(RequestContext::saveContext(), nullptr);
  RequestContext::create();
  EXPECT_NE(RequestContext::saveContext(), nullptr);
  auto rootids = getRootIdsFromAllThreads();
  EXPECT_EQ(1, rootids.size());
  EXPECT_EQ(RequestContext::get()->getRootId(), rootids[0]);
  EXPECT_EQ(reinterpret_cast<intptr_t>(RequestContext::get()), rootids[0]);
  EXPECT_NE(RequestContext::get()->getRootId(), 0);
  RequestContext::create();
  EXPECT_NE(RequestContext::saveContext(), nullptr);
  EXPECT_NE(RequestContext::get()->getRootId(), rootids[0]);

  EXPECT_EQ(nullptr, RequestContext::get()->getContextData("test"));

  RequestContext::get()->setContextData("test", std::make_unique<TestData>(10));
  base.runInEventBaseThread([&]() {
    EXPECT_TRUE(RequestContext::get() != nullptr);
    auto data =
        dynamic_cast<TestData*>(
            RequestContext::get()->getContextData(testtoken))
            ->data_;
    EXPECT_EQ(10, data);
    rootids = getRootIdsFromAllThreads();
    EXPECT_EQ(2, rootids.size());
    EXPECT_EQ(RequestContext::get()->getRootId(), rootids[0]);
    EXPECT_EQ(RequestContext::get()->getRootId(), rootids[1]);
    base.terminateLoopSoon();
  });
  auto th = std::thread([&]() { base.loopForever(); });
  th.join();
  EXPECT_TRUE(RequestContext::get() != nullptr);
  auto a =
      dynamic_cast<TestData*>(RequestContext::get()->getContextData("test"));
  auto data = a->data_;
  EXPECT_EQ(10, data);

  RequestContext::setContext(std::shared_ptr<RequestContext>());
  // There should always be a default context
  EXPECT_TRUE(nullptr != RequestContext::get());
}

TEST_F(RequestContextTest, RequestContextScopeGuard) {
  RequestContextScopeGuard g0;
  setData(10);
  {
    RequestContextScopeGuard g1;
    EXPECT_FALSE(hasData());
    setData(20);
    EXPECT_EQ(20, getData().data_);
    EXPECT_EQ(1, getData().set_);
    EXPECT_EQ(0, getData().unset_);
  }
  EXPECT_EQ(10, getData().data_);
  EXPECT_EQ(2, getData().set_);
  EXPECT_EQ(1, getData().unset_);
}

TEST_F(RequestContextTest, defaultContext) {
  // Don't create a top level guard
  setData(10);
  {
    RequestContextScopeGuard g1;
    EXPECT_FALSE(hasData());
  }
  EXPECT_EQ(10, getData().data_);
  EXPECT_EQ(1, getData().set_);
  EXPECT_EQ(0, getData().unset_);
}

TEST_F(RequestContextTest, setIfAbsentTest) {
  RequestContextScopeGuard guard;
  EXPECT_TRUE(RequestContext::get() != nullptr);

  RequestContext::get()->setContextData("test", std::make_unique<TestData>(10));
  EXPECT_FALSE(
      RequestContext::get()->setContextDataIfAbsent(
          "test", std::make_unique<TestData>(20)));
  EXPECT_EQ(
      10,
      dynamic_cast<TestData*>(RequestContext::get()->getContextData(testtoken))
          ->data_);

  EXPECT_TRUE(
      RequestContext::get()->setContextDataIfAbsent(
          "test2", std::make_unique<TestData>(20)));
  EXPECT_EQ(
      20,
      dynamic_cast<TestData*>(RequestContext::get()->getContextData("test2"))
          ->data_);

  RequestContext::setContext(std::shared_ptr<RequestContext>());
  EXPECT_TRUE(nullptr != RequestContext::get());
}

namespace {
class NonClearableData : public TestData {
 public:
  using TestData::TestData;
  bool clearable() override { return false; }
};
} // namespace

TEST_F(RequestContextTest, nonClearableThrowsOnClear) {
  RequestContextScopeGuard g;
  auto& ctx = getContext();
  ctx.setContextData("test", std::make_unique<NonClearableData>(1));

  EXPECT_THROW(ctx.clearContextData("test"), std::invalid_argument);
  EXPECT_THROW(
      ctx.setContextData("test", std::make_unique<NonClearableData>(2)),
      std::invalid_argument);
  EXPECT_THROW(
      ctx.overwriteContextData("test", std::make_unique<NonClearableData>(2)),
      std::invalid_argument);
  EXPECT_FALSE(ctx.setContextDataIfAbsent(
      "test", std::make_unique<NonClearableData>(2)));
  EXPECT_EQ(1, getData().data_);
}

TEST_F(RequestContextTest, nonClearableShallowCopyOverwrite) {
  RequestContextScopeGuard g0;
  getContext().setContextData("test", std::make_unique<NonClearableData>(123));
  {
    ShallowCopyRequestContextScopeGuard g1(
        "test", std::make_unique<NonClearableData>(789));
    EXPECT_EQ(789, getData().data_);
  }
  {
    ShallowCopyRequestContextScopeGuard g1("test", nullptr);
    EXPECT_EQ(nullptr, getContext().getContextData("test"));
  }
  EXPECT_EQ(123, getData().data_);
  EXPECT_EQ(3, getData().set_);
  EXPECT_EQ(2, getData().unset_);
}

TEST_F(RequestContextTest, testSetUnset) {
  RequestContext::create();
  auto ctx1 = RequestContext::saveContext();
  ctx1->setContextData("test", std::make_unique<TestData>(10));
  auto testData1 = dynamic_cast<TestData*>(ctx1->getContextData("test"));

  // onSet called in setContextData
  EXPECT_EQ(1, testData1->set_);
  EXPECT_EQ(ctx1.get(), testData1->onSetRctx);

  // Override RequestContext
  RequestContext::create();
  auto ctx2 = RequestContext::saveContext();
  ctx2->setContextData(testtoken, std::make_unique<TestData>(20));
  auto testData2 = dynamic_cast<TestData*>(ctx2->getContextData(testtoken));

  // onSet called in setContextData
  EXPECT_EQ(1, testData2->set_);
  EXPECT_EQ(ctx2.get(), testData2->onSetRctx);

  // Check ctx1->onUnset was called
  EXPECT_EQ(1, testData1->unset_);
  EXPECT_EQ(ctx1.get(), testData1->onUnSetRctx);

  RequestContext::setContext(ctx1);
  EXPECT_EQ(2, testData1->set_);
  EXPECT_EQ(1, testData1->unset_);
  EXPECT_EQ(1, testData2->unset_);
  EXPECT_EQ(ctx1.get(), testData1->onSetRctx);
  EXPECT_EQ(ctx1.get(), testData1->onUnSetRctx);
  EXPECT_EQ(ctx2.get(), testData2->onUnSetRctx);

  RequestContext::setContext(ctx2);
  EXPECT_EQ(2, testData1->set_);
  EXPECT_EQ(2, testData1->unset_);
  EXPECT_EQ(2, testData2->set_);
  EXPECT_EQ(1, testData2->unset_);
}

TEST_F(RequestContextTest, deadlockTest) {
  class DeadlockTestData : public RequestData {
   public:
    explicit DeadlockTestData(const std::string& val) : val_(val) {}

    ~DeadlockTestData() override {
      RequestContext::get()->setContextData(
          val_, std::make_unique<TestData>(1));
    }

    bool hasCallback() override { return false; }

    std::string val_;
  };

  RequestContext::get()->setContextData(
      "test", std::make_unique<DeadlockTestData>("test1"));
  RequestContext::get()->clearContextData(testtoken);
}

// A common use case is to use set/unset to maintain a thread global
// Regression test to ensure that unset is always called before set
TEST_F(RequestContextTest, sharedGlobalTest) {
  static bool global = false;

  class GlobalTestData : public RequestData {
   public:
    void onSet() override {
      ASSERT_FALSE(global);
      global = true;
    }

    void onUnset() override {
      ASSERT_TRUE(global);
      global = false;
    }

    bool hasCallback() override { return true; }
  };

  intptr_t root = 0;
  {
    RequestContextScopeGuard g0;
    RequestContext::get()->setContextData(
        "test", std::make_unique<GlobalTestData>());
    auto root0 = RequestContext::saveContext().get()->getRootId();
    EXPECT_EQ(getRootIdsFromAllThreads()[0], root0);
    {
      RequestContextScopeGuard g1;
      RequestContext::get()->setContextData(
          "test", std::make_unique<GlobalTestData>());
      auto root1 = RequestContext::saveContext().get()->getRootId();
      EXPECT_EQ(getRootIdsFromAllThreads()[0], root1);
    }
    EXPECT_EQ(getRootIdsFromAllThreads()[0], root0);
  }
  EXPECT_EQ(getRootIdsFromAllThreads()[0], root);
}

TEST_F(RequestContextTest, ShallowCopyBasic) {
  ShallowCopyRequestContextScopeGuard g0;
  setData(123, "immutable");
  EXPECT_EQ(123, getData("immutable").data_);
  EXPECT_FALSE(hasData());
  EXPECT_EQ(0, getRootIdsFromAllThreads()[0]);

  {
    ShallowCopyRequestContextScopeGuard g1;
    EXPECT_EQ(123, getData("immutable").data_);
    setData(789);
    EXPECT_EQ(789, getData().data_);
    EXPECT_EQ(0, getRootIdsFromAllThreads()[0]);
  }

  EXPECT_FALSE(hasData());
  EXPECT_EQ(123, getData("immutable").data_);
  EXPECT_EQ(1, getData("immutable").set_);
  EXPECT_EQ(0, getData("immutable").unset_);
  EXPECT_EQ(0, getRootIdsFromAllThreads()[0]);
}

TEST_F(RequestContextTest, ShallowCopyOverwrite) {
  RequestContextScopeGuard g0;
  setData(123);
  EXPECT_EQ(123, getData().data_);
  auto rootid = RequestContext::get()->getRootId();
  EXPECT_EQ(rootid, getRootIdsFromAllThreads()[0]);
  {
    ShallowCopyRequestContextScopeGuard g1(
        "test", std::make_unique<TestData>(789));
    EXPECT_EQ(789, getData().data_);
    EXPECT_EQ(1, getData().set_);
    EXPECT_EQ(0, getData().unset_);
    // should have inherited parent's rootid
    EXPECT_EQ(rootid, getRootIdsFromAllThreads()[0]);

    {
      // rootId is preserved for shallow copies of shallow copies
      ShallowCopyRequestContextScopeGuard g2;
      EXPECT_EQ(rootid, getRootIdsFromAllThreads()[0]);
    }
    EXPECT_EQ(rootid, getRootIdsFromAllThreads()[0]);
  }
  EXPECT_EQ(123, getData().data_);
  EXPECT_EQ(2, getData().set_);
  EXPECT_EQ(1, getData().unset_);
  EXPECT_EQ(rootid, getRootIdsFromAllThreads()[0]);
}

TEST_F(RequestContextTest, ShallowCopyDefaultContext) {
  // Don't set global scope guard
  setData(123);
  EXPECT_EQ(123, getData().data_);
  {
    ShallowCopyRequestContextScopeGuard g1(
        "test", std::make_unique<TestData>(789));
    EXPECT_EQ(789, getData().data_);
  }
  EXPECT_EQ(123, getData().data_);
  EXPECT_EQ(1, getData().set_);
  EXPECT_EQ(0, getData().unset_);
}

TEST_F(RequestContextTest, ShallowCopyClear) {
  RequestContextScopeGuard g0;
  setData(123);
  EXPECT_EQ(123, getData().data_);
  {
    ShallowCopyRequestContextScopeGuard g1;
    EXPECT_EQ(123, getData().data_);
    clearData();
    setData(789);
    EXPECT_EQ(789, getData().data_);
  }
  EXPECT_EQ(123, getData().data_);
  EXPECT_EQ(2, getData().set_);
  EXPECT_EQ(1, getData().unset_);
}

TEST_F(RequestContextTest, ShallowCopyWithConcurrentModifications) {
  RequestContextScopeGuard g0;
  auto ctx = getContext().saveContext();

  relaxed_atomic<bool> done{false};
  std::thread mutator([&] {
    constexpr size_t kNumIters = 2;
    constexpr size_t kNumKeys = 1000;
    std::vector<RequestToken> keys(kNumKeys);
    for (auto&& [i, key] : folly::enumerate(keys)) {
      key = RequestToken{fmt::format("key-{}", i)};
    }

    for (size_t i = 0; i < kNumIters; ++i) {
      for (const auto& key : keys) {
        ctx->setContextData(key, std::make_unique<TestData>(0));
      }
      for (const auto& key : keys) {
        ctx->clearContextData(key);
      }
    }
    done = true;
  });

  std::thread copier([&] {
    RequestContextScopeGuard g1(ctx);
    while (!done) {
      ShallowCopyRequestContextScopeGuard shallowGuard;
      // Force unsetting and re-setting all the RequestDatas.
      RequestContextScopeGuard nullptrGuard(nullptr);
    }
  });

  mutator.join();
  copier.join();
}

TEST_F(RequestContextTest, ShallowCopyMulti) {
  RequestContextScopeGuard g0;
  setData(1, "test1");
  setData(2, "test2");
  EXPECT_EQ(1, getData("test1").data_);
  EXPECT_EQ(2, getData("test2").data_);
  {
    ShallowCopyRequestContextScopeGuard g1(
        RequestDataItem{"test1", std::make_unique<TestData>(2)},
        RequestDataItem{"test2", std::make_unique<TestData>(4)});

    EXPECT_EQ(2, getData("test1").data_);
    EXPECT_EQ(4, getData("test2").data_);
    clearData("test1");
    clearData("test2");
    setData(4, "test1");
    setData(8, "test2");
    EXPECT_EQ(4, getData("test1").data_);
    EXPECT_EQ(8, getData("test2").data_);
  }
  EXPECT_EQ(1, getData("test1").data_);
  EXPECT_EQ(2, getData("test2").data_);
}

TEST_F(RequestContextTest, ShallowCopyMultiOverwriteExpandsCallbackSlots) {
  RequestContextScopeGuard g0;
  setData(1, "test1");
  setData(2, "test2");
  setData(3, "test3");

  {
    ShallowCopyRequestContextScopeGuard g1(
        RequestDataItem{"test1", std::make_unique<TestData>(4)},
        RequestDataItem{"test2", std::make_unique<TestData>(5)});

    EXPECT_EQ(4, getData("test1").data_);
    EXPECT_EQ(5, getData("test2").data_);
    EXPECT_EQ(3, getData("test3").data_);
  }

  EXPECT_EQ(1, getData("test1").data_);
  EXPECT_EQ(2, getData("test2").data_);
  EXPECT_EQ(3, getData("test3").data_);
}

TEST_F(RequestContextTest, RootIdOnCopy) {
  auto ctxBase = std::make_shared<RequestContext>(0xab);
  EXPECT_EQ(0xab, ctxBase->getRootId());
  {
    auto ctx = RequestContext::copyAsRoot(*ctxBase, 0xabc);
    EXPECT_EQ(0xabc, ctx->getRootId());
  }
  {
    auto ctx = RequestContext::copyAsChild(*ctxBase);
    EXPECT_EQ(0xab, ctx->getRootId());
  }
}

TEST_F(RequestContextTest, ThreadId) {
  folly::setThreadName("DummyThread");
  RequestContextScopeGuard g;
  auto ctxBase = std::make_shared<RequestContext>();
  auto rootids = RequestContext::getRootIdsFromAllThreads();
  EXPECT_EQ(*folly::getThreadName(rootids[0].tid), "DummyThread");
  EXPECT_EQ(rootids[0].tidOS, folly::getOSThreadID());

  EventBase base;
  base.runInEventBaseThread([&]() {
    RequestContextScopeGuard g_;
    folly::setThreadName("DummyThread2");
    rootids = RequestContext::getRootIdsFromAllThreads();
    base.terminateLoopSoon();
  });

  auto th = std::thread([&]() { base.loopForever(); });
  th.join();

  std::sort(rootids.begin(), rootids.end(), [](const auto& a, const auto& b) {
    auto aname = folly::getThreadName(a.tid);
    auto bname = folly::getThreadName(b.tid);
    static const std::string kUnnamed = "zzz";
    return (aname ? *aname : kUnnamed) < (bname ? *bname : kUnnamed);
  });

  EXPECT_EQ(*folly::getThreadName(rootids[0].tid), "DummyThread");
  EXPECT_FALSE(folly::getThreadName(rootids[1].tid));
}

TEST_F(RequestContextTest, Clear) {
  struct Foo : public RequestData {
    bool& cleared;
    bool& deleted;
    Foo(bool& c, bool& d) : cleared(c), deleted(d) {}
    ~Foo() override {
      EXPECT_TRUE(cleared);
      deleted = true;
    }
    bool hasCallback() override { return false; }
    void onClear() override {
      EXPECT_FALSE(cleared);
      cleared = true;
    }
  };

  std::string key = "clear";
  {
    bool cleared = false;
    bool deleted = false;
    {
      RequestContextScopeGuard g;
      RequestContext::get()->setContextData(
          key, std::make_unique<Foo>(cleared, deleted));
      EXPECT_FALSE(cleared);
      RequestContext::get()->clearContextData(key);
      EXPECT_TRUE(cleared);
    }
    EXPECT_TRUE(deleted);
  }
  {
    bool cleared = false;
    bool deleted = false;
    {
      RequestContextScopeGuard g;
      RequestContext::get()->setContextData(
          key, std::make_unique<Foo>(cleared, deleted));
      EXPECT_FALSE(cleared);
      EXPECT_FALSE(deleted);
    }
    EXPECT_TRUE(cleared);
    EXPECT_TRUE(deleted);
  }
}

TEST_F(RequestContextTest, OverwriteNullData) {
  folly::ShallowCopyRequestContextScopeGuard g0("token", nullptr);
  {
    folly::ShallowCopyRequestContextScopeGuard g1(
        "token", std::make_unique<TestData>(0));
    EXPECT_NE(folly::RequestContext::get()->getContextData("token"), nullptr);
  }
}

TEST_F(RequestContextTest, ConcurrentDataRefRelease) {
  for (int i = 0; i < 100; ++i) {
    std::atomic<int> step{0};
    std::shared_ptr<folly::RequestContext> sp1;
    auto th1 = std::thread([&]() {
      folly::RequestContextScopeGuard g0; // Creates ctx0.
      setData(); // Creates data0 with one reference in ctx0.
      {
        folly::ShallowCopyRequestContextScopeGuard g1;
        // g1 created ctx1 with second reference to data0.
        EXPECT_NE(&getData(), nullptr);
        // Keep shared_ptr to ctx1 to pass to th2
        sp1 = folly::RequestContext::saveContext();
        step.store(1); // sp1 is ready.
        while (step.load() < 2) {
          /* Wait for th2 to clear reference to data0. */;
        }
      }
      // End of g2 released shared_ptr to ctx1, switched back to ctx0
      // At this point:
      // - One shared_ptr to ctx0, held by th1.
      // - One shared_ptr to ctx1, help by th2.
      // - data0 has one clear count (for reference from ctx0) and
      //   two delete counts (one each from ctx0 and ctx1).
      step.store(3);
      // End of g1 will destroy ctx0, release clear/delete counts for data0.
    });
    auto th2 = std::thread([&]() {
      while (step.load() < 1) {
        /* Wait for th1 to set sp1. */;
      }
      folly::RequestContextScopeGuard g2(std::move(sp1));
      // g2 set context to ctx1.
      EXPECT_EQ(sp1.get(), nullptr);
      EXPECT_NE(&getData(), nullptr);
      clearData();
      step.store(2); // th2 cleared reference to data0 in ctx1.
      while (step.load() < 3) {
        /* Wait for th1 to release shared_ptr to ctx1. */;
      }
      // End of g2 will destroy ctx1, release delete count for data0.
    });
    th1.join();
    th2.join();
  }
}

TEST_F(RequestContextTest, ClearOutlivesConcurrentDeleteRefRelease) {
  std::atomic<int> step{0};
  bool deleted = false;

  struct Foo : public RequestData {
    Foo(std::atomic<int>& s, bool& d) : step_(s), deleted_(d) {}
    ~Foo() override { deleted_ = true; }
    bool hasCallback() override { return false; }
    void onClear() override {
      // One delete count held by the thread running onClear(), one held by
      // ctx1's retired state; no clear counts left.
      EXPECT_EQ(refCount(), 2);
      step_.store(1); // onClear() is running.
      while (step_.load() < 2) {
        /* Wait for the main thread to release the delete count. */;
      }
      // The releasing thread still holds a delete count, so the object must
      // have survived.
      EXPECT_FALSE(deleted_);
    }
    std::atomic<int>& step_;
    bool& deleted_;
  };

  const std::string key = "clear-outlives-delete-ref";
  std::shared_ptr<folly::RequestContext> sp1;

  auto th = std::thread([&]() {
    folly::RequestContextScopeGuard g0; // Creates ctx0 holding data.
    folly::RequestContext::get()->setContextData(
        key, std::make_unique<Foo>(step, deleted));
    {
      folly::ShallowCopyRequestContextScopeGuard g1;
      // ctx1 has a second reference to data.
      sp1 = folly::RequestContext::saveContext();
      // Releases ctx1's clear count and leaves a delete count in ctx1's
      // retired state.
      folly::RequestContext::get()->clearContextData(key);
    }
    // End of g0 releases the last clear count, running onClear().
  });

  while (step.load() < 1) {
    /* Wait for onClear() to start. */;
  }
  sp1.reset(); // Destroying ctx1 releases the remaining delete count.
  step.store(2);
  th.join();
  EXPECT_TRUE(deleted);
}

TEST_F(RequestContextTest, AccessAllThreadsDestructionGuard) {
  constexpr auto kNumThreads = 128;

  std::vector<std::thread> threads{kNumThreads};
  folly::test::Barrier barrier{kNumThreads + 1};

  std::atomic<std::size_t> count{0};
  for (auto& thread : threads) {
    thread = std::thread([&] {
      // Force creation of thread local
      RequestContext::setContext(nullptr);
      ++count;
      // Wait for all other threads to do the same
      barrier.wait();
      // Wait until signaled to die
      barrier.wait();
    });
  }

  barrier.wait();
  // Sanity check
  EXPECT_EQ(count.load(), kNumThreads);

  {
    auto accessor = RequestContext::accessAllThreads();
    // Allow threads to die (but they should not as long as we hold accessor!)
    barrier.wait();
    auto accessorsCount = std::distance(accessor.begin(), accessor.end());
    EXPECT_EQ(accessorsCount, kNumThreads + 1);
    for (RequestContext::StaticContext& staticContext : accessor) {
      EXPECT_EQ(staticContext.requestContext, nullptr);
    }
  }

  for (auto& thread : threads) {
    thread.join();
  }
}

namespace {

struct KeyATraits {
  static inline const RequestToken kToken{"keyA"};
};

struct KeyBTraits {
  static inline const RequestToken kToken{"keyB"};
};

} // namespace

TEST_F(RequestContextTest, GetThreadCachedContextData) {
  auto makeData = [](int value) {
    return std::make_unique<ImmutableRequestData<int>>(value);
  };

  auto getData = [](auto traits) {
    auto* data =
        RequestContext::try_get()
            ->getThreadCachedContextData<decltype(traits)>();
    CHECK(data != nullptr);
    auto* idata = dynamic_cast<ImmutableRequestData<int>*>(data);
    CHECK(idata != nullptr);
    return idata;
  };

  RequestContextScopeGuard guard;

  RequestContext::try_get()->setContextData(KeyATraits::kToken, makeData(1));
  RequestContext::try_get()->setContextData(KeyBTraits::kToken, makeData(2));

  EXPECT_EQ(getData(KeyATraits{})->value(), 1);
  EXPECT_EQ(getData(KeyBTraits{})->value(), 2);

  RequestContext::try_get()->overwriteContextData(
      KeyATraits::kToken, makeData(3));
  EXPECT_EQ(getData(KeyATraits{})->value(), 3);
  EXPECT_EQ(getData(KeyBTraits{})->value(), 2);

  RequestContext::try_get()->overwriteContextData(
      KeyATraits::kToken, makeData(4), /*safe=*/true);
  EXPECT_EQ(getData(KeyATraits{})->value(), 4);
  EXPECT_EQ(getData(KeyBTraits{})->value(), 2);

  RequestContext::try_get()->clearContextData(KeyATraits::kToken);
  EXPECT_TRUE(
      RequestContext::try_get()->getThreadCachedContextData<KeyATraits>() ==
      nullptr);
  EXPECT_EQ(getData(KeyBTraits{})->value(), 2);

  // Invalidations are delivered from other threads too.
  std::thread([&, ctx = RequestContext::saveContext()] {
    RequestContextScopeGuard guard2(ctx);
    RequestContext::try_get()->setContextData(KeyATraits::kToken, makeData(5));
  }).join();
  EXPECT_EQ(getData(KeyATraits{})->value(), 5);
  EXPECT_EQ(getData(KeyBTraits{})->value(), 2);

  // Caches are not leaked when switching request context.
  {
    RequestContextScopeGuard guard3;
    EXPECT_TRUE(
        RequestContext::try_get()->getThreadCachedContextData<KeyATraits>() ==
        nullptr);
    EXPECT_TRUE(
        RequestContext::try_get()->getThreadCachedContextData<KeyBTraits>() ==
        nullptr);
  }
}

TEST_F(RequestContextTest, ContextDataVersion) {
  RequestContextScopeGuard guard;
  auto* context = RequestContext::try_get();
  const auto initialVersion = context->getContextDataVersion();
  EXPECT_NE(initialVersion, 0);

  context->setContextData(
      "version-test", std::make_unique<ImmutableRequestData<int>>(1));
  const auto setVersion = context->getContextDataVersion();
  EXPECT_NE(initialVersion, setVersion);

  context->overwriteContextData(
      "version-test", std::make_unique<ImmutableRequestData<int>>(2));
  const auto overwriteVersion = context->getContextDataVersion();
  EXPECT_NE(setVersion, overwriteVersion);

  context->overwriteContextData(
      "version-test",
      std::make_unique<ImmutableRequestData<int>>(3),
      /*safe=*/true);
  const auto safeOverwriteVersion = context->getContextDataVersion();
  EXPECT_NE(overwriteVersion, safeOverwriteVersion);

  context->clearContextData("version-test");
  const auto clearVersion = context->getContextDataVersion();
  EXPECT_NE(safeOverwriteVersion, clearVersion);

  {
    ShallowCopyRequestContextScopeGuard nestedGuard;
    EXPECT_NE(clearVersion, RequestContext::try_get()->getContextDataVersion());
  }
  EXPECT_EQ(clearVersion, context->getContextDataVersion());
}

TEST(RequestContextTryGetTest, TryGetTest) {
  RequestContextScopeGuard guard(std::shared_ptr<RequestContext>{});
  // try_get() should not create a default RequestContext object if none exists.
  EXPECT_EQ(RequestContext::try_get(), nullptr);
  // Explicitly create a new instance so that subsequent calls to try_get()
  // return it.
  RequestContext::create();
  EXPECT_NE(RequestContext::saveContext(), nullptr);
  EXPECT_NE(RequestContext::try_get(), nullptr);
  // Make sure that the pointers returned by both get() and try_get() point to
  // the same underlying instance.
  EXPECT_EQ(RequestContext::try_get(), RequestContext::get());
  // Set some context data and read it out via try_get() accessor.
  RequestContext::get()->setContextData("test", std::make_unique<TestData>(10));
  auto rc = RequestContext::try_get();
  EXPECT_TRUE(rc->hasContextData("test"));
  auto* dataPtr = dynamic_cast<TestData*>(rc->getContextData("test"));
  EXPECT_EQ(dataPtr->data_, 10);

  auto thread = std::thread([&] {
    auto accessor = RequestContext::accessAllThreads();
    // test there is no deadlock with try_get()
    RequestContext::try_get();
  });
  thread.join();

  thread = std::thread([&] {
    RequestContext::get();
    auto accessor = RequestContext::accessAllThreads();
    // test there is no deadlock with get()
    RequestContext::get();
  });
  thread.join();
}

TEST(ImmutableRequestTest, simple) {
  ImmutableRequestData<int> ird(4);
  EXPECT_EQ(ird.value(), 4);
}

TEST(ImmutableRequestTest, typeTraits) {
  using IRDI = ImmutableRequestData<int>;

  auto c1 = std::is_constructible<IRDI, int>::value;
  EXPECT_TRUE(c1);
  auto n1 = std::is_nothrow_constructible<IRDI, int>::value;
  EXPECT_TRUE(n1);

  auto c2 = std::is_constructible<IRDI, int, int>::value;
  EXPECT_FALSE(c2);
}

namespace {

class LocalLifetimeData : public RequestData {
 public:
  explicit LocalLifetimeData(std::atomic<int>& destroyed)
      : destroyed_(destroyed) {}
  ~LocalLifetimeData() override { ++destroyed_; }
  bool hasCallback() override { return false; }

 private:
  std::atomic<int>& destroyed_;
};

RequestContextHandle makeLocalHandle() {
  LocalRequestContextScopeGuard guard;
  return RequestContext::saveContextLocal();
}

RequestContextHandle makeLocalHandle(intptr_t rootId) {
  LocalRequestContextScopeGuard guard(rootId);
  return RequestContext::saveContextLocal();
}

static_assert(sizeof(RequestContextHandle) == sizeof(void*));
static_assert(!std::is_copy_constructible_v<RequestContextHandle>);
static_assert(!std::is_copy_assignable_v<RequestContextHandle>);
static_assert(std::is_nothrow_move_constructible_v<RequestContextHandle>);
static_assert(std::is_nothrow_move_assignable_v<RequestContextHandle>);

} // namespace

TEST_F(RequestContextTest, AmbientOwnerSurvivesLastPinRelease) {
  std::atomic<int> destroyed{0};
  std::weak_ptr<RequestContext> weak;
  {
    LocalRequestContextScopeGuard guard;
    auto* raw = RequestContext::try_get();
    raw->setContextData(
        testtoken, std::make_unique<LocalLifetimeData>(destroyed));
    auto handle = RequestContext::saveContextLocal();
    EXPECT_EQ(handle.get(), raw);
    handle.reset();
    EXPECT_EQ(destroyed, 0);
    EXPECT_EQ(RequestContext::try_get(), raw);
    EXPECT_TRUE(raw->hasContextData(testtoken));
    {
      auto captured = RequestContext::saveContext();
      EXPECT_EQ(captured.get(), raw);
      weak = captured;
    }
    EXPECT_FALSE(weak.expired());
    handle = RequestContext::saveContextLocal();
    handle.reset();
    EXPECT_EQ(destroyed, 0);
  }
  EXPECT_EQ(destroyed, 1);
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(RequestContext::try_get(), nullptr);
}

TEST_F(RequestContextTest, LocalHandleNestedActivation) {
  auto first = makeLocalHandle();
  auto second = makeLocalHandle(123);
  first->setContextData(testtoken, std::make_unique<TestData>(10));
  second->setContextData(testtoken, std::make_unique<TestData>(20));
  EXPECT_EQ(first->getRootId(), reinterpret_cast<intptr_t>(first.get()));
  EXPECT_EQ(second->getRootId(), 123);
  {
    BorrowedRequestContextScopeGuard outer(first);
    EXPECT_EQ(RequestContext::try_get(), first.get());
    EXPECT_EQ(10, getData().data_);
    EXPECT_EQ(2, getData().set_);
    {
      BorrowedRequestContextScopeGuard inner(second);
      EXPECT_EQ(RequestContext::try_get(), second.get());
      EXPECT_EQ(20, getData().data_);
      EXPECT_EQ(2, getData().set_);
      EXPECT_EQ(0, getData().unset_);
      const auto ids = getRootIdsFromAllThreads();
      EXPECT_NE(std::find(ids.begin(), ids.end(), 123), ids.end());
    }
    EXPECT_EQ(RequestContext::try_get(), first.get());
    EXPECT_EQ(3, getData().set_);
    EXPECT_EQ(1, getData().unset_);
  }
  EXPECT_EQ(RequestContext::try_get(), nullptr);
  auto* data = static_cast<TestData*>(first->getContextData(testtoken));
  EXPECT_EQ(2, data->unset_);
}

TEST_F(RequestContextTest, LocalHandleSameContextDoesNotNotify) {
  auto owner = makeLocalHandle();
  owner->setContextData(testtoken, std::make_unique<TestData>(10));
  BorrowedRequestContextScopeGuard outer(owner);
  {
    BorrowedRequestContextScopeGuard inner(owner);
    EXPECT_EQ(2, getData().set_);
    EXPECT_EQ(0, getData().unset_);
  }
  EXPECT_EQ(2, getData().set_);
  EXPECT_EQ(0, getData().unset_);
}

TEST_F(RequestContextTest, BorrowedSameContextRestoresSharedBinding) {
  auto handle = makeLocalHandle();
  handle->setContextData(testtoken, std::make_unique<TestData>(10));
  RequestContextScopeGuard shared(handle.capture());
  auto* data = static_cast<TestData*>(handle->getContextData(testtoken));
  const auto sets = data->set_.load();
  const auto unsets = data->unset_.load();
  EXPECT_THROW(RequestContext::saveContextLocal(), std::logic_error);
  {
    BorrowedRequestContextScopeGuard borrowed(handle);
    EXPECT_EQ(data->set_, sets);
    EXPECT_EQ(data->unset_, unsets);
    EXPECT_THROW(RequestContext::saveContextLocal(), std::logic_error);
    RequestContext::setContext(nullptr);
    EXPECT_EQ(RequestContext::try_get(), nullptr);
  }
  EXPECT_EQ(RequestContext::try_get(), handle.get());
  EXPECT_THROW(RequestContext::saveContextLocal(), std::logic_error);
  EXPECT_EQ(data->set_, sets + 1);
  EXPECT_EQ(data->unset_, unsets + 1);
}

TEST_F(RequestContextTest, LocalHandleMoveAndResetDuringActivation) {
  std::atomic<int> destroyed{0};
  std::atomic<int> replacedDestroyed{0};
  auto owner = makeLocalHandle();
  owner->setContextData(
      testtoken, std::make_unique<LocalLifetimeData>(destroyed));
  auto* raw = owner.get();
  {
    BorrowedRequestContextScopeGuard guard(owner);
    RequestContextHandle moved(std::move(owner));
    EXPECT_EQ(moved.get(), raw);
    auto assigned = makeLocalHandle();
    assigned->setContextData(
        testtoken, std::make_unique<LocalLifetimeData>(replacedDestroyed));
    assigned = std::move(moved);
    EXPECT_EQ(replacedDestroyed, 1);
    assigned.reset();
    EXPECT_FALSE(assigned);
    EXPECT_EQ(destroyed, 0);
    EXPECT_EQ(RequestContext::try_get(), raw);
    EXPECT_TRUE(raw->hasContextData(testtoken));
  }
  EXPECT_EQ(destroyed, 1);
  EXPECT_EQ(RequestContext::try_get(), nullptr);
}

TEST_F(RequestContextTest, AccessAllThreadsReadsSharedAndLocalBindings) {
  for (bool local : {false, true}) {
    std::atomic<int> destroyed{0};
    RequestContext* expected = nullptr;
    RequestContext::StaticContext* cached = nullptr;
    folly::test::Barrier ready(2);
    folly::test::Barrier inspected(2);
    std::thread thread([&] {
      cached = &RequestContext::getThreadContextHolder();
      auto inspect = [&] {
        expected = RequestContext::try_get();
        expected->setContextData(
            testtoken, std::make_unique<LocalLifetimeData>(destroyed));
        ready.wait();
        inspected.wait();
      };
      if (local) {
        LocalRequestContextScopeGuard scope(123);
        inspect();
      } else {
        RequestContextScopeGuard scope(std::make_shared<RequestContext>(123));
        inspect();
      }
    });
    ready.wait();
    bool found = false;
    {
      auto accessor = RequestContext::accessAllThreads();
      for (auto it = accessor.begin(); it != accessor.end(); ++it) {
        if (it.getRootIdInfo().tid == thread.get_id()) {
          found = true;
          EXPECT_EQ(&*it, cached);
          EXPECT_EQ(cached->rootId.load(std::memory_order_relaxed), 123);
          if (local) {
            EXPECT_EQ(cached->requestContext.get(), nullptr);
          } else {
            EXPECT_EQ(cached->requestContext.get(), expected);
          }
          auto* context = it.getContext();
          EXPECT_EQ(context, expected);
          if (context) {
            EXPECT_EQ(context->getRootId(), 123);
            EXPECT_TRUE(context->hasContextData(testtoken));
          }
          EXPECT_EQ(destroyed, 0);
        }
      }
    }
    EXPECT_TRUE(found);
    inspected.wait();
    thread.join();
    EXPECT_EQ(destroyed, 1);
  }
}

TEST_F(RequestContextTest, LocalHandleCaptureOutlivesOwnerAndActivation) {
  std::atomic<int> destroyed{0};
  std::shared_ptr<RequestContext> captured;
  std::weak_ptr<RequestContext> weak;
  {
    auto owner = makeLocalHandle();
    owner->setContextData(
        testtoken, std::make_unique<LocalLifetimeData>(destroyed));
    BorrowedRequestContextScopeGuard guard(owner);
    captured = RequestContext::saveContext();
    auto again = owner.capture();
    EXPECT_EQ(captured.get(), owner.get());
    EXPECT_FALSE(captured.owner_before(again));
    EXPECT_FALSE(again.owner_before(captured));
    weak = captured;
    owner.reset();
  }
  EXPECT_EQ(destroyed, 0);
  EXPECT_EQ(RequestContext::try_get(), nullptr);
  {
    RequestContextScopeGuard guard(captured);
    EXPECT_EQ(RequestContext::try_get(), captured.get());
    EXPECT_TRUE(RequestContext::get()->hasContextData(testtoken));
  }
  captured.reset();
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(destroyed, 1);
}

TEST_F(RequestContextTest, LocalHandleReusesPromotionAfterCpuCaptureRelease) {
  auto owner = makeLocalHandle();
  auto* raw = owner.get();
  std::weak_ptr<RequestContext> weak;
  {
    auto captured = owner.capture();
    weak = captured;
  }
  EXPECT_FALSE(weak.expired());
  {
    BorrowedRequestContextScopeGuard guard(owner);
    auto captured = RequestContext::saveContext();
    EXPECT_EQ(captured.get(), raw);
    auto original = weak.lock();
    EXPECT_FALSE(captured.owner_before(original));
    EXPECT_FALSE(original.owner_before(captured));
    owner.reset();
    EXPECT_EQ(RequestContext::try_get(), raw);
  }
  EXPECT_TRUE(weak.expired());
}

TEST_F(RequestContextTest, LocalHandleSetContextReturnsOwnedPrevious) {
  std::atomic<int> destroyed{0};
  std::shared_ptr<RequestContext> previous;
  {
    auto owner = makeLocalHandle();
    owner->setContextData(
        testtoken, std::make_unique<LocalLifetimeData>(destroyed));
    BorrowedRequestContextScopeGuard guard(owner);
    previous = RequestContext::setContext(nullptr);
    EXPECT_EQ(previous.get(), owner.get());
    EXPECT_EQ(RequestContext::try_get(), nullptr);
    owner.reset();
  }
  EXPECT_EQ(destroyed, 0);
  EXPECT_TRUE(previous->hasContextData(testtoken));
  previous.reset();
  EXPECT_EQ(destroyed, 1);
}

static_assert(
    sizeof(RequestContext::StaticContext) ==
    sizeof(std::shared_ptr<RequestContext>) + sizeof(std::atomic<intptr_t>));
static_assert(
    offsetof(RequestContext::StaticContext, rootId) ==
    sizeof(std::shared_ptr<RequestContext>));
static_assert(
    sizeof(RequestContextSaverScopeGuard) ==
    sizeof(std::shared_ptr<RequestContext>));
static_assert(
    alignof(RequestContextSaverScopeGuard) ==
    alignof(std::shared_ptr<RequestContext>));
static_assert(
    sizeof(RequestContextScopeGuard) ==
    sizeof(std::shared_ptr<RequestContext>));
static_assert(
    alignof(RequestContextScopeGuard) ==
    alignof(std::shared_ptr<RequestContext>));
static_assert(
    sizeof(ShallowCopyRequestContextScopeGuard) ==
    sizeof(std::shared_ptr<RequestContext>));
static_assert(
    alignof(ShallowCopyRequestContextScopeGuard) ==
    alignof(std::shared_ptr<RequestContext>));

TEST_F(RequestContextTest, LocalHandleRestoredAfterSharedAndShallowScopes) {
  auto owner = makeLocalHandle();
  owner->setContextData(testtoken, std::make_unique<TestData>(10));
  BorrowedRequestContextScopeGuard outer(owner);
  {
    RequestContextScopeGuard shared;
    EXPECT_NE(RequestContext::try_get(), owner.get());
    EXPECT_THROW(RequestContext::saveContextLocal(), std::logic_error);
  }
  EXPECT_EQ(RequestContext::try_get(), owner.get());
  EXPECT_EQ(RequestContext::saveContext().get(), owner.get());
  EXPECT_THROW(RequestContext::saveContextLocal(), std::logic_error);
  {
    ShallowCopyRequestContextScopeGuard child(
        testtoken, std::make_unique<TestData>(20));
    EXPECT_NE(RequestContext::try_get(), owner.get());
    EXPECT_EQ(20, getData().data_);
    EXPECT_EQ(RequestContext::get()->getRootId(), owner->getRootId());
  }
  EXPECT_EQ(RequestContext::try_get(), owner.get());
  EXPECT_EQ(RequestContext::saveContext().get(), owner.get());
  EXPECT_THROW(RequestContext::saveContextLocal(), std::logic_error);
  EXPECT_EQ(10, getData().data_);
}

TEST_F(RequestContextTest, LocalAmbientRestoredAfterSaverScope) {
  std::atomic<int> destroyed{0};
  {
    LocalRequestContextScopeGuard ambient;
    auto* raw = RequestContext::try_get();
    raw->setContextData(
        testtoken, std::make_unique<LocalLifetimeData>(destroyed));
    {
      RequestContextSaverScopeGuard saver;
      saver.setContext(std::make_shared<RequestContext>());
      EXPECT_NE(RequestContext::try_get(), raw);
      saver.restoreContext();
      EXPECT_EQ(RequestContext::saveContext().get(), raw);
      EXPECT_TRUE(RequestContext::try_get()->hasContextData(testtoken));
      saver.setContext(std::make_shared<RequestContext>());
      EXPECT_NE(RequestContext::try_get(), raw);
      EXPECT_EQ(destroyed, 0);
    }
    EXPECT_EQ(RequestContext::saveContext().get(), raw);
    EXPECT_TRUE(RequestContext::try_get()->hasContextData(testtoken));
    EXPECT_EQ(destroyed, 0);
  }
  EXPECT_EQ(destroyed, 1);
  EXPECT_EQ(RequestContext::try_get(), nullptr);
}

TEST_F(RequestContextTest, LocalOwnerOutlivesBindingAndCpuCapture) {
  std::atomic<int> destroyed{0};
  std::weak_ptr<RequestContext> weak;
  {
    LocalRequestContextScopeGuard owner;
    RequestContext::get()->setContextData(
        testtoken, std::make_unique<LocalLifetimeData>(destroyed));
    auto captured = RequestContext::setContext(nullptr);
    weak = captured;
    EXPECT_EQ(RequestContext::try_get(), nullptr);
    std::thread cpu([context = std::move(captured)]() mutable {
      context.reset();
    });
    cpu.join();
    EXPECT_FALSE(weak.expired());
    EXPECT_EQ(destroyed, 0);
  }
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(destroyed, 1);
}

TEST_F(RequestContextTest, ConcurrentCpuCaptureAndLastLocalPinRelease) {
  for (int i = 0; i < 64; ++i) {
    std::atomic<int> destroyed{0};
    auto owner = makeLocalHandle();
    owner->setContextData(
        testtoken, std::make_unique<LocalLifetimeData>(destroyed));
    auto captured = owner.capture();
    std::weak_ptr<RequestContext> weak = captured;
    folly::test::Barrier release(2);
    std::thread cpu([&release, context = std::move(captured)]() mutable {
      release.wait();
      context.reset();
    });
    release.wait();
    owner.reset();
    cpu.join();
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(destroyed, 1);
  }
}

TEST_F(RequestContextTest, LocalHandlePromotesBeforeCpuOffload) {
  std::atomic<int> destroyed{0};
  constexpr std::size_t numThreads = 8;
  folly::test::Barrier activated(numThreads + 1);
  folly::test::Barrier released(numThreads + 1);
  std::vector<std::thread> threads;
  {
    auto owner = makeLocalHandle();
    owner->setContextData(
        testtoken, std::make_unique<LocalLifetimeData>(destroyed));
    auto* raw = owner.get();
    BorrowedRequestContextScopeGuard guard(owner);
    for (std::size_t i = 0; i < numThreads; ++i) {
      auto capturedContext = RequestContext::saveContext();
      threads.emplace_back([&, raw, context = std::move(capturedContext)] {
        RequestContextScopeGuard cpuGuard(context);
        activated.wait();
        released.wait();
        EXPECT_EQ(RequestContext::try_get(), raw);
        EXPECT_TRUE(RequestContext::get()->hasContextData(testtoken));
        auto captured = RequestContext::saveContext();
        EXPECT_FALSE(captured.owner_before(context));
        EXPECT_FALSE(context.owner_before(captured));
      });
    }
    activated.wait();
    owner.reset();
    EXPECT_EQ(destroyed, 0);
  }
  EXPECT_EQ(destroyed, 0);
  released.wait();
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(destroyed, 1);
}

TEST_F(RequestContextTest, LocalHandleCpuReleaseKeepsEvbScopeAlive) {
  std::atomic<int> destroyed{0};
  auto owner = makeLocalHandle();
  owner->setContextData(
      testtoken, std::make_unique<LocalLifetimeData>(destroyed));
  auto* raw = owner.get();
  {
    BorrowedRequestContextScopeGuard guard(owner);
    std::thread cpu([context = RequestContext::saveContext()]() mutable {
      RequestContextScopeGuard cpuGuard(std::move(context));
      EXPECT_TRUE(RequestContext::get()->hasContextData(testtoken));
    });
    owner.reset();
    cpu.join();
    EXPECT_EQ(destroyed, 0);
    EXPECT_EQ(RequestContext::try_get(), raw);
    EXPECT_TRUE(raw->hasContextData(testtoken));
  }
  EXPECT_EQ(destroyed, 1);
}

#ifndef NDEBUG
TEST_F(RequestContextTest, LocalOwnerRejectsCpuDestruction) {
  EXPECT_DEATH(
      {
        auto owner = std::make_unique<LocalRequestContextScopeGuard>();
        std::thread cpu([scope = std::move(owner)]() mutable {
          scope.reset();
        });
        cpu.join();
      },
      "thread == std::this_thread::get_id");
}

TEST_F(RequestContextTest, LocalHandleRejectsCpuActivation) {
  auto owner = makeLocalHandle();
  EXPECT_DEATH(
      {
        std::thread cpu([&] { BorrowedRequestContextScopeGuard guard(owner); });
        cpu.join();
      },
      "thread == std::this_thread::get_id");
}

TEST_F(RequestContextTest, BorrowedScopeRejectsCpuDestruction) {
  auto owner = makeLocalHandle();
  for (bool sharedPrevious : {false, true}) {
    RequestContextScopeGuard previous(
        sharedPrevious ? std::make_shared<RequestContext>() : nullptr);
    EXPECT_DEATH(
        {
          auto scope =
              std::make_unique<BorrowedRequestContextScopeGuard>(owner);
          std::thread cpu([scope = std::move(scope)]() mutable {
            scope.reset();
          });
          cpu.join();
        },
        "thread_ == std::this_thread::get_id");
  }
}
#endif

TEST_F(RequestContextTest, LocalHandleEventBaseCaptureOutlivesRequest) {
  std::atomic<int> destroyed{0};
  bool ran = false;
  {
    EventBase base;
    auto owner = makeLocalHandle();
    owner->setContextData(
        testtoken, std::make_unique<LocalLifetimeData>(destroyed));
    auto* raw = owner.get();
    {
      BorrowedRequestContextScopeGuard guard(owner);
      base.runInEventBaseThread([&] {
        ran = true;
        EXPECT_EQ(RequestContext::try_get(), raw);
        EXPECT_TRUE(RequestContext::get()->hasContextData(testtoken));
        base.terminateLoopSoon();
      });
    }
    owner.reset();
    EXPECT_EQ(destroyed, 0);
    base.loopForever();
    EXPECT_TRUE(ran);
    EXPECT_EQ(RequestContext::try_get(), nullptr);
  }
  EXPECT_EQ(destroyed, 1);
}

TEST_F(RequestContextTest, LocalHandleSuspendedScopeSurvivesOwnerRelease) {
  std::atomic<int> destroyed{0};
  auto parent = makeLocalHandle();
  auto child = makeLocalHandle();
  parent->setContextData(
      testtoken, std::make_unique<LocalLifetimeData>(destroyed));
  auto* raw = parent.get();
  {
    BorrowedRequestContextScopeGuard outer(parent);
    {
      BorrowedRequestContextScopeGuard inner(child);
      parent.reset();
      child.reset();
      EXPECT_EQ(destroyed, 0);
    }
    EXPECT_EQ(RequestContext::try_get(), raw);
    EXPECT_TRUE(raw->hasContextData(testtoken));
    EXPECT_EQ(destroyed, 0);
  }
  EXPECT_EQ(destroyed, 1);
  EXPECT_EQ(RequestContext::try_get(), nullptr);
}

namespace {
struct WatcherState {
  int callCount{0};
  std::shared_ptr<RequestContext> lastPrev;
  std::shared_ptr<RequestContext> lastCurrent;
};

thread_local WatcherState* g_watcherState = nullptr;

void testWatcher(
    const std::shared_ptr<RequestContext>& prev,
    const std::shared_ptr<RequestContext>& ctx) {
  if (g_watcherState) {
    g_watcherState->callCount++;
    g_watcherState->lastPrev = prev;
    g_watcherState->lastCurrent = ctx;
  }
}

thread_local WatcherState* g_registrationWatcherState = nullptr;
std::atomic<int> g_callbackRegistrations{0};

void registeredDuringCallbackWatcher(
    const std::shared_ptr<RequestContext>& prev,
    const std::shared_ptr<RequestContext>& ctx) {
  if (g_registrationWatcherState) {
    ++g_registrationWatcherState->callCount;
    g_registrationWatcherState->lastPrev = prev;
    g_registrationWatcherState->lastCurrent = ctx;
  }
}

class WatcherRegistrationData : public RequestData {
 public:
  explicit WatcherRegistrationData(bool onSet) : registerOnSet_(onSet) {}
  bool hasCallback() override { return true; }
  void onSet() override { maybeRegister(true); }
  void onUnset() override { maybeRegister(false); }
  void arm() { armed_ = true; }

 private:
  void maybeRegister(bool onSet) {
    if (armed_ && onSet == registerOnSet_) {
      armed_ = false;
      RequestContext::addSetContextWatcher(registeredDuringCallbackWatcher);
      ++g_callbackRegistrations;
    }
  }
  bool registerOnSet_;
  bool armed_{false};
};

} // namespace

TEST_F(RequestContextTest, WatchersRegisteredDuringCallbacksSeeCurrentSwitch) {
  WatcherState watcher;
  g_registrationWatcherState = &watcher;
  SCOPE_EXIT {
    g_registrationWatcherState = nullptr;
  };
  for (bool sharedTarget : {false, true}) {
    for (bool onSet : {true, false}) {
      auto previous = makeLocalHandle();
      auto nextLocal =
          sharedTarget ? RequestContextHandle{} : makeLocalHandle();
      auto nextShared =
          sharedTarget ? std::make_shared<RequestContext>() : nullptr;
      auto* next = sharedTarget ? nextShared.get() : nextLocal.get();
      auto data = std::make_unique<WatcherRegistrationData>(onSet);
      auto* registration = data.get();
      (onSet ? next : previous.get())
          ->setContextData(testtoken, std::move(data));
      BorrowedRequestContextScopeGuard active(previous);
      watcher.callCount = 0;
      registration->arm();
      auto verify = [&] {
        EXPECT_EQ(watcher.callCount, g_callbackRegistrations.load());
        EXPECT_EQ(watcher.lastPrev.get(), previous.get());
        EXPECT_EQ(watcher.lastCurrent.get(), next);
      };
      if (sharedTarget) {
        RequestContextScopeGuard target(nextShared);
        verify();
      } else {
        BorrowedRequestContextScopeGuard target(nextLocal);
        verify();
      }
    }
  }
}

TEST_F(RequestContextTest, AddSetContextWatcher) {
  std::atomic<int> destroyed{0};
  WatcherState watcher;
  g_watcherState = &watcher;
  SCOPE_EXIT {
    g_watcherState = nullptr;
  };
  static folly::once_flag registered;
  folly::call_once(registered, [] {
    RequestContext::addSetContextWatcher(testWatcher);
  });

  // Initially no calls
  EXPECT_EQ(0, watcher.callCount);

  // Create initial context
  RequestContext::create();
  auto ctx1 = RequestContext::saveContext();
  EXPECT_EQ(1, watcher.callCount);
  EXPECT_EQ(nullptr, watcher.lastPrev);
  EXPECT_EQ(ctx1, watcher.lastCurrent);

  // Reset call count for next test
  watcher.callCount = 0;

  // Create and set a new context
  auto ctx2 = std::make_shared<RequestContext>();
  RequestContext::setContext(ctx2);

  EXPECT_EQ(1, watcher.callCount);
  EXPECT_EQ(ctx1, watcher.lastPrev);
  EXPECT_EQ(ctx2, watcher.lastCurrent);

  // Reset call count for next test
  watcher.callCount = 0;

  // Set context to nullptr (should trigger watcher)
  RequestContext::setContext(nullptr);

  EXPECT_EQ(1, watcher.callCount);
  EXPECT_EQ(ctx2, watcher.lastPrev);
  EXPECT_EQ(nullptr, watcher.lastCurrent);

  // Reset call count for next test
  watcher.callCount = 0;

  // Setting the same context will not trigger the watcher
  RequestContext::setContext(nullptr);

  EXPECT_EQ(0, watcher.callCount);
  EXPECT_EQ(ctx2, watcher.lastPrev);
  EXPECT_EQ(nullptr, watcher.lastCurrent);

  RequestContext* raw;
  {
    auto owner = makeLocalHandle();
    owner->setContextData(
        testtoken, std::make_unique<LocalLifetimeData>(destroyed));
    raw = owner.get();
    {
      BorrowedRequestContextScopeGuard guard(owner);
      EXPECT_EQ(watcher.lastCurrent.get(), raw);
      EXPECT_EQ(RequestContext::saveContextLocal().get(), raw);
      auto captured = RequestContext::saveContext();
      EXPECT_EQ(captured.get(), raw);
      EXPECT_EQ(RequestContext::saveContextLocal().get(), raw);
      owner.reset();
      EXPECT_EQ(destroyed, 0);
    }
  }
  EXPECT_EQ(watcher.lastPrev.get(), raw);
  EXPECT_EQ(watcher.lastCurrent, nullptr);
  EXPECT_EQ(destroyed, 0);
  watcher.lastPrev.reset();
  EXPECT_EQ(destroyed, 1);
}

namespace {

// Records the current context when the RequestData is destroyed.
class DestructionObserver : public RequestData {
 public:
  explicit DestructionObserver(
      RequestContext** out, RequestContext** cachedOut = nullptr)
      : out_(out), cachedOut_(cachedOut) {}
  ~DestructionObserver() override {
    *out_ = RequestContext::try_get();
    if (cachedOut_) {
      *cachedOut_ = getCachedRequestContext();
    }
  }
  bool hasCallback() override { return false; }

 private:
  RequestContext** out_;
  RequestContext** cachedOut_;
};

} // namespace

TEST_F(RequestContextTest, LocalThreadTeardownClearsSignalSafeCache) {
  RequestContext* destroyedUnder = nullptr;
  RequestContext* cachedDuringDestruction = nullptr;
  std::thread thread([&] {
    auto handle = makeLocalHandle();
    destroyedUnder = cachedDuringDestruction = handle.get();
    handle->setContextData(
        testtoken,
        std::make_unique<DestructionObserver>(
            &destroyedUnder, &cachedDuringDestruction));
    // Leave the binding active for TLS cleanup without allocating a guard.
    alignas(BorrowedRequestContextScopeGuard)
        std::byte storage[sizeof(BorrowedRequestContextScopeGuard)];
    new (storage) BorrowedRequestContextScopeGuard(handle);
    EXPECT_EQ(getCachedRequestContext(), handle.get());
    handle.reset();
  });
  thread.join();
  EXPECT_EQ(destroyedUnder, nullptr);
  EXPECT_EQ(cachedDuringDestruction, nullptr);
}

TEST_F(RequestContextTest, SaverScopeGuardSetContext) {
  auto prev = RequestContext::saveContext();
  {
    RequestContextSaverScopeGuard guard;
    auto ctx = std::make_shared<RequestContext>();
    auto* raw = ctx.get();
    guard.setContext(std::move(ctx));
    EXPECT_EQ(ctx, nullptr);
    EXPECT_EQ(RequestContext::try_get(), raw);
  }
  EXPECT_EQ(RequestContext::try_get(), prev.get());
}

TEST_F(RequestContextTest, SaverScopeGuardSetContextSameContext) {
  auto ctx = std::make_shared<RequestContext>();
  RequestContext::setContext(ctx);
  RequestContextSaverScopeGuard guard;
  auto same = ctx;
  guard.setContext(std::move(same));
  // The argument is consumed even when there is nothing to replace.
  EXPECT_EQ(same, nullptr);
  EXPECT_EQ(RequestContext::try_get(), ctx.get());
}

TEST_F(RequestContextTest, SaverScopeGuardDestroysReplacedContextUnderPrev) {
  auto prev = std::make_shared<RequestContext>();
  RequestContext::setContext(prev);
  RequestContext* destroyedUnder = nullptr;
  auto victim = std::make_shared<RequestContext>();
  victim->setContextData(
      "victim", std::make_unique<DestructionObserver>(&destroyedUnder));
  {
    RequestContextSaverScopeGuard guard;
    guard.setContext(std::move(victim));
    EXPECT_EQ(victim, nullptr);
    auto next = std::make_shared<RequestContext>();
    guard.setContext(std::move(next));
    EXPECT_EQ(next, nullptr);
    EXPECT_EQ(destroyedUnder, prev.get());
  }
  EXPECT_EQ(RequestContext::try_get(), prev.get());
}

TEST_F(RequestContextTest, SaverScopeGuardRestoreContext) {
  auto prev = std::make_shared<RequestContext>();
  RequestContext::setContext(prev);
  {
    RequestContextSaverScopeGuard guard;
    auto other = std::make_shared<RequestContext>();
    RequestContext::setContext(other);
    EXPECT_EQ(RequestContext::try_get(), other.get());
    guard.restoreContext();
    EXPECT_EQ(RequestContext::try_get(), prev.get());
  }
  EXPECT_EQ(RequestContext::try_get(), prev.get());
}
