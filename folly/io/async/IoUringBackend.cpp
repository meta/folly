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

#include <signal.h>
#include <folly/portability/SysResource.h>

#include <cerrno>
#include <chrono>
#include <thread>

#include <folly/Demangle.h>
#include <folly/FileUtil.h>
#include <folly/GLog.h>
#include <folly/Likely.h>
#include <folly/SpinLock.h>
#include <folly/String.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <folly/io/async/IoUringBackend.h>
#include <folly/io/async/IoUringProvidedBufferRing.h>
#include <folly/lang/Bits.h>
#include <folly/portability/GFlags.h>
#include <folly/portability/Sockets.h>
#include <folly/portability/SysMman.h>
#include <folly/portability/SysSyscall.h>
#include <folly/synchronization/CallOnce.h>
#include <folly/tracing/StaticTracepoint.h>

#if __has_include(<sys/timerfd.h>)
#include <sys/timerfd.h>
#endif

#if FOLLY_HAS_LIBURING

extern "C" FOLLY_ATTR_WEAK void eb_poll_loop_pre_hook(uint64_t* call_time);
extern "C" FOLLY_ATTR_WEAK void eb_poll_loop_post_hook(
    uint64_t call_time, int ret);

namespace folly {

namespace {

struct SignalRegistry {
  struct SigInfo {
    struct sigaction sa_{};
    size_t refs_{0};
  };
  using SignalMap = std::map<int, SigInfo>;

  constexpr SignalRegistry() {}

  void notify(int sig);
  void setNotifyFd(int sig, int fd);

  // lock protecting the signal map
  folly::MicroSpinLock mapLock_ = {0};
  std::unique_ptr<SignalMap> map_;
  std::atomic<int> notifyFd_{-1};
};

SignalRegistry& getSignalRegistry() {
  static auto& sInstance = *new SignalRegistry();
  return sInstance;
}

void evSigHandler(int sig) {
  getSignalRegistry().notify(sig);
}

void SignalRegistry::notify(int sig) {
  // use try_lock in case somebody already has the lock
  std::unique_lock lk(mapLock_, std::try_to_lock);
  if (lk.owns_lock()) {
    int fd = notifyFd_.load();
    if (fd >= 0) {
      uint8_t sigNum = static_cast<uint8_t>(sig);
      std::ignore = fileops::write(fd, &sigNum, 1);
    }
  }
}

void SignalRegistry::setNotifyFd(int sig, int fd) {
  std::lock_guard g(mapLock_);
  if (fd >= 0) {
    if (!map_) {
      map_ = std::make_unique<SignalMap>();
    }
    // switch the fd
    notifyFd_.store(fd);

    auto iter = map_->find(sig);
    if (iter != map_->end()) {
      iter->second.refs_++;
    } else {
      auto& entry = (*map_)[sig];
      entry.refs_ = 1;
      struct sigaction sa = {};
      sa.sa_handler = evSigHandler;
      sa.sa_flags |= SA_RESTART;
      ::sigfillset(&sa.sa_mask);

      if (::sigaction(sig, &sa, &entry.sa_) == -1) {
        map_->erase(sig);
      }
    }
  } else {
    notifyFd_.store(fd);

    if (map_) {
      auto iter = map_->find(sig);
      if ((iter != map_->end()) && (--iter->second.refs_ == 0)) {
        auto entry = iter->second;
        map_->erase(iter);
        // just restore
        ::sigaction(sig, &entry.sa_, nullptr);
      }
    }
  }
}

void checkLogOverflow([[maybe_unused]] io_uring* ring) {
  if (::io_uring_cq_has_overflow(ring)) {
    FB_LOG_EVERY_MS(ERROR, 10000)
        << "IoUringBackend " << ring << " cq overflow";
  }
}

class SQGroupInfoRegistry {
 private:
  // a group is a collection of io_uring instances
  // that share up to numThreads SQ poll threads
  struct SQGroupInfo {
    struct SQSubGroupInfo {
      folly::F14FastSet<int> fds;
      size_t count{0};

      void add(int fd) {
        CHECK(!fds.count(fd));
        fds.insert(fd);
        ++count;
      }

      size_t remove(int fd) {
        auto iter = fds.find(fd);
        CHECK(iter != fds.end());
        fds.erase(fd);
        --count;

        return count;
      }
    };

    SQGroupInfo(size_t num, std::set<uint32_t> const& cpus)
        : subGroups(num), nextCpu(cpus.begin(), cpus.end()) {}

    // returns the least loaded subgroup
    SQSubGroupInfo* getNextSubgroup() {
      size_t min_idx = 0;
      for (size_t i = 0; i < subGroups.size(); i++) {
        if (subGroups[i].count == 0) {
          return &subGroups[i];
        }

        if (subGroups[i].count < subGroups[min_idx].count) {
          min_idx = i;
        }
      }

      return &subGroups[min_idx];
    }

    size_t add(int fd, SQSubGroupInfo* sg) {
      CHECK(!fdSgMap.count(fd));
      fdSgMap.emplace(fd, sg);
      sg->add(fd);
      ++count;

      return count;
    }

    size_t remove(int fd) {
      auto iter = fdSgMap.find(fd);
      CHECK(iter != fdSgMap.end());
      iter->second->remove(fd);
      fdSgMap.erase(iter);
      --count;

      return count;
    }

    // file descriptor to sub group index map
    folly::F14FastMap<int, SQSubGroupInfo*> fdSgMap;
    // array of subgoups
    std::vector<SQSubGroupInfo> subGroups;
    // number of entries
    size_t count{0};
    // Set of CPUs we will bind threads to.
    std::vector<uint32_t> nextCpu;
    int nextCpuIndex{0};
  };

  using SQGroupInfoMap = folly::F14FastMap<std::string, SQGroupInfo>;
  SQGroupInfoMap map_;
  std::mutex mutex_;

 public:
  SQGroupInfoRegistry() = default;

  using FDCreateFunc = folly::Function<int(struct io_uring_params&)>;
  using FDCloseFunc = folly::Function<void()>;

  size_t addTo(
      const std::string& groupName,
      size_t groupNumThreads,
      FDCreateFunc& createFd,
      struct io_uring_params& params,
      std::set<uint32_t> const& cpus) {
    if (groupName.empty()) {
      createFd(params);
      return 0;
    }

    std::lock_guard g(mutex_);

    auto [iter, inserted] = map_.try_emplace(groupName, groupNumThreads, cpus);
    auto& info = iter->second;
    auto* sg = info.getNextSubgroup();
    if (sg->count) {
      // we're adding to a non empty subgroup
      params.wq_fd = *(sg->fds.begin());
      params.flags |= IORING_SETUP_ATTACH_WQ;
    } else if (!info.nextCpu.empty()) {
      // First use of this subgroup, pin thread to CPU if specified.
      auto cpu = info.nextCpu[info.nextCpuIndex];
      info.nextCpuIndex = (info.nextCpuIndex + 1) % info.nextCpu.size();

      params.sq_thread_cpu = cpu;
      params.flags |= IORING_SETUP_SQ_AFF;
    }

    auto fd = createFd(params);
    if (fd < 0) {
      return 0;
    }

    return info.add(fd, sg);
  }

  size_t removeFrom(const std::string& groupName, int fd, FDCloseFunc& func) {
    if (groupName.empty()) {
      func();
      return 0;
    }

    size_t ret;

    std::lock_guard g(mutex_);

    func();

    auto iter = map_.find(groupName);
    CHECK(iter != map_.end());
    // check for empty group
    if ((ret = iter->second.remove(fd)) == 0) {
      map_.erase(iter);
    }

    return ret;
  }
};

static folly::Indestructible<SQGroupInfoRegistry> sSQGroupInfoRegistry;

bool validateZeroCopyRxOptions(IoUringOptions& options) {
  if (options.zeroCopyRx && options.zcRxNoDev) {
    return options.zcRxNumBuffers > 0 && options.zcRxRefillEntries > 0;
  }

  if (options.zeroCopyRx &&
      (options.zcRxIfname.empty() || options.zcRxIfindex <= 0 ||
       options.zcRxQueueId == -1 || !options.resolveNapiId ||
       !options.srcPortQueueId)) {
    return false;
  }

  return true;
}

} // namespace

IoUringBackend::SocketPair::SocketPair() {
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds_.data())) {
    throw std::runtime_error("socketpair error");
  }

  // set the sockets to non blocking mode
  for (auto fd : fds_) {
    auto flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

IoUringBackend::SocketPair::~SocketPair() {
  for (auto fd : fds_) {
    if (fd >= 0) {
      fileops::close(fd);
    }
  }
}

IoUringBackend::FdRegistry::FdRegistry(io_uring& ioRing, uint32_t n)
    : ioRing_(ioRing), files_(n, -1), inUse_(n), records_(n) {
  if (n > std::numeric_limits<int>::max()) {
    throw std::runtime_error("too many registered files");
  }
}

int IoUringBackend::FdRegistry::init() {
  if (inUse_) {
    int ret = ::io_uring_register_files(&ioRing_, files_.data(), inUse_);

    if (!ret) {
      // build and set the free list head if we succeed
      for (int i = 0; i < static_cast<int>(records_.size()); i++) {
        records_[i].idx_ = i;
        free_.push_front(records_[i]);
      }
    } else {
      LOG(ERROR) << "io_uring_register_files(" << inUse_ << ") "
                 << "failed errno = " << errno << ":\""
                 << folly::errnoStr(errno) << "\" " << this;
    }

    return ret;
  }

  return 0;
}

IoUringFdRegistrationRecord* IoUringBackend::FdRegistry::alloc(
    int fd) noexcept {
  if (err_ || free_.empty()) [[unlikely]] {
    return nullptr;
  }

  auto& record = free_.front();

  // now we have an idx
  int ret = ::io_uring_register_files_update(&ioRing_, record.idx_, &fd, 1);
  if (ret != 1) {
    // set the err_ flag so we do not retry again
    // this usually happens when we hit the file desc limit
    // and retrying this operation for every request is expensive
    err_ = true;
    LOG(ERROR) << "io_uring_register_files(1) " << "failed errno = " << errno
               << ":\"" << folly::errnoStr(errno) << "\" " << this;
    return nullptr;
  }

  record.fd_ = fd;
  record.count_ = 1;
  free_.pop_front();

  return &record;
}

bool IoUringBackend::FdRegistry::free(IoUringFdRegistrationRecord* record) {
  if (record && (--record->count_ == 0)) {
    record->fd_ = -1;
    int ret = ::io_uring_register_files_update(
        &ioRing_, record->idx_, &record->fd_, 1);

    // we add it to the free list anyway here
    free_.push_front(*record);

    return (ret == 1);
  }

  return false;
}

FOLLY_ALWAYS_INLINE io_uring_sqe* IoUringBackend::getUntrackedSqe() {
  io_uring_sqe* ret = ::io_uring_get_sqe(&ioRing_);
  // if running with SQ poll enabled
  // we might have to wait for an sq entry to available
  // before we can submit another one
  while (!ret) {
    if (options_.flags & Options::Flags::POLL_SQ) {
      asm_volatile_pause();
      ret = ::io_uring_get_sqe(&ioRing_);
    } else {
      submitEager();
      ret = ::io_uring_get_sqe(&ioRing_);
      CHECK(ret != nullptr);
    }
  }

  ++waitingToSubmit_;
  return ret;
}

FOLLY_ALWAYS_INLINE io_uring_sqe* IoUringBackend::getSqe() {
  ++numInsertedEvents_;
  return getUntrackedSqe();
}

void IoSqeBase::internalSubmit(io_uring_sqe* sqe) noexcept {
  if (inFlight_) {
    LOG(ERROR) << "cannot resubmit an IoSqe. type="
               << folly::demangle(typeid(*this));
    return;
  }
  inFlight_ = true;
  processSubmit(sqe);
  ::io_uring_sqe_set_data(sqe, this);
}

void IoSqeBase::internalCallback(const io_uring_cqe* cqe) noexcept {
  auto flags = cqe->flags;
  if (!(flags & (IORING_CQE_F_MORE | IORING_CQE_F_NOTIF))) {
    inFlight_ = false;
  }
  if (evb_) {
    evb_->bumpHandlingTime();
  }
  if (cancelled_) {
    callbackCancelled(cqe);
  } else {
    callback(cqe);
  }
}

FOLLY_ALWAYS_INLINE void IoUringBackend::setProcessTimers() {
  processTimers_ = true;
  --numInternalEvents_;
}

FOLLY_ALWAYS_INLINE void IoUringBackend::setProcessSignals() {
  processSignals_ = true;
  --numInternalEvents_;
}

void IoUringBackend::processPollIoSqe(
    IoUringBackend* backend, IoSqe* ioSqe, const io_uring_cqe* cqe) {
  backend->processPollIo(ioSqe, cqe->res, cqe->flags);
}

void IoUringBackend::processTimerIoSqe(
    IoUringBackend* backend, IoSqe* /*sqe*/, const io_uring_cqe* /*cqe*/) {
  backend->setProcessTimers();
}

void IoUringBackend::processSignalReadIoSqe(
    IoUringBackend* backend, IoSqe* /*sqe*/, const io_uring_cqe* /*cqe*/) {
  backend->setProcessSignals();
}

IoUringBackend::IoUringBackend(Options options)
    : options_(std::move(options)),
      numEntries_(options_.capacity),
      fdRegistry_(ioRing_, options_.registeredFds) {
  // create the timer fd
  timerFd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (timerFd_ < 0) {
    throw std::runtime_error("timerfd_create error");
  }
  if (!validateZeroCopyRxOptions(options_)) {
    throw std::runtime_error("invalid zero copy rx options");
  }

  params_.flags |= IORING_SETUP_CQSIZE;
  params_.cq_entries = options_.capacity;

  if (options_.taskRunCoop) {
    params_.flags |= IORING_SETUP_COOP_TASKRUN;
  }
  if (options_.deferTaskRun) {
    params_.flags |= IORING_SETUP_SINGLE_ISSUER;
    params_.flags |= IORING_SETUP_DEFER_TASKRUN;
    params_.flags |= IORING_SETUP_SUBMIT_ALL;
    params_.flags |= IORING_SETUP_R_DISABLED;
    usingDeferTaskrun_ = true;
  }

  if (options_.zeroCopyRx || options_.zcRxImport) {
    params_.flags |= IORING_SETUP_CQE32;
    params_.flags |= IORING_SETUP_DEFER_TASKRUN;
    params_.flags |= IORING_SETUP_SINGLE_ISSUER;
    params_.flags |= IORING_SETUP_R_DISABLED;
    params_.flags |= IORING_SETUP_COOP_TASKRUN;
    params_.flags |= IORING_SETUP_SUBMIT_ALL;
  }
  if (options_.zeroCopyRx && !options_.zcRxNoDev) {
    napiId_ =
        options_.resolveNapiId(options_.zcRxIfindex, options_.zcRxQueueId);
  }

  // poll SQ options
  if (options_.flags & Options::Flags::POLL_SQ) {
    params_.flags |= IORING_SETUP_SQPOLL;
    params_.sq_thread_idle = static_cast<__u32>(options_.sqIdle.count());
  }

  SQGroupInfoRegistry::FDCreateFunc func = [&](io_uring_params& params) {
    while (true) {
      // allocate entries both for poll add and cancel
      auto sqeSize =
          options_.sqeSize > 0 ? options_.sqeSize : 2 * options_.maxSubmit;
      int ret = ::io_uring_queue_init_params(sqeSize, &ioRing_, &params);
      if (ret == 0) {
        return ioRing_.ring_fd;
      }

      options_.capacity /= 2;
      if (options_.minCapacity && (options_.capacity >= options_.minCapacity)) {
        LOG(INFO) << "io_uring_queue_init_params(" << 2 * options_.maxSubmit
                  << "," << params.cq_entries << ") failed errno = " << errno
                  << ":\"" << folly::errnoStr(errno) << "\" " << this
                  << " retrying with capacity = " << options_.capacity;

        params_.cq_entries = options_.capacity;
        numEntries_ = options_.capacity;
        continue;
      }

      LOG(ERROR) << "io_uring_queue_init_params(" << 2 * options_.maxSubmit
                 << "," << params.cq_entries << ") failed ret = " << ret
                 << ":\"" << folly::errnoStr(ret) << "\" " << this;

      if (ret == -ENOMEM) {
        throw OutOfMemory("io_uring_queue_init error out of memory");
      }
      throw NotAvailable("io_uring_queue_init error");
    }
  };

  auto ret = sSQGroupInfoRegistry->addTo(
      options_.sqGroupName,
      options_.sqGroupNumThreads,
      func,
      params_,
      options_.sqCpus);

  if (!options_.sqGroupName.empty()) {
    LOG(INFO) << "Adding to SQ poll group \"" << options_.sqGroupName
              << "\" ret = " << ret << " fd = " << ioRing_.ring_fd;
  }

  numEntries_ *= 2;
  poolAllocRemaining_ = numEntries_;

  // timer entry
  timerEntry_ = std::make_unique<IoSqe>(this, false, true /*persist*/);
  timerEntry_->backendCb_ = IoUringBackend::processTimerIoSqe;

  // signal entry
  signalReadEntry_ = std::make_unique<IoSqe>(this, false, true /*persist*/);
  signalReadEntry_->backendCb_ = IoUringBackend::processSignalReadIoSqe;
  // delay adding the timer and signal fds until running the loop first time

  if (!usingDeferTaskrun_) {
    // can do this now, no need to delay if not using IORING_SETUP_SINGLE_ISSUER
    initSubmissionLinked();
  }
}

IoUringBackend::~IoUringBackend() {
  shuttingDown_ = true;

  cleanup();

  CHECK(!timerEntry_);
  CHECK(!signalReadEntry_);
  CHECK(freeList_.empty());

  fileops::close(timerFd_);
}

void IoUringBackend::cleanup() {
  if (ioRing_.ring_fd <= 0) {
    return;
  }

  // release the nonsubmitted items from the submitList
  auto processSubmitList = [&]() {
    IoSqeBaseList mustSubmitList;
    while (!submitList_.empty()) {
      auto* ioSqe = &submitList_.front();
      submitList_.pop_front();
      if (IoSqe* i = dynamic_cast<IoSqe*>(ioSqe); i) {
        releaseIoSqe(i);
      } else {
        mustSubmitList.push_back(*ioSqe);
      }
    }
    prepList(mustSubmitList);
  };

  // wait for the outstanding events to finish
  processSubmitList();
  while (isWaitingToSubmit() || numInsertedEvents_ > numInternalEvents_) {
    io_uring_cqe* cqe = nullptr;
    processSubmitList();
    int ret = submitEager();
    if (ret == -EEXIST) {
      LOG(ERROR) << "using DeferTaskrun, but submitting from the wrong thread";
      break;
    }

    bool const canContinue = ret != -EEXIST && ret != -EBADR;
    if (canContinue) {
      ::io_uring_wait_cqe(&ioRing_, &cqe);
    }
    internalProcessCqe(
        std::numeric_limits<unsigned int>::max(),
        InternalProcessCqeMode::CANCEL_ALL);

    if (!canContinue) {
      LOG(ERROR) << "Submit resulted in : " << folly::errnoStr(-ret)
                 << " not cleanly shutting down IoUringBackend";
      break;
    }
  }

  // release the active events
  while (!activeEvents_.empty()) {
    auto* ioSqe = static_cast<IoSqe*>(&activeEvents_.front());
    activeEvents_.pop_front();
    releaseIoSqe(ioSqe);
  }

  // free the entries
  timerEntry_.reset();
  signalReadEntry_.reset();
  freeList_.clear_and_dispose([](IoSqeBase* ioSqe) {
    delete static_cast<IoSqe*>(ioSqe);
  });

  int fd = ioRing_.ring_fd;
  SQGroupInfoRegistry::FDCloseFunc func = [&]() {
    // exit now
    ::io_uring_queue_exit(&ioRing_);
    ioRing_.ring_fd = -1;
  };

  auto ret = sSQGroupInfoRegistry->removeFrom(
      options_.sqGroupName, ioRing_.ring_fd, func);

  if (!options_.sqGroupName.empty()) {
    LOG(INFO) << "Removing from SQ poll group \"" << options_.sqGroupName
              << "\" ret = " << ret << " fd = " << fd;
  }
}

bool IoUringBackend::isAvailable() {
  static bool sAvailable = true;

  static folly::once_flag initFlag;
  folly::call_once(initFlag, [&]() {
    static constexpr auto kRetryDelay = std::chrono::milliseconds(20);
    static constexpr int kMaxAttempts = 5;
    for (int attempt = 0;; ++attempt) {
      try {
        Options options;
        options.setCapacity(16).setMaxSubmit(8);
        IoUringBackend backend(std::move(options));
        return;
      } catch (const NotAvailable&) {
        sAvailable = false;
        return;
      } catch (const OutOfMemory&) {
        if (attempt + 1 >= kMaxAttempts) {
          sAvailable = false;
          return;
        }
        /* sleep override */ std::this_thread::sleep_for(kRetryDelay);
      }
    }
  });

  return sAvailable;
}

bool IoUringBackend::addTimerFd() {
  submitOutstanding();
  auto* entry = getSqe();
  timerEntry_->prepPollAdd(entry, timerFd_, POLLIN);
  ++numInternalEvents_;
  return (1 == submitOne());
}

bool IoUringBackend::addSignalFds() {
  submitOutstanding();
  auto* entry = getSqe();
  signalReadEntry_->prepPollAdd(entry, signalFds_.readFd(), POLLIN);
  ++numInternalEvents_;
  return (1 == submitOne());
}

void IoUringBackend::scheduleTimeout() {
  if (!timerChanged_) {
    return;
  }

  // reset
  timerChanged_ = false;
  if (!timers_.empty()) {
    auto delta = std::chrono::duration_cast<std::chrono::microseconds>(
        timers_.begin()->first - std::chrono::steady_clock::now());
    if (delta < std::chrono::microseconds(1000)) {
      delta = std::chrono::microseconds(1000);
    }
    scheduleTimeout(delta);
  } else if (timerSet_) {
    scheduleTimeout(std::chrono::microseconds(0)); // disable
  }

  // we do not call addTimerFd() here
  // since it has to be added only once, after
  // we process a poll callback
}

void IoUringBackend::scheduleTimeout(const std::chrono::microseconds& us) {
  itimerspec val{};
  timerSet_ = us.count() != 0;
  val.it_interval = {0, 0};
  val.it_value.tv_sec =
      std::chrono::duration_cast<std::chrono::seconds>(us).count();
  val.it_value.tv_nsec =
      std::chrono::duration_cast<std::chrono::nanoseconds>(us).count() %
      1000000000LL;

  CHECK_EQ(::timerfd_settime(timerFd_, 0, &val, nullptr), 0);
}

namespace {

struct TimerUserData {
  std::multimap<std::chrono::steady_clock::time_point, IoUringBackend::Event*>::
      const_iterator iter;
};

void timerUserDataFreeFunction(void* v) {
  delete static_cast<TimerUserData*>(v);
}

} // namespace

void IoUringBackend::addTimerEvent(Event& event, const timeval* timeout) {
  auto getTimerExpireTime = [](const auto& timeout2) {
    using namespace std::chrono;
    auto now = steady_clock::now();

    auto us = duration_cast<microseconds>(seconds(timeout2.tv_sec)) +
        microseconds(timeout2.tv_usec);
    return now + us;
  };

  auto expire = getTimerExpireTime(*timeout);

  TimerUserData* td = static_cast<TimerUserData*>(event.getUserData());
  VLOG(6) << "addTimerEvent this=" << this << " event=" << &event << " td="
          << td << " changed_=" << timerChanged_ << " u=" << timeout->tv_usec;
  if (td) {
    CHECK_EQ(event.getFreeFunction(), timerUserDataFreeFunction);
    if (td->iter == timers_.end()) {
      td->iter = timers_.emplace(expire, &event);
    } else {
      auto ex = timers_.extract(td->iter);
      ex.key() = expire;
      td->iter = timers_.insert(std::move(ex));
    }
  } else {
    auto it = timers_.emplace(expire, &event);
    td = new TimerUserData();
    td->iter = it;
    VLOG(6) << "addTimerEvent::alloc " << td << " event=" << &event;
    event.setUserData(td, timerUserDataFreeFunction);
  }
  timerChanged_ |= td->iter == timers_.begin();
}

void IoUringBackend::removeTimerEvent(Event& event) {
  TimerUserData* td = static_cast<TimerUserData*>(event.getUserData());
  VLOG(6) << "removeTimerEvent this=" << this << " event=" << &event
          << " td=" << td;
  CHECK(td && event.getFreeFunction() == timerUserDataFreeFunction);
  timerChanged_ |= td->iter == timers_.begin();
  timers_.erase(td->iter);
  td->iter = timers_.end();
  event.setUserData(nullptr, nullptr);
  delete td;
}

size_t IoUringBackend::processTimers() {
  VLOG(3) << "IoUringBackend::processTimers " << timers_.size();
  size_t ret = 0;
  uint64_t data = 0;
  // this can fail with but it is OK since the fd
  // will still be readable
  folly::readNoInt(timerFd_, &data, sizeof(data));

  auto now = std::chrono::steady_clock::now();
  while (true) {
    auto it = timers_.begin();
    if (it == timers_.end() || now < it->first) {
      break;
    }
    timerChanged_ = true;
    Event* e = it->second;
    TimerUserData* td = static_cast<TimerUserData*>(e->getUserData());
    VLOG(5) << "processTimer " << e << " td=" << td;
    CHECK(td && e->getFreeFunction() == timerUserDataFreeFunction);
    td->iter = timers_.end();
    timers_.erase(it);
    e->eb_set_ev_res(EV_TIMEOUT);
    e->eb_clear_registration();
    // NOTE: The callback might change the set of registered timers.
    e->eb_ev_invoke_callback();
    ++ret;
  }

  VLOG(3) << "IoUringBackend::processTimers done, changed= " << timerChanged_
          << " count=" << ret;
  return ret;
}

void IoUringBackend::addSignalEvent(Event& event) {
  signals_[event.eb_ev_fd()].insert(&event);

  // we pass the write fd for notifications
  getSignalRegistry().setNotifyFd(event.eb_ev_fd(), signalFds_.writeFd());
}

void IoUringBackend::removeSignalEvent(Event& event) {
  auto iter = signals_.find(event.eb_ev_fd());
  if (iter != signals_.end()) {
    iter->second.erase(&event);
    if (iter->second.empty()) {
      signals_.erase(iter);
      getSignalRegistry().setNotifyFd(event.eb_ev_fd(), -1);
    }
  }
}

size_t IoUringBackend::processSignals() {
  size_t ret = 0;
  static constexpr auto kNumEntries = NSIG * 2;
  static_assert(
      NSIG < 256, "Use a different data type to cover all the signal values");
  std::array<bool, NSIG> processed{};
  std::array<uint8_t, kNumEntries> signals{};

  ssize_t num =
      folly::readNoInt(signalFds_.readFd(), signals.data(), signals.size());
  for (ssize_t i = 0; i < num; i++) {
    int signum = static_cast<int>(signals[i]);
    if ((signum >= 0) && (signum < static_cast<int>(processed.size())) &&
        !processed[signum]) {
      processed[signum] = true;
      auto iter = signals_.find(signum);
      if (iter != signals_.end()) {
        auto& set = iter->second;
        for (auto& event : set) {
          event->eb_set_ev_res(0);
          event->eb_set_active(true);
          event->eb_ev_invoke_callback();
          event->eb_set_active(false);
        }
      }
    }
  }
  // add the signal fd(s) back
  addSignalFds();
  return ret;
}

IoUringBackend::IoSqe* IoUringBackend::allocIoSqe() {
  // try to allocate from the pool first
  if (!freeList_.empty()) {
    auto* ret = static_cast<IoSqe*>(&freeList_.front());
    freeList_.pop_front();
    return ret;
  }

  // alloc a new IoSqe
  return allocNewIoSqe();
}

void IoUringBackend::releaseIoSqe(IoUringBackend::IoSqe* aioIoSqe) noexcept {
  // unregister the file dsecriptor record
  if (aioIoSqe->fdRecord_) {
    unregisterFd(aioIoSqe->fdRecord_);
    aioIoSqe->fdRecord_ = nullptr;
  }

  if (aioIoSqe->poolAlloc_) [[likely]] {
    aioIoSqe->event_ = nullptr;
    freeList_.push_front(*aioIoSqe);
  } else if (!aioIoSqe->persist_) {
    delete aioIoSqe;
  }
}

void IoUringBackend::IoSqe::release() noexcept {
  backend_->releaseIoSqe(this);
}

void IoUringBackend::processPollIo(
    IoSqe* ioSqe, int res, uint32_t flags) noexcept {
  auto* ev = ioSqe->event_;
  if (ev) {
    if (flags & IORING_CQE_F_MORE) {
      ioSqe->useCount_++;
      SCOPE_EXIT {
        ioSqe->useCount_--;
      };
    }

    if (!(ev->eb_ev_events() & EV_PERSIST)) {
      ev->eb_set_registration(Event::Registration::kNone);
    }

    if (ev->eb_internal()) {
      DCHECK_GT(numInternalEvents_, 0);
      --numInternalEvents_;
    }

    ev->eb_set_active(true);

    // only clamp upper bound, as no error codes are smaller than short min
    ev->eb_set_ev_res(
        static_cast<short>(
            std::min<int64_t>(res, std::numeric_limits<short>::max())));

    ioSqe->res_ = res;
    ioSqe->cqeFlags_ = flags;
    activeEvents_.push_back(*ioSqe);
  } else {
    releaseIoSqe(ioSqe);
  }
}

size_t IoUringBackend::processActiveEvents() {
  size_t ret = 0;
  IoSqe* ioSqe;

  while (!activeEvents_.empty() && !loopBreak_) {
    bool release = true;
    ioSqe = static_cast<IoSqe*>(&activeEvents_.front());
    activeEvents_.pop_front();
    ret++;
    auto* event = ioSqe->event_;
    if (event) {
      event->eb_set_active(false);
      bool inserted =
          event->eb_registration() == Event::Registration::kInserted;
      // prevent the callback from freeing the aioIoSqe
      ioSqe->useCount_++;
      // adjust the ev_res for the poll case
      event->eb_set_ev_res(getPollEvents(ioSqe->res_, event->eb_ev_events()));
      // handle spurious poll events that return 0
      // this can happen during high load on process startup
      if (event->eb_ev_res()) {
        event->eb_ev_invoke_callback();
      }
      // get the event again
      event = ioSqe->event_;
      if (event && inserted &&
          event->eb_registration() == Event::Registration::kInserted &&
          !shuttingDown_) {
        release = false;
        eb_event_modify_inserted(*event, ioSqe);
      }
      ioSqe->useCount_--;
    } else {
      ioSqe->processActive();
    }
    if (release) {
      releaseIoSqe(ioSqe);
    }
  }

  return ret;
}

void IoUringBackend::submitOutstanding() {
  delayedInit();
  prepList(submitList_);
  submitEager();
}

unsigned int IoUringBackend::processCompleted() {
  return internalProcessCqe(
      options_.maxGet, InternalProcessCqeMode::AVAILABLE_ONLY);
}

size_t IoUringBackend::loopPoll() {
  delayedInit();
  prepList(submitList_);
  size_t ret = getActiveEvents(WaitForEventsMode::DONT_WAIT);
  processActiveEvents();
  return ret;
}

void IoUringBackend::dCheckSubmitTid() {
  if (!kIsDebug) {
    // lets only check this in DEBUG
    return;
  }
  if (!usingDeferTaskrun_) {
    // only care in defer_taskrun mode
    return;
  }
  if (!submitTid_) {
    submitTid_ = std::this_thread::get_id();
  } else {
    DCHECK_EQ(*submitTid_, std::this_thread::get_id())
        << "Cannot change submit/reap threads with DeferTaskrun";
  }
}

void IoUringBackend::initSubmissionLinked() {
  // we need to call the init before adding the timer fd
  // so we avoid a deadlock - waiting for the queue to be drained
  if (options_.registeredFds > 0) {
    // now init the file registry
    // if this fails, we still continue since we
    // can run without registered fds
    fdRegistry_.init();
  }

  if (options_.initialProvidedBuffersCount) {
    try {
      IoUringBufferProvider::Options options = {
          .gid = nextBufferProviderGid(),
          .bufferCount = options_.initialProvidedBuffersCount,
          .bufferSize = options_.initialProvidedBuffersEachSize,
          .useIncrementalBuffers = options_.enableIncrementalBuffers,
      };
      for (size_t i = 0; i < options_.providedBufRings; i++) {
        bufferProviders_.push_back(
            IoUringBufferProvider::create(
                this->ioRingPtr(),
                options_.providedBufferRingMode ==
                    IoUringOptions::ProvidedBufferRingMode::Dynamic,
                options));
        options.gid = nextBufferProviderGid();
      }
    } catch (const IoUringBufferProvider::LibUringCallError& ex) {
      LOG(ERROR) << folly::to<std::string>(
          "failed to make provided buffer ring, buffer count: ",
          options_.initialProvidedBuffersCount,
          ", buffer size: ",
          options_.initialProvidedBuffersEachSize);
      throw NotAvailable(ex.what());
    }
  }
}

bool IoUringBackend::createZcBufferPool() {
  if (zcBufferPool_) {
    LOG(WARNING) << "Buffer pool already exists";
    return false;
  }
  IoUringZeroCopyBufferPool::Params params = {
      .ring = this->ioRingPtr(),
      .numBuffers = static_cast<size_t>(options_.zcRxNumBuffers),
      .bufferSizeHint = options_.zcRxBufferSizeHint,
      .rqEntries = static_cast<uint32_t>(options_.zcRxRefillEntries),
      .ifindex = static_cast<uint32_t>(options_.zcRxIfindex),
      .queueId = static_cast<uint16_t>(options_.zcRxQueueId),
      .noDev = options_.zcRxNoDev,
  };
  zcBufferPool_ = IoUringZeroCopyBufferPool::create(params);
  return zcBufferPool_ != nullptr;
}

bool IoUringBackend::importZcBufferPool(
    IoUringZeroCopyBufferPool::ExportHandle handle) {
  if (zcBufferPool_) {
    LOG(WARNING) << "Buffer pool already exists";
    return false;
  }
  auto napiId = handle.napiId_;
  auto queueId = handle.queueId_;
  zcBufferPool_ = IoUringZeroCopyBufferPool::importHandle(
      std::move(handle), this->ioRingPtr());
  if (!zcBufferPool_) {
    return false;
  }
  napiId_ = napiId;
  options_.zcRxQueueId = queueId;
  return true;
}

IoUringZeroCopyBufferPool::ExportHandle IoUringBackend::exportZcBufferPool() {
  CHECK(zcBufferPool_) << "No buffer pool to export";
  auto handle = zcBufferPool_->exportHandle();
  handle.napiId_ = napiId_;
  handle.queueId_ = options_.zcRxQueueId;
  return handle;
}

void IoUringBackend::delayedInit() {
  dCheckSubmitTid();

  if (!needsDelayedInit_) [[likely]] {
    return;
  }

  needsDelayedInit_ = false;

  if (usingDeferTaskrun_) {
    auto ret = io_uring_enable_rings(&ioRing_);
    if (ret) {
      LOG(ERROR) << "io_uring_enable_rings gave " << folly::errnoStr(-ret);
    }
    initSubmissionLinked();
  }

  if (useReqBatching() || options_.disableIoWait) {
    auto ret = ::io_uring_set_iowait(&ioRing_, false);
    if (ret && ret != -EOPNOTSUPP) {
      LOG(ERROR) << "io_uring_set_iowait gave " << folly::errnoStr(-ret);
    }
  }

  if (options_.registerRingFd) {
    // registering just has some perf impact, so no need to fall back
    if (io_uring_register_ring_fd(&ioRing_) < 0) {
      LOG(ERROR) << "unable to register io_uring ring fd";
    }
  }

  if (options_.arenaIndex > 0) {
    const size_t arenaBytes = options_.arenaRegion.iov_len;
    struct rlimit memlock{};
    if (::getrlimit(RLIMIT_MEMLOCK, &memlock) == 0 &&
        memlock.rlim_cur != RLIM_INFINITY && arenaBytes > memlock.rlim_cur) {
      LOG(WARNING)
          << "io_uring fixed-buffer arena of " << (arenaBytes >> 20)
          << " MiB exceeds RLIMIT_MEMLOCK of " << (memlock.rlim_cur >> 20)
          << " MiB; io_uring_register_buffers will likely fail with ENOMEM. "
          << "Raise RLIMIT_MEMLOCK or reduce the arena size.";
    }
    int ret = ::io_uring_register_buffers(&ioRing_, &options_.arenaRegion, 1);
    if (ret < 0) {
      const std::string memlockHint = (ret == -ENOMEM)
          ? " (RLIMIT_MEMLOCK too low for the arena; raise it or reduce arena size)"
          : "";
      throw NotAvailable(
          fmt::format(
              "io_uring_register_buffers failed for a {} MiB fixed-buffer arena: {}{}",
              arenaBytes >> 20,
              folly::errnoStr(-ret),
              memlockHint));
    }
  }

  if (!addTimerFd() || !addSignalFds()) {
    cleanup();
    throw NotAvailable("io_uring_submit error");
  }
}

int IoUringBackend::eb_event_base_loop(int flags) {
  delayedInit();

  const auto waitForEvents = (flags & EVLOOP_NONBLOCK)
      ? WaitForEventsMode::DONT_WAIT
      : WaitForEventsMode::WAIT;

  bool hadEvents = true;
  for (bool done = false; !done;) {
    scheduleTimeout();

    // check if we need to break here
    if (loopBreak_) {
      loopBreak_ = false;
      return 0;
    }

    prepList(submitList_);

    if (numInternalEvents_ == numInsertedEvents_ && timers_.empty() &&
        signals_.empty()) {
      VLOG(2) << "IoUringBackend::eb_event_base_loop nothing to do";
      return 1;
    }

    uint64_t call_time = 0;
    if (eb_poll_loop_pre_hook) {
      eb_poll_loop_pre_hook(&call_time);
    }
    if (pollLoopHook_.preLoopHook) {
      pollLoopHook_.preLoopHook(pollLoopHook_.hookCtx);
    }

    // do not wait for events if EVLOOP_NONBLOCK is set
    size_t processedEvents = getActiveEvents(waitForEvents);

    if (eb_poll_loop_post_hook) {
      eb_poll_loop_post_hook(call_time, static_cast<int>(processedEvents));
    }
    if (pollLoopHook_.postLoopHook) {
      pollLoopHook_.postLoopHook(
          pollLoopHook_.hookCtx, static_cast<int>(processedEvents));
    }

    size_t numProcessedTimers = 0;

    // save the processTimers_
    // this means we've received a notification
    // and we need to add the timer fd back
    bool processTimersFlag = processTimers_;
    if (processTimers_ && !loopBreak_) {
      numProcessedTimers = processTimers();
      processTimers_ = false;
    }

    size_t numProcessedSignals = 0;

    if (processSignals_ && !loopBreak_) {
      numProcessedSignals = processSignals();
      processSignals_ = false;
    }

    if (!activeEvents_.empty() && !loopBreak_) {
      processActiveEvents();
      if (flags & EVLOOP_ONCE) {
        done = true;
      }
    } else if (flags & EVLOOP_NONBLOCK) {
      if (signals_.empty()) {
        done = true;
      }
    }

    hadEvents = numProcessedTimers || numProcessedSignals || processedEvents;
    if (hadEvents && (flags & EVLOOP_ONCE)) {
      done = true;
    }

    VLOG(2) << "IoUringBackend::eb_event_base_loop processedEvents="
            << processedEvents << " numProcessedSignals=" << numProcessedSignals
            << " numProcessedTimers=" << numProcessedTimers << " done=" << done;

    if (processTimersFlag) {
      addTimerFd();
    }
  }

  return hadEvents ? 0 : 2;
}

int IoUringBackend::eb_event_base_loopbreak() {
  loopBreak_ = true;

  return 0;
}

int IoUringBackend::eb_event_add(Event& event, const timeval* timeout) {
  VLOG(4) << "Add event " << &event;
  // we do not support read/write timeouts
  if (timeout) {
    event.eb_set_registration(Event::Registration::kTimeout);
    addTimerEvent(event, timeout);
    return 0;
  }

  if (event.eb_ev_events() & EV_SIGNAL) {
    event.eb_set_registration(Event::Registration::kInserted);
    addSignalEvent(event);
    return 0;
  }

  if ((event.eb_ev_events() & (EV_READ | EV_WRITE)) &&
      event.eb_registration() != Event::Registration::kInserted &&
      !event.eb_active()) {
    auto* ioSqe = allocIoSqe();
    CHECK(ioSqe);
    ioSqe->event_ = &event;
    ioSqe->setEventBase(event.eb_ev_base());

    // just append it
    submitList_.push_back(*ioSqe);
    if (event.eb_internal()) {
      numInternalEvents_++;
    }
    event.eb_set_registration(Event::Registration::kInserted);
    event.setUserData(ioSqe);
  }

  return 0;
}

int IoUringBackend::eb_event_del(Event& event) {
  VLOG(4) << "Del event " << &event;
  if (!event.eb_ev_base()) {
    return -1;
  }

  if (event.eb_registration() == Event::Registration::kTimeout) {
    event.eb_set_registration(Event::Registration::kNone);
    removeTimerEvent(event);
    return 1;
  }

  if (!event.eb_active() &&
      event.eb_registration() != Event::Registration::kInserted) {
    return -1;
  }

  if (event.eb_ev_events() & EV_SIGNAL) {
    event.eb_clear_registration();
    removeSignalEvent(event);
    return 0;
  }

  auto* ioSqe = reinterpret_cast<IoSqe*>(event.getUserData());
  bool wasLinked = ioSqe->is_linked();
  ioSqe->resetEvent();

  event.eb_set_active(false);

  if (event.eb_registration() == Event::Registration::kInserted) {
    event.eb_set_registration(Event::Registration::kNone);

    // not in use  - we can cancel it
    if (!ioSqe->useCount_ && !wasLinked) {
      // io_cancel will attempt to cancel the event. the result is
      // EINVAL - usually the event has already been delivered
      // EINPROGRESS - cancellation in progress
      // EFAULT - bad ctx
      int ret = cancelOne(ioSqe);
      if (ret < 0) {
        // release the ioSqe
        releaseIoSqe(ioSqe);
      }
    } else {
      if (!ioSqe->useCount_) {
        releaseIoSqe(ioSqe);
      }
    }

    if (event.eb_internal()) {
      DCHECK_GT(numInternalEvents_, 0);
      numInternalEvents_--;
    }

    return 0;
  } else {
    // an active event need not be inserted, so we release it here
    releaseIoSqe(ioSqe);
  }

  return -1;
}

int IoUringBackend::eb_event_modify_inserted(Event& event, IoSqe* ioSqe) {
  VLOG(4) << "Modify event " << &event;
  // unlink and append
  ioSqe->unlink();
  if (event.eb_internal()) {
    numInternalEvents_++;
  }
  submitList_.push_back(*ioSqe);
  event.setUserData(ioSqe);

  return 0;
}

void IoUringBackend::submitNextLoop(IoSqeBase& ioSqe) noexcept {
  submitList_.push_back(ioSqe);
}

void IoUringBackend::submitImmediateIoSqe(IoSqeBase& ioSqe) {
  if (options_.flags &
      (Options::Flags::POLL_SQ | Options::Flags::POLL_SQ_IMMEDIATE_IO)) {
    submitNow(ioSqe);
  } else {
    submitList_.push_back(ioSqe);
  }
}

int IoUringBackend::submitOne() {
  return submitBusyCheck(1, WaitForEventsMode::DONT_WAIT);
}

void IoUringBackend::submitNow(IoSqeBase& ioSqe) {
  internalSubmit(ioSqe);
  submitBusyCheck(waitingToSubmit_, WaitForEventsMode::DONT_WAIT);
}

void IoUringBackend::internalSubmit(IoSqeBase& ioSqe) noexcept {
  auto* sqe = getSqe();
  setSubmitting();
  ioSqe.internalSubmit(sqe);
  if (ioSqe.type() == IoSqeBase::Type::Write) {
    numSendEvents_++;
    numSendEventsSinceReap_++;
  }
  doneSubmitting();
}

void IoUringBackend::submitSoon(IoSqeBase& ioSqe) noexcept {
  internalSubmit(ioSqe);
  if (waitingToSubmit_ >= options_.maxSubmit) {
    submitBusyCheck(waitingToSubmit_, WaitForEventsMode::DONT_WAIT);
  }
}

void IoUringBackend::cancel(IoSqeBase* ioSqe) {
  bool skip = false;
  ioSqe->markCancelled();
  auto* sqe = getUntrackedSqe();
  ::io_uring_prep_cancel64(sqe, reinterpret_cast<uint64_t>(ioSqe), 0);
  ::io_uring_sqe_set_data(sqe, nullptr);
  if (params_.features & IORING_FEAT_CQE_SKIP) {
    sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;
    skip = true;
  }
  VLOG(4) << "Cancel " << ioSqe << " skip=" << skip;
}

int IoUringBackend::cancelOne(IoSqe* ioSqe) {
  auto* rentry = static_cast<IoSqe*>(allocIoSqe());
  if (!rentry) {
    return 0;
  }

  auto* sqe = getSqe();
  rentry->prepCancel(sqe, ioSqe); // prev entry

  int ret = submitBusyCheck(waitingToSubmit_, WaitForEventsMode::DONT_WAIT);

  if (ret < 0) {
    // release the sqe
    releaseIoSqe(rentry);
  }

  return ret;
}

int IoUringBackend::waitForRequestBatch(uint32_t numSendEvents) noexcept {
  const auto timeoutSeconds =
      std::chrono::duration_cast<std::chrono::seconds>(options_.timeout);
  struct __kernel_timespec timeout{};
  timeout.tv_sec = timeoutSeconds.count();
  timeout.tv_nsec =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          options_.timeout - timeoutSeconds)
          .count();

  io_uring_cqe* batchCqe = nullptr;
  const int res = ::io_uring_wait_cqes(
      &ioRing_,
      &batchCqe,
      static_cast<uint32_t>(options_.batchSize) + numSendEvents,
      &timeout,
      nullptr);
  FOLLY_SDT(
      folly,
      folly_io_uring_backend_pre_submit_and_wait_timeout,
      options_.timeout,
      options_.batchSize,
      numSendEvents,
      res);

  // The first CQE is still available, so an interrupted batching wait should
  // process it instead of restarting the timeout.
  return res == -EINTR ? 0 : res;
}

int IoUringBackend::doInnerWait(io_uring_cqe*& cqe) noexcept {
  if (options_.batchTimeoutAfterFirstCompletion) {
    return waitForFirstCompletionThenBatch(cqe);
  }

  if (waitingToSubmit_) {
    submitBusyCheck(waitingToSubmit_, WaitForEventsMode::WAIT);
    return ::io_uring_peek_cqe(&ioRing_, &cqe);
  } else if (useReqBatching()) {
    struct __kernel_timespec timeout{};
    timeout.tv_sec = 0;
    timeout.tv_nsec = options_.timeout.count() * 1000;
    return ::io_uring_wait_cqes(
        &ioRing_, &cqe, options_.batchSize, &timeout, nullptr);
  } else {
    return ::io_uring_wait_cqe(&ioRing_, &cqe);
  }
}

int IoUringBackend::waitForFirstCompletionThenBatch(
    io_uring_cqe*& cqe) noexcept {
  const uint32_t numSendEvents = numSendEventsSinceReap_;
  int ret;
  if (waitingToSubmit_) {
    ret = submitBusyCheck(waitingToSubmit_, WaitForEventsMode::WAIT);
    if (ret < 0) {
      return ret;
    }
    ret = ::io_uring_peek_cqe(&ioRing_, &cqe);
  } else {
    ret = ::io_uring_wait_cqe(&ioRing_, &cqe);
  }

  if (ret != 0 || !useReqBatching()) {
    return ret;
  }

  return waitForRequestBatch(numSendEvents);
}

int IoUringBackend::doWait(io_uring_cqe*& cqe) {
  if (kIsDebug && VLOG_IS_ON(1)) {
    auto start = std::chrono::steady_clock::now();
    unsigned was = ::io_uring_cq_ready(&ioRing_);
    auto was_submit = waitingToSubmit_;
    int ret = doInnerWait(cqe);
    auto end = std::chrono::steady_clock::now();
    auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    if (micros.count()) {
      VLOG(1) << "wait took " << micros.count()
              << "us have=" << ::io_uring_cq_ready(&ioRing_) << " was=" << was
              << " submit_len=" << was_submit;
    }
    return ret;
  } else {
    return doInnerWait(cqe);
  }
}

int IoUringBackend::doPeek(io_uring_cqe*& cqe) noexcept {
  if (usingDeferTaskrun_) {
    return ::io_uring_get_events(&ioRing_);
  }
  return ::io_uring_peek_cqe(&ioRing_, &cqe);
}

size_t IoUringBackend::getActiveEvents(WaitForEventsMode waitForEvents) {
  io_uring_cqe* cqe = nullptr;

  if (zcBufferPool_) [[unlikely]] {
    zcBufferPool_->flushRefillQueueIfNeeded();
  }

  if (kIsDebug && gettingEvents_) {
    throw std::runtime_error("getting events is not reentrant");
  }
  gettingEvents_ = true;

  SCOPE_EXIT {
    gettingEvents_ = false;
  };

  int ret;
  // we can be called from the submitList() method
  // or with non blocking flags
  do {
    if (waitForEvents == WaitForEventsMode::WAIT) {
      // if polling the CQ, busy wait for one entry
      if (options_.flags & Options::Flags::POLL_CQ) {
        if (waitingToSubmit_) {
          submitBusyCheck(waitingToSubmit_, WaitForEventsMode::DONT_WAIT);
        }
        do {
          ret = ::io_uring_peek_cqe(&ioRing_, &cqe);
          asm_volatile_pause();
          // call the loop callback if installed
          // we call it every time we poll for a CQE
          // regardless of the io_uring_peek_cqe result
          if (cqPollLoopCallback_) {
            cqPollLoopCallback_();
          }
        } while (ret);
      } else {
        ret = doWait(cqe);
      }
    } else {
      if (waitingToSubmit_) {
        ret = submitBusyCheck(waitingToSubmit_, WaitForEventsMode::DONT_WAIT);
      } else {
        ret = doPeek(cqe);
      }
    }
  } while (ret == -EINTR);

  if (ret == -EBADR) {
    // cannot recover from droped CQE
    folly::terminate_with<std::runtime_error>("BADR");
  } else if (ret == -EAGAIN) {
    return 0;
  } else if (ret == -ETIME) {
    if (cqe == nullptr) {
      return 0;
    }
  } else if (ret < 0) {
    LOG(ERROR) << "wait_cqe error: " << ret;
    return 0;
  }

  return internalProcessCqe(options_.maxGet, InternalProcessCqeMode::NORMAL);
}

unsigned int IoUringBackend::internalProcessCqe(
    unsigned int maxGet, InternalProcessCqeMode mode) noexcept {
  io_uring_cqe* cqe;

  if (zcBufferPool_) [[unlikely]] {
    zcBufferPool_->flushRefillQueueIfNeeded();
  }

  unsigned int count_more = 0;
  unsigned int count = 0;
  unsigned int count_send = 0;

  numSendEventsSinceReap_ = 0;
  checkLogOverflow(&ioRing_);
  do {
    unsigned int head;
    unsigned int loop_count = 0;
    io_uring_for_each_cqe(&ioRing_, head, cqe) {
      loop_count++;
      if (zcBufferPool_ && zcBufferPool_->cqeIsNotif(cqe)) [[unlikely]] {
        zcBufferPool_->processNotificationCqe(cqe);
        continue;
      }
      if (cqe->flags & IORING_CQE_F_MORE) {
        count_more++;
      }
      IoSqeBase* sqe = reinterpret_cast<IoSqeBase*>(cqe->user_data);
      if (cqe->user_data) {
        count++;
        if (sqe->type() == IoSqeBase::Type::Write) {
          count_send++;
        }
        cqeStats_.providedBufferCount += !!(cqe->flags & IORING_CQE_F_BUFFER);
        cqeStats_.bufMoreCount += !!(cqe->flags & IORING_CQE_F_BUF_MORE);
        const bool isZeroCopyNotif = cqe->flags & IORING_CQE_F_NOTIF;
        cqeStats_.zeroCopyNotifCount += isZeroCopyNotif;
        if (isZeroCopyNotif) {
          cqeStats_.zeroCopyNotifCopiedCount += !!(
              static_cast<uint32_t>(cqe->res) & IORING_NOTIF_USAGE_ZC_COPIED);
        }
        if (mode == InternalProcessCqeMode::CANCEL_ALL) [[unlikely]] {
          sqe->markCancelled();
        }
        sqe->internalCallback(cqe);
      } else {
        // untracked, do not increment count
      }
      if (count >= options_.maxGet) {
        break;
      }
    }
    if (!loop_count) {
      break;
    }
    io_uring_cq_advance(&ioRing_, loop_count);
    if (count >= maxGet) {
      break;
    }

    // io_uring_peek_cqe will check for any overflows and copy them to the cq
    // ring.
    if (mode != InternalProcessCqeMode::AVAILABLE_ONLY) {
      int ret = ::io_uring_peek_cqe(&ioRing_, &cqe);
      if (ret == -EBADR) {
        // cannot recover from droped CQE
        folly::terminate_with<std::runtime_error>("BADR");
      } else if (ret) {
        break;
      }
    }
  } while (true);
  if (zcBufferPool_) [[unlikely]] {
    zcBufferPool_->flushRefillQueueIfNeeded();
  }
  numInsertedEvents_ -= (count - count_more);
  numSendEvents_ -= count_send;
  FOLLY_SDT(
      folly,
      folly_io_uring_backend_post_process_all_cqes,
      count,
      count_more,
      count_send,
      numInsertedEvents_,
      numSendEvents_);
  return count;
}

int IoUringBackend::submitEager() {
  int res;
  DCHECK(!isSubmitting()) << "mid processing a submit, cannot submit";
  do {
    res = ::io_uring_submit(&ioRing_);
  } while (res == -EINTR);
  VLOG(2) << "IoUringBackend::submitEager() " << waitingToSubmit_;
  if (res >= 0) {
    DCHECK(waitingToSubmit_ >= static_cast<uint32_t>(res));
    waitingToSubmit_ -= static_cast<uint32_t>(res);
  }
  return res;
}

int IoUringBackend::submitBusyCheck(
    uint32_t num, WaitForEventsMode waitForEvents) noexcept {
  uint32_t i = 0;
  int res;
  DCHECK(!isSubmitting()) << "mid processing a submit, cannot submit";
  while (i < num) {
    VLOG(2) << "IoUringBackend::submit() " << waitingToSubmit_;

    if (waitForEvents == WaitForEventsMode::WAIT) {
      if (options_.flags & Options::Flags::POLL_CQ) {
        res = ::io_uring_submit(&ioRing_);
      } else {
        if (useReqBatching() && !options_.batchTimeoutAfterFirstCompletion) {
          io_uring_cqe* cqe;
          struct __kernel_timespec timeout{};
          timeout.tv_sec = 0;
          timeout.tv_nsec = options_.timeout.count() * 1000;
          res = ::io_uring_submit_and_wait_timeout(
              &ioRing_,
              &cqe,
              options_.batchSize + numSendEvents_,
              &timeout,
              nullptr);
          FOLLY_SDT(
              folly,
              folly_io_uring_backend_pre_submit_and_wait_timeout,
              options_.timeout,
              options_.batchSize,
              numSendEvents_,
              res);
        } else {
          res = ::io_uring_submit_and_wait(&ioRing_, 1);
        }
        if (res >= 0) {
          // no more waiting
          waitForEvents = WaitForEventsMode::DONT_WAIT;
        }
      }
    } else {
      if (usingDeferTaskrun_) {
        // usingDeferTaskrun_ implies SUBMIT_ALL, and we definitely
        // want to do get_events() to process outstanding work
        res = ::io_uring_submit_and_get_events(&ioRing_);
        if (res >= 0) {
          // this is ok since we are using SUBMIT_ALL
          i = waitingToSubmit_;
          break;
        }
      } else {
        res = ::io_uring_submit(&ioRing_);
      }
    }

    if (res < 0) {
      // continue if interrupted
      if (res == -EINTR || errno == EINTR) {
        continue;
      }
      CHECK_NE(res, -EBADR);
      if (res == -EEXIST) {
        FB_LOG_EVERY_MS(ERROR, 1000)
            << "Received -EEXIST, likely calling get_events/submit "
            << " from the wrong thread with DeferTaskrun enabled";
      }

      return res;
    }

    // we do not have any other entries to submit
    if (res == 0) {
      break;
    }

    i += static_cast<uint32_t>(res);

    // if polling the CQ, busy wait for one entry
    if (waitForEvents == WaitForEventsMode::WAIT &&
        options_.flags & Options::Flags::POLL_CQ && i == num) {
      io_uring_cqe* cqe = nullptr;
      while (!cqe) {
        ::io_uring_peek_cqe(&ioRing_, &cqe);
      }
    }
  }

  DCHECK(waitingToSubmit_ >= i);
  waitingToSubmit_ -= i;
  return static_cast<int>(num);
}

size_t IoUringBackend::prepList(IoSqeBaseList& ioSqes) {
  int i = 0;

  while (!ioSqes.empty()) {
    if (static_cast<uint32_t>(i) == options_.maxSubmit) {
      int num = submitBusyCheck(i, WaitForEventsMode::DONT_WAIT);
      CHECK_EQ(num, i);
      i = 0;
    }

    auto* entry = &ioSqes.front();
    ioSqes.pop_front();
    auto* sqe = getSqe();
    entry->internalSubmit(sqe);
    i++;
  }

  return i;
}

void IoUringBackend::queueRead(
    int fd, void* buf, size_t nbytes, off_t offset, FileOpCallback&& cb) {
  iovec iov{buf, nbytes};
  auto* ioSqe = new ReadIoSqe(this, fd, &iov, offset, std::move(cb));
  ioSqe->backendCb_ = processFileOpCB;

  submitImmediateIoSqe(*ioSqe);
}

void IoUringBackend::queueWrite(
    int fd, const void* buf, size_t nbytes, off_t offset, FileOpCallback&& cb) {
  iovec iov{const_cast<void*>(buf), nbytes};
  auto* ioSqe = new WriteIoSqe(this, fd, &iov, offset, std::move(cb));
  ioSqe->backendCb_ = processFileOpCB;

  submitImmediateIoSqe(*ioSqe);
}

void IoUringBackend::queueReadv(
    int fd, Range<const iovec*> iovecs, off_t offset, FileOpCallback&& cb) {
  auto* ioSqe = new ReadvIoSqe(this, fd, iovecs, offset, std::move(cb));
  ioSqe->backendCb_ = processFileOpCB;

  submitImmediateIoSqe(*ioSqe);
}

void IoUringBackend::queueWritev(
    int fd, Range<const iovec*> iovecs, off_t offset, FileOpCallback&& cb) {
  auto* ioSqe = new WritevIoSqe(this, fd, iovecs, offset, std::move(cb));
  ioSqe->backendCb_ = processFileOpCB;

  submitImmediateIoSqe(*ioSqe);
}

void IoUringBackend::queueFsync(int fd, FileOpCallback&& cb) {
  queueFsync(fd, FSyncFlags::FLAGS_FSYNC, std::move(cb));
}

void IoUringBackend::queueFdatasync(int fd, FileOpCallback&& cb) {
  queueFsync(fd, FSyncFlags::FLAGS_FDATASYNC, std::move(cb));
}

void IoUringBackend::queueFsync(int fd, FSyncFlags flags, FileOpCallback&& cb) {
  auto* ioSqe = new FSyncIoSqe(this, fd, flags, std::move(cb));
  ioSqe->backendCb_ = processFileOpCB;

  submitImmediateIoSqe(*ioSqe);
}

void IoUringBackend::queueOpenat(
    int dfd, const char* path, int flags, mode_t mode, FileOpCallback&& cb) {
  auto* ioSqe = new FOpenAtIoSqe(this, dfd, path, flags, mode, std::move(cb));
  ioSqe->backendCb_ = processFileOpCB;

  submitImmediateIoSqe(*ioSqe);
}

void IoUringBackend::queueOpenat2(
    int dfd, const char* path, open_how* how, FileOpCallback&& cb) {
  auto* ioSqe = new FOpenAt2IoSqe(this, dfd, path, how, std::move(cb));
  ioSqe->backendCb_ = processFileOpCB;

  submitImmediateIoSqe(*ioSqe);
}

void IoUringBackend::queueClose(int fd, FileOpCallback&& cb) {
  auto* ioSqe = new FCloseIoSqe(this, fd, std::move(cb));
  ioSqe->backendCb_ = processFileOpCB;

  submitImmediateIoSqe(*ioSqe);
}

void IoUringBackend::queueStatx(
    int dirfd,
    const char* pathname,
    int flags,
    unsigned int mask,
    struct statx* statxbuf,
    FileOpCallback&& cb) {
  auto* ioSqe = new FStatxIoSqe(
      this, dirfd, pathname, flags, mask, statxbuf, std::move(cb));
  ioSqe->backendCb_ = processFileOpCB;

  submitImmediateIoSqe(*ioSqe);
}

void IoUringBackend::queueRename(
    const char* oldPath, const char* newPath, FileOpCallback&& cb) {
  auto* ioSqe = new FRenameIoSqe(this, oldPath, newPath, std::move(cb));
  ioSqe->backendCb_ = processFileOpCB;

  submitImmediateIoSqe(*ioSqe);
}

void IoUringBackend::queueUnlinkat(
    int dirfd, const char* path, int flags, FileOpCallback&& cb) {
  auto* ioSqe = new FUnlinkIoSqe(this, dirfd, path, flags, std::move(cb));
  ioSqe->backendCb_ = processFileOpCB;

  submitImmediateIoSqe(*ioSqe);
}

void IoUringBackend::queueUnlink(const char* path, FileOpCallback&& cb) {
  queueUnlinkat(AT_FDCWD, path, 0, std::move(cb));
}

void IoUringBackend::queueFallocate(
    int fd, int mode, off_t offset, off_t len, FileOpCallback&& cb) {
  auto* ioSqe = new FAllocateIoSqe(this, fd, mode, offset, len, std::move(cb));
  ioSqe->backendCb_ = processFileOpCB;

  submitImmediateIoSqe(*ioSqe);
}

void IoUringBackend::queueSendmsg(
    int fd, const msghdr* msg, unsigned int flags, FileOpCallback&& cb) {
  auto* ioSqe = new SendmsgIoSqe(this, fd, msg, flags, std::move(cb));
  ioSqe->backendCb_ = processFileOpCB;

  submitImmediateIoSqe(*ioSqe);
}

void IoUringBackend::queueRecvmsg(
    int fd, msghdr* msg, unsigned int flags, FileOpCallback&& cb) {
  auto* ioSqe = new RecvmsgIoSqe(this, fd, msg, flags, std::move(cb));
  ioSqe->backendCb_ = processFileOpCB;

  submitImmediateIoSqe(*ioSqe);
}

void IoUringBackend::queueRecvZc(
    int fd, void* buf, unsigned long nbytes, RecvZcCallback&& cb) {
  iovec iov = {
      .iov_base = buf,
      .iov_len = nbytes,
  };
  auto* ioSqe = new RecvzcIoSqe(this, fd, &iov, 0, std::move(cb));
  ioSqe->backendCb_ = processRecvZcCB;

  submitImmediateIoSqe(*ioSqe);
}

int IoUringBackend::computeSrcPortForQueueId(
    const folly::IPAddress& destAddr,
    uint16_t destPort,
    uint16_t startPort,
    uint16_t minPort,
    uint16_t maxPort) {
  if (!options_.srcPortQueueId || napiId_ < 0) {
    return -1;
  }

  return options_.srcPortQueueId(
      destAddr,
      destPort,
      options_.zcRxQueueId,
      options_.zcRxIfname.c_str(),
      startPort,
      minPort,
      maxPort);
}

void IoUringBackend::processFileOp(IoSqe* sqe, int res) noexcept {
  auto* ioSqe = reinterpret_cast<FileOpIoSqe*>(sqe);
  // save the res
  ioSqe->res_ = res;
  activeEvents_.push_back(*ioSqe);
}

void IoUringBackend::processRecvZc(
    IoSqe* sqe, const io_uring_cqe* cqe) noexcept {
  RecvzcIoSqe* ioSqe = reinterpret_cast<RecvzcIoSqe*>(sqe);
  const io_uring_zcrx_cqe* rcqe =
      reinterpret_cast<const io_uring_zcrx_cqe*>(cqe + 1);

  auto iov = ioSqe->iov_.data();
  if (cqe->res == 0 && cqe->flags == 0) {
    CHECK_EQ(static_cast<size_t>(ioSqe->offset_), iov->iov_len);
    ioSqe->res_ = cqe->res;
    ioSqe->cb_(static_cast<int>(iov->iov_len));
    delete ioSqe;
    return;
  }

  if (cqe->res < 0) {
    ioSqe->res_ = cqe->res;
    ioSqe->cb_(cqe->res);
    delete ioSqe;
    return;
  }

  auto result = zcBufferPool_->getIoBuf(cqe, rcqe);
  ::memcpy(
      reinterpret_cast<char*>(iov->iov_base) + ioSqe->offset_,
      result.buffer->data(),
      result.buffer->length());
  ioSqe->offset_ += cqe->res;
}

} // namespace folly

#endif
