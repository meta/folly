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

#include <cstdint>

#include <folly/container/IntrusiveList.h>

namespace folly {

/**
 * Observes the execution of a task. Multiple execution observers can be chained
 * together. As a caveat, execution observers should not remove themselves from
 * the list of observers during execution
 */
class ExecutionObserver {
  IntrusiveListHook observerHook_;

 public:
  enum class CallbackType {
    // Owned by EventBase.
    Event,
    Loop,
    NotificationQueue,
    // Owned by FiberManager.
    Fiber,
  };
  // Defined after ExecutionObserver, which must be complete when the list
  // type names its hook.
  class List;

  virtual ~ExecutionObserver() = default;

  bool is_linked() const noexcept { return observerHook_.is_linked(); }
  void unlink() noexcept { observerHook_.unlink(); }

  /**
   * Called when a task is about to start executing.
   *
   * @param id Unique id for the task which is starting.
   */
  virtual void starting(uintptr_t id, CallbackType callbackType) noexcept = 0;

  /**
   * Called just after a task stops executing.
   *
   * @param id Unique id for the task which stopped.
   */
  virtual void stopped(uintptr_t id, CallbackType callbackType) noexcept = 0;
};

class ExecutionObserver::List
    : public IntrusiveList<
          ExecutionObserver,
          &ExecutionObserver::observerHook_> {};

class ExecutionObserverScopeGuard {
 public:
  ExecutionObserverScopeGuard(
      folly::ExecutionObserver::List* observerList,
      void* id,
      folly::ExecutionObserver::CallbackType callbackType);

  ~ExecutionObserverScopeGuard();

 private:
  folly::ExecutionObserver::List* observerList_;
  uintptr_t id_;
  folly::ExecutionObserver::CallbackType callbackType_;
};

} // namespace folly
