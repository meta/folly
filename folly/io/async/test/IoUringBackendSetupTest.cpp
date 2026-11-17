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
#include <vector>

#include <folly/io/async/EventBase.h>
#include <folly/io/async/IoUringBackend.h>
#include <folly/io/async/IoUringBufferPoolSharing.h>
#include <folly/portability/GTest.h>

namespace folly::test {

namespace {

// An EventBase backed by IoUringBackend, looping on its own thread.
// ScopedEventBaseThread cannot be used here: it offers no constructor taking
// EventBase::Options, so it cannot install the io_uring backend factory.
class IoUringEventBaseThread {
 public:
  IoUringEventBaseThread()
      : evb_(
            EventBase::Options{}.setBackendFactory(
                []() -> std::unique_ptr<EventBaseBackendBase> {
                  // Buffer geometry must be set explicitly: the defaults are
                  // -1, and createZcBufferPool() casts them to size_t. No
                  // interface is configured, so registering the pool fails
                  // against the kernel, which is the failure this fixture
                  // exercises.
                  IoUringBackend::Options options;
                  options.setZeroCopyRxNumBuffers(64)
                      .setZeroCopyRxRefillEntries(32)
                      .setZeroCopyRxBufferSizeHint(4096);
                  return std::make_unique<IoUringBackend>(std::move(options));
                })),
        thread_([this] { evb_.loopForever(); }) {
    evb_.waitUntilRunning();
  }

  ~IoUringEventBaseThread() {
    evb_.terminateLoopSoon();
    thread_.join();
  }

  EventBase* getEventBase() { return &evb_; }

 private:
  EventBase evb_;
  std::thread thread_;
};

} // namespace

class IoUringBufferPoolSharingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!IoUringBackend::isAvailable()) {
      GTEST_SKIP() << "io_uring not available";
    }
  }
};

// These backends have no zero-copy RX options, so registering the pool fails.
// IoUringZeroCopyBufferPool reports that by throwing on the EventBase thread;
// setup must surface it as a return value and leave the worker usable rather
// than letting the exception terminate it.
TEST_F(IoUringBufferPoolSharingTest, ReportsFailureWhenPoolSetupThrows) {
  IoUringEventBaseThread eventBaseThread;
  std::vector<EventBase*> eventBases{eventBaseThread.getEventBase()};

  EXPECT_FALSE(setupIoUringBufferPoolSharing(eventBases, 1));

  bool followUpRan = false;
  eventBaseThread.getEventBase()->runInEventBaseThreadAndWait([&] {
    followUpRan = true;
  });
  EXPECT_TRUE(followUpRan);
}

// Repeated failed attempts must not leave an EventBase wedged or accumulate
// partial pool state that changes the outcome of a later attempt.
TEST_F(IoUringBufferPoolSharingTest, RecoversAfterRepeatedSetupFailures) {
  IoUringEventBaseThread eventBaseThread;
  std::vector<EventBase*> eventBases{eventBaseThread.getEventBase()};

  for (size_t i = 0; i < 3; ++i) {
    EXPECT_FALSE(setupIoUringBufferPoolSharing(eventBases, 1));
  }

  size_t invocationCount = 0;
  eventBaseThread.getEventBase()->runInEventBaseThreadAndWait([&] {
    ++invocationCount;
  });
  EXPECT_EQ(invocationCount, 1);
}

// The sharing loop runs after the owner loop, so a create failure must be
// reported before any export/import is attempted, with every worker intact.
TEST_F(IoUringBufferPoolSharingTest, ReportsFailureWithFewerQueuesThanThreads) {
  IoUringEventBaseThread owner;
  IoUringEventBaseThread importer;
  std::vector<EventBase*> eventBases{
      owner.getEventBase(), importer.getEventBase()};

  EXPECT_FALSE(setupIoUringBufferPoolSharing(eventBases, 1));

  for (auto* evb : eventBases) {
    bool followUpRan = false;
    evb->runInEventBaseThreadAndWait([&] { followUpRan = true; });
    EXPECT_TRUE(followUpRan);
  }
}

// These three cases are rejected before any real io_uring work is attempted,
// so they don't need IoUringBackend::isAvailable() or an io_uring-backed
// EventBase at all.
TEST(
    IoUringBufferPoolSharingArgValidationTest,
    FailsGracefullyWithNoEventBases) {
  std::vector<EventBase*> eventBases;
  EXPECT_FALSE(setupIoUringBufferPoolSharing(eventBases, 1));
}

TEST(
    IoUringBufferPoolSharingArgValidationTest,
    FailsGracefullyWithZeroHwQueues) {
  EventBase evb;
  std::vector<EventBase*> eventBases{&evb};
  EXPECT_FALSE(setupIoUringBufferPoolSharing(eventBases, 0));
}

TEST(
    IoUringBufferPoolSharingArgValidationTest,
    FailsGracefullyWithoutIoUringBackend) {
  EventBase evb; // default backend, not IoUringBackend
  std::vector<EventBase*> eventBases{&evb};
  EXPECT_FALSE(setupIoUringBufferPoolSharing(eventBases, 1));
}

class IoUringBackendSetupTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!IoUringBackend::isAvailable()) {
      GTEST_SKIP() << "io_uring not available";
    }
  }
};

TEST_F(IoUringBackendSetupTest, SetupPollNoGroup) {
  IoUringBackend::Options options;
  options.setFlags(IoUringBackend::Options::Flags::POLL_SQ);

  IoUringBackend io(std::move(options));

  EXPECT_TRUE(io.params().flags & IORING_SETUP_CQSIZE);
  EXPECT_TRUE(io.params().flags & IORING_SETUP_SQPOLL);
  EXPECT_FALSE(io.params().flags & IORING_SETUP_SQ_AFF);
  EXPECT_FALSE(io.params().flags & IORING_SETUP_ATTACH_WQ);
}

TEST_F(IoUringBackendSetupTest, SetupPollWithGroup) {
  auto makeOptions = []() {
    IoUringBackend::Options options;
    options.setFlags(IoUringBackend::Options::Flags::POLL_SQ)
        .setSQGroupName("test group")
        .setSQGroupNumThreads(1);
    return options;
  };

  IoUringBackend io1(makeOptions());
  IoUringBackend io2(makeOptions());

  // We set up one thread for the group, so the first call should be normal...
  EXPECT_TRUE(io1.params().flags & IORING_SETUP_SQPOLL);
  EXPECT_FALSE(io1.params().flags & IORING_SETUP_SQ_AFF);
  EXPECT_FALSE(io1.params().flags & IORING_SETUP_ATTACH_WQ);

  // second call should have attached to existing fd 1.
  EXPECT_TRUE(io2.params().flags & IORING_SETUP_SQPOLL);
  EXPECT_FALSE(io2.params().flags & IORING_SETUP_SQ_AFF);
  EXPECT_TRUE(io2.params().flags & IORING_SETUP_ATTACH_WQ);
  EXPECT_EQ(io2.params().wq_fd, io1.ioRingPtr()->ring_fd);
}

TEST_F(IoUringBackendSetupTest, SetupPollWithGroupAndCpu) {
  auto makeOptions = []() {
    IoUringBackend::Options options;
    options.setFlags(IoUringBackend::Options::Flags::POLL_SQ)
        .setSQGroupName("test group")
        .setSQGroupNumThreads(2)
        .setSQCpu(1)
        .setSQCpu(0);
    return options;
  };

  IoUringBackend io1(makeOptions());
  IoUringBackend io2(makeOptions());
  IoUringBackend io3(makeOptions());

  // The first call should create a thread with CPU affinity set.
  EXPECT_TRUE(io1.params().flags & IORING_SETUP_SQPOLL);
  EXPECT_TRUE(io1.params().flags & IORING_SETUP_SQ_AFF);
  EXPECT_FALSE(io1.params().flags & IORING_SETUP_ATTACH_WQ);
  // We don't know which CPU code will choose, but it better be one or the
  // other.
  EXPECT_TRUE(
      io1.params().sq_thread_cpu == 1 || io1.params().sq_thread_cpu == 0);

  // We set two threads, so second call should create a thread with other CPU.
  EXPECT_TRUE(io2.params().flags & IORING_SETUP_CQSIZE);
  EXPECT_TRUE(io2.params().flags & IORING_SETUP_SQPOLL);
  EXPECT_TRUE(io2.params().flags & IORING_SETUP_SQ_AFF);
  EXPECT_FALSE(io2.params().flags & IORING_SETUP_ATTACH_WQ);
  // This one better choose the other CPU.
  EXPECT_TRUE(
      io2.params().sq_thread_cpu == 1 || io2.params().sq_thread_cpu == 0);
  EXPECT_NE(io1.params().sq_thread_cpu, io2.params().sq_thread_cpu);

  // And the third thread should have attached to an existing SQ and not
  // specified an affinity.
  EXPECT_TRUE(io3.params().flags & IORING_SETUP_SQPOLL);
  EXPECT_FALSE(io3.params().flags & IORING_SETUP_SQ_AFF);
  EXPECT_TRUE(io3.params().flags & IORING_SETUP_ATTACH_WQ);
}

TEST_F(IoUringBackendSetupTest, SetupPollGroupReusesSubgroupsAfterRemoval) {
  auto makeOptions = []() {
    IoUringBackend::Options options;
    options.setFlags(IoUringBackend::Options::Flags::POLL_SQ)
        .setSQGroupName("test subgroup reuse")
        .setSQGroupNumThreads(2);
    return options;
  };

  auto first = std::make_unique<IoUringBackend>(makeOptions());
  IoUringBackend second(makeOptions());
  auto third = std::make_unique<IoUringBackend>(makeOptions());
  ASSERT_EQ(first->ioRingPtr()->ring_fd, third->params().wq_fd);

  first.reset();
  {
    IoUringBackend replacement(makeOptions());
    EXPECT_TRUE(replacement.params().flags & IORING_SETUP_ATTACH_WQ);
    EXPECT_EQ(third->ioRingPtr()->ring_fd, replacement.params().wq_fd);
  }

  third.reset();
  IoUringBackend fresh(makeOptions());
  EXPECT_FALSE(fresh.params().flags & IORING_SETUP_ATTACH_WQ);
}

TEST_F(IoUringBackendSetupTest, SetupPollGroupResetsAfterLastRemoval) {
  auto makeOptions = [](uint32_t numThreads) {
    IoUringBackend::Options options;
    options.setFlags(IoUringBackend::Options::Flags::POLL_SQ)
        .setSQGroupName("test group reuse")
        .setSQGroupNumThreads(numThreads);
    return options;
  };
  {
    IoUringBackend first(makeOptions(2));
    IoUringBackend second(makeOptions(2));
    EXPECT_FALSE(second.params().flags & IORING_SETUP_ATTACH_WQ);
  }

  IoUringBackend first(makeOptions(1));
  IoUringBackend second(makeOptions(1));
  EXPECT_TRUE(second.params().flags & IORING_SETUP_ATTACH_WQ);
  EXPECT_EQ(first.ioRingPtr()->ring_fd, second.params().wq_fd);
}

TEST_F(IoUringBackendSetupTest, DisableIoWaitAppliesNoIoWait) {
  IoUringBackend::Options options;
  options.setDisableIoWait(true);
  IoUringBackend io(std::move(options));

  io.eb_event_base_loop(EVLOOP_NONBLOCK);

  io_uring* ring = io.ioRingPtr();
  if (!(ring->features & IORING_FEAT_NO_IOWAIT)) {
    GTEST_SKIP() << "kernel lacks IORING_FEAT_NO_IOWAIT";
  }
  EXPECT_TRUE(ring->int_flags & IORING_ENTER_NO_IOWAIT);
}

TEST_F(IoUringBackendSetupTest, DefaultDoesNotDisableIoWait) {
  IoUringBackend::Options options;
  IoUringBackend io(std::move(options));
  io.eb_event_base_loop(EVLOOP_NONBLOCK);

  EXPECT_FALSE(io.ioRingPtr()->int_flags & IORING_ENTER_NO_IOWAIT);
}

} // namespace folly::test
