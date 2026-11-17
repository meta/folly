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

#include <folly/executors/testing/SimpleThreadPoolExecutor.h>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/portability/GTest.h>
#include <folly/synchronization/Baton.h>

using namespace folly;

TEST(Interrupt, raise) {
  using eggs_t = std::runtime_error;
  Promise<Unit> p;
  p.setInterruptHandler([&](const exception_wrapper& e) {
    EXPECT_THROW(e.throw_exception(), eggs_t);
  });
  p.getFuture().raise(eggs_t("eggs"));
}

TEST(Interrupt, cancel) {
  Promise<Unit> p;
  p.setInterruptHandler([&](const exception_wrapper& e) {
    EXPECT_THROW(e.throw_exception(), FutureCancellation);
  });
  p.getFuture().cancel();
}

TEST(Interrupt, handleThenInterrupt) {
  Promise<int> p;
  bool flag = false;
  p.setInterruptHandler([&](const exception_wrapper& /* e */) { flag = true; });
  p.getFuture().cancel();
  EXPECT_TRUE(flag);
}

TEST(Interrupt, interruptThenHandle) {
  Promise<int> p;
  bool flag = false;
  p.getFuture().cancel();
  p.setInterruptHandler([&](const exception_wrapper& /* e */) { flag = true; });
  EXPECT_TRUE(flag);
}

TEST(Interrupt, interruptAfterFulfilNoop) {
  Promise<Unit> p;
  bool flag = false;
  p.setInterruptHandler([&](const exception_wrapper& /* e */) { flag = true; });
  p.setValue();
  p.getFuture().cancel();
  EXPECT_FALSE(flag);
}

TEST(Interrupt, secondInterruptNoop) {
  Promise<Unit> p;
  int count = 0;
  p.setInterruptHandler([&](const exception_wrapper& /* e */) { count++; });
  auto f = p.getFuture();
  f.cancel();
  f.cancel();
  EXPECT_EQ(1, count);
}

TEST(Interrupt, futureWithinTimedOut) {
  Promise<int> p;
  Baton<> done;
  p.setInterruptHandler([&](const exception_wrapper& /* e */) { done.post(); });
  p.getFuture().within(std::chrono::milliseconds(1));
  // Give it 100ms to time out and call the interrupt handler
  EXPECT_TRUE(done.try_wait_for(std::chrono::milliseconds(100)));
}

TEST(Interrupt, semiFutureWithinTimedOut) {
  folly::test::SimpleThreadPoolExecutor ex(1);
  Promise<int> p;
  Baton<> done;
  p.setInterruptHandler([&](const exception_wrapper& /* e */) { done.post(); });
  p.getSemiFuture().within(std::chrono::milliseconds(1)).via(&ex);
  // Give it 100ms to time out and call the interrupt handler
  EXPECT_TRUE(done.try_wait_for(std::chrono::milliseconds(100)));
}

TEST(Interrupt, futureThenValue) {
  folly::test::SimpleThreadPoolExecutor ex(1);
  Promise<folly::Unit> p;
  bool flag = false;
  p.setInterruptHandler([&](const exception_wrapper& /* e */) { flag = true; });
  p.getFuture().thenValue([](folly::Unit&&) {}).cancel();
  EXPECT_TRUE(flag);
}

TEST(Interrupt, semiFutureDeferValue) {
  folly::test::SimpleThreadPoolExecutor ex(1);
  Promise<folly::Unit> p;
  bool flag = false;
  p.setInterruptHandler([&](const exception_wrapper& /* e */) { flag = true; });
  p.getSemiFuture().deferValue([](folly::Unit&&) {}).cancel();
  EXPECT_TRUE(flag);
}

TEST(Interrupt, futureThenValueFuture) {
  folly::test::SimpleThreadPoolExecutor ex(1);
  Promise<folly::Unit> p;
  bool flag = false;
  p.setInterruptHandler([&](const exception_wrapper& /* e */) { flag = true; });
  p.getFuture()
      .thenValue([](folly::Unit&&) { return folly::makeFuture(); })
      .cancel();
  EXPECT_TRUE(flag);
}

TEST(Interrupt, semiFutureDeferValueSemiFuture) {
  folly::test::SimpleThreadPoolExecutor ex(1);
  Promise<folly::Unit> p;
  bool flag = false;
  p.setInterruptHandler([&](const exception_wrapper& /* e */) { flag = true; });
  p.getSemiFuture()
      .deferValue([](folly::Unit&&) { return folly::makeSemiFuture(); })
      .cancel();
  EXPECT_TRUE(flag);
}

TEST(Interrupt, futureThenError) {
  folly::test::SimpleThreadPoolExecutor ex(1);
  Promise<folly::Unit> p;
  bool flag = false;
  p.setInterruptHandler([&](const exception_wrapper& /* e */) { flag = true; });
  p.getFuture().thenError([](const exception_wrapper& /* e */) {}).cancel();
  EXPECT_TRUE(flag);
}

TEST(Interrupt, semiFutureDeferError) {
  folly::test::SimpleThreadPoolExecutor ex(1);
  Promise<folly::Unit> p;
  bool flag = false;
  p.setInterruptHandler([&](const exception_wrapper& /* e */) { flag = true; });
  p.getSemiFuture()
      .deferError([](const exception_wrapper& /* e */) {})
      .cancel();
  EXPECT_TRUE(flag);
}

TEST(Interrupt, futureThenErrorTagged) {
  folly::test::SimpleThreadPoolExecutor ex(1);
  Promise<folly::Unit> p;
  bool flag = false;
  p.setInterruptHandler([&](const exception_wrapper& /* e */) { flag = true; });
  p.getFuture()
      .thenError(
          folly::tag_t<std::runtime_error>{},
          [](const exception_wrapper& /* e */) {})
      .cancel();
  EXPECT_TRUE(flag);
}

TEST(Interrupt, semiFutureDeferErrorTagged) {
  folly::test::SimpleThreadPoolExecutor ex(1);
  Promise<folly::Unit> p;
  bool flag = false;
  p.setInterruptHandler([&](const exception_wrapper& /* e */) { flag = true; });
  p.getSemiFuture()
      .deferError(
          folly::tag_t<std::runtime_error>{},
          [](const exception_wrapper& /* e */) {})
      .cancel();
  EXPECT_TRUE(flag);
}

TEST(Interrupt, futureThenErrorFuture) {
  folly::test::SimpleThreadPoolExecutor ex(1);
  Promise<folly::Unit> p;
  bool flag = false;
  p.setInterruptHandler([&](const exception_wrapper& /* e */) { flag = true; });
  p.getFuture()
      .thenError([](const exception_wrapper& /* e */) { return makeFuture(); })
      .cancel();
  EXPECT_TRUE(flag);
}

TEST(Interrupt, semiFutureDeferErrorSemiFuture) {
  folly::test::SimpleThreadPoolExecutor ex(1);
  Promise<folly::Unit> p;
  bool flag = false;
  p.setInterruptHandler([&](const exception_wrapper& /* e */) { flag = true; });
  p.getSemiFuture()
      .deferError([](const exception_wrapper& /* e */) {
        return makeSemiFuture();
      })
      .cancel();
  EXPECT_TRUE(flag);
}

TEST(Interrupt, futureThenErrorTaggedFuture) {
  folly::test::SimpleThreadPoolExecutor ex(1);
  Promise<folly::Unit> p;
  bool flag = false;
  p.setInterruptHandler([&](const exception_wrapper& /* e */) { flag = true; });
  p.getFuture()
      .thenError(
          folly::tag_t<std::runtime_error>{},
          [](const exception_wrapper& /* e */) { return makeFuture(); })
      .cancel();
  EXPECT_TRUE(flag);
}

TEST(Interrupt, semiFutureDeferErrorTaggedSemiFuture) {
  folly::test::SimpleThreadPoolExecutor ex(1);
  Promise<folly::Unit> p;
  bool flag = false;
  p.setInterruptHandler([&](const exception_wrapper& /* e */) { flag = true; });
  p.getSemiFuture()
      .deferError(
          folly::tag_t<std::runtime_error>{},
          [](const exception_wrapper& /* e */) { return makeSemiFuture(); })
      .cancel();
  EXPECT_TRUE(flag);
}
