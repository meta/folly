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

#include <folly/coro/SerialQueueRunner.h>

#include <stdexcept>
#include <utility>

#if FOLLY_HAS_COROUTINES

namespace folly::coro {

void SerialQueueRunner::add(Work task) {
  std::unique_lock lock{mut_};
  if (done_) {
    throw std::runtime_error("add after done");
  }
  tasks_.push_back(std::move(task));
  if (baton_) {
    baton_->post();
  }
}

void SerialQueueRunner::done() {
  std::unique_lock lock{mut_};
  if (done_) {
    throw std::runtime_error("add after done");
  }
  done_ = true;
  if (baton_) {
    baton_->post();
  }
}

Task<> SerialQueueRunner::run() {
  if (running_.exchange(true, std::memory_order_relaxed)) {
    co_yield co_error{
        make_exception_wrapper<std::runtime_error>("multiple calls to run")};
  }
  while (true) {
    auto [done, tasks] = co_await pull();
    for (auto& task : tasks) {
      auto res = co_await co_awaitTry(std::move(task));
      exception_wrapper exn =
          res.hasException() ? std::move(res.exception()) : exception_wrapper();
      if (!exn_ && exn && !exn.get_exception<OperationCancelled>()) {
        exn_ = std::move(exn);
      }
    }
    if (done) {
      break;
    }
  }
  if (exn_) {
    co_yield co_error{std::move(exn_)};
  }
}

void SerialQueueRunner::cancel() {
  std::unique_lock lock{mut_};
  if (!done_) {
    done();
  }
}

Task<> SerialQueueRunner::await() {
  CancellationCallback cb{
      co_await co_current_cancellation_token, [this] { cancel(); }};
  co_await *baton_;
}

Task<SerialQueueRunner::PullResult> SerialQueueRunner::pull() {
  std::unique_lock lock{mut_};
  if (!done_ && tasks_.empty()) {
    folly::coro::Baton baton;
    baton_ = &baton;
    lock.unlock();
    co_await await();
    lock.lock();
    baton_ = nullptr;
  }
  co_return std::pair{done_, std::move(tasks_)};
}

} // namespace folly::coro

#endif
