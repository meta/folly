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

#include <folly/Executor.h>

#include <stdexcept>

#include <glog/logging.h>

#include <folly/ExceptionString.h>
#include <folly/Portability.h>
#include <folly/lang/Exception.h>

namespace folly {

bool setKeepAliveObjTraceHooks(
    detail::KeepAliveObjTraceHook acquire,
    detail::KeepAliveObjTraceHook release,
    detail::KeepAliveObjMoveHook move) {
  detail::keepAliveObjAcquireHook().store(acquire, std::memory_order_relaxed);
  detail::keepAliveObjReleaseHook().store(release, std::memory_order_relaxed);
  detail::keepAliveObjMoveHook().store(move, std::memory_order_relaxed);
  return FOLLY_ENABLE_KEEPALIVE_OBJ_TRACE != 0;
}

void Executor::invokeCatchingExnsLog(char const* const prefix) noexcept {
  auto ep = current_exception();
  LOG(ERROR) << prefix << " threw unhandled " << exceptionStr(ep);
}

void Executor::addWithPriority(Func, int8_t /* priority */) {
  throw std::runtime_error(
      "addWithPriority() is not implemented for this Executor");
}

bool Executor::keepAliveAcquire() noexcept {
  return false;
}

void Executor::keepAliveRelease() noexcept {
  LOG(FATAL) << __func__ << "() should not be called for folly::Executor types "
             << "which do not override keepAliveAcquire()";
}

// Base case of permitting with no termination to avoid nullptr tests
static ExecutorBlockingList emptyList{nullptr, {false, false, nullptr, {}}};

thread_local ExecutorBlockingList* executor_blocking_list = &emptyList;

Optional<ExecutorBlockingContext> getExecutorBlockingContext() noexcept {
  return //
      kIsMobile || !executor_blocking_list->curr.forbid ? none : //
      make_optional(executor_blocking_list->curr);
}

ExecutorBlockingGuard::ExecutorBlockingGuard(PermitTag) noexcept {
  if (!kIsMobile) {
    list_ = *executor_blocking_list;
    list_.prev = executor_blocking_list;
    list_.curr.forbid = false;
    // Do not overwrite tag or executor pointer
    executor_blocking_list = &list_;
  }
}

ExecutorBlockingGuard::ExecutorBlockingGuard(
    TrackTag, Executor* ex, std::string_view tag) noexcept {
  if (!kIsMobile) {
    list_ = *executor_blocking_list;
    list_.prev = executor_blocking_list;
    list_.curr.forbid = true;
    list_.curr.ex = ex;
    // If no string was provided, maintain the parent string to keep some
    // information
    if (!tag.empty()) {
      list_.curr.tag = tag;
    }
    executor_blocking_list = &list_;
  }
}

ExecutorBlockingGuard::ExecutorBlockingGuard(
    ProhibitTag, Executor* ex, std::string_view tag) noexcept {
  if (!kIsMobile) {
    list_ = *executor_blocking_list;
    list_.prev = executor_blocking_list;
    list_.curr.forbid = true;
    list_.curr.ex = ex;
    list_.curr.allowTerminationOnBlocking = true;
    // If no string was provided, maintain the parent string to keep some
    // information
    if (!tag.empty()) {
      list_.curr.tag = tag;
    }
    executor_blocking_list = &list_;
  }
}

ExecutorBlockingGuard::~ExecutorBlockingGuard() {
  if (!kIsMobile) {
    if (executor_blocking_list != &list_) {
      terminate_with<std::logic_error>("dtor mismatch");
    }
    executor_blocking_list = list_.prev;
  }
}

} // namespace folly
