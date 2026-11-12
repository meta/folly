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

#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <array>
#include <numeric>

#include <folly/FileUtil.h>
#include <folly/Function.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <folly/init/Init.h>
#include <folly/io/async/AsyncUDPServerSocket.h>
#include <folly/io/async/EventHandler.h>
#include <folly/io/async/IoUringBackend.h>
#include <folly/io/async/IoUringOptions.h>
#include <folly/io/async/IoUringProvidedBufferRing.h>
#include <folly/io/async/test/AsyncSignalHandlerTestLib.h>
#include <folly/io/async/test/EventBaseTestLib.h>
#include <folly/io/async/test/IoTestTempFileUtil.h>
#include <folly/portability/GTest.h>
#include <folly/portability/SysResource.h>

#ifndef RESOLVE_IN_ROOT
#define RESOLVE_IN_ROOT 0x10
#endif

// IoUringBackend specific tests
namespace {
class AlignedBuf {
 public:
  static constexpr size_t kAlign = 4096;
  AlignedBuf() = delete;

  AlignedBuf(size_t count, char ch) : size_(count) {
    ::posix_memalign(&data_, kAlign, size_);
    CHECK(!!data_);
    ::memset(data_, ch, count);
  }

  AlignedBuf(const AlignedBuf& buf) : size_(buf.size_) {
    if (size_) {
      ::posix_memalign(&data_, kAlign, size_);
      CHECK(!!data_);
      ::memcpy(data_, buf.data_, size_);
    }
  }

  ~AlignedBuf() {
    if (data_) {
      ::free(data_);
    }
  }

  AlignedBuf& operator=(const AlignedBuf& buf) {
    if (data_) {
      ::free(data_);
    }

    size_ = buf.size_;
    if (size_) {
      ::posix_memalign(&data_, kAlign, size_);
      CHECK(!!data_);
      ::memcpy(data_, buf.data_, size_);
    }

    return *this;
  }

  bool operator==(const AlignedBuf& buf) const {
    if (size_ != buf.size_) {
      return false;
    }

    if (size_ == 0) {
      return true;
    }

    return (0 == ::memcmp(data_, buf.data_, size_));
  }

  bool operator!=(const AlignedBuf& buf) const { return !(*this == buf); }

  void* data() const { return data_; }

  size_t size() const { return size_; }

 private:
  void* data_{nullptr};
  size_t size_{0};
};
class EventFD : public folly::EventHandler {
 public:
  EventFD(
      bool valid,
      uint64_t num,
      uint64_t& total,
      bool persist,
      folly::EventBase* eventBase)
      : EventFD(total, valid ? createFd(num) : -1, persist, eventBase) {}
  ~EventFD() override {
    unregisterHandler();

    if (fd_ >= 0) {
      folly::fileops::close(fd_);
      fd_ = -1;
    }
  }

  // from folly::EventHandler
  void handlerReady(uint16_t /*events*/) noexcept override {
    // we do not read to leave the fd signalled
    ++num_;
    if (total_ > 0) {
      --total_;
    }

    if (total_ > 0) {
      if (!persist_) {
        registerHandler(folly::EventHandler::READ);
      }
    } else {
      if (persist_) {
        unregisterHandler();
      }
    }
  }

  uint64_t getNum() const { return num_; }

 private:
  static int createFd(uint64_t num) {
    // we want it a semaphore
    // and blocking for the async reads
    int fd = ::eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE);
    CHECK_GT(fd, 0);
    CHECK_EQ(folly::writeNoInt(fd, &num, sizeof(num)), sizeof(num));
    return fd;
  }

  EventFD(uint64_t& total, int fd, bool persist, folly::EventBase* eventBase)
      : EventHandler(eventBase, folly::NetworkSocket::fromFd(fd)),
        total_(total),
        fd_(fd),
        persist_(persist) {
    if (persist_) {
      registerHandler(folly::EventHandler::READ | folly::EventHandler::PERSIST);
    } else {
      registerHandler(folly::EventHandler::READ);
    }
  }

  uint64_t num_{0};
  uint64_t& total_;
  int fd_{-1};
  bool persist_;
};

class TimerFD : public folly::EventHandler {
 public:
  explicit TimerFD(folly::EventBase* eventBase)
      : TimerFD(eventBase, createFd()) {}

  ~TimerFD() override {
    unregisterHandler();
    folly::fileops::close(fd_);
  }

  void arm(std::chrono::microseconds delay) {
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(delay);
    itimerspec timer{};
    timer.it_value.tv_sec = seconds.count();
    timer.it_value.tv_nsec =
        std::chrono::duration_cast<std::chrono::nanoseconds>(delay - seconds)
            .count();
    CHECK_EQ(::timerfd_settime(fd_, 0, &timer, nullptr), 0);
  }

  uint64_t getNum() const { return num_; }

  void handlerReady(uint16_t /*events*/) noexcept override {
    uint64_t expirations = 0;
    CHECK_EQ(
        folly::readNoInt(fd_, &expirations, sizeof(expirations)),
        sizeof(expirations));
    ++num_;
  }

 private:
  static int createFd() {
    const int fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    CHECK_GE(fd, 0);
    return fd;
  }

  TimerFD(folly::EventBase* eventBase, int fd)
      : EventHandler(eventBase, folly::NetworkSocket::fromFd(fd)), fd_(fd) {
    registerHandler(folly::EventHandler::READ);
  }

  uint64_t num_{0};
  int fd_;
};

struct PollLoopStats {
  int preHookCalls{0};
  int postHookCalls{0};
  int lastEventCount{-1};
  int emptyPollCount{0};
  folly::Function<void()> firstPreLoop;
};

folly::EventBaseBackendBase::PollLoopHook makePollLoopHook(
    PollLoopStats& stats) {
  folly::EventBaseBackendBase::PollLoopHook hook;
  hook.preLoopHook = [](void* ctx) {
    auto& hookStats = *static_cast<PollLoopStats*>(ctx);
    if (hookStats.preHookCalls++ == 0) {
      hookStats.firstPreLoop();
    }
  };
  hook.postLoopHook = [](void* ctx, int numEvents) {
    auto& hookStats = *static_cast<PollLoopStats*>(ctx);
    ++hookStats.postHookCalls;
    hookStats.lastEventCount = numEvents;
    if (numEvents == 0) {
      ++hookStats.emptyPollCount;
    }
  };
  hook.hookCtx = &stats;
  return hook;
}

std::unique_ptr<folly::EventBase> getEventBase(folly::IoUringOptions opts) {
  try {
    auto optsPtr = std::make_shared<folly::IoUringOptions>(std::move(opts));
    auto factory = [optsPtr]() mutable {
      return std::make_unique<folly::IoUringBackend>(std::move(*optsPtr));
    };
    return std::make_unique<folly::EventBase>(
        folly::EventBase::Options().setBackendFactory(std::move(factory)));
  } catch (const folly::IoUringBackend::NotAvailable&) {
    return nullptr;
  }
}

std::unique_ptr<folly::EventBase> getEventBase() {
  static constexpr uint32_t kBackendCapacity = 32;
  static constexpr uint32_t kBackendMaxSubmit = 16;
  static constexpr uint32_t kBackendMaxGet = 8;
  folly::IoUringOptions options;
  options.setCapacity(kBackendCapacity)
      .setMaxSubmit(kBackendMaxSubmit)
      .setMaxGet(kBackendMaxGet)
      .setUseRegisteredFds(0);
  return getEventBase(std::move(options));
}

class RingStatsBackend : public folly::IoUringBackend {
 public:
  using IoUringBackend::IoUringBackend;

  void queueCancel() { cancel(&cancelTarget_); }

 private:
  struct CancelTarget : folly::IoSqeBase {
    void processSubmit(io_uring_sqe*) noexcept override {}
    void callback(const io_uring_cqe*) noexcept override {}
    void callbackCancelled(const io_uring_cqe*) noexcept override {}
  } cancelTarget_;
};

class IoUringRingStatsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    SKIP_IF(!folly::IoUringBackend::isAvailable()) << "Backend not available";
  }

  static folly::IoUringOptions options(uint32_t capacity = 32) {
    folly::IoUringOptions opts;
    opts.setCapacity(capacity)
        .setSqeSize(8)
        .setMaxSubmit(8)
        .setMaxGet(2)
        .setUseRegisteredFds(0);
    return opts;
  }
};

TEST_F(IoUringRingStatsTest, GetStatsReturnsAndResetsRecordedValues) {
  RingStatsBackend backend(options());
  backend.submitOutstanding();
  backend.getRingStats();

  for (int i = 0; i < 3; ++i) {
    backend.queueCancel();
  }
  const auto first = backend.getRingStats();
  EXPECT_EQ(first.sqPeak, 0);

  backend.queueCancel();
  backend.queueCancel();
  backend.submitOutstanding();
  backend.processCompleted();
  const auto second = backend.getStats().ring;
  EXPECT_EQ(second.sqPeak, 5);
  EXPECT_EQ(second.cqPeak, 5);

  const auto reset = backend.getRingStats();
  EXPECT_EQ(reset.sqPeak, 0);
  EXPECT_EQ(reset.cqPeak, 0);
}

TEST_F(IoUringRingStatsTest, CountsInitialSqFullAndForcedSubmission) {
  RingStatsBackend backend(options());
  backend.submitOutstanding();
  backend.getRingStats();
  const auto capacity = backend.params().sq_entries;
  for (uint32_t i = 0; i <= capacity; ++i) {
    backend.queueCancel();
  }
  backend.submitOutstanding();
  backend.processCompleted();
  const auto stats = backend.getRingStats();
  EXPECT_EQ(stats.sqPeak, capacity);
  EXPECT_EQ(stats.sqFullCount, 1);
  const auto reset = backend.getRingStats();
  EXPECT_EQ(reset.sqFullCount, 0);
}

TEST_F(IoUringRingStatsTest, FullSqWithoutFailedAcquisition) {
  RingStatsBackend backend(options());
  backend.submitOutstanding();
  backend.getRingStats();
  const auto capacity = backend.params().sq_entries;
  for (uint32_t i = 0; i < capacity; ++i) {
    backend.queueCancel();
  }
  backend.submitOutstanding();
  backend.processCompleted();
  const auto stats = backend.getRingStats();
  EXPECT_EQ(stats.sqPeak, capacity);
  EXPECT_EQ(stats.sqFullCount, 0);
}

TEST_F(IoUringRingStatsTest, CqPeakPrecedesReapLimitAndDeferredTaskrun) {
  struct Nop : folly::IoSqeBase {
    void processSubmit(io_uring_sqe* sqe) noexcept override {
      ::io_uring_prep_nop(sqe);
    }
    void callback(const io_uring_cqe* cqe) noexcept override {
      EXPECT_EQ(cqe->res, 0);
    }
    void callbackCancelled(const io_uring_cqe*) noexcept override {}
  };

  for (bool deferTaskrun : {false, true}) {
    SCOPED_TRACE(deferTaskrun);
    auto opts = options();
    opts.setDeferTaskRun(deferTaskrun).setMaxSubmit(4);
    RingStatsBackend backend(std::move(opts));
    backend.submitOutstanding();
    backend.getRingStats();
    std::array<Nop, 6> nops;
    for (auto& nop : nops) {
      backend.submitNextLoop(nop);
    }
    backend.submitOutstanding();
    backend.loopPoll();
    const auto first = backend.getRingStats();
    EXPECT_EQ(first.sqPeak, 4);
    EXPECT_EQ(first.cqPeak, 6);

    backend.processCompleted();
    const auto second = backend.getRingStats();
    EXPECT_EQ(second.cqPeak, 4);

    backend.loopPoll();
    const auto third = backend.getRingStats();
    EXPECT_EQ(third.cqPeak, 2);
    EXPECT_EQ(backend.getRingStats().cqPeak, 0);
  }
}

TEST_F(IoUringRingStatsTest, CountsCqOverflowObservations) {
  RingStatsBackend backend(options(8));
  SKIP_IF(!(backend.params().features & IORING_FEAT_NODROP))
      << "Kernel cannot retain overflow completions";
  backend.submitOutstanding();
  backend.getRingStats();
  const auto capacity = backend.params().cq_entries;
  for (uint32_t i = 0; i < capacity; ++i) {
    backend.queueCancel();
  }
  backend.submitOutstanding();
  backend.queueCancel();
  backend.submitOutstanding();
  ASSERT_TRUE(::io_uring_cq_has_overflow(backend.ioRingPtr()));
  backend.submitOutstanding();
  backend.submitOutstanding();
  EXPECT_EQ(backend.getRingStats().cqOverflowCount, 2);
  backend.submitOutstanding();
  EXPECT_EQ(backend.getRingStats().cqOverflowCount, 1);
  backend.loopPoll();
  EXPECT_FALSE(::io_uring_cq_has_overflow(backend.ioRingPtr()));
  const auto stats = backend.getRingStats();
  EXPECT_EQ(stats.cqPeak, capacity);
  EXPECT_GT(stats.cqOverflowCount, 0);
  EXPECT_EQ(backend.getRingStats().cqOverflowCount, 0);
}

void testEventFD(bool overflow, bool persist) {
  static constexpr uint32_t kBackendCapacity = 64;
  static constexpr uint32_t kBackendMaxSubmit = 8;
  // for overflow == true  we use a greater than kBackendCapacity number of
  // EventFD instances and lower when overflow == false
  uint32_t kNumEventFds = overflow ? 2048 : 32;
  static constexpr size_t kEventFdCount = 2;
  auto total = kNumEventFds * kEventFdCount + kEventFdCount / 2;

  folly::IoUringOptions options;
  options.setCapacity(kBackendCapacity)
      .setMaxSubmit(kBackendMaxSubmit)
      .setMaxGet(kNumEventFds * 2);
  auto evbPtr = getEventBase(std::move(options));
  SKIP_IF(!evbPtr) << "Backend not available";

  std::vector<std::unique_ptr<EventFD>> eventsVec;
  eventsVec.reserve(kNumEventFds);
  for (size_t i = 0; i < kNumEventFds; i++) {
    auto ev = std::make_unique<EventFD>(
        true, 2 * kEventFdCount, total, persist, evbPtr.get());

    eventsVec.emplace_back(std::move(ev));
  }

  evbPtr->loop();

  for (size_t i = 0; i < kNumEventFds; i++) {
    EXPECT_GE(eventsVec[i]->getNum(), kEventFdCount)
        << " persist=" << persist << " overflow=" << overflow
        << " num= " << eventsVec[i]->getNum()
        << " kEventFdCount=" << kEventFdCount << " i=" << i;
  }
}

void testInvalidFd(size_t numTotal, size_t numValid, size_t numInvalid) {
  static constexpr uint32_t kBackendCapacity = 32;
  static constexpr uint32_t kBackendMaxSubmit = 16;

  auto total = numTotal;

  folly::IoUringOptions options;
  options.setCapacity(kBackendCapacity).setMaxSubmit(kBackendMaxSubmit);
  auto evbPtr = getEventBase(std::move(options));
  SKIP_IF(!evbPtr) << "Backend not available";

  std::vector<std::unique_ptr<EventFD>> eventsVec;
  eventsVec.reserve(numTotal);

  for (size_t i = 0; i < numTotal; i++) {
    bool valid = (i % (numValid + numInvalid)) < numValid;
    eventsVec.emplace_back(
        std::make_unique<EventFD>(
            valid, 1, total, false /*persist*/, evbPtr.get()));
  }

  evbPtr->loop();

  for (size_t i = 0; i < numTotal; i++) {
    CHECK_GE(eventsVec[i]->getNum(), 1);
  }
}

} // namespace

TEST(IoUringBackend, FailCreateNoRetry) {
  bool bSuccess = true;
  try {
    folly::IoUringOptions options;
    options.setCapacity(256 * 1024);
    options.setMinCapacity(0);
    folly::IoUringBackend backend(std::move(options));
  } catch (const folly::IoUringBackend::NotAvailable&) {
    bSuccess = false;
  }
  CHECK(!bSuccess);
}

TEST(IoUringBackend, SuccessCreateRetry) {
  bool bSuccess = true;
  try {
    folly::IoUringOptions options;
    options.setCapacity(256 * 1024);
    options.setMinCapacity(16);
    options.setMaxSubmit(8);
    folly::IoUringBackend backend(std::move(options));
  } catch (const folly::IoUringBackend::NotAvailable&) {
    bSuccess = false;
  }
  CHECK(bSuccess);
}

// Unreliable: whether a zero RLIMIT_MEMLOCK makes ring setup fail with ENOMEM
// depends on the environment, so this test skips when setup succeeds anyway.
// * Capabilities: the kernel does not charge rings created with CAP_IPC_LOCK
//   in the thread's effective set, as is typical when running as root.
// * User: charges accrue per user, so other processes running as the same
//   user can exhaust the budget and make even the unrestricted setup below
//   fail.
// * Kernel version: which io_uring memory is charged against RLIMIT_MEMLOCK,
//   rather than against the memory cgroup or nothing, has varied.
TEST(IoUringBackend, FailCreateOutOfMemory) {
  auto makeOptions = [] {
    folly::IoUringOptions options;
    options.setCapacity(32);
    options.setMaxSubmit(16);
    return options;
  };
  try {
    folly::IoUringBackend backend(makeOptions());
  } catch (const folly::IoUringBackend::NotAvailable&) {
    GTEST_SKIP() << "IoUringBackend not available";
  }

  struct rlimit original{};
  PCHECK(::getrlimit(RLIMIT_MEMLOCK, &original) == 0);
  struct rlimit exhausted = original;
  exhausted.rlim_cur = 0;
  PCHECK(::setrlimit(RLIMIT_MEMLOCK, &exhausted) == 0);
  SCOPE_EXIT {
    PCHECK(::setrlimit(RLIMIT_MEMLOCK, &original) == 0);
  };

  try {
    folly::IoUringBackend backend(makeOptions());
  } catch (const folly::IoUringBackend::OutOfMemory&) {
    return;
  }
  GTEST_SKIP() << "io_uring memory is not charged against RLIMIT_MEMLOCK";
}

TEST(IoUringBackend, RequestBatchTimeoutStartsAfterFirstCompletion) {
  PollLoopStats stats;
  folly::IoUringOptions options;
  options.setBatchSize(4)
      .setTimeout(std::chrono::milliseconds(5))
      .setBatchTimeoutAfterFirstCompletion(true)
      .setUseRegisteredFds(0);
  auto evbPtr = getEventBase(std::move(options));
  SKIP_IF(!evbPtr) << "Backend not available";

  evbPtr->loopOnce(EVLOOP_NONBLOCK);
  TimerFD timer(evbPtr.get());
  evbPtr->loopOnce(EVLOOP_NONBLOCK);

  stats.firstPreLoop = [&] { timer.arm(std::chrono::milliseconds(30)); };
  evbPtr->getBackend()->setPollLoopHook(makePollLoopHook(stats));
  evbPtr->loopOnce();

  EXPECT_EQ(timer.getNum(), 1);
  EXPECT_EQ(stats.preHookCalls, 1);
  EXPECT_EQ(stats.postHookCalls, 1);
  EXPECT_GE(stats.lastEventCount, 1);
}

TEST(IoUringBackend, RequestBatchTimeoutBoundsWholeWaitByDefault) {
  PollLoopStats stats;
  folly::IoUringOptions options;
  options.setBatchSize(4)
      .setTimeout(std::chrono::milliseconds(5))
      .setUseRegisteredFds(0);
  auto evbPtr = getEventBase(std::move(options));
  SKIP_IF(!evbPtr) << "Backend not available";

  evbPtr->loopOnce(EVLOOP_NONBLOCK);
  TimerFD timer(evbPtr.get());
  evbPtr->loopOnce(EVLOOP_NONBLOCK);

  // Without the opt-in, each wait gives up after the timeout even when idle,
  // so the loop polls empty several times before the 30ms timer fires.
  stats.firstPreLoop = [&] { timer.arm(std::chrono::milliseconds(30)); };
  evbPtr->getBackend()->setPollLoopHook(makePollLoopHook(stats));
  evbPtr->loopOnce();

  EXPECT_EQ(timer.getNum(), 1);
  EXPECT_GE(stats.emptyPollCount, 2);
}

TEST(IoUringBackend, RequestBatchCompletesAtBatchSize) {
  PollLoopStats stats;
  folly::IoUringOptions options;
  options.setBatchSize(4)
      .setTimeout(std::chrono::milliseconds(100))
      .setBatchTimeoutAfterFirstCompletion(true)
      .setUseRegisteredFds(0);
  auto evbPtr = getEventBase(std::move(options));
  SKIP_IF(!evbPtr) << "Backend not available";

  std::vector<std::unique_ptr<TimerFD>> timers;
  timers.reserve(4);
  for (size_t i = 0; i < 4; ++i) {
    timers.push_back(std::make_unique<TimerFD>(evbPtr.get()));
  }
  evbPtr->loopOnce(EVLOOP_NONBLOCK);

  stats.firstPreLoop = [&] {
    timers.front()->arm(std::chrono::milliseconds(10));
    for (size_t i = 1; i < timers.size(); ++i) {
      timers[i]->arm(std::chrono::milliseconds(30));
    }
  };
  evbPtr->getBackend()->setPollLoopHook(makePollLoopHook(stats));
  evbPtr->loopOnce();

  for (const auto& timer : timers) {
    EXPECT_EQ(timer->getNum(), 1);
  }
  EXPECT_EQ(stats.preHookCalls, 1);
  EXPECT_EQ(stats.postHookCalls, 1);
  EXPECT_EQ(stats.lastEventCount, 4);
}

TEST(IoUringBackend, OpenAt) {
  auto evbPtr = getEventBase();
  SKIP_IF(!evbPtr) << "Backend not available";

  auto* backendPtr = dynamic_cast<folly::IoUringBackend*>(evbPtr->getBackend());
  CHECK(!!backendPtr);

  auto dirPath = folly::fs::temp_directory_path();
  auto path = folly::fs::unique_path();
  auto filePath = dirPath / path;

  int dfd = folly::fileops::open(dirPath.c_str(), O_DIRECTORY | O_RDONLY, 0666);
  CHECK_GE(dfd, 0);

  SCOPE_EXIT {
    folly::fileops::close(dfd);
    ::unlink(filePath.c_str());
  };

  folly::IoUringBackend::FileOpCallback openCb = [&](int res) {
    evbPtr->terminateLoopSoon();
    CHECK_GE(res, 0);
    CHECK_EQ(0, folly::fileops::close(res));
  };

  backendPtr->queueOpenat(
      dfd, path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666, std::move(openCb));

  evbPtr->loopForever();
}

TEST(IoUringBackend, OpenAtAbsolutePath) {
  auto evbPtr = getEventBase();
  SKIP_IF(!evbPtr) << "Backend not available";

  auto* backendPtr = dynamic_cast<folly::IoUringBackend*>(evbPtr->getBackend());
  CHECK(!!backendPtr);

  auto dirPath = folly::fs::temp_directory_path();
  auto path = folly::fs::unique_path();
  auto filePath = dirPath / path;

  SCOPE_EXIT {
    ::unlink(filePath.c_str());
  };

  folly::IoUringBackend::FileOpCallback openCb = [&](int res) {
    evbPtr->terminateLoopSoon();
    CHECK_GE(res, 0);
    CHECK_EQ(0, folly::fileops::close(res));
  };

  backendPtr->queueOpenat(
      -1, filePath.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666, std::move(openCb));

  evbPtr->loopForever();
}

TEST(IoUringBackend, Rename) {
  auto evbPtr = getEventBase();
  SKIP_IF(!evbPtr) << "Backend not available";

  auto* backendPtr = dynamic_cast<folly::IoUringBackend*>(evbPtr->getBackend());
  CHECK(!!backendPtr);

  auto dirPath = folly::fs::temp_directory_path();
  auto oldName = folly::fs::unique_path();
  auto newName = folly::fs::unique_path();
  auto oldPath = dirPath / oldName;
  auto newPath = dirPath / newName;

  int fd = folly::fileops::open(oldPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC);
  CHECK_GE(fd, 0);

  SCOPE_EXIT {
    folly::fileops::close(fd);
    ::unlink(oldPath.c_str());
    ::unlink(newPath.c_str());
  };

  folly::IoUringBackend::FileOpCallback renameCb = [&](int res) {
    evbPtr->terminateLoopSoon();
    CHECK_GE(res, 0);
    EXPECT_TRUE(folly::fs::exists(newPath));
    EXPECT_FALSE(folly::fs::exists(oldPath));
  };

  backendPtr->queueRename(
      oldPath.c_str(), newPath.c_str(), std::move(renameCb));

  evbPtr->loopForever();
}

TEST(IoUringBackend, RenameDstExists) {
  auto evbPtr = getEventBase();
  SKIP_IF(!evbPtr) << "Backend not available";

  auto* backendPtr = dynamic_cast<folly::IoUringBackend*>(evbPtr->getBackend());
  CHECK(!!backendPtr);

  auto dirPath = folly::fs::temp_directory_path();
  auto oldName = folly::fs::unique_path();
  auto newName = folly::fs::unique_path();
  auto oldPath = dirPath / oldName;
  auto newPath = dirPath / newName;

  {
    int oldFd =
        folly::fileops::open(oldPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC);
    CHECK_GE(oldFd, 0);
    folly::fileops::close(oldFd);
  }

  SCOPE_EXIT {
    ::unlink(oldPath.c_str());
  };

  {
    int newFd =
        folly::fileops::open(newPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC);
    CHECK_GE(newFd, 0);
    folly::fileops::close(newFd);
  }

  folly::IoUringBackend::FileOpCallback renameCb = [&](int res) {
    evbPtr->terminateLoopSoon();
    CHECK_GE(res, 0);
    EXPECT_TRUE(folly::fs::exists(newPath));
    EXPECT_FALSE(folly::fs::exists(oldPath));
  };

  backendPtr->queueRename(
      oldPath.c_str(), newPath.c_str(), std::move(renameCb));

  evbPtr->loopForever();
}

TEST(IoUringBackend, RenameSrcDoesntExist) {
  auto evbPtr = getEventBase();
  SKIP_IF(!evbPtr) << "Backend not available";

  auto* backendPtr = dynamic_cast<folly::IoUringBackend*>(evbPtr->getBackend());
  CHECK(!!backendPtr);

  auto dirPath = folly::fs::temp_directory_path();
  auto oldName = folly::fs::unique_path();
  auto newName = folly::fs::unique_path();
  auto oldPath = dirPath / oldName;
  auto newPath = dirPath / newName;

  folly::IoUringBackend::FileOpCallback renameCb = [&](int res) {
    evbPtr->terminateLoopSoon();
    CHECK_LT(res, 0);
    EXPECT_FALSE(folly::fs::exists(newPath));
    EXPECT_FALSE(folly::fs::exists(oldPath));
  };

  backendPtr->queueRename(
      oldPath.c_str(), newPath.c_str(), std::move(renameCb));

  evbPtr->loopForever();
}

TEST(IoUringBackend, Unlink) {
  auto evbPtr = getEventBase();
  SKIP_IF(!evbPtr) << "Backend not available";

  auto* backendPtr = dynamic_cast<folly::IoUringBackend*>(evbPtr->getBackend());
  CHECK(!!backendPtr);

  auto dirPath = folly::fs::temp_directory_path();
  auto fileName = folly::fs::unique_path();
  auto filePath = dirPath / fileName;

  int fd = folly::fileops::open(filePath.c_str(), O_CREAT | O_WRONLY | O_TRUNC);
  CHECK_GE(fd, 0);
  folly::fileops::close(fd);

  SCOPE_EXIT {
    ::unlink(filePath.c_str());
  };

  EXPECT_TRUE(folly::fs::exists(filePath));

  folly::IoUringBackend::FileOpCallback unlinkCb = [&](int res) {
    evbPtr->terminateLoopSoon();
    CHECK_GE(res, 0);
    EXPECT_FALSE(folly::fs::exists(filePath));
  };

  backendPtr->queueUnlink(filePath.c_str(), std::move(unlinkCb));

  evbPtr->loopForever();
}

TEST(IoUringBackend, UnlinkDoesntExist) {
  auto evbPtr = getEventBase();
  SKIP_IF(!evbPtr) << "Backend not available";

  auto* backendPtr = dynamic_cast<folly::IoUringBackend*>(evbPtr->getBackend());
  CHECK(!!backendPtr);

  auto dirPath = folly::fs::temp_directory_path();
  auto fileName = folly::fs::unique_path();
  auto filePath = dirPath / fileName;

  EXPECT_FALSE(folly::fs::exists(filePath));

  folly::IoUringBackend::FileOpCallback unlinkCb = [&](int res) {
    evbPtr->terminateLoopSoon();
    CHECK_LT(res, 0);
    EXPECT_FALSE(folly::fs::exists(filePath));
  };

  backendPtr->queueUnlink(filePath.c_str(), std::move(unlinkCb));

  evbPtr->loopForever();
}

TEST(IoUringBackend, Unlinkat) {
  auto evbPtr = getEventBase();
  SKIP_IF(!evbPtr) << "Backend not available";

  auto* backendPtr = dynamic_cast<folly::IoUringBackend*>(evbPtr->getBackend());
  CHECK(!!backendPtr);

  auto dirPath = folly::fs::temp_directory_path();
  auto fileName = folly::fs::unique_path();
  auto filePath = dirPath / fileName;

  int fd = folly::fileops::open(filePath.c_str(), O_CREAT | O_WRONLY | O_TRUNC);
  CHECK_GE(fd, 0);
  folly::fileops::close(fd);

  int dirfd = folly::fileops::open(dirPath.c_str(), O_DIRECTORY);
  CHECK_GE(dirfd, 0);

  SCOPE_EXIT {
    folly::fileops::close(dirfd);
    ::unlink(filePath.c_str());
  };

  EXPECT_TRUE(folly::fs::exists(filePath));

  folly::IoUringBackend::FileOpCallback unlinkCb = [&](int res) {
    evbPtr->terminateLoopSoon();
    CHECK_GE(res, 0);
    EXPECT_FALSE(folly::fs::exists(filePath));
  };

  backendPtr->queueUnlinkat(dirfd, fileName.c_str(), 0, std::move(unlinkCb));

  evbPtr->loopForever();
}

TEST(IoUringBackend, UnlinkatRemoveDir) {
  auto evbPtr = getEventBase();
  SKIP_IF(!evbPtr) << "Backend not available";

  auto* backendPtr = dynamic_cast<folly::IoUringBackend*>(evbPtr->getBackend());
  CHECK(!!backendPtr);

  auto parentPath = folly::fs::temp_directory_path();
  auto dirName = folly::fs::unique_path();
  auto dirPath = parentPath / dirName;

  folly::fs::create_directory(dirPath);

  SCOPE_EXIT {
    folly::fs::remove(dirPath);
  };

  EXPECT_TRUE(folly::fs::exists(dirPath));
  EXPECT_TRUE(folly::fs::is_directory(dirPath));

  folly::IoUringBackend::FileOpCallback unlinkCb = [&](int res) {
    evbPtr->terminateLoopSoon();
    CHECK_GE(res, 0);
    EXPECT_FALSE(folly::fs::exists(dirPath));
  };

  // AT_REMOVEDIR flag allows removing directories
  backendPtr->queueUnlinkat(
      AT_FDCWD, dirPath.c_str(), AT_REMOVEDIR, std::move(unlinkCb));

  evbPtr->loopForever();
}

TEST(IoUringBackend, Statx) {
  auto evbPtr = getEventBase();
  SKIP_IF(!evbPtr) << "Backend not available";

  auto* backendPtr = dynamic_cast<folly::IoUringBackend*>(evbPtr->getBackend());
  CHECK(!!backendPtr);

  auto dirPath = folly::fs::temp_directory_path();
  auto path = folly::fs::unique_path();
  auto filePath = dirPath / path;

  int dfd = folly::fileops::open(dirPath.c_str(), O_DIRECTORY | O_RDONLY, 0666);
  CHECK_GE(dfd, 0);
  int fd = folly::fileops::open(filePath.c_str(), O_CREAT | O_WRONLY | O_TRUNC);
  CHECK_GE(fd, 0);

  SCOPE_EXIT {
    folly::fileops::close(dfd);
    folly::fileops::close(fd);
    ::unlink(filePath.c_str());
  };

  folly::IoUringBackend::FileOpCallback statxCb = [&](int res) {
    evbPtr->terminateLoopSoon();
    CHECK_GE(res, 0);
  };

  struct ::statx s;
  backendPtr->queueStatx(
      dfd, path.c_str(), 0, STATX_MODE, &s, std::move(statxCb));

  evbPtr->loopForever();
}

TEST(IoUringBackend, StatxAbsolute) {
  auto evbPtr = getEventBase();
  SKIP_IF(!evbPtr) << "Backend not available";

  auto* backendPtr = dynamic_cast<folly::IoUringBackend*>(evbPtr->getBackend());
  CHECK(!!backendPtr);

  auto dirPath = folly::fs::temp_directory_path();
  auto path = folly::fs::unique_path();
  auto filePath = dirPath / path;

  int fd = folly::fileops::open(filePath.c_str(), O_CREAT | O_WRONLY | O_TRUNC);
  CHECK_GE(fd, 0);

  SCOPE_EXIT {
    folly::fileops::close(fd);
    ::unlink(filePath.c_str());
  };

  folly::IoUringBackend::FileOpCallback statxCb = [&](int res) {
    evbPtr->terminateLoopSoon();
    CHECK_GE(res, 0);
  };

  struct ::statx s;
  backendPtr->queueStatx(
      -1, filePath.c_str(), AT_EMPTY_PATH, STATX_MODE, &s, std::move(statxCb));

  evbPtr->loopForever();
}

TEST(IoUringBackend, OpenAt2) {
  auto evbPtr = getEventBase();
  SKIP_IF(!evbPtr) << "Backend not available";

  auto* backendPtr = dynamic_cast<folly::IoUringBackend*>(evbPtr->getBackend());
  CHECK(!!backendPtr);

  auto dirPath = folly::fs::temp_directory_path();
  auto path = folly::fs::unique_path();
  auto filePath = dirPath / path;

  int dfd = folly::fileops::open(dirPath.c_str(), O_DIRECTORY | O_RDONLY, 0666);
  CHECK_GE(dfd, 0);

  SCOPE_EXIT {
    folly::fileops::close(dfd);
    ::unlink(filePath.c_str());
  };

  folly::IoUringBackend::FileOpCallback openCb = [&](int res) {
    evbPtr->terminateLoopSoon();
    CHECK_GE(res, 0);
    CHECK_EQ(0, folly::fileops::close(res));
  };

  struct open_how how = {};
  how.flags = O_RDWR | O_CREAT | O_EXCL;
  how.mode = 0666;
  how.resolve = RESOLVE_IN_ROOT;

  backendPtr->queueOpenat2(dfd, path.c_str(), &how, std::move(openCb));
}

TEST(IoUringBackend, Close) {
  auto evbPtr = getEventBase();
  SKIP_IF(!evbPtr) << "Backend not available";

  auto* backendPtr = dynamic_cast<folly::IoUringBackend*>(evbPtr->getBackend());
  CHECK(!!backendPtr);

  auto path = folly::fs::temp_directory_path();
  path /= folly::fs::unique_path();

  int fd = folly::fileops::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
  CHECK_GE(fd, 0);

  SCOPE_EXIT {
    if (fd >= 0) {
      folly::fileops::close(fd);
    }
    ::unlink(path.c_str());
  };

  folly::IoUringBackend::FileOpCallback closeCb = [&](int res) {
    evbPtr->terminateLoopSoon();
    CHECK_EQ(res, 0);
    fd = -1;
  };

  backendPtr->queueClose(fd, std::move(closeCb));

  evbPtr->loopForever();

  CHECK_EQ(fd, -1);
}

TEST(IoUringBackend, Fallocate) {
  auto evbPtr = getEventBase();
  SKIP_IF(!evbPtr) << "Backend not available";

  auto* backendPtr = dynamic_cast<folly::IoUringBackend*>(evbPtr->getBackend());
  CHECK(!!backendPtr);

  auto path = folly::fs::temp_directory_path();
  path /= folly::fs::unique_path();

  int fd = folly::fileops::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
  CHECK_GE(fd, 0);

  SCOPE_EXIT {
    if (fd >= 0) {
      folly::fileops::close(fd);
      ::unlink(path.c_str());
    }
  };

  folly::IoUringBackend::FileOpCallback fallocateCb = [&](int res) {
    CHECK_EQ(res, 0);
    evbPtr->terminateLoopSoon();
  };

  backendPtr->queueFallocate(fd, 0, 0, 4096, std::move(fallocateCb));

  evbPtr->loopForever();
}

TEST(IoUringBackend, EventFDNooverflownopersist) {
  testEventFD(false, false);
}

TEST(IoUringBackend, EventFDOverflownopersist) {
  testEventFD(true, false);
}

TEST(IoUringBackend, EventFDNooverflowpersist) {
  testEventFD(false, true);
}

TEST(IoUringBackend, EventFDOverflowpersist) {
  testEventFD(true, true);
}

// 9 valid fds followed by an invalid one
TEST(IoUringBackend, InvalidFd91) {
  testInvalidFd(32, 10, 1);
}

// only invalid fds
TEST(IoUringBackend, InvalidFd010) {
  testInvalidFd(32, 0, 10);
}

// equal distribution
TEST(IoUringBackend, InvalidFd55) {
  testInvalidFd(32, 10, 10);
}

TEST(IoUringBackend, RegisteredFds) {
  static constexpr uint32_t kBackendCapacity = 16;
  static constexpr uint32_t kBackendMaxSubmit = 8;
  static constexpr uint32_t kBackendMaxGet = 8;

  std::unique_ptr<folly::IoUringBackend> backendReg;
  std::unique_ptr<folly::IoUringBackend> backendNoReg;

  try {
    folly::IoUringOptions optionsReg;
    optionsReg.setCapacity(kBackendCapacity)
        .setMaxSubmit(kBackendMaxSubmit)
        .setMaxGet(kBackendMaxGet)
        .setUseRegisteredFds(kBackendCapacity);

    backendReg = std::make_unique<folly::IoUringBackend>(std::move(optionsReg));

    folly::IoUringOptions optionsNoReg;
    optionsNoReg.setCapacity(kBackendCapacity)
        .setMaxSubmit(kBackendMaxSubmit)
        .setMaxGet(kBackendMaxGet)
        .setUseRegisteredFds(0);

    backendNoReg =
        std::make_unique<folly::IoUringBackend>(std::move(optionsNoReg));
  } catch (const folly::IoUringBackend::NotAvailable&) {
  }

  SKIP_IF(!backendReg) << "Backend not available";
  SKIP_IF(!backendNoReg) << "Backend not available";

  int eventFd = ::eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE | EFD_NONBLOCK);
  CHECK_GT(eventFd, 0);

  SCOPE_EXIT {
    folly::fileops::close(eventFd);
  };

  // verify for useRegisteredFds = false we get a nullptr
  // IoUringFdRegistrationRecord
  auto* record = backendNoReg->registerFd(eventFd);
  CHECK(!record);

  std::vector<folly::IoUringFdRegistrationRecord*> records;
  // we use kBackendCapacity since the timerFd
  // allocates it only on the first loop
  records.reserve(kBackendCapacity);
  for (size_t i = 0; i < kBackendCapacity; i++) {
    record = backendReg->registerFd(eventFd);
    CHECK(record);
    records.emplace_back(record);
  }

  // try to allocate one more and check if we get a nullptr
  record = backendReg->registerFd(eventFd);
  CHECK(!record);

  // deallocate and allocate again
  for (size_t i = 0; i < records.size(); i++) {
    CHECK(backendReg->unregisterFd(records[i]));
    record = backendReg->registerFd(eventFd);
    CHECK(record);
    records[i] = record;
  }
}

TEST(IoUringBackend, FileReadWrite) {
  static constexpr uint32_t kBackendCapacity = 512;
  static constexpr uint32_t kBackendMaxSubmit = 8;
  static constexpr uint32_t kBackendMaxGet = 8;

  folly::IoUringOptions options;
  options.setCapacity(kBackendCapacity)
      .setMaxSubmit(kBackendMaxSubmit)
      .setMaxGet(kBackendMaxGet)
      .setUseRegisteredFds(0);
  auto evbPtr = getEventBase(std::move(options));
  SKIP_IF(!evbPtr) << "Backend not available";

  static constexpr size_t kNumBlocks = 512;
  static constexpr size_t kBlockSize = 4096;
  static constexpr size_t kFileSize = kNumBlocks * kBlockSize;
  auto tempFile = folly::test::TempFileUtil::getTempFile(kFileSize);

  int fd = folly::fileops::open(tempFile.path().c_str(), O_DIRECT | O_RDWR);
  if (fd == -1) {
    fd = folly::fileops::open(tempFile.path().c_str(), O_RDWR);
  }
  SKIP_IF(fd == -1) << "Tempfile can't be opened: " << folly::errnoStr(errno);
  SCOPE_EXIT {
    folly::fileops::close(fd);
  };

  auto* backendPtr = dynamic_cast<folly::IoUringBackend*>(evbPtr->getBackend());
  CHECK(!!backendPtr);

  size_t num = 0;

  AlignedBuf writeData(kBlockSize, 'A'), readData(kBlockSize, 'Z');
  std::vector<AlignedBuf> writeDataVec(kNumBlocks, writeData),
      readDataVec(kNumBlocks, readData);

  CHECK(readData != writeData);

  for (size_t i = 0; i < kNumBlocks; i++) {
    folly::IoUringBackend::FileOpCallback writeCb = [&, i](int res) {
      CHECK_EQ(res, writeDataVec[i].size());
      folly::IoUringBackend::FileOpCallback readCb = [&, i](int res) {
        CHECK_EQ(res, readDataVec[i].size());
        CHECK(readDataVec[i] == writeDataVec[i]);
        ++num;
      };
      backendPtr->queueRead(
          fd,
          readDataVec[i].data(),
          readDataVec[i].size(),
          i * kBlockSize,
          std::move(readCb));
    };

    backendPtr->queueWrite(
        fd,
        writeDataVec[i].data(),
        writeDataVec[i].size(),
        i * kBlockSize,
        std::move(writeCb));
  }

  evbPtr->loop();

  EXPECT_EQ(num, kNumBlocks);
}

TEST(IoUringBackend, FileReadvWritev) {
  static constexpr uint32_t kBackendCapacity = 512;
  static constexpr uint32_t kBackendMaxSubmit = 8;
  static constexpr uint32_t kBackendMaxGet = 8;

  folly::IoUringOptions options;
  options.setCapacity(kBackendCapacity)
      .setMaxSubmit(kBackendMaxSubmit)
      .setMaxGet(kBackendMaxGet)
      .setUseRegisteredFds(0);
  auto evbPtr = getEventBase(std::move(options));
  SKIP_IF(!evbPtr) << "Backend not available";

  static constexpr size_t kNumBlocks = 512;
  static constexpr size_t kNumIov = 4;
  static constexpr size_t kIovSize = 4096;
  static constexpr size_t kBlockSize = kNumIov * kIovSize;
  static constexpr size_t kFileSize = kNumBlocks * kBlockSize;
  auto tempFile = folly::test::TempFileUtil::getTempFile(kFileSize);

  int fd = folly::fileops::open(tempFile.path().c_str(), O_DIRECT | O_RDWR);
  if (fd == -1) {
    fd = folly::fileops::open(tempFile.path().c_str(), O_RDWR);
  }
  SKIP_IF(fd == -1) << "Tempfile can't be opened: " << folly::errnoStr(errno);
  SCOPE_EXIT {
    folly::fileops::close(fd);
  };

  auto* backendPtr = dynamic_cast<folly::IoUringBackend*>(evbPtr->getBackend());
  CHECK(!!backendPtr);

  size_t num = 0;

  AlignedBuf writeData(kIovSize, 'A'), readData(kIovSize, 'Z');
  std::vector<AlignedBuf> writeDataVec(kNumIov, writeData),
      readDataVec(kNumIov, readData);

  std::vector<std::vector<AlignedBuf>> writeDataVecVec(
      kNumBlocks, writeDataVec),
      readDataVecVec(kNumBlocks, readDataVec);

  CHECK(readDataVec != writeDataVec);

  std::vector<std::vector<struct iovec>> readDataIov, writeDataIov;
  std::vector<size_t> lenVec;

  readDataIov.reserve(kNumBlocks);
  writeDataIov.reserve(kNumBlocks);
  lenVec.reserve(kNumBlocks);

  for (size_t i = 0; i < kNumBlocks; i++) {
    size_t len = 0;
    std::vector<struct iovec> readIov, writeIov;
    readIov.reserve(kNumIov);
    writeIov.reserve(kNumIov);
    for (size_t j = 0; j < kNumIov; j++) {
      struct iovec riov{
          readDataVecVec[i][j].data(), readDataVecVec[i][j].size()};
      readIov.push_back(riov);
      struct iovec wiov{
          writeDataVecVec[i][j].data(), writeDataVecVec[i][j].size()};
      writeIov.push_back(wiov);
      len += riov.iov_len;
    }

    readDataIov.emplace_back(std::move(readIov));
    writeDataIov.emplace_back(std::move(writeIov));
    lenVec.emplace_back(len);
  }

  for (size_t i = 0; i < kNumBlocks; i++) {
    folly::IoUringBackend::FileOpCallback writeCb = [&, i](int res) {
      CHECK_EQ(res, lenVec[i]);
      folly::IoUringBackend::FileOpCallback readCb = [&, i](int res) {
        CHECK_EQ(res, lenVec[i]);
        CHECK(readDataVecVec[i] == writeDataVecVec[i]);
        if (++num == kNumBlocks) {
          evbPtr->terminateLoopSoon();
        }
      };

      backendPtr->queueReadv(
          fd, readDataIov[i], i * kBlockSize, std::move(readCb));
    };

    backendPtr->queueWritev(
        fd, writeDataIov[i], i * kBlockSize, std::move(writeCb));
  }

  evbPtr->loopForever();

  EXPECT_EQ(num, kNumBlocks);
}

TEST(IoUringBackend, FileReadMany) {
  static constexpr uint32_t kBackendCapacity = 256;
  static constexpr uint32_t kBackendMaxSubmit = 32;
  static constexpr uint32_t kBackendMaxGet = 32;

  folly::IoUringOptions options;
  options.setCapacity(kBackendCapacity)
      .setMaxSubmit(kBackendMaxSubmit)
      .setMaxGet(kBackendMaxGet)
      .setUseRegisteredFds(0);
  auto evbPtr = getEventBase(std::move(options));
  SKIP_IF(!evbPtr) << "Backend not available";

  static constexpr size_t kNumBlocks = 8 * 1024;
  static constexpr size_t kBlockSize = 4096;
  static constexpr size_t kBigBlockSize = 2 * 1024 * 1024;

  static constexpr size_t kFileSize = kNumBlocks * kBlockSize;
  auto tempFile = folly::test::TempFileUtil::getTempFile(kFileSize);

  int fd = folly::fileops::open(tempFile.path().c_str(), O_DIRECT | O_RDWR);
  if (fd == -1) {
    fd = folly::fileops::open(tempFile.path().c_str(), O_RDWR);
  }
  SKIP_IF(fd == -1) << "Tempfile can't be opened: " << folly::errnoStr(errno);
  SCOPE_EXIT {
    folly::fileops::close(fd);
  };

  auto* backendPtr = dynamic_cast<folly::IoUringBackend*>(evbPtr->getBackend());
  CHECK(!!backendPtr);

  size_t num = 0;
  AlignedBuf readData(kBlockSize, 'Z');
  std::vector<AlignedBuf> readDataVec(kNumBlocks, readData);

  AlignedBuf bigReadData(kBigBlockSize, 'Z');

  for (size_t i = 0; i < kNumBlocks; i++) {
    folly::IoUringBackend::FileOpCallback readCb = [&, i](int res) {
      CHECK_EQ(res, readDataVec[i].size());
      ++num;
    };
    backendPtr->queueRead(
        fd,
        readDataVec[i].data(),
        readDataVec[i].size(),
        i * kBlockSize,
        std::move(readCb));
  }

  folly::IoUringBackend::FileOpCallback bigReadCb = [&](int res) {
    CHECK_EQ(res, bigReadData.size());
  };

  backendPtr->queueRead(
      fd, bigReadData.data(), bigReadData.size(), 0, std::move(bigReadCb));

  evbPtr->loop();

  EXPECT_EQ(num, kNumBlocks);
}

TEST(IoUringBackend, FileWriteMany) {
  static constexpr uint32_t kBackendCapacity = 256;
  static constexpr uint32_t kBackendMaxSubmit = 32;
  static constexpr uint32_t kBackendMaxGet = 32;

  folly::IoUringOptions options;
  options.setCapacity(kBackendCapacity)
      .setMaxSubmit(kBackendMaxSubmit)
      .setMaxGet(kBackendMaxGet)
      .setUseRegisteredFds(0);
  auto evbPtr = getEventBase(std::move(options));
  SKIP_IF(!evbPtr) << "Backend not available";

  static constexpr size_t kNumBlocks = 8 * 1024;
  static constexpr size_t kBlockSize = 4096;
  static constexpr size_t kBigBlockSize = 2 * 1024 * 1024;

  static constexpr size_t kFileSize = kNumBlocks * kBlockSize;
  auto tempFile = folly::test::TempFileUtil::getTempFile(kFileSize);

  int fd = folly::fileops::open(tempFile.path().c_str(), O_DIRECT | O_RDWR);
  if (fd == -1) {
    fd = folly::fileops::open(tempFile.path().c_str(), O_RDWR);
  }
  SKIP_IF(fd == -1) << "Tempfile can't be opened: " << folly::errnoStr(errno);
  SCOPE_EXIT {
    folly::fileops::close(fd);
  };

  auto* backendPtr = dynamic_cast<folly::IoUringBackend*>(evbPtr->getBackend());
  CHECK(!!backendPtr);

  size_t num = 0;
  AlignedBuf writeData(kBlockSize, 'A');
  std::vector<AlignedBuf> writeDataVec(kNumBlocks, writeData);

  AlignedBuf bigWriteData(kBigBlockSize, 'A');

  bool bFdatasync = false;

  for (size_t i = 0; i < kNumBlocks; i++) {
    folly::IoUringBackend::FileOpCallback writeCb = [&, i](int res) {
      CHECK_EQ(res, writeDataVec[i].size());
      ++num;

      if (num == kNumBlocks) {
        folly::IoUringBackend::FileOpCallback fdatasyncCb = [&](int res) {
          CHECK_EQ(res, 0);
          bFdatasync = true;
        };
        backendPtr->queueFdatasync(fd, std::move(fdatasyncCb));
      }
    };

    backendPtr->queueWrite(
        fd,
        writeDataVec[i].data(),
        writeDataVec[i].size(),
        i * kBlockSize,
        std::move(writeCb));
  }

  evbPtr->loop();
  EXPECT_EQ(num, kNumBlocks);
  EXPECT_EQ(bFdatasync, true);

  bool bFsync = false;
  folly::IoUringBackend::FileOpCallback bigWriteCb = [&](int res) {
    CHECK_EQ(res, bigWriteData.size());

    folly::IoUringBackend::FileOpCallback fsyncCb = [&](int res) {
      CHECK_EQ(res, 0);
      bFsync = true;
    };

    backendPtr->queueFsync(fd, std::move(fsyncCb));
  };

  backendPtr->queueWrite(
      fd, bigWriteData.data(), bigWriteData.size(), 0, std::move(bigWriteCb));

  evbPtr->loop();

  EXPECT_EQ(bFsync, true);
}

TEST(IoUringBackend, SendmsgRecvmsg) {
  static constexpr uint32_t kBackendCapacity = 256;
  static constexpr uint32_t kBackendMaxSubmit = 32;
  static constexpr uint32_t kBackendMaxGet = 32;

  folly::IoUringOptions options;
  options.setCapacity(kBackendCapacity)
      .setMaxSubmit(kBackendMaxSubmit)
      .setMaxGet(kBackendMaxGet)
      .setUseRegisteredFds(0);
  auto evbPtr = getEventBase(std::move(options));
  SKIP_IF(!evbPtr) << "Backend not available";

  auto* backendPtr = dynamic_cast<folly::IoUringBackend*>(evbPtr->getBackend());
  CHECK(!!backendPtr);

  // we want raw sockets
  auto sendFd = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  CHECK_GT(sendFd, 0);
  auto recvFd = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  CHECK_GT(recvFd, 0);

  folly::SocketAddress addr("::1", 0);

  sockaddr_storage addrStorage;
  addr.getAddress(&addrStorage);
  auto& saddr = reinterpret_cast<sockaddr&>(addrStorage);
  auto ret = ::bind(sendFd, &saddr, addr.getActualSize());
  CHECK_EQ(ret, 0);
  ret = ::bind(recvFd, &saddr, addr.getActualSize());
  CHECK_EQ(ret, 0);

  folly::SocketAddress sendAddr;
  folly::SocketAddress recvAddr;
  sendAddr.setFromLocalAddress(folly::NetworkSocket(sendFd));
  recvAddr.setFromLocalAddress(folly::NetworkSocket(recvFd));

  bool sendDone = false;
  bool recvDone = false;

  static constexpr size_t kNumBytes = 64;
  static std::array<char, kNumBytes> sendBuf, recvBuf;

  folly::IoUringBackend::FileOpCallback sendCb = [&](int res) {
    CHECK_EQ(res, kNumBytes);
    CHECK(!sendDone);
    sendDone = true;

    if (recvDone) {
      evbPtr->terminateLoopSoon();
    }
  };

  folly::IoUringBackend::FileOpCallback recvCb = [&](int res) {
    CHECK_EQ(res, kNumBytes);
    CHECK(!recvDone);
    recvDone = true;

    CHECK_EQ(::memcmp(sendBuf.data(), recvBuf.data(), kNumBytes), 0);

    if (sendDone) {
      evbPtr->terminateLoopSoon();
    }
  };

  struct msghdr sendMsg = {};
  struct msghdr recvMsg = {};
  struct iovec sendIov, recvIov;
  sendIov.iov_base = sendBuf.data();
  sendIov.iov_len = sendBuf.size();

  recvIov.iov_base = recvBuf.data();
  recvIov.iov_len = recvBuf.size();

  recvAddr.getAddress(&addrStorage);

  sendMsg.msg_iov = &sendIov;
  sendMsg.msg_iovlen = 1;

  sendMsg.msg_name = reinterpret_cast<void*>(&addrStorage);
  sendMsg.msg_namelen = recvAddr.getActualSize();

  recvMsg.msg_iov = &recvIov;
  recvMsg.msg_iovlen = 1;

  ::memset(sendBuf.data(), 0xAB, sendBuf.size());
  ::memset(recvBuf.data(), 0x0, recvBuf.size());

  CHECK_NE(::memcmp(sendBuf.data(), recvBuf.data(), kNumBytes), 0);

  backendPtr->queueRecvmsg(recvFd, &recvMsg, 0, std::move(recvCb));
  backendPtr->queueSendmsg(sendFd, &sendMsg, 0, std::move(sendCb));

  evbPtr->loopForever();

  CHECK(sendDone && recvDone);

  folly::fileops::close(sendFd);
  folly::fileops::close(recvFd);
}

TEST(IoUringBackend, ProvidedBuffers) {
  auto evbPtr = getEventBase();
  std::unique_ptr<folly::IoUringBackend> backend;
  try {
    /* 2 buffers of size 2 */
    folly::IoUringOptions options;
    options.setInitialProvidedBuffers(2, 2);
    backend = std::make_unique<folly::IoUringBackend>(std::move(options));
  } catch (folly::IoUringBackend::NotAvailable const&) {
  }
  SKIP_IF(!backend) << "Backend not available";

  auto* bufferProvider = backend->bufferProvider();
  ASSERT_NE(bufferProvider, nullptr);
  EXPECT_EQ(bufferProvider, backend->bufferProvider());
  EXPECT_EQ(2, bufferProvider->count());

  struct Reader : folly::IoSqeBase {
    Reader(int fd, uint16_t bgid, std::function<void(int, uint32_t)> oncqe)
        : fd_(fd), bgid_(bgid), oncqe_(oncqe) {}

    void processSubmit(struct io_uring_sqe* sqe) noexcept override {
      io_uring_prep_read(sqe, fd_, nullptr, 2 /* max read 2 per go */, 0);
      sqe->flags |= IOSQE_BUFFER_SELECT;
      sqe->buf_group = bgid_;
    }

    void callback(const io_uring_cqe* cqe) noexcept override {
      oncqe_(cqe->res, cqe->flags);
    }

    void callbackCancelled(const io_uring_cqe*) noexcept override { FAIL(); }

    int fd_;
    uint16_t bgid_;
    std::function<void(int, uint32_t)> oncqe_;
  };

  int fds[2];
  ASSERT_EQ(0, folly::fileops::pipe(fds));
  SCOPE_EXIT {
    folly::fileops::close(fds[0]);
    folly::fileops::close(fds[1]);
  };

  std::vector<std::pair<int, uint32_t>> cqes;
  std::vector<std::unique_ptr<Reader>> readers;
  auto addReaders = [&](int n) {
    for (int i = 0; i < n; i++) {
      readers.push_back(
          std::make_unique<Reader>(
              fds[0], bufferProvider->gid(), [&](int r, uint32_t f) {
                cqes.emplace_back(r, f);
              }));
      backend->submit(*readers.back());
    }
  };

  auto toString = [](std::unique_ptr<folly::IOBuf> x) -> std::string {
    std::string ret;
    x->appendTo(ret);
    return ret;
  };

  addReaders(3);
  ASSERT_EQ(6, folly::fileops::write(fds[1], "123456", 6));
  backend->eb_event_base_loop(EVLOOP_ONCE);
  ASSERT_EQ(3, cqes.size()) << "expect 2 completions and 1 nobufs";

  EXPECT_EQ(-ENOBUFS, cqes[2].first);
  ASSERT_EQ(2, cqes[0].first);
  ASSERT_EQ(2, cqes[1].first);
  EXPECT_EQ(
      "12", toString(bufferProvider->getIoBuf(cqes[0].second >> 16, 2, false)));
  EXPECT_EQ(
      "34", toString(bufferProvider->getIoBuf(cqes[1].second >> 16, 2, false)));
  EXPECT_EQ(
      (folly::IoUringBackend::CqeStats{.providedBufferCount = 2}),
      backend->getStats().cqe);

  // now the buffers should be back
  readers.clear();
  cqes.clear();
  addReaders(1);
  backend->eb_event_base_loop(EVLOOP_ONCE);
  ASSERT_EQ(1, cqes.size());
  EXPECT_EQ(2, cqes[0].first);
  EXPECT_EQ(
      "56", toString(bufferProvider->getIoBuf(cqes[0].second >> 16, 2, false)));
  EXPECT_EQ(
      (folly::IoUringBackend::CqeStats{.providedBufferCount = 1}),
      backend->getStats().cqe);
}

TEST(IoUringBackend, IncrementalProvidedBuffersReportBufMore) {
  auto evbPtr = getEventBase();
  std::unique_ptr<folly::IoUringBackend> backend;
  try {
    folly::IoUringOptions options;
    options.setInitialProvidedBuffers(8, 2).setEnableIncrementalBuffers(true);
    backend = std::make_unique<folly::IoUringBackend>(std::move(options));
  } catch (folly::IoUringBackend::NotAvailable const&) {
  }
  SKIP_IF(!backend) << "Backend not available";

  struct Reader : folly::IoSqeBase {
    Reader(int fd, uint16_t bgid) : fd_(fd), bgid_(bgid) {}

    void processSubmit(struct io_uring_sqe* sqe) noexcept override {
      io_uring_prep_read(sqe, fd_, nullptr, 2, 0);
      sqe->flags |= IOSQE_BUFFER_SELECT;
      sqe->buf_group = bgid_;
    }

    void callback(const io_uring_cqe* cqe) noexcept override {
      res_ = cqe->res;
    }

    void callbackCancelled(const io_uring_cqe*) noexcept override { FAIL(); }

    int fd_;
    uint16_t bgid_;
    int res_{0};
  };

  int fds[2];
  ASSERT_EQ(0, folly::fileops::pipe(fds));
  SCOPE_EXIT {
    folly::fileops::close(fds[0]);
    folly::fileops::close(fds[1]);
  };

  Reader reader(fds[0], backend->bufferProvider()->gid());
  backend->submit(reader);
  ASSERT_EQ(2, folly::fileops::write(fds[1], "12", 2));
  backend->eb_event_base_loop(EVLOOP_ONCE);
  ASSERT_EQ(2, reader.res_);

  EXPECT_EQ(
      (folly::IoUringBackend::CqeStats{
          .providedBufferCount = 1, .bufMoreCount = 1}),
      backend->getStats().cqe);
}

TEST(IoUringBackend, DynamicProvidedBuffers) {
  auto evbPtr = getEventBase();
  std::unique_ptr<folly::IoUringBackend> backend;
  try {
    folly::IoUringOptions options;
    options.setInitialProvidedBuffers(32, 2).setProvidedBufferRingMode(
        folly::IoUringOptions::ProvidedBufferRingMode::Dynamic);
    backend = std::make_unique<folly::IoUringBackend>(std::move(options));
  } catch (folly::IoUringBackend::NotAvailable const&) {
  }
  SKIP_IF(!backend) << "Backend not available";

  auto* bufferProvider = backend->bufferProvider();
  ASSERT_NE(bufferProvider, nullptr);
  EXPECT_EQ(2, bufferProvider->count());

  folly::IoUringBufferProvider::Stats stats;
  bufferProvider->getStats(stats);
  EXPECT_EQ(2, stats.areaCount);
}

TEST(IoUringBackend, ProvidedBufferRingsPow2) {
  folly::IoUringOptions options;
  EXPECT_THROW(options.setProvidedBufRings(3), std::runtime_error);
}

TEST(IoUringBackend, ProvidedBufferRingMultiple) {
  auto evbPtr = getEventBase();
  std::unique_ptr<folly::IoUringBackend> backend;
  try {
    folly::IoUringOptions options;
    options.setInitialProvidedBuffers(2, 2).setProvidedBufRings(2);
    backend = std::make_unique<folly::IoUringBackend>(std::move(options));
  } catch (folly::IoUringBackend::NotAvailable const&) {
  }
  SKIP_IF(!backend) << "Backend not available";

  auto* bp1 = backend->bufferProvider();
  ASSERT_NE(bp1, nullptr);

  auto* bp2 = backend->bufferProvider();
  ASSERT_NE(bp2, nullptr);

  auto* bp3 = backend->bufferProvider();
  ASSERT_NE(bp3, nullptr);

  EXPECT_NE(bp1, bp2);
  EXPECT_EQ(bp1, bp3);
}

TEST(IoUringBackend, ProvidedBufferRing) {
  auto evbPtr = getEventBase();
  int constexpr kBuffs = 3;
  for (int keep = 0; keep <= kBuffs; keep++) {
    std::vector<std::unique_ptr<folly::IOBuf>> bufs;
    std::unique_ptr<folly::IoUringBackend> backend;
    try {
      folly::IoUringOptions options;
      options.setInitialProvidedBuffers(1024, kBuffs);
      backend = std::make_unique<folly::IoUringBackend>(std::move(options));
    } catch (folly::IoUringBackend::NotAvailable const&) {
      return;
    }

    auto* bufferProvider = backend->bufferProvider();
    ASSERT_NE(bufferProvider, nullptr);
    for (int i = 0; i < 16; i++) {
      bufferProvider->getIoBuf(i % kBuffs, 1, false);
    }
    for (int i = 0; i < keep; i++) {
      bufs.push_back(bufferProvider->getIoBuf(i % kBuffs, 1, false));
    }
  }
}

TEST(IoUringBackend, BigProvidedBufferRing) {
  auto evbPtr = getEventBase();
  // test that big buffer rings don't have memory corruption
  int constexpr kBuffs = 32000;
  int constexpr kSize = 100000;

  std::unique_ptr<folly::IoUringBackend> backend;
  try {
    folly::IoUringOptions options;
    options.setInitialProvidedBuffers(kSize, kBuffs);
    backend = std::make_unique<folly::IoUringBackend>(std::move(options));
  } catch (folly::IoUringBackend::NotAvailable const&) {
    return;
  }

  auto* bufferProvider = backend->bufferProvider();
  ASSERT_NE(bufferProvider, nullptr);
  for (int i = 0; i < kBuffs; i++) {
    // test that we can obtain all the possible buffers and return them
    auto buff = bufferProvider->getIoBuf(i, kSize, false);
    memset(buff->writableData(), 0, buff->length());
  }
}

TEST(IoUringBackend, IncrementalBuffers) {
  auto evbPtr = getEventBase();
  std::unique_ptr<folly::IoUringBackend> backend;
  try {
    /* 2 buffers of size 32 bytes with incremental buffers enabled */
    folly::IoUringOptions options;
    options
        .setInitialProvidedBuffers(32, 2) // 32 bytes per buffer, 2 buffers
        .setEnableIncrementalBuffers(true);
    backend = std::make_unique<folly::IoUringBackend>(std::move(options));
  } catch (folly::IoUringBackend::NotAvailable const&) {
  }
  SKIP_IF(!backend);

  auto* bufferProvider = backend->bufferProvider();
  ASSERT_NE(bufferProvider, nullptr);
  EXPECT_EQ(2, bufferProvider->count());
  EXPECT_EQ(32, bufferProvider->sizePerBuffer());

  struct Reader : folly::IoSqeBase {
    Reader(int fd, uint16_t bgid, std::function<void(int, uint32_t)> oncqe)
        : fd_(fd), bgid_(bgid), oncqe_(oncqe) {}

    void processSubmit(struct io_uring_sqe* sqe) noexcept override {
      io_uring_prep_read(sqe, fd_, nullptr, 32 /* max read 32 per go */, 0);
      sqe->flags |= IOSQE_BUFFER_SELECT;
      sqe->buf_group = bgid_;
    }

    void callback(const io_uring_cqe* cqe) noexcept override {
      oncqe_(cqe->res, cqe->flags);
    }

    void callbackCancelled(const io_uring_cqe*) noexcept override { FAIL(); }

    int fd_;
    uint16_t bgid_;
    std::function<void(int, uint32_t)> oncqe_;
  };

  int fds[2];
  ASSERT_EQ(0, folly::fileops::pipe(fds));
  SCOPE_EXIT {
    folly::fileops::close(fds[0]);
    folly::fileops::close(fds[1]);
  };

  std::vector<std::pair<int, uint32_t>> cqes;
  std::vector<std::unique_ptr<Reader>> readers;

  auto addReaders = [&](int n) {
    for (int i = 0; i < n; i++) {
      readers.push_back(
          std::make_unique<Reader>(
              fds[0], bufferProvider->gid(), [&](int r, uint32_t f) {
                cqes.emplace_back(r, f);
              }));
      backend->submit(*readers.back());
    }
  };

  auto toString = [](const std::unique_ptr<folly::IOBuf>& x) -> std::string {
    std::string ret;
    x->appendTo(ret);
    return ret;
  };

  auto generateTestData = [](size_t length, char startChar) -> std::string {
    std::string data;
    data.reserve(length);
    for (size_t i = 0; i < length; i++) {
      data.push_back(startChar + (i % 26));
    }
    return data;
  };
  addReaders(1);

  std::string data1 = generateTestData(10, 'a'); // "abcdefghij"
  ASSERT_EQ(10, folly::fileops::write(fds[1], data1.c_str(), data1.size()));
  backend->eb_event_base_loop(EVLOOP_ONCE);

  ASSERT_EQ(1, cqes.size());
  EXPECT_EQ(10, cqes[0].first);
  EXPECT_TRUE(cqes[0].second & IORING_CQE_F_BUFFER);

  uint16_t bufferId1 = cqes[0].second >> 16;
  bool hasMore1 = !!(cqes[0].second & IORING_CQE_F_BUF_MORE);
  auto iob1 = bufferProvider->getIoBuf(bufferId1, cqes[0].first, hasMore1);
  EXPECT_TRUE(hasMore1);
  ASSERT_NE(iob1, nullptr);
  EXPECT_EQ(data1, toString(iob1));

  readers.clear();
  addReaders(1);

  std::string data2 = generateTestData(20, 'k'); // "klmnopqrstuvwxyz{|}~"
  ASSERT_EQ(20, folly::fileops::write(fds[1], data2.c_str(), data2.size()));
  backend->eb_event_base_loop(EVLOOP_ONCE);

  ASSERT_EQ(2, cqes.size());
  EXPECT_EQ(20, cqes[1].first);

  uint16_t bufferId2 = cqes[1].second >> 16;
  bool hasMore2 = !!(cqes[1].second & IORING_CQE_F_BUF_MORE);
  auto iob2 = bufferProvider->getIoBuf(bufferId2, cqes[1].first, hasMore2);

  EXPECT_EQ(bufferId1, bufferId2);
  EXPECT_TRUE(hasMore2);
  ASSERT_NE(iob2, nullptr);
  EXPECT_EQ(data2, toString(iob2));

  readers.clear();
  addReaders(2);

  std::string data3 = generateTestData(34, 'A');
  ASSERT_EQ(34, folly::fileops::write(fds[1], data3.c_str(), data3.size()));
  backend->eb_event_base_loop(EVLOOP_ONCE);
  ASSERT_EQ(4, cqes.size());

  ASSERT_EQ(cqes[2].first, 2);
  ASSERT_EQ(cqes[3].first, 32);

  uint16_t bufferId3a = cqes[2].second >> 16;
  bool hasMore3a = !!(cqes[2].second & IORING_CQE_F_BUF_MORE);
  auto iob3a = bufferProvider->getIoBuf(bufferId3a, cqes[2].first, hasMore3a);

  uint16_t bufferId3b = cqes[3].second >> 16;
  bool hasMore3b = !!(cqes[3].second & IORING_CQE_F_BUF_MORE);
  auto iob3b = bufferProvider->getIoBuf(bufferId3b, cqes[3].first, hasMore3b);

  EXPECT_EQ(bufferId1, bufferId3a);
  EXPECT_NE(bufferId3a, bufferId3b);
  EXPECT_FALSE(hasMore3a);
  EXPECT_FALSE(hasMore3b);

  size_t len3a = cqes[2].first;
  size_t len3b = cqes[3].first;
  EXPECT_EQ("AB", toString(iob3a));
  EXPECT_EQ(data3.substr(len3a, len3b), toString(iob3b));

  readers.clear();
  addReaders(1);

  std::string data4 = generateTestData(10, 'Z');
  ASSERT_EQ(10, folly::fileops::write(fds[1], data4.c_str(), data4.size()));
  backend->eb_event_base_loop(EVLOOP_ONCE);

  ASSERT_EQ(5, cqes.size());
  EXPECT_EQ(-ENOBUFS, cqes[4].first);

  iob1.reset();
  iob2.reset();

  readers.clear();
  addReaders(1);
  backend->eb_event_base_loop(EVLOOP_ONCE);

  ASSERT_EQ(6, cqes.size());
  EXPECT_EQ(-ENOBUFS, cqes[5].first);

  iob3a.reset();

  readers.clear();
  addReaders(1);
  backend->eb_event_base_loop(EVLOOP_ONCE);

  ASSERT_EQ(7, cqes.size());
  EXPECT_EQ(10, cqes[6].first);

  uint16_t bufferId4 = cqes[6].second >> 16;
  bool hasMore4 = !!(cqes[6].second & IORING_CQE_F_BUF_MORE);
  auto iob4 = bufferProvider->getIoBuf(bufferId4, cqes[6].first, hasMore4);

  EXPECT_EQ(bufferId1, bufferId4);
  EXPECT_TRUE(hasMore4);
  EXPECT_EQ(data4, toString(iob4));

  iob3b.reset();
  readers.clear();
  addReaders(1);

  std::string data5 = generateTestData(22, 'A');
  ASSERT_EQ(22, folly::fileops::write(fds[1], data5.c_str(), data5.size()));
  backend->eb_event_base_loop(EVLOOP_ONCE);

  ASSERT_EQ(8, cqes.size());
  EXPECT_EQ(22, cqes[7].first);

  uint16_t bufferId5 = cqes[7].second >> 16;
  bool hasMore5 = !!(cqes[7].second & IORING_CQE_F_BUF_MORE);
  auto iob5 = bufferProvider->getIoBuf(bufferId5, cqes[7].first, hasMore5);

  EXPECT_EQ(bufferId5, bufferId1);
  EXPECT_FALSE(hasMore5);
  ASSERT_NE(iob5, nullptr);
  EXPECT_EQ(data5, toString(iob5));

  readers.clear();
  addReaders(1);
  std::string data6 = generateTestData(32, 'a');
  ASSERT_EQ(32, folly::fileops::write(fds[1], data6.c_str(), data6.size()));
  backend->eb_event_base_loop(EVLOOP_ONCE);

  ASSERT_EQ(9, cqes.size());
  EXPECT_EQ(32, cqes[8].first);

  uint16_t bufferId6 = cqes[8].second >> 16;
  bool hasMore6 = !!(cqes[8].second & IORING_CQE_F_BUF_MORE);
  auto iob6 = bufferProvider->getIoBuf(bufferId6, cqes[8].first, hasMore6);

  EXPECT_NE(bufferId6, bufferId5);
  EXPECT_FALSE(hasMore6);
  ASSERT_NE(iob6, nullptr);
  EXPECT_EQ(data6, toString(iob6));

  readers.clear();
  addReaders(1);

  std::string data7 = generateTestData(10, 'Z');
  ASSERT_EQ(10, folly::fileops::write(fds[1], data7.c_str(), data7.size()));
  backend->eb_event_base_loop(EVLOOP_ONCE);

  ASSERT_EQ(10, cqes.size());
  EXPECT_EQ(-ENOBUFS, cqes[9].first);

  iob6.reset();

  readers.clear();
  addReaders(1);
  backend->eb_event_base_loop(EVLOOP_ONCE);

  ASSERT_EQ(11, cqes.size());
  EXPECT_EQ(10, cqes[10].first);

  uint16_t bufferId7 = cqes[10].second >> 16;
  bool hasMore7 = !!(cqes[10].second & IORING_CQE_F_BUF_MORE);
  auto iob7 = bufferProvider->getIoBuf(bufferId7, cqes[10].first, hasMore7);

  EXPECT_EQ(bufferId6, bufferId7);
  EXPECT_TRUE(hasMore7);
  EXPECT_EQ(data7, toString(iob7));

  backend.reset();
  EXPECT_EQ(data4, toString(iob4));
  EXPECT_EQ(data5, toString(iob5));
  EXPECT_EQ(data7, toString(iob7));

  iob4.reset();
  iob5.reset();
  iob7.reset();
}

TEST(IoUringBackend, IncrementalBuffersEnobufTracking) {
  auto evbPtr = getEventBase();
  std::unique_ptr<folly::IoUringBackend> backend;
  try {
    /* 2 buffers of size 32 bytes with incremental buffers enabled */
    folly::IoUringOptions options;
    options
        .setInitialProvidedBuffers(32, 2) // 32 bytes per buffer, 2 buffers
        .setEnableIncrementalBuffers(true);
    backend = std::make_unique<folly::IoUringBackend>(std::move(options));
  } catch (folly::IoUringBackend::NotAvailable const&) {
  }
  SKIP_IF(!backend);

  auto* bufferProvider = backend->bufferProvider();
  ASSERT_NE(bufferProvider, nullptr);
  EXPECT_EQ(2, bufferProvider->count());
  EXPECT_EQ(32, bufferProvider->sizePerBuffer());

  struct Reader : folly::IoSqeBase {
    Reader(int fd, uint16_t bgid, std::function<void(int, uint32_t)> oncqe)
        : fd_(fd), bgid_(bgid), oncqe_(oncqe) {}

    void processSubmit(struct io_uring_sqe* sqe) noexcept override {
      io_uring_prep_read(sqe, fd_, nullptr, 32 /* max read 32 per go */, 0);
      sqe->flags |= IOSQE_BUFFER_SELECT;
      sqe->buf_group = bgid_;
    }

    void callback(const io_uring_cqe* cqe) noexcept override {
      oncqe_(cqe->res, cqe->flags);
    }

    void callbackCancelled(const io_uring_cqe*) noexcept override { FAIL(); }

    int fd_;
    uint16_t bgid_;
    std::function<void(int, uint32_t)> oncqe_;
  };

  int fds[2];
  ASSERT_EQ(0, folly::fileops::pipe(fds));
  SCOPE_EXIT {
    folly::fileops::close(fds[0]);
    folly::fileops::close(fds[1]);
  };

  std::vector<std::pair<int, uint32_t>> cqes;
  std::vector<std::unique_ptr<Reader>> readers;

  auto addReaders = [&](int n) {
    for (int i = 0; i < n; i++) {
      readers.push_back(
          std::make_unique<Reader>(
              fds[0], bufferProvider->gid(), [&](int r, uint32_t f) {
                cqes.emplace_back(r, f);
                if (r == -ENOBUFS) {
                  // Notify the buffer provider so it can track internally
                  bufferProvider->enobuf();
                }
              }));
      backend->submit(*readers.back());
    }
  };

  auto toString = [](const std::unique_ptr<folly::IOBuf>& x) -> std::string {
    std::string ret;
    x->appendTo(ret);
    return ret;
  };

  auto generateTestData = [](size_t length, char startChar) -> std::string {
    std::string data;
    data.reserve(length);
    for (size_t i = 0; i < length; i++) {
      data.push_back(startChar + (i % 26));
    }
    return data;
  };

  addReaders(1);

  std::string data1 = generateTestData(10, 'a');
  ASSERT_EQ(10, folly::fileops::write(fds[1], data1.c_str(), data1.size()));
  backend->eb_event_base_loop(EVLOOP_ONCE);

  ASSERT_EQ(1, cqes.size());
  EXPECT_EQ(10, cqes[0].first);

  uint16_t bufferId1 = cqes[0].second >> 16;
  bool hasMore1 = !!(cqes[0].second & IORING_CQE_F_BUF_MORE);
  auto iob1 = bufferProvider->getIoBuf(bufferId1, cqes[0].first, hasMore1);
  EXPECT_EQ(data1, toString(iob1));

  readers.clear();
  addReaders(1);

  std::string data2 = generateTestData(20, 'k');
  ASSERT_EQ(20, folly::fileops::write(fds[1], data2.c_str(), data2.size()));
  backend->eb_event_base_loop(EVLOOP_ONCE);

  ASSERT_EQ(2, cqes.size());
  EXPECT_EQ(20, cqes[1].first);

  uint16_t bufferId2 = cqes[1].second >> 16;
  bool hasMore2 = !!(cqes[1].second & IORING_CQE_F_BUF_MORE);
  auto iob2 = bufferProvider->getIoBuf(bufferId2, cqes[1].first, hasMore2);
  EXPECT_EQ(data2, toString(iob2));

  readers.clear();
  addReaders(2);

  std::string data3 = generateTestData(34, 'A');
  ASSERT_EQ(34, folly::fileops::write(fds[1], data3.c_str(), data3.size()));
  backend->eb_event_base_loop(EVLOOP_ONCE);
  ASSERT_EQ(4, cqes.size());

  uint16_t bufferId3a = cqes[2].second >> 16;
  bool hasMore3a = !!(cqes[2].second & IORING_CQE_F_BUF_MORE);
  auto iob3a = bufferProvider->getIoBuf(bufferId3a, cqes[2].first, hasMore3a);

  uint16_t bufferId3b = cqes[3].second >> 16;
  bool hasMore3b = !!(cqes[3].second & IORING_CQE_F_BUF_MORE);
  auto iob3b = bufferProvider->getIoBuf(bufferId3b, cqes[3].first, hasMore3b);

  readers.clear();
  addReaders(1);

  std::string data4 = generateTestData(10, 'Z');
  ASSERT_EQ(10, folly::fileops::write(fds[1], data4.c_str(), data4.size()));
  backend->eb_event_base_loop(EVLOOP_ONCE);

  ASSERT_EQ(5, cqes.size());
  EXPECT_EQ(-ENOBUFS, cqes[4].first);

  iob1.reset();
  iob2.reset();

  readers.clear();
  addReaders(1);
  backend->eb_event_base_loop(EVLOOP_ONCE);

  ASSERT_EQ(6, cqes.size());
  EXPECT_EQ(-ENOBUFS, cqes[5].first);

  uint64_t resetCount = bufferProvider->getAndResetEnobufCount();
  EXPECT_EQ(2, resetCount) << "getAndResetEnobufCount should return 2";

  iob3a.reset();

  readers.clear();
  addReaders(1);
  backend->eb_event_base_loop(EVLOOP_ONCE);

  ASSERT_EQ(7, cqes.size());
  EXPECT_EQ(10, cqes[6].first);

  uint16_t bufferId4 = cqes[6].second >> 16;
  bool hasMore4 = !!(cqes[6].second & IORING_CQE_F_BUF_MORE);
  auto iob4 = bufferProvider->getIoBuf(bufferId4, cqes[6].first, hasMore4);
  EXPECT_EQ(data4, toString(iob4));

  iob3b.reset();
  readers.clear();
  addReaders(1);

  std::string data5 = generateTestData(22, 'B');
  ASSERT_EQ(22, folly::fileops::write(fds[1], data5.c_str(), data5.size()));
  backend->eb_event_base_loop(EVLOOP_ONCE);

  ASSERT_EQ(8, cqes.size());
  EXPECT_EQ(22, cqes[7].first);

  uint16_t bufferId5 = cqes[7].second >> 16;
  bool hasMore5 = !!(cqes[7].second & IORING_CQE_F_BUF_MORE);
  auto iob5 = bufferProvider->getIoBuf(bufferId5, cqes[7].first, hasMore5);
  EXPECT_EQ(data5, toString(iob5));

  readers.clear();
  addReaders(1);
  std::string data6 = generateTestData(32, 'C');
  ASSERT_EQ(32, folly::fileops::write(fds[1], data6.c_str(), data6.size()));
  backend->eb_event_base_loop(EVLOOP_ONCE);

  ASSERT_EQ(9, cqes.size());
  EXPECT_EQ(32, cqes[8].first);

  uint16_t bufferId6 = cqes[8].second >> 16;
  bool hasMore6 = !!(cqes[8].second & IORING_CQE_F_BUF_MORE);
  auto iob6 = bufferProvider->getIoBuf(bufferId6, cqes[8].first, hasMore6);
  EXPECT_EQ(data6, toString(iob6));

  readers.clear();
  addReaders(1);

  std::string data7 = generateTestData(10, 'X');
  ASSERT_EQ(10, folly::fileops::write(fds[1], data7.c_str(), data7.size()));
  backend->eb_event_base_loop(EVLOOP_ONCE);

  ASSERT_EQ(10, cqes.size());
  EXPECT_EQ(-ENOBUFS, cqes[9].first);

  uint64_t resetCount2 = bufferProvider->getAndResetEnobufCount();
  EXPECT_EQ(1, resetCount2)
      << "getAndResetEnobufCount should return 1 after the 3rd ENOBUFS";
}

TEST(IoUringBackend, ReceiveBundleTest) {
  constexpr size_t TEST_SIZE = 32 * 1024;
  constexpr size_t MSG_SIZE = 1024;

  auto evbPtr = getEventBase();
  std::unique_ptr<folly::IoUringBackend> backend;
  try {
    folly::IoUringOptions options;
    options.setInitialProvidedBuffers(MSG_SIZE, 64);
    backend = std::make_unique<folly::IoUringBackend>(std::move(options));
  } catch (folly::IoUringBackend::NotAvailable const&) {
  }
  SKIP_IF(!backend) << "Backend not available";

  auto* bufferProvider = backend->bufferProvider();
  ASSERT_NE(bufferProvider, nullptr);

  std::string expected(TEST_SIZE, 'A');
  for (size_t i = 0; i < TEST_SIZE; ++i) {
    expected[i] = 'A' + (i % 26); // A-Z pattern
  }

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(server_fd, 0);

  int opt = 1;
  ASSERT_EQ(
      0, setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)));

  struct sockaddr_in server_addr{};
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(0);

  ASSERT_EQ(
      0, bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)));
  ASSERT_EQ(0, listen(server_fd, 1));

  socklen_t addr_len = sizeof(server_addr);
  ASSERT_EQ(
      0, getsockname(server_fd, (struct sockaddr*)&server_addr, &addr_len));
  int server_port = ntohs(server_addr.sin_port);

  SCOPE_EXIT {
    if (server_fd >= 0) {
      close(server_fd);
    }
  };

  int client_fd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(client_fd, 0);

  struct sockaddr_in client_addr{};
  memset(&client_addr, 0, sizeof(client_addr));
  client_addr.sin_family = AF_INET;
  client_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  client_addr.sin_port = htons(server_port);

  ASSERT_EQ(
      0,
      connect(client_fd, (struct sockaddr*)&client_addr, sizeof(client_addr)));

  int accepted_fd = accept(server_fd, nullptr, nullptr);
  ASSERT_GE(accepted_fd, 0);

  SCOPE_EXIT {
    if (client_fd >= 0) {
      close(client_fd);
    }

    if (accepted_fd >= 0) {
      close(accepted_fd);
    }
  };

  std::string received;
  bool gotBundle = false;
  bool eofReceived = false;

  struct SimpleReader : folly::IoSqeBase {
    SimpleReader(
        int fd,
        uint16_t bgid,
        folly::IoUringBufferProvider* bufProvider,
        std::string* received,
        bool* gotBundle,
        bool* eofReceived)
        : fd_(fd),
          bgid_(bgid),
          bufProvider_(bufProvider),
          received_(received),
          gotBundle_(gotBundle),
          eofReceived_(eofReceived) {}

    void processSubmit(struct io_uring_sqe* sqe) noexcept override {
      io_uring_prep_recv_multishot(sqe, fd_, nullptr, 0, 0);
      sqe->ioprio |= IORING_RECVSEND_BUNDLE;
      sqe->flags |= IOSQE_BUFFER_SELECT;
      sqe->buf_group = bgid_;
    }

    void callback(const io_uring_cqe* cqe) noexcept override {
      if (cqe->res == 0) {
        *eofReceived_ = true;
        LOG(INFO) << "EOF received";
        return;
      }

      if (cqe->res > 1024) {
        *gotBundle_ = true;
      }

      uint16_t bufId = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
      bool hasMore = cqe->flags & IORING_CQE_F_BUF_MORE;
      auto buf = bufProvider_->getIoBuf(bufId, cqe->res, hasMore);

      if (buf) {
        buf->coalesce();
        received_->append(
            reinterpret_cast<const char*>(buf->data()), buf->length());
      }
    }

    void callbackCancelled(const io_uring_cqe*) noexcept override {}

   private:
    int fd_;
    uint16_t bgid_;
    folly::IoUringBufferProvider* bufProvider_;
    std::string* received_;
    bool* gotBundle_;
    bool* eofReceived_;
  };

  auto reader = std::make_unique<SimpleReader>(
      accepted_fd,
      bufferProvider->gid(),
      bufferProvider,
      &received,
      &gotBundle,
      &eofReceived);
  backend->submit(*reader);

  ssize_t sent = send(client_fd, expected.data(), expected.size(), 0);
  ASSERT_EQ(sent, expected.size()) << "Failed to send all data";

  close(client_fd);
  client_fd = -1;

  backend->eb_event_base_loop(EVLOOP_ONCE);

  EXPECT_EQ(received.size(), expected.size()) << "Should receive all data";
  EXPECT_EQ(received, expected) << "Data should match exactly";
  EXPECT_TRUE(gotBundle) << "Should detect at least one bundle";
  EXPECT_TRUE(eofReceived) << "Should receive EOF";
}

TEST(IoUringBackend, ProvidedBufferUtilization) {
  auto evbPtr = getEventBase();
  std::unique_ptr<folly::IoUringBackend> backend;
  try {
    folly::IoUringOptions options;
    options.setInitialProvidedBuffers(100, 8);
    backend = std::make_unique<folly::IoUringBackend>(std::move(options));
  } catch (folly::IoUringBackend::NotAvailable const&) {
  }
  SKIP_IF(!backend) << "Backend not available";

  auto* bufferProvider = backend->bufferProvider();
  ASSERT_NE(bufferProvider, nullptr);
  EXPECT_EQ(bufferProvider, backend->bufferProvider());
  EXPECT_EQ(8, bufferProvider->count());

  struct Reader : folly::IoSqeBase {
    Reader(int fd, uint16_t bgid, std::function<void(int, uint32_t)> oncqe)
        : fd_(fd), bgid_(bgid), oncqe_(oncqe) {}

    void processSubmit(struct io_uring_sqe* sqe) noexcept override {
      io_uring_prep_read(sqe, fd_, nullptr, 100 /* max read 100 per go */, 0);
      sqe->flags |= IOSQE_BUFFER_SELECT;
      sqe->buf_group = bgid_;
    }

    void callback(const io_uring_cqe* cqe) noexcept override {
      oncqe_(cqe->res, cqe->flags);
    }

    void callbackCancelled(const io_uring_cqe*) noexcept override { FAIL(); }

    int fd_;
    uint16_t bgid_;
    std::function<void(int, uint32_t)> oncqe_;
  };

  int fds[2];
  ASSERT_EQ(0, folly::fileops::pipe(fds));
  SCOPE_EXIT {
    folly::fileops::close(fds[0]);
    folly::fileops::close(fds[1]);
  };

  std::vector<std::pair<int, uint32_t>> cqes;
  std::vector<std::unique_ptr<Reader>> readers;
  auto addReaders = [&](int n) {
    for (int i = 0; i < n; i++) {
      readers.push_back(
          std::make_unique<Reader>(
              fds[0], bufferProvider->gid(), [&](int r, uint32_t f) {
                cqes.emplace_back(r, f);
              }));
      backend->submit(*readers.back());
    }
  };

  addReaders(8);
  ASSERT_EQ(
      800, folly::fileops::write(fds[1], std::string(800, 'A').c_str(), 800));
  backend->eb_event_base_loop(EVLOOP_ONCE);
  ASSERT_EQ(8, cqes.size()) << "expect 8 completions";

  ASSERT_EQ(100, cqes[0].first);
  ASSERT_EQ(100, cqes[1].first);
  auto iobuf0 = bufferProvider->getIoBuf(cqes[0].second >> 16, 100, false);
  auto iobuf1 = bufferProvider->getIoBuf(cqes[1].second >> 16, 100, false);
  auto iobuf2 = bufferProvider->getIoBuf(cqes[2].second >> 16, 100, false);
  auto iobuf3 = bufferProvider->getIoBuf(cqes[3].second >> 16, 100, false);
  auto iobuf4 = bufferProvider->getIoBuf(cqes[4].second >> 16, 100, false);
  auto iobuf5 = bufferProvider->getIoBuf(cqes[5].second >> 16, 100, false);
  auto iobuf6 = bufferProvider->getIoBuf(cqes[6].second >> 16, 100, false);
  auto iobuf7 = bufferProvider->getIoBuf(cqes[7].second >> 16, 100, false);

  int utilization = bufferProvider->getUtilPct();
  EXPECT_EQ(100, utilization)
      << "All 8 buffers in use, expected 100% utilization";

  iobuf4.reset();
  iobuf5.reset();
  iobuf6.reset();
  iobuf7.reset();

  utilization = bufferProvider->getUtilPct();
  EXPECT_EQ(50, utilization)
      << "4 out of 8 buffers in use, expected 50% utilization";

  iobuf0.reset();
  iobuf1.reset();
  iobuf2.reset();
  iobuf3.reset();

  utilization = bufferProvider->getUtilPct();
  EXPECT_EQ(0, utilization) << "No buffers in use, expected 0% utilization";

  readers.clear();
}

TEST(IoUringBackend, ProvidedBufferUtilizationFullRing) {
  auto evbPtr = getEventBase();
  SKIP_IF(!folly::IoUringBackend::isAvailable()) << "Backend not available";

  folly::IoUringOptions options;
  options.setInitialProvidedBuffers(100, 1024);
  auto backend = std::make_unique<folly::IoUringBackend>(std::move(options));

  auto* bufferProvider = backend->bufferProvider();
  ASSERT_NE(bufferProvider, nullptr);
  EXPECT_EQ(1024, bufferProvider->count());

  EXPECT_EQ(0, bufferProvider->getUtilPct())
      << "Freshly created full ring, expected 0% utilization";
}

TEST(IoUringBackend, DeferTaskRun) {
  std::atomic<int> doneA{0};
  std::atomic<int> doneB{0};
  struct N : folly::IoSqeBase {
    explicit N(std::atomic<int>& v) : val(v) {}
    std::atomic<int>& val;
    void processSubmit(struct io_uring_sqe* sqe) noexcept override {
      ::io_uring_prep_nop(sqe);
    }
    void callback(const io_uring_cqe*) noexcept override {
      ++val;
      delete this;
    }
    void callbackCancelled(const io_uring_cqe*) noexcept override {
      ++val;
      delete this;
    }
  };

  N* maybeLeaks = nullptr;

  std::unique_ptr<folly::IoUringBackend> backend;
  std::thread([&]() {
    folly::IoUringOptions options;
    options.setDeferTaskRun(true);
    backend = std::make_unique<folly::IoUringBackend>(std::move(options));
    backend->submitNow(*new N(doneA));
    backend->loopPoll();
    maybeLeaks = new N(doneB);
    backend->submitSoon(*maybeLeaks);
  }).join();
  backend.reset();
  EXPECT_EQ(1, doneA.load());
  ASSERT_EQ(0, doneB.load()) << "could not run on other thread";
  delete maybeLeaks;
}

namespace folly {
namespace test {
static constexpr uint32_t kCapacity = 32;
static constexpr uint32_t kMaxSubmit = 4;
static constexpr uint32_t kMaxGet = std::numeric_limits<uint32_t>::max();

struct IoUringBackendProviderBase : BackendProviderBase {
  static bool isIoUringBackend() { return true; }
};

struct IoUringBackendProvider : IoUringBackendProviderBase {
  static std::unique_ptr<folly::EventBaseBackendBase> getBackend() {
    try {
      folly::IoUringOptions options;
      options.setCapacity(kCapacity)
          .setMaxSubmit(kMaxSubmit)
          .setMaxGet(kMaxGet)
          .setUseRegisteredFds(0);

      return std::make_unique<folly::IoUringBackend>(std::move(options));
    } catch (const IoUringBackend::NotAvailable&) {
      return nullptr;
    }
  }
};

struct IoUringRegFdBackendProvider : IoUringBackendProviderBase {
  static std::unique_ptr<folly::EventBaseBackendBase> getBackend() {
    try {
      folly::IoUringOptions options;
      options.setCapacity(kCapacity)
          .setMaxSubmit(kMaxSubmit)
          .setMaxGet(kMaxGet)
          .setUseRegisteredFds(kCapacity);
      return std::make_unique<folly::IoUringBackend>(std::move(options));
    } catch (const IoUringBackend::NotAvailable&) {
      return nullptr;
    }
  }
};

// CQ polling
struct IoUringPollCQBackendProvider : IoUringBackendProviderBase {
  static std::unique_ptr<folly::EventBaseBackendBase> getBackend() {
    try {
      folly::IoUringOptions options;
      options.setCapacity(kCapacity)
          .setMaxSubmit(kMaxSubmit)
          .setMaxGet(kMaxGet)
          .setUseRegisteredFds(0)
          .setFlags(folly::IoUringOptions::Flags::POLL_CQ);
      return std::make_unique<folly::IoUringBackend>(std::move(options));
    } catch (const IoUringBackend::NotAvailable&) {
      return nullptr;
    }
  }
};

// SQ/CQ polling
struct IoUringPollSQCQBackendProvider : IoUringBackendProviderBase {
  static std::unique_ptr<folly::EventBaseBackendBase> getBackend() {
    try {
      folly::IoUringOptions options;
      options.setCapacity(kCapacity)
          .setMaxSubmit(kMaxSubmit)
          .setMaxGet(kMaxGet)
          .setUseRegisteredFds(0)
          .setFlags(
              folly::IoUringOptions::Flags::POLL_SQ |
              folly::IoUringOptions::Flags::POLL_CQ);
      return std::make_unique<folly::IoUringBackend>(std::move(options));
    } catch (const IoUringBackend::NotAvailable&) {
      return nullptr;
    }
  }
};

// Instantiate the non registered fd tests
INSTANTIATE_TYPED_TEST_SUITE_P(IoUring, EventBaseTest, IoUringBackendProvider);
INSTANTIATE_TYPED_TEST_SUITE_P(
    IoUring, AsyncSignalHandlerTest, IoUringBackendProvider);

// Instantiate the registered fd tests
INSTANTIATE_TYPED_TEST_SUITE_P(
    IoUringRegFd, EventBaseTest, IoUringRegFdBackendProvider);
INSTANTIATE_TYPED_TEST_SUITE_P(
    IoUringRegFd, AsyncSignalHandlerTest, IoUringRegFdBackendProvider);

// Instantiate the poll CQ tests
INSTANTIATE_TYPED_TEST_SUITE_P(
    IoUringPollCQ, EventBaseTest, IoUringPollCQBackendProvider);
INSTANTIATE_TYPED_TEST_SUITE_P(
    IoUringPollCQ, AsyncSignalHandlerTest, IoUringPollCQBackendProvider);

// Instantiate the poll SQ/CQ tests
INSTANTIATE_TYPED_TEST_SUITE_P(
    IoUringPollSQCQ, EventBaseTest, IoUringPollCQBackendProvider);
INSTANTIATE_TYPED_TEST_SUITE_P(
    IoUringPollSQCQ, AsyncSignalHandlerTest, IoUringPollCQBackendProvider);
} // namespace test
} // namespace folly
