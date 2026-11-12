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

#pragma once

#include <sys/types.h>

#include <chrono>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include <folly/CPortability.h>
#include <folly/Conv.h>
#include <folly/CppAttributes.h>
#include <folly/ExceptionString.h>
#include <folly/Function.h>
#include <folly/IPAddress.h>
#include <folly/Optional.h>
#include <folly/Range.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseBackendBase.h>
#include <folly/io/async/IoUringBase.h>
#include <folly/io/async/IoUringBufferProvider.h>
#include <folly/io/async/IoUringOptions.h>
#include <folly/io/async/IoUringZeroCopyBufferPool.h>
#include <folly/io/async/Liburing.h>
#include <folly/portability/Asm.h>
#include <folly/small_vector.h>

#if __has_include(<poll.h>)
#include <poll.h>
#endif

#if FOLLY_HAS_LIBURING

#include <liburing.h> // @manual
#include <net/if.h>

namespace folly {

class IoUringBackend : public EventBaseBackendBase {
 public:
  class FOLLY_EXPORT NotAvailable : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
  };

  // Thrown when io_uring setup fails with ENOMEM, typically because the
  // RLIMIT_MEMLOCK budget is exhausted. Deliberately not a NotAvailable: the
  // condition may be transient, so callers should not permanently fall back.
  class FOLLY_EXPORT OutOfMemory : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
  };

  // Type aliases for backwards compatibility
  using Options = IoUringOptions;

  explicit IoUringBackend(Options options);
  ~IoUringBackend() override;
  Options const& options() const { return options_; }

  uint32_t getArenaIndex() const { return options_.arenaIndex; }

  bool isWaitingToSubmit() const {
    return waitingToSubmit_ || !submitList_.empty();
  }
  io_uring* ioRingPtr() { return &ioRing_; }
  io_uring_params const& params() const { return params_; }
  bool useReqBatching() const {
    return options_.timeout.count() > 0 && options_.batchSize > 0;
  }
  bool useBundles() const {
    // Bundles are only supported with the dynamic provided buffer ring;
    // the fixed (static) ring's bundle support is broken.
    return (params_.features & IORING_FEAT_RECVSEND_BUNDLE) &&
        options_.providedBufferRingMode ==
        IoUringOptions::ProvidedBufferRingMode::Dynamic;
  }

  int computeSrcPortForQueueId(
      const folly::IPAddress& destAddr,
      uint16_t destPort,
      uint16_t startPort,
      uint16_t minPort,
      uint16_t maxPort);

  // from EventBaseBackendBase
  int getPollableFd() const override { return ioRing_.ring_fd; }
  int getNapiId() const override { return napiId_; }
  void queueRecvZc(
      int fd, void* buf, unsigned long nbytes, RecvZcCallback&& callback)
      override;

  int eb_event_base_loop(int flags) override;
  int eb_event_base_loopbreak() override;

  int eb_event_add(Event& event, const timeval* timeout) override;
  int eb_event_del(Event& event) override;

  bool eb_event_active(Event&, int) override { return false; }

  size_t loopPoll();
  void submitOutstanding();
  unsigned int processCompleted();

  // returns true if the current Linux kernel version
  // supports the io_uring backend
  static bool isAvailable();

  IoUringFdRegistrationRecord* registerFd(int fd) noexcept {
    return fdRegistry_.alloc(fd);
  }

  bool unregisterFd(IoUringFdRegistrationRecord* rec) {
    return fdRegistry_.free(rec);
  }

  // CQ poll mode loop callback
  using CQPollLoopCallback = folly::Function<void()>;

  void setCQPollLoopCallback(CQPollLoopCallback&& cb) {
    cqPollLoopCallback_ = std::move(cb);
  }

  // read/write/fsync/fdatasync file operation callback
  // int param is the io_uring_cqe res field
  // i.e. the result of the file operation
  using FileOpCallback = folly::Function<void(int)>;

  void queueRead(
      int fd, void* buf, size_t nbytes, off_t offset, FileOpCallback&& cb);

  void queueWrite(
      int fd,
      const void* buf,
      size_t nbytes,
      off_t offset,
      FileOpCallback&& cb);

  void queueReadv(
      int fd, Range<const iovec*> iovecs, off_t offset, FileOpCallback&& cb);

  void queueWritev(
      int fd, Range<const iovec*> iovecs, off_t offset, FileOpCallback&& cb);

  // there is no ordering between the prev submitted write
  // requests and the sync ops
  // ordering can be achieved by calling queue*sync from one of
  // the prev write callbacks, once all the write operations
  // we have to wait for are done
  void queueFsync(int fd, FileOpCallback&& cb);
  void queueFdatasync(int fd, FileOpCallback&& cb);

  void queueOpenat(
      int dfd, const char* path, int flags, mode_t mode, FileOpCallback&& cb);

  void queueOpenat2(
      int dfd, const char* path, open_how* how, FileOpCallback&& cb);

  void queueClose(int fd, FileOpCallback&& cb);

  void queueStatx(
      int dirfd,
      const char* pathname,
      int flags,
      unsigned int mask,
      struct statx* statxbuf,
      FileOpCallback&& cb);

  void queueRename(
      const char* oldPath, const char* newPath, FileOpCallback&& cb);

  void queueUnlinkat(
      int dirfd, const char* path, int flags, FileOpCallback&& cb);

  void queueUnlink(const char* path, FileOpCallback&& cb);

  void queueFallocate(
      int fd, int mode, off_t offset, off_t len, FileOpCallback&& cb);

  // sendmgs/recvmsg
  void queueSendmsg(
      int fd, const msghdr* msg, unsigned int flags, FileOpCallback&& cb);

  void queueRecvmsg(
      int fd, msghdr* msg, unsigned int flags, FileOpCallback&& cb);

  void submit(IoSqeBase& ioSqe) {
    // todo verify that the sqe is valid!
    submitImmediateIoSqe(ioSqe);
  }

  void submitNextLoop(IoSqeBase& ioSqe) noexcept;
  void submitSoon(IoSqeBase& ioSqe) noexcept;
  void submitNow(IoSqeBase& ioSqe);
  void cancel(IoSqeBase* sqe);

  // built in buffer provider
  IoUringBufferProvider* bufferProvider() {
    return bufferProviders_
        [bufferProviderIdx_++ & (bufferProviders_.size() - 1)]
            .get();
  }
  bool hasBufferProvider() { return !bufferProviders_.empty(); }
  uint16_t nextBufferProviderGid() { return bufferProviderGidNext_++; }
  IoUringZeroCopyBufferPool* zcBufferPool() { return zcBufferPool_.get(); }
  bool createZcBufferPool();
  bool importZcBufferPool(IoUringZeroCopyBufferPool::ExportHandle handle);
  IoUringZeroCopyBufferPool::ExportHandle exportZcBufferPool();

  static bool kernelZeroCopyRxSupported() {
    return IoUringZeroCopyBufferPool::kernelZeroCopyRxSupported();
  }

  bool canReceiveBuffer(bool useRecvmsg) {
    return (!useRecvmsg && zcBufferPool()) || hasBufferProvider();
  }

  struct CqeStats {
    uint64_t providedBufferCount{0};
    uint64_t bufMoreCount{0};
    uint64_t zeroCopyNotifCount{0};
    uint64_t zeroCopyNotifCopiedCount{0};

    auto operator<=>(const CqeStats&) const = default;
  };

  struct RingStats {
    uint32_t sqPeak{0};
    uint32_t cqPeak{0};
    uint32_t sqFullCount{0};
    uint32_t cqOverflowCount{0};

    auto operator<=>(const RingStats&) const = default;
  };

  struct IoUringStats {
    IoUringZeroCopyBufferPool::Stats zcrx;
    IoUringBufferProvider::Stats providedBuffer;
    CqeStats cqe;
    RingStats ring;

    auto operator<=>(const IoUringStats&) const = default;
  };

  CqeStats getCqeStats() { return std::exchange(cqeStats_, {}); }

  RingStats getRingStats() { return std::exchange(ringStats_, {}); }

  IoUringStats getStats() {
    IoUringStats stats;
    stats.cqe = getCqeStats();
    stats.ring = getRingStats();
    if (zcBufferPool_) {
      zcBufferPool_->getStats(stats.zcrx);
    }

    if (hasBufferProvider()) {
      IoUringBufferProvider* bufProvider =
          bufferProviders_[bufferProviderIdx_ & (bufferProviders_.size() - 1)]
              .get();
      bufProvider->getStats(stats.providedBuffer);
    }
    return stats;
  }

 protected:
  enum class WaitForEventsMode { WAIT, DONT_WAIT };

  class SocketPair {
   public:
    SocketPair();

    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;

    ~SocketPair();

    int readFd() const { return fds_[1]; }

    int writeFd() const { return fds_[0]; }

   private:
    std::array<int, 2> fds_{{-1, -1}};
  };

  static uint32_t getPollFlags(short events) {
    uint32_t ret = 0;
    if (events & EV_READ) {
      ret |= POLLIN;
    }

    if (events & EV_WRITE) {
      ret |= POLLOUT;
    }

    return ret;
  }

  static short getPollEvents(uint32_t flags, short events) {
    short ret = 0;
    if (flags & POLLIN) {
      ret |= EV_READ;
    }

    if (flags & POLLOUT) {
      ret |= EV_WRITE;
    }

    if (flags & (POLLERR | POLLHUP)) {
      ret |= (EV_READ | EV_WRITE);
    }

    ret &= events;

    return ret;
  }

  // timer processing
  bool addTimerFd();
  void scheduleTimeout();
  void scheduleTimeout(const std::chrono::microseconds& us);
  void addTimerEvent(Event& event, const timeval* timeout);
  void removeTimerEvent(Event& event);
  size_t processTimers();
  void setProcessTimers();

  size_t processActiveEvents();

  struct IoSqe;

  static void processPollIoSqe(
      IoUringBackend* backend, IoSqe* ioSqe, const io_uring_cqe* cqe);
  static void processTimerIoSqe(
      IoUringBackend* backend, IoSqe* /*sqe*/, const io_uring_cqe* /*cqe*/);
  static void processSignalReadIoSqe(
      IoUringBackend* backend, IoSqe* /*sqe*/, const io_uring_cqe* /*cqe*/);

  // signal handling
  void addSignalEvent(Event& event);
  void removeSignalEvent(Event& event);
  bool addSignalFds();
  size_t processSignals();
  void setProcessSignals();

  void processPollIo(IoSqe* ioSqe, int res, uint32_t flags) noexcept;

  IoSqe* FOLLY_NULLABLE allocIoSqe();
  void releaseIoSqe(IoSqe* aioIoSqe) noexcept;

  // submit immediate if POLL_SQ | POLL_SQ_IMMEDIATE_IO flags are set
  void submitImmediateIoSqe(IoSqeBase& ioSqe);

  void internalSubmit(IoSqeBase& ioSqe) noexcept;

  enum class InternalProcessCqeMode {
    NORMAL, // process existing and any available
    AVAILABLE_ONLY, // process existing but don't get more
    CANCEL_ALL, // cancel every sqe
  };
  unsigned int internalProcessCqe(
      unsigned int maxGet, InternalProcessCqeMode mode) noexcept;

  int eb_event_modify_inserted(Event& event, IoSqe* ioSqe);

  struct FdRegistry {
    FdRegistry() = delete;
    FdRegistry(io_uring& ioRing, uint32_t n);

    IoUringFdRegistrationRecord* alloc(int fd) noexcept;
    bool free(IoUringFdRegistrationRecord* record);

    int init();

    bool err_{false};
    io_uring& ioRing_;
    std::vector<int> files_;
    uint32_t inUse_;
    std::vector<IoUringFdRegistrationRecord> records_;
    IoUringFdRegistrationRecord::FreeList free_;
  };

  struct IoSqe : public IoSqeBase {
    using BackendCb = void(IoUringBackend*, IoSqe*, const io_uring_cqe*);
    explicit IoSqe(
        IoUringBackend* backend = nullptr,
        bool poolAlloc = false,
        bool persist = false)
        : backend_(backend), poolAlloc_(poolAlloc), persist_(persist) {}

    void callback(const io_uring_cqe* cqe) noexcept override {
      backendCb_(backend_, this, cqe);
    }
    void callbackCancelled(const io_uring_cqe*) noexcept override { release(); }
    virtual void release() noexcept;

    IoUringBackend* backend_;
    BackendCb* backendCb_{nullptr};
    const bool poolAlloc_;
    const bool persist_;
    Event* event_{nullptr};
    IoUringFdRegistrationRecord* fdRecord_{nullptr};
    size_t useCount_{0};
    int res_;
    uint32_t cqeFlags_;

    FOLLY_ALWAYS_INLINE void resetEvent() {
      // remove it from the list
      unlink();
      setEventBase(nullptr);
      if (event_) {
        event_->setUserData(nullptr);
        event_ = nullptr;
      }
    }

    void processSubmit(io_uring_sqe* sqe) noexcept override {
      prepPollAdd(
          sqe, event_->eb_ev_fd(), getPollFlags(event_->eb_ev_events()));
    }

    virtual void processActive() {}

    void prepPollAdd(io_uring_sqe* sqe, int fd, uint32_t events) noexcept {
      CHECK(sqe);
      ::io_uring_prep_poll_add(sqe, fd, events);
      ::io_uring_sqe_set_data(sqe, this);
    }

    void prepRead(
        io_uring_sqe* sqe,
        int fd,
        const iovec* iov,
        off_t offset,
        bool registerFd) noexcept {
      prepUtilFunc(
          ::io_uring_prep_read,
          sqe,
          registerFd,
          fd,
          iov->iov_base,
          static_cast<unsigned int>(iov->iov_len),
          offset);
    }

    void prepWrite(
        io_uring_sqe* sqe,
        int fd,
        const iovec* iov,
        off_t offset,
        bool registerFd) noexcept {
      prepUtilFunc(
          ::io_uring_prep_write,
          sqe,
          registerFd,
          fd,
          iov->iov_base,
          static_cast<unsigned int>(iov->iov_len),
          offset);
    }

    template <typename Fn, typename... Args>
    void prepUtilFunc(
        Fn fn, io_uring_sqe* sqe, bool registerFd, int fd, Args... args) {
      CHECK(sqe);
      if (registerFd && !fdRecord_) {
        fdRecord_ = backend_->registerFd(fd);
      }

      if (fdRecord_) {
        fn(sqe, fdRecord_->idx_, std::forward<Args>(args)...);
        sqe->flags |= IOSQE_FIXED_FILE;
      } else {
        fn(sqe, fd, std::forward<Args>(args)...);
      }

      ::io_uring_sqe_set_data(sqe, this);
    }

    void prepRecvmsgMultishot(io_uring_sqe* sqe, int fd, msghdr* msg) noexcept {
      CHECK(sqe);
      ::io_uring_prep_recvmsg_multishot(sqe, fd, msg, MSG_TRUNC);
      if (auto* bp = backend_->bufferProvider()) {
        sqe->buf_group = bp->gid();
        sqe->flags |= IOSQE_BUFFER_SELECT;
      }
      ::io_uring_sqe_set_data(sqe, this);
    }

    FOLLY_ALWAYS_INLINE void prepCancel(io_uring_sqe* sqe, IoSqe* cancel_sqe) {
      CHECK(sqe);
      ::io_uring_prep_cancel(sqe, cancel_sqe, 0);
      ::io_uring_sqe_set_data(sqe, this);
    }
  };

  using IoSqeBaseList = IntrusiveList<IoSqeBase, &IoSqeBase::ioSqeHook_>;

  struct FileOpIoSqe : public IoSqe {
    FileOpIoSqe(IoUringBackend* backend, int fd, FileOpCallback&& cb)
        : IoSqe(backend, false), fd_(fd), cb_(std::move(cb)) {}

    void processActive() override { cb_(res_); }

    int fd_{-1};

    FileOpCallback cb_;
  };

  struct ReadWriteIoSqe : public FileOpIoSqe {
    ReadWriteIoSqe(
        IoUringBackend* backend,
        int fd,
        const iovec* iov,
        off_t offset,
        FileOpCallback&& cb)
        : FileOpIoSqe(backend, fd, std::move(cb)),
          iov_(iov, iov + 1),
          offset_(offset) {}

    ReadWriteIoSqe(
        IoUringBackend* backend,
        int fd,
        Range<const iovec*> iov,
        off_t offset,
        FileOpCallback&& cb)
        : FileOpIoSqe(backend, fd, std::move(cb)), iov_(iov), offset_(offset) {}

    folly::small_vector<iovec> iov_;
    off_t offset_;
  };

  struct ReadIoSqe : public ReadWriteIoSqe {
    using ReadWriteIoSqe::ReadWriteIoSqe;

    void processSubmit(io_uring_sqe* sqe) noexcept override {
      prepRead(sqe, fd_, iov_.data(), offset_, false);
    }
  };

  struct WriteIoSqe : public ReadWriteIoSqe {
    using ReadWriteIoSqe::ReadWriteIoSqe;

    void processSubmit(io_uring_sqe* sqe) noexcept override {
      prepWrite(sqe, fd_, iov_.data(), offset_, false);
    }
  };

  struct ReadvIoSqe : public ReadWriteIoSqe {
    using ReadWriteIoSqe::ReadWriteIoSqe;

    void processSubmit(io_uring_sqe* sqe) noexcept override {
      ::io_uring_prep_readv(
          sqe,
          fd_,
          iov_.data(),
          static_cast<unsigned int>(iov_.size()),
          offset_);
      ::io_uring_sqe_set_data(sqe, this);
    }
  };

  struct WritevIoSqe : public ReadWriteIoSqe {
    using ReadWriteIoSqe::ReadWriteIoSqe;

    void processSubmit(io_uring_sqe* sqe) noexcept override {
      ::io_uring_prep_writev(
          sqe,
          fd_,
          iov_.data(),
          static_cast<unsigned int>(iov_.size()),
          offset_);
      ::io_uring_sqe_set_data(sqe, this);
    }
  };

  enum class FSyncFlags {
    FLAGS_FSYNC = 0,
    FLAGS_FDATASYNC = 1,
  };

  struct FSyncIoSqe : public FileOpIoSqe {
    FSyncIoSqe(
        IoUringBackend* backend, int fd, FSyncFlags flags, FileOpCallback&& cb)
        : FileOpIoSqe(backend, fd, std::move(cb)), flags_(flags) {}

    void processSubmit(io_uring_sqe* sqe) noexcept override {
      unsigned int fsyncFlags = 0;
      switch (flags_) {
        case FSyncFlags::FLAGS_FSYNC:
          fsyncFlags = 0;
          break;
        case FSyncFlags::FLAGS_FDATASYNC:
          fsyncFlags = IORING_FSYNC_DATASYNC;
          break;
      }

      ::io_uring_prep_fsync(sqe, fd_, fsyncFlags);
      ::io_uring_sqe_set_data(sqe, this);
    }

    FSyncFlags flags_;
  };

  struct FOpenAtIoSqe : public FileOpIoSqe {
    FOpenAtIoSqe(
        IoUringBackend* backend,
        int dfd,
        const char* path,
        int flags,
        mode_t mode,
        FileOpCallback&& cb)
        : FileOpIoSqe(backend, dfd, std::move(cb)),
          path_(path),
          flags_(flags),
          mode_(mode) {}

    void processSubmit(io_uring_sqe* sqe) noexcept override {
      ::io_uring_prep_openat(sqe, fd_, path_.c_str(), flags_, mode_);
      ::io_uring_sqe_set_data(sqe, this);
    }

    std::string path_;
    int flags_;
    mode_t mode_;
  };

  struct FOpenAt2IoSqe : public FileOpIoSqe {
    FOpenAt2IoSqe(
        IoUringBackend* backend,
        int dfd,
        const char* path,
        open_how* how,
        FileOpCallback&& cb)
        : FileOpIoSqe(backend, dfd, std::move(cb)), path_(path), how_(*how) {}

    void processSubmit(io_uring_sqe* sqe) noexcept override {
      ::io_uring_prep_openat2(sqe, fd_, path_.c_str(), &how_);
      ::io_uring_sqe_set_data(sqe, this);
    }

    std::string path_;
    open_how how_;
  };

  struct FCloseIoSqe : public FileOpIoSqe {
    using FileOpIoSqe::FileOpIoSqe;

    void processSubmit(io_uring_sqe* sqe) noexcept override {
      ::io_uring_prep_close(sqe, fd_);
      ::io_uring_sqe_set_data(sqe, this);
    }
  };

  struct FStatxIoSqe : public FileOpIoSqe {
    FStatxIoSqe(
        IoUringBackend* backend,
        int dfd,
        const char* pathname,
        int flags,
        unsigned int mask,
        struct statx* statxbuf,
        FileOpCallback&& cb)
        : FileOpIoSqe(backend, dfd, std::move(cb)),
          path_(pathname),
          flags_(flags),
          mask_(mask),
          statxbuf_(statxbuf) {}

    void processSubmit(io_uring_sqe* sqe) noexcept override {
      ::io_uring_prep_statx(sqe, fd_, path_, flags_, mask_, statxbuf_);
      ::io_uring_sqe_set_data(sqe, this);
    }

    const char* path_;
    int flags_;
    unsigned int mask_;
    struct statx* statxbuf_;
  };

  struct FRenameIoSqe : public FileOpIoSqe {
    FRenameIoSqe(
        IoUringBackend* backend,
        const char* oldPath,
        const char* newPath,
        FileOpCallback&& cb)
        : FileOpIoSqe(backend, -1, std::move(cb)),
          oldPath_(oldPath),
          newPath_(newPath) {}

    void processSubmit(io_uring_sqe* sqe) noexcept override {
      ::io_uring_prep_rename(sqe, oldPath_, newPath_);
      ::io_uring_sqe_set_data(sqe, this);
    }

    const char* oldPath_;
    const char* newPath_;
  };

  struct FUnlinkIoSqe : public FileOpIoSqe {
    FUnlinkIoSqe(
        IoUringBackend* backend,
        int dirfd,
        const char* path,
        int flags,
        FileOpCallback&& cb)
        : FileOpIoSqe(backend, dirfd, std::move(cb)),
          path_(path),
          flags_(flags) {}

    void processSubmit(io_uring_sqe* sqe) noexcept override {
      ::io_uring_prep_unlinkat(sqe, fd_, path_, flags_);
      ::io_uring_sqe_set_data(sqe, this);
    }

    const char* path_;
    int flags_;
  };

  struct FAllocateIoSqe : public FileOpIoSqe {
    FAllocateIoSqe(
        IoUringBackend* backend,
        int fd,
        int mode,
        off_t offset,
        off_t len,
        FileOpCallback&& cb)
        : FileOpIoSqe(backend, fd, std::move(cb)),
          mode_(mode),
          offset_(offset),
          len_(len) {}

    void processSubmit(io_uring_sqe* sqe) noexcept override {
      ::io_uring_prep_fallocate(sqe, fd_, mode_, offset_, len_);
      ::io_uring_sqe_set_data(sqe, this);
    }

    int mode_;
    off_t offset_;
    off_t len_;
  };

  struct SendmsgIoSqe : public FileOpIoSqe {
    SendmsgIoSqe(
        IoUringBackend* backend,
        int fd,
        const msghdr* msg,
        unsigned int flags,
        FileOpCallback&& cb)
        : FileOpIoSqe(backend, fd, std::move(cb)), msg_(msg), flags_(flags) {}

    void processSubmit(io_uring_sqe* sqe) noexcept override {
      ::io_uring_prep_sendmsg(sqe, fd_, msg_, flags_);
      ::io_uring_sqe_set_data(sqe, this);
    }

    const msghdr* msg_;
    unsigned int flags_;
  };

  struct RecvmsgIoSqe : public FileOpIoSqe {
    RecvmsgIoSqe(
        IoUringBackend* backend,
        int fd,
        msghdr* msg,
        unsigned int flags,
        FileOpCallback&& cb)
        : FileOpIoSqe(backend, fd, std::move(cb)), msg_(msg), flags_(flags) {}

    void processSubmit(io_uring_sqe* sqe) noexcept override {
      ::io_uring_prep_recvmsg(sqe, fd_, msg_, flags_);
      ::io_uring_sqe_set_data(sqe, this);
    }

    msghdr* msg_;
    unsigned int flags_;
  };

  struct RecvzcIoSqe : public ReadWriteIoSqe {
    using ReadWriteIoSqe::ReadWriteIoSqe;

    void processSubmit(io_uring_sqe* sqe) noexcept override {
      ::io_uring_prep_rw(
          IORING_OP_RECV_ZC,
          sqe,
          fd_,
          nullptr,
          static_cast<unsigned int>(iov_.data()->iov_len),
          0);
      ::io_uring_sqe_set_data(sqe, this);
      sqe->ioprio |= IORING_RECV_MULTISHOT;
      backend_->zcBufferPool_->sqePrepZc(sqe);
    }
  };

  size_t getActiveEvents(WaitForEventsMode waitForEvents);
  size_t prepList(IoSqeBaseList& ioSqes);
  int submitOne();
  int cancelOne(IoSqe* ioSqe);

  int submitBusyCheck(uint32_t num, WaitForEventsMode waitForEvents) noexcept;
  int submitEager();

  void queueFsync(int fd, FSyncFlags flags, FileOpCallback&& cb);

  void processFileOp(IoSqe* ioSqe, int res) noexcept;

  void processRecvZc(IoSqe* sqe, const io_uring_cqe* cqe) noexcept;

  static void processFileOpCB(
      IoUringBackend* backend, IoSqe* ioSqe, const io_uring_cqe* cqe) {
    backend->processFileOp(ioSqe, cqe->res);
  }

  static void processRecvZcCB(
      IoUringBackend* backend, IoSqe* ioSqe, const io_uring_cqe* cqe) {
    backend->processRecvZc(ioSqe, cqe);
  }

  IoUringBackend::IoSqe* allocNewIoSqe() {
    // allow pool alloc if poolAllocRemaining_ > 0
    bool poolAlloc = poolAllocRemaining_ > 0;
    auto* ret = new IoSqe(this, poolAlloc);
    if (poolAlloc) {
      --poolAllocRemaining_;
    }
    ret->backendCb_ = IoUringBackend::processPollIoSqe;

    return ret;
  }

  void cleanup();

  io_uring_sqe* getUntrackedSqe();
  io_uring_sqe* getSqe();
  void recordSqState() noexcept;
  void recordCqPeak() noexcept;
  void recordCqOverflow() noexcept;

  // Wait helpers
  int waitForRequestBatch(uint32_t numSendEvents) noexcept;
  int waitForFirstCompletionThenBatch(io_uring_cqe*& cqe) noexcept;
  int doInnerWait(io_uring_cqe*& cqe) noexcept;
  int doWait(io_uring_cqe*& cqe);
  int doPeek(io_uring_cqe*& cqe) noexcept;

  /// some ring calls require being called on a single system thread, so we need
  /// to delay init of those things until the correct thread is ready
  void delayedInit();

  /// init things that are linked to the io_uring submitter concept
  /// so for DeferTaskrun, only do this in delayed init
  void initSubmissionLinked();

  Options options_;
  uint32_t numEntries_;
  std::unique_ptr<IoSqe> timerEntry_;
  std::unique_ptr<IoSqe> signalReadEntry_;
  // Holds only IoSqe objects.
  IoSqeBaseList freeList_;
  bool usingDeferTaskrun_{false};
  int napiId_{-1};

  // timer related
  int timerFd_{-1};
  bool timerChanged_{false};
  bool timerSet_{false};
  std::multimap<std::chrono::steady_clock::time_point, Event*> timers_;

  // signal related
  SocketPair signalFds_;
  std::map<int, std::set<Event*>> signals_;

  // submit
  IoSqeBaseList submitList_;
  uint16_t bufferProviderGidNext_{0};
  std::vector<std::unique_ptr<IoUringBufferProvider>> bufferProviders_;
  uint64_t bufferProviderIdx_{0};
  IoUringZeroCopyBufferPool::UniquePtr zcBufferPool_;

  // loop related
  bool loopBreak_{false};
  bool shuttingDown_{false};
  bool processTimers_{false};
  bool processSignals_{false};
  // Holds only IoSqe objects.
  IoSqeBaseList activeEvents_;
  uint32_t waitingToSubmit_{0};
  uint32_t numInsertedEvents_{0};
  uint32_t numInternalEvents_{0};
  uint32_t numSendEvents_{0};
  // Sends submitted since the last CQ reap. Older sends that are blocked on
  // socket writability must not inflate the request batch target.
  uint32_t numSendEventsSinceReap_{0};
  CqeStats cqeStats_;
  RingStats ringStats_;

  // io_uring related
  io_uring_params params_{};
  io_uring ioRing_{};

  FdRegistry fdRegistry_;
  uint32_t poolAllocRemaining_{0};

  // poll callback to be invoked if POLL_CQ flag is set
  // every time we poll for a CQE
  CQPollLoopCallback cqPollLoopCallback_;

  bool needsDelayedInit_{true};

  // stuff for ensuring we don't re-enter submit/getActiveEvents
  folly::Optional<std::thread::id> submitTid_;
  int isSubmitting_{0};
  bool gettingEvents_{false};
  void dCheckSubmitTid();
  void setSubmitting() noexcept { isSubmitting_++; }
  void doneSubmitting() noexcept { isSubmitting_--; }
  bool isSubmitting() const noexcept { return isSubmitting_; }
};
} // namespace folly

#endif
