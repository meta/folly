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

#include <folly/container/IntrusiveList.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/DelayedDestruction.h>

struct io_uring_sqe;
struct io_uring_cqe;

namespace folly {

class IoUringBackend;
class EventBase;

struct IoSqeBase {
  enum class Type {
    Unknown,
    Read,
    Write,
    Open,
    Close,
    Connect,
    Cancel,
  };

  IoSqeBase() : IoSqeBase(Type::Unknown) {}
  explicit IoSqeBase(Type type) : type_(type) {}
  // use raw addresses, so disallow copy/move
  IoSqeBase(IoSqeBase&&) = delete;
  IoSqeBase(const IoSqeBase&) = delete;
  IoSqeBase& operator=(IoSqeBase&&) = delete;
  IoSqeBase& operator=(const IoSqeBase&) = delete;

  virtual ~IoSqeBase() = default;
  virtual void processSubmit(struct io_uring_sqe* sqe) noexcept = 0;
  virtual void callback(const io_uring_cqe* cqe) noexcept = 0;
  virtual void callbackCancelled(const io_uring_cqe* cqe) noexcept = 0;
  IoSqeBase::Type type() const { return type_; }
  bool inFlight() const { return inFlight_; }
  bool cancelled() const { return cancelled_; }
  void markCancelled() { cancelled_ = true; }
  void setEventBase(EventBase* evb) { evb_ = evb; }

  bool is_linked() const noexcept { return ioSqeHook_.is_linked(); }
  void unlink() noexcept { ioSqeHook_.unlink(); }

 protected:
  // This is used if you want to prepare this sqe for reuse, but will manage the
  // lifetime. For example for zerocopy send, you might want to reuse the sqe
  // but still have a notification inbound.
  void prepareForReuse() { internalMarkInflight(false); }
  void internalMarkInflight(bool val) { inFlight_ = val; }

 private:
  friend class IoUringBackend;
  void internalSubmit(struct io_uring_sqe* sqe) noexcept;
  void internalCallback(const io_uring_cqe* cqe) noexcept;

  IntrusiveListHook ioSqeHook_;
  bool inFlight_ = false;
  bool cancelled_ = false;
  EventBase* evb_ = nullptr;
  Type type_;
};

struct IoUringFdRegistrationRecord {
 private:
  SafeIntrusiveListHook freeListHook_;

 public:
  using FreeList = SafeIntrusiveList<
      IoUringFdRegistrationRecord,
      &IoUringFdRegistrationRecord::freeListHook_>;

  int count_{0};
  int fd_{-1};
  int idx_{0};
};

} // namespace folly
