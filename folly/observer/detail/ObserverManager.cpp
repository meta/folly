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

#include <fmt/format.h>
#include <folly/observer/detail/ObserverManager.h>

#include <future>

#include <folly/ExceptionString.h>
#include <folly/Range.h>
#include <folly/Singleton.h>
#include <folly/concurrency/UnboundedQueue.h>
#include <folly/portability/GFlags.h>
#include <folly/system/ThreadName.h>

namespace folly {
namespace observer_detail {

#if !defined(__MACH__)
thread_local constinit bool ObserverManager::inManagerThread_{false};
#else
thread_local bool ObserverManager::inManagerThread_{false};
#endif
thread_local ObserverManager::DependencyRecorder::Dependencies*
    ObserverManager::DependencyRecorder::currentDependencies_{nullptr};

FOLLY_GFLAGS_DEFINE_int32(
    observer_manager_pool_size,
    4,
    "How many internal threads ObserverManager should use");

namespace {
constexpr StringPiece kObserverManagerThreadNamePrefix{"ObserverMngr"};
constexpr size_t kNextBatchSize{1024};

auto& currentQueue() {
  static folly::Indestructible<UMPMCQueue<Function<void()>, true>> instance;
  return *instance;
}

auto& nextQueue() {
  static folly::Indestructible<UMPSCQueue<Function<Core::Ptr()>, true>>
      instance;
  return *instance;
}
} // namespace

class ObserverManager::UpdatesManager::CurrentQueueProcessor {
 public:
  CurrentQueueProcessor() {
    if (FLAGS_observer_manager_pool_size < 1) {
      LOG(ERROR) << "--observer_manager_pool_size should be >= 1";
      FLAGS_observer_manager_pool_size = 1;
    }
    threads_.reserve(FLAGS_observer_manager_pool_size);
    for (int32_t i = 0; i < FLAGS_observer_manager_pool_size; ++i) {
      threads_.emplace_back([this, i]() {
        folly::setThreadName(
            fmt::format("{}{}", kObserverManagerThreadNamePrefix, i));
        ObserverManager::inManagerThread_ = true;

        while (true) {
          Function<void()> task;
          queue_.dequeue(task);

          if (!task) {
            return;
          }

          try {
            task();
          } catch (...) {
            LOG(ERROR) << "Exception while running CurrentQueue task: "
                       << exceptionStr(current_exception());
          }
        }
      });
    }
  }

  CurrentQueueProcessor(const CurrentQueueProcessor&) = delete;
  CurrentQueueProcessor& operator=(const CurrentQueueProcessor&) = delete;
  CurrentQueueProcessor(CurrentQueueProcessor&&) = delete;
  CurrentQueueProcessor& operator=(CurrentQueueProcessor&&) = delete;

  ~CurrentQueueProcessor() {
    for (size_t i = 0; i < threads_.size(); ++i) {
      queue_.enqueue(nullptr);
    }

    for (auto& thread : threads_) {
      thread.join();
    }
  }

 private:
  UMPMCQueue<Function<void()>, true>& queue_{currentQueue()};
  std::vector<std::thread> threads_;
};

class ObserverManager::UpdatesManager::NextQueueProcessor {
 public:
  NextQueueProcessor() {
    thread_ = std::thread([&]() {
      auto& manager = getInstance();

      folly::setThreadName(
          fmt::format("{}NQ", kObserverManagerThreadNamePrefix));

      Function<Core::Ptr()> queueCoreFunc;

      while (!stop_) {
        std::vector<Core::Ptr> cores;
        queue_.dequeue(queueCoreFunc);
        do {
          {
            if (auto queueCore = queueCoreFunc()) {
              cores.emplace_back(std::move(queueCore));
            }
          }
        } while (!stop_ && cores.size() < kNextBatchSize &&
                 queue_.try_dequeue(queueCoreFunc));

        {
          std::unique_lock wh(manager.versionMutex_);

          // We can't pick more tasks from the queue after we bumped the
          // version, so we have to do this while holding the lock.

          for (auto& corePtr : cores) {
            corePtr->setForceRefresh();
          }

          ++manager.version_;
        }

        for (auto& core : cores) {
          manager.scheduleRefresh(std::move(core), manager.version_);
        }

        {
          auto wEmptyWaiters = emptyWaiters_.wlock();
          // We don't want any new waiters to be added while we are checking the
          // queue.
          if (queue_.empty()) {
            for (auto& promise : *wEmptyWaiters) {
              promise.set_value();
            }
            wEmptyWaiters->clear();
          }
        }
      }
    });
  }

  NextQueueProcessor(const NextQueueProcessor&) = delete;
  NextQueueProcessor& operator=(const NextQueueProcessor&) = delete;
  NextQueueProcessor(NextQueueProcessor&&) = delete;
  NextQueueProcessor& operator=(NextQueueProcessor&&) = delete;

  ~NextQueueProcessor() {
    stop_ = true;
    // Write to the queue to notify the thread.
    queue_.enqueue([]() -> Core::Ptr { return nullptr; });
    thread_.join();
  }

  void waitForEmpty() {
    std::promise<void> promise;
    auto future = promise.get_future();
    emptyWaiters_.wlock()->push_back(std::move(promise));

    // Write to the queue to notify the thread.
    queue_.enqueue([]() -> Core::Ptr { return nullptr; });

    future.get();
  }

 private:
  UMPSCQueue<Function<Core::Ptr()>, true>& queue_{nextQueue()};
  std::thread thread_;
  std::atomic<bool> stop_{false};
  folly::Synchronized<std::vector<std::promise<void>>> emptyWaiters_;
};

ObserverManager::UpdatesManager::UpdatesManager() {
  currentQueueProcessor_ = std::make_unique<CurrentQueueProcessor>();
  nextQueueProcessor_ = std::make_unique<NextQueueProcessor>();
}

void ObserverManager::scheduleCurrent(Function<void()> task) {
  currentQueue().enqueue(std::move(task));
  getUpdatesManager();
}

void ObserverManager::scheduleNext(Function<Core::Ptr()> coreFunc) {
  nextQueue().enqueue(std::move(coreFunc));
  getUpdatesManager();
}

bool ObserverManager::tryWaitForAllUpdatesImpl(TryWaitForAllUpdatesImplOp op) {
  if (auto updatesManager = getUpdatesManager()) {
    return updatesManager->tryWaitForAllUpdatesImpl(op);
  }
  return true;
}

bool ObserverManager::UpdatesManager::tryWaitForAllUpdatesImpl(
    TryWaitForAllUpdatesImplOp op) {
  auto& instance = ObserverManager::getInstance();
  nextQueueProcessor_->waitForEmpty();
  // Wait for all readers to release the lock.
  return op(instance.versionMutex_).owns_lock();
}

struct ObserverManager::Singleton {
  static folly::Singleton<UpdatesManager> instance;
  // MSVC 2015 doesn't let us access ObserverManager's constructor if we
  // try to use a lambda to initialize instance, so we have to create
  // an actual function instead.
  static UpdatesManager* createManager() { return new UpdatesManager(); }
};

folly::Singleton<ObserverManager::UpdatesManager>
    ObserverManager::Singleton::instance =
        folly::Singleton<ObserverManager::UpdatesManager>(createManager)
            .shouldEagerInitOnReenable();

std::shared_ptr<ObserverManager::UpdatesManager>
ObserverManager::getUpdatesManager() {
  return Singleton::instance.try_get();
}

ObserverManager& ObserverManager::getInstance() {
  static auto instance = new ObserverManager();
  return *instance;
}
} // namespace observer_detail
} // namespace folly
