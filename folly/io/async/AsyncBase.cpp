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

#include <folly/io/async/AsyncBase.h>

#include <cerrno>
#include <ostream>
#include <stdexcept>
#include <string>

#include <glog/logging.h>

#include <folly/Conv.h>
#include <folly/Exception.h>
#include <folly/portability/Filesystem.h>
#include <folly/portability/Unistd.h>

namespace folly {

AsyncBaseOp::AsyncBaseOp(NotificationCallback cb)
    : cb_(std::move(cb)), state_(State::UNINITIALIZED), result_(-EINVAL) {}

void AsyncBaseOp::reset(NotificationCallback cb) {
  CHECK_NE(state_, State::PENDING);
  cb_ = std::move(cb);
  state_ = State::UNINITIALIZED;
  result_ = -EINVAL;
}

AsyncBaseOp::~AsyncBaseOp() {
  CHECK_NE(state_, State::PENDING);
}

void AsyncBaseOp::start() {
  DCHECK_EQ(state_, State::INITIALIZED);
  state_ = State::PENDING;
}

void AsyncBaseOp::unstart() {
  DCHECK_EQ(state_, State::PENDING);
  state_ = State::INITIALIZED;
}

void AsyncBaseOp::complete(ssize_t result) {
  DCHECK_EQ(state_, State::PENDING);
  state_ = State::COMPLETED;
  result_ = result;
  if (cb_) {
    cb_(this);
  }
}

void AsyncBaseOp::cancel() {
  DCHECK_EQ(state_, State::PENDING);
  state_ = State::CANCELED;
}

ssize_t AsyncBaseOp::result() const {
  CHECK_EQ(state_, State::COMPLETED);
  return result_;
}

void AsyncBaseOp::init() {
  CHECK_EQ(state_, State::UNINITIALIZED);
  state_ = State::INITIALIZED;
}

std::string AsyncBaseOp::fd2name(int fd) {
  auto link = fs::path{"/proc/self/fd"} / folly::to<std::string>(fd);
  return fs::read_symlink(link).string();
}

AsyncBase::AsyncBase(size_t capacity, PollMode pollMode)
    : capacity_(capacity), pollMode_(pollMode) {
  CHECK_GT(capacity_, 0);
  completed_.reserve(capacity_);
}

AsyncBase::~AsyncBase() {
  CHECK_EQ(pending_, 0);
  CHECK_EQ(pollFd_, -1);
}

void AsyncBase::decrementPending(size_t n) {
  auto p =
      pending_.fetch_add(static_cast<size_t>(-n), std::memory_order_acq_rel);
  DCHECK_GE(p, 1);
}

void AsyncBase::submit(Op* op) {
  CHECK_EQ(op->state(), Op::State::INITIALIZED);
  initializeContext(); // on demand

  // We can increment past capacity, but we'll clean up after ourselves.
  auto p = pending_.fetch_add(1, std::memory_order_acq_rel);
  if (p >= capacity_) {
    decrementPending();
    throw std::range_error("AsyncBase: too many pending requests");
  }

  op->start();
  int rc = submitOne(op);

  if (rc <= 0) {
    op->unstart();
    decrementPending();
    if (rc < 0) {
      throwSystemErrorExplicit(-rc, "AsyncBase: io_submit failed");
    }
  }
  submitted_ += rc;
  DCHECK_EQ(rc, 1);
}

int AsyncBase::submit(Range<Op**> ops) {
  for (auto& op : ops) {
    CHECK_EQ(op->state(), Op::State::INITIALIZED);
    op->start();
  }
  initializeContext(); // on demand

  // We can increment past capacity, but we'll clean up after ourselves.
  auto p = pending_.fetch_add(ops.size(), std::memory_order_acq_rel);
  if (p >= capacity_) {
    decrementPending(ops.size());
    throw std::range_error("AsyncBase: too many pending requests");
  }

  int rc = submitRange(ops);

  if (rc < 0) {
    decrementPending(ops.size());
    throwSystemErrorExplicit(-rc, "AsyncBase: io_submit failed");
  }
  // Any ops that did not get submitted go back to INITIALIZED state
  // and are removed from pending count.
  for (size_t i = rc; i < ops.size(); i++) {
    ops[i]->unstart();
    decrementPending(1);
  }
  submitted_ += rc;
  DCHECK_LE(rc, ops.size());

  return rc;
}

Range<AsyncBase::Op**> AsyncBase::wait(size_t minRequests) {
  CHECK(isInit());
  CHECK_EQ(pollFd_, -1) << "wait() only allowed on non-pollable object";
  auto p = pending_.load(std::memory_order_acquire);
  CHECK_LE(minRequests, p);
  return doWait(WaitType::COMPLETE, minRequests, p, completed_);
}

Range<AsyncBase::Op**> AsyncBase::cancel() {
  CHECK(isInit());
  auto p = pending_.load(std::memory_order_acquire);
  return doWait(WaitType::CANCEL, p, p, canceled_);
}

Range<AsyncBase::Op**> AsyncBase::pollCompleted() {
  CHECK(isInit());
  CHECK_NE(pollFd_, -1) << "pollCompleted() only allowed on pollable object";

  if (drainPollFd() <= 0) {
    return Range<Op**>(); // nothing completed
  }

  // Don't reap more than pending_, as we've just reset the counter to 0.
  return doWait(WaitType::COMPLETE, 0, pending_.load(), completed_);
}

AsyncBaseQueue::AsyncBaseQueue(AsyncBase* asyncBase) : asyncBase_(asyncBase) {}

AsyncBaseQueue::~AsyncBaseQueue() {
  CHECK_EQ(asyncBase_->pending(), 0);
}

void AsyncBaseQueue::submit(AsyncBaseOp* op) {
  submit([op]() { return op; });
}

void AsyncBaseQueue::submit(OpFactory op) {
  queue_.push_back(op);
  maybeDequeue();
}

void AsyncBaseQueue::onCompleted(AsyncBaseOp* /* op */) {
  maybeDequeue();
}

void AsyncBaseQueue::maybeDequeue() {
  while (!queue_.empty() && asyncBase_->pending() < asyncBase_->capacity()) {
    auto& opFactory = queue_.front();
    auto op = opFactory();
    queue_.pop_front();

    // Interpose our completion callback
    auto nextCb = op->getNotificationCallback();
    op->setNotificationCallback(
        [this, nextCb{std::move(nextCb)}](AsyncBaseOp* op2) mutable {
          this->onCompleted(op2);
          if (nextCb) {
            nextCb(op2);
          }
        });

    asyncBase_->submit(op);
  }
}

// debugging helpers:

namespace {

#define X(c) \
  case c:    \
    return #c

const char* asyncIoOpStateToString(AsyncBaseOp::State state) {
  switch (state) {
    X(AsyncBaseOp::State::UNINITIALIZED);
    X(AsyncBaseOp::State::INITIALIZED);
    X(AsyncBaseOp::State::PENDING);
    X(AsyncBaseOp::State::COMPLETED);
    X(AsyncBaseOp::State::CANCELED);
  }
  return "<INVALID AsyncBaseOp::State>";
}
#undef X
} // namespace

std::ostream& operator<<(std::ostream& os, const AsyncBaseOp& op) {
  op.toStream(os);
  return os;
}

std::ostream& operator<<(std::ostream& os, AsyncBaseOp::State state) {
  return os << asyncIoOpStateToString(state);
}

} // namespace folly
