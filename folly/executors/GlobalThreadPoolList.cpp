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

#include <folly/executors/GlobalThreadPoolList.h>
#include <folly/system/ThreadId.h>

#include <memory>
#include <string>
#include <vector>

#include <folly/CppAttributes.h>
#include <folly/Indestructible.h>
#include <folly/Synchronized.h>
#include <folly/ThreadLocal.h>

namespace folly {

namespace debugger_detail {

class ThreadListHook {
 public:
  ThreadListHook(
      ThreadPoolListHook* poolId,
      std::thread::id threadId,
      uint64_t osThreadId);
  ~ThreadListHook();

 private:
  ThreadListHook() = default;
  ThreadPoolListHook* poolId_;
  std::thread::id threadId_;
  uint64_t osThreadId_;
};

class GlobalThreadPoolListImpl {
 public:
  GlobalThreadPoolListImpl() = default;

  void registerThreadPool(ThreadPoolListHook* threadPoolId, std::string name);

  void unregisterThreadPool(ThreadPoolListHook* threadPoolId);

  void registerThreadPoolThread(
      ThreadPoolListHook* threadPoolId,
      std::thread::id threadId,
      uint64_t osThreadId);

  void unregisterThreadPoolThread(
      ThreadPoolListHook* threadPoolId, std::thread::id threadId);

 private:
  struct PoolInfo {
    ThreadPoolListHook* addr;
    std::string name;
    std::vector<std::thread::id> threads;
    // separate data structure for backwards compatibility
    std::vector<uint64_t> osThreadIds;
  };

  struct Pools {
    // Just a vector since ease of access from gdb is the most important
    // property
    std::vector<PoolInfo> poolsInfo_;

    PoolInfo* FOLLY_NULLABLE getPool(void* threadPoolId) {
      for (auto& elem : vector()) {
        if (elem.addr == threadPoolId) {
          return &elem;
        }
      }

      return nullptr;
    }

    std::vector<PoolInfo>& vector() { return poolsInfo_; }
  };

  Pools pools_;
};

class GlobalThreadPoolList {
 public:
  GlobalThreadPoolList() noexcept { debug = this; }

  static GlobalThreadPoolList& instance();

  void registerThreadPool(ThreadPoolListHook* threadPoolId, std::string name);

  void unregisterThreadPool(ThreadPoolListHook* threadPoolId);

  void registerThreadPoolThread(
      ThreadPoolListHook* threadPoolId,
      std::thread::id threadId,
      uint64_t osThreadId);

  void unregisterThreadPoolThread(
      ThreadPoolListHook* threadPoolId, std::thread::id threadId);

  GlobalThreadPoolList(GlobalThreadPoolList const&) = delete;
  void operator=(GlobalThreadPoolList const&) = delete;

 private:
  // Make instance() available to the debugger
  static GlobalThreadPoolList* debug;

  folly::Synchronized<GlobalThreadPoolListImpl> globalListImpl_;
  folly::ThreadLocalPtr<ThreadListHook> threadHook_;
};

GlobalThreadPoolList* GlobalThreadPoolList::debug;

GlobalThreadPoolList& GlobalThreadPoolList::instance() {
  static folly::Indestructible<GlobalThreadPoolList> ret;
  return *ret;
}

void GlobalThreadPoolList::registerThreadPool(
    ThreadPoolListHook* threadPoolId, std::string name) {
  globalListImpl_.wlock()->registerThreadPool(threadPoolId, std::move(name));
}

void GlobalThreadPoolList::unregisterThreadPool(
    ThreadPoolListHook* threadPoolId) {
  globalListImpl_.wlock()->unregisterThreadPool(threadPoolId);
}

void GlobalThreadPoolList::registerThreadPoolThread(
    ThreadPoolListHook* threadPoolId,
    std::thread::id threadId,
    uint64_t osThreadId) {
  DCHECK(!threadHook_);
  threadHook_.reset(
      std::make_unique<ThreadListHook>(threadPoolId, threadId, osThreadId));

  globalListImpl_.wlock()->registerThreadPoolThread(
      threadPoolId, threadId, osThreadId);
}

void GlobalThreadPoolList::unregisterThreadPoolThread(
    ThreadPoolListHook* threadPoolId, std::thread::id threadId) {
  (void)threadPoolId;
  (void)threadId;
  globalListImpl_.wlock()->unregisterThreadPoolThread(threadPoolId, threadId);
}

void GlobalThreadPoolListImpl::registerThreadPool(
    ThreadPoolListHook* threadPoolId, std::string name) {
  PoolInfo info;
  info.name = std::move(name);
  info.addr = threadPoolId;
  pools_.vector().push_back(std::move(info));
}

void GlobalThreadPoolListImpl::unregisterThreadPool(
    ThreadPoolListHook* threadPoolId) {
  auto& vector = pools_.vector();
  std::erase_if(vector, [=](PoolInfo& i) { return i.addr == threadPoolId; });
}

void GlobalThreadPoolListImpl::registerThreadPoolThread(
    ThreadPoolListHook* threadPoolId,
    std::thread::id threadId,
    uint64_t osThreadId) {
  auto* pool = pools_.getPool(threadPoolId);
  if (pool == nullptr) {
    return;
  }

  auto& threads = pool->threads;
  auto& osids = pool->osThreadIds;
  threads.push_back(threadId);
  osids.push_back(osThreadId);
}

void GlobalThreadPoolListImpl::unregisterThreadPoolThread(
    ThreadPoolListHook* threadPoolId, std::thread::id threadId) {
  auto* pool = pools_.getPool(threadPoolId);
  if (pool == nullptr) {
    return;
  }

  auto& threads = pool->threads;
  auto& osids = pool->osThreadIds;
  DCHECK_EQ(threads.size(), osids.size());
  for (unsigned i = 0; i < threads.size(); ++i) {
    if (threads[i] == threadId) {
      threads.erase(threads.begin() + i);
      osids.erase(osids.begin() + i);
      break;
    }
  }
}

ThreadListHook::ThreadListHook(
    ThreadPoolListHook* poolId, std::thread::id threadId, uint64_t osThreadId) {
  poolId_ = poolId;
  threadId_ = threadId;
  osThreadId_ = osThreadId;
}

ThreadListHook::~ThreadListHook() {
  GlobalThreadPoolList::instance().unregisterThreadPoolThread(
      poolId_, threadId_);
}

} // namespace debugger_detail

ThreadPoolListHook::ThreadPoolListHook(std::string name) {
  debugger_detail::GlobalThreadPoolList::instance().registerThreadPool(
      this, std::move(name));
}

ThreadPoolListHook::~ThreadPoolListHook() {
  debugger_detail::GlobalThreadPoolList::instance().unregisterThreadPool(this);
}

void ThreadPoolListHook::registerThread() {
  uint64_t ostid = folly::getOSThreadID();
  debugger_detail::GlobalThreadPoolList::instance().registerThreadPoolThread(
      this, std::this_thread::get_id(), ostid);
}

} // namespace folly
