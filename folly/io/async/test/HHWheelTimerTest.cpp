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

#include <folly/io/async/HHWheelTimer.h>

#include <folly/io/async/DelayedDestruction.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/test/Util.h>
#include <folly/portability/GTest.h>
#include <folly/portability/Unistd.h>

using namespace folly;
using std::chrono::milliseconds;

using StackWheelTimer = UndelayedDestruction<HHWheelTimer>;

class TestTimeout : public HHWheelTimer::Callback {
 public:
  TestTimeout() {}
  TestTimeout(HHWheelTimer* t, milliseconds timeout) {
    t->scheduleTimeout(this, timeout);
  }

  void timeoutExpired() noexcept override {
    timestamps.emplace_back();
    if (fn) {
      fn();
    }
  }

  void callbackCanceled() noexcept override {
    canceledTimestamps.emplace_back();
    if (fn) {
      fn();
    }
  }

  std::deque<TimePoint> timestamps;
  std::deque<TimePoint> canceledTimestamps;
  std::function<void()> fn;
};

struct HHWheelTimerTest : public ::testing::Test {
  EventBase eventBase;
};

/*
 * Test firing some simple timeouts that are fired once and never rescheduled
 */
TEST_F(HHWheelTimerTest, FireOnce) {
  StackWheelTimer t(&eventBase, milliseconds(1));

  TestTimeout t1;
  TestTimeout t2;
  TestTimeout t3;

  ASSERT_EQ(t.count(), 0);

  t.scheduleTimeout(&t1, milliseconds(20));
  t.scheduleTimeout(&t2, milliseconds(20));
  // Verify scheduling it twice cancels, then schedules.
  // Should only get one callback.
  t.scheduleTimeout(&t2, milliseconds(20));
  t.scheduleTimeout(&t3, milliseconds(40));

  ASSERT_EQ(t.count(), 3);

  TimePoint start;
  eventBase.loop();
  TimePoint end;

  ASSERT_EQ(t1.timestamps.size(), 1);
  ASSERT_EQ(t2.timestamps.size(), 1);
  ASSERT_EQ(t3.timestamps.size(), 1);

  ASSERT_EQ(t.count(), 0);

  T_CHECK_NOT_EARLY(start, t1.timestamps[0], milliseconds(20));
  T_CHECK_NOT_EARLY(start, t2.timestamps[0], milliseconds(20));
  T_CHECK_NOT_EARLY(start, t3.timestamps[0], milliseconds(40));
  T_CHECK_NOT_EARLY(start, end, milliseconds(40));
  T_CHECK_BEFORE(t1.timestamps[0], t3.timestamps[0]);
  T_CHECK_BEFORE(t2.timestamps[0], t3.timestamps[0]);
}

TEST_F(HHWheelTimerTest, NoRequestContextLeak) {
  StackWheelTimer t(&eventBase, milliseconds(1));
  std::set<int> destructed;

  class TestData : public RequestData {
   public:
    TestData(int data, std::set<int>& destructed)
        : data_(data), destructed_(destructed) {}
    ~TestData() override { destructed_.insert(data_); }

    bool hasCallback() override { return false; }

   private:
    int data_;
    std::set<int>& destructed_;
  };

  folly::Optional<TestTimeout> t1 = TestTimeout{};
  folly::Optional<TestTimeout> t2 = TestTimeout{};

  {
    RequestContextScopeGuard g;
    RequestContext::get()->setContextData(
        "k", std::make_unique<TestData>(1, destructed));
    t.scheduleTimeout(&*t1, milliseconds(5));
  }

  {
    RequestContextScopeGuard g;
    RequestContext::get()->setContextData(
        "k", std::make_unique<TestData>(2, destructed));
    t.scheduleTimeout(&*t2, milliseconds(5));
  }

  EXPECT_EQ(0, destructed.size());
  t1.reset();
  EXPECT_TRUE(destructed.contains(1));
  EXPECT_FALSE(destructed.contains(2));
}

/*
 * Test scheduling a timeout from another timeout callback.
 */
TEST_F(HHWheelTimerTest, TestSchedulingWithinCallback) {
  HHWheelTimer& t = eventBase.timer();

  TestTimeout t1, t2;

  t.scheduleTimeout(&t1, milliseconds(500));
  t1.fn = [&] {
    t.scheduleTimeout(&t2, milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  };
  // If t is in an inconsistent state, detachEventBase should fail.
  t2.fn = [&] { t.detachEventBase(); };

  ASSERT_EQ(t.count(), 1);

  eventBase.loop();

  ASSERT_EQ(t.count(), 0);
  ASSERT_EQ(t1.timestamps.size(), 1);
  ASSERT_EQ(t2.timestamps.size(), 1);
}

/*
 * Test changing default-timeout in timer.
 */
TEST_F(HHWheelTimerTest, TestSetDefaultTimeout) {
  HHWheelTimer& t = eventBase.timer();

  t.setDefaultTimeout(milliseconds(1000));
  // verify: default-time has been modified
  ASSERT_EQ(t.getDefaultTimeout(), milliseconds(1000));
}

/*
 * Test cancelling a timeout when it is scheduled to be fired right away.
 */

TEST_F(HHWheelTimerTest, CancelTimeout) {
  StackWheelTimer t(&eventBase, milliseconds(1));

  // Create several timeouts that will all fire in 5ms.
  TestTimeout t5_1(&t, milliseconds(5));
  TestTimeout t5_2(&t, milliseconds(5));
  TestTimeout t5_3(&t, milliseconds(5));
  TestTimeout t5_4(&t, milliseconds(5));
  TestTimeout t5_5(&t, milliseconds(5));

  // Also create a few timeouts to fire in 10ms
  TestTimeout t10_1(&t, milliseconds(10));
  TestTimeout t10_2(&t, milliseconds(10));
  TestTimeout t10_3(&t, milliseconds(10));

  TestTimeout t20_1(&t, milliseconds(20));
  TestTimeout t20_2(&t, milliseconds(20));

  // Have t5_1 cancel t5_2 and t5_4.
  //
  // Cancelling t5_2 will test cancelling a timeout that is at the head of the
  // list and ready to be fired.
  //
  // Cancelling t5_4 will test cancelling a timeout in the middle of the list
  t5_1.fn = [&] {
    t5_2.cancelTimeout();
    t5_4.cancelTimeout();
  };

  // Have t5_3 cancel t5_5.
  // This will test cancelling the last remaining timeout.
  //
  // Then have t5_3 reschedule itself.
  t5_3.fn = [&] {
    t5_5.cancelTimeout();
    // Reset our function so we won't continually reschedule ourself
    std::function<void()> fnDtorGuard;
    t5_3.fn.swap(fnDtorGuard);
    t.scheduleTimeout(&t5_3, milliseconds(5));

    // Also test cancelling timeouts in another timeset that isn't ready to
    // fire yet.
    //
    // Cancel the middle timeout in ts10.
    t10_2.cancelTimeout();
    // Cancel both the timeouts in ts20.
    t20_1.cancelTimeout();
    t20_2.cancelTimeout();
  };

  TimePoint start;
  eventBase.loop();
  TimePoint end;

  ASSERT_EQ(t5_1.timestamps.size(), 1);
  T_CHECK_NOT_EARLY(start, t5_1.timestamps[0], milliseconds(5));

  ASSERT_EQ(t5_3.timestamps.size(), 2);
  T_CHECK_NOT_EARLY(start, t5_3.timestamps[0], milliseconds(5));
  T_CHECK_NOT_EARLY(t5_3.timestamps[0], t5_3.timestamps[1], milliseconds(5));

  ASSERT_EQ(t10_1.timestamps.size(), 1);
  T_CHECK_NOT_EARLY(start, t10_1.timestamps[0], milliseconds(10));
  ASSERT_EQ(t10_3.timestamps.size(), 1);
  T_CHECK_NOT_EARLY(start, t10_3.timestamps[0], milliseconds(10));

  T_CHECK_BEFORE(t5_1.timestamps[0], t10_1.timestamps[0]);
  T_CHECK_BEFORE(t5_1.timestamps[0], t10_3.timestamps[0]);
  T_CHECK_BEFORE(t5_3.timestamps[0], t10_1.timestamps[0]);
  T_CHECK_BEFORE(t5_3.timestamps[0], t10_3.timestamps[0]);

  // Cancelled timeouts
  ASSERT_EQ(t5_2.timestamps.size(), 0);
  ASSERT_EQ(t5_4.timestamps.size(), 0);
  ASSERT_EQ(t5_5.timestamps.size(), 0);
  ASSERT_EQ(t10_2.timestamps.size(), 0);
  ASSERT_EQ(t20_1.timestamps.size(), 0);
  ASSERT_EQ(t20_2.timestamps.size(), 0);

  T_CHECK_NOT_EARLY(start, end, milliseconds(10));
}

/*
 * Test destroying a HHWheelTimer with timeouts outstanding
 */

TEST_F(HHWheelTimerTest, DestroyTimeoutSet) {
  HHWheelTimer::UniquePtr t(
      HHWheelTimer::newTimer(&eventBase, milliseconds(1)));

  TestTimeout t5_1(t.get(), milliseconds(5));
  TestTimeout t5_2(t.get(), milliseconds(5));
  TestTimeout t5_3(t.get(), milliseconds(5));

  TestTimeout t10_1(t.get(), milliseconds(10));
  TestTimeout t10_2(t.get(), milliseconds(10));

  // Have t5_2 destroy t
  // Note that this will call destroy() inside t's timeoutExpired()
  // method.
  t5_2.fn = [&] {
    t5_3.cancelTimeout();
    t5_1.cancelTimeout();
    t10_1.cancelTimeout();
    t10_2.cancelTimeout();
    t.reset();
  };

  TimePoint start;
  eventBase.loop();
  TimePoint end;

  ASSERT_EQ(t5_1.timestamps.size(), 1);
  T_CHECK_NOT_EARLY(start, t5_1.timestamps[0], milliseconds(5));
  ASSERT_EQ(t5_2.timestamps.size(), 1);
  T_CHECK_NOT_EARLY(start, t5_2.timestamps[0], milliseconds(5));

  ASSERT_EQ(t5_3.timestamps.size(), 0);
  ASSERT_EQ(t10_1.timestamps.size(), 0);
  ASSERT_EQ(t10_2.timestamps.size(), 0);

  T_CHECK_NOT_EARLY(start, end, milliseconds(5));
}

/*
 * Test an event scheduled before the last event fires on time
 */
TEST_F(HHWheelTimerTest, SlowFast) {
  StackWheelTimer t(&eventBase, milliseconds(1));

  TestTimeout t1;
  TestTimeout t2;

  ASSERT_EQ(t.count(), 0);

  t.scheduleTimeout(&t1, milliseconds(10));
  t.scheduleTimeout(&t2, milliseconds(5));

  ASSERT_EQ(t.count(), 2);

  TimePoint start;
  eventBase.loop();
  TimePoint end;

  ASSERT_EQ(t1.timestamps.size(), 1);
  ASSERT_EQ(t2.timestamps.size(), 1);
  ASSERT_EQ(t.count(), 0);

  T_CHECK_NOT_EARLY(start, t1.timestamps[0], milliseconds(10));
  T_CHECK_NOT_EARLY(start, t2.timestamps[0], milliseconds(5));
  T_CHECK_BEFORE(t2.timestamps[0], t1.timestamps[0]);
}

TEST_F(HHWheelTimerTest, ReschedTest) {
  StackWheelTimer t(&eventBase, milliseconds(1));

  TestTimeout t1;
  TestTimeout t2;

  ASSERT_EQ(t.count(), 0);

  t.scheduleTimeout(&t1, milliseconds(128));
  TimePoint start2;
  t1.fn = [&]() {
    t.scheduleTimeout(&t2, milliseconds(255)); // WHEEL_SIZE - 1
    start2.reset();
    ASSERT_EQ(t.count(), 1);
  };

  ASSERT_EQ(t.count(), 1);

  TimePoint start;
  eventBase.loop();
  TimePoint end;

  ASSERT_EQ(t1.timestamps.size(), 1);
  ASSERT_EQ(t2.timestamps.size(), 1);
  ASSERT_EQ(t.count(), 0);

  T_CHECK_NOT_EARLY(start, t1.timestamps[0], milliseconds(128));
  T_CHECK_NOT_EARLY(start2, t2.timestamps[0], milliseconds(255));
}

TEST_F(HHWheelTimerTest, DeleteWheelInTimeout) {
  auto t = HHWheelTimer::newTimer(&eventBase, milliseconds(1));

  TestTimeout t1;
  TestTimeout t2;
  TestTimeout t3;

  ASSERT_EQ(t->count(), 0);

  t->scheduleTimeout(&t1, milliseconds(128));
  t->scheduleTimeout(&t2, milliseconds(128));
  t->scheduleTimeout(&t3, milliseconds(128));
  t1.fn = [&]() { t2.cancelTimeout(); };
  t3.fn = [&]() { t.reset(); };

  ASSERT_EQ(t->count(), 3);

  TimePoint start;
  eventBase.loop();
  TimePoint end;

  ASSERT_EQ(t1.timestamps.size(), 1);
  ASSERT_EQ(t2.timestamps.size(), 0);

  T_CHECK_NOT_EARLY(start, t1.timestamps[0], milliseconds(128));
}

/*
 * Test scheduling a mix of timers with default timeout and variable timeout.
 */
TEST_F(HHWheelTimerTest, DefaultTimeout) {
  milliseconds defaultTimeout(milliseconds(5));
  StackWheelTimer t(
      &eventBase,
      milliseconds(1),
      AsyncTimeout::InternalEnum::NORMAL,
      defaultTimeout);

  TestTimeout t1;
  TestTimeout t2;

  ASSERT_EQ(t.count(), 0);
  ASSERT_EQ(t.getDefaultTimeout(), defaultTimeout);

  t.scheduleTimeout(&t1);
  t.scheduleTimeout(&t2, milliseconds(10));

  ASSERT_EQ(t.count(), 2);

  TimePoint start;
  eventBase.loop();
  TimePoint end;

  ASSERT_EQ(t1.timestamps.size(), 1);
  ASSERT_EQ(t2.timestamps.size(), 1);

  ASSERT_EQ(t.count(), 0);

  T_CHECK_NOT_EARLY(start, t1.timestamps[0], defaultTimeout);
  T_CHECK_NOT_EARLY(start, t2.timestamps[0], milliseconds(10));
  T_CHECK_NOT_EARLY(start, end, milliseconds(10));
  T_CHECK_BEFORE(t1.timestamps[0], t2.timestamps[0]);
}

TEST_F(HHWheelTimerTest, lambda) {
  StackWheelTimer t(&eventBase, milliseconds(1));
  size_t count = 0;
  t.scheduleTimeoutFn([&] { count++; }, milliseconds(1));
  eventBase.loop();
  EXPECT_EQ(1, count);
}

// shouldn't crash because we swallow and log the error (you'll have to look
// at the console to confirm logging)
TEST_F(HHWheelTimerTest, lambdaThrows) {
  StackWheelTimer t(&eventBase, milliseconds(1));
  t.scheduleTimeoutFn(
      [&] { throw std::runtime_error("expected"); }, milliseconds(1));
  eventBase.loop();
}

TEST_F(HHWheelTimerTest, cancelAll) {
  StackWheelTimer t(&eventBase, milliseconds(1));
  TestTimeout t1;
  TestTimeout t2;
  t.scheduleTimeout(&t1, std::chrono::milliseconds(1));
  t.scheduleTimeout(&t2, std::chrono::milliseconds(1));
  size_t canceled = 0;
  t1.fn = [&] { canceled += t.cancelAll(); };
  t2.fn = [&] { canceled += t.cancelAll(); };
  // Sleep 20ms to ensure both timeouts will fire in a single event (in case
  // they ended up in different slots)
  ::usleep(20000);
  eventBase.loop();
  EXPECT_EQ(1, t1.canceledTimestamps.size() + t2.canceledTimestamps.size());
  EXPECT_EQ(1, canceled);
}

TEST_F(HHWheelTimerTest, IntrusivePtr) {
  HHWheelTimer::UniquePtr t(
      HHWheelTimer::newTimer(&eventBase, milliseconds(1)));

  TestTimeout t1;
  TestTimeout t2;
  TestTimeout t3;

  ASSERT_EQ(t->count(), 0);

  t->scheduleTimeout(&t1, milliseconds(5));
  t->scheduleTimeout(&t2, milliseconds(5));

  DelayedDestruction::IntrusivePtr<HHWheelTimer> s(t);

  s->scheduleTimeout(&t3, milliseconds(10));

  ASSERT_EQ(t->count(), 3);

  // Kill the UniquePtr, but the SharedPtr keeps it alive
  t.reset();

  TimePoint start;
  eventBase.loop();
  TimePoint end;

  ASSERT_EQ(t1.timestamps.size(), 1);
  ASSERT_EQ(t2.timestamps.size(), 1);
  ASSERT_EQ(t3.timestamps.size(), 1);

  ASSERT_EQ(s->count(), 0);

  T_CHECK_NOT_EARLY(start, t1.timestamps[0], milliseconds(5));
  T_CHECK_NOT_EARLY(start, t2.timestamps[0], milliseconds(5));
  T_CHECK_NOT_EARLY(start, t3.timestamps[0], milliseconds(10));
  T_CHECK_NOT_EARLY(start, end, milliseconds(10));
  T_CHECK_BEFORE(t1.timestamps[0], t3.timestamps[0]);
  T_CHECK_BEFORE(t2.timestamps[0], t3.timestamps[0]);
}

TEST_F(HHWheelTimerTest, GetTimeRemaining) {
  StackWheelTimer t(&eventBase, milliseconds(1));
  TestTimeout t1;

  // Not scheduled yet, time remaining should be zero
  ASSERT_EQ(t1.getTimeRemaining(), milliseconds(0));
  ASSERT_EQ(t.count(), 0);

  // Scheduled, time remaining should be less than or equal to the scheduled
  // timeout
  t.scheduleTimeout(&t1, milliseconds(10));
  ASSERT_LE(t1.getTimeRemaining(), milliseconds(10));

  TimePoint start;
  eventBase.loop();
  TimePoint end;

  // Expired and time remaining should be zero
  ASSERT_EQ(t1.getTimeRemaining(), milliseconds(0));

  ASSERT_EQ(t.count(), 0);
  T_CHECK_NOT_EARLY(start, end, milliseconds(10));
}

TEST_F(HHWheelTimerTest, prematureTimeout) {
  StackWheelTimer t(&eventBase, milliseconds(10));
  TestTimeout t1;
  TestTimeout t2;
  // Schedule the timeout for the nextTick of timer
  t.scheduleTimeout(&t1, std::chrono::milliseconds(1));
  // Make sure that time is past that tick.
  ::usleep(10000);
  // Schedule the timeout for the +255 tick, due to sleep above it will overlap
  // with what would be ran on the next timeoutExpired of the timer.
  auto timeout = std::chrono::milliseconds(2555);
  t.scheduleTimeout(&t2, std::chrono::milliseconds(2555));
  auto start = std::chrono::steady_clock::now();
  eventBase.loop();
  ASSERT_EQ(t2.timestamps.size(), 1);
  auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      t2.timestamps[0].getTime() - start);
  EXPECT_GE(elapsedMs.count(), timeout.count());
}

TEST_F(HHWheelTimerTest, Level1) {
  StackWheelTimer t(&eventBase, milliseconds(1));
  TestTimeout tt;
  // Schedule the timeout for the tick in a next epoch.
  t.scheduleTimeout(&tt, std::chrono::milliseconds(500));
  TimePoint start;
  eventBase.loop();
  TimePoint end;
  ASSERT_EQ(tt.timestamps.size(), 1);
  T_CHECK_NOT_EARLY(start, end, milliseconds(500));
}

// Test that we handle negative timeouts properly (i.e. treat them as 0)
TEST_F(HHWheelTimerTest, NegativeTimeout) {
  StackWheelTimer t(&eventBase, milliseconds(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  TestTimeout tt1;
  TestTimeout tt2;
  // Make sure we have event scheduled.
  t.scheduleTimeout(&tt1, std::chrono::milliseconds(1));
  // Schedule another timeout that would appear to be earlier than
  // the already scheduled one.
  t.scheduleTimeout(&tt2, std::chrono::milliseconds(-500000000));
  TimePoint start;
  eventBase.loop();
  TimePoint end;
  ASSERT_EQ(tt2.timestamps.size(), 1);
  T_CHECK_NOT_EARLY(start, end, milliseconds(1));
}

TEST(HHWheelTimerDetailsTest, Divider) {
  auto no_overflow_add = [](uint64_t& base, int offset) -> bool {
    if (offset >= 0 || static_cast<unsigned int>(-offset) < base) {
      base += offset;
      return true;
    }
    return false;
  };

  for (uint64_t denShift = 1; denShift <= 60; denShift++) {
    for (int denOffset = -10; denOffset <= 10; denOffset++) {
      uint64_t den = uint64_t(1) << denShift;

      if (!no_overflow_add(den, denOffset)) {
        continue;
      }

      folly::detail::HHWheelTimerDurationInterval<
          std::chrono::milliseconds>::Divider divider{den};
      for (uint64_t numShift = 3; numShift <= 58; numShift++) {
        for (int numOffset = -10; numOffset <= 10; numOffset++) {
          uint64_t num = (1LLU << numShift);
          if (!no_overflow_add(num, numOffset)) {
            continue;
          }
          int allowedError = 0;
          if (numShift >= 32 || denShift >= 32) {
            allowedError = 1;
          }
          auto expected = num / den;
          auto calc = divider.divide(num);

          // either it is accurate or the result is overstated by allowedError
          EXPECT_TRUE(
              expected == calc ||
              (calc >= expected && calc <= expected + allowedError))
              << "expected=" << expected << " calc=" << calc << " allowedError="
              << allowedError << ": " << num << "/" << den << " ns=" << numShift
              << " + " << numOffset << " ds=" << denShift << " + " << denOffset;
        }
      }
    }
  }

  for (uint64_t den = 1; den <= 10000; den++) {
    folly::detail::HHWheelTimerDurationInterval<
        std::chrono::milliseconds>::Divider divider{den};
    for (uint64_t num = 0; num <= 10000; num++) {
      EXPECT_EQ(num / den, divider.divide(num)) << num << "/" << den;
    }
  }
}
