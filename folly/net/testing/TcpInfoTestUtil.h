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

#include <sys/ioctl.h>
#include <cstring>
#include <folly/net/TcpInfo.h>
#include <folly/net/testing/MockNetOpsDispatcher.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

namespace folly {
namespace test {

class TcpInfoTestUtil {
 public:
  /**
   * Mock to enable testing of socket buffer lookups.
   */
  class MockIoctlDispatcher : public folly::TcpInfo::IoctlDispatcher {
   public:
    MockIoctlDispatcher() = default;
    virtual ~MockIoctlDispatcher() = default;

    /**
     * Configures mocked methods to forward calls to default implementation.
     */
    void forwardToDefaultImpl() {
      ON_CALL(*this, ioctl(testing::_, testing::_, testing::_))
          .WillByDefault(
              testing::Invoke([](int fd, unsigned long request, void* argp) {
                return ::ioctl(fd, request, argp);
              }));
    }

    MOCK_METHOD(int, ioctl, (int fd, unsigned long request, void* argp));
  };

  template <typename T1>
  static void setupExpectCallTcpInfo(
      folly::netops::test::MockDispatcher& mockDispatcher,
      const folly::NetworkSocket& s,
      const T1& tInfo) {
    EXPECT_CALL(
        mockDispatcher,
        getsockopt(s, IPPROTO_TCP, TCP_INFO, testing::_, testing::_))
        .WillOnce(
            testing::WithArgs<3, 4>(
                testing::Invoke([tInfo](void* optval, socklen_t* optlen) {
                  auto copied = std::min((unsigned int)sizeof tInfo, *optlen);
                  std::memcpy(optval, (void*)&tInfo, copied);
                  *optlen = copied;
                  return 0;
                })));
  }

  static void setupExpectCallCcName(
      folly::netops::test::MockDispatcher& mockDispatcher,
      const folly::NetworkSocket& s,
      const std::string& ccName) {
    EXPECT_CALL(
        mockDispatcher,
        getsockopt(
            s,
            IPPROTO_TCP,
            TCP_CONGESTION,
            testing::NotNull(),
            testing::Pointee(testing::Eq(folly::TcpInfo::kLinuxTcpCaNameMax))))
        .WillOnce(
            testing::WithArgs<3, 4>(
                testing::Invoke([ccName](void* optval, socklen_t* optlen) {
                  EXPECT_THAT(
                      optlen, testing::Pointee(testing::Ge(ccName.size())));
                  std::copy(
                      ccName.begin(),
                      ccName.end(),
                      ((std::array<
                           char,
                           (unsigned int)folly::TcpInfo::kLinuxTcpCaNameMax>*)
                           optval)
                          ->data());
                  *optlen = std::min<socklen_t>(ccName.size(), *optlen);
                  return 0;
                })));
  }

  static void setupExpectCallCcInfo(
      folly::netops::test::MockDispatcher& mockDispatcher,
      const NetworkSocket& s,
      const folly::TcpInfo::tcp_cc_info& ccInfo) {
    EXPECT_CALL(
        mockDispatcher,
        getsockopt(
            s,
            IPPROTO_TCP,
            TCP_CC_INFO,
            testing::NotNull(),
            testing::Pointee(testing::Eq(sizeof(folly::TcpInfo::tcp_cc_info)))))
        .WillOnce(
            testing::WithArgs<3, 4>(
                testing::Invoke([ccInfo](void* optval, socklen_t* optlen) {
                  auto copied = std::min((unsigned int)sizeof ccInfo, *optlen);
                  std::memcpy(optval, (void*)&(ccInfo), copied);
                  *optlen = copied;
                  return 0;
                })));
  }
};

} // namespace test
} // namespace folly
