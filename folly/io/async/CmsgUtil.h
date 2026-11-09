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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <folly/io/async/Liburing.h>

#if FOLLY_HAS_LIBURING
#include <sys/socket.h>
#endif

namespace folly {

class IOBuf;

namespace detail {

struct IoUringCmsg {
  int level{-1};
  int type{-1};
  std::vector<uint8_t> payload;
};

struct IoUringRecvmsgControl {
  std::vector<IoUringCmsg> cmsgs;
  int msgFlags{0};
};

#if FOLLY_HAS_LIBURING
bool parseCmsgs(
    const cmsghdr* firstCmsg,
    size_t controlLength,
    int msgFlags,
    IoUringRecvmsgControl& control);

bool serializeCmsgs(
    const IoUringRecvmsgControl& control,
    void* buffer,
    size_t bufferLength,
    msghdr& msg);

bool parseIoUringRecvmsg(
    std::unique_ptr<IOBuf>& data,
    int recvmsgLength,
    size_t nameLength,
    size_t controlLength,
    IoUringRecvmsgControl& control);
#endif

} // namespace detail
} // namespace folly
