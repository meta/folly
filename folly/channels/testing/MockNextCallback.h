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

#include <stdexcept>
#include <string>

#include <glog/logging.h>

#include <folly/OperationCancelled.h>
#include <folly/Try.h>
#include <folly/portability/GMock.h>

namespace folly {
namespace channels {

/// GMock implementation of the per-item ("next") callback consumed by
/// consumeChannelWithCallback().
///
/// operator() fans each Try<TValue> out to the mock method matching the
/// outcome, so tests can EXPECT_CALL on values, clean closes,
/// cancellations, and runtime errors independently. Any other exception
/// is fatal: it signals a test bug rather than an expected outcome.
template <typename TValue>
class MockNextCallback {
 public:
  void operator()(Try<TValue> result) {
    if (result.hasValue()) {
      onValue(result.value());
    } else if (result.template hasException<folly::OperationCancelled>()) {
      onCancelled();
    } else if (result.template hasException<std::runtime_error>()) {
      onRuntimeError(result.exception().what().toStdString());
    } else if (result.hasException()) {
      LOG(FATAL) << "Unexpected exception: " << result.exception().what();
    } else {
      onClosed();
    }
  }

  MOCK_METHOD(void, onValue, (TValue));
  MOCK_METHOD(void, onClosed, ());
  MOCK_METHOD(void, onCancelled, ());
  MOCK_METHOD(void, onRuntimeError, (std::string));
};

} // namespace channels
} // namespace folly
