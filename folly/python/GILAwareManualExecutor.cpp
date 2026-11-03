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

#include <folly/python/GILAwareManualExecutor.h>

#include <folly/python/Weak.h>

#include <folly/ScopeGuard.h>

#if defined(__APPLE__)
#include <TargetConditionals.h> // @manual
#endif

namespace folly {
namespace python {
namespace {

// Releases the GIL while the executor waits for work. Unlocks the caller's
// lock before re-acquiring the GIL, to avoid deadlock if another GIL-owning
// thread is calling add. No-op where there is no interpreter.
class ReleaseGILWhileWaiting {
 public:
#if defined(__APPLE__) && (TARGET_OS_SIMULATOR || TARGET_OS_IPHONE)
  explicit ReleaseGILWhileWaiting(std::unique_lock<std::mutex>&) {}
#else
  explicit ReleaseGILWhileWaiting(std::unique_lock<std::mutex>& lock)
      : lock_(lock), pyThreadState_(PyEval_SaveThread()) {}
  ~ReleaseGILWhileWaiting() {
    lock_.unlock();
    PyEval_RestoreThread(pyThreadState_);
  }
  ReleaseGILWhileWaiting(const ReleaseGILWhileWaiting&) = delete;
  ReleaseGILWhileWaiting& operator=(const ReleaseGILWhileWaiting&) = delete;
  ReleaseGILWhileWaiting(ReleaseGILWhileWaiting&&) = delete;
  ReleaseGILWhileWaiting& operator=(ReleaseGILWhileWaiting&&) = delete;

 private:
  std::unique_lock<std::mutex>& lock_;
  PyThreadState* pyThreadState_;
#endif
};

} // namespace

GILAwareManualExecutor::~GILAwareManualExecutor() {
  while (keepAliveCount_.load(std::memory_order_relaxed)) {
    drive();
  }
  driveImpl();
}

void GILAwareManualExecutor::add(Func callback) {
  {
    std::lock_guard lock(lock_);
    funcs_.emplace(std::move(callback));
  }
  cv_.notify_one();
}

void GILAwareManualExecutor::waitBeforeDrive() {
  std::unique_lock lock(lock_);
  if (!funcs_.empty()) {
    return;
  }

  ReleaseGILWhileWaiting gilRelease(lock);
  cv_.wait(lock, [&] { return !funcs_.empty(); });
}

void GILAwareManualExecutor::driveImpl() {
  Func func;
  while (true) {
    {
      std::lock_guard lock(lock_);
      if (funcs_.empty()) {
        break;
      }

      func = std::move(funcs_.front());
      funcs_.pop();
    }
    func();
    func = nullptr;
  }
}

} // namespace python
} // namespace folly
