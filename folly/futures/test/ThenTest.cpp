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

#include <thread>
#include <glog/logging.h>
#include <gtest/gtest_pred_impl.h>

#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/GlobalExecutor.h>
#include <folly/futures/Future.h>
#include <folly/portability/GTest.h>
#include <folly/system/ThreadId.h>
#include <folly/system/ThreadName.h>

using namespace folly;

namespace {
struct Widget {
  int v_, copied_, moved_;
  /* implicit */ Widget(int v) : v_(v), copied_(0), moved_(0) {}
  Widget(const Widget& other)
      : v_(other.v_), copied_(other.copied_ + 1), moved_(other.moved_) {}
  Widget(Widget&& other) noexcept
      : v_(other.v_), copied_(other.copied_), moved_(other.moved_ + 1) {}
  Widget& operator=(const Widget& /* other */) {
    throw std::logic_error("unexpected copy assignment");
  }
  [[maybe_unused]] Widget& operator=(Widget&& /* other */) {
    throw std::logic_error("unexpected move assignment");
  }
};

struct CountedWidget : Widget {
  static std::vector<Widget*> instances_;
  bool alive = true;
  /* implicit */ CountedWidget(int v) : Widget(v) {
    instances_.push_back(this);
  }
  CountedWidget(const CountedWidget& other) : Widget(other) {
    instances_.push_back(this);
  }
  CountedWidget(CountedWidget&& other) noexcept(false)
      : Widget(std::move(other)) {
    other.alive = false;
    other.remove();
    instances_.push_back(this);
  }
  ~CountedWidget() {
    if (alive) {
      remove();
    }
  }

 private:
  CountedWidget& operator=(const CountedWidget&) = delete;
  CountedWidget& operator=(CountedWidget&&) = delete;

  void remove() {
    auto iter = std::find(instances_.begin(), instances_.end(), this);
    EXPECT_TRUE(iter != instances_.end());
    instances_.erase(iter);
  }
};

std::vector<Widget*> CountedWidget::instances_;
} // namespace

TEST(Then, tryConstructor) {
  auto t = Try<Widget>(23);
  EXPECT_EQ(t.value().v_, 23);
  EXPECT_EQ(t.value().copied_, 0);
  EXPECT_EQ(t.value().moved_, 1);
}

TEST(Then, makeFuture) {
  auto future = makeFuture<Widget>(23);
  EXPECT_EQ(future.value().v_, 23);
  EXPECT_EQ(future.value().copied_, 0);
  EXPECT_EQ(future.value().moved_, 2);
}

TEST(Then, tryConstRValueReference) {
  auto future = makeFuture<Widget>(23).then([](const Try<Widget>&& t) {
    EXPECT_EQ(t.value().copied_, 0);
    EXPECT_EQ(t.value().moved_, 2);
    return t.value().v_;
  });
  EXPECT_EQ(future.value(), 23);
}

TEST(Then, tryRValueReference) {
  auto future = makeFuture<Widget>(23).then([](Try<Widget>&& t) {
    EXPECT_EQ(t.value().copied_, 0);
    EXPECT_EQ(t.value().moved_, 2);
    return t.value().v_;
  });
  EXPECT_EQ(future.value(), 23);
}

TEST(Then, tryConstLValueReference) {
  auto future = makeFuture<Widget>(23).then([](const Try<Widget>& t) {
    EXPECT_EQ(t.value().copied_, 0);
    EXPECT_EQ(t.value().moved_, 2);
    return t.value().v_;
  });
  EXPECT_EQ(future.value(), 23);
}

TEST(Then, tryValue) {
  auto future = makeFuture<Widget>(23).then([](Try<Widget> t) {
    EXPECT_EQ(t.value().copied_, 0);
    EXPECT_EQ(t.value().moved_, 3);
    return t.value().v_;
  });
  EXPECT_EQ(future.value(), 23);
}

TEST(Then, tryConstValue) {
  auto future = makeFuture<Widget>(23).then([](const Try<Widget> t) {
    EXPECT_EQ(t.value().copied_, 0);
    EXPECT_EQ(t.value().moved_, 3);
    return t.value().v_;
  });
  EXPECT_EQ(future.value(), 23);
}

TEST(Then, constRValueReference) {
  auto future = makeFuture<Widget>(23).thenValue([](const Widget&& w) {
    EXPECT_EQ(w.copied_, 0);
    EXPECT_EQ(w.moved_, 2);
    return w.v_;
  });
  EXPECT_EQ(future.value(), 23);
}

TEST(Then, rValueReference) {
  auto future = makeFuture<Widget>(23).thenValue([](Widget&& w) {
    EXPECT_EQ(w.copied_, 0);
    EXPECT_EQ(w.moved_, 2);
    return w.v_;
  });
  EXPECT_EQ(future.value(), 23);
}

TEST(Then, constLValueReference) {
  auto future = makeFuture<Widget>(23).thenValue([](const Widget& w) {
    EXPECT_EQ(w.copied_, 0);
    EXPECT_EQ(w.moved_, 2);
    return w.v_;
  });
  EXPECT_EQ(future.value(), 23);
}

TEST(Then, value) {
  auto future = makeFuture<Widget>(23).thenValue([](Widget w) {
    EXPECT_EQ(w.copied_, 0);
    EXPECT_EQ(w.moved_, 3);
    return w.v_;
  });
  EXPECT_EQ(future.value(), 23);
}

TEST(Then, constValue) {
  auto future = makeFuture<Widget>(23).thenValue([](const Widget w) {
    EXPECT_EQ(w.copied_, 0);
    EXPECT_EQ(w.moved_, 3);
    return w.v_;
  });
  EXPECT_EQ(future.value(), 23);
}

TEST(Then, objectAliveDuringImmediateNoParamContinuation) {
  auto f = makeFuture<CountedWidget>(23);
  auto called = false;
  std::move(f).thenValue([&](auto&&) {
    EXPECT_EQ(CountedWidget::instances_.size(), 1u);
    EXPECT_EQ(CountedWidget::instances_[0]->v_, 23);
    called = true;
  });
  EXPECT_EQ(true, called);
}

TEST(Then, objectAliveDuringDeferredNoParamContinuation) {
  auto p = Promise<CountedWidget>{};
  bool called = false;
  p.getFuture().thenValue([&](auto&&) {
    EXPECT_EQ(CountedWidget::instances_.size(), 1u);
    EXPECT_EQ(CountedWidget::instances_[0]->v_, 23);
    called = true;
  });
  p.setValue(CountedWidget{23});
  EXPECT_EQ(true, called);
}

TEST(Then, voidThenShouldPropagateExceptions) {
  EXPECT_FALSE(makeFuture(42).then().hasException());
  EXPECT_TRUE(makeFuture<int>(std::runtime_error("err")).then().hasException());
}

using namespace std::chrono;
TEST(Then, thenInlineFollowContinuationReturnsValue) {
  uint64_t threadId0 = folly::getCurrentThreadID();
  uint64_t threadId1 = 0;
  folly::makeFuture()
      .via(folly::getGlobalCPUExecutor())
      .thenValue([&threadId1](auto&&) { // callback-1
        threadId1 = folly::getCurrentThreadID();
        return true;
      })
      .thenInline([&threadId0, &threadId1](auto&&) { // callback-2
        auto threadId2 = folly::getCurrentThreadID();
        EXPECT_PRED3(
            [](auto t0, auto t1, auto t2) {
              // There is a race condition that when this 'thenInline' is
              // executed, has callback-1 already finished:
              //  - Yes, hence this(callback-2) is executed inline in threadId0
              //  - No, hence this(callback-2) is executed inline in threadId1
              return t2 == t0 || t2 == t1;
            },
            threadId0,
            threadId1,
            threadId2);
      })
      .wait();
}

TEST(Then, thenInlineFollowContinuationReturnsImmeidateFuture) {
  uint64_t threadId1 = 0;
  folly::makeFuture()
      .via(folly::getGlobalCPUExecutor())
      .thenValue([&threadId1](auto&&) {
        threadId1 = folly::getCurrentThreadID();
        return folly::makeFuture(true);
      })
      .thenInline([&threadId1](auto&&) {
        auto threadId2 = folly::getCurrentThreadID();
        ASSERT_EQ(threadId1, threadId2);
      })
      .wait();
}
