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

#include <chrono>

#include <folly/io/async/TimeoutManager.h>
#include <folly/portability/GMock.h>

namespace folly {
namespace test {

class MockTimeoutManager : public folly::TimeoutManager {
 public:
  MOCK_METHOD(
      void,
      attachTimeoutManager,
      (folly::AsyncTimeout*, folly::TimeoutManager::InternalEnum));

  MOCK_METHOD(void, detachTimeoutManager, (folly::AsyncTimeout*));

  MOCK_METHOD(
      bool, scheduleTimeout, (folly::AsyncTimeout*, std::chrono::milliseconds));

  MOCK_METHOD(void, cancelTimeout, (folly::AsyncTimeout*));

  MOCK_METHOD(void, bumpHandlingTime, ());
  MOCK_METHOD(bool, isInTimeoutManagerThread, ());
};
} // namespace test
} // namespace folly
