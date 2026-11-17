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

#include <folly/executors/testing/SimpleThreadPoolExecutor.h>

using namespace std;

namespace folly::test {

SimpleThreadPoolExecutor::SimpleThreadPoolExecutor(size_t numThreads) {
  const auto kWorkers = std::max(size_t(1), numThreads);
  for (auto idx = 0u; idx < kWorkers; ++idx) {
    workers_.emplace_back([this] {
      while (true) {
        Func work;
        {
          unique_lock<mutex> lk(m_);
          cv_.wait(lk, [this] { return !workItems_.empty(); });
          work = std::move(workItems_.front());
          workItems_.pop();
        }
        if (!work) {
          break;
        }
        work();
      }
    });
  }
}

SimpleThreadPoolExecutor::~SimpleThreadPoolExecutor() {
  for (auto& worker : workers_) {
    (void)worker;
    addImpl({});
  }

  for (auto& worker : workers_) {
    worker.join();
  }
}

void SimpleThreadPoolExecutor::add(Func f) {
  if (f) {
    addImpl(std::move(f));
  }
}

size_t SimpleThreadPoolExecutor::numThreads() const {
  return workers_.size();
}

void SimpleThreadPoolExecutor::addImpl(Func f) {
  {
    lock_guard<mutex> g(m_);
    workItems_.push(std::move(f));
  }
  cv_.notify_one();
}

} // namespace folly::test
