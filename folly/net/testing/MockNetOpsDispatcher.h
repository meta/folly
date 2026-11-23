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

#include <folly/net/NetOpsDispatcher.h>
#include <folly/portability/GMock.h>

namespace folly {
namespace netops {
namespace test {

class MockDispatcher : public Dispatcher {
 public:
  MockDispatcher() = default;
  virtual ~MockDispatcher() = default;

  /**
   * Configures mocked methods to forward calls to default implementation.
   */
  void forwardToDefaultImpl() {
    ON_CALL(*this, socket(testing::_, testing::_, testing::_))
        .WillByDefault([this](int af, int type, int protocol) {
          return Dispatcher::socket(af, type, protocol);
        });

    ON_CALL(*this, bind(testing::_, testing::_, testing::_))
        .WillByDefault(
            [this](NetworkSocket s, const sockaddr* name, socklen_t namelen) {
              return Dispatcher::bind(s, name, namelen);
            });

    ON_CALL(*this, close(testing::_)).WillByDefault([this](NetworkSocket s) {
      return Dispatcher::close(s);
    });

    ON_CALL(*this, connect(testing::_, testing::_, testing::_))
        .WillByDefault(
            [this](NetworkSocket s, const sockaddr* name, socklen_t namelen) {
              return Dispatcher::connect(s, name, namelen);
            });

    ON_CALL(*this, set_socket_close_on_exec(testing::_))
        .WillByDefault([this](NetworkSocket s) {
          return Dispatcher::set_socket_close_on_exec(s);
        });

    ON_CALL(
        *this,
        getsockopt(testing::_, testing::_, testing::_, testing::_, testing::_))
        .WillByDefault(
            [this](
                NetworkSocket s,
                int level,
                int optname,
                void* optval,
                socklen_t* optlen) {
              return Dispatcher::getsockopt(s, level, optname, optval, optlen);
            });

    ON_CALL(*this, sendmsg(testing::_, testing::_, testing::_))
        .WillByDefault(
            [this](NetworkSocket s, const msghdr* message, int flags) {
              return Dispatcher::sendmsg(s, message, flags);
            });

    ON_CALL(*this, recvmsg(testing::_, testing::_, testing::_))
        .WillByDefault([this](NetworkSocket s, msghdr* message, int flags) {
          return Dispatcher::recvmsg(s, message, flags);
        });

    ON_CALL(
        *this,
        setsockopt(testing::_, testing::_, testing::_, testing::_, testing::_))
        .WillByDefault(
            [this](
                NetworkSocket s,
                int level,
                int optname,
                const void* optval,
                socklen_t optlen) {
              return Dispatcher::setsockopt(s, level, optname, optval, optlen);
            });
  }

  MOCK_METHOD(NetworkSocket, socket, (int af, int type, int protocol));

  MOCK_METHOD(
      int, bind, (NetworkSocket s, const sockaddr* name, socklen_t namelen));

  MOCK_METHOD(int, close, (NetworkSocket s));

  MOCK_METHOD(
      int, connect, (NetworkSocket s, const sockaddr* name, socklen_t namelen));

  MOCK_METHOD(int, set_socket_close_on_exec, (NetworkSocket s));

  MOCK_METHOD(
      int,
      getsockopt,
      (NetworkSocket s,
       int level,
       int optname,
       void* optval,
       socklen_t* optlen));

  MOCK_METHOD(
      ssize_t, sendmsg, (NetworkSocket s, const msghdr* message, int flags));

  MOCK_METHOD(
      int,
      sendmmsg,
      (NetworkSocket s, mmsghdr* msgvec, unsigned int vlen, int flags));

  MOCK_METHOD(ssize_t, recvmsg, (NetworkSocket s, msghdr* message, int flags));

  MOCK_METHOD(
      int,
      setsockopt,
      (NetworkSocket s,
       int level,
       int optname,
       const void* optval,
       socklen_t optlen));
};

} // namespace test
} // namespace netops
} // namespace folly
