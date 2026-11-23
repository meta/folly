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

#include <folly/net/TcpInfoDispatcher.h>
#include <folly/portability/GMock.h>

namespace folly {
namespace test {

class MockTcpInfoDispatcher : public folly::TcpInfoDispatcher {
 public:
  MOCK_METHOD(
      (folly::Expected<TcpInfo, std::errc>),
      initFromFd,
      (const NetworkSocket&,
       const TcpInfo::LookupOptions&,
       folly::netops::Dispatcher&,
       TcpInfo::IoctlDispatcher&));
};

} // namespace test
} // namespace folly
