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

#include <folly/SharedMutex.h>
#include <folly/io/async/AsyncBase.h>
#include <folly/io/async/Liburing.h>

#if FOLLY_HAS_LIBURING

#include <liburing.h> // @manual

namespace folly {

/**
 * An IoUringOp represents a pending operation.  You may set a notification
 * callback or you may use this class's methods directly.
 *
 * The op must remain allocated until it is completed or canceled.
 */
class IoUringOp : public AsyncBaseOp {
  friend class IoUring;
  friend std::ostream& operator<<(std::ostream& stream, const IoUringOp& o);

 public:
  struct Options {
    Options() : sqe128(false), cqe32(false) {}
    bool sqe128;
    bool cqe32;

    bool operator==(const Options& options) const {
      return sqe128 == options.sqe128 && cqe32 == options.cqe32;
    }
  };

  IoUringOp(
      NotificationCallback cb = NotificationCallback(),
      Options options = Options());
  IoUringOp(const IoUringOp&) = delete;
  IoUringOp& operator=(const IoUringOp&) = delete;
  ~IoUringOp() override;

  /**
   * Initiate a read request.
   */
  void pread(int fd, void* buf, size_t size, off_t start) override;
  void preadv(int fd, const iovec* iov, int iovcnt, off_t start) override;
  void pread(
      int fd, void* buf, size_t size, off_t start, int buf_index) override;

  /**
   * Initiate a write request.
   */
  void pwrite(int fd, const void* buf, size_t size, off_t start) override;
  void pwritev(int fd, const iovec* iov, int iovcnt, off_t start) override;
  void pwrite(int fd, const void* buf, size_t size, off_t start, int buf_index)
      override;

  void reset(NotificationCallback cb = NotificationCallback()) override;

  AsyncIOOp* getAsyncIOOp() override { return nullptr; }

  IoUringOp* getIoUringOp() override { return this; }

  void toStream(std::ostream& os) const override;

  void initBase() { init(); }

  struct io_uring_sqe& getSqe() { return sqe_.sqe; }

  size_t getSqeSize() const {
    return options_.sqe128 ? 128 : sizeof(struct io_uring_sqe);
  }

  const struct io_uring_cqe& getCqe() const {
    return *reinterpret_cast<const struct io_uring_cqe*>(&cqe_);
  }

  size_t getCqeSize() const {
    return options_.cqe32 ? 32 : sizeof(struct io_uring_cqe);
  }

  void setCqe(const struct io_uring_cqe* cqe) {
    ::memcpy(&cqe_, cqe, getCqeSize());
  }

  const Options& getOptions() const { return options_; }

 private:
  Options options_;

  // we use unions with the largest size to avoid
  // indidual allocations for the sqe/cqe
  union {
    struct io_uring_sqe sqe;
    uint8_t data[128];
  } sqe_ = {};

  // we have to use a union here because of -Wgnu-variable-sized-type-not-at-end
  //__u64 big_cqe[];
  union {
    __u64 user_data; // first member from from io_uring_cqe
    uint8_t data[32];
  } cqe_ = {};

  struct iovec iov_[1];
};

std::ostream& operator<<(std::ostream& stream, const IoUringOp& op);

/**
 * C++ interface around Linux io_uring
 */
class IoUring : public AsyncBase {
 public:
  using Op = IoUringOp;

  /**
   * Note: the maximum number of allowed concurrent requests is controlled
   * by the kernel IORING_MAX_ENTRIES and the memlock limit,
   * The default IORING_MAX_ENTRIES value is usually 32K.
   */
  explicit IoUring(
      size_t capacity,
      PollMode pollMode = NOT_POLLABLE,
      size_t maxSubmit = 1,
      IoUringOp::Options options = IoUringOp::Options());
  IoUring(const IoUring&) = delete;
  IoUring& operator=(const IoUring&) = delete;
  ~IoUring() override;

  static bool isAvailable();

  const IoUringOp::Options& getOptions() const { return options_; }

  int register_buffers(const struct iovec* iovecs, unsigned int nr_iovecs);

  int unregister_buffers();

  void initializeContext() override;

 protected:
  int drainPollFd() override;
  int submitOne(AsyncBase::Op* op) override;
  int submitRange(Range<AsyncBase::Op**> ops) override;

 private:
  Range<AsyncBase::Op**> doWait(
      WaitType type,
      size_t minRequests,
      size_t maxRequests,
      std::vector<AsyncBase::Op*>& result) override;

  size_t maxSubmit_;
  IoUringOp::Options options_;
  struct io_uring_params params_;
  struct io_uring ioRing_;
  mutable SharedMutex submitMutex_;
};

using IoUringQueue = AsyncBaseQueue;
} // namespace folly

#endif
